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

/* ── Hashes ──────────────────────────────────────────────────────────
 *
 * SHA-256 / SHA-512 deliberately stay as direct TweetNaCl calls
 * (with hardware-accelerated SHA-256 paths on platforms that have
 * them) - they're tight, well-tested, and pure functions with no
 * keyed-secret leak surface. Moving them behind a backend vtable
 * would duplicate the hash infrastructure that HlCryptoAsymBackend's
 * RSA / ECDSA verify already maintains internally, for no observable
 * benefit. If a future need arises (FIPS box, HSM offload), this is
 * the natural spot to drop in HlCryptoHashBackend; until then,
 * direct dispatch is the right trade.
 */

/**
 * @brief Compute SHA-256 of a byte buffer.
 *
 * @param data  Input bytes.
 * @param len   Byte count.
 * @param out   32-byte output buffer.
 *
 * @return `0` on success, `-1` on internal failure.
 */
int hl_cap_crypto_sha256(const void *data, size_t len, uint8_t out[32]);

/* ── Incremental SHA-256 ──────────────────────────────────────────── */

/**
 * @brief Incremental SHA-256 hasher state.
 *
 * Opaque to callers - fields are public so the struct can be stack- or
 * userdata-allocated, but only the init/update/final functions should
 * touch them. Hashing one buffer of `N` bytes incrementally produces
 * the exact same 32-byte digest as `hl_cap_crypto_sha256(buf, N)`.
 */
typedef struct {
    uint32_t state[8];   /**< Working SHA-256 state. */
    uint8_t  buf[64];    /**< Partial-block buffer (< 64 unprocessed bytes). */
    size_t   buf_len;    /**< Bytes currently in `buf`. */
    uint64_t total_bits; /**< Bits absorbed so far (for the length pad). */
} HlSha256Ctx;

/**
 * @brief Initialize an incremental SHA-256 context.
 *
 * Always succeeds; the context is ready for `_update` calls on return.
 */
void hl_cap_crypto_sha256_init(HlSha256Ctx *ctx);

/**
 * @brief Feed bytes into an incremental SHA-256 context.
 *
 * Safe to call any number of times with any chunk size, including 0.
 *
 * @param ctx   Initialized context.
 * @param data  Bytes to absorb.
 * @param len   Byte count (0 is a no-op).
 *
 * @return `0` on success, `-1` on NULL ctx/data.
 */
int hl_cap_crypto_sha256_update(HlSha256Ctx *ctx,
                                  const void *data, size_t len);

/**
 * @brief Finalize an incremental SHA-256 and write the digest.
 *
 * The context must not be used after `_final` - call `_init` again to
 * reuse it.
 *
 * @param ctx  Context to finalize.
 * @param out  32-byte output buffer.
 *
 * @return `0` on success, `-1` on NULL ctx/out.
 */
int hl_cap_crypto_sha256_final(HlSha256Ctx *ctx, uint8_t out[32]);

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

/**
 * @brief Compute SHA-1 of a byte buffer.
 *
 * **LEGACY INTEROP ONLY.** SHA-1 is cryptographically broken for
 * collision-resistance (Stevens et al. 2017). Exposed strictly to
 * drive legacy/3rd-party protocols whose wire format hardcodes
 * SHA-1 (HIBP range API, certain CSV/PDF signing standards, etc.).
 *
 * **DO NOT USE for new cryptography** - for new password hashing,
 * MAC, or digest needs use `hash_password` / `hmac_sha256` /
 * `sha256` instead.
 *
 * @param data  Input bytes.
 * @param len   Byte count.
 * @param out   20-byte output buffer.
 *
 * @return `0` on success, `-1` on internal failure.
 */
int hl_cap_crypto_sha1(const void *data, size_t len, uint8_t out[20]);

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

/* ── HMAC backend vtable + HMAC-SHA1 (HOTP/TOTP only) ──────────────── */

/** HMAC algorithm identifier. Values are stable wire-level integers;
 *  backend impls and tests rely on them as dispatch keys. Don't
 *  renumber.
 */
typedef enum {
    HL_CRYPTO_HMAC_NONE   = 0, /**< Sentinel: rejected by every compute path. */
    HL_CRYPTO_HMAC_SHA1   = 1, /**< HMAC-SHA1, 20-byte output. HOTP/TOTP only. */
    HL_CRYPTO_HMAC_SHA256 = 2, /**< HMAC-SHA256, 32-byte output. */
    HL_CRYPTO_HMAC_SHA512 = 3, /**< HMAC-SHA512, 64-byte output. */
} HlCryptoHmacAlg;

/** Backend vtable for HMAC compute. Same swap-friendly convention as
 *  HlCryptoAsymBackend / HlCompilerVtable / HlGpuBackendVtable.
 *
 *  Every HMAC alg (SHA1 / SHA256 / SHA512) routes through this
 *  interface - including hl_cap_crypto_hmac_sha256, which was
 *  originally an in-tree HMAC construction over hl_cap_crypto_sha256
 *  but was migrated to the vtable on 2026-06-13 so a future backend
 *  swap covers all three algs in one drop-in (mbedTLS today; could
 *  be WolfSSL, BoringSSL, a hardware token later) without disturbing
 *  the cap surface or the Lua / JS bindings.
 *
 *  PBKDF2-HMAC-SHA256 (hl_cap_crypto_pbkdf2) transitively benefits
 *  via its internal HMAC calls.
 */
typedef struct HlCryptoHmacBackend {
    /** Returns 1 if the backend can compute HMAC under this alg. */
    int (*supports)(HlCryptoHmacAlg alg);

    /** Compute HMAC(key, msg) under @p alg. @p out_len must equal the
     *  alg's digest size (20 for SHA1, 32 for SHA256, 64 for SHA512);
     *  truncation is the caller's responsibility. Returns 0 on
     *  success, -1 on internal failure or unsupported alg.
     */
    int (*compute)(const struct HlCryptoHmacBackend *self,
                   HlCryptoHmacAlg alg,
                   const uint8_t *key, size_t key_len,
                   const uint8_t *msg, size_t msg_len,
                   uint8_t *out, size_t out_len);
} HlCryptoHmacBackend;

/** Built-in mbedTLS HMAC backend. Always present in builds that link
 *  mbedTLS (any build with `HL_ENABLE_HTTP_CLIENT=1` or
 *  `HL_ENABLE_HTTP_SERVER=1`). On builds without mbedTLS the symbol
 *  still exists but every compute call returns -1.
 */
extern const HlCryptoHmacBackend hl_crypto_hmac_backend_mbedtls;

/** Portable, mbedTLS-free HMAC backend. Computes HMAC over the in-tree
 *  hand-rolled hashes (SHA-256 incremental + SHA-1). Always compiled, so
 *  it is testable in every build; the cap layer selects it as the active
 *  backend only when mbedTLS is absent (the pure-compute flavor, built
 *  with both HTTP halves off). Supports HMAC-SHA1 + HMAC-SHA256; rejects
 *  HMAC-SHA512 (the cap layer never requests it).
 */
extern const HlCryptoHmacBackend hl_crypto_hmac_backend_portable;

/**
 * @brief Compute HMAC-SHA1.
 *
 * SHA-1 is cryptographically deprecated for general hashing. This
 * primitive exists ONLY to implement HOTP (RFC 4226) and TOTP
 * (RFC 6238), both of which fix HMAC-SHA1 in the on-the-wire format
 * that authenticator apps (Google Authenticator, Authy, 1Password,
 * etc.) interoperate over. Do NOT use this for new MAC schemes -
 * use `hl_cap_crypto_hmac_sha256` instead.
 *
 * Dispatched through @ref HlCryptoHmacBackend so the impl can be
 * swapped. No raw SHA-1 primitive is exposed.
 *
 * @param key      HMAC key bytes.
 * @param key_len  Key length. Keys longer than 64 bytes are hashed
 *                 first (RFC 2104 step 1) by the backend.
 * @param msg      Message bytes.
 * @param msg_len  Message length.
 * @param out      20-byte output buffer.
 *
 * @return `0` on success, `-1` on internal failure.
 */
int hl_cap_crypto_hmac_sha1(const uint8_t *key, size_t key_len,
                            const uint8_t *msg, size_t msg_len,
                            uint8_t out[20]);

/* ── HMAC-SHA512/256 (NaCl crypto_auth) ────────────────────────────── */

/**
 * @brief NaCl `crypto_auth` - HMAC-SHA512 truncated to 256 bits.
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

/* ── Lowercase hex encode / decode ─────────────────────────────────── */

/**
 * @brief Lowercase hex-encode a byte buffer.
 *
 * Writes exactly `2 * in_len` characters into @p out_hex (no NUL).
 *
 * @param in        Input bytes.
 * @param in_len    Byte count.
 * @param out_hex   Output buffer (caller-allocated).
 * @param out_size  Capacity of @p out_hex (must be >= 2 * in_len).
 *
 * @return Number of characters written (`2 * in_len`) on success,
 *         or `-1` if @p out_size is too small.
 */
int hl_cap_crypto_hex_encode(const uint8_t *in, size_t in_len,
                             char *out_hex, size_t out_size);

/**
 * @brief Decode a lowercase or uppercase hex string into bytes.
 *
 * @param hex       Hex input (NOT required to be NUL-terminated).
 * @param hex_len   Hex character count (must be even).
 * @param out       Output buffer.
 * @param out_size  Capacity of @p out (must be >= hex_len / 2).
 *
 * @return Bytes written (`hex_len / 2`) on success,
 *         or `-1` on odd length, non-hex character, or insufficient
 *         capacity.
 */
int hl_cap_crypto_hex_decode(const char *hex, size_t hex_len,
                             uint8_t *out, size_t out_size);

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

/* ── Asymmetric verify (RSA / ECDSA, PEM-keyed) ────────────────────── */

/** Asymmetric-signature algorithm identifier. Values are stable wire-
 *  level integers; backend impls and tests rely on them as dispatch
 *  keys. Don't renumber.
 */
typedef enum {
    HL_CRYPTO_ASYM_NONE  = 0, /**< Sentinel: rejected by every verify path. */
    HL_CRYPTO_ASYM_RS256 = 1, /**< RSA-PKCS1v15, SHA-256. */
    HL_CRYPTO_ASYM_RS384 = 2, /**< RSA-PKCS1v15, SHA-384. */
    HL_CRYPTO_ASYM_RS512 = 3, /**< RSA-PKCS1v15, SHA-512. */
    HL_CRYPTO_ASYM_PS256 = 4, /**< RSA-PSS, SHA-256, MGF1-SHA256, salt=32. */
    HL_CRYPTO_ASYM_ES256 = 5, /**< ECDSA P-256, SHA-256, raw r||s. */
    HL_CRYPTO_ASYM_ES384 = 6, /**< ECDSA P-384, SHA-384, raw r||s. */
} HlCryptoAsymAlg;

/** Backend vtable for asymmetric verify. The vtable so the impl can
 *  be swapped later (mbedTLS today; could be BoringSSL, WolfSSL, a
 *  hardware token, etc.) without disturbing the cap surface or the
 *  Lua / JS bindings - same pattern as `HlCompilerVtable` /
 *  `HlGpuBackendVtable`. The existing primitives in this header
 *  (SHA / HMAC / Ed25519) call mbedTLS / TweetNaCl directly because
 *  they shipped before the vtable convention; new asymmetric work
 *  goes through this interface from day one.
 */
typedef struct HlCryptoAsymBackend {
    /** Returns 1 if the backend can verify with this alg, 0 otherwise. */
    int (*supports)(HlCryptoAsymAlg alg);

    /** Verify a signature. Same contract as @ref
     *  hl_cap_crypto_asym_verify (this is the underlying call the
     *  cap surface wraps).
     */
    int (*verify)(const void *pubkey_pem, size_t pubkey_len,
                  HlCryptoAsymAlg alg,
                  const void *data, size_t data_len,
                  const void *sig,  size_t sig_len);
} HlCryptoAsymBackend;

/** Built-in mbedTLS asym backend. Present only in builds that link mbedTLS
 *  (any build with `HL_ENABLE_HTTP_CLIENT=1` or `HL_ENABLE_HTTP_SERVER=1`, or a
 *  composed TLS feature). On a TLS-less base the symbol is ABSENT; the active
 *  backend is then a fail-closed stub whose `verify` returns -2. Prefer
 *  `hl_crypto_asym_active_backend()` (hull/tls_feature.h) /
 *  `hl_cap_crypto_asym_verify_default()` over referencing this symbol directly,
 *  so code links on both TLS and TLS-less bases. See docs/tls_feature.md.
 */
extern const HlCryptoAsymBackend hl_crypto_asym_backend_mbedtls;

/** Parse an alg string ("RS256", "rs256", ...) into the numeric tag.
 *  Case-insensitive. Returns HL_CRYPTO_ASYM_NONE on unknown alg.
 */
HlCryptoAsymAlg hl_crypto_asym_alg_from_string(const char *s, size_t len);

/** Inverse: numeric tag to canonical uppercase string. Returns NULL
 *  for HL_CRYPTO_ASYM_NONE.
 */
const char *hl_crypto_asym_alg_to_string(HlCryptoAsymAlg alg);

/** Verify @p sig over @p data with @p pubkey under @p alg.
 *
 *  Public-key input is PEM-encoded SubjectPublicKeyInfo. ECDSA
 *  signatures must be JOSE raw r||s (NOT DER), matching the JWT
 *  wire format. Caller-supplied length, NUL termination not
 *  required (the backend handles both with/without trailing
 *  newline).
 *
 *  @return  0 on a valid signature.
 *          -1 if the signature does not verify (bad sig OR malformed
 *             PEM OR wrong key type for alg OR mismatched sig_len).
 *          -2 on a programming error: NULL pointer, zero-length
 *             input where one is required, unsupported alg.
 *
 *  Treat the -1 / -2 distinction as advisory. Callers should fail
 *  closed on anything non-zero and avoid surfacing the distinction
 *  to end users (it's an oracle).
 */
int hl_cap_crypto_asym_verify(const HlCryptoAsymBackend *backend,
                              const void *pubkey_pem, size_t pubkey_len,
                              HlCryptoAsymAlg alg,
                              const void *data, size_t data_len,
                              const void *sig,  size_t sig_len);

/** Convenience wrapper: verify via the built-in mbedTLS backend. */
int hl_cap_crypto_asym_verify_default(const void *pubkey_pem, size_t pubkey_len,
                                      HlCryptoAsymAlg alg,
                                      const void *data, size_t data_len,
                                      const void *sig,  size_t sig_len);

/** Extract the SubjectPublicKeyInfo PEM from an X.509 certificate.
 *
 *  Use case: OIDC JWKS endpoints typically include an `x5c` field
 *  with the X.509 cert chain (base64-DER); this helper bridges from
 *  that DER blob to the PEM that @ref hl_cap_crypto_asym_verify
 *  consumes, without forcing the caller to do ASN.1 + DER assembly
 *  from raw JWK components.
 *
 *  @param der        DER-encoded X.509 certificate bytes.
 *  @param der_len    Length of @p der.
 *  @param out_pem    Output buffer for the PEM-encoded public key
 *                    (NUL-terminated, headers "BEGIN PUBLIC KEY").
 *  @param out_size   Capacity of @p out_pem (recommend >= 4096 to
 *                    accommodate RSA-4096 + line wrapping).
 *  @param out_len    Out: number of bytes written (excluding NUL).
 *
 *  @return  0 on success, -1 on parse error, NULL input, or
 *           insufficient output capacity.
 */
int hl_cap_crypto_x509_pubkey_pem(const void *der, size_t der_len,
                                  char *out_pem, size_t out_size,
                                  size_t *out_len);

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
 * @warning Nonce reuse with the same key is catastrophic - Hull does not
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
 *         use @p out in that case - it may contain partial garbage).
 */
int hl_cap_crypto_secretbox_open(uint8_t *out, const void *ct, size_t ct_len,
                                 const uint8_t nonce[24], const uint8_t key[32]);

/* ── Public-key authenticated encryption (Curve25519+XSalsa20+Poly1305) */

#define HL_BOX_PUBLICKEYBYTES  32 /**< Curve25519 public key (32 bytes). */
#define HL_BOX_SECRETKEYBYTES  32 /**< Curve25519 secret key (32 bytes). */
#define HL_BOX_NONCEBYTES      24 /**< Nonce size - same as secretbox. */
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
