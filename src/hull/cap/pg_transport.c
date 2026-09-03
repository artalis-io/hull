/*
 * pg_transport.c: PostgreSQL byte transport over Keel v3 public primitives.
 *
 * See include/hull/cap/pg_transport.h for the ownership split and the contract.
 *
 * This composes Keel's PUBLIC transport surface for the CONNECT phase only:
 * KlConnectOp (resolve + Happy-Eyeballs racing connect) driven on a PRIVATE,
 * operation-local KlEventCtx, plus a KlSocketProvider's public ops table. The
 * connect machinery is the reviewed cap/smtp_transport.c pattern (resolve adapter
 * + KlConnectOp hooks + private KlEventCtx pump + socket-provider wrappers), MINUS
 * the KlStream: a synchronous PostgreSQL connection needs no queued-write /
 * read pause-resume / graceful-close machinery. After the race is won the winning
 * fd is set_blocking()'d and every subsequent byte is plain blocking recv / send
 * with no event loop (design D1).
 *
 * TLS stays the shared hl_tls_client_* helpers layered over the blocking fd
 * (design D2). This transport only STORES the optional attached HlTlsClient and
 * routes send/recv through it; the SSLRequest negotiation + handshake belong to
 * the consumer (pg_conn.c), which hands the session here via attach_tls.
 *
 * The transport is a HEAP allocation reached through an opaque handle so the
 * exceptional non-detachment path can drop the whole block intentionally and
 * safely (a live op keeps referencing its own storage). See the header banner.
 *
 * NO vendor/keel/src header is used. keel/connect_op_detail.h + keel/event_ctx.h
 * are included solely so KlConnectOp / KlEventCtx can be EMBEDDED fields (storage
 * sizing via the public opt-in layout); their internal struct fields are never
 * read or written.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/pg_transport.h"
#include "hull/shared/tls_client.h"   /* hl_tls_client_read/_write/_shutdown/_free */

/* PUBLIC Keel headers only. */
#include <keel/connect_op.h>
#include <keel/connect_op_detail.h> /* opt-in layout: embed a KlConnectOp (storage only) */
#include <keel/event_ctx.h>
#include <keel/event.h>
#include <keel/socket.h>
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include <keel/timer.h>
#include <keel/error.h>
#include <keel/allocator.h>
#include <keel/clock.h>   /* kl_monotonic_ms: monotonic connect deadline */

/* Resolution is a sandbox-compatible BLOCKING getaddrinfo, kept out of
 * cap/pg_conn.c (which the design keeps free of getaddrinfo/socket/poll/raw I/O).
 * This preserves the current Postgres behavior: /etc/hosts, search domains, and
 * the OS resolver the kernel-sandbox network-outbound grant permits. */
#include <netdb.h>
#include <sys/socket.h>   /* AF_UNSPEC / AF_INET / AF_INET6 (cosmo needs it explicit) */
#include <netinet/in.h>   /* struct sockaddr_in / sockaddr_in6 for the resolver */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>   /* atoi, calloc, free */
#include <string.h>

#include "log.h"

/* One connection's byte transport (opaque to consumers; defined here). Heap-
 * allocated; the exceptional non-detachment path leaks the whole block. */
struct PgTransport {
    /* Borrowed, immutable, outlives the connection (Amendment 4). */
    const KlSocketProvider *sp;

    /* Owned: the private connect-time event context. Created only when a race is
     * needed (hl_pg_transport_connect); the adopt path leaves it uninitialized. */
    KlEventCtx  ev;
    int         ev_ready;      /* 1 once kl_event_ctx_init succeeded            */
    KlAllocator alloc;         /* event-ctx allocator (event-loop lifetime)     */

    /* Owned: the embedded KlConnectOp and its resolved address set. */
    KlConnectOp connect_op;       /* embedded (connect_op_detail.h: storage only) */
    int         connect_started;  /* 1 once kl_connect_op_start succeeded        */
    KlSockAddr  addrs[KL_CONNECT_MAX_ADDRS];
    int         naddrs;
    KlSocketHandle attempt_fd[KL_CONNECT_MAX_ADDRS]; /* in-flight racing fds     */
    int         attempt_armed[KL_CONNECT_MAX_ADDRS]; /* 1 if a watcher is armed  */

    /* Connect outcome. */
    int             connect_done;      /* on_done fired                          */
    KlConnectResult connect_result;
    int             connect_error;

    /* Overall connect deadline (TCP establishment only, design D3). */
    int64_t   deadline_timer;   /* Keel timer id, or -1                          */
    uint64_t  deadline_ms;      /* absolute monotonic instant, or 0 = unbounded  */
    int       deadline_fired;

    /* RFC 8305 Connection Attempt Delay timer (the Happy-Eyeballs stagger). */
    int64_t   delay_timer;      /* Keel timer id, or -1                          */

    /* The winning connected descriptor (blocking after connect). */
    KlSocketHandle fd;

    /* Owned: optional attached TLS session. When non-NULL, send/recv tunnel
     * through it (design D2); the transport frees it in close. */
    struct HlTlsClient *tls;
};

/* SIGPIPE suppression on send has parity with pg_conn.c: the winning / adopted fd
 * gets SO_NOSIGPIPE via the provider's set_nosigpipe hook where the platform
 * offers it, and the POSIX provider's send applies MSG_NOSIGNAL internally, so
 * hl_pg_transport_send never emits SIGPIPE. */

#define PG_PUMP_STEP_MS   50   /* one kl_event_ctx_run readiness wait (a bounded increment) */

/* Teardown detachment confirm is ITERATION-bounded (deterministic), NOT
 * wall-bounded: after cancel a well-behaved op retires everything synchronously
 * and detaches within 0-1 steps, so this bound only decides when to declare a
 * pathological non-detachment (fail closed, preserve storage). This is a teardown
 * safety bound, never the connect operation's deadline (which stays select(NULL)
 * unbounded when timeout_ms <= 0). */
#define PG_DETACH_MAX_STEPS  256

/* RFC 8305 Connection Attempt Delay: wait before starting the next address while
 * an earlier attempt is still in flight (the Happy-Eyeballs stagger). */
#define PG_CONNECT_ATTEMPT_DELAY_MS  250

/* ────────────────────────────────────────────────────────────────────────────
 * Socket provider: an immutable process-wide ops table. We call its PUBLIC ops
 * directly. Would-block / EINPROGRESS / EINTR classification prefers the
 * provider's io_status op and falls back to hosted errno only when it is absent.
 * ────────────────────────────────────────────────────────────────────────── */

static const KlSocketProvider *g_sp;
static pthread_once_t          g_sp_once = PTHREAD_ONCE_INIT;
static void init_sp(void) { g_sp = kl_socket_provider_posix(); }
static const KlSocketProvider *default_provider(void)
{
    pthread_once(&g_sp_once, init_sp);
    return g_sp;
}

#ifdef HL_PG_TEST_HOOKS
/* Test-only seam (compiled ONLY under -DHL_PG_TEST_HOOKS, ABSENT from the
 * production object). pg_test_socket_provider REPLACES the default POSIX provider
 * for every sp_* op; pg_test_resolve fills t->addrs in exact injected order;
 * pg_test_checkpoint observes each pump checkpoint; pg_test_force_no_detach makes
 * the detach-confirm predicate report "not detached" so a test can drive the
 * pathological non-detachment path (preserve storage + retryable close). */
const KlSocketProvider *pg_test_socket_provider;
int (*pg_test_resolve)(PgTransport *t, const char *host, int port);
void (*pg_test_checkpoint)(PgTransport *t, unsigned idx);
int  pg_test_force_no_detach;
static unsigned pg_test_checkpoint_seq;
#endif

/* The active provider: a test-supplied one when set, else the transport's own
 * borrowed reference. */
static const KlSocketProvider *active_sp(const PgTransport *t)
{
#ifdef HL_PG_TEST_HOOKS
    if (pg_test_socket_provider) return pg_test_socket_provider;
#endif
    return t->sp ? t->sp : default_provider();
}

/* Required provider op subset + capability. Missing any is fail-closed: the
 * private event loop watches provider handles (needs KL_SOCK_CAP_NATIVE_FD), and
 * the connect / I/O / close paths dereference these ops. io_status and
 * set_nosigpipe are OPTIONAL (documented hosted-errno / no-op fallbacks). */
static int validate_provider(const KlSocketProvider *sp)
{
    if (!sp || !sp->ops)
        return -1;
    const KlSocketOps *o = sp->ops;
    if (!o->socket || !o->connect || !o->close || !o->send || !o->recv ||
        !o->get_so_error || !o->set_nonblocking || !o->set_blocking)
        return -1;
    if (!kl_socket_provider_has_cap(sp, KL_SOCK_CAP_NATIVE_FD))
        return -1;
    return 0;
}

static KlSocketHandle sp_socket(const PgTransport *t, int domain, int type, int protocol)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->socket(sp->context, domain, type, protocol); }
static int sp_set_nonblocking(const PgTransport *t, KlSocketHandle fd)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->set_nonblocking(sp->context, fd); }
static int sp_set_blocking(const PgTransport *t, KlSocketHandle fd)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->set_blocking(sp->context, fd); }
static void sp_set_nosigpipe(const PgTransport *t, KlSocketHandle fd)
{ const KlSocketProvider *sp = active_sp(t); if (sp->ops->set_nosigpipe) sp->ops->set_nosigpipe(sp->context, fd); }
static int sp_connect(const PgTransport *t, KlSocketHandle fd, const KlSockAddr *a)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->connect(sp->context, fd, a); }
static int sp_get_so_error(const PgTransport *t, KlSocketHandle fd, int *out)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->get_so_error(sp->context, fd, out); }
static kl_ssize_t sp_send(const PgTransport *t, KlSocketHandle fd, const void *b, size_t n)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->send(sp->context, fd, b, n); }
static kl_ssize_t sp_recv(const PgTransport *t, KlSocketHandle fd, void *b, size_t n)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->recv(sp->context, fd, b, n); }
static int sp_close(const PgTransport *t, KlSocketHandle fd)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->close(sp->context, fd); }

/* Classify the last op: prefer the provider's io_status, else hosted errno. */
static KlIoStatus sp_io_status(const PgTransport *t)
{
    const KlSocketProvider *sp = active_sp(t);
    if (sp->ops->io_status)
        return sp->ops->io_status(sp->context);
    switch (errno) {
        case EINTR:       return KL_IO_INTERRUPTED;
        case EINPROGRESS: return KL_IO_PENDING;
#if EAGAIN != EWOULDBLOCK
        case EAGAIN:      return KL_IO_PENDING;
        case EWOULDBLOCK: return KL_IO_PENDING;
#else
        case EAGAIN:      return KL_IO_PENDING;
#endif
        case EPIPE:       return KL_IO_CLOSED;
        case ECONNRESET:  return KL_IO_RESET;
        case 0:           return KL_IO_OK;
        default:          return KL_IO_FATAL;
    }
}

static void set_err(char *dst, size_t cap, const char *msg)
{ if (dst && cap) snprintf(dst, cap, "%s", msg); }

/* ────────────────────────────────────────────────────────────────────────────
 * KlConnectOp hooks (bring-your-own I/O).
 *
 * Resolution is a sandbox-compatible BLOCKING getaddrinfo (or an IP-literal fast
 * path) that completes synchronously before start; the hook only yields the
 * pre-filled address list to KlConnectOp.
 * ────────────────────────────────────────────────────────────────────────── */

static void co_connect_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data);

/* Resolve host:port into t->addrs. An IP-literal host takes the no-DNS
 * kl_sockaddr_parse fast path; a hostname goes through getaddrinfo. Returns
 * naddrs (>=1) or 0. */
static int resolve_addrs(PgTransport *t, const char *host, int port)
{
#ifdef HL_PG_TEST_HOOKS
    if (pg_test_resolve)
        return pg_test_resolve(t, host, port);
#endif
    /* IP-literal fast path: kl_sockaddr_parse is numeric-only (no DNS), matching
     * the IP-literal-only databases.dynamic CIDR gate. */
    if (kl_sockaddr_parse(&t->addrs[0], host, (uint16_t)port) == 0)
        return 1;

    char port_str[8];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return 0;

    int n = 0;
    for (struct addrinfo *ai = res; ai && n < KL_CONNECT_MAX_ADDRS; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET && ai->ai_addr) {
            const struct sockaddr_in *s = (const struct sockaddr_in *)(const void *)ai->ai_addr;
            uint8_t ip[4];
            memcpy(ip, &s->sin_addr, 4);
            if (kl_sockaddr_from_ipv4(&t->addrs[n], ip, (uint16_t)port) == 0)
                n++;
        } else if (ai->ai_family == AF_INET6 && ai->ai_addr) {
            const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)ai->ai_addr;
            uint8_t ip6[16];
            memcpy(ip6, &s6->sin6_addr, 16);
            if (kl_sockaddr_from_ipv6(&t->addrs[n], ip6, (uint16_t)port,
                                      s6->sin6_scope_id) == 0)
                n++;
        }
    }
    freeaddrinfo(res);
    return n;
}

static int co_start_resolve(void *ctx)
{
    PgTransport *t = ctx;
    /* t->naddrs was filled by resolve_addrs before start (synchronous, inline). */
    if (t->naddrs < 1) {
        kl_connect_op_on_resolve_failed(&t->connect_op, (int)KL_ERR_DNS);
        return 0;
    }
    kl_connect_op_on_resolved(&t->connect_op, t->naddrs);
    return 0;
}

static int co_start_attempt(void *ctx, int idx, int *out_err)
{
    PgTransport *t = ctx;
    if (idx < 0 || idx >= t->naddrs) { *out_err = (int)KL_ERR_CONNECT; return -1; }

    KlSocketHandle fd = sp_socket(t, t->addrs[idx].family == KL_AF_INET6 ? AF_INET6
                                                                         : AF_INET,
                                  SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) { *out_err = (int)KL_ERR_CONNECT; return -1; }
    if (sp_set_nonblocking(t, fd) < 0) { sp_close(t, fd); *out_err = (int)KL_ERR_CONNECT; return -1; }
    sp_set_nosigpipe(t, fd);

    int rc = sp_connect(t, fd, &t->addrs[idx]);
    if (rc < 0 && sp_io_status(t) != KL_IO_PENDING) {
        sp_close(t, fd);
        *out_err = (int)KL_ERR_CONNECT;
        return -1;   /* hard local failure: advance to the next address */
    }
    if (kl_watcher_add(&t->ev, fd, KL_EVENT_WRITE, co_connect_watcher, t) != 0) {
        sp_close(t, fd);
        *out_err = (int)KL_ERR_CONNECT;
        return -1;
    }
    t->attempt_fd[idx]    = fd;
    t->attempt_armed[idx] = 1;

    if (rc == 0) {   /* connected immediately (loopback often does) */
        kl_watcher_del(&t->ev, fd);
        t->attempt_armed[idx] = 0;
        t->attempt_fd[idx]    = KL_INVALID_SOCKET;
        kl_connect_op_on_attempt_connected(&t->connect_op, idx, fd);
    }
    return 0;
}

static void co_connect_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    PgTransport *t = user_data;
    (void)ready;
    int idx = -1;
    for (int i = 0; i < t->naddrs; i++)
        if (t->attempt_fd[i] == fd) { idx = i; break; }
    if (idx < 0) return;

    int soerr = 0;
    int gsrc = sp_get_so_error(t, fd, &soerr);
    kl_watcher_del(&t->ev, fd);
    t->attempt_armed[idx] = 0;
    if (gsrc == 0 && soerr == 0) {
        t->attempt_fd[idx] = KL_INVALID_SOCKET;   /* ownership moves to the op */
        kl_connect_op_on_attempt_connected(&t->connect_op, idx, fd);
    } else {
        sp_close(t, fd);
        t->attempt_fd[idx] = KL_INVALID_SOCKET;
        kl_connect_op_on_attempt_failed(&t->connect_op, idx, (int)KL_ERR_CONNECT);
    }
}

static void co_cancel_attempt(void *ctx, int idx)
{
    PgTransport *t = ctx;
    if (idx >= 0 && idx < t->naddrs && kl_handle_valid(t->attempt_fd[idx])) {
        if (t->attempt_armed[idx]) {
            kl_watcher_del(&t->ev, t->attempt_fd[idx]);
            t->attempt_armed[idx] = 0;
        }
        sp_close(t, t->attempt_fd[idx]);
        t->attempt_fd[idx] = KL_INVALID_SOCKET;
    }
    kl_connect_op_on_attempt_failed(&t->connect_op, idx, (int)KL_ERR_CONNECT);
}

static void co_dispose_fd(void *ctx, KlSocketHandle fd)
{
    PgTransport *t = ctx;
    if (kl_handle_valid(fd))
        sp_close(t, fd);
}

static void co_on_deadline_fired(void *user_data)
{
    PgTransport *t = user_data;
    t->deadline_timer = -1;
    t->deadline_fired = 1;
    kl_connect_op_on_deadline(&t->connect_op, (int)KL_ERR_TIMEOUT);
}

/* ── RFC 8305 Connection Attempt Delay (Happy Eyeballs stagger) ─────────────
 * arm_delay schedules a ~250 ms timer while an attempt is in flight; on fire it
 * tells the op to start the NEXT address (racing) rather than waiting for the
 * earlier attempt to fail. cancel_delay retires the timer synchronously (the
 * KlConnectOp contract requires it when arm_delay is set). */
static void co_on_delay_fired(void *user_data)
{
    PgTransport *t = user_data;
    t->delay_timer = -1;
    kl_connect_op_on_delay(&t->connect_op);
}
static int co_arm_delay(void *ctx)
{
    PgTransport *t = ctx;
    t->delay_timer = kl_timer_add(&t->ev, PG_CONNECT_ATTEMPT_DELAY_MS,
                                  co_on_delay_fired, t);
    if (t->delay_timer < 0)
        return -1;   /* arm failed: the machine fast-starts the next address */
    return 0;
}
static void co_cancel_delay(void *ctx)
{
    PgTransport *t = ctx;
    if (t->delay_timer >= 0) {
        kl_timer_cancel(&t->ev, t->delay_timer);
        t->delay_timer = -1;
    }
}

/* The overall connect deadline bounds TCP establishment only (design D3). It is
 * armed with the caller's remaining budget once, at start; the stagger timer does
 * not refresh it. When timeout_ms <= 0 the transport leaves arm_deadline OUT of
 * the hooks table entirely (below), so the op has no deadline (unbounded). */
static int co_arm_deadline(void *ctx, int *out_err)
{
    PgTransport *t = ctx;
    uint64_t now = kl_monotonic_ms();
    uint64_t delay = t->deadline_ms > now ? t->deadline_ms - now : 0;
    t->deadline_timer = kl_timer_add(&t->ev, delay, co_on_deadline_fired, t);
    if (t->deadline_timer < 0) { *out_err = (int)KL_ERR_TIMEOUT; return -1; }
    return 0;
}
static void co_cancel_deadline(void *ctx)
{
    PgTransport *t = ctx;
    if (t->deadline_timer >= 0) {
        kl_timer_cancel(&t->ev, t->deadline_timer);
        t->deadline_timer = -1;
    }
}

static void co_on_done(void *ctx, KlConnectResult result, KlSocketHandle fd, int error)
{
    PgTransport *t = ctx;
    t->connect_done = 1;
    t->connect_result = result;
    t->connect_error = error;
    if (result == KL_CONNECT_SUCCESS) {
        /* The winning fd is non-blocking (created that way for the race). Restore
         * blocking so every subsequent recv / send is plain blocking I/O with no
         * event loop (design D1). set_blocking is a validated-present required op;
         * a failure retires the fd + marks the op failed for the pump. */
        if (sp_set_blocking(t, fd) < 0) {
            t->connect_result = KL_CONNECT_FAILED;
            sp_close(t, fd);
            t->fd = KL_INVALID_SOCKET;
        } else {
            t->fd = fd;
        }
    }
}

/* Two hook tables: one WITH the deadline hooks (timeout_ms > 0), one WITHOUT (an
 * unbounded connect: no arm_deadline, so no deadline timer). The connect / delay
 * / dispose hooks are shared. on_detach is omitted: detachment is observed via
 * kl_connect_op_is_detached (no per-transport flag needed). */
static const KlConnectOpHooks PG_HOOKS_DEADLINE = {
    .start_resolve   = co_start_resolve,
    .cancel_resolve  = NULL,
    .start_attempt   = co_start_attempt,
    .cancel_attempt  = co_cancel_attempt,
    .dispose_fd      = co_dispose_fd,
    .arm_delay       = co_arm_delay,
    .cancel_delay    = co_cancel_delay,
    .arm_deadline    = co_arm_deadline,
    .cancel_deadline = co_cancel_deadline,
    .on_done         = co_on_done,
    .on_detach       = NULL,
};
static const KlConnectOpHooks PG_HOOKS_NO_DEADLINE = {
    .start_resolve   = co_start_resolve,
    .cancel_resolve  = NULL,
    .start_attempt   = co_start_attempt,
    .cancel_attempt  = co_cancel_attempt,
    .dispose_fd      = co_dispose_fd,
    .arm_delay       = co_arm_delay,
    .cancel_delay    = co_cancel_delay,
    .arm_deadline    = NULL,
    .cancel_deadline = NULL,
    .on_done         = co_on_done,
    .on_detach       = NULL,
};

/* ────────────────────────────────────────────────────────────────────────────
 * Event-loop pumps. The private KlEventCtx drives ONLY the connect phase.
 * ────────────────────────────────────────────────────────────────────────── */

static int done_connect(PgTransport *t)
{ return t->connect_done; }

static int done_connect_detached(PgTransport *t)
{
#ifdef HL_PG_TEST_HOOKS
    if (pg_test_force_no_detach) return 0;
#endif
    return kl_connect_op_is_detached(&t->connect_op);
}

static void pump_checkpoint(PgTransport *t)
{
#ifdef HL_PG_TEST_HOOKS
    if (pg_test_checkpoint)
        pg_test_checkpoint(t, pg_test_checkpoint_seq++);
#else
    (void)t;
#endif
}

/* Pump the connect op to its terminal (done_connect). When t->deadline_ms is set
 * (timeout_ms > 0) the loop is bounded by that single absolute connect ceiling;
 * when it is 0 (timeout_ms <= 0) the connect is TRULY UNBOUNDED - the loop pumps
 * in bounded PG_PUMP_STEP_MS increments with NO total deadline, until the op
 * completes (the select(..., NULL) contract, no hidden ceiling). Returns 0 when
 * done, -1 on a deadline expiry or an event-loop error. */
static int pump_connect(PgTransport *t)
{
    uint64_t dl = t->deadline_ms;   /* 0 == unbounded */
    for (;;) {
        pump_checkpoint(t);
        if (done_connect(t)) return 0;
        if (dl && kl_monotonic_ms() >= dl) return -1;
        if (kl_event_ctx_run(&t->ev, 64, PG_PUMP_STEP_MS) < 0) return -1;
        if (done_connect(t)) return 0;
        if (dl && kl_monotonic_ms() >= dl) return -1;
    }
}

/* Confirm the op has detached, ITERATION-bounded (a teardown safety bound, not
 * the connect deadline). Returns 0 detached, -1 if not within PG_DETACH_MAX_STEPS.
 * After kl_connect_op_cancel a well-behaved op retires everything and detaches
 * synchronously (or via queued work), so this returns at step 0-1. The pump is
 * NON-BLOCKING (timeout 0): detachment is never fd-readiness-driven here, so a
 * blocking wait would only stall the pathological non-detachment case; a bounded
 * non-blocking spin declares it promptly instead. */
static int pump_detach(PgTransport *t)
{
    for (int i = 0; i < PG_DETACH_MAX_STEPS; i++) {
        if (done_connect_detached(t)) return 0;
        if (kl_event_ctx_run(&t->ev, 64, 0 /* non-blocking poll */) < 0) return -1;
        if (done_connect_detached(t)) return 0;
    }
    return -1;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Connect-time teardown. Cancels a still-live op and confirms detachment before
 * retiring embedded storage (frozen rule 4). Returns 0 when the transport is fully
 * quiesced (event ctx freed, no live op) and the heap block may be freed; returns
 * -1 iff the op will NOT confirm detachment - in which case NOTHING is freed (the
 * live op still references its storage) and the caller must preserve or leak the
 * whole allocation.
 * ────────────────────────────────────────────────────────────────────────── */
static int connect_teardown(PgTransport *t)
{
    if (t->connect_started && !kl_connect_op_is_detached(&t->connect_op)) {
        kl_connect_op_cancel(&t->connect_op);
        if (pump_detach(t) != 0 || !kl_connect_op_is_detached(&t->connect_op)) {
            log_error("pg: connect op did not confirm detachment; preserving the "
                      "whole transport allocation (a live op still references it)");
            return -1;   /* preserve: do NOT free the event ctx / the block */
        }
    }

    /* Retire the winning fd + any escaped racing fds + timers. The op's cancel
     * path normally retires racing attempt fds already, so these find none. */
    if (kl_handle_valid(t->fd)) {
        sp_close(t, t->fd);
        t->fd = KL_INVALID_SOCKET;
    }
    for (int i = 0; i < KL_CONNECT_MAX_ADDRS; i++) {
        if (kl_handle_valid(t->attempt_fd[i])) {
            if (t->attempt_armed[i])
                kl_watcher_del(&t->ev, t->attempt_fd[i]);
            sp_close(t, t->attempt_fd[i]);
            t->attempt_fd[i] = KL_INVALID_SOCKET;
        }
    }
    if (t->delay_timer >= 0) {
        kl_timer_cancel(&t->ev, t->delay_timer);
        t->delay_timer = -1;
    }
    if (t->deadline_timer >= 0) {
        kl_timer_cancel(&t->ev, t->deadline_timer);
        t->deadline_timer = -1;
    }
    if (t->ev_ready) {
        kl_event_ctx_free(&t->ev);
        t->ev_ready = 0;
    }
    return 0;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API.
 * ────────────────────────────────────────────────────────────────────────── */

static PgTransport *transport_new(const KlSocketProvider *sp)
{
    PgTransport *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->sp = sp;
    t->fd = KL_INVALID_SOCKET;
    t->deadline_timer = -1;
    t->delay_timer = -1;
    t->connect_result = KL_CONNECT_CANCELLED;
    for (int i = 0; i < KL_CONNECT_MAX_ADDRS; i++)
        t->attempt_fd[i] = KL_INVALID_SOCKET;
    return t;
}

/* Fail a connect: tear down, then free the block iff fully quiesced; otherwise
 * leak the whole allocation intentionally (a live op still references it). Always
 * returns NULL. */
static PgTransport *connect_fail(PgTransport *t, char *errbuf, size_t errlen,
                                 const char *msg)
{
    set_err(errbuf, errlen, msg);
    if (connect_teardown(t) == 0)
        free(t);
    /* else: intentional safe leak - the op will not detach; never free under it. */
    return NULL;
}

PgTransport *hl_pg_transport_connect(const char *host, const char *port,
                                     int timeout_ms,
                                     const KlSocketProvider *sp_or_NULL,
                                     char *errbuf, size_t errlen)
{
    if (!host || !port) {
        set_err(errbuf, errlen, "invalid connect arguments");
        return NULL;
    }
    int port_n = atoi(port);
    if (port_n < 1 || port_n > 65535) {
        set_err(errbuf, errlen, "invalid port");
        return NULL;
    }
    const KlSocketProvider *sp = sp_or_NULL ? sp_or_NULL : default_provider();
    if (validate_provider(sp) != 0) {
        set_err(errbuf, errlen, "socket provider missing a required op or KL_SOCK_CAP_NATIVE_FD");
        return NULL;
    }

    PgTransport *t = transport_new(sp);
    if (!t) { set_err(errbuf, errlen, "out of memory"); return NULL; }
    t->alloc = kl_allocator_default();

    if (kl_event_ctx_init(&t->ev, &t->alloc) != 0) {
        set_err(errbuf, errlen, "failed to init event context");
        free(t);
        return NULL;
    }
    t->ev_ready = 1;

    /* Sandbox-compatible blocking system resolve (or IP-literal fast path),
     * inline before start. */
    t->naddrs = resolve_addrs(t, host, port_n);
    if (t->naddrs < 1)
        return connect_fail(t, errbuf, errlen, "could not resolve host");

    /* Freeze the connect deadline AFTER the blocking resolve (DNS is outside the
     * TCP-establishment bound, design D3). timeout_ms <= 0 leaves deadline_ms 0
     * (unbounded) and picks the no-deadline hook table. */
    const KlConnectOpHooks *hooks;
    if (timeout_ms > 0) {
        t->deadline_ms = kl_monotonic_ms() + (uint64_t)timeout_ms;
        hooks = &PG_HOOKS_DEADLINE;
    } else {
        hooks = &PG_HOOKS_NO_DEADLINE;
    }

    if (kl_connect_op_init(&t->connect_op, hooks, t) != 0)
        return connect_fail(t, errbuf, errlen, "failed to init connect op");
    if (kl_connect_op_start(&t->connect_op) != 0)
        return connect_fail(t, errbuf, errlen, "failed to start connect op");
    t->connect_started = 1;

    if (pump_connect(t) != 0 ||
        t->connect_result != KL_CONNECT_SUCCESS || !kl_handle_valid(t->fd))
        return connect_fail(t, errbuf, errlen, "connection failed");

    /* Connect succeeded: the winning fd is already blocking and handed to us, but
     * the op still needs confirmed detachment before its embedded storage is
     * reusable. Confirm detachment, then free the connect-only event ctx: post-
     * connect I/O is pure blocking, no loop. If it will not detach, fail closed
     * (retire the fd) and leak the whole allocation - never free under a live op. */
    if (!kl_connect_op_is_detached(&t->connect_op)) {
        if (pump_detach(t) != 0 || !kl_connect_op_is_detached(&t->connect_op)) {
            set_err(errbuf, errlen, "connect op did not detach");
            if (kl_handle_valid(t->fd)) { sp_close(t, t->fd); t->fd = KL_INVALID_SOCKET; }
            log_error("pg: connect op did not detach after success; leaking the "
                      "transport allocation (a live op still references it)");
            return NULL;   /* intentional safe leak */
        }
    }
    if (t->delay_timer >= 0) {
        kl_timer_cancel(&t->ev, t->delay_timer);
        t->delay_timer = -1;
    }
    if (t->deadline_timer >= 0) {
        kl_timer_cancel(&t->ev, t->deadline_timer);
        t->deadline_timer = -1;
    }
    kl_event_ctx_free(&t->ev);
    t->ev_ready = 0;
    return t;
}

PgTransport *hl_pg_transport_adopt(int fd, const KlSocketProvider *sp_or_NULL,
                                   char *errbuf, size_t errlen)
{
    if (fd < 0) {
        set_err(errbuf, errlen, "invalid descriptor");
        return NULL;
    }
    const KlSocketProvider *sp = sp_or_NULL ? sp_or_NULL : default_provider();
    if (validate_provider(sp) != 0) {
        set_err(errbuf, errlen, "socket provider missing a required op or KL_SOCK_CAP_NATIVE_FD");
        return NULL;
    }
    PgTransport *t = transport_new(sp);
    if (!t) { set_err(errbuf, errlen, "out of memory"); return NULL; }
    t->fd = (KlSocketHandle)fd;
    /* SIGPIPE suppression parity: every subsequent write goes through
     * hl_pg_transport_send (nosignal flag) plus this per-fd SO_NOSIGPIPE where the
     * platform offers it. No event ctx, no resolution, no racing (Amendment 2). */
    sp_set_nosigpipe(t, t->fd);
    return t;
}

int hl_pg_transport_attach_tls(PgTransport *t, struct HlTlsClient *tls)
{
    if (!t || !tls)                    return -1;  /* one-shot + non-NULL required */
    if (!kl_handle_valid(t->fd))       return -1;  /* require a live descriptor    */
    if (t->tls)                        return -1;  /* reject a second attachment   */
    t->tls = tls;
    return 0;
}

ssize_t hl_pg_transport_send(PgTransport *t, const uint8_t *buf, size_t len)
{
    if (!t || !kl_handle_valid(t->fd))
        return -1;
    if (t->tls)
        return hl_tls_client_write((int)t->fd, t->tls, buf, len);
    /* EINTR retry classified via the provider's io_status (hosted-errno fallback
     * when absent). SIGPIPE suppression is the fd's SO_NOSIGPIPE + the provider's
     * internal nosignal send flag. */
    kl_ssize_t n;
    do { n = sp_send(t, t->fd, buf, len); }
    while (n < 0 && sp_io_status(t) == KL_IO_INTERRUPTED);
    return (ssize_t)n;
}

ssize_t hl_pg_transport_recv(PgTransport *t, uint8_t *buf, size_t len)
{
    if (!t || !kl_handle_valid(t->fd))
        return -1;
    if (t->tls)
        return hl_tls_client_read((int)t->fd, t->tls, buf, len);
    kl_ssize_t n;
    do { n = sp_recv(t, t->fd, buf, len); }
    while (n < 0 && sp_io_status(t) == KL_IO_INTERRUPTED);
    return (ssize_t)n;
}

int hl_pg_transport_send_all(PgTransport *t, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = hl_pg_transport_send(t, buf + sent, len - sent);
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

int hl_pg_transport_fd(const PgTransport *t)
{
    if (!t || !kl_handle_valid(t->fd))
        return -1;
    return (int)t->fd;
}

int hl_pg_transport_close(PgTransport *t)
{
    if (!t)
        return 0;

    /* One close path: TLS shutdown, then TLS free, then provider close (design
     * Amendment 1). TLS teardown is idempotent (t->tls cleared), so a retry after
     * a preserved non-detachment does not repeat it. */
    if (t->tls) {
        if (kl_handle_valid(t->fd))
            hl_tls_client_shutdown((int)t->fd, t->tls);
        hl_tls_client_free(t->tls);
        t->tls = NULL;
    }

    /* A raced connection whose event ctx is still live (a caller closing without a
     * successful connect, or retrying after a preserved non-detachment): drive the
     * op to detachment. On non-detachment PRESERVE the whole allocation (do not
     * free) and report -1 so the owner can retry or intentionally leak. */
    if (t->connect_started && t->ev_ready) {
        if (connect_teardown(t) != 0)
            return -1;   /* preserved: caller retries hl_pg_transport_close or leaks */
        free(t);
        return 0;
    }

    /* Common paths: a successful connect already freed the ev ctx, or an adopted
     * fd never had one. Plain provider close of the descriptor, then free. */
    if (kl_handle_valid(t->fd)) {
        sp_close(t, t->fd);
        t->fd = KL_INVALID_SOCKET;
    }
    free(t);
    return 0;
}
