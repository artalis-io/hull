/*
 * test_hull_cap_crypto.c - Tests for shared crypto capability
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

/* ── SHA-1 (legacy interop; HIBP only) ─────────────────────────────── */

static void hex20(const uint8_t in[20], char out[41])
{
    for (int i = 0; i < 20; i++) snprintf(out + i*2, 3, "%02x", in[i]);
    out[40] = '\0';
}

UTEST(hl_cap_crypto, sha1_empty)
{
    uint8_t hash[20];
    ASSERT_EQ(hl_cap_crypto_sha1("", 0, hash), 0);
    char hex[41]; hex20(hash, hex);
    /* RFC 3174 test vector: SHA-1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709 */
    ASSERT_STREQ(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

UTEST(hl_cap_crypto, sha1_abc)
{
    uint8_t hash[20];
    ASSERT_EQ(hl_cap_crypto_sha1("abc", 3, hash), 0);
    char hex[41]; hex20(hash, hex);
    /* RFC 3174 test vector: SHA-1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d */
    ASSERT_STREQ(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");
}

UTEST(hl_cap_crypto, sha1_password_for_hibp)
{
    /* HIBP test vector: SHA-1("password") = 5BAA61E4C9B93F3F0682250B6CF8331B7EE68FD8.
     * This drives the realistic pwned-passwords flow that pwned.lua/.js
     * now route through the cap layer instead of pure-script SHA-1. */
    const char *pw = "password";
    uint8_t hash[20];
    ASSERT_EQ(hl_cap_crypto_sha1(pw, strlen(pw), hash), 0);
    char hex[41]; hex20(hash, hex);
    ASSERT_STREQ(hex, "5baa61e4c9b93f3f0682250b6cf8331b7ee68fd8");
}

UTEST(hl_cap_crypto, sha1_null)
{
    uint8_t hash[20];
    /* (NULL, 0) hashes the empty input (matches SHA-256 cap semantics). */
    ASSERT_EQ(hl_cap_crypto_sha1(NULL, 0, hash), 0);
    char hex[41]; hex20(hash, hex);
    ASSERT_STREQ(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    /* (NULL, >0) and (any, NULL out) are hard errors. */
    ASSERT_EQ(hl_cap_crypto_sha1(NULL, 1, hash), -1);
    ASSERT_EQ(hl_cap_crypto_sha1("abc", 3, NULL), -1);
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

UTEST(hl_cap_crypto, sha256_multi_block)
{
    /* Exercises the hardware-acceleration paths (ARMv8 SHA2 + x86
     * SHA-NI) across block boundaries. 1 MiB of zeros = 16384
     * blocks; any bug in state-carryover between blocks would
     * produce the wrong digest. */
    size_t n = 1024 * 1024;
    uint8_t *buf = calloc(1, n);
    ASSERT_TRUE(buf != NULL);
    uint8_t hash[32];
    int rc = hl_cap_crypto_sha256(buf, n, hash);
    free(buf);
    ASSERT_EQ(rc, 0);

    char hex[65];
    hex_encode(hash, 32, hex);
    /* SHA-256 of 1 MiB of zero bytes - well-known constant. */
    ASSERT_STREQ(hex,
        "30e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58");
}

UTEST(hl_cap_crypto, sha256_null)
{
    /* (NULL, 0) is the well-defined "hash the empty input" case -
     * SHA-256("") = e3b0c4...b855. (NULL, len>0) and (any, NULL out)
     * are still hard errors. */
    uint8_t hash[32];
    int rc = hl_cap_crypto_sha256(NULL, 0, hash);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(hash[0], 0xe3);
    ASSERT_EQ(hash[1], 0xb0);
    ASSERT_EQ(hash[2], 0xc4);

    rc = hl_cap_crypto_sha256(NULL, 1, hash);
    ASSERT_EQ(rc, -1);

    rc = hl_cap_crypto_sha256("abc", 3, NULL);
    ASSERT_EQ(rc, -1);
}

/* ── Incremental SHA-256 ───────────────────────────────────────────── */

/* Helper: format a 32-byte digest as 64-char lowercase hex.
 * snprintf to match the convention used by the runtime bindings. */
static void hex32(const uint8_t in[32], char out[65])
{
    for (int i = 0; i < 32; i++) snprintf(out + i*2, 3, "%02x", in[i]);
    out[64] = '\0';
}

UTEST(hl_cap_crypto, sha256_inc_matches_oneshot_empty)
{
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);
    uint8_t out[32];
    ASSERT_EQ(hl_cap_crypto_sha256_final(&ctx, out), 0);
    char hex[65]; hex32(out, hex);
    ASSERT_STREQ(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

UTEST(hl_cap_crypto, sha256_inc_matches_oneshot_short)
{
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);
    ASSERT_EQ(hl_cap_crypto_sha256_update(&ctx, "abc", 3), 0);
    uint8_t out[32];
    ASSERT_EQ(hl_cap_crypto_sha256_final(&ctx, out), 0);
    char hex[65]; hex32(out, hex);
    /* RFC 6234 §8.5 test vector. */
    ASSERT_STREQ(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

UTEST(hl_cap_crypto, sha256_inc_chunked_equals_oneshot)
{
    /* Hash the same 1 KiB of pseudo-random data three ways and check
     * the digests all match: one-shot, byte-at-a-time, and in
     * boundary-straddling 17-byte chunks. */
    uint8_t data[1024];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 31u + 7u);

    uint8_t want[32];
    ASSERT_EQ(hl_cap_crypto_sha256(data, sizeof(data), want), 0);

    /* Byte-at-a-time */
    {
        HlSha256Ctx ctx;
        hl_cap_crypto_sha256_init(&ctx);
        for (size_t i = 0; i < sizeof(data); i++)
            ASSERT_EQ(hl_cap_crypto_sha256_update(&ctx, &data[i], 1), 0);
        uint8_t out[32];
        ASSERT_EQ(hl_cap_crypto_sha256_final(&ctx, out), 0);
        ASSERT_EQ(memcmp(out, want, 32), 0);
    }

    /* 17-byte chunks - straddles 64-byte block boundaries. */
    {
        HlSha256Ctx ctx;
        hl_cap_crypto_sha256_init(&ctx);
        size_t off = 0;
        while (off < sizeof(data)) {
            size_t step = (off + 17 <= sizeof(data)) ? 17 : (sizeof(data) - off);
            ASSERT_EQ(hl_cap_crypto_sha256_update(&ctx, data + off, step), 0);
            off += step;
        }
        uint8_t out[32];
        ASSERT_EQ(hl_cap_crypto_sha256_final(&ctx, out), 0);
        ASSERT_EQ(memcmp(out, want, 32), 0);
    }
}

UTEST(hl_cap_crypto, sha256_inc_zero_update_is_noop)
{
    /* Calling update with len=0 must not change the digest. */
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);
    ASSERT_EQ(hl_cap_crypto_sha256_update(&ctx, NULL, 0), 0);  /* NULL OK at len=0 */
    ASSERT_EQ(hl_cap_crypto_sha256_update(&ctx, "abc", 3), 0);
    ASSERT_EQ(hl_cap_crypto_sha256_update(&ctx, "", 0), 0);
    uint8_t out[32];
    ASSERT_EQ(hl_cap_crypto_sha256_final(&ctx, out), 0);
    char hex[65]; hex32(out, hex);
    ASSERT_STREQ(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

UTEST(hl_cap_crypto, sha256_inc_null_args)
{
    uint8_t out[32];
    /* NULL ctx is always -1 */
    ASSERT_EQ(hl_cap_crypto_sha256_update(NULL, "abc", 3), -1);
    ASSERT_EQ(hl_cap_crypto_sha256_final(NULL, out), -1);

    /* Update with len>0 and NULL data is -1 */
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);
    ASSERT_EQ(hl_cap_crypto_sha256_update(&ctx, NULL, 5), -1);

    /* Final with NULL out is -1 */
    ASSERT_EQ(hl_cap_crypto_sha256_final(&ctx, NULL), -1);
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

/* ── HMAC-SHA256 vtable migration check ─────────────────────────────
 *
 * Direct cap-level coverage that the new vtable-dispatched
 * hl_cap_crypto_hmac_sha256 still produces RFC 4231 § Test Case 1
 * output byte-for-byte. The runtime / stdlib tests cover it
 * transitively (auth-flows token sign/verify, oauth state cookie,
 * PBKDF2 deterministic check below), but a focused vector test
 * makes a backend swap regression unambiguous. */
UTEST(hl_cap_crypto, hmac_sha256_rfc4231_vector1)
{
    uint8_t key[20];
    for (int i = 0; i < 20; i++) key[i] = 0x0b;
    const char *msg = "Hi There";
    uint8_t out[32];
    ASSERT_EQ(0, hl_cap_crypto_hmac_sha256(key, sizeof(key),
                                            (const uint8_t *)msg, 8,
                                            out));
    /* RFC 4231 expected MAC. */
    const uint8_t expected[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };
    ASSERT_EQ(0, memcmp(out, expected, 32));
}

UTEST(hl_cap_crypto, hmac_sha256_long_key_is_prehashed)
{
    /* Per RFC 2104 § 2 the HMAC construction prehashes any key
     * longer than the block size (64 bytes for SHA-256). Sanity:
     * a 65-byte key matches HMAC(SHA256(key), msg). */
    uint8_t key[65];
    for (int i = 0; i < 65; i++) key[i] = (uint8_t)i;
    const char *msg = "vtable check";

    uint8_t direct[32];
    ASSERT_EQ(0, hl_cap_crypto_hmac_sha256(key, sizeof(key),
                                            (const uint8_t *)msg, 12,
                                            direct));

    uint8_t hashed_key[32];
    ASSERT_EQ(0, hl_cap_crypto_sha256(key, sizeof(key), hashed_key));
    uint8_t indirect[32];
    ASSERT_EQ(0, hl_cap_crypto_hmac_sha256(hashed_key, sizeof(hashed_key),
                                            (const uint8_t *)msg, 12,
                                            indirect));
    ASSERT_EQ(0, memcmp(direct, indirect, 32));
}

UTEST(hl_cap_crypto, hmac_sha256_null_args)
{
    uint8_t out[32];
    uint8_t key[16] = { 0 };
    const char *msg = "x";
    ASSERT_EQ(-1, hl_cap_crypto_hmac_sha256(NULL, 0, (const uint8_t *)msg, 1, out));
    ASSERT_EQ(-1, hl_cap_crypto_hmac_sha256(key, sizeof(key), NULL, 0, out));
    ASSERT_EQ(-1, hl_cap_crypto_hmac_sha256(key, sizeof(key), (const uint8_t *)msg, 1, NULL));
}

/* ── Portable (mbedtls-free) HMAC backend ───────────────────────────
 *
 * hl_crypto_hmac_backend_portable is the active HMAC backend in the
 * pure-compute flavor (mbedTLS dropped). It is always compiled, so these
 * tests cover it in every build - independently of which backend the cap
 * entry points dispatch through here. RFC 4231 / RFC 2202 vectors prove
 * the hand-rolled construction is correct; the cross-check proves it is
 * byte-identical to whatever backend hl_cap_crypto_hmac_* uses. */
UTEST(hl_cap_crypto, hmac_portable_sha256_rfc4231_vector1)
{
    const HlCryptoHmacBackend *b = &hl_crypto_hmac_backend_portable;
    uint8_t key[20];
    for (int i = 0; i < 20; i++) key[i] = 0x0b;
    const char *msg = "Hi There";
    uint8_t out[32];
    ASSERT_EQ(1, b->supports(HL_CRYPTO_HMAC_SHA256));
    ASSERT_EQ(0, b->compute(b, HL_CRYPTO_HMAC_SHA256, key, sizeof(key),
                            (const uint8_t *)msg, 8, out, 32));
    const uint8_t expected[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };
    ASSERT_EQ(0, memcmp(out, expected, 32));
}

UTEST(hl_cap_crypto, hmac_portable_sha1_rfc2202_vector1)
{
    const HlCryptoHmacBackend *b = &hl_crypto_hmac_backend_portable;
    uint8_t key[20];
    for (int i = 0; i < 20; i++) key[i] = 0x0b;
    const char *msg = "Hi There";
    uint8_t out[20];
    ASSERT_EQ(1, b->supports(HL_CRYPTO_HMAC_SHA1));
    ASSERT_EQ(0, b->compute(b, HL_CRYPTO_HMAC_SHA1, key, sizeof(key),
                            (const uint8_t *)msg, 8, out, 20));
    /* RFC 2202 HMAC-SHA1 Test Case 1. */
    const uint8_t expected[20] = {
        0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64,
        0xe2, 0x8b, 0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e,
        0xf1, 0x46, 0xbe, 0x00,
    };
    ASSERT_EQ(0, memcmp(out, expected, 20));
}

UTEST(hl_cap_crypto, hmac_portable_long_key_prehashed)
{
    /* RFC 2104 §2: keys longer than the 64-byte block are prehashed.
     * Exercises the >block-size K' = H(K) branch for both algs. */
    const HlCryptoHmacBackend *b = &hl_crypto_hmac_backend_portable;
    uint8_t key[131];
    for (int i = 0; i < 131; i++) key[i] = 0xaa;
    const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    size_t mlen = strlen(msg);

    uint8_t mac256[32];
    ASSERT_EQ(0, b->compute(b, HL_CRYPTO_HMAC_SHA256, key, sizeof(key),
                            (const uint8_t *)msg, mlen, mac256, 32));
    /* RFC 4231 Test Case 6 expected MAC. */
    const uint8_t exp256[32] = {
        0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f,
        0x0d, 0x8a, 0x26, 0xaa, 0xcb, 0xf5, 0xb7, 0x7f,
        0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28, 0xc5, 0x14,
        0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3, 0x7f, 0x54,
    };
    ASSERT_EQ(0, memcmp(mac256, exp256, 32));
}

UTEST(hl_cap_crypto, hmac_portable_matches_cap_and_rejects_sha512)
{
    /* Whatever backend the cap entry points use here, the portable one
     * must produce identical output (no silent divergence between flavors). */
    const HlCryptoHmacBackend *b = &hl_crypto_hmac_backend_portable;
    const uint8_t key[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const char *msg = "cross-check the backends agree";
    size_t mlen = strlen(msg);

    uint8_t via_cap[32], via_portable[32];
    ASSERT_EQ(0, hl_cap_crypto_hmac_sha256(key, sizeof(key),
                                           (const uint8_t *)msg, mlen, via_cap));
    ASSERT_EQ(0, b->compute(b, HL_CRYPTO_HMAC_SHA256, key, sizeof(key),
                            (const uint8_t *)msg, mlen, via_portable, 32));
    ASSERT_EQ(0, memcmp(via_cap, via_portable, 32));

    uint8_t cap1[20], port1[20];
    ASSERT_EQ(0, hl_cap_crypto_hmac_sha1(key, sizeof(key),
                                         (const uint8_t *)msg, mlen, cap1));
    ASSERT_EQ(0, b->compute(b, HL_CRYPTO_HMAC_SHA1, key, sizeof(key),
                            (const uint8_t *)msg, mlen, port1, 20));
    ASSERT_EQ(0, memcmp(cap1, port1, 20));

    /* SHA-512 HMAC is intentionally unsupported by the portable backend. */
    uint8_t out512[64];
    ASSERT_EQ(0, b->supports(HL_CRYPTO_HMAC_SHA512));
    ASSERT_EQ(-1, b->compute(b, HL_CRYPTO_HMAC_SHA512, key, sizeof(key),
                             (const uint8_t *)msg, mlen, out512, 64));
}

/* ── PBKDF2 tests ───────────────────────────────────────────────────── */

UTEST(hl_cap_crypto, pbkdf2_basic)
{
    const char *pw = "password";
    uint8_t salt[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    uint8_t out[32];

    int rc = hl_cap_crypto_pbkdf2(pw, strlen(pw),
                                    salt, sizeof(salt),
                                    100000, out, sizeof(out));
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
                           100000, out1, sizeof(out1));
    hl_cap_crypto_pbkdf2(pw, strlen(pw), salt, sizeof(salt),
                           100000, out2, sizeof(out2));

    ASSERT_EQ(memcmp(out1, out2, 32), 0);
}

UTEST(hl_cap_crypto, pbkdf2_different_passwords)
{
    uint8_t salt[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    uint8_t out1[32], out2[32];

    hl_cap_crypto_pbkdf2("password1", 9, salt, sizeof(salt),
                           100000, out1, sizeof(out1));
    hl_cap_crypto_pbkdf2("password2", 9, salt, sizeof(salt),
                           100000, out2, sizeof(out2));

    ASSERT_NE(memcmp(out1, out2, 32), 0);
}

UTEST(hl_cap_crypto, pbkdf2_null_args)
{
    uint8_t salt[4] = { 0 };
    uint8_t out[32];

    int rc = hl_cap_crypto_pbkdf2(NULL, 0, salt, sizeof(salt),
                                    100000, out, sizeof(out));
    ASSERT_EQ(rc, -1);

    rc = hl_cap_crypto_pbkdf2("pw", 2, NULL, 0,
                                100000, out, sizeof(out));
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

/* ── Hex encode / decode ─────────────────────────────────────────── */

UTEST(hl_cap_crypto, hex_encode_known_vector)
{
    const uint8_t in[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xAB };
    char out[12];
    int n = hl_cap_crypto_hex_encode(in, sizeof(in), out, sizeof(out));
    ASSERT_EQ(12, n);
    ASSERT_EQ(0, memcmp(out, "00017f80ffab", 12));
}

UTEST(hl_cap_crypto, hex_encode_empty_is_ok)
{
    char out[1];
    int n = hl_cap_crypto_hex_encode(NULL, 0, out, sizeof(out));
    ASSERT_EQ(0, n);
}

UTEST(hl_cap_crypto, hex_encode_short_output_rejected)
{
    const uint8_t in[] = { 0xAB, 0xCD };
    char out[3];                          /* need 4 */
    ASSERT_EQ(-1, hl_cap_crypto_hex_encode(in, sizeof(in), out, sizeof(out)));
}

UTEST(hl_cap_crypto, hex_decode_round_trip_all_bytes)
{
    uint8_t in[256];
    for (int i = 0; i < 256; i++) in[i] = (uint8_t)i;
    char hex[512];
    ASSERT_EQ(512, hl_cap_crypto_hex_encode(in, 256, hex, sizeof(hex)));
    uint8_t back[256];
    ASSERT_EQ(256, hl_cap_crypto_hex_decode(hex, 512, back, sizeof(back)));
    ASSERT_EQ(0, memcmp(in, back, 256));
}

UTEST(hl_cap_crypto, hex_decode_uppercase_ok)
{
    const char *hex = "DEADBEEF";
    uint8_t out[4];
    ASSERT_EQ(4, hl_cap_crypto_hex_decode(hex, 8, out, sizeof(out)));
    ASSERT_EQ(0xDE, out[0]); ASSERT_EQ(0xAD, out[1]);
    ASSERT_EQ(0xBE, out[2]); ASSERT_EQ(0xEF, out[3]);
}

UTEST(hl_cap_crypto, hex_decode_mixed_case_ok)
{
    const char *hex = "dEaDbEeF";
    uint8_t out[4];
    ASSERT_EQ(4, hl_cap_crypto_hex_decode(hex, 8, out, sizeof(out)));
    ASSERT_EQ(0xDE, out[0]);
}

UTEST(hl_cap_crypto, hex_decode_odd_length_rejected)
{
    uint8_t out[2];
    ASSERT_EQ(-1, hl_cap_crypto_hex_decode("abc", 3, out, sizeof(out)));
}

UTEST(hl_cap_crypto, hex_decode_non_hex_char_rejected)
{
    uint8_t out[2];
    ASSERT_EQ(-1, hl_cap_crypto_hex_decode("zzzz", 4, out, sizeof(out)));
    ASSERT_EQ(-1, hl_cap_crypto_hex_decode("ab/d", 4, out, sizeof(out)));
}

UTEST(hl_cap_crypto, hex_decode_short_output_rejected)
{
    uint8_t out[1];
    ASSERT_EQ(-1, hl_cap_crypto_hex_decode("abcd", 4, out, sizeof(out)));
}

UTEST(hl_cap_crypto, hex_decode_empty_is_ok)
{
    uint8_t out[1];
    ASSERT_EQ(0, hl_cap_crypto_hex_decode(NULL, 0, out, sizeof(out)));
}

UTEST_MAIN();
