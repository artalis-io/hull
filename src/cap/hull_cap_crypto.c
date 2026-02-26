/*
 * hull_cap_crypto.c — Shared crypto capability
 *
 * Wraps TweetNaCl / mbedTLS for hashing, random, key derivation,
 * and signature verification. Both Lua and JS bindings call these.
 *
 * For initial implementation, uses OS-provided crypto primitives.
 * Full TweetNaCl/mbedTLS integration added when those are vendored.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/hull_cap.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ── SHA-256 (minimal implementation) ───────────────────────────────── */

/*
 * Minimal SHA-256 — will be replaced by mbedTLS or TweetNaCl when
 * those are vendored. This is a correct, portable implementation.
 */

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define S0(x)        (ROR32(x, 2) ^ ROR32(x,13) ^ ROR32(x,22))
#define S1(x)        (ROR32(x, 6) ^ ROR32(x,11) ^ ROR32(x,25))
#define s0(x)        (ROR32(x, 7) ^ ROR32(x,18) ^ ((x) >> 3))
#define s1(x)        (ROR32(x,17) ^ ROR32(x,19) ^ ((x) >> 10))

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4+0] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] <<  8) |
               ((uint32_t)block[i*4+3]);
    }
    for (int i = 16; i < 64; i++)
        w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + S1(e) + CH(e,f,g) + sha256_k[i] + w[i];
        uint32_t t2 = S0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

int hl_cap_crypto_sha256(const void *data, size_t len, uint8_t out[32])
{
    if (!data || !out)
        return -1;

    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    const uint8_t *p = (const uint8_t *)data;
    size_t remaining = len;

    /* Process full blocks */
    while (remaining >= 64) {
        sha256_transform(state, p);
        p += 64;
        remaining -= 64;
    }

    /* Pad final block */
    uint8_t block[64];
    memset(block, 0, 64);
    memcpy(block, p, remaining);
    block[remaining] = 0x80;

    if (remaining >= 56) {
        sha256_transform(state, block);
        memset(block, 0, 64);
    }

    /* Append length in bits (big-endian) */
    uint64_t bits = (uint64_t)len * 8;
    block[56] = (uint8_t)(bits >> 56);
    block[57] = (uint8_t)(bits >> 48);
    block[58] = (uint8_t)(bits >> 40);
    block[59] = (uint8_t)(bits >> 32);
    block[60] = (uint8_t)(bits >> 24);
    block[61] = (uint8_t)(bits >> 16);
    block[62] = (uint8_t)(bits >>  8);
    block[63] = (uint8_t)(bits);

    sha256_transform(state, block);

    /* Write output (big-endian) */
    for (int i = 0; i < 8; i++) {
        out[i*4+0] = (uint8_t)(state[i] >> 24);
        out[i*4+1] = (uint8_t)(state[i] >> 16);
        out[i*4+2] = (uint8_t)(state[i] >>  8);
        out[i*4+3] = (uint8_t)(state[i]);
    }

    return 0;
}

/* ── Random bytes ───────────────────────────────────────────────────── */

int hl_cap_crypto_random(void *buf, size_t len)
{
    if (!buf || len == 0)
        return -1;

#ifdef __APPLE__
    /* macOS: arc4random_buf is always available and non-failing */
    arc4random_buf(buf, len);
    return 0;
#else
    /* Linux / POSIX: read from /dev/urandom */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;

    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    close(fd);
    return 0;
#endif
}

/* ── PBKDF2-HMAC-SHA256 ────────────────────────────────────────────── */

static int hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *msg, size_t msg_len,
                       uint8_t out[32])
{
    uint8_t k_ipad[64], k_opad[64];
    uint8_t tk[32];

    /* If key is longer than block size, hash it first */
    if (key_len > 64) {
        hl_cap_crypto_sha256(key, key_len, tk);
        key = tk;
        key_len = 32;
    }

    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5c, 64);
    for (size_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    /* inner hash: SHA256(k_ipad || msg) */
    size_t inner_len = 64 + msg_len;
    uint8_t *inner = malloc(inner_len);
    if (!inner)
        return -1;
    memcpy(inner, k_ipad, 64);
    memcpy(inner + 64, msg, msg_len);
    uint8_t inner_hash[32];
    hl_cap_crypto_sha256(inner, inner_len, inner_hash);
    free(inner);

    /* outer hash: SHA256(k_opad || inner_hash) */
    uint8_t outer[64 + 32];
    memcpy(outer, k_opad, 64);
    memcpy(outer + 64, inner_hash, 32);
    hl_cap_crypto_sha256(outer, 96, out);
    return 0;
}

int hl_cap_crypto_pbkdf2(const char *password, size_t pw_len,
                           const uint8_t *salt, size_t salt_len,
                           int iterations,
                           uint8_t *out, size_t out_len)
{
    if (!password || !salt || !out || iterations < 1 || out_len == 0)
        return -1;

    /* Overflow guard */
    if (salt_len > SIZE_MAX / 2)
        return -1;

    uint8_t *work = malloc(salt_len + 4);
    if (!work)
        return -1;

    memcpy(work, salt, salt_len);

    size_t offset = 0;
    uint32_t block_num = 1;

    while (offset < out_len) {
        /* Append block number (big-endian) */
        work[salt_len + 0] = (uint8_t)(block_num >> 24);
        work[salt_len + 1] = (uint8_t)(block_num >> 16);
        work[salt_len + 2] = (uint8_t)(block_num >>  8);
        work[salt_len + 3] = (uint8_t)(block_num);

        uint8_t u[32], t[32];
        if (hmac_sha256((const uint8_t *)password, pw_len,
                        work, salt_len + 4, u) != 0) {
            free(work);
            return -1;
        }
        memcpy(t, u, 32);

        for (int i = 1; i < iterations; i++) {
            if (hmac_sha256((const uint8_t *)password, pw_len,
                            u, 32, u) != 0) {
                free(work);
                return -1;
            }
            for (int j = 0; j < 32; j++)
                t[j] ^= u[j];
        }

        size_t to_copy = out_len - offset;
        if (to_copy > 32)
            to_copy = 32;
        memcpy(out + offset, t, to_copy);

        offset += to_copy;
        block_num++;
    }

    free(work);
    return 0;
}

/* ── Ed25519 verification ───────────────────────────────────────────── */

int hl_cap_crypto_ed25519_verify(const uint8_t *msg, size_t msg_len,
                                   const uint8_t sig[64],
                                   const uint8_t pubkey[32])
{
    /* TODO: integrate TweetNaCl crypto_sign_ed25519_open() */
    /* For now, return -1 (unimplemented) */
    (void)msg; (void)msg_len; (void)sig; (void)pubkey;
    return -1;
}
