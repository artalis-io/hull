/*
 * cap/crypto.h — Cryptographic primitives
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_CRYPTO_H
#define HL_CAP_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

int hl_cap_crypto_sha256(const void *data, size_t len, uint8_t out[32]);
int hl_cap_crypto_random(void *buf, size_t len);

int hl_cap_crypto_pbkdf2(const char *password, size_t pw_len,
                           const uint8_t *salt, size_t salt_len,
                           int iterations,
                           uint8_t *out, size_t out_len);

int hl_cap_crypto_ed25519_verify(const uint8_t *msg, size_t msg_len,
                                   const uint8_t sig[64],
                                   const uint8_t pubkey[32]);

int hl_cap_crypto_ed25519_sign(const uint8_t *msg, size_t msg_len,
                                 const uint8_t secret_key[64],
                                 uint8_t out_sig[64]);

int hl_cap_crypto_ed25519_keypair(uint8_t out_pk[32], uint8_t out_sk[64]);

/* ── SHA-512 (via TweetNaCl) ─────────────────────────────────────────── */

int hl_cap_crypto_sha512(const void *data, size_t len, uint8_t out[64]);

/* ── HMAC-SHA512/256 authentication (via TweetNaCl crypto_auth) ──────── */

int hl_cap_crypto_auth(const void *msg, size_t msg_len,
                       const uint8_t key[32], uint8_t out[32]);
int hl_cap_crypto_auth_verify(const uint8_t tag[32],
                              const void *msg, size_t msg_len,
                              const uint8_t key[32]);

/* ── Secret-key authenticated encryption (XSalsa20+Poly1305) ─────────── */

#define HL_SECRETBOX_KEYBYTES   32
#define HL_SECRETBOX_NONCEBYTES 24
#define HL_SECRETBOX_MACBYTES   16

int hl_cap_crypto_secretbox(uint8_t *out, const void *msg, size_t msg_len,
                            const uint8_t nonce[24], const uint8_t key[32]);
int hl_cap_crypto_secretbox_open(uint8_t *out, const void *ct, size_t ct_len,
                                 const uint8_t nonce[24], const uint8_t key[32]);

/* ── Public-key authenticated encryption (Curve25519+XSalsa20+Poly1305) */

#define HL_BOX_PUBLICKEYBYTES  32
#define HL_BOX_SECRETKEYBYTES  32
#define HL_BOX_NONCEBYTES      24
#define HL_BOX_MACBYTES        16

int hl_cap_crypto_box(uint8_t *out, const void *msg, size_t msg_len,
                      const uint8_t nonce[24], const uint8_t pk[32],
                      const uint8_t sk[32]);
int hl_cap_crypto_box_open(uint8_t *out, const void *ct, size_t ct_len,
                           const uint8_t nonce[24], const uint8_t pk[32],
                           const uint8_t sk[32]);
int hl_cap_crypto_box_keypair(uint8_t out_pk[32], uint8_t out_sk[32]);

#endif /* HL_CAP_CRYPTO_H */
