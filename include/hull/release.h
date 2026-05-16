/**
 * @file release.h
 * @brief Ed25519 signing/verification for release-artifact manifests.
 *
 * Distinct from `signature.h` (which signs built apps + platforms). This
 * module signs and verifies *release artifacts* — specifically the
 * `hull.sha256` checksum manifest distributed alongside every GitHub
 * release. It is the trust root for `hull update`.
 *
 * @par Roles:
 *   - **CI / release workflow:** uses @ref hl_release_load_secret_key and
 *     @ref hl_release_sign_manifest (via `hull sign-release`) with the
 *     private half stored in offline + GitHub Actions secret
 *     `HULL_RELEASE_KEY`.
 *   - **Client `hull update`:** uses @ref hl_release_verify_manifest_sig
 *     with the embedded #HL_RELEASE_PUBKEY_HEX (or `--pubkey` override).
 *
 * Design: docs/release_signing.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RELEASE_H
#define HL_RELEASE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Embedded release public key (Ed25519, 32 bytes, hex-encoded, 64 chars).
 *
 * Placeholder all-zeros until the v0.1.0 release key is generated. The
 * actual key bytes are committed before the v0.1.0 tag; the private half
 * lives in offline storage + GitHub Actions secret `HULL_RELEASE_KEY`.
 *
 * Tested for non-zero by hl_release_pubkey_configured() — when the key
 * is still the placeholder, signature verification is bypassed (with a
 * one-time warning) so pre-v0.1.0 builds can self-update.
 */
#define HL_RELEASE_PUBKEY_HEX \
    "0000000000000000000000000000000000000000000000000000000000000000"

/**
 * @brief Is the embedded release pubkey real (not all-zero placeholder)?
 *
 * Callers (notably `hull update`) should skip signature verification — with
 * a visible one-time warning — when this returns 0. This lets pre-v0.1.0
 * builds self-update without a chicken-and-egg key-bootstrap problem.
 *
 * @return 1 if #HL_RELEASE_PUBKEY_HEX is non-zero, 0 otherwise.
 */
int hl_release_pubkey_configured(void);

/**
 * @brief Decode the embedded #HL_RELEASE_PUBKEY_HEX into 32 raw bytes.
 * @return 0 on success, -1 on hex decode error (should be unreachable
 *   for a correctly-encoded build).
 */
int hl_release_pubkey_decode(uint8_t out_pk[32]);

/**
 * @brief Verify an Ed25519 signature over a release-manifest buffer.
 *
 * @param manifest      Bytes that were signed (typically the entire
 *                      `hull.sha256` file contents).
 * @param manifest_len  Length of `manifest` in bytes.
 * @param sig_hex       128 hex chars + optional trailing whitespace/NL
 *                      (stripped).
 * @param sig_hex_len   Length of `sig_hex` (may include trailing NL).
 * @param pubkey        32-byte Ed25519 public key, or NULL to use the
 *                      embedded #HL_RELEASE_PUBKEY_HEX.
 * @return 0 on valid signature, -1 on parse error or signature mismatch.
 *
 * @note Constant-time only in the verifier itself (TweetNaCl
 *   `crypto_sign_ed25519_open`). Parse failures short-circuit early.
 */
int hl_release_verify_manifest_sig(const void *manifest, size_t manifest_len,
                                   const char *sig_hex, size_t sig_hex_len,
                                   const uint8_t pubkey[32]);

/**
 * @brief Sign a release manifest. Used by `hull sign-release` in CI.
 *
 * @param manifest          Bytes to sign.
 * @param manifest_len      Length of `manifest`.
 * @param secret_key        64-byte Ed25519 secret key (output of
 *                          `hl_cap_crypto_ed25519_keypair`).
 * @param out_sig_hex       Buffer for 128 hex chars + NUL.
 * @param out_sig_hex_size  Must be >= 129.
 * @return 0 on success, -1 on any error.
 */
int hl_release_sign_manifest(const void *manifest, size_t manifest_len,
                             const uint8_t secret_key[64],
                             char *out_sig_hex, size_t out_sig_hex_size);

/**
 * @brief Load a 64-byte Ed25519 secret key from a `hull keygen` hex file.
 *
 * Accepts 128 hex chars optionally followed by a single newline.
 *
 * @param[in]  path    Path to the hex secret-key file.
 * @param[out] out_sk  64-byte buffer, filled on success.
 * @return 0 on success, -1 on file I/O or parse error.
 * @warning Caller must `hull_secure_zero()` `out_sk` after use.
 */
int hl_release_load_secret_key(const char *path, uint8_t out_sk[64]);

#endif /* HL_RELEASE_H */
