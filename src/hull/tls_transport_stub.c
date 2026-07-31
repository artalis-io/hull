/*
 * tls_transport_stub.c — weak TLS-transport defaults for a TLS-less base.
 *
 * Weak, fail-closed defaults for the hl_tls_* accessors serve.c / serve_cli.c
 * call (docs/tls_feature.md, transport seam). When the strong override
 * (src/hull/shared/tls_transport.c, or a composed libhull_feature-tls.a) is
 * linked, its defs win and TLS works; when it is NOT (a TLS-less base), these weak
 * no-ops satisfy the link and the server serves plaintext HTTP only / outbound
 * HTTPS is unavailable (fail closed).
 *
 * Mirrors http_weakstub.c / wasm_weakstub.c. Always compiled into the base.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/tls_transport.h"

__attribute__((weak))
KlTlsCtx *hl_tls_server_ctx_create(const char *cert_path, const char *key_path,
                                   KlAllocator *alloc)
{
    (void)cert_path; (void)key_path; (void)alloc;
    return NULL;   /* no TLS composed; server serves plaintext HTTP */
}

__attribute__((weak))
KlTlsCtx *hl_tls_client_ctx_create(const char *ca_path, KlAllocator *alloc)
{
    (void)ca_path; (void)alloc;
    return NULL;
}

__attribute__((weak))
KlTlsCtx *hl_tls_client_ctx_create_from_buf(const unsigned char *ca_buf,
                                            size_t ca_len, KlAllocator *alloc)
{
    (void)ca_buf; (void)ca_len; (void)alloc;
    return NULL;
}

__attribute__((weak))
void hl_tls_ctx_destroy(KlTlsCtx *ctx)
{
    (void)ctx;   /* no ctx exists without the transport */
}

__attribute__((weak))
void hl_tls_config_wire(KlTlsConfig *cfg, KlTlsCtx *ctx)
{
    (void)cfg; (void)ctx;   /* nothing to wire without the transport */
}
