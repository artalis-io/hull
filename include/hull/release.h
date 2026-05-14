/*
 * release.h — Release artifact signature verification
 *
 * Distinct from `signature.h` (which signs built apps / platforms). This module
 * signs and verifies *release artifacts* — specifically the `hull.sha256`
 * checksum manifest distributed alongside every GitHub release.
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

/*
 * Returns 1 if HL_RELEASE_PUBKEY_HEX is non-zero (a real key has been
 * committed), 0 otherwise. Callers should skip signature verification
 * when this returns 0.
 */
int hl_release_pubkey_configured(void);

/*
 * Decode the embedded HL_RELEASE_PUBKEY_HEX into 32 raw bytes.
 * Returns 0 on success, -1 on hex decode error (should be unreachable for
 * a correctly-encoded constant).
 */
int hl_release_pubkey_decode(uint8_t out_pk[32]);

/*
 * Verify an Ed25519 signature over a release manifest buffer.
 *
 *   manifest, manifest_len  — the bytes that were signed (typically the
 *                             entire `hull.sha256` file contents).
 *   sig_hex, sig_hex_len    — 128 hex chars + optional trailing newline.
 *                             Any trailing whitespace/newline is stripped.
 *   pubkey                  — 32-byte Ed25519 public key. Pass NULL to
 *                             use the embedded HL_RELEASE_PUBKEY_HEX.
 *
 * Returns 0 on valid signature, -1 on any error or mismatch. Constant-time
 * in the sense that TweetNaCl's crypto_sign_ed25519_open is — every input
 * shape failure short-circuits early on parsing, but successful parse
 * paths reach the verifier.
 */
int hl_release_verify_manifest_sig(const void *manifest, size_t manifest_len,
                                   const char *sig_hex, size_t sig_hex_len,
                                   const uint8_t pubkey[32]);

/*
 * Sign a release manifest. Used by `hull sign-release` in the GitHub
 * Actions release workflow.
 *
 *   manifest, manifest_len  — bytes to sign.
 *   secret_key              — 64-byte Ed25519 secret key (output of
 *                             hl_cap_crypto_ed25519_keypair).
 *   out_sig_hex             — buffer for 128 hex chars + NUL.
 *   out_sig_hex_size        — must be >= 129.
 *
 * Returns 0 on success, -1 on any error.
 */
int hl_release_sign_manifest(const void *manifest, size_t manifest_len,
                             const uint8_t secret_key[64],
                             char *out_sig_hex, size_t out_sig_hex_size);

/*
 * Load a 64-byte Ed25519 secret key from a hex file (the format produced
 * by `hull keygen <prefix>` — 128 hex chars optionally trailed by a
 * newline). On success, fills out_sk[64] and returns 0.
 *
 * Caller must hull_secure_zero() out_sk after use.
 */
int hl_release_load_secret_key(const char *path, uint8_t out_sk[64]);

#endif /* HL_RELEASE_H */
