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
#include <keel/clock.h>   /* kl_monotonic_ms: one absolute handshake deadline */
#include <keel/tls.h>
#include <keel_tls_mbedtls.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

struct HlTlsClient {
    KlTlsCtx *ctx;        /* NULL when the ctx is caller-owned (cfg path) */
    KlTls    *tls;
};

/* The KlTls / KlTlsCtx capture the KlAllocator BY POINTER and dereference it at
 * destroy time - long after the handshake function returns - so the allocator
 * must outlive them. kl_allocator_default() is a stateless process-wide default
 * (static stdlib wrappers, NULL ctx); hold ONE copy in static storage and hand
 * its address to every KlTls/KlTlsCtx, instead of a stack local that dangles the
 * moment the handshake returns. A dangling stack allocator only bit LONG-LIVED
 * TLS connections (e.g. a pooled DB connection) whose handshake frame is gone by
 * teardown - it surfaced as a SIGSEGV in tls_destroy's kl_free(t->alloc, ...) on
 * a graceful `-d <postgres|mysql>://...?sslmode!=disable` process exit. SMTP
 * frees the KlTls inside the same call stack, so its local never dangled. */
static KlAllocator    s_default_alloc;
static pthread_once_t s_default_alloc_once = PTHREAD_ONCE_INIT;
static void init_default_alloc(void) { s_default_alloc = kl_allocator_default(); }
static KlAllocator *default_alloc(void)
{
    pthread_once(&s_default_alloc_once, init_default_alloc);
    return &s_default_alloc;
}

/* The poll-based handshake bound below only works if tls->handshake yields
 * WANT_READ/WANT_WRITE instead of blocking in recv/send. The caller's fd is
 * blocking (the transport set_blocking()s the winning descriptor for the
 * post-handshake byte I/O), so a peer that never completes the handshake (a
 * plaintext server answering a rediss:// ClientHello with nothing, or a hung TLS
 * peer) would otherwise block here forever, defeating the timeout. So the fd is
 * made non-blocking for the handshake and the caller's mode restored after.
 *
 * These two helpers manage that transition and FAIL CLOSED (they are separately
 * unit-tested, incl. the failure paths). If the mode cannot be read or cleared,
 * proceeding would risk the original infinite block; if it cannot be restored, a
 * successfully-handshaked descriptor would be left non-blocking for the wire
 * client's blocking I/O. Either way the handshake must fail. */

#ifdef HL_TLS_CLIENT_TEST_HOOKS
/* Test-only fcntl seam (compiled ONLY under -DHL_TLS_CLIENT_TEST_HOOKS, ABSENT
 * from the production object; nm-verified). When set, replaces the fcntl the
 * fd-mode helpers use, so a test can force F_GETFL, the enable-nonblocking
 * F_SETFL, or the restore F_SETFL to fail and drive every fail-closed path. */
int (*tls_test_fcntl)(int fd, int cmd, int arg);
static int tls_fcntl(int fd, int cmd, int arg)
{
    if (tls_test_fcntl)
        return tls_test_fcntl(fd, cmd, arg);
    return fcntl(fd, cmd, arg);
}
#else
static int tls_fcntl(int fd, int cmd, int arg) { return fcntl(fd, cmd, arg); }
#endif

/* Save the fd's flags into *saved and enable O_NONBLOCK. 0 on success; -1 if the
 * flags cannot be read OR O_NONBLOCK cannot be set (fail closed). */
static int tls_fd_set_nonblocking(int fd, int *saved)
{
    int flags = tls_fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;                                  /* mode unknown -> fail closed */
    *saved = flags;
    if ((flags & O_NONBLOCK) == 0 && tls_fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;                                  /* cannot enable -> fail closed */
    return 0;
}

/* Restore the fd's saved flags. 0 on success; -1 if they cannot be restored (the
 * descriptor would be left non-blocking). */
static int tls_fd_restore(int fd, int saved)
{
    if (tls_fcntl(fd, F_SETFL, saved) < 0)
        return -1;
    return 0;
}

/* Drive tls->handshake to completion, bounded by @p timeout_ms (<= 0 = wait
 * indefinitely) against ONE monotonic absolute deadline. 0 on success, -1 on
 * error / timeout / an fd-mode setup or restore failure. */
static int tls_handshake_loop(KlTls *tls, int fd, int timeout_ms)
{
    int saved;
    if (tls_fd_set_nonblocking(fd, &saved) != 0)
        return -1;                                  /* blocker 1: setup fail closed */

    const int step = 100;   /* max single poll wait (ms); the deadline is the bound */
    uint64_t deadline = (timeout_ms > 0) ? kl_monotonic_ms() + (uint64_t)timeout_ms : 0;
    int rc = -1;
    for (;;) {
        /* Check the absolute deadline BEFORE every handshake step: once it has
         * passed no further step is attempted, so a would-be-successful post-expiry
         * result is never accepted, and expiry takes precedence when readiness and
         * expiry coincide (the prior loop checked only after the step). */
        if (deadline && kl_monotonic_ms() >= deadline) { rc = -1; break; }

        KlTlsResult r = tls->handshake(tls, fd);
        if (r == KL_TLS_OK)    { rc = 0;  break; }
        if (r == KL_TLS_ERROR) { rc = -1; break; }

        int wait = step;
        if (deadline) {
            uint64_t now = kl_monotonic_ms();
            uint64_t rem = (now < deadline) ? deadline - now : 0;   /* 0 -> poll returns at once */
            if (rem < (uint64_t)step) wait = (int)rem;              /* min(remaining, step) */
        }
        short events = (r == KL_TLS_WANT_READ) ? POLLIN : POLLOUT;
        struct pollfd pfd = { .fd = fd, .events = events, .revents = 0 };
        int pr = poll(&pfd, 1, wait);
        if (pr < 0) {
            if (errno == EINTR) continue;               /* retry; the clock decides */
            rc = -1; break;
        }
        /* pr == 0 (poll timed out) or pr > 0 (ready): loop; the top-of-loop
         * deadline check is the sole timeout authority. */
    }

    if (tls_fd_restore(fd, saved) != 0)
        rc = -1;   /* blocker 1: leaving the fd non-blocking must fail the handshake */
    return rc;
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
    KlAllocator *alloc = default_alloc();   /* persistent; outlives the KlTls/ctx */

    /* verify: trust anchor = embedded CA bundle, cert chain + hostname
     * checked. Otherwise: no CA, server certificate accepted as-is. */
    KlTlsCtx *ctx = NULL;
    if (verify) {
        const unsigned char *cab = NULL;
        size_t cab_len = 0;
        if (hl_embedded_ca_bundle(&cab, &cab_len) == 0)
            ctx = kl_tls_mbedtls_client_ctx_create_from_buf(cab, cab_len, alloc);
    } else {
        ctx = kl_tls_mbedtls_client_ctx_create(NULL, alloc);
    }
    if (!ctx)
        return NULL;

    KlTls *tls = kl_tls_mbedtls_create(ctx, alloc);
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

    KlTls *tls = cfg->factory(cfg->ctx, default_alloc());
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
