/*
 * embedded_platform_sig.c — Accessor for the embedded signed
 * platform manifest blob.
 *
 * When `HL_EMBED_PLATFORM_SIG` is defined at build time (CI sets it
 * after sign-platform-manifest emits `build/embedded_platform_sig.h`),
 * the data is statically linked into the hull binary. When undefined
 * (local dev), the accessor returns -1 and the rest of the codebase
 * short-circuits the platform-sig check.
 *
 * Mirrors the cacert.c pattern exactly — generated header from xxd,
 * gated by a compile-time macro, expose via a stable accessor.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/embedded_platform_sig.h"

#ifdef HL_EMBED_PLATFORM_SIG
#include "embedded_platform_sig.h"
/* xxd emits the data + length symbols. CI's sign-platform-manifest
 * generates `build/embedded_platform_sig.h` containing:
 *   unsigned char hl_embedded_platform_sig_manifest[];
 *   unsigned int  hl_embedded_platform_sig_manifest_len;
 *   unsigned char hl_embedded_platform_sig_signature[];
 *   unsigned int  hl_embedded_platform_sig_signature_len;
 */
#endif

int hl_embedded_platform_sig(const unsigned char **manifest, size_t *manifest_len,
                             const unsigned char **signature, size_t *sig_len)
{
#ifdef HL_EMBED_PLATFORM_SIG
    if (manifest)     *manifest     = hl_embedded_platform_sig_manifest;
    if (manifest_len) *manifest_len = hl_embedded_platform_sig_manifest_len;
    if (signature)    *signature    = hl_embedded_platform_sig_signature;
    if (sig_len)      *sig_len      = hl_embedded_platform_sig_signature_len;
    return 0;
#else
    if (manifest)     *manifest     = NULL;
    if (manifest_len) *manifest_len = 0;
    if (signature)    *signature    = NULL;
    if (sig_len)      *sig_len      = 0;
    return -1;
#endif
}
