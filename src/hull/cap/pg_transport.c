/*
 * pg_transport.c: PostgreSQL byte transport over Keel v3 public primitives.
 *
 * See include/hull/cap/pg_transport.h for the ownership split and the contract.
 *
 * This composes Keel's PUBLIC transport surface for the CONNECT phase only:
 * KlConnectOp (resolve + Happy-Eyeballs racing connect) driven on a PRIVATE,
 * operation-local KlEventCtx, plus the POSIX socket provider's public ops table.
 * The connect machinery is the reviewed cap/smtp_transport.c pattern (resolve
 * adapter + KlConnectOp hooks + private KlEventCtx pump + socket-provider
 * wrappers), MINUS the KlStream: a synchronous PostgreSQL connection needs no
 * queued-write / read pause-resume / graceful-close machinery. After the race is
 * won the winning fd is set_blocking()'d and every subsequent byte is plain
 * blocking recv / send with no event loop (design D1).
 *
 * TLS stays the shared hl_tls_client_* helpers layered over the blocking fd
 * (design D2). This transport only STORES the optional attached HlTlsClient and
 * routes send/recv through it; the SSLRequest negotiation + handshake belong to
 * the consumer (pg_conn.c), which hands the session here via attach_tls.
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
#include <stdlib.h>   /* atoi */
#include <string.h>

#include "log.h"

/* SIGPIPE suppression on send has parity with pg_conn.c: the winning / adopted fd
 * gets SO_NOSIGPIPE via the provider's set_nosigpipe hook where the platform
 * offers it, and the POSIX provider's send applies MSG_NOSIGNAL internally
 * (vendor/keel/src/socket_posix.c), so hl_pg_transport_send never emits SIGPIPE. */

/* Default connect budget when the caller passes timeout_ms <= 0 is NONE: the
 * transport arms no deadline and the connect is unbounded, preserving
 * pg_connect's prior select(NULL) behavior. But the pump still needs a per-stage
 * step budget to bound one kl_event_ctx_run wait; a completed op / detachment is
 * the real terminal, so a generous fallback only caps how long one pump loop
 * blocks with no readiness. */
#define PG_PUMP_STEP_MS   50
#define PG_PUMP_FALLBACK_MS  30000   /* stage cap when timeout_ms <= 0 (unbounded op) */

/* RFC 8305 Connection Attempt Delay: wait before starting the next address while
 * an earlier attempt is still in flight (the Happy-Eyeballs stagger). */
#define PG_CONNECT_ATTEMPT_DELAY_MS  250

/* ────────────────────────────────────────────────────────────────────────────
 * Socket provider: an immutable process-wide ops table. We call its PUBLIC ops
 * directly, reading errno for would-block / EINPROGRESS classification (the
 * defined hosted-errno fallback on the POSIX provider).
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
/* Test-only fault-injection provider (compiled ONLY under -DHL_PG_TEST_HOOKS,
 * ABSENT from the production object). When non-NULL it REPLACES the default POSIX
 * provider for every sp_* op, so a test can drive connect attempts to
 * pending / succeed / fail per address and observe attempt order + timestamps. It
 * is a COHERENT socket seam: it hands the connect op real fds the event loop can
 * watch, so the Happy-Eyeballs stagger timer and the connect-deadline timer fire
 * on the same real clock as production (never a divergent virtual clock). */
const KlSocketProvider *pg_test_socket_provider;

/* When non-NULL, fills t->addrs[] in EXACT injected order and returns the count
 * (value-copied into the transport-owned array, so no dangling), replacing the
 * blocking getaddrinfo. */
int (*pg_test_resolve)(PgTransport *t, const char *host, int port);

/* Fires at the TOP of every pump check with the transport and a monotonically-
 * increasing checkpoint index, so a test can observe pump progress without
 * diverging from Keel's clock. */
void (*pg_test_checkpoint)(PgTransport *t, unsigned idx);
static unsigned pg_test_checkpoint_seq;
#endif

/* The active provider: a test-supplied one when set, else the transport's own
 * borrowed reference (default POSIX). t->sp is set at connect / adopt time; the
 * connect hooks run while t->sp is live. */
static const KlSocketProvider *active_sp(const PgTransport *t)
{
#ifdef HL_PG_TEST_HOOKS
    if (pg_test_socket_provider) return pg_test_socket_provider;
#endif
    return t->sp ? t->sp : default_provider();
}

static KlSocketHandle sp_socket(const PgTransport *t, int domain, int type, int protocol)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->socket(sp->context, domain, type, protocol); }
static int sp_set_nonblocking(const PgTransport *t, KlSocketHandle fd)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->set_nonblocking(sp->context, fd); }
static int sp_set_blocking(const PgTransport *t, KlSocketHandle fd)
{ const KlSocketProvider *sp = active_sp(t); return sp->ops->set_blocking ? sp->ops->set_blocking(sp->context, fd) : 0; }
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
    int ce = errno;
    if (rc < 0 && ce != EINPROGRESS) {
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
         * event loop (design D1). A set_blocking failure retires the fd + marks
         * the op failed for the pump. */
        if (sp_set_blocking(t, fd) < 0) {
            t->connect_result = KL_CONNECT_FAILED;
            sp_close(t, fd);
            t->fd = KL_INVALID_SOCKET;
        } else {
            t->fd = fd;
        }
    }
}

static void co_on_detach(void *ctx)
{
    PgTransport *t = ctx;
    t->connect_detached = 1;
}

/* Two hook tables: one WITH the deadline hooks (timeout_ms > 0), one WITHOUT (an
 * unbounded connect: no arm_deadline, so no deadline timer). The connect / delay
 * / dispose hooks are shared. */
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
    .on_detach       = co_on_detach,
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
    .on_detach       = co_on_detach,
};

/* ────────────────────────────────────────────────────────────────────────────
 * Event-loop pump. Runs the private KlEventCtx until @p done() or the stage
 * deadline elapses so a dead peer cannot hang the synchronous caller.
 *
 * The deadline is a MONOTONIC ABSOLUTE instant (kl_monotonic_ms() + budget), NOT
 * an accumulation of assumed tick durations. Each kl_event_ctx_run waits up to
 * step_ms for readiness; the check runs BEFORE and AFTER each step, so a
 * condition that arrives during the step takes effect within one step (~50 ms).
 * ────────────────────────────────────────────────────────────────────────── */

typedef int (*DonePred)(PgTransport *t);

static int pump_check(PgTransport *t, DonePred done, uint64_t deadline_ms)
{
#ifdef HL_PG_TEST_HOOKS
    if (pg_test_checkpoint)
        pg_test_checkpoint(t, pg_test_checkpoint_seq++);
#endif
    if (done(t))
        return 1;
    if (deadline_ms && kl_monotonic_ms() >= deadline_ms)
        return -1;
    return 0;
}

static int pump_until_abs(PgTransport *t, DonePred done, uint64_t deadline_ms)
{
    for (;;) {
        int c = pump_check(t, done, deadline_ms);
        if (c) return c > 0 ? 0 : -1;
        if (kl_event_ctx_run(&t->ev, 64, PG_PUMP_STEP_MS) < 0)
            return -1;
        c = pump_check(t, done, deadline_ms);
        if (c) return c > 0 ? 0 : -1;
    }
}

/* One pump stage. When the op is bounded (t->deadline_ms != 0) the stage shares
 * that single connect ceiling; when unbounded, one pump loop is capped by the
 * fallback so a wedged loop cannot block forever, but a completed op / detachment
 * is the real terminal (done predicate). */
static int pump_until(PgTransport *t, DonePred done)
{
    uint64_t dl = t->deadline_ms;
    if (!dl)
        dl = kl_monotonic_ms() + PG_PUMP_FALLBACK_MS;
    return pump_until_abs(t, done, dl);
}

/* Predicates. */
static int done_connect(PgTransport *t)
{ return t->connect_done; }
static int done_connect_detached(PgTransport *t)
{ return kl_connect_op_is_detached(&t->connect_op); }

/* ────────────────────────────────────────────────────────────────────────────
 * Connect-time teardown: cancel a still-live op and pump to confirmed
 * detachment before releasing embedded storage (frozen rule 4). Also retire any
 * winning fd + racing fds + timers. Leaves the event ctx freed. Used on both the
 * connect-failure path and by hl_pg_transport_close for a raced connection.
 * ────────────────────────────────────────────────────────────────────────── */
static void connect_teardown(PgTransport *t)
{
    /* If the op was started and has NOT yet reached confirmed detachment, cancel
     * it (retiring every racing fd via cancel_attempt / dispose_fd and both timers
     * via cancel_delay / cancel_deadline) and pump to detachment. Fail LOUDLY if
     * it will not detach within the bound rather than free storage a live op still
     * references. */
    if (t->connect_started && !kl_connect_op_is_detached(&t->connect_op)) {
        kl_connect_op_cancel(&t->connect_op);
        /* This teardown ignores the (possibly-exhausted) deadline: clear it so an
         * expired deadline cannot clip the detachment confirm. */
        t->deadline_ms = 0;
        if (pump_until(t, done_connect_detached) != 0 ||
            !kl_connect_op_is_detached(&t->connect_op)) {
            log_error("pg: connect op did not detach within the bound; leaking "
                      "transport descriptors rather than freeing a live op");
            /* Do not free the event ctx: the live op still references it. */
            return;
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
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API.
 * ────────────────────────────────────────────────────────────────────────── */

static void transport_zero(PgTransport *t)
{
    memset(t, 0, sizeof *t);
    t->fd = KL_INVALID_SOCKET;
    t->deadline_timer = -1;
    t->delay_timer = -1;
    t->connect_result = KL_CONNECT_CANCELLED;
    for (int i = 0; i < KL_CONNECT_MAX_ADDRS; i++)
        t->attempt_fd[i] = KL_INVALID_SOCKET;
}

int hl_pg_transport_connect(PgTransport *t, const char *host, const char *port,
                            int timeout_ms, const KlSocketProvider *sp_or_NULL,
                            char *errbuf, size_t errlen)
{
    if (!t || !host || !port) {
        set_err(errbuf, errlen, "invalid connect arguments");
        return -1;
    }
    int port_n = atoi(port);
    if (port_n < 1 || port_n > 65535) {
        set_err(errbuf, errlen, "invalid port");
        return -1;
    }

    transport_zero(t);
    t->sp = sp_or_NULL ? sp_or_NULL : default_provider();
    t->alloc = kl_allocator_default();
    /* timeout_ms > 0 bounds TCP establishment (design D3); <= 0 = unbounded (no
     * deadline hooks, deadline_ms 0). */
    t->deadline_ms = 0;

    if (kl_event_ctx_init(&t->ev, &t->alloc) != 0) {
        set_err(errbuf, errlen, "failed to init event context");
        return -1;
    }
    t->ev_ready = 1;

    /* Sandbox-compatible blocking system resolve (or IP-literal fast path),
     * inline before start. */
    t->naddrs = resolve_addrs(t, host, port_n);
    if (t->naddrs < 1) {
        set_err(errbuf, errlen, "could not resolve host");
        connect_teardown(t);
        return -1;
    }

    /* Freeze the connect deadline AFTER the blocking resolve (DNS is outside the
     * TCP-establishment bound, design D3). */
    const KlConnectOpHooks *hooks;
    if (timeout_ms > 0) {
        t->deadline_ms = kl_monotonic_ms() + (uint64_t)timeout_ms;
        hooks = &PG_HOOKS_DEADLINE;
    } else {
        hooks = &PG_HOOKS_NO_DEADLINE;   /* unbounded connect: no deadline armed */
    }

    if (kl_connect_op_init(&t->connect_op, hooks, t) != 0) {
        set_err(errbuf, errlen, "failed to init connect op");
        connect_teardown(t);
        return -1;
    }
    if (kl_connect_op_start(&t->connect_op) != 0) {
        set_err(errbuf, errlen, "failed to start connect op");
        connect_teardown(t);
        return -1;
    }
    t->connect_started = 1;

    if (pump_until(t, done_connect) != 0 ||
        t->connect_result != KL_CONNECT_SUCCESS || !kl_handle_valid(t->fd)) {
        set_err(errbuf, errlen, "connection failed");
        connect_teardown(t);
        return -1;
    }

    /* Connect succeeded: the op still needs confirmed detachment before its
     * embedded storage is reusable, but the winning fd is already handed to us and
     * blocking. Pump the op to detachment (it retires its own timers), then free
     * the connect-only event ctx: post-connect I/O is pure blocking, no loop. */
    if (!kl_connect_op_is_detached(&t->connect_op)) {
        if (pump_until(t, done_connect_detached) != 0 ||
            !kl_connect_op_is_detached(&t->connect_op)) {
            /* The op will not detach: we cannot free the event ctx it references.
             * The fd is live and usable, but leaving a live op wedged is a hard
             * failure - fail closed, retire the fd, and report. */
            set_err(errbuf, errlen, "connect op did not detach");
            if (kl_handle_valid(t->fd)) { sp_close(t, t->fd); t->fd = KL_INVALID_SOCKET; }
            log_error("pg: connect op did not detach after success; failing closed");
            return -1;
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

int hl_pg_transport_adopt(PgTransport *t, int fd,
                          const KlSocketProvider *sp_or_NULL)
{
    if (!t || fd < 0)
        return -1;
    transport_zero(t);
    t->sp = sp_or_NULL ? sp_or_NULL : default_provider();
    t->fd = (KlSocketHandle)fd;
    /* SIGPIPE suppression parity: adopt() takes a connected blocking fd and every
     * subsequent write goes through hl_pg_transport_send (nosignal flag) plus
     * this per-fd SO_NOSIGPIPE where the platform offers it. No event ctx, no
     * resolution, no racing (design Amendment 2). */
    sp_set_nosigpipe(t, t->fd);
    return 0;
}

void hl_pg_transport_attach_tls(PgTransport *t, struct HlTlsClient *tls)
{
    if (!t)
        return;
    t->tls = tls;
}

ssize_t hl_pg_transport_send(PgTransport *t, const uint8_t *buf, size_t len)
{
    if (!t || !kl_handle_valid(t->fd))
        return -1;
    if (t->tls)
        return hl_tls_client_write((int)t->fd, t->tls, buf, len);
    /* SIGPIPE suppression + EINTR retry: the POSIX provider's send already applies
     * MSG_NOSIGNAL and retries EINTR internally (socket_posix.c), and we set
     * SO_NOSIGPIPE on the fd where the platform offers it (sp_set_nosigpipe at
     * adopt / attempt). The extra EINTR guard here is defense in depth for a
     * provider whose send does not retry (e.g. the test mock). */
    kl_ssize_t n;
    do { n = sp_send(t, t->fd, buf, len); }
    while (n < 0 && errno == EINTR);
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
    while (n < 0 && errno == EINTR);
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

void hl_pg_transport_close(PgTransport *t)
{
    if (!t || t->closed)
        return;
    t->closed = 1;

    /* One close path: TLS shutdown, then TLS free, then provider close (design
     * Amendment 1). */
    if (t->tls) {
        if (kl_handle_valid(t->fd))
            hl_tls_client_shutdown((int)t->fd, t->tls);
        hl_tls_client_free(t->tls);
        t->tls = NULL;
    }

    /* A raced connection that never freed its event ctx (an error path returned
     * before detachment, or a caller closes without a successful connect): drive
     * the op to detachment and retire storage. On the common paths (successful
     * connect freed the ev ctx, or an adopted fd never had one) this is a plain
     * fd close. */
    if (t->connect_started && t->ev_ready) {
        connect_teardown(t);   /* cancels + detaches + closes fd + frees ev ctx */
        return;
    }

    if (kl_handle_valid(t->fd)) {
        sp_close(t, t->fd);
        t->fd = KL_INVALID_SOCKET;
    }
}
