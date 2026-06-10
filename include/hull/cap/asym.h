/**
 * @file cap/asym.h
 * @brief Asymmetric-signature verification (RSA / ECDSA).
 *
 * Verify-only surface. No signing (Hull apps don't sign with RSA /
 * ECDSA keys; the only Hull-owned signing is Ed25519 in cap/crypto.c).
 * The verify side unlocks JWT validation for the RS / ES family (OIDC
 * ID tokens) and inbound webhook signatures from external IdPs or
 * SaaS providers.
 *
 * Backend vtable:
 *   The underlying implementation is selected through @ref HlAsymBackend
 *   so the impl can be swapped later (BoringSSL, a hardware token, a
 *   future native ECC lib) without disturbing the cap surface or the
 *   Lua / JS bindings. Today the only ship-ready backend is
 *   `hl_asym_backend_mbedtls`. New backends drop in by implementing
 *   the same vtable.
 *
 * Algorithms (this iteration):
 *   - RS256 / RS384 / RS512: RSA PKCS#1 v1.5 with SHA-256 / 384 / 512.
 *   - PS256: RSA-PSS with SHA-256, MGF1-SHA-256, salt length = 32.
 *   - ES256 / ES384: ECDSA over P-256 / P-384 with SHA-256 / 384.
 *     Signature is the raw r||s concatenation (NOT DER-encoded),
 *     which is what JOSE / JWT uses.
 *
 * Constant-time:
 *   The verify itself is constant-time (mbedTLS handles this
 *   internally). The result return is a strict 0 / -1; callers must
 *   not branch on intermediate state.
 *
 * Inputs:
 *   - Public keys are PEM-encoded SubjectPublicKeyInfo
 *     ("BEGIN PUBLIC KEY"). Caller supplies the key length
 *     explicitly; the PEM bytes do NOT need to be NUL-terminated.
 *   - JWK input format is NOT supported in this iteration; callers
 *     using JWKS endpoints must convert JWK to PEM in the stdlib
 *     (planned follow-up).
 *
 * Capability boundary:
 *   This is a pure-compute surface. No I/O, no sandbox-relevant
 *   operations. Can be called from any runtime context. The mbedTLS
 *   backend pulls entropy from the platform CSPRNG
 *   (`hl_cap_crypto_random`) for the PSS verify path, which is
 *   already sandbox-permitted.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_ASYM_H
#define HL_CAP_ASYM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Asymmetric-signature algorithm identifier.
 *
 *  Values are stable wire-level integers. Backend impls and tests can
 *  rely on them as dispatch keys. Don't renumber without updating
 *  every backend.
 */
typedef enum {
    HL_ASYM_NONE  = 0,  /**< Sentinel: rejected by every verify path. */
    HL_ASYM_RS256 = 1,  /**< RSA-PKCS1v15, SHA-256. */
    HL_ASYM_RS384 = 2,  /**< RSA-PKCS1v15, SHA-384. */
    HL_ASYM_RS512 = 3,  /**< RSA-PKCS1v15, SHA-512. */
    HL_ASYM_PS256 = 4,  /**< RSA-PSS, SHA-256, MGF1-SHA256, salt=32. */
    HL_ASYM_ES256 = 5,  /**< ECDSA P-256, SHA-256, raw r||s. */
    HL_ASYM_ES384 = 6,  /**< ECDSA P-384, SHA-384, raw r||s. */
} HlAsymAlg;

/** Backend vtable for asymmetric verify.
 *
 *  A backend implements `verify` against its underlying crypto lib
 *  (mbedTLS today; could be BoringSSL / WolfSSL / a hardware token
 *  later). The verify signature must match the contract documented on
 *  @ref hl_cap_asym_verify below.
 */
typedef struct HlAsymBackend {
    /** Algorithm support probe. Returns 1 if the backend can verify
     *  with this alg, 0 otherwise. Backends should be conservative:
     *  if the underlying lib was built without the relevant primitive,
     *  return 0 rather than failing at verify time.
     */
    int (*supports)(HlAsymAlg alg);

    /** Verify a signature. Same contract as @ref hl_cap_asym_verify
     *  (this is the underlying call the cap surface wraps).
     */
    int (*verify)(const void *pubkey_pem, size_t pubkey_len,
                  HlAsymAlg alg,
                  const void *data, size_t data_len,
                  const void *sig,  size_t sig_len);
} HlAsymBackend;

/** Built-in mbedTLS backend. Always present in builds that link
 *  mbedTLS (i.e. anything with `HL_ENABLE_HTTP_CLIENT=1`). On builds
 *  without mbedTLS the symbol still exists but its `verify` returns
 *  -2 (input error) for every alg.
 */
extern const HlAsymBackend hl_asym_backend_mbedtls;

/** Parse an alg string ("RS256", "rs256", "ES256", ...) into the
 *  numeric tag. Case-insensitive. Returns HL_ASYM_NONE if the string
 *  doesn't name a supported algorithm, so callers can blanket-reject
 *  by comparing to HL_ASYM_NONE.
 */
HlAsymAlg hl_asym_alg_from_string(const char *s, size_t len);

/** Inverse of @ref hl_asym_alg_from_string. Returns a static
 *  uppercase string ("RS256", "ES256", ...) or NULL for HL_ASYM_NONE.
 */
const char *hl_asym_alg_to_string(HlAsymAlg alg);

/** Verify @p sig over @p data with @p pubkey under @p alg via the
 *  given @p backend.
 *
 *  @param backend     Backend to dispatch through. Pass
 *                     `&hl_asym_backend_mbedtls` for the default.
 *                     NULL is rejected as a programming error.
 *  @param pubkey_pem  PEM-encoded SubjectPublicKeyInfo bytes.
 *                     Does NOT need to be NUL-terminated.
 *  @param pubkey_len  Length of @p pubkey_pem.
 *  @param alg         Algorithm. HL_ASYM_NONE is unconditionally
 *                     rejected (-2).
 *  @param data        Message bytes. The backend hashes them
 *                     internally per @p alg (SHA-256/384/512).
 *  @param data_len    Length of @p data.
 *  @param sig         Raw signature bytes.
 *                       - RSA (RS / PS): MUST equal the modulus
 *                         length in bytes (e.g. 256 for RSA-2048).
 *                       - ECDSA (ES): MUST equal `2 * coord_bytes`
 *                         (64 for P-256, 96 for P-384), JOSE's
 *                         raw r||s, NOT DER.
 *  @param sig_len     Length of @p sig.
 *  @return  0 on a valid signature.
 *          -1 if the signature does not verify (bad sig OR malformed
 *             PEM OR wrong key type for @p alg OR mismatched sig_len).
 *          -2 on a programming error: NULL pointer, zero-length
 *             input where one is required, unsupported alg.
 *
 *  Treat the -1 / -2 distinction as advisory; callers should
 *  generally fail closed on anything non-zero. Don't expose -1 vs -2
 *  to end users in a way that leaks oracle information about the
 *  failure cause.
 */
int hl_cap_asym_verify(const HlAsymBackend *backend,
                       const void *pubkey_pem, size_t pubkey_len,
                       HlAsymAlg alg,
                       const void *data, size_t data_len,
                       const void *sig,  size_t sig_len);

/** Convenience wrapper. Equivalent to
 *  `hl_cap_asym_verify(&hl_asym_backend_mbedtls, ...)`.
 */
int hl_cap_asym_verify_default(const void *pubkey_pem, size_t pubkey_len,
                               HlAsymAlg alg,
                               const void *data, size_t data_len,
                               const void *sig,  size_t sig_len);

#ifdef __cplusplus
}
#endif

#endif /* HL_CAP_ASYM_H */
