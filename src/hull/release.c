/*
 * release.c — Release artifact signature primitives
 *
 * Sign and verify Ed25519 signatures over the `hull.sha256` release
 * manifest. See `docs/release_signing.md` for the full design and
 * `include/hull/release.h` for the API.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/release.h"
#include "hull/cap/crypto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Local hex helpers ─────────────────────────────────────────────── */

static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, size_t hex_len,
                      uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble((unsigned char)hex[i * 2]);
        int lo = hex_nibble((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void hex_encode(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }
    out[len * 2] = '\0';
}

/* Strip trailing whitespace (\n, \r, space, tab). */
static size_t strip_trailing_ws(const char *buf, size_t len)
{
    while (len > 0) {
        unsigned char c = (unsigned char)buf[len - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            len--;
        else
            break;
    }
    return len;
}

static void secure_zero(volatile void *p, size_t n)
{
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n--) *q++ = 0;
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_release_pubkey_configured(void)
{
    /* Placeholder is all-'0' chars. Any non-'0' hex digit means a real
     * key has been committed. */
    const char *hex = HL_RELEASE_PUBKEY_HEX;
    for (size_t i = 0; i < 64; i++)
        if (hex[i] != '0')
            return 1;
    return 0;
}

int hl_release_pubkey_decode(uint8_t out_pk[32])
{
    if (!out_pk) return -1;
    return hex_decode(HL_RELEASE_PUBKEY_HEX, 64, out_pk, 32);
}

int hl_release_verify_manifest_sig(const void *manifest, size_t manifest_len,
                                   const char *sig_hex, size_t sig_hex_len,
                                   const uint8_t pubkey[32])
{
    if (!manifest || !sig_hex) return -1;
    if (manifest_len == 0) return -1;

    /* Strip trailing whitespace from sig_hex */
    sig_hex_len = strip_trailing_ws(sig_hex, sig_hex_len);

    /* Decode signature */
    uint8_t sig[64];
    if (hex_decode(sig_hex, sig_hex_len, sig, sizeof(sig)) != 0)
        return -1;

    /* Resolve public key */
    uint8_t pk_buf[32];
    const uint8_t *pk = pubkey;
    if (!pk) {
        if (hl_release_pubkey_decode(pk_buf) != 0)
            return -1;
        pk = pk_buf;
    }

    int rc = hl_cap_crypto_ed25519_verify((const uint8_t *)manifest,
                                          manifest_len, sig, pk);
    secure_zero(pk_buf, sizeof(pk_buf));
    return rc == 0 ? 0 : -1;
}

int hl_release_sign_manifest(const void *manifest, size_t manifest_len,
                             const uint8_t secret_key[64],
                             char *out_sig_hex, size_t out_sig_hex_size)
{
    if (!manifest || !secret_key || !out_sig_hex) return -1;
    if (manifest_len == 0) return -1;
    if (out_sig_hex_size < 129) return -1;

    uint8_t sig[64];
    int rc = hl_cap_crypto_ed25519_sign((const uint8_t *)manifest,
                                        manifest_len, secret_key, sig);
    if (rc != 0) {
        secure_zero(sig, sizeof(sig));
        return -1;
    }

    hex_encode(sig, sizeof(sig), out_sig_hex);
    secure_zero(sig, sizeof(sig));
    return 0;
}

int hl_release_load_secret_key(const char *path, uint8_t out_sk[64])
{
    if (!path || !out_sk) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    /* The .key file is 128 hex chars + optional newline = up to 130 bytes.
     * Read up to 256 to allow CRLF or trailing whitespace, but reject
     * anything beyond 128 hex chars after stripping. */
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    int read_err = ferror(f);
    fclose(f);
    if (read_err) {
        secure_zero(buf, sizeof(buf));
        return -1;
    }
    buf[n] = '\0';

    size_t hex_len = strip_trailing_ws(buf, n);
    if (hex_len != 128) {
        secure_zero(buf, sizeof(buf));
        return -1;
    }

    int rc = hex_decode(buf, hex_len, out_sk, 64);
    secure_zero(buf, sizeof(buf));
    return rc;
}
