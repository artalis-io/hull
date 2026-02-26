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

#endif /* HL_CAP_CRYPTO_H */
