/*
 * shared/tls_client.c: blocking TLS client helper over Keel's KlTls
 *
 * See tls_client.h. Mirrors the handshake loop cap/smtp.c hand-rolls and the
 * CA-bundle context release_io.c builds, so PostgreSQL (and, later, a
 * retrofitted SMTP) share one tested copy instead of N.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/tls_client.h"
#include "hull/cacert.h"

#include <keel/allocator.h>
#include <keel/tls.h>
#include <keel/tls_mbedtls.h>

#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

struct HlTlsClient {
    KlTlsCtx *ctx;
    KlTls    *tls;
};

HlTlsClient *hl_tls_client_handshake(int fd, const char *host,
                                     int verify, int timeout_ms)
{
    KlAllocator alloc = kl_allocator_default();

    /* verify: trust anchor = embedded CA bundle, cert chain + hostname
     * checked. Otherwise: no CA, server certificate accepted as-is. */
    KlTlsCtx *ctx = NULL;
    if (verify) {
        const unsigned char *cab = NULL;
        size_t cab_len = 0;
        if (hl_embedded_ca_bundle(&cab, &cab_len) == 0)
            ctx = kl_tls_mbedtls_client_ctx_create_from_buf(cab, cab_len, &alloc);
    } else {
        ctx = kl_tls_mbedtls_client_ctx_create(NULL, &alloc);
    }
    if (!ctx)
        return NULL;

    KlTls *tls = kl_tls_mbedtls_create(ctx, &alloc);
    if (!tls) {
        kl_tls_mbedtls_ctx_destroy(ctx);
        return NULL;
    }
    if (host && host[0] && tls->set_hostname)
        tls->set_hostname(tls, host);

    int elapsed = 0;
    const int step = 100;
    for (;;) {
        KlTlsResult r = tls->handshake(tls, fd);
        if (r == KL_TLS_OK)
            break;
        if (r == KL_TLS_ERROR)
            goto fail;

        short events = (r == KL_TLS_WANT_READ) ? POLLIN : POLLOUT;
        struct pollfd pfd = { .fd = fd, .events = events, .revents = 0 };
        if (poll(&pfd, 1, step) < 0)
            goto fail;
        elapsed += step;
        if (timeout_ms > 0 && elapsed >= timeout_ms)
            goto fail;
    }

    HlTlsClient *c = calloc(1, sizeof *c);
    if (!c)
        goto fail;
    c->ctx = ctx;
    c->tls = tls;
    return c;

fail:
    tls->destroy(tls);
    kl_tls_mbedtls_ctx_destroy(ctx);
    return NULL;
}

ssize_t hl_tls_client_read(int fd, HlTlsClient *c, void *buf, size_t len)
{
    return c->tls->read(c->tls, fd, buf, len);
}

ssize_t hl_tls_client_write(int fd, HlTlsClient *c, const void *buf, size_t len)
{
    return c->tls->write(c->tls, fd, buf, len);
}

void hl_tls_client_free(HlTlsClient *c)
{
    if (!c)
        return;
    if (c->tls)
        c->tls->destroy(c->tls);
    if (c->ctx)
        kl_tls_mbedtls_ctx_destroy(c->ctx);
    free(c);
}
