/*
 * agent/probe.c - bounded TCP-connect liveness probe for `hull agent status`.
 *
 * Drives a single-address (127.0.0.1:<port>) connect through Keel's socket
 * provider + a private KlEventCtx watcher: nonblocking connect, provider
 * io_status classification (documented hosted-errno fallback), and a
 * writability wait bounded by ONE monotonic deadline. No raw Berkeley
 * socket call and no hand-rolled poll()/errno event axis. Returns 1 iff a
 * connection establishes within timeout_ms, 0 otherwise (refused, error, or
 * timeout). The probe never keeps the connection: the descriptor is closed
 * exactly once on every path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>   /* AF_INET / SOCK_STREAM ints for the provider socket() op */

#include <keel/allocator.h>
#include <keel/clock.h>       /* kl_monotonic_ms: one monotonic connect deadline */
#include <keel/event.h>       /* KL_EVENT_WRITE */
#include <keel/event_ctx.h>   /* private KlEventCtx + watcher API */
#include <keel/handle.h>      /* KlSocketHandle, kl_handle_valid */
#include <keel/sockaddr.h>    /* kl_sockaddr_from_ipv4 */
#include <keel/socket.h>      /* kl_socket_provider_posix + ops table */

#ifdef HL_AGENT_PROBE_TEST_HOOKS
/* Test-only seam (compiled ONLY under -DHL_AGENT_PROBE_TEST_HOOKS, ABSENT from
 * the production object). When set, REPLACES the default POSIX provider so a
 * white-box test can script immediate-connect / refusal / pending outcomes over
 * real socketpair fds the private event loop can watch. */
const KlSocketProvider *hl_agent_probe_test_provider;
#endif

static const KlSocketProvider *probe_provider(void)
{
#ifdef HL_AGENT_PROBE_TEST_HOOKS
    if (hl_agent_probe_test_provider)
        return hl_agent_probe_test_provider;
#endif
    return kl_socket_provider_posix();
}

/* Classify the last provider op: prefer the provider's io_status op, else the
 * documented hosted-errno fallback (mirrors cap/db_transport.c). */
static KlIoStatus probe_io_status(const KlSocketProvider *sp)
{
    if (sp->ops->io_status)
        return sp->ops->io_status(sp->context);
    switch (errno) {
        case EINTR:       return KL_IO_INTERRUPTED;
        case EINPROGRESS: return KL_IO_PENDING;
        case 0:           return KL_IO_OK;
        default:          return KL_IO_FATAL;
    }
}

/* Writability-wait state for a pending connect. */
typedef struct {
    const KlSocketProvider *sp;
    KlSocketHandle          fd;
    int                     done;     /* 1 once the connect resolved */
    int                     running;  /* 1 on established, 0 on error */
} ProbeWait;

static void probe_on_writable(KlSocketHandle fd, KlEventMask ready, void *user)
{
    (void)fd;
    (void)ready;
    ProbeWait *w = user;
    int soerr = 0;
    if (w->sp->ops->get_so_error(w->sp->context, w->fd, &soerr) == 0 && soerr == 0)
        w->running = 1;
    w->done = 1;
}

int hl_agent_tcp_probe(int port, int timeout_ms)
{
    const KlSocketProvider *sp = probe_provider();
    if (!sp || !sp->ops || !sp->ops->socket || !sp->ops->connect ||
        !sp->ops->close || !sp->ops->set_nonblocking || !sp->ops->get_so_error)
        return 0;
    if (!kl_socket_provider_has_cap(sp, KL_SOCK_CAP_NATIVE_FD))
        return 0;
    const KlSocketOps *ops = sp->ops;
    void *sctx = sp->context;

    KlSockAddr addr;
    const uint8_t loopback[4] = { 127, 0, 0, 1 };
    if (kl_sockaddr_from_ipv4(&addr, loopback, (uint16_t)port) != 0)
        return 0;

    KlSocketHandle fd = ops->socket(sctx, AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd))
        return 0;

    int running = 0;
    if (ops->set_nonblocking(sctx, fd) == 0) {
        errno = 0;
        int cr = ops->connect(sctx, fd, &addr);
        if (cr == 0) {
            running = 1;                    /* connected at once (common on loopback) */
        } else {
            KlIoStatus st = probe_io_status(sp);
            if (st == KL_IO_OK) {
                running = 1;
            } else if (st == KL_IO_PENDING || st == KL_IO_INTERRUPTED) {
                /* Wait for writability on a private event context, bounded by a
                 * single monotonic deadline - no raw poll()/errno spin. */
                KlEventCtx ev;
                KlAllocator alloc = kl_allocator_default();
                if (kl_event_ctx_init(&ev, &alloc) == 0) {
                    ProbeWait w = { .sp = sp, .fd = fd, .done = 0, .running = 0 };
                    if (kl_watcher_add(&ev, fd, KL_EVENT_WRITE,
                                       probe_on_writable, &w) == 0) {
                        uint64_t deadline = kl_monotonic_ms() +
                            (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
                        while (!w.done) {
                            uint64_t now = kl_monotonic_ms();
                            if (timeout_ms > 0 && now >= deadline)
                                break;               /* deadline is the sole timeout */
                            int wait = (timeout_ms > 0) ? (int)(deadline - now) : -1;
                            if (kl_event_ctx_run(&ev, 4, wait) < 0)
                                break;
                        }
                        kl_watcher_del(&ev, fd);
                        running = w.running;
                    }
                    kl_event_ctx_free(&ev);
                }
            }
            /* any other status (closed / reset / error): hard failure -> 0 */
        }
    }

    ops->close(sctx, fd);
    return running;
}
