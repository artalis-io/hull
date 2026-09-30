/*
 * test_crypto_key.c - a 32-byte key held in C (cap/crypto_key.c).
 *
 * The key's text forms, what is refused, and that sealing under a held key is
 * byte-for-byte the plain secretbox under the same bytes - so a value sealed
 * with a key from the environment opens with the same key given as bytes, and
 * the other way round.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/cap/crypto.h"
#include "hull/cap/crypto_key.h"

#include <string.h>

/* 32 bytes 0x00..0x1f in each text form. */
static const char KEY_HEX[] =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static const char KEY_HEX_UPPER[] =
    "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
static const char KEY_B64[]     = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
static const char KEY_B64_NOPAD[] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";

static void raw_key(uint8_t k[32])
{
    for (int i = 0; i < 32; i++) k[i] = (uint8_t)i;
}

/* Seal a fixed message under `key` and compare with the plain secretbox. */
static int seals_like_raw(const HlCryptoKey *key)
{
    uint8_t raw[32], nonce[24], a[64 + 16], b[64 + 16];
    const char msg[] = "a value sealed at rest";
    raw_key(raw);
    memset(nonce, 7, sizeof nonce);
    if (hl_cap_crypto_key_secretbox(key, a, msg, sizeof msg - 1, nonce) != 0) return 0;
    if (hl_cap_crypto_secretbox(b, msg, sizeof msg - 1, nonce, raw) != 0) return 0;
    return memcmp(a, b, sizeof msg - 1 + 16) == 0;
}

UTEST(hl_cap_crypto_key, every_text_form_is_the_same_key)
{
    const char *forms[] = { KEY_HEX, KEY_HEX_UPPER, KEY_B64, KEY_B64_NOPAD };
    for (size_t i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        HlCryptoKey *k = NULL;
        ASSERT_EQ(0, hl_cap_crypto_key_from_text(forms[i], strlen(forms[i]), &k));
        ASSERT_TRUE(k != NULL);
        ASSERT_TRUE(seals_like_raw(k));
        hl_cap_crypto_key_free(k);
    }
}

UTEST(hl_cap_crypto_key, the_url_safe_alphabet_is_accepted)
{
    /* 32 bytes of 0xfb encode with '+' and '/' in the standard alphabet and
     * '-' and '_' in the url-safe one; both must give the same key. */
    const char std[] = "+/v7+/v7+/v7+/v7+/v7+/v7+/v7+/v7+/v7+/v7+/s=";
    const char url[] = "-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_s=";
    HlCryptoKey *a = NULL, *b = NULL;
    ASSERT_EQ(0, hl_cap_crypto_key_from_text(std, strlen(std), &a));
    ASSERT_EQ(0, hl_cap_crypto_key_from_text(url, strlen(url), &b));
    uint8_t nonce[24] = { 0 }, ca[16 + 1], cb[16 + 1];
    ASSERT_EQ(0, hl_cap_crypto_key_secretbox(a, ca, "x", 1, nonce));
    ASSERT_EQ(0, hl_cap_crypto_key_secretbox(b, cb, "x", 1, nonce));
    ASSERT_EQ(0, memcmp(ca, cb, sizeof ca));
    hl_cap_crypto_key_free(a);
    hl_cap_crypto_key_free(b);
}

UTEST(hl_cap_crypto_key, surrounding_whitespace_is_ignored)
{
    /* A key read from a secret file usually ends in a newline. */
    char padded[128];
    snprintf(padded, sizeof padded, "  %s\r\n", KEY_HEX);
    HlCryptoKey *k = NULL;
    ASSERT_EQ(0, hl_cap_crypto_key_from_text(padded, strlen(padded), &k));
    ASSERT_TRUE(seals_like_raw(k));
    hl_cap_crypto_key_free(k);
}

UTEST(hl_cap_crypto_key, anything_but_32_bytes_is_refused)
{
    const char *bad[] = {
        "",
        "000102",                                   /* 3 bytes */
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e",   /* 31 */
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20", /* 33 */
        "zz0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",  /* not hex */
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHg==",  /* base64 of 31 bytes */
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gIQ", /* base64 of 34 bytes */
        "correct horse battery staple, a passphrase",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        HlCryptoKey *k = (HlCryptoKey *)1;
        ASSERT_EQ(-1, hl_cap_crypto_key_from_text(bad[i], strlen(bad[i]), &k));
        ASSERT_TRUE(k == NULL);
    }
    HlCryptoKey *k = NULL;
    ASSERT_EQ(-1, hl_cap_crypto_key_from_text(NULL, 0, &k));
    ASSERT_EQ(-1, hl_cap_crypto_key_from_text(KEY_HEX, strlen(KEY_HEX), NULL));
}

UTEST(hl_cap_crypto_key, opens_what_it_sealed_and_refuses_a_forgery)
{
    HlCryptoKey *k = NULL;
    ASSERT_EQ(0, hl_cap_crypto_key_from_text(KEY_HEX, strlen(KEY_HEX), &k));
    uint8_t nonce[24], ct[16 + 5], pt[5];
    memset(nonce, 3, sizeof nonce);
    ASSERT_EQ(0, hl_cap_crypto_key_secretbox(k, ct, "hello", 5, nonce));
    ASSERT_EQ(0, hl_cap_crypto_key_secretbox_open(k, pt, ct, sizeof ct, nonce));
    ASSERT_EQ(0, memcmp(pt, "hello", 5));

    ct[sizeof ct - 1] ^= 1;
    memset(pt, 0xAA, sizeof pt);
    ASSERT_EQ(-1, hl_cap_crypto_key_secretbox_open(k, pt, ct, sizeof ct, nonce));
    for (size_t i = 0; i < sizeof pt; i++) ASSERT_EQ(0, pt[i]);   /* zeroed */

    ASSERT_EQ(-1, hl_cap_crypto_key_secretbox_open(k, pt, ct, 15, nonce));  /* short */
    hl_cap_crypto_key_free(k);
    hl_cap_crypto_key_free(NULL);
}

UTEST_MAIN();
