/**
 * @file tls_feature.h
 * @brief Composed-feature seam for the mbedTLS-backed crypto backends.
 *
 * Part of the "TLS as a composable feature" work (docs/tls_feature.md). The
 * mbedTLS-backed crypto backends are selected at RUNTIME through weak accessor
 * hooks instead of a compile-time `#ifdef`, so a future TLS-less base can
 * default to the portable/fail-closed backends while a composed TLS feature
 * swaps in the mbedTLS backends via a strong override (mirrors the gpu/tui
 * feature hooks).
 *
 * Per-backend accessors (rather than one combined struct) on purpose: the base
 * links crypto.o independently of the asym TU (some test link sets pull crypto.o
 * + the HMAC TU without the asym TU), so a hook whose default referenced the
 * asym symbol would break those links. Each accessor references only its own
 * backend.
 *
 * Phase 1 (this seam) is additive + dormant: the weak default preserves the
 * historical compile-time selection byte-for-byte. Later phases move mbedTLS out
 * of the base and provide the strong override from libhull_feature-tls.a. The
 * asym accessor lands with the asym slice of Phase 1.
 *
 * Stability: Tier 3 (internal seam).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_TLS_FEATURE_H
#define HL_TLS_FEATURE_H

#include "hull/cap/crypto.h"   /* HlCryptoHmacBackend */

/**
 * Return the active HMAC backend.
 *
 * WEAK in the base (cap/crypto.c): the default returns the mbedTLS HMAC backend
 * when it is compiled in (the historical `HL_ENABLE_HTTP` selection) and the
 * portable backend otherwise. A composed TLS feature provides a STRONG override
 * returning the mbedTLS HMAC backend unconditionally. Portable and mbedTLS HMAC
 * produce identical output, so the choice is transparent to callers.
 */
const HlCryptoHmacBackend *hl_crypto_hmac_active_backend(void);

#endif /* HL_TLS_FEATURE_H */
