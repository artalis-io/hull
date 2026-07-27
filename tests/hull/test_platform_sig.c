/*
 * test_platform_sig.c — Unit tests for the platform manifest builder,
 * signer, verifier, and per-arch extractor.
 *
 * Pure-data exercises with synthetic hashes — no `.a` files needed,
 * no I/O. Covers:
 *   - hl_platform_sig_build_manifest: canonical sort, format
 *     ("<hex>  <arch>\n"), overflow rejection, duplicate-arch
 *     rejection, validation of hash hex + arch chars.
 *   - hl_platform_sig_sign + hl_platform_sig_verify roundtrip,
 *     tamper detection, wrong-key rejection.
 *   - hl_platform_sig_extract_for_arch: hit, miss,
 *     prefix-collision guard.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/platform_sig.h"
#include "hull/cap/crypto.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Synthetic per-arch hashes (deterministic across runs) ──────── */

static const char *HASH_LX86 =
    "0000000000000000000000000000000000000000000000000000000000000001";
static const char *HASH_LARM =
    "0000000000000000000000000000000000000000000000000000000000000002";
static const char *HASH_DARM =
    "0000000000000000000000000000000000000000000000000000000000000003";
static const char *HASH_CX86 =
    "0000000000000000000000000000000000000000000000000000000000000004";
static const char *HASH_CARM =
    "0000000000000000000000000000000000000000000000000000000000000005";

/* ── build_manifest ─────────────────────────────────────────────── */

UTEST(platform_sig, build_one_entry) {
    HlPlatformArchHash e[] = {{ "darwin-arm64", HASH_DARM }};
    char buf[256];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 1, buf, sizeof(buf), &n), 0);
    /* "<64 hex>  darwin-arm64\n" = 64 + 2 + 12 + 1 = 79 */
    ASSERT_EQ(n, 79u);
    char expect[128];
    snprintf(expect, sizeof(expect), "%s  darwin-arm64\n", HASH_DARM);
    ASSERT_EQ(memcmp(buf, expect, n), 0);
}

UTEST(platform_sig, build_sorts_canonically) {
    /* Provided in a non-sorted order — the helper sorts by arch name. */
    HlPlatformArchHash e[] = {
        { "linux-x86_64",  HASH_LX86 },
        { "darwin-arm64",  HASH_DARM },
        { "cosmo-x86_64",  HASH_CX86 },
        { "linux-aarch64", HASH_LARM },
        { "cosmo-aarch64", HASH_CARM },
    };
    char buf[1024];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 5, buf, sizeof(buf), &n), 0);

    /* LC_ALL=C sort order: cosmo-aarch64, cosmo-x86_64, darwin-arm64,
     * linux-aarch64, linux-x86_64. */
    char expect[1024];
    int len = snprintf(expect, sizeof(expect),
        "%s  cosmo-aarch64\n"
        "%s  cosmo-x86_64\n"
        "%s  darwin-arm64\n"
        "%s  linux-aarch64\n"
        "%s  linux-x86_64\n",
        HASH_CARM, HASH_CX86, HASH_DARM, HASH_LARM, HASH_LX86);
    ASSERT_EQ((int)n, len);
    ASSERT_EQ(memcmp(buf, expect, n), 0);
}

UTEST(platform_sig, build_format_matches_sha256sum) {
    /* The manifest must be byte-compatible with sha256sum output and
     * with hull.sha256: 64 hex + two spaces + name + newline. Verify
     * the exact byte sequence at the spaces. */
    HlPlatformArchHash e[] = {{ "darwin-arm64", HASH_DARM }};
    char buf[256];
    size_t n = 0;
    hl_platform_sig_build_manifest(e, 1, buf, sizeof(buf), &n);
    ASSERT_EQ(buf[64], ' ');
    ASSERT_EQ(buf[65], ' ');
    ASSERT_EQ(buf[n - 1], '\n');
}

UTEST(platform_sig, build_rejects_overflow) {
    HlPlatformArchHash e[] = {{ "darwin-arm64", HASH_DARM }};
    char tiny[16];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 1, tiny, sizeof(tiny), &n), -1);
}

UTEST(platform_sig, build_rejects_duplicate_arch) {
    HlPlatformArchHash e[] = {
        { "darwin-arm64", HASH_DARM },
        { "darwin-arm64", HASH_LARM },  /* same arch, different hash */
    };
    char buf[256];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 2, buf, sizeof(buf), &n), -1);
}

UTEST(platform_sig, build_rejects_short_hash) {
    HlPlatformArchHash e[] = {{ "darwin-arm64", "deadbeef" }};
    char buf[256];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 1, buf, sizeof(buf), &n), -1);
}

UTEST(platform_sig, build_rejects_uppercase_hash) {
    /* Per spec: lowercase hex only. Mixed-case would break the
     * byte-reproducible-manifest property. */
    HlPlatformArchHash e[] = {{
        "darwin-arm64",
        "0000000000000000000000000000000000000000000000000000000000000ABC",
    }};
    char buf[256];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 1, buf, sizeof(buf), &n), -1);
}

UTEST(platform_sig, build_rejects_arch_with_slash) {
    /* Arch names go into URL paths + filesystem paths downstream;
     * `/` in an arch name would be a traversal vector. */
    HlPlatformArchHash e[] = {{ "../etc", HASH_DARM }};
    char buf[256];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 1, buf, sizeof(buf), &n), -1);
}

UTEST(platform_sig, build_rejects_empty_arch) {
    HlPlatformArchHash e[] = {{ "", HASH_DARM }};
    char buf[256];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 1, buf, sizeof(buf), &n), -1);
}

UTEST(platform_sig, build_null_args_safe) {
    HlPlatformArchHash e[] = {{ "darwin-arm64", HASH_DARM }};
    char buf[256];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(NULL, 1, buf, sizeof(buf), &n), -1);
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 1, NULL, sizeof(buf), &n), -1);
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 1, buf, sizeof(buf), NULL), -1);
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 0, buf, sizeof(buf), &n), -1);
}

/* ── sign + verify roundtrip ────────────────────────────────────── */

UTEST(platform_sig, sign_verify_roundtrip) {
    /* Build a real manifest. */
    HlPlatformArchHash e[] = {
        { "darwin-arm64",  HASH_DARM },
        { "linux-x86_64",  HASH_LX86 },
    };
    char manifest[1024];
    size_t mlen = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 2, manifest, sizeof(manifest), &mlen), 0);

    /* Generate a fresh keypair (no dependency on
     * HL_PLATFORM_PUBKEY_HEX — that's covered by a separate test). */
    uint8_t pk[32], sk[64];
    ASSERT_EQ(hl_cap_crypto_ed25519_keypair(pk, sk), 0);

    /* Sign. */
    char sig_hex[129];
    ASSERT_EQ(hl_platform_sig_sign(manifest, mlen, sk, sig_hex, sizeof(sig_hex)), 0);
    ASSERT_EQ((int)strlen(sig_hex), 128);

    /* Verify with the matching key. */
    ASSERT_EQ(hl_platform_sig_verify(manifest, mlen, sig_hex, 128, pk), 0);
}

UTEST(platform_sig, verify_rejects_tampered_manifest) {
    HlPlatformArchHash e[] = {{ "darwin-arm64", HASH_DARM }};
    char manifest[256];
    size_t mlen = 0;
    hl_platform_sig_build_manifest(e, 1, manifest, sizeof(manifest), &mlen);

    uint8_t pk[32], sk[64];
    hl_cap_crypto_ed25519_keypair(pk, sk);
    char sig_hex[129];
    hl_platform_sig_sign(manifest, mlen, sk, sig_hex, sizeof(sig_hex));

    /* Flip a single byte in the hash. */
    manifest[63] = (manifest[63] == '0') ? '1' : '0';
    ASSERT_EQ(hl_platform_sig_verify(manifest, mlen, sig_hex, 128, pk), -1);
}

UTEST(platform_sig, verify_rejects_tampered_signature) {
    HlPlatformArchHash e[] = {{ "darwin-arm64", HASH_DARM }};
    char manifest[256];
    size_t mlen = 0;
    hl_platform_sig_build_manifest(e, 1, manifest, sizeof(manifest), &mlen);

    uint8_t pk[32], sk[64];
    hl_cap_crypto_ed25519_keypair(pk, sk);
    char sig_hex[129];
    hl_platform_sig_sign(manifest, mlen, sk, sig_hex, sizeof(sig_hex));

    /* Flip one hex nibble in the signature. */
    sig_hex[0] = (sig_hex[0] == '0') ? '1' : '0';
    ASSERT_EQ(hl_platform_sig_verify(manifest, mlen, sig_hex, 128, pk), -1);
}

UTEST(platform_sig, verify_rejects_wrong_key) {
    HlPlatformArchHash e[] = {{ "darwin-arm64", HASH_DARM }};
    char manifest[256];
    size_t mlen = 0;
    hl_platform_sig_build_manifest(e, 1, manifest, sizeof(manifest), &mlen);

    uint8_t pk[32], sk[64];
    hl_cap_crypto_ed25519_keypair(pk, sk);
    char sig_hex[129];
    hl_platform_sig_sign(manifest, mlen, sk, sig_hex, sizeof(sig_hex));

    /* Generate a SECOND keypair, verify with the wrong pubkey. */
    uint8_t other_pk[32], other_sk[64];
    hl_cap_crypto_ed25519_keypair(other_pk, other_sk);
    ASSERT_EQ(hl_platform_sig_verify(manifest, mlen, sig_hex, 128, other_pk), -1);
}

UTEST(platform_sig, verify_null_inputs_rejected) {
    uint8_t pk[32];
    memset(pk, 0, sizeof(pk));
    char dummy_sig[129] = "deadbeef";
    ASSERT_EQ(hl_platform_sig_verify(NULL, 0, dummy_sig, 128, pk), -1);
    ASSERT_EQ(hl_platform_sig_verify("x", 1, NULL, 128, pk), -1);
}

/* ── extract_for_arch ─────────────────────────────────────────── */

UTEST(extract_for_arch, finds_each_entry) {
    HlPlatformArchHash e[] = {
        { "darwin-arm64",  HASH_DARM },
        { "linux-aarch64", HASH_LARM },
        { "linux-x86_64",  HASH_LX86 },
    };
    char manifest[1024];
    size_t mlen = 0;
    hl_platform_sig_build_manifest(e, 3, manifest, sizeof(manifest), &mlen);

    char hex[65];
    ASSERT_EQ(hl_platform_sig_extract_for_arch(manifest, mlen, "darwin-arm64", hex), 0);
    ASSERT_STREQ(hex, HASH_DARM);
    ASSERT_EQ(hl_platform_sig_extract_for_arch(manifest, mlen, "linux-aarch64", hex), 0);
    ASSERT_STREQ(hex, HASH_LARM);
    ASSERT_EQ(hl_platform_sig_extract_for_arch(manifest, mlen, "linux-x86_64", hex), 0);
    ASSERT_STREQ(hex, HASH_LX86);
}

UTEST(extract_for_arch, misses_unknown) {
    HlPlatformArchHash e[] = {{ "darwin-arm64", HASH_DARM }};
    char manifest[256];
    size_t mlen = 0;
    hl_platform_sig_build_manifest(e, 1, manifest, sizeof(manifest), &mlen);

    char hex[65];
    ASSERT_EQ(hl_platform_sig_extract_for_arch(manifest, mlen, "freebsd-riscv", hex), -1);
}

UTEST(extract_for_arch, no_prefix_collision) {
    /* "linux-x86" must not match the "linux-x86_64" line. */
    HlPlatformArchHash e[] = {{ "linux-x86_64", HASH_LX86 }};
    char manifest[256];
    size_t mlen = 0;
    hl_platform_sig_build_manifest(e, 1, manifest, sizeof(manifest), &mlen);

    char hex[65];
    ASSERT_EQ(hl_platform_sig_extract_for_arch(manifest, mlen, "linux-x86", hex), -1);
}

/* ── multi-asset manifest (issue #114 composed-feature signing) ─────── */

/* The name column now also carries asset names (with a `.<arch>.a` suffix, i.e.
 * dots), not just bare arches. Build a manifest mixing both, sign, verify, and
 * extract every entry by its exact name. */
UTEST(platform_sig, multi_asset_roundtrip) {
    HlPlatformArchHash e[] = {
        { "linux-x86_64",                          HASH_LX86 },  /* platform lib, bare arch */
        { "libhull_feature-lua.linux-x86_64.a",    HASH_LARM },  /* embedded runtime archive */
        { "libhull_feature-http.linux-x86_64.a",   HASH_DARM },
        { "libhull_feature-http-lua.linux-x86_64.a", HASH_CX86 },
        { "libhull_feature-tui-lua.linux-x86_64.a",  HASH_CARM },
    };
    char manifest[1024];
    size_t mlen = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(e, 5, manifest, sizeof(manifest), &mlen), 0);

    uint8_t pk[32], sk[64];
    ASSERT_EQ(hl_cap_crypto_ed25519_keypair(pk, sk), 0);
    char sig_hex[129];
    ASSERT_EQ(hl_platform_sig_sign(manifest, mlen, sk, sig_hex, sizeof(sig_hex)), 0);
    ASSERT_EQ(hl_platform_sig_verify(manifest, mlen, sig_hex, 128, pk), 0);

    /* Every entry - bare arch and dotted asset alike - is retrievable by name. */
    char hex[65];
    ASSERT_EQ(hl_platform_sig_extract_for_arch(manifest, mlen, "linux-x86_64", hex), 0);
    ASSERT_STREQ(hex, HASH_LX86);
    ASSERT_EQ(hl_platform_sig_extract_for_arch(manifest, mlen,
              "libhull_feature-http-lua.linux-x86_64.a", hex), 0);
    ASSERT_STREQ(hex, HASH_CX86);
    /* A partial name must not collide with the full asset entry. */
    ASSERT_EQ(hl_platform_sig_extract_for_arch(manifest, mlen,
              "libhull_feature-http", hex), -1);
}

/* A dotted asset name is accepted by build_manifest (regression for the
 * name-validation relaxation); a name with whitespace or a slash is not. */
UTEST(platform_sig, accepts_dotted_asset_name) {
    HlPlatformArchHash ok[] = {{ "libhull_feature-tui-js.darwin-arm64.a", HASH_DARM }};
    char buf[256];
    size_t n = 0;
    ASSERT_EQ(hl_platform_sig_build_manifest(ok, 1, buf, sizeof(buf), &n), 0);

    HlPlatformArchHash bad_space[] = {{ "libhull feature.a", HASH_DARM }};
    ASSERT_EQ(hl_platform_sig_build_manifest(bad_space, 1, buf, sizeof(buf), &n), -1);
    HlPlatformArchHash bad_slash[] = {{ "lib/hull.a", HASH_DARM }};
    ASSERT_EQ(hl_platform_sig_build_manifest(bad_slash, 1, buf, sizeof(buf), &n), -1);
}

UTEST_MAIN()
