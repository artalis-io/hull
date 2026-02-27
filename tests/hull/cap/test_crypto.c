/*
 * test_hull_cap_crypto.c — Tests for shared crypto capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/crypto.h"
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

/* ── Ed25519 tests ─────────────────────────────────────────────────── */

UTEST(hl_cap_crypto, ed25519_keypair)
{
    uint8_t pk[32], sk[64];
    int rc = hl_cap_crypto_ed25519_keypair(pk, sk);
    ASSERT_EQ(rc, 0);

    /* Public key should be non-zero */
    int all_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (pk[i] != 0) { all_zero = 0; break; }
    }
    ASSERT_EQ(all_zero, 0);

    /* Last 32 bytes of sk should equal pk (TweetNaCl format) */
    ASSERT_EQ(memcmp(sk + 32, pk, 32), 0);
}

UTEST(hl_cap_crypto, ed25519_sign_verify)
{
    uint8_t pk[32], sk[64];
    hl_cap_crypto_ed25519_keypair(pk, sk);

    const char *msg = "hello world";
    uint8_t sig[64];
    int rc = hl_cap_crypto_ed25519_sign((const uint8_t *)msg, strlen(msg),
                                          sk, sig);
    ASSERT_EQ(rc, 0);

    /* Verify with correct key */
    rc = hl_cap_crypto_ed25519_verify((const uint8_t *)msg, strlen(msg),
                                        sig, pk);
    ASSERT_EQ(rc, 0);
}

UTEST(hl_cap_crypto, ed25519_verify_wrong_key)
{
    uint8_t pk1[32], sk1[64];
    uint8_t pk2[32], sk2[64];
    hl_cap_crypto_ed25519_keypair(pk1, sk1);
    hl_cap_crypto_ed25519_keypair(pk2, sk2);

    const char *msg = "test message";
    uint8_t sig[64];
    hl_cap_crypto_ed25519_sign((const uint8_t *)msg, strlen(msg), sk1, sig);

    /* Verify with wrong key should fail */
    int rc = hl_cap_crypto_ed25519_verify((const uint8_t *)msg, strlen(msg),
                                            sig, pk2);
    ASSERT_EQ(rc, -1);
}

UTEST(hl_cap_crypto, ed25519_verify_tampered)
{
    uint8_t pk[32], sk[64];
    hl_cap_crypto_ed25519_keypair(pk, sk);

    const char *msg = "original message";
    uint8_t sig[64];
    hl_cap_crypto_ed25519_sign((const uint8_t *)msg, strlen(msg), sk, sig);

    /* Tamper with message */
    const char *tampered = "tampered message";
    int rc = hl_cap_crypto_ed25519_verify((const uint8_t *)tampered,
                                            strlen(tampered), sig, pk);
    ASSERT_EQ(rc, -1);
}

UTEST(hl_cap_crypto, ed25519_null_args)
{
    uint8_t pk[32], sk[64], sig[64];
    uint8_t msg[] = "test";

    ASSERT_EQ(hl_cap_crypto_ed25519_keypair(NULL, sk), -1);
    ASSERT_EQ(hl_cap_crypto_ed25519_keypair(pk, NULL), -1);
    ASSERT_EQ(hl_cap_crypto_ed25519_sign(NULL, 4, sk, sig), -1);
    ASSERT_EQ(hl_cap_crypto_ed25519_sign(msg, 4, NULL, sig), -1);
    ASSERT_EQ(hl_cap_crypto_ed25519_sign(msg, 4, sk, NULL), -1);
    ASSERT_EQ(hl_cap_crypto_ed25519_verify(NULL, 4, sig, pk), -1);
    ASSERT_EQ(hl_cap_crypto_ed25519_verify(msg, 4, NULL, pk), -1);
    ASSERT_EQ(hl_cap_crypto_ed25519_verify(msg, 4, sig, NULL), -1);
}

/* ── SHA-512 tests ──────────────────────────────────────────────────── */

UTEST(hl_cap_crypto, sha512_empty)
{
    uint8_t hash[64];
    int rc = hl_cap_crypto_sha512("", 0, hash);
    ASSERT_EQ(rc, 0);

    char hex[129];
    hex_encode(hash, 64, hex);
    ASSERT_STREQ(hex,
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}

UTEST(hl_cap_crypto, sha512_abc)
{
    uint8_t hash[64];
    int rc = hl_cap_crypto_sha512("abc", 3, hash);
    ASSERT_EQ(rc, 0);

    char hex[129];
    hex_encode(hash, 64, hex);
    ASSERT_STREQ(hex,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

UTEST(hl_cap_crypto, sha512_null)
{
    uint8_t hash[64];
    ASSERT_EQ(hl_cap_crypto_sha512(NULL, 0, hash), -1);
    ASSERT_EQ(hl_cap_crypto_sha512("x", 1, NULL), -1);
}

/* ── HMAC-SHA512/256 auth tests ────────────────────────────────────── */

UTEST(hl_cap_crypto, auth_roundtrip)
{
    uint8_t key[32];
    memset(key, 0x42, 32);

    const char *msg = "authenticate me";
    uint8_t tag[32];

    ASSERT_EQ(0, hl_cap_crypto_auth(msg, strlen(msg), key, tag));
    ASSERT_EQ(0, hl_cap_crypto_auth_verify(tag, msg, strlen(msg), key));
}

UTEST(hl_cap_crypto, auth_wrong_key)
{
    uint8_t key[32], wrong_key[32];
    memset(key, 0x42, 32);
    memset(wrong_key, 0x43, 32);

    const char *msg = "authenticate me";
    uint8_t tag[32];

    ASSERT_EQ(0, hl_cap_crypto_auth(msg, strlen(msg), key, tag));
    ASSERT_NE(0, hl_cap_crypto_auth_verify(tag, msg, strlen(msg), wrong_key));
}

UTEST(hl_cap_crypto, auth_tampered)
{
    uint8_t key[32];
    memset(key, 0x42, 32);

    const char *msg = "authenticate me";
    uint8_t tag[32];

    ASSERT_EQ(0, hl_cap_crypto_auth(msg, strlen(msg), key, tag));
    tag[0] ^= 0x01;
    ASSERT_NE(0, hl_cap_crypto_auth_verify(tag, msg, strlen(msg), key));
}

UTEST(hl_cap_crypto, auth_null_guard)
{
    uint8_t key[32], tag[32];
    memset(key, 0, 32);
    ASSERT_EQ(-1, hl_cap_crypto_auth(NULL, 0, key, tag));
    ASSERT_EQ(-1, hl_cap_crypto_auth("x", 1, NULL, tag));
    ASSERT_EQ(-1, hl_cap_crypto_auth("x", 1, key, NULL));
    ASSERT_EQ(-1, hl_cap_crypto_auth_verify(NULL, "x", 1, key));
    ASSERT_EQ(-1, hl_cap_crypto_auth_verify(tag, NULL, 1, key));
    ASSERT_EQ(-1, hl_cap_crypto_auth_verify(tag, "x", 1, NULL));
}

/* ── Secretbox tests ────────────────────────────────────────────────── */

UTEST(hl_cap_crypto, secretbox_roundtrip)
{
    uint8_t key[32], nonce[24];
    memset(key, 0xAA, 32);
    memset(nonce, 0xBB, 24);

    const char *msg = "secret message";
    size_t msg_len = strlen(msg);
    size_t ct_len = msg_len + HL_SECRETBOX_MACBYTES;

    uint8_t ct[128], pt[128];

    ASSERT_EQ(0, hl_cap_crypto_secretbox(ct, msg, msg_len, nonce, key));
    ASSERT_EQ(0, hl_cap_crypto_secretbox_open(pt, ct, ct_len, nonce, key));
    ASSERT_EQ(0, memcmp(pt, msg, msg_len));
}

UTEST(hl_cap_crypto, secretbox_wrong_key)
{
    uint8_t key[32], wrong_key[32], nonce[24];
    memset(key, 0xAA, 32);
    memset(wrong_key, 0xCC, 32);
    memset(nonce, 0xBB, 24);

    const char *msg = "secret message";
    size_t msg_len = strlen(msg);
    size_t ct_len = msg_len + HL_SECRETBOX_MACBYTES;

    uint8_t ct[128], pt[128];

    ASSERT_EQ(0, hl_cap_crypto_secretbox(ct, msg, msg_len, nonce, key));
    ASSERT_NE(0, hl_cap_crypto_secretbox_open(pt, ct, ct_len, nonce, wrong_key));
}

UTEST(hl_cap_crypto, secretbox_tampered)
{
    uint8_t key[32], nonce[24];
    memset(key, 0xAA, 32);
    memset(nonce, 0xBB, 24);

    const char *msg = "secret message";
    size_t msg_len = strlen(msg);
    size_t ct_len = msg_len + HL_SECRETBOX_MACBYTES;

    uint8_t ct[128], pt[128];

    ASSERT_EQ(0, hl_cap_crypto_secretbox(ct, msg, msg_len, nonce, key));
    ct[0] ^= 0x01;
    ASSERT_NE(0, hl_cap_crypto_secretbox_open(pt, ct, ct_len, nonce, key));
}

UTEST(hl_cap_crypto, secretbox_null_guard)
{
    uint8_t key[32], nonce[24], ct[32], pt[32];
    memset(key, 0, 32);
    memset(nonce, 0, 24);
    ASSERT_EQ(-1, hl_cap_crypto_secretbox(NULL, "x", 1, nonce, key));
    ASSERT_EQ(-1, hl_cap_crypto_secretbox_open(NULL, ct, 17, nonce, key));
    ASSERT_EQ(-1, hl_cap_crypto_secretbox_open(pt, ct, 15, nonce, key));
}

/* ── Box (public-key encryption) tests ───────────────────────────────── */

UTEST(hl_cap_crypto, box_roundtrip)
{
    uint8_t alice_pk[32], alice_sk[32];
    uint8_t bob_pk[32], bob_sk[32];

    ASSERT_EQ(0, hl_cap_crypto_box_keypair(alice_pk, alice_sk));
    ASSERT_EQ(0, hl_cap_crypto_box_keypair(bob_pk, bob_sk));

    uint8_t nonce[24];
    hl_cap_crypto_random(nonce, 24);

    const char *msg = "hello bob from alice";
    size_t msg_len = strlen(msg);
    size_t ct_len = msg_len + HL_BOX_MACBYTES;

    uint8_t ct[128], pt[128];

    ASSERT_EQ(0, hl_cap_crypto_box(ct, msg, msg_len, nonce, bob_pk, alice_sk));
    ASSERT_EQ(0, hl_cap_crypto_box_open(pt, ct, ct_len, nonce, alice_pk, bob_sk));
    ASSERT_EQ(0, memcmp(pt, msg, msg_len));
}

UTEST(hl_cap_crypto, box_wrong_key)
{
    uint8_t alice_pk[32], alice_sk[32];
    uint8_t bob_pk[32], bob_sk[32];
    uint8_t eve_pk[32], eve_sk[32];

    ASSERT_EQ(0, hl_cap_crypto_box_keypair(alice_pk, alice_sk));
    ASSERT_EQ(0, hl_cap_crypto_box_keypair(bob_pk, bob_sk));
    ASSERT_EQ(0, hl_cap_crypto_box_keypair(eve_pk, eve_sk));

    uint8_t nonce[24];
    hl_cap_crypto_random(nonce, 24);

    const char *msg = "hello bob";
    size_t msg_len = strlen(msg);
    size_t ct_len = msg_len + HL_BOX_MACBYTES;

    uint8_t ct[128], pt[128];

    ASSERT_EQ(0, hl_cap_crypto_box(ct, msg, msg_len, nonce, bob_pk, alice_sk));
    ASSERT_NE(0, hl_cap_crypto_box_open(pt, ct, ct_len, nonce, alice_pk, eve_sk));
}

UTEST(hl_cap_crypto, box_null_guard)
{
    uint8_t pk[32], sk[32], nonce[24], ct[32], pt[32];
    memset(pk, 0, 32);
    memset(sk, 0, 32);
    memset(nonce, 0, 24);
    ASSERT_EQ(-1, hl_cap_crypto_box(NULL, "x", 1, nonce, pk, sk));
    ASSERT_EQ(-1, hl_cap_crypto_box_open(NULL, ct, 17, nonce, pk, sk));
    ASSERT_EQ(-1, hl_cap_crypto_box_open(pt, ct, 15, nonce, pk, sk));
}

UTEST(hl_cap_crypto, box_keypair_unique)
{
    uint8_t pk1[32], sk1[32], pk2[32], sk2[32];
    ASSERT_EQ(0, hl_cap_crypto_box_keypair(pk1, sk1));
    ASSERT_EQ(0, hl_cap_crypto_box_keypair(pk2, sk2));
    ASSERT_NE(0, memcmp(pk1, pk2, 32));
    ASSERT_NE(0, memcmp(sk1, sk2, 32));
}

UTEST_MAIN();
