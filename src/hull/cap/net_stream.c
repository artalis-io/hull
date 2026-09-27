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
#include <stddef.h>  /* offsetof: op -> owning stream */
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
static kl_ssize_t sp_send(KlSocketHandle fd, const void *b, size_t n)
{ const KlSocketProvider *sp = socket_provider(); return sp->ops->send(sp->context, fd, b, n); }
static kl_ssize_t sp_recv(KlSocketHandle fd, void *b, size_t n)
{ const KlSocketProvider *sp = socket_provider(); return sp->ops->recv(sp->context, fd, b, n); }
static int sp_close(KlSocketHandle fd)
{ const KlSocketProvider *sp = socket_provider(); return sp->ops->close(sp->context, fd); }

/* ── Limits ─────────────────────────────────────────────────────────── */

#define NET_MAX_ADDRS        8      /* addresses raced; bounds the fd set   */
#define HL_NET_HOST_MAX      256    /* 253 is the DNS maximum, plus NUL     */

/* Resolution runs on a pool worker, so the stream outlives connect() with
 * work still referencing it. These gate release exactly as detachment does. */
#define NET_RESOLVE_NONE     0
#define NET_RESOLVE_INFLIGHT 1
#define NET_RESOLVE_DONE     2
#define NET_CONNECT_MS_DEF   30000
#define NET_ATTEMPT_DELAY_MS 250    /* Happy-Eyeballs spacing               */

/* ── Stream ─────────────────────────────────────────────────────────── */

struct HlNetStream {
    /* Scheduling. The backend is borrowed from the caller (the binding passes
     * the runtime's async_ctx) so this file needs no runtime knowledge. */
    HlAsyncBackendCtx *async;
    const HlAsyncBackend *be;
    HlAsyncBackendPool *pool;       /* borrowed; resolution runs here */

    /* Resolution inputs, OWNED. The worker reads these after connect() has
     * returned, so borrowing the caller's host pointer would be a dangling
     * read the moment a Lua string is collected. */
    char         host[HL_NET_HOST_MAX];
    int          port;
    int          resolve_state;     /* NET_RESOLVE_*                        */
    int          resolve_rc;        /* HL_NET_* written by the worker       */

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

    /* I/O. Two bounded buffers and one watcher.
     *
     * Deliberately NOT KlStream. Its value is vectored writes, sendfile and
     * submit-mode, none of which a protocol like SSH uses, and wiring it costs
     * the arm/disarm/deliver hook set plus embedding stream_detail.h. Plain
     * recv/send against a readiness watcher is fewer moving parts to audit,
     * and KlStream stays available if vectored writes ever matter. */
    char        *rx;                /* lazily allocated, read_cap bytes      */
    size_t       rx_len, rx_off;    /* unread span is [rx_off, rx_len)       */
    char        *tx;                /* lazily allocated, write_cap bytes     */
    size_t       tx_len, tx_off;    /* unsent span is [tx_off, tx_len)       */
    unsigned     io_mask;           /* currently armed; 0 = not registered   */
    int          want_read;         /* a read is parked                      */
    int          want_write;        /* a write is parked on drain            */
    int          eof;               /* peer sent FIN                         */

    /* One caller-owned pointer, so a resume callback can find its binding
     * state. Never dereferenced or freed here; see net_stream.h. */
    void        *user;

    /* TLS. NULL = plaintext, which is every field below being inert.
     *
     * The session sits BETWEEN the socket and the rx/tx buffers above: the
     * buffers always hold plaintext, so everything downstream of here - the
     * read and write entry points, the parking protocol, the caps - is
     * unchanged by encryption. Only the two calls that touch the socket move. */
    const KlTlsConfig *tls_cfg;     /* borrowed; NULL = plaintext        */
    KlTls       *tls;
    KlAllocator  tls_alloc;         /* copied; the caller's need not persist */
    char        *tls_host;          /* owned; SNI + cert name               */
    int          tls_done;          /* handshake finished                   */
    unsigned     tls_want;          /* readiness the handshake is waiting on */
    uint64_t     tls_timer;         /* the handshake's share of the budget   */

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

/* The connect callback drives the handshake, and sits above the TLS and
 * I/O sections that implement it. */
static void tls_teardown(HlNetStream *s);
static int  tls_begin(HlNetStream *s, const KlTlsConfig *cfg);
static int  tls_step(HlNetStream *s);
static void io_rearm(HlNetStream *s);

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
    /* A worker may still be reading s->host. The pool fires exactly one of
     * done_fn / cancel_fn per item, and both run on the loop, so release from
     * there rather than racing the worker here. */
    if (s->resolve_state == NET_RESOLVE_INFLIGHT) return;

    if (s->connect_started && !s->connect_detached &&
        !kl_connect_op_is_detached(&s->connect_op)) return;

    if (s->deadline_timer && s->be->timer_cancel)
        s->be->timer_cancel(s->async, s->deadline_timer);
    if (s->delay_timer && s->be->timer_cancel)
        s->be->timer_cancel(s->async, s->delay_timer);

    retire_attempts(s);
    tls_teardown(s);
    if (s->io_mask && s->be->watcher_del)
        s->be->watcher_del(s->async, (int)s->fd);
    if (kl_handle_valid(s->fd)) sp_close(s->fd);

    /* The op lives INSIDE this allocation, and op_complete defers: a resume
     * queued by the wake() on the way here would otherwise fire against
     * freed storage on the next tick. Retracting it is the last thing before
     * the free, so nothing can queue another one afterwards. */
    if (s->be->op_cancel) s->be->op_cancel(s->async, &s->op);
    s->op_pending = 0;

    free(s->rx);
    free(s->tx);
    free(s);
}

/* ── Resolution ─────────────────────────────────────────────────────── */

/* Blocking getaddrinfo. Runs on a POOL WORKER, never on the event loop - see
 * resolve_work below, which is its only caller.
 *
 * That is the whole reason hl_net_stream_connect requires a pool. Calling it
 * inline would be the same thing cap/smtp_transport.c does, and would be
 * wrong here for the reason SMTP is right: SMTP already runs on a worker. A
 * fleet fan-out resolving eight nodes would stall the loop eight times.
 *
 * The shape is libuv's, not Keel's async resolver: HlAsyncBackend already
 * exposes pool_submit(work_fn, done_fn, cancel_fn), which nine other files
 * use. Resolution is short-lived, so a worker held for one lookup is nothing
 * like holding one for a whole connection - which is the thing section 6a of
 * the design rejected.
 *
 * Keel does ship kl_dns_resolver_create, and it cannot serve this. Its own
 * header scopes it to "recursive resolution via a configured nameserver (no
 * TCP fallback on truncation, no EDNS0, no DNSSEC, no /etc/hosts or search
 * domains)". That rules out more than mDNS: no /etc/hosts means even
 * "localhost" does not resolve, and no search domains means a bare hostname
 * does not either. hsctl leads with spark-7468.local, which is mDNS and so is
 * not DNS at all. getaddrinfo is the system resolver and honours whatever the
 * host is configured to do. */
static int resolve_addrs(HlNetStream *s)
{
    const char *host = s->host;
    const int   port = s->port;

    /* SEAM. When keel#332 lands and kl_resolve_sync is exported, this whole
     * body collapses to:
     *
     *     return kl_resolve_sync(host, (uint16_t)port, SOCK_STREAM,
     *                            s->addrs, NET_MAX_ADDRS, &s->naddrs) == 0
     *                ? HL_NET_OK : HL_NET_E_RESOLVE;
     *
     * and <netdb.h>, <netinet/in.h>, <sys/socket.h>, struct sockaddr_in/in6
     * and the AF_* constants all leave this file with it. Everything around
     * this function is already written against that shape: the caller only
     * needs "fill s->addrs / s->naddrs, return an HL_NET_* code".
     *
     * Until then this is the same getaddrinfo loop Keel keeps in
     * src/resolve_sync.c and Hull keeps a second copy of in
     * cap/smtp_transport.c. */
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
    /* Already resolved on a worker before the op started; report the count. */
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

        if (s->tls_cfg) {
            int rc = tls_begin(s, s->tls_cfg);
            if (rc != HL_NET_OK) {
                s->result = rc;
            } else if (tls_step(s) == 0) {
                /* Still handshaking. Deliberately no wake: the caller is
                 * parked on the connect, and a stream whose handshake has not
                 * finished is not yet a connection anything can be sent on.
                 * io_ready carries it from here and wakes once. */
                retire_attempts(s);
                io_rearm(s);
                return;
            }
        }
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

/* ── Resolution on a worker ─────────────────────────────────────────── */

/* On capacity, and why there is deliberately NO admission cap here.
 *
 * docs/dns_resolver_keel_design.md section 6 warns that pool-based resolution
 * "consumes pool capacity and can delay shutdown", and cap/smtp_admit.c answers
 * the same worry with a hard cap of max(1, floor(W/2)) concurrent operations.
 * Copying that here would be wrong.
 *
 * An SMTP send is a whole conversation and can run for minutes, so capping it
 * protects db and compute from a genuinely long occupation. A name lookup is
 * milliseconds in the normal case. The pool already provides the bound that
 * matters: the CLI entry point creates 4 workers with a 64-slot queue, so a
 * fleet fan-out of eight nodes runs four lookups and queues four, and
 * pool_submit reports back when the queue is actually full.
 *
 * With W = 4 the SMTP formula gives a cap of 2, which would REFUSE six of
 * those eight connects. That trades the primary use case away to halve a rare
 * pathological one.
 *
 * Residual risk, stated rather than designed around: getaddrinfo cannot be
 * cancelled or given a deadline, so a hung system resolver pins its worker
 * until the resolver's own timeout. W simultaneous hung lookups therefore
 * stall other async work for that long, and delay pool_free by the same.
 * A cap would halve that and cost the fan-out; the trade only becomes worth
 * revisiting if a real hang shows up, which is also what section 7 of that
 * record says about this whole area. */

/* WORKER THREAD. Touches only the resolution inputs (read) and outputs
 * (write), nothing else on the stream, and calls nothing back. Everything that
 * needs the loop happens in resolve_done below. */
static void resolve_work(void *user)
{
    HlNetStream *s = user;
    s->resolve_rc = resolve_addrs(s);
}

/* EVENT LOOP. The worker finished; start the connect op, or fail terminally. */
static void resolve_done(void *user)
{
    HlNetStream *s = user;
    s->resolve_state = NET_RESOLVE_DONE;

    /* Freed or cancelled while the lookup was in flight: the names are
     * useless now, and this callback is the release point. */
    if (s->freed || s->closing) { maybe_release(s); return; }

    if (s->resolve_rc != HL_NET_OK) {
        s->result       = s->resolve_rc;
        s->connect_done = 1;
        wake(s);
        return;
    }

    if (kl_connect_op_init(&s->connect_op, &NET_CONNECT_HOOKS, s) != 0 ||
        kl_connect_op_start(&s->connect_op) != 0) {
        s->result       = HL_NET_E_IO;
        s->connect_done = 1;
        wake(s);
        return;
    }
    s->connect_started = 1;

    /* A synchronous completion already woke the caller inside start(). */
    if (s->connect_done) wake(s);
}

/* EVENT LOOP. Queued but never started (pool drained at shutdown). */
static void resolve_cancel(void *user)
{
    HlNetStream *s = user;
    s->resolve_state = NET_RESOLVE_DONE;
    if (!s->closing) { s->closing = 1; s->result = HL_NET_E_CANCELLED; }
    s->connect_done = 1;
    wake(s);
    maybe_release(s);
}

/* ── I/O ────────────────────────────────────────────────────────────── */

static void io_ready(int fd, unsigned ready, void *user);

/* ── TLS ────────────────────────────────────────────────────────────── */

/* Tear the session down. Best effort on the close_notify: a peer that has
 * already gone means the shutdown cannot be delivered, and that is not a
 * failure worth reporting to a caller who asked to close. */
static void tls_teardown(HlNetStream *s)
{
    if (s->tls_timer && s->be->timer_cancel) {
        s->be->timer_cancel(s->async, s->tls_timer);
        s->tls_timer = 0;
    }
    if (s->tls) {
        if (s->tls_done && s->tls->shutdown && kl_handle_valid(s->fd))
            (void)s->tls->shutdown(s->tls, s->fd);
        if (s->tls->destroy) s->tls->destroy(s->tls);
        s->tls = NULL;
    }
    free(s->tls_host);
    s->tls_host = NULL;
}

/* The connect deadline expired with the handshake still running.
 *
 * Needed because the connect OP's deadline is cancelled the moment it
 * completes, and the handshake runs after that - so without this a peer that
 * completes a TCP connection and then says nothing would park the caller
 * forever. From the caller's side the connect is not finished until the
 * handshake is, so the same budget covers both. */
static void tls_deadline_fired(void *user)
{
    HlNetStream *s = user;
    s->tls_timer = 0;
    if (s->tls_done) return;
    s->result = HL_NET_E_TIMEOUT;
    s->tls_want = 0;
    io_rearm(s);
    wake(s);
}

/* Drive the handshake one step and record what it is waiting on.
 *
 * Returns 0 while still running, 1 when complete, -1 on failure. The caller
 * decides what to do about each; nothing here wakes a parked caller, because
 * the handshake runs INSIDE the connect park - a caller asked for a usable
 * stream, and with TLS that means one that has finished its handshake. Waking
 * early would hand back a connection nothing could be sent on. */
static int tls_step(HlNetStream *s)
{
    if (!s->tls || s->tls_done) return 1;

    KlTlsResult r = s->tls->handshake(s->tls, s->fd);
    if (r == KL_TLS_OK) {
        s->tls_done = 1;
        s->tls_want = 0;
        if (s->tls_timer && s->be->timer_cancel) {
            s->be->timer_cancel(s->async, s->tls_timer);
            s->tls_timer = 0;
        }
        return 1;
    }
    if (r == KL_TLS_WANT_READ)  { s->tls_want = HL_ASYNC_READ;  return 0; }
    if (r == KL_TLS_WANT_WRITE) { s->tls_want = HL_ASYNC_WRITE; return 0; }

    /* A failed handshake is terminal and deliberately not detailed: the
     * reasons an mbedTLS handshake fails (bad certificate, no common cipher,
     * hostname mismatch) are all "this is not the peer you asked for", and a
     * caller cannot act differently on which. */
    s->result = HL_NET_E_TLS;
    if (s->tls_timer && s->be->timer_cancel) {
        s->be->timer_cancel(s->async, s->tls_timer);
        s->tls_timer = 0;
    }
    return -1;
}

/* Begin TLS once the socket is connected. Returns 0 on success, negative on a
 * setup failure the caller should report. */
static int tls_begin(HlNetStream *s, const KlTlsConfig *cfg)
{
    if (!cfg || !cfg->factory) return HL_NET_E_INVAL;

    s->tls = cfg->factory(cfg->ctx, &s->tls_alloc);
    if (!s->tls) return HL_NET_E_TLS;
    if (!kl_tls_vtable_valid(s->tls)) {
        /* A backend that does not fill the required ops would fail later in a
         * way that looks like a network fault. Refuse it here instead. */
        if (s->tls->destroy) s->tls->destroy(s->tls);
        s->tls = NULL;
        return HL_NET_E_TLS;
    }

    /* SNI and the name the certificate is checked against. A backend without
     * set_hostname cannot verify a name, so refuse rather than negotiate an
     * encrypted connection to an unverified peer: that is the failure mode
     * TLS exists to prevent. */
    if (!s->tls->set_hostname || !s->tls_host ||
        s->tls->set_hostname(s->tls, s->tls_host) != 0) {
        if (s->tls->destroy) s->tls->destroy(s->tls);
        s->tls = NULL;
        return HL_NET_E_TLS;
    }

    if (s->be->timer_add)
        s->tls_timer = s->be->timer_add(s->async, s->connect_ms,
                                        tls_deadline_fired, s);
    return HL_NET_OK;
}

/* Re-arm the readiness watcher to exactly what is wanted now. Registering once
 * and modifying afterwards keeps the backend's fd table stable; a mask of 0
 * deregisters, so an idle stream costs the loop nothing. */
static void io_rearm(HlNetStream *s)
{
    if (!kl_handle_valid(s->fd) || s->closing) return;

    unsigned want = 0;
    if (s->tls && !s->tls_done) {
        /* Mid-handshake the socket readiness that matters is whatever the
         * handshake asked for, not what the application wants: no application
         * byte can move until it finishes. */
        want = s->tls_want;
    } else {
        if (s->want_read && !s->eof)   want |= HL_ASYNC_READ;
        if (s->tx_off < s->tx_len)     want |= HL_ASYNC_WRITE;
    }
    if (want == s->io_mask) return;

    if (!want) {
        /* Only reachable when io_mask differs from want, and want is 0 here,
         * so io_mask is non-zero: there IS a registration to remove. */
        if (s->be->watcher_del)
            s->be->watcher_del(s->async, (int)s->fd);
        s->io_mask = 0;
        return;
    }
    if (!s->io_mask) {
        if (s->be->watcher_add &&
            s->be->watcher_add(s->async, (int)s->fd, want, io_ready, s) == 0)
            s->io_mask = want;
        return;
    }
    if (s->be->watcher_mod &&
        s->be->watcher_mod(s->async, (int)s->fd, want) == 0)
        s->io_mask = want;
}

/* Compact the receive buffer so a partially-drained span does not wedge the
 * free tail. Cheap: the unread span is at most read_cap and usually small. */
static void rx_compact(HlNetStream *s)
{
    if (s->rx_off == 0) return;
    size_t n = s->rx_len - s->rx_off;
    if (n) memmove(s->rx, s->rx + s->rx_off, n);
    s->rx_len = n;
    s->rx_off = 0;
}

static void tx_compact(HlNetStream *s)
{
    if (s->tx_off == 0) return;
    size_t n = s->tx_len - s->tx_off;
    if (n) memmove(s->tx, s->tx + s->tx_off, n);
    s->tx_len = n;
    s->tx_off = 0;
}

/* Push whatever is queued. Returns 0, or a negative HL_NET_E_* on a hard
 * transport error. A short write is normal and simply leaves the remainder. */
static int tx_flush(HlNetStream *s)
{
    /* Nothing may be sent before the handshake completes; the queue simply
     * waits, which is what the caller's backpressure already handles. */
    if (s->tls && !s->tls_done) return HL_NET_OK;

    while (s->tx_off < s->tx_len) {
        kl_ssize_t n;
        if (s->tls) {
            /* 0 is WANT_WRITE, not EOF: the record could not be flushed to
             * the socket, so the remainder stays queued exactly as it does
             * for a short plaintext write. */
            n = s->tls->write(s->tls, s->fd, s->tx + s->tx_off,
                              s->tx_len - s->tx_off);
            if (n > 0) { s->tx_off += (size_t)n; continue; }
            if (n == 0) break;
            return HL_NET_E_IO;
        }
        n = sp_send(s->fd, s->tx + s->tx_off, s->tx_len - s->tx_off);
        if (n > 0) { s->tx_off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        return HL_NET_E_IO;
    }
    if (s->tx_off == s->tx_len) { s->tx_len = s->tx_off = 0; }
    return HL_NET_OK;
}

/* Fill the receive buffer from the socket, decrypting on the way when a
 * session is present. Shared by the readiness callback and the read entry
 * point, which needs it for the buffered-plaintext case below. */
static void rx_fill(HlNetStream *s)
{
    if (!s->rx || s->eof) return;
    rx_compact(s);
    while (s->rx_len < s->read_cap) {
        kl_ssize_t n;
        if (s->tls) {
            /* The TLS return codes do NOT line up with recv's, and assuming
             * they do is how every ordinary end-of-response becomes a
             * transport error:
             *
             *   >0  plaintext
             *    0  WANT_READ - retry later, NOT end of stream
             *   -1  error OR a clean close_notify; only at_eof separates them
             *
             * The -1 case is the trap. A fake that returns 0 for EOF, which is
             * what the header's prose suggests, passes every test and then
             * fails against a real server on the first response that ends. */
            n = s->tls->read(s->tls, s->fd, s->rx + s->rx_len,
                             s->read_cap - s->rx_len);
            if (n > 0) { s->rx_len += (size_t)n; continue; }
            if (n == 0) break;                      /* WANT_READ */
            s->eof = 1;
            if (!(s->tls->at_eof && s->tls->at_eof(s->tls)))
                s->result = HL_NET_E_IO;
            break;
        }
        n = sp_recv(s->fd, s->rx + s->rx_len, s->read_cap - s->rx_len);
        if (n > 0) { s->rx_len += (size_t)n; continue; }
        if (n == 0) { s->eof = 1; break; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        s->result = HL_NET_E_IO;
        s->eof    = 1;
        break;
    }
}

/* Copy out of the receive buffer. Returns bytes moved, 0 when it is empty. */
static size_t rx_take(HlNetStream *s, void *buf, size_t len)
{
    if (!s->rx || s->rx_off >= s->rx_len) return 0;
    size_t n = s->rx_len - s->rx_off;
    if (n > len) n = len;
    memcpy(buf, s->rx + s->rx_off, n);
    s->rx_off += n;
    if (s->rx_off == s->rx_len) { s->rx_len = s->rx_off = 0; }
    return n;
}

/* Readiness on the connected socket. */
static void io_ready(int fd, unsigned ready, void *user)
{
    (void)fd;
    HlNetStream *s = user;
    int woke = 0;

    /* Until the handshake is done the socket belongs to it, whichever
     * direction became ready. */
    if (s->tls && !s->tls_done) {
        int r = tls_step(s);
        if (r != 0) {
            /* Complete or failed: either way the connect park ends here. */
            io_rearm(s);
            wake(s);
            return;
        }
        io_rearm(s);
        return;
    }

    if ((ready & HL_ASYNC_READ) && s->rx) {
        rx_fill(s);
        if (s->want_read && (s->rx_off < s->rx_len || s->eof)) {
            s->want_read = 0;
            woke = 1;
        }
    }

    if (ready & HL_ASYNC_WRITE) {
        if (tx_flush(s) != HL_NET_OK) { s->result = HL_NET_E_IO; s->eof = 1; woke = 1; }
        else if (s->want_write && s->tx_off == s->tx_len) {
            s->want_write = 0;
            woke = 1;
        }
    }

    io_rearm(s);
    if (woke) wake(s);
}

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

    /* Resolution is blocking, so it runs on a worker rather than on the loop
     * this transport is scheduled on. A pool is therefore required; both Hull
     * entry points create one (serve.c and serve_cli.c), so a NULL here is a
     * wiring error, and blocking the loop instead would be the wrong
     * "recovery". */
    if (!cfg->pool) return HL_NET_E_INVAL;

    size_t hlen = strlen(cfg->host);
    if (hlen >= HL_NET_HOST_MAX) return HL_NET_E_INVAL;

    /* Asking for TLS without the means to do it is a wiring error, and the
     * one failure mode that must NOT degrade quietly: a caller that wanted an
     * encrypted stream and got a plaintext one would never find out. The
     * context creators in tls_transport.h return NULL when TLS is not
     * composed, so this is also where that reaches a caller. */
    if (cfg->tls && (!cfg->tls->factory || !cfg->tls_alloc))
        return HL_NET_E_INVAL;

    HlNetStream *s = calloc(1, sizeof *s);
    if (!s) return HL_NET_E_NOMEM;

    s->async = cfg->async;
    s->be    = be;
    s->pool  = cfg->pool;
    s->fd    = KL_INVALID_SOCKET;
    for (int i = 0; i < NET_MAX_ADDRS; i++) s->attempt_fd[i] = KL_INVALID_SOCKET;

    /* Owned copy: the worker reads this after we return. */
    memcpy(s->host, cfg->host, hlen + 1);
    s->port = cfg->port;

    if (cfg->tls) {
        const char *name = cfg->tls_hostname ? cfg->tls_hostname : cfg->host;
        s->tls_host = strdup(name);
        if (!s->tls_host) { free(s); return HL_NET_E_NOMEM; }
        s->tls_cfg   = cfg->tls;
        s->tls_alloc = *cfg->tls_alloc;   /* by value: the caller's may go */
    }

    s->connect_ms = cfg->connect_ms > 0 ? cfg->connect_ms : NET_CONNECT_MS_DEF;
    s->read_cap   = cfg->read_cap  ? cfg->read_cap  : HL_NET_READ_CAP_DEFAULT;
    s->write_cap  = cfg->write_cap ? cfg->write_cap : HL_NET_WRITE_CAP_DEFAULT;
    if (s->read_cap  > HL_NET_READ_CAP_MAX)  s->read_cap  = HL_NET_READ_CAP_MAX;
    if (s->write_cap > HL_NET_WRITE_CAP_MAX) s->write_cap = HL_NET_WRITE_CAP_MAX;

    /* Mark BEFORE submitting: the work can start on another thread the moment
     * submit returns, and maybe_release must already be refusing by then. */
    s->resolve_state = NET_RESOLVE_INFLIGHT;
    if (be->pool_submit(s->pool, resolve_work, resolve_done,
                        resolve_cancel, s) != 0) {
        /* Never queued, so no callback will fire and nothing else references
         * the stream yet - this is the one place the stream is released
         * WITHOUT going through maybe_release, so anything allocated above
         * has to be released by hand. tls_host was not, and the queue being
         * full is exactly the fan-out case this transport is built for. */
        s->resolve_state = NET_RESOLVE_NONE;
        free(s->tls_host);
        free(s);
        return HL_NET_E_IO;
    }

    *out = s;
    s->op_pending = 1;
    return HL_NET_E_AGAIN;
}

int hl_net_stream_connect_result(HlNetStream *s)
{
    if (!s) return HL_NET_E_INVAL;
    if (s->closing) return HL_NET_E_CLOSED;
    if (!s->connect_done) return HL_NET_E_AGAIN;
    /* A connected socket whose handshake is still running is not yet usable,
     * and saying OK here would hand the caller a stream whose first write
     * goes nowhere. */
    if (s->result == HL_NET_OK && s->tls && !s->tls_done) return HL_NET_E_AGAIN;
    return s->result;
}

struct HlAsyncOp *hl_net_stream_pending_op(HlNetStream *s)
{
    if (!s || !s->op_pending) return NULL;
    return &s->op;
}

/* The op is embedded by value, so the walk back is a fixed offset. Done here
 * rather than by the caller because the layout is private to this file. */
HlNetStream *hl_net_stream_from_op(struct HlAsyncOp *op)
{
    if (!op) return NULL;
    return (HlNetStream *)((char *)op - offsetof(HlNetStream, op));
}

void hl_net_stream_set_user(HlNetStream *s, void *user)
{
    if (s) s->user = user;
}

void *hl_net_stream_user(const HlNetStream *s)
{
    return s ? s->user : NULL;
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

    /* Before the descriptor goes: close_notify needs it. */
    tls_teardown(s);
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

    if (s->io_mask && s->be->watcher_del) {
        s->be->watcher_del(s->async, (int)s->fd);
        s->io_mask = 0;
    }

    tls_teardown(s);

    /* In-flight attempt descriptors belong to the op until it retires them:
     * kl_connect_op_cancel drives co_cancel_attempt / co_dispose_fd for each.
     * Closing them here as well raced those hooks. retire_attempts stays as
     * the final sweep in maybe_release, where nothing is live any more.
     *
     * s->fd is different: on_done handed it over, so it is ours to close. */
    if (kl_handle_valid(s->fd)) { sp_close(s->fd); s->fd = KL_INVALID_SOCKET; }
    wake(s);
}

long hl_net_stream_read(HlNetStream *s, void *buf, size_t len)
{
    if (!s || (!buf && len)) return HL_NET_E_INVAL;
    if (s->closing)          return HL_NET_E_CLOSED;
    if (!s->connect_done)    return HL_NET_E_AGAIN;   /* still connecting */
    if (s->result != HL_NET_OK && s->result != HL_NET_E_CLOSED)
        return s->result;
    if (!len) return 0;

    if (!s->rx) {
        s->rx = malloc(s->read_cap);
        if (!s->rx) return HL_NET_E_NOMEM;
    }

    /* Serve whatever arrived. A read returns WHAT IS THERE, never "one
     * record": framing is the caller's business, which is what lets a protocol
     * sit on top without this layer knowing it. */
    {
        size_t n = rx_take(s, buf, len);
        if (n) return (long)n;
    }

    /* TLS can decrypt several application records out of one TCP segment.
     * Those bytes are inside the session, not on the socket, so arming a
     * readiness watcher would wait for a segment that never comes - the
     * classic edge-triggered TLS stall. Drain them before parking. */
    if (s->tls && s->tls_done && s->tls->pending && s->tls->pending(s->tls)) {
        rx_fill(s);
        size_t n = rx_take(s, buf, len);
        if (n) return (long)n;
    }

    if (s->eof) return 0;              /* clean EOF, after draining */

    s->want_read = 1;
    s->op_pending = 1;
    io_rearm(s);
    return HL_NET_E_AGAIN;
}

int hl_net_stream_write(HlNetStream *s, const void *buf, size_t len)
{
    if (!s || (!buf && len)) return HL_NET_E_INVAL;
    if (s->closing)          return HL_NET_E_CLOSED;
    if (!s->connect_done)    return HL_NET_E_AGAIN;
    if (s->result != HL_NET_OK) return s->result;
    if (!len) return HL_NET_OK;

    /* A buffer larger than the whole send capacity can never be admitted, so
     * say so instead of parking forever. */
    if (len > s->write_cap) return HL_NET_E_INVAL;

    if (!s->tx) {
        s->tx = malloc(s->write_cap);
        if (!s->tx) return HL_NET_E_NOMEM;
    }

    tx_compact(s);
    if (s->write_cap - s->tx_len < len) {
        /* All-or-none: no room for the whole buffer, so admit none of it and
         * park until the queue drains. Backpressure, not an error. */
        s->want_write = 1;
        s->op_pending = 1;
        io_rearm(s);
        return HL_NET_E_AGAIN;
    }

    memcpy(s->tx + s->tx_len, buf, len);
    s->tx_len += len;

    if (tx_flush(s) != HL_NET_OK) { s->result = HL_NET_E_IO; return HL_NET_E_IO; }
    io_rearm(s);
    return HL_NET_OK;
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
    case HL_NET_E_TLS:        return "TLS handshake failed";
    default:                  return "unknown error";
    }
}
