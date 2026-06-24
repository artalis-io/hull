/**
 * @file release_io.h
 * @brief Shared HTTPS + manifest + atomic-install plumbing for
 *        `hull update` and `hull tools install`.
 *
 * These helpers were originally inline in `commands/update.c`. They're
 * extracted so both the self-update path and the side-loaded tools
 * path can verify against the same Ed25519-signed `hull.sha256`
 * manifest using the same trust chain — no duplication of the keel
 * HTTPS + mbedTLS SHA-256 + manifest-parsing code.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RELEASE_IO_H
#define HL_RELEASE_IO_H

#include <stddef.h>

/* Keel allocator + TLS types are typedefs over anonymous structs, so
 * we can't forward-declare them — pull the headers in directly. Both
 * consumers (`commands/update.c`, `commands/tools.c`) already need
 * them, so there's no extra cost. */
#include <keel/allocator.h>
#include <keel/tls.h>

/**
 * Return the OS/arch platform identifier this binary was compiled for.
 * Drives asset-name selection (`hull-<platform>`,
 * `hull-wamrc-<platform>`, etc.). One of:
 *   "linux-x86_64", "linux-aarch64", "darwin-arm64", "cosmo".
 * Returns "cosmo" for platforms without a native build.
 */
const char *hl_release_io_platform(void);

/**
 * Resolve the running binary's filesystem path (Linux: /proc/self/exe,
 * macOS: _NSGetExecutablePath, cosmo: returns -1 by design — caller is
 * expected to fall back to argv[0]).
 *
 * @returns 0 on success, -1 on failure / unsupported platform.
 */
int hl_release_io_self_path(char *out, size_t out_sz);

/**
 * Open a TLS client context backed by the embedded Mozilla CA bundle.
 * Falls back to system bundle paths if the embedded one isn't
 * present. Caller owns the returned pointer; free with
 * `kl_tls_mbedtls_ctx_destroy()`.
 *
 * @returns NULL if no CA store is available.
 */
KlTlsCtx *hl_release_io_open_tls(KlAllocator *alloc);

/**
 * HTTPS GET. Allocates the response body via @p alloc. On success,
 * @p out_body is malloc'd and the caller must `kl_free()` it.
 *
 * @param user_agent  Sent as the User-Agent header; pass a short
 *                    identifier like "hull-update" or "hull-tools".
 * @returns 0 on HTTP 2xx, -1 on transport error or non-2xx response.
 *          A non-zero return prints an error to stderr.
 */
int hl_release_io_get(const char *url,
                      char **out_body, size_t *out_len,
                      KlAllocator *alloc,
                      KlTlsCtx *tls,
                      const char *user_agent);

/**
 * Download `hull.sha256` and (when a release pubkey is embedded) its
 * `.sig`, verify the Ed25519 signature over the manifest, and return the
 * verified manifest buffer. The caller `kl_free()`s *out_manifest.
 *
 * The single trust-chain entry shared by `hull tools install` and
 * `hull platform install`. On a placeholder (all-zero) embedded pubkey the
 * signature step is skipped with a warning (SHA-256 only), matching
 * `hull update`. Refuses to proceed if a configured pubkey can't verify.
 *
 * @returns 0 on success, -1 on any download/verification failure (a message
 *          prefixed with @p ua is printed to stderr).
 */
int hl_release_io_fetch_verified_manifest(const char *repo, const char *tag,
                                          KlAllocator *alloc, KlTlsCtx *tls,
                                          const char *ua,
                                          char **out_manifest,
                                          size_t *out_manifest_len);

/**
 * Extract a flat `"key":"value"` entry from a JSON blob. Deliberately
 * tiny — sufficient for GitHub release metadata fields like
 * `tag_name`. Not a general parser.
 *
 * @returns 0 on success, -1 on missing key or buffer overflow.
 */
int hl_release_io_json_str(const char *json, const char *key,
                           char *out, size_t out_sz);

/**
 * Hex-encode the SHA-256 of @p data into @p hex (must be at least 65
 * bytes — 64 lowercase hex chars + NUL).
 *
 * @returns 0 on success, -1 on mbedTLS error.
 */
int hl_release_io_sha256_hex(const unsigned char *data, size_t len,
                             char hex[65]);

/**
 * Locate a `<sha256-hex>  <asset>\n` line in a `hull.sha256` manifest
 * and copy the 64-char hex into @p hex_out.
 *
 * @returns 0 on match, -1 if @p asset is not in the manifest.
 */
int hl_release_io_find_checksum(const char *manifest, size_t mlen,
                                const char *asset, char hex_out[65]);

/**
 * Atomically write @p data to @p target_path: opens
 * `<target_path>.new` with @p mode, writes, `fsync()`s, then
 * `rename(2)`s over @p target_path. Existing file at the target is
 * replaced atomically.
 *
 * @returns 0 on success, -1 on any I/O failure (with a leftover
 *          `.new` file cleaned up).
 */
int hl_release_io_atomic_write(const char *target_path,
                               const void *data, size_t len,
                               int mode);

#endif /* HL_RELEASE_IO_H */
