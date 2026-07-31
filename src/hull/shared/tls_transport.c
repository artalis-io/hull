/*
 * tls_transport.c — strong mbedTLS transport override (Keel TLS ctx setup).
 *
 * The STRONG override of the weak hl_tls_* accessors (tls_transport_stub.c). Wraps
 * Keel's kl_tls_mbedtls_* so serve.c / serve_cli.c never reference those symbols
 * directly -- this TU is the sole in-Hull consumer of Keel's tls_mbedtls.o for
 * server + client context setup. Compiled only when the TLS stack is linked
 * (HL_LINK_TLS, the Makefile gate); a later phase (docs/tls_feature.md a2) moves
 * this TU into libhull_feature-tls.a so a TLS-less base drops Keel's tls_mbedtls.o
 * and falls back to the weak NULL stubs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/tls_transport.h"

#include <keel/tls_mbedtls.h>   /* kl_tls_mbedtls_*, KL_MTLS_NONE */

KlTlsCtx *hl_tls_server_ctx_create(const char *cert_path, const char *key_path,
                                   KlAllocator *alloc)
{
    return kl_tls_mbedtls_ctx_create(cert_path, key_path, NULL,
                                     KL_MTLS_NONE, alloc);
}

KlTlsCtx *hl_tls_client_ctx_create(const char *ca_path, KlAllocator *alloc)
{
    return kl_tls_mbedtls_client_ctx_create(ca_path, alloc);
}

KlTlsCtx *hl_tls_client_ctx_create_from_buf(const unsigned char *ca_buf,
                                            size_t ca_len, KlAllocator *alloc)
{
    return kl_tls_mbedtls_client_ctx_create_from_buf(ca_buf, ca_len, alloc);
}

void hl_tls_ctx_destroy(KlTlsCtx *ctx)
{
    if (ctx) kl_tls_mbedtls_ctx_destroy(ctx);
}

void hl_tls_config_wire(KlTlsConfig *cfg, KlTlsCtx *ctx)
{
    if (!cfg || !ctx) return;
    cfg->ctx         = ctx;
    cfg->factory     = (KlTlsFactory)kl_tls_mbedtls_create;
    cfg->ctx_destroy = (void (*)(KlTlsCtx *))kl_tls_mbedtls_ctx_destroy;
}
