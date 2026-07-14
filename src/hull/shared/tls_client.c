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
    KlTlsCtx *ctx;        /* NULL when the ctx is caller-owned (cfg path) */
    KlTls    *tls;
};

/* Drive tls->handshake to completion with a poll-based blocking wait, bounded
 * by @p timeout_ms (<= 0 = wait indefinitely). 0 on success, -1 on error/timeout. */
static int tls_handshake_loop(KlTls *tls, int fd, int timeout_ms)
{
    int elapsed = 0;
    const int step = 100;
    for (;;) {
        KlTlsResult r = tls->handshake(tls, fd);
        if (r == KL_TLS_OK)
            return 0;
        if (r == KL_TLS_ERROR)
            return -1;

        short events = (r == KL_TLS_WANT_READ) ? POLLIN : POLLOUT;
        struct pollfd pfd = { .fd = fd, .events = events, .revents = 0 };
        if (poll(&pfd, 1, step) < 0)
            return -1;
        elapsed += step;
        if (timeout_ms > 0 && elapsed >= timeout_ms)
            return -1;
    }
}

/* Wrap an already-handshaked session. @p ctx is NULL when caller-owned. */
static HlTlsClient *tls_client_wrap(KlTlsCtx *ctx, KlTls *tls)
{
    HlTlsClient *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->ctx = ctx;
    c->tls = tls;
    return c;
}

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

    HlTlsClient *c = NULL;
    if (tls_handshake_loop(tls, fd, timeout_ms) != 0 ||
        (c = tls_client_wrap(ctx, tls)) == NULL) {
        tls->destroy(tls);
        kl_tls_mbedtls_ctx_destroy(ctx);
        return NULL;
    }
    return c;
}

HlTlsClient *hl_tls_client_handshake_cfg(int fd, const char *host,
                                         void *tls_cfg, int timeout_ms)
{
    KlTlsConfig *cfg = (KlTlsConfig *)tls_cfg;
    if (!cfg || !cfg->factory)
        return NULL;

    KlAllocator alloc = kl_allocator_default();
    KlTls *tls = cfg->factory(cfg->ctx, &alloc);
    if (!tls)
        return NULL;
    if (host && host[0] && tls->set_hostname)
        tls->set_hostname(tls, host);

    /* ctx is user-owned (cfg->ctx): wrap with ctx=NULL so free leaves it. */
    HlTlsClient *c = NULL;
    if (tls_handshake_loop(tls, fd, timeout_ms) != 0 ||
        (c = tls_client_wrap(NULL, tls)) == NULL) {
        tls->destroy(tls);
        return NULL;
    }
    return c;
}

ssize_t hl_tls_client_read(int fd, HlTlsClient *c, void *buf, size_t len)
{
    return c->tls->read(c->tls, fd, buf, len);
}

ssize_t hl_tls_client_write(int fd, HlTlsClient *c, const void *buf, size_t len)
{
    return c->tls->write(c->tls, fd, buf, len);
}

void hl_tls_client_shutdown(int fd, HlTlsClient *c)
{
    if (c && c->tls && c->tls->shutdown)
        c->tls->shutdown(c->tls, fd);
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
