/*
 * cap/net.c - outbound byte stream: connect, close, free.
 *
 * Read and write land in the next slice; this one carries the ownership
 * protocol, which is the part that bites. See include/hull/cap/net_stream.h for
 * the contract and docs/net_module_design.md section 6a for why this schedules
 * on the event loop rather than pinning a worker like Hull's other transports.
 *
 * Structure mirrors cap/smtp_transport.c deliberately: the same eleven
 * KlConnectOp adapter hooks in the same order, the same heap-allocated opaque
 * handle, the same detachment rule. Two proven implementations reached that
 * shape independently; a third that invented its own would be the odd one out
 * for no benefit.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/net_stream.h"
#include "hull/shared/async_backend.h"

#include <keel/connect_op.h>
#include <keel/connect_op_detail.h> /* opt-in layout: embed a KlConnectOp (storage only) */
#include <keel/sockaddr.h>
#include <keel/socket.h>
#include <keel/handle.h>
#include <keel/error.h>

#include <errno.h>
#include <netdb.h>        /* getaddrinfo: the SYSTEM resolver, see resolve_addrs */
#include <netinet/in.h>   /* sockaddr_in / sockaddr_in6, to read what it returns */
#include <pthread.h>
#include <stdio.h>        /* snprintf */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>   /* AF_UNSPEC / AF_INET / AF_INET6 only. The socket
                           * CALLS all go through the Keel provider; these are
                           * just the family constants getaddrinfo speaks. */

/* ── Socket provider ────────────────────────────────────────────────── */

static const KlSocketProvider *g_sp;
static pthread_once_t          g_sp_once = PTHREAD_ONCE_INIT;
static void init_sp(void) { g_sp = kl_socket_provider_posix(); }
static const KlSocketProvider *socket_provider(void)
{
    pthread_once(&g_sp_once, init_sp);
    return g_sp;
}

/* The provider is a vtable plus its own context; every op takes that context
 * as its first argument. Reaching past it to bare POSIX would work on Linux
 * and quietly diverge everywhere else, which is the whole reason it exists. */
static KlSocketHandle sp_socket(int d, int t, int p)
{ const KlSocketProvider *sp = socket_provider(); return sp->ops->socket(sp->context, d, t, p); }
static int sp_set_nonblocking(KlSocketHandle fd)
{ const KlSocketProvider *sp = socket_provider(); return sp->ops->set_nonblocking(sp->context, fd); }
static int sp_connect(KlSocketHandle fd, const KlSockAddr *a)
{ const KlSocketProvider *sp = socket_provider(); return sp->ops->connect(sp->context, fd, a); }
static int sp_get_so_error(KlSocketHandle fd, int *out)
{ const KlSocketProvider *sp = socket_provider(); return sp->ops->get_so_error(sp->context, fd, out); }
static int sp_close(KlSocketHandle fd)
{ const KlSocketProvider *sp = socket_provider(); return sp->ops->close(sp->context, fd); }

/* ── Limits ─────────────────────────────────────────────────────────── */

#define NET_MAX_ADDRS        8      /* addresses raced; bounds the fd set   */
#define NET_CONNECT_MS_DEF   30000
#define NET_ATTEMPT_DELAY_MS 250    /* Happy-Eyeballs spacing               */

/* ── Stream ─────────────────────────────────────────────────────────── */

struct HlNetStream {
    /* Scheduling. The backend is borrowed from the caller (the binding passes
     * the runtime's async_ctx) so this file needs no runtime knowledge. */
    HlAsyncBackendCtx *async;
    const HlAsyncBackend *be;

    /* Connect */
    KlConnectOp  connect_op;
    int          connect_started;   /* op is live; teardown must cancel it  */
    int          connect_done;
    int          connect_detached;  /* the ownership release signal         */
    int          result;            /* HL_NET_* once connect_done           */

    KlSockAddr   addrs[NET_MAX_ADDRS];
    int          naddrs;

    KlSocketHandle fd;
    KlSocketHandle attempt_fd[NET_MAX_ADDRS];

    /* Timers, each paired with its cancel per the KlConnectOp contract. */
    uint64_t     delay_timer;
    uint64_t     deadline_timer;
    int          connect_ms;

    /* Parking. The binding fills op.on_resume and suspends; we complete it. */
    HlAsyncOp    op;
    int          op_pending;

    /* Lifecycle */
    int          closing;
    int          freed;             /* free() called; drop when detached    */
    size_t       read_cap, write_cap;
};

/* Complete a pending park exactly once. Called from terminal transitions, so
 * an op that was never suspended (synchronous completion) is a no-op rather
 * than a double-resume. */
static void wake(HlNetStream *s)
{
    if (!s->op_pending) return;
    s->op_pending = 0;
    if (s->be && s->be->op_complete) s->be->op_complete(s->async, &s->op);
}

/* Drop every racing descriptor we still hold. Idempotent. */
static void retire_attempts(HlNetStream *s)
{
    for (int i = 0; i < NET_MAX_ADDRS; i++) {
        if (kl_handle_valid(s->attempt_fd[i])) {
            sp_close(s->attempt_fd[i]);
            s->attempt_fd[i] = KL_INVALID_SOCKET;
        }
    }
}

/* The only place a stream's storage is released.
 *
 * Refuses while the connect op has not confirmed detachment, because a live op
 * still references this allocation. The caller's free() therefore returns
 * promptly with the handle dead, and the actual release happens here from
 * co_on_detach. If detachment never comes, the allocation is intentionally
 * abandoned: leaking a bounded block beats freeing storage under a live op. */
static void maybe_release(HlNetStream *s)
{
    if (!s->freed) return;

    /* The OP's own state is the authority here, not our callback flag.
     * Cancelling an already-terminal op has nothing to detach, so co_on_detach
     * never fires, and a flag-only check refuses release forever: a leak.
     * LeakSanitizer caught exactly that through cancel_then_free_is_safe.
     * cap/smtp_transport.c asks the op the same way. */
    if (s->connect_started && !s->connect_detached &&
        !kl_connect_op_is_detached(&s->connect_op)) return;

    if (s->deadline_timer && s->be->timer_cancel)
        s->be->timer_cancel(s->async, s->deadline_timer);
    if (s->delay_timer && s->be->timer_cancel)
        s->be->timer_cancel(s->async, s->delay_timer);

    retire_attempts(s);
    if (kl_handle_valid(s->fd)) sp_close(s->fd);
    free(s);
}

/* ── Resolution ─────────────────────────────────────────────────────── */

/* Blocking getaddrinfo, inline before the op starts. This mirrors the SMTP
 * transport, where the same call is the documented non-interruptible
 * exception: it runs before anything is armed, so a cancel arriving during it
 * has nothing to tear down. Replacing it needs an async resolver in Keel, not
 * a workaround here.
 *
 * Keel does ship kl_dns_resolver_create, and it is deliberately NOT used: it
 * issues UDP DNS queries, so it would not resolve an mDNS ".local" name, which
 * is exactly the case hsctl leads with (spark-7468.local). getaddrinfo is the
 * system resolver and honours whatever the host is configured to do, mDNS
 * included. It is also the sandbox-compatible choice Hull already made for
 * SMTP. */
static int resolve_addrs(HlNetStream *s, const char *host, int port)
{
    char port_str[8];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return HL_NET_E_RESOLVE;

    int n = 0;
    for (struct addrinfo *ai = res; ai && n < NET_MAX_ADDRS; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            struct sockaddr_in *v4 = (struct sockaddr_in *)ai->ai_addr;
            uint8_t ip[4];
            memcpy(ip, &v4->sin_addr, 4);
            if (kl_sockaddr_from_ipv4(&s->addrs[n], ip, (uint16_t)port) == 0) n++;
        } else if (ai->ai_family == AF_INET6) {
            struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ai->ai_addr;
            uint8_t ip[16];
            memcpy(ip, &v6->sin6_addr, 16);
            if (kl_sockaddr_from_ipv6(&s->addrs[n], ip, (uint16_t)port, 0) == 0) n++;
        }
    }
    freeaddrinfo(res);

    s->naddrs = n;
    return n > 0 ? HL_NET_OK : HL_NET_E_RESOLVE;
}

/* ── KlConnectOp adapter hooks ──────────────────────────────────────── */

static int co_start_resolve(void *ctx)
{
    HlNetStream *s = ctx;
    /* Already resolved inline before start; just report the count. */
    if (s->naddrs < 1) {
        kl_connect_op_on_resolve_failed(&s->connect_op, (int)KL_ERR_DNS);
        return 0;
    }
    kl_connect_op_on_resolved(&s->connect_op, s->naddrs);
    return 0;
}

/* Watcher callback for an in-flight connect attempt.
 *
 * The fd arrives as an int because that is HlAsyncWatcherFn's signature, while
 * Keel handles are intptr_t (keel/handle.h) to leave room for a Win32 SOCKET.
 * Hull only ever reaches this on a build where descriptors are POSIX ints, so
 * the widening is safe, but it is written out rather than left implicit. */
static void attempt_ready(int fd, unsigned ready, void *user)
{
    (void)ready;
    HlNetStream *s = user;
    KlSocketHandle h = (KlSocketHandle)fd;

    int idx = -1;
    for (int i = 0; i < NET_MAX_ADDRS; i++)
        if (s->attempt_fd[i] == h) { idx = i; break; }
    if (idx < 0) return;

    /* Writable means the connect resolved one way or the other; SO_ERROR says
     * which. Treating writability alone as success is the classic bug here.
     * Asked through the provider rather than getsockopt directly: the provider
     * exists precisely so this works the same on every socket backend. */
    int err = 0;
    if (sp_get_so_error(h, &err) != 0) err = EIO;

    if (s->be->watcher_del) s->be->watcher_del(s->async, fd);

    if (err == 0) {
        s->attempt_fd[idx] = KL_INVALID_SOCKET;   /* op owns it now */
        kl_connect_op_on_attempt_connected(&s->connect_op, idx, h);
    } else {
        sp_close(h);
        s->attempt_fd[idx] = KL_INVALID_SOCKET;
        kl_connect_op_on_attempt_failed(&s->connect_op, idx, (int)KL_ERR_CONNECT);
    }
}

static int co_start_attempt(void *ctx, int idx, int *out_err)
{
    HlNetStream *s = ctx;
    if (idx < 0 || idx >= s->naddrs) { *out_err = (int)KL_ERR_CONNECT; return -1; }

    KlSocketHandle fd = sp_socket(s->addrs[idx].family == KL_AF_INET6 ? AF_INET6
                                                                     : AF_INET,
                                  SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) { *out_err = (int)KL_ERR_CONNECT; return -1; }
    if (sp_set_nonblocking(fd) < 0) {
        sp_close(fd); *out_err = (int)KL_ERR_CONNECT; return -1;
    }

    int rc = sp_connect(fd, &s->addrs[idx]);
    int ce = errno;
    if (rc == 0) {
        /* Connected synchronously (loopback usually). Report immediately; the
         * op tolerates synchronous completion by contract. */
        kl_connect_op_on_attempt_connected(&s->connect_op, idx, fd);
        return 0;
    }
    if (ce != EINPROGRESS) {
        sp_close(fd); *out_err = (int)KL_ERR_CONNECT; return -1;
    }

    s->attempt_fd[idx] = fd;
    if (!s->be->watcher_add ||
        s->be->watcher_add(s->async, (int)fd, HL_ASYNC_WRITE,
                           attempt_ready, s) != 0) {
        sp_close(fd);
        s->attempt_fd[idx] = KL_INVALID_SOCKET;
        *out_err = (int)KL_ERR_CONNECT;
        return -1;
    }
    return 0;
}

static void co_cancel_attempt(void *ctx, int idx)
{
    HlNetStream *s = ctx;
    if (idx < 0 || idx >= NET_MAX_ADDRS) return;
    if (kl_handle_valid(s->attempt_fd[idx])) {
        if (s->be->watcher_del)
            s->be->watcher_del(s->async, (int)s->attempt_fd[idx]);
        sp_close(s->attempt_fd[idx]);
        s->attempt_fd[idx] = KL_INVALID_SOCKET;
    }
    /* CONFIRM the retirement. The op cancel-REQUESTS an attempt; it is still
     * counted outstanding until the adapter reports back, and
     * "on_detach fires only after the terminal AND every outstanding op AND
     * both timers retire" (keel/connect_op.h). Without this line the attempt
     * never retires, the op never detaches, and the stream is never released:
     * that was the leak.
     *
     * Reported unconditionally, outside the fd check, because the op asked
     * about THIS index and needs an answer whether or not a descriptor was
     * still open for it. cap/smtp_transport.c does the same. */
    kl_connect_op_on_attempt_failed(&s->connect_op, idx, (int)KL_ERR_CONNECT);
}

static void co_dispose_fd(void *ctx, KlSocketHandle fd)
{
    (void)ctx;
    if (kl_handle_valid(fd)) sp_close(fd);
}

static void delay_fired(void *user)
{
    HlNetStream *s = user;
    s->delay_timer = 0;
    kl_connect_op_on_delay(&s->connect_op);
}

static int co_arm_delay(void *ctx)
{
    HlNetStream *s = ctx;
    if (!s->be->timer_add) return -1;
    s->delay_timer = s->be->timer_add(s->async, NET_ATTEMPT_DELAY_MS,
                                      delay_fired, s);
    return s->delay_timer ? 0 : -1;
}

static void co_cancel_delay(void *ctx)
{
    HlNetStream *s = ctx;
    if (s->delay_timer && s->be->timer_cancel) {
        s->be->timer_cancel(s->async, s->delay_timer);
        s->delay_timer = 0;
    }
}

static void deadline_fired(void *user)
{
    HlNetStream *s = user;
    s->deadline_timer = 0;
    kl_connect_op_on_deadline(&s->connect_op, (int)KL_ERR_TIMEOUT);
}

static int co_arm_deadline(void *ctx, int *out_err)
{
    HlNetStream *s = ctx;
    if (!s->be->timer_add) { *out_err = (int)KL_ERR_TIMEOUT; return -1; }
    s->deadline_timer = s->be->timer_add(s->async, s->connect_ms,
                                         deadline_fired, s);
    if (!s->deadline_timer) { *out_err = (int)KL_ERR_TIMEOUT; return -1; }
    return 0;
}

static void co_cancel_deadline(void *ctx)
{
    HlNetStream *s = ctx;
    if (s->deadline_timer && s->be->timer_cancel) {
        s->be->timer_cancel(s->async, s->deadline_timer);
        s->deadline_timer = 0;
    }
}

static void co_on_done(void *ctx, KlConnectResult result, KlSocketHandle fd,
                       int error)
{
    HlNetStream *s = ctx;
    s->connect_done = 1;

    if (result == KL_CONNECT_SUCCESS) {
        s->fd = fd;
        s->result = HL_NET_OK;
    } else if (result == KL_CONNECT_CANCELLED) {
        s->result = HL_NET_E_CANCELLED;
    } else {
        /* KL_CONNECT_FAILED covers resolve failure, every attempt failing,
         * AND the deadline: there is no KL_CONNECT_TIMEOUT. The distinction
         * an application acts on lives in `error`, so read it rather than
         * flattening a timeout into a refusal. */
        switch (error) {
        case (int)KL_ERR_TIMEOUT: s->result = HL_NET_E_TIMEOUT; break;
        case (int)KL_ERR_DNS:     s->result = HL_NET_E_RESOLVE; break;
        default:                  s->result = HL_NET_E_CONNECT; break;
        }
    }

    /* Losing descriptors are the op's to dispose via co_dispose_fd; anything
     * still in our table never reached the op and is ours to retire. */
    retire_attempts(s);
    wake(s);
}

static void co_on_detach(void *ctx)
{
    HlNetStream *s = ctx;
    s->connect_detached = 1;
    /* The one place a deferred free can complete. */
    maybe_release(s);
}

static const KlConnectOpHooks NET_CONNECT_HOOKS = {
    .start_resolve   = co_start_resolve,
    .cancel_resolve  = NULL,        /* resolution is inline and uninterruptible */
    .start_attempt   = co_start_attempt,
    .cancel_attempt  = co_cancel_attempt,
    .dispose_fd      = co_dispose_fd,
    .arm_delay       = co_arm_delay,
    .cancel_delay    = co_cancel_delay,
    .arm_deadline    = co_arm_deadline,
    .cancel_deadline = co_cancel_deadline,
    .on_done         = co_on_done,
    .on_detach       = co_on_detach,
};

/* ── Public API ─────────────────────────────────────────────────────── */

int hl_net_stream_connect(HlNetStream **out, const HlNetStreamConfig *cfg)
{
    if (!out) return HL_NET_E_INVAL;
    *out = NULL;
    if (!cfg || !cfg->host || !cfg->host[0]) return HL_NET_E_INVAL;
    if (cfg->port < 1 || cfg->port > 65535)  return HL_NET_E_INVAL;
    if (!cfg->async)                         return HL_NET_E_INVAL;

    const HlAsyncBackend *be = hl_async_backend();
    if (!be) return HL_NET_E_INVAL;
    /* Backend-agnostic on purpose. KlConnectOp and KlStream are hook-driven
     * state machines that take no event context, so all scheduling arrives
     * through the HlAsyncBackend vtable below and this works on the Keel and
     * poll backends alike. An earlier draft demanded the Keel loop here; that
     * was a gate on a requirement that does not exist. */

    HlNetStream *s = calloc(1, sizeof *s);
    if (!s) return HL_NET_E_NOMEM;

    s->async = cfg->async;
    s->be    = be;
    s->fd    = KL_INVALID_SOCKET;
    for (int i = 0; i < NET_MAX_ADDRS; i++) s->attempt_fd[i] = KL_INVALID_SOCKET;

    s->connect_ms = cfg->connect_ms > 0 ? cfg->connect_ms : NET_CONNECT_MS_DEF;
    s->read_cap   = cfg->read_cap  ? cfg->read_cap  : HL_NET_READ_CAP_DEFAULT;
    s->write_cap  = cfg->write_cap ? cfg->write_cap : HL_NET_WRITE_CAP_DEFAULT;
    if (s->read_cap  > HL_NET_READ_CAP_MAX)  s->read_cap  = HL_NET_READ_CAP_MAX;
    if (s->write_cap > HL_NET_WRITE_CAP_MAX) s->write_cap = HL_NET_WRITE_CAP_MAX;

    int rc = resolve_addrs(s, cfg->host, cfg->port);
    if (rc != HL_NET_OK) { free(s); return rc; }

    /* Nothing is armed until here, so every failure above frees outright. */
    if (kl_connect_op_init(&s->connect_op, &NET_CONNECT_HOOKS, s) != 0) {
        free(s);
        return HL_NET_E_IO;
    }
    if (kl_connect_op_start(&s->connect_op) != 0) {
        free(s);
        return HL_NET_E_IO;
    }
    /* Live from here: teardown must cancel and await detachment. */
    s->connect_started = 1;

    *out = s;
    if (s->connect_done) return s->result;   /* synchronous completion */

    s->op_pending = 1;
    return HL_NET_E_AGAIN;
}

int hl_net_stream_connect_result(HlNetStream *s)
{
    if (!s) return HL_NET_E_INVAL;
    if (s->closing) return HL_NET_E_CLOSED;
    return s->connect_done ? s->result : HL_NET_E_AGAIN;
}

struct HlAsyncOp *hl_net_stream_pending_op(HlNetStream *s)
{
    if (!s || !s->op_pending) return NULL;
    return &s->op;
}

void hl_net_stream_deadline(HlNetStream *s, int ms)
{
    if (!s) return;
    s->op.deadline_ms = ms > 0 ? (uint64_t)ms : 0;
}

void hl_net_stream_close(HlNetStream *s)
{
    if (!s || s->closing) return;
    s->closing = 1;
    s->result  = HL_NET_E_CLOSED;

    if (s->connect_started && !s->connect_done)
        kl_connect_op_cancel(&s->connect_op);

    if (kl_handle_valid(s->fd)) { sp_close(s->fd); s->fd = KL_INVALID_SOCKET; }
    wake(s);
}

void hl_net_stream_cancel(HlNetStream *s)
{
    if (!s) return;
    if (!s->closing) {
        s->closing = 1;
        s->result  = HL_NET_E_CANCELLED;
    }
    /* Cancel on NOT-DETACHED, not on not-done. A completed op still has to be
     * retired before it detaches, and on loopback the connect completes
     * synchronously, so gating on !connect_done skipped the cancel for exactly
     * the common case and the op never detached. That was the leak. */
    if (s->connect_started && !kl_connect_op_is_detached(&s->connect_op))
        kl_connect_op_cancel(&s->connect_op);

    co_cancel_delay(s);
    co_cancel_deadline(s);

    /* In-flight attempt descriptors belong to the op until it retires them:
     * kl_connect_op_cancel drives co_cancel_attempt / co_dispose_fd for each.
     * Closing them here as well raced those hooks. retire_attempts stays as
     * the final sweep in maybe_release, where nothing is live any more.
     *
     * s->fd is different: on_done handed it over, so it is ours to close. */
    if (kl_handle_valid(s->fd)) { sp_close(s->fd); s->fd = KL_INVALID_SOCKET; }
    wake(s);
}

void hl_net_stream_free(HlNetStream *s)
{
    if (!s || s->freed) return;
    hl_net_stream_cancel(s);
    s->freed = 1;
    /* Releases now if the op already detached, otherwise co_on_detach will. */
    maybe_release(s);
}

const char *hl_net_stream_strerror(int err)
{
    switch (err) {
    case HL_NET_OK:           return "ok";
    case HL_NET_E_DENIED:     return "capability denied";
    case HL_NET_E_RESOLVE:    return "host did not resolve";
    case HL_NET_E_CONNECT:    return "connection refused or unreachable";
    case HL_NET_E_TIMEOUT:    return "timed out";
    case HL_NET_E_CLOSED:     return "connection closed";
    case HL_NET_E_CANCELLED:  return "cancelled";
    case HL_NET_E_IO:         return "transport error";
    case HL_NET_E_NOMEM:      return "out of memory";
    case HL_NET_E_INVAL:      return "invalid argument";
    case HL_NET_E_AGAIN:      return "would block";
    default:                  return "unknown error";
    }
}
