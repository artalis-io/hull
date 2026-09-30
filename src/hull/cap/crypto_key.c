/*
 * cap/crypto_key.c - a 32-byte secret key held in C. See hull/cap/crypto_key.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/crypto_key.h"
#include "hull/cap/crypto.h"
#include "../utils/hex.h"
#include "../utils/base64.h"

#include <stdlib.h>
#include <string.h>

struct HlCryptoKey {
    uint8_t k[HL_CRYPTO_KEY_LEN];
};

/* Zeroing a compiler cannot remove as a dead store. */
static void key_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Decode into `k` (HL_CRYPTO_KEY_LEN bytes). 0 on a 32-byte key, else -1. */
static int decode_key(const char *t, size_t n, uint8_t *k)
{
    if (n == 2 * HL_CRYPTO_KEY_LEN)
        return hl_hex_decode(t, n, k, HL_CRYPTO_KEY_LEN) == HL_CRYPTO_KEY_LEN ? 0 : -1;
    /* base64 of 32 bytes is 43 characters, or 44 with one '='. Anything
     * longer cannot be a 32-byte key, so it is refused before decoding into
     * a buffer sized for one. */
    if (n != 43 && n != 44) return -1;
    static const unsigned flags[] = { 0, HL_BASE64_URL };
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        uint8_t tmp[HL_CRYPTO_KEY_LEN + 2];
        size_t got = 0;
        int rc = hl_base64_decode(t, n, tmp, sizeof tmp, &got, flags[i]);
        if (rc == 0 && got == HL_CRYPTO_KEY_LEN) {
            memcpy(k, tmp, HL_CRYPTO_KEY_LEN);
            key_zero(tmp, sizeof tmp);
            return 0;
        }
        key_zero(tmp, sizeof tmp);
    }
    return -1;
}

int hl_cap_crypto_key_from_text(const char *text, size_t len, HlCryptoKey **out)
{
    if (!out) return -1;
    *out = NULL;
    if (!text) return -1;
    while (len && is_space(text[0])) { text++; len--; }
    while (len && is_space(text[len - 1])) len--;

    HlCryptoKey *key = malloc(sizeof *key);
    if (!key) return -1;
    if (decode_key(text, len, key->k) != 0) {
        key_zero(key, sizeof *key);
        free(key);
        return -1;
    }
    *out = key;
    return 0;
}

int hl_cap_crypto_key_secretbox(const HlCryptoKey *k, uint8_t *out,
                                const void *msg, size_t msg_len,
                                const uint8_t nonce[24])
{
    if (!k || !out || (!msg && msg_len) || !nonce) return -1;
    return hl_cap_crypto_secretbox(out, msg, msg_len, nonce, k->k);
}

int hl_cap_crypto_key_secretbox_open(const HlCryptoKey *k, uint8_t *out,
                                     const void *ct, size_t ct_len,
                                     const uint8_t nonce[24])
{
    if (!k || !out || !ct || !nonce || ct_len < 16) return -1;
    int rc = hl_cap_crypto_secretbox_open(out, ct, ct_len, nonce, k->k);
    if (rc != 0) key_zero(out, ct_len - 16);
    return rc;
}

void hl_cap_crypto_key_free(HlCryptoKey *k)
{
    if (!k) return;
    key_zero(k, sizeof *k);
    free(k);
}
