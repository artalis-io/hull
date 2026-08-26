/**
 * @file embedded_platform_sig.h
 * @brief Accessor for the signed platform manifest blob embedded in
 *        the hull binary at release-build time.
 *
 * The blob is the canonical platform manifest (per-arch SHA-256 of
 * `libhull_platform.a` artifacts, see `platform_sig.h` for the
 * format) plus its Ed25519 signature under the `HULL_PLATFORM_KEY`
 * secret. CI's `sign-platform-manifest` job generates the contents
 * (`build/embedded_platform_sig.h`); `src/hull/embedded_platform_sig.c`
 * compiles it into the hull binary; this accessor exposes it.
 *
 * Consumers (v0.1.3 batch):
 *   - `hull build` (C3) - passes the blob into `package.sig.platform`
 *     so apps inherit the manifest + sig at build time.
 *   - `hull verify` / `--verify-sig` (C4) - validates the blob's sig
 *     against `HL_PLATFORM_PUBKEY_HEX` at startup.
 *
 * When `HL_EMBED_PLATFORM_SIG` is undefined at compile time (local
 * dev builds, anything that hasn't run the CI signing step), the
 * accessor returns -1 and signals an empty blob. The same opt-out
 * path that `--no-verify-platform` triggers downstream applies.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_EMBEDDED_PLATFORM_SIG_H
#define HL_EMBEDDED_PLATFORM_SIG_H

#include <stddef.h>

/**
 * @brief Read the embedded signed platform manifest.
 *
 * @param[out] manifest      Pointer to the manifest bytes (NOT
 *                           NUL-terminated). Lives in static storage;
 *                           never freed. Set to NULL if no embedded
 *                           blob is present.
 * @param[out] manifest_len  Length of @p manifest. 0 if absent.
 * @param[out] signature     Pointer to the Ed25519 signature hex
 *                           string bytes (128 hex chars + trailing
 *                           newline, NOT NUL-terminated). NULL if
 *                           absent.
 * @param[out] sig_len       Length of @p signature. 0 if absent.
 * @return  0 if a real signed blob is embedded;
 *         -1 if absent (placeholder build / local dev / opt-out).
 *
 * NULL pointer args are treated as "don't fill" - pass NULL for
 * outputs you don't care about. NULL for ALL outputs is treated as
 * a presence probe.
 */
int hl_embedded_platform_sig(const unsigned char **manifest, size_t *manifest_len,
                             const unsigned char **signature, size_t *sig_len);

#endif /* HL_EMBEDDED_PLATFORM_SIG_H */
