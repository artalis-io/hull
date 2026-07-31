/**
 * @file tls_transport.h
 * @brief Composed-feature seam for Keel's mbedTLS transport (TLS server/client ctx).
 *
 * Part of "TLS as a composable feature" (docs/tls_feature.md), the transport
 * half. serve.c / serve_cli.c set up KlServer + http.fetch + SMTP TLS by calling
 * Keel's `kl_tls_mbedtls_*` directly, which pulls Keel's `tls_mbedtls.o` (and
 * mbedTLS) into the base. Routing those calls through these weak accessors lets a
 * future TLS-less base emit no `kl_tls_*` reference (serving HTTP-only) while a
 * composed TLS feature provides the strong override that wires Keel's TLS.
 *
 * Kept OUT of tls_feature.h (the crypto seam) so cap/crypto.c stays free of Keel
 * headers. Phase a1 is additive + dormant: the strong override
 * (src/hull/shared/tls_transport.c, HL_LINK_TLS) still lives in the base, so TLS
 * behaves exactly as before; the weak defaults (tls_transport_stub.c) satisfy the
 * link when it is absent. Phase a2 moves the strong override into
 * libhull_feature-tls.a.
 *
 * Stability: Tier 3 (internal seam).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_TLS_TRANSPORT_H
#define HL_TLS_TRANSPORT_H

#include <stddef.h>
#include <keel/tls.h>   /* KlTlsCtx, KlTlsConfig, KlTlsFactory, KlAllocator */

/**
 * Create a server-side TLS context from a cert + key (no client auth), or NULL
 * when TLS is not linked/composed (server then serves plaintext HTTP only).
 */
KlTlsCtx *hl_tls_server_ctx_create(const char *cert_path, const char *key_path,
                                   KlAllocator *alloc);

/**
 * Create a client-side TLS context from a CA bundle path (NULL = no verify), or
 * NULL when TLS is absent (outbound HTTPS / SMTP-TLS then unavailable).
 */
KlTlsCtx *hl_tls_client_ctx_create(const char *ca_path, KlAllocator *alloc);

/** Same, from an in-memory CA bundle (the embedded Mozilla bundle). */
KlTlsCtx *hl_tls_client_ctx_create_from_buf(const unsigned char *ca_buf,
                                            size_t ca_len, KlAllocator *alloc);

/** Destroy a context from any of the creators above. NULL-safe. */
void hl_tls_ctx_destroy(KlTlsCtx *ctx);

/**
 * Wire @p cfg's {ctx, factory, ctx_destroy} for a created @p ctx so Keel can spin
 * up per-connection sessions. No-op when @p ctx is NULL / TLS is absent. This is
 * what keeps the concrete `kl_tls_mbedtls_create` / `_ctx_destroy` function
 * pointers out of serve.c.
 */
void hl_tls_config_wire(KlTlsConfig *cfg, KlTlsCtx *ctx);

#endif /* HL_TLS_TRANSPORT_H */
