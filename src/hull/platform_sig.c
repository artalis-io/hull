/*
 * platform_sig.c — Platform manifest builder + signer + verifier.
 *
 * Pure data — no I/O, no global state. See platform_sig.h for the
 * design rationale and v0.1.3 wire-up plan.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/platform_sig.h"
#include "hull/release.h"
#include "hull/release_io.h"
#include "hull/signature.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile-time cap on manifest entries. The name column is either a bare arch
 * (the platform lib: 5 today) OR an asset name (issue #114: the embedded feature
 * archives - runtime/http/web/tui bridges - one per {archive, arch}). Budget for
 * ~8 archives x 5 arches plus headroom; 64 keeps the qsort + duplicate-check
 * O(n²) without ever mattering. */
#define HL_PLATFORM_SIG_MAX_ARCHES 64

/* Name column: a bare arch ("cosmo-aarch64", 13 chars) OR an asset name
 * ("libhull_feature-http-lua.linux-x86_64.a", ~40). Cap at 64 for headroom. */
#define HL_PLATFORM_SIG_MAX_ARCH_NAME 64

/* ── Validation helpers ──────────────────────────────────────────── */

/* True iff @s is exactly 64 lowercase hex characters. */
static int is_valid_hash_hex(const char *s)
{
    if (!s) return 0;
    for (size_t i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return s[64] == '\0';
}

/* True iff @s is a plausible arch identifier: non-empty,
 * `[A-Za-z0-9_-]+`, length-bounded. Same character class as
 * tool names and asset names — keeps the manifest format
 * round-trippable without quoting. */
/* True iff @s is a valid name-column value: a bare arch name OR an asset name
 * (issue #114). Allows [A-Za-z0-9._-] so asset names with a `.<arch>.a` suffix
 * (e.g. "libhull_feature-lua.linux-x86_64.a") pass, while still excluding
 * whitespace (which would break the two-space-delimited manifest line) and
 * path/control chars. Bounded by HL_PLATFORM_SIG_MAX_ARCH_NAME. */
static int is_valid_name(const char *s)
{
    if (!s || !*s) return 0;
    size_t n = 0;
    for (const char *p = s; *p; p++, n++) {
        if (n >= HL_PLATFORM_SIG_MAX_ARCH_NAME) return 0;
        char c = *p;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

/* qsort comparator: arch name ascending. */
static int arch_entry_cmp(const void *a, const void *b)
{
    const HlPlatformArchHash *ea = a;
    const HlPlatformArchHash *eb = b;
    return strcmp(ea->arch, eb->arch);
}

/* ── Manifest builder ────────────────────────────────────────────── */

int hl_platform_sig_build_manifest(const HlPlatformArchHash *entries, size_t n,
                                   char *out, size_t out_sz,
                                   size_t *out_len)
{
    if (!entries || !out || !out_len || n == 0) return -1;
    if (n > HL_PLATFORM_SIG_MAX_ARCHES) return -1;

    /* Validate every entry before doing any work. */
    for (size_t i = 0; i < n; i++) {
        if (!is_valid_name(entries[i].arch))     return -1;
        if (!is_valid_hash_hex(entries[i].hash_hex)) return -1;
    }

    /* Copy to a local buffer so we can sort without mutating the
     * caller's array. */
    HlPlatformArchHash sorted[HL_PLATFORM_SIG_MAX_ARCHES];
    for (size_t i = 0; i < n; i++) sorted[i] = entries[i];
    qsort(sorted, n, sizeof(sorted[0]), arch_entry_cmp);

    /* Reject duplicate arch names — sorted, so dups are adjacent. */
    for (size_t i = 1; i < n; i++) {
        if (strcmp(sorted[i - 1].arch, sorted[i].arch) == 0) return -1;
    }

    /* Emit. */
    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
        size_t arch_len = strlen(sorted[i].arch);
        size_t line_len = 64 + 2 + arch_len + 1;  /* hash + "  " + arch + \n */
        if (written + line_len > out_sz) return -1;

        memcpy(out + written, sorted[i].hash_hex, 64);
        written += 64;
        out[written++] = ' ';
        out[written++] = ' ';
        memcpy(out + written, sorted[i].arch, arch_len);
        written += arch_len;
        out[written++] = '\n';
    }

    *out_len = written;
    return 0;
}

/* ── Sign / verify ───────────────────────────────────────────────── */

int hl_platform_sig_sign(const void *manifest, size_t manifest_len,
                         const uint8_t secret_key[64],
                         char *out_sig_hex, size_t out_sig_hex_size)
{
    /* Pure-Ed25519 signing; release.h's signer is key-agnostic. */
    return hl_release_sign_manifest(manifest, manifest_len, secret_key,
                                    out_sig_hex, out_sig_hex_size);
}

/* Decode HL_PLATFORM_PUBKEY_HEX into 32 bytes. Returns 0 on success,
 * -1 if the macro isn't a valid 64-char hex string (should be
 * unreachable for a correctly-encoded build). */
static int decode_embedded_platform_pubkey(uint8_t out_pk[32])
{
    const char *hex = HL_PLATFORM_PUBKEY_HEX;
    if (strlen(hex) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        int hi_v = (hi >= '0' && hi <= '9') ? hi - '0' :
                   (hi >= 'a' && hi <= 'f') ? 10 + hi - 'a' :
                   (hi >= 'A' && hi <= 'F') ? 10 + hi - 'A' : -1;
        int lo_v = (lo >= '0' && lo <= '9') ? lo - '0' :
                   (lo >= 'a' && lo <= 'f') ? 10 + lo - 'a' :
                   (lo >= 'A' && lo <= 'F') ? 10 + lo - 'A' : -1;
        if (hi_v < 0 || lo_v < 0) return -1;
        out_pk[i] = (uint8_t)((hi_v << 4) | lo_v);
    }
    return 0;
}

int hl_platform_pubkey_is_placeholder(void)
{
    uint8_t pk[32];
    if (decode_embedded_platform_pubkey(pk) != 0) return 0;
    for (int i = 0; i < 32; i++) {
        if (pk[i] != 0) return 0;
    }
    return 1;
}

int hl_platform_sig_verify(const void *manifest, size_t manifest_len,
                           const char *sig_hex, size_t sig_hex_len,
                           const uint8_t pubkey[32])
{
    if (!manifest || !sig_hex) return -1;

    /* When the caller doesn't supply a key, decode the embedded
     * HL_PLATFORM_PUBKEY_HEX and use it. release.h's verifier
     * defaults to HL_RELEASE_PUBKEY_HEX in the same NULL case — we
     * substitute the platform key explicitly so the caller's intent
     * (platform vs release) is unambiguous. */
    uint8_t embedded_pk[32];
    const uint8_t *use_pk;
    if (pubkey) {
        use_pk = pubkey;
    } else {
        if (decode_embedded_platform_pubkey(embedded_pk) != 0) return -1;
        use_pk = embedded_pk;
    }

    return hl_release_verify_manifest_sig(manifest, manifest_len,
                                          sig_hex, sig_hex_len, use_pk);
}

/* ── Per-arch lookup ─────────────────────────────────────────────── */

int hl_platform_sig_extract_for_arch(const char *manifest, size_t manifest_len,
                                     const char *arch, char hex_out[65])
{
    /* The manifest format is byte-compatible with release_io's
     * checksum-manifest format (sha256sum-style: "<hex>  <name>\n").
     * Reuse the existing parser. */
    return hl_release_io_find_checksum(manifest, manifest_len, arch, hex_out);
}

/* ── Composed-attestation verify (issue #114) ────────────────────── */

int hl_platform_sig_verify_composed(const char *manifest, size_t manifest_len,
                                    const char *sig_hex, size_t sig_hex_len,
                                    const uint8_t pubkey[32],
                                    const HlPlatformArchHash *assets,
                                    size_t n_assets)
{
    if (!manifest || !sig_hex) return -1;
    if (n_assets > 0 && !assets) return -1;

    /* 1. The manifest must be authentically signed for this domain. Do this
     *    first: an unsigned/forged manifest makes every hash lookup worthless. */
    if (hl_platform_sig_verify(manifest, manifest_len, sig_hex, sig_hex_len,
                               pubkey) != 0)
        return -1;

    /* 2. Every composed archive's recorded hash must appear in the signed
     *    manifest under its name. A missing name or a hash mismatch means the
     *    app was built composing an archive gethull did not sign. */
    for (size_t i = 0; i < n_assets; i++) {
        if (!assets[i].arch || !assets[i].hash_hex) return -1;
        char hex[65];
        if (hl_platform_sig_extract_for_arch(manifest, manifest_len,
                                             assets[i].arch, hex) != 0)
            return -1;
        /* Hashes are public; a plain fixed-length compare is fine (no secret
         * material, so no timing channel worth defending). */
        if (strncmp(hex, assets[i].hash_hex, 64) != 0) return -1;
    }
    return 0;
}
