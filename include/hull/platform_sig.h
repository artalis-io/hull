/**
 * @file platform_sig.h
 * @brief Build, sign, verify, and parse the per-arch platform manifest.
 *
 * The platform manifest is a deterministic text format identical in
 * shape to `hull.sha256` and signed under the gethull.dev platform
 * Ed25519 key (the same `HULL_PLATFORM_KEY` secret already used by
 * `hull sign-platform`). One line per published arch:
 *
 *     <64-hex-sha256>  <arch-name>\n
 *
 * Arches are emitted in `LC_ALL=C` sort order so the manifest is
 * byte-reproducible across CI runs regardless of input ordering. The
 * signature is computed over the canonical manifest bytes, identical
 * pattern to `hull.sha256` + `hull.sha256.sig`.
 *
 * Why a separate module from `release.h` / `release_io.h`:
 *   - `release.{h,c}` owns the release-side trust chain (hull.sha256,
 *     HL_RELEASE_PUBKEY_HEX) — DIFFERENT key, DIFFERENT manifest.
 *   - `release_io.{h,c}` owns the I/O + crypto primitives that BOTH
 *     trust chains share (HTTPS GET, SHA-256 hex, manifest-line
 *     lookup, atomic write). This module reuses them.
 *   - Platform-specific concerns — per-arch sort, arch-name iteration,
 *     pinning against `HL_PLATFORM_PUBKEY_HEX` — live here.
 *
 * v0.1.3 wire-up:
 *   - Build-time (release.yml): `sign-platform-manifest` job invokes
 *     `hl_platform_sig_build_manifest()` + `hl_platform_sig_sign()`
 *     to produce `embedded_platform_sig.h` for embedding into the
 *     hull binary.
 *   - Build-time (hull build): `hl_platform_sig_extract_for_arch()`
 *     pulls THIS arch's expected hash from the embedded manifest;
 *     `build.lua` cross-checks against the linked `libhull_platform.a`.
 *   - Runtime (`hull verify` / `--verify-sig`):
 *     `hl_platform_sig_verify()` validates `package.sig.platform`'s
 *     embedded manifest + signature against `HL_PLATFORM_PUBKEY_HEX`.
 *
 * Pure data — no I/O, no global state. Safe to call from CI tools,
 * `hull build`, runtime verifier, or unit tests.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_PLATFORM_SIG_H
#define HL_PLATFORM_SIG_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief One arch entry: { name, SHA-256-hex }.
 *
 * Arch names are stable strings matching `hl_release_io_platform()`
 * output plus the cosmo variants — `"linux-x86_64"`,
 * `"linux-aarch64"`, `"darwin-arm64"`, `"cosmo-x86_64"`,
 * `"cosmo-aarch64"`. Hash strings MUST be 64 lowercase hex chars.
 */
typedef struct {
    const char *arch;
    const char *hash_hex;
} HlPlatformArchHash;

/**
 * @brief Build the canonical platform manifest text.
 *
 * Sorts entries by arch name (LC_ALL=C `strcmp` order) and emits one
 * line per arch as `<hash_hex>  <arch>\n`. Two spaces between hash
 * and name (matches `sha256sum` output, matches what
 * `hl_release_io_find_checksum()` parses).
 *
 * @param entries   Per-arch hashes (NOT pre-sorted; this helper sorts).
 * @param n         Number of entries (max 16; release matrix today has 5).
 * @param out       Buffer for the manifest text. NOT NUL-terminated.
 * @param out_sz    Capacity of @p out.
 * @param out_len   Filled with the number of bytes written.
 * @return 0 on success, -1 on invalid arg / overflow / duplicate arch.
 */
int hl_platform_sig_build_manifest(const HlPlatformArchHash *entries, size_t n,
                                   char *out, size_t out_sz,
                                   size_t *out_len);

/**
 * @brief Sign a manifest blob with the platform Ed25519 secret key.
 *
 * Thin wrapper over `hl_release_sign_manifest()` — they use the same
 * underlying Ed25519 implementation. Distinct API name so the caller's
 * intent (platform vs release) is explicit at the call site.
 *
 * @param manifest         Manifest bytes (typically from
 *                         `hl_platform_sig_build_manifest`).
 * @param manifest_len     Length of @p manifest.
 * @param secret_key       64-byte Ed25519 secret key
 *                         (`HULL_PLATFORM_KEY` decoded).
 * @param out_sig_hex      Buffer for 128 hex chars + NUL.
 * @param out_sig_hex_size Must be >= 129.
 * @return 0 on success, -1 on any error.
 */
int hl_platform_sig_sign(const void *manifest, size_t manifest_len,
                         const uint8_t secret_key[64],
                         char *out_sig_hex, size_t out_sig_hex_size);

/**
 * @brief Verify an Ed25519 signature over a platform manifest.
 *
 * @param manifest     Bytes that were signed.
 * @param manifest_len Length of @p manifest.
 * @param sig_hex      128 hex chars + optional trailing whitespace/NL.
 * @param sig_hex_len  Length of @p sig_hex.
 * @param pubkey       32-byte Ed25519 public key, or NULL to use the
 *                     embedded `HL_PLATFORM_PUBKEY_HEX`.
 * @return 0 on valid signature, -1 on parse error or mismatch.
 *
 * @note Constant-time only in the verifier itself (TweetNaCl
 *   `crypto_sign_ed25519_open`). Parse failures short-circuit early.
 */
int hl_platform_sig_verify(const void *manifest, size_t manifest_len,
                           const char *sig_hex, size_t sig_hex_len,
                           const uint8_t pubkey[32]);

/**
 * @brief True iff `HL_PLATFORM_PUBKEY_HEX` is the all-zeros placeholder
 *        sentinel.
 *
 * Single source of truth for "this hull was built without a real
 * pinned platform pubkey." Used by both runtime verify (signature.c
 * §5b skips with a warning) and the `tool.platform_pubkey()` Lua
 * binding (returns nil so verify.lua takes the placeholder-skip
 * branch). Keeping the check in one place stops the two consumers
 * from drifting if the macro's formatting ever changes (uppercase
 * hex, surrounding whitespace, etc.).
 *
 * @return 1 if the embedded pubkey decodes to 32 zero bytes;
 *         0 otherwise (real key, or unparseable macro — both treated
 *         as "not placeholder" so the verifier runs and fails
 *         loudly rather than silently skipping).
 */
int hl_platform_pubkey_is_placeholder(void);

/**
 * @brief Extract the SHA-256 hex for a specific arch from the manifest.
 *
 * Thin wrapper over `hl_release_io_find_checksum()` — the manifest
 * format is identical. Lookup is O(n) over manifest lines, but n is
 * the arch count (≤ 16) so this is cheap.
 *
 * @param manifest     Manifest bytes.
 * @param manifest_len Length of @p manifest.
 * @param arch         Arch name to look up (e.g. `"darwin-arm64"`).
 * @param hex_out      Filled with 64 hex chars + NUL on success.
 * @return 0 on match, -1 if @p arch is not in the manifest.
 */
int hl_platform_sig_extract_for_arch(const char *manifest, size_t manifest_len,
                                     const char *arch, char hex_out[65]);

/**
 * @brief Verify a composed-attestation set against one signing domain
 *        (issue #114, docs/composed_feature_signing.md).
 *
 * A composed app's `package.sig.gethull.composed.<domain>` carries a signed
 * manifest plus the list of `{name, sha256}` archives that were composed from
 * that domain. This attests all of them at once:
 *   1. `sig_hex` is a valid signature over `manifest` under `pubkey`, AND
 *   2. every `assets[i]`'s hash appears in `manifest` under `assets[i].arch`
 *      (the name column: a bare arch for the platform lib, an asset name for a
 *      composed archive).
 *
 * @param manifest     Signed manifest bytes (sha256sum-style lines).
 * @param manifest_len Length of @p manifest.
 * @param sig_hex      128 hex chars (+ optional trailing whitespace).
 * @param sig_hex_len  Length of @p sig_hex.
 * @param pubkey       32-byte Ed25519 public key for this domain (platform key
 *                     for embedded archives, release key for installed ones);
 *                     NULL uses the embedded `HL_PLATFORM_PUBKEY_HEX`.
 * @param assets       `{name, sha256_hex}` entries to attest (reuses
 *                     `HlPlatformArchHash`: `.arch` is the name, `.hash_hex` the
 *                     expected 64-char lowercase hash). May be NULL iff n == 0.
 * @param n_assets     Number of entries. 0 (with a valid signature) returns 0.
 * @return 0 iff the signature is valid AND every asset is present and matches;
 *         -1 on any signature failure, missing asset, or hash mismatch.
 */
int hl_platform_sig_verify_composed(const char *manifest, size_t manifest_len,
                                    const char *sig_hex, size_t sig_hex_len,
                                    const uint8_t pubkey[32],
                                    const HlPlatformArchHash *assets,
                                    size_t n_assets);

#endif /* HL_PLATFORM_SIG_H */
