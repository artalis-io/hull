/*
 * test_hull_cap_crypto.c — Tests for shared crypto capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap.h"
#include <string.h>
#include <stdio.h>

/* ── SHA-256 tests ──────────────────────────────────────────────────── */

static void hex_encode(const uint8_t *data, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02x", data[i]);
}

UTEST(hl_cap_crypto, sha256_empty)
{
    uint8_t hash[32];
    int rc = hl_cap_crypto_sha256("", 0, hash);
    ASSERT_EQ(rc, 0);

    char hex[65];
    hex_encode(hash, 32, hex);
    /* SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    ASSERT_STREQ(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

UTEST(hl_cap_crypto, sha256_abc)
{
    uint8_t hash[32];
    int rc = hl_cap_crypto_sha256("abc", 3, hash);
    ASSERT_EQ(rc, 0);

    char hex[65];
    hex_encode(hash, 32, hex);
    /* SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
    ASSERT_STREQ(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

UTEST(hl_cap_crypto, sha256_longer)
{
    const char *msg = "The quick brown fox jumps over the lazy dog";
    uint8_t hash[32];
    int rc = hl_cap_crypto_sha256(msg, strlen(msg), hash);
    ASSERT_EQ(rc, 0);

    char hex[65];
    hex_encode(hash, 32, hex);
    /* Known hash */
    ASSERT_STREQ(hex,
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

UTEST(hl_cap_crypto, sha256_null)
{
    uint8_t hash[32];
    int rc = hl_cap_crypto_sha256(NULL, 0, hash);
    ASSERT_EQ(rc, -1);

    rc = hl_cap_crypto_sha256("abc", 3, NULL);
    ASSERT_EQ(rc, -1);
}

/* ── Random bytes tests ─────────────────────────────────────────────── */

UTEST(hl_cap_crypto, random_nonzero)
{
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));

    int rc = hl_cap_crypto_random(buf, sizeof(buf));
    ASSERT_EQ(rc, 0);

    /* Extremely unlikely that 32 random bytes are all zero */
    int all_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (buf[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    ASSERT_EQ(all_zero, 0);
}

UTEST(hl_cap_crypto, random_different)
{
    uint8_t buf1[16], buf2[16];

    hl_cap_crypto_random(buf1, sizeof(buf1));
    hl_cap_crypto_random(buf2, sizeof(buf2));

    /* Extremely unlikely that two 16-byte random buffers are identical */
    ASSERT_NE(memcmp(buf1, buf2, 16), 0);
}

UTEST(hl_cap_crypto, random_null)
{
    int rc = hl_cap_crypto_random(NULL, 16);
    ASSERT_EQ(rc, -1);
}

/* ── PBKDF2 tests ───────────────────────────────────────────────────── */

UTEST(hl_cap_crypto, pbkdf2_basic)
{
    const char *pw = "password";
    uint8_t salt[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    uint8_t out[32];

    int rc = hl_cap_crypto_pbkdf2(pw, strlen(pw),
                                    salt, sizeof(salt),
                                    1, out, sizeof(out));
    ASSERT_EQ(rc, 0);

    /* Verify output is non-zero */
    int all_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (out[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    ASSERT_EQ(all_zero, 0);
}

UTEST(hl_cap_crypto, pbkdf2_deterministic)
{
    const char *pw = "test";
    uint8_t salt[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    uint8_t out1[32], out2[32];

    hl_cap_crypto_pbkdf2(pw, strlen(pw), salt, sizeof(salt),
                           10, out1, sizeof(out1));
    hl_cap_crypto_pbkdf2(pw, strlen(pw), salt, sizeof(salt),
                           10, out2, sizeof(out2));

    ASSERT_EQ(memcmp(out1, out2, 32), 0);
}

UTEST(hl_cap_crypto, pbkdf2_different_passwords)
{
    uint8_t salt[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    uint8_t out1[32], out2[32];

    hl_cap_crypto_pbkdf2("password1", 9, salt, sizeof(salt),
                           10, out1, sizeof(out1));
    hl_cap_crypto_pbkdf2("password2", 9, salt, sizeof(salt),
                           10, out2, sizeof(out2));

    ASSERT_NE(memcmp(out1, out2, 32), 0);
}

UTEST(hl_cap_crypto, pbkdf2_null_args)
{
    uint8_t salt[4] = { 0 };
    uint8_t out[32];

    int rc = hl_cap_crypto_pbkdf2(NULL, 0, salt, sizeof(salt),
                                    10, out, sizeof(out));
    ASSERT_EQ(rc, -1);

    rc = hl_cap_crypto_pbkdf2("pw", 2, NULL, 0,
                                10, out, sizeof(out));
    ASSERT_EQ(rc, -1);
}

UTEST_MAIN();
