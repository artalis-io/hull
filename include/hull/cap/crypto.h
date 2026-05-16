/**
 * @file cap/crypto.h
 * @brief Cryptographic primitives.
 *
 * Hashes (SHA-256/512), HMAC, PBKDF2, Ed25519 signatures, NaCl
 * authenticated encryption (secretbox / box), base64url, and a
 * platform-grade CSPRNG.
 *
 * @par Implementation:
 *   - SHA-256, HMAC-SHA256, base64url, PBKDF2: mbedTLS.
 *   - SHA-512, crypto_auth (HMAC-SHA512/256), Ed25519, secretbox, box: TweetNaCl.
 *   - random: `getentropy(3)` on macOS/BSD, `getrandom(2)` on Linux, `BCryptGenRandom` on Windows (cosmocc).
 *
 * @par Constant-time:
 *   All verify-style functions (`*_verify`, `*_open`) use constant-time
 *   comparison internally. Hull does NOT expose a generic
 *   `crypto.compare(a, b)` that would let app code accidentally write
 *   variable-time comparisons.
 *
 * @par Key material handling:
 *   Functions that take a secret key (signing, secretbox, box) zero
 *   their local key copies via `hull_secure_zero()` before return.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_CRYPTO_H
#define HL_CAP_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* ── Hashes ────────────────────────────────────────────────────────── */

/**
 * @brief Compute SHA-256 of a byte buffer.
 *
 * @param data  Input bytes.
 * @param len   Byte count.
 * @param out   32-byte output buffer.
 *
 * @return `0` on success, `-1` on internal failure (mbedTLS error).
 */
int hl_cap_crypto_sha256(const void *data, size_t len, uint8_t out[32]);

/**
 * @brief Compute SHA-512 of a byte buffer.
 *
 * @param data  Input bytes.
 * @param len   Byte count.
 * @param out   64-byte output buffer.
 *
 * @return `0` on success, `-1` on internal failure.
 */
int hl_cap_crypto_sha512(const void *data, size_t len, uint8_t out[64]);

/* ── Random ────────────────────────────────────────────────────────── */

/**
 * @brief Fill a buffer with cryptographically-secure random bytes.
 *
 * @param buf  Output buffer.
 * @param len  Byte count. Hard-capped at 65536 by the runtime bindings to
 *             prevent accidental multi-MB allocations.
 *
 * @return `0` on success, `-1` on failure.
 *
 * @note On Linux this may block briefly during early boot if `/dev/urandom`
 *       is not yet seeded. On all platforms it never silently falls back
 *       to a weaker source.
 */
int hl_cap_crypto_random(void *buf, size_t len);

/* ── HMAC-SHA256 ───────────────────────────────────────────────────── */

/**
 * @brief HMAC-SHA256 over @p msg with key @p key.
 *
 * @param key      HMAC key bytes.
 * @param key_len  Key length. Keys longer than 64 bytes are hashed first
 *                 (RFC 2104 step 1).
 * @param msg      Message bytes.
 * @param msg_len  Message length.
 * @param out      32-byte output buffer.
 *
 * @return `0` on success, `-1` on internal failure.
 */
int hl_cap_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *msg, size_t msg_len,
                              uint8_t out[32]);

/**
 * @brief Constant-time HMAC-SHA256 verify.
 *
 * Computes HMAC-SHA256(key, msg) and compares against @p expected in
 * constant time.
 *
 * @param key       HMAC key.
 * @param key_len   Key length.
 * @param msg       Message bytes.
 * @param msg_len   Message length.
 * @param expected  32-byte expected MAC.
 *
 * @return `0` on match, `-1` on mismatch or internal failure.
 */
int hl_cap_crypto_hmac_sha256_verify(const uint8_t *key, size_t key_len,
                                      const uint8_t *msg, size_t msg_len,
                                      const uint8_t expected[32]);

/* ── HMAC-SHA512/256 (NaCl crypto_auth) ────────────────────────────── */

/**
 * @brief NaCl `crypto_auth` — HMAC-SHA512 truncated to 256 bits.
 *
 * Slightly faster than HMAC-SHA256 on 64-bit platforms; equivalent security.
 *
 * @param msg      Message bytes.
 * @param msg_len  Message length.
 * @param key      32-byte secret key.
 * @param out      32-byte authentication tag.
 *
 * @return `0` on success, `-1` on internal failure.
 */
int hl_cap_crypto_auth(const void *msg, size_t msg_len,
                       const uint8_t key[32], uint8_t out[32]);

/**
 * @brief Verify a NaCl `crypto_auth` tag (constant time).
 *
 * @param tag      32-byte tag to verify.
 * @param msg      Message bytes.
 * @param msg_len  Message length.
 * @param key      32-byte secret key.
 *
 * @return `0` on match, `-1` on mismatch.
 */
int hl_cap_crypto_auth_verify(const uint8_t tag[32],
                              const void *msg, size_t msg_len,
                              const uint8_t key[32]);

/* ── Base64url (no padding, RFC 4648 §5) ───────────────────────────── */

/**
 * @brief Encode bytes to base64url (no padding).
 *
 * @param data      Input bytes.
 * @param len       Byte count.
 * @param out       Output buffer (caller-allocated).
 * @param out_size  Capacity of @p out (in bytes, not including a NUL).
 * @param out_len   Out-parameter: encoded length written to @p out.
 *
 * @return `0` on success, `-1` if @p out_size is too small.
 */
int hl_cap_crypto_base64url_encode(const void *data, size_t len,
                                   char *out, size_t out_size,
                                   size_t *out_len);

/**
 * @brief Decode base64url (no padding required) to bytes.
 *
 * @param str       Input string.
 * @param str_len   Input length.
 * @param out       Output buffer.
 * @param out_size  Capacity.
 * @param out_len   Out-parameter: decoded length.
 *
 * @return `0` on success, `-1` on invalid input or insufficient capacity.
 */
int hl_cap_crypto_base64url_decode(const char *str, size_t str_len,
                                   uint8_t *out, size_t out_size,
                                   size_t *out_len);

/* ── Password-based key derivation (PBKDF2-HMAC-SHA256) ────────────── */

/**
 * @brief PBKDF2 with HMAC-SHA256.
 *
 * @param password    Password bytes.
 * @param pw_len      Password length.
 * @param salt        Salt bytes (recommended ≥ 16).
 * @param salt_len    Salt length.
 * @param iterations  Iteration count. Hull defaults to 100_000 in the
 *                    stdlib (`crypto.hash_password`); the OWASP minimum
 *                    for PBKDF2-HMAC-SHA256 is also 100_000 as of 2026.
 * @param out         Output buffer.
 * @param out_len     Desired output length (commonly 32).
 *
 * @return `0` on success, `-1` on internal failure.
 */
int hl_cap_crypto_pbkdf2(const char *password, size_t pw_len,
                           const uint8_t *salt, size_t salt_len,
                           int iterations,
                           uint8_t *out, size_t out_len);

/* ── Ed25519 signatures ────────────────────────────────────────────── */

/**
 * @brief Verify an Ed25519 signature.
 *
 * @param msg      Signed message.
 * @param msg_len  Message length.
 * @param sig      64-byte signature.
 * @param pubkey   32-byte public key.
 *
 * @return `0` on valid signature, `-1` on invalid.
 */
int hl_cap_crypto_ed25519_verify(const uint8_t *msg, size_t msg_len,
                                   const uint8_t sig[64],
                                   const uint8_t pubkey[32]);

/**
 * @brief Sign a message with Ed25519.
 *
 * @param msg         Message to sign.
 * @param msg_len     Length.
 * @param secret_key  64-byte expanded secret key (as produced by
 *                    @ref hl_cap_crypto_ed25519_keypair).
 * @param out_sig     64-byte signature output.
 *
 * @return `0` on success, `-1` on failure.
 *
 * @note Local copies of @p secret_key are zeroed on exit via `hull_secure_zero`.
 */
int hl_cap_crypto_ed25519_sign(const uint8_t *msg, size_t msg_len,
                                 const uint8_t secret_key[64],
                                 uint8_t out_sig[64]);

/**
 * @brief Generate a fresh Ed25519 keypair.
 *
 * @param out_pk  32-byte public key output.
 * @param out_sk  64-byte expanded secret key output.
 *
 * @return `0` on success, `-1` on CSPRNG failure.
 */
int hl_cap_crypto_ed25519_keypair(uint8_t out_pk[32], uint8_t out_sk[64]);

/* ── Secret-key authenticated encryption (XSalsa20+Poly1305) ───────── */

#define HL_SECRETBOX_KEYBYTES   32 /**< Symmetric key size (bytes). */
#define HL_SECRETBOX_NONCEBYTES 24 /**< Required nonce size. */
#define HL_SECRETBOX_MACBYTES   16 /**< Authenticated overhead per ciphertext. */

/**
 * @brief NaCl secretbox encrypt (authenticated, XSalsa20+Poly1305).
 *
 * @param out      Ciphertext output. Capacity must be `msg_len + HL_SECRETBOX_MACBYTES`.
 * @param msg      Plaintext.
 * @param msg_len  Plaintext length.
 * @param nonce    24-byte nonce. **Must be unique per `(key, message)` pair.**
 * @param key      32-byte symmetric key.
 *
 * @return `0` on success, `-1` on internal failure.
 *
 * @warning Nonce reuse with the same key is catastrophic — Hull does not
 *          generate nonces for you. Use `crypto.random(24)` per encryption.
 */
int hl_cap_crypto_secretbox(uint8_t *out, const void *msg, size_t msg_len,
                            const uint8_t nonce[24], const uint8_t key[32]);

/**
 * @brief NaCl secretbox decrypt + verify.
 *
 * @param out     Plaintext output. Capacity must be `ct_len - HL_SECRETBOX_MACBYTES`.
 * @param ct      Ciphertext.
 * @param ct_len  Ciphertext length.
 * @param nonce   24-byte nonce (the same one used for encryption).
 * @param key     32-byte symmetric key.
 *
 * @return `0` on valid + decrypted, `-1` on authentication failure (do NOT
 *         use @p out in that case — it may contain partial garbage).
 */
int hl_cap_crypto_secretbox_open(uint8_t *out, const void *ct, size_t ct_len,
                                 const uint8_t nonce[24], const uint8_t key[32]);

/* ── Public-key authenticated encryption (Curve25519+XSalsa20+Poly1305) */

#define HL_BOX_PUBLICKEYBYTES  32 /**< Curve25519 public key (32 bytes). */
#define HL_BOX_SECRETKEYBYTES  32 /**< Curve25519 secret key (32 bytes). */
#define HL_BOX_NONCEBYTES      24 /**< Nonce size — same as secretbox. */
#define HL_BOX_MACBYTES        16 /**< Authenticated overhead per ciphertext. */

/**
 * @brief NaCl box encrypt (sender signs for one specific recipient).
 *
 * @param out      Ciphertext output. Capacity `msg_len + HL_BOX_MACBYTES`.
 * @param msg      Plaintext.
 * @param msg_len  Plaintext length.
 * @param nonce    24-byte nonce.
 * @param pk       32-byte **recipient** public key.
 * @param sk       32-byte **sender** secret key.
 *
 * @return `0` on success, `-1` on failure.
 */
int hl_cap_crypto_box(uint8_t *out, const void *msg, size_t msg_len,
                      const uint8_t nonce[24], const uint8_t pk[32],
                      const uint8_t sk[32]);

/**
 * @brief NaCl box decrypt + verify.
 *
 * @param out      Plaintext output. Capacity `ct_len - HL_BOX_MACBYTES`.
 * @param ct       Ciphertext.
 * @param ct_len   Ciphertext length.
 * @param nonce    Nonce used at encrypt time.
 * @param pk       32-byte **sender** public key.
 * @param sk       32-byte **recipient** secret key.
 *
 * @return `0` on success, `-1` on authentication failure.
 */
int hl_cap_crypto_box_open(uint8_t *out, const void *ct, size_t ct_len,
                           const uint8_t nonce[24], const uint8_t pk[32],
                           const uint8_t sk[32]);

/**
 * @brief Generate a fresh Curve25519 keypair (for box).
 *
 * @param out_pk  32-byte public key.
 * @param out_sk  32-byte secret key.
 *
 * @return `0` on success, `-1` on CSPRNG failure.
 */
int hl_cap_crypto_box_keypair(uint8_t out_pk[32], uint8_t out_sk[32]);

#endif /* HL_CAP_CRYPTO_H */
