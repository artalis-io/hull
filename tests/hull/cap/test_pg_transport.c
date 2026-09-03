/*
 * test_pg_transport.c - Focused tests for the PostgreSQL-over-Keel-v3 transport
 * (Checkpoint 2 of docs/pg_keel_transport_slice3.md).
 *
 * These drive the transport's connect machinery + blocking I/O + lifecycle
 * through the -DHL_PG_TEST_HOOKS seam, WITHOUT any network or DNS. Coverage:
 *
 *   - pending -> succeed, and immediate connect() success (rc == 0);
 *   - RFC 8305 stagger: the second address wins;
 *   - every-address failure reaches confirmed detachment (no fd leak);
 *   - the connect deadline fires and reaches confirmed detachment;
 *   - truly unbounded connect (timeout_ms <= 0) with NO hidden ceiling, driven to
 *     completion by the test (controlled), not by waiting;
 *   - provider io_status classification with an IRRELEVANT hosted errno;
 *   - a provider missing a required op, or missing KL_SOCK_CAP_NATIVE_FD, fails
 *     closed;
 *   - set_blocking failure fails the connect closed;
 *   - the IP-literal fast path (kl_sockaddr_parse, no DNS callback);
 *   - partial send completes via send_all; partial recv returns the short count;
 *   - adopt(fd) routes I/O and closes the descriptor EXACTLY once;
 *   - TLS attach is one-shot + fallible (NULL / no-fd / double rejected);
 *   - forced non-detachment PRESERVES the whole allocation and close is retryable.
 *
 * The static helpers + the opaque struct are reached by direct-including the
 * capability .c; the Makefile rule for this binary EXCLUDES cap_pg_transport.o
 * from the common link (mirrors test_smtp_transport / test_pg_conn). This test
 * source is never in the shipped binary. Run under ASan/LSan + TSan in CI.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

/* Direct-source include: the static helpers (resolve_addrs, the connect hooks,
 * the pumps, connect_teardown) plus the opaque struct + public API are under
 * test. */
#include "../../../src/hull/cap/pg_transport.c"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <keel/sockaddr.h>

/* A watchdog so a wedged pump can never hang CI: SIGALRM aborts the test. Every
 * test that pumps arms alarm(5); a truly-unbounded loop with no completion would
 * trip it, so the unbounded test proves it completes via controlled completion,
 * NOT via a hidden ceiling. */
static void tp_watchdog_fired(int sig) { (void)sig; _exit(77); }
static void tp_arm_watchdog(void)
{ signal(SIGALRM, tp_watchdog_fired); alarm(5); }
static void tp_disarm_watchdog(void) { alarm(0); }

/* ════════════════════════════════════════════════════════════════════
 *  Fake socket provider. It hands the connect op REAL fds the event loop can
 *  watch (ONE clock domain): a "pending" address is a socketpair whose send
 *  buffer is filled so its fd is never write-ready; "succeed" leaves it writable
 *  and get_so_error reports 0; "fail" reports ECONNREFUSED; "immediate" makes
 *  connect() return 0. Close calls are counted per fd.
 * ════════════════════════════════════════════════════════════════════ */

enum { FK_PENDING = 0, FK_SUCCEED = 1, FK_FAIL = 2, FK_IMMEDIATE = 3 };

typedef struct { int used; int a; int b; int disp; int idx; } FkSlot;
static FkSlot g_fk[32];
static int    g_fk_disp[KL_CONNECT_MAX_ADDRS];
static int    g_fk_order[KL_CONNECT_MAX_ADDRS];
static int    g_fk_n;
static int    g_fk_succeeded_idx;
static int    g_fk_naddr;
static int    g_fk_blocking_set;
static int    g_fk_blocking_fail;    /* when 1, set_blocking returns -1        */
static int    g_fk_close_n;          /* total provider close() calls           */
static int    g_fk_bogus_errno;      /* when 1, connect leaves errno = EPERM   */
static KlIoStatus g_fk_io_status_val;/* value fk_io_status returns             */
static int    g_fk_send_eintr_once;  /* when 1, first send reports INTERRUPTED  */
static int    g_fk_send_max;         /* > 0 caps bytes per send (forces partial)*/
static int    g_fk_send_n;           /* count of send() calls (partial-write proof) */

static void fk_reset(void)
{
    memset(g_fk, 0, sizeof g_fk);
    memset(g_fk_disp, 0, sizeof g_fk_disp);
    memset(g_fk_order, 0, sizeof g_fk_order);
    g_fk_n = 0;
    g_fk_succeeded_idx = -1;
    g_fk_naddr = 0;
    g_fk_blocking_set = 0;
    g_fk_blocking_fail = 0;
    g_fk_close_n = 0;
    g_fk_bogus_errno = 0;
    g_fk_io_status_val = KL_IO_OK;
    g_fk_send_eintr_once = 0;
    g_fk_send_max = 0;
    g_fk_send_n = 0;
    /* The checkpoint counter is a process-global static in pg_transport.c; reset it
     * per test so a "complete at checkpoint N" hook fires deterministically (a
     * prior test's checkpoints must not advance it past N). Visible because this
     * test direct-includes the capability .c under -DHL_PG_TEST_HOOKS. */
    pg_test_checkpoint_seq = 0;
}

static FkSlot *fk_slot_for(int a)
{
    for (size_t i = 0; i < sizeof g_fk / sizeof g_fk[0]; i++)
        if (g_fk[i].used && g_fk[i].a == a) return &g_fk[i];
    return NULL;
}

static KlSocketHandle fk_socket(void *c, int d, int ty, int p)
{
    (void)c; (void)d; (void)ty; (void)p;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return KL_INVALID_SOCKET;
    int small = 2048;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    for (size_t i = 0; i < sizeof g_fk / sizeof g_fk[0]; i++)
        if (!g_fk[i].used) {
            g_fk[i].used = 1; g_fk[i].a = sv[0]; g_fk[i].b = sv[1];
            g_fk[i].disp = -1; g_fk[i].idx = -1;
            return (KlSocketHandle)sv[0];
        }
    close(sv[0]); close(sv[1]);
    return KL_INVALID_SOCKET;
}
static int  fk_set_nb(void *c, KlSocketHandle fd)
{ (void)c; int a = (int)fd; int fl = fcntl(a, F_GETFL, 0); return fcntl(a, F_SETFL, fl | O_NONBLOCK); }
static int  fk_set_blocking(void *c, KlSocketHandle fd)
{ (void)c; if (g_fk_blocking_fail) return -1; g_fk_blocking_set++;
  int a = (int)fd; int fl = fcntl(a, F_GETFL, 0); return fcntl(a, F_SETFL, fl & ~O_NONBLOCK); }
static int  fk_set_int(void *c, KlSocketHandle fd, int on) { (void)c; (void)fd; (void)on; return 0; }
static void fk_set_void(void *c, KlSocketHandle fd) { (void)c; (void)fd; }
static int  fk_get_local(void *c, KlSocketHandle fd, KlSockAddr *out)
{ (void)c; (void)fd; uint8_t ip[4] = {127,0,0,1}; return kl_sockaddr_from_ipv4(out, ip, 0); }

static int fk_connect(void *c, KlSocketHandle fd, const KlSockAddr *addr)
{
    (void)c;
    int a = (int)fd;
    int idx = (addr->addr_len == 4) ? (addr->u.ip[3] - 1) : 0;
    if (idx < 0 || idx >= KL_CONNECT_MAX_ADDRS) idx = 0;
    int disp = g_fk_disp[idx];
    if (g_fk_n < KL_CONNECT_MAX_ADDRS)
        g_fk_order[g_fk_n++] = idx;
    FkSlot *s = fk_slot_for(a);
    if (s) { s->disp = disp; s->idx = idx; }
    if (disp == FK_IMMEDIATE) { errno = 0; return 0; }   /* rc == 0 fast path */
    if (disp == FK_PENDING) {
        char buf[2048];
        memset(buf, 'x', sizeof buf);
        int fl = fcntl(a, F_GETFL, 0); fcntl(a, F_SETFL, fl | O_NONBLOCK);
        while (write(a, buf, sizeof buf) > 0) { }
    }
    errno = g_fk_bogus_errno ? EPERM : EINPROGRESS;
    return -1;
}
static int fk_get_so_error(void *c, KlSocketHandle fd, int *out)
{
    (void)c;
    FkSlot *s = fk_slot_for((int)fd);
    if (s && s->disp == FK_FAIL) { *out = ECONNREFUSED; }
    else { *out = 0; if (s) g_fk_succeeded_idx = s->idx; }
    return 0;
}
static kl_ssize_t fk_send(void *c, KlSocketHandle fd, const void *b, size_t n)
{
    (void)c;
    g_fk_send_n++;
    if (g_fk_send_eintr_once) { g_fk_send_eintr_once = 0; errno = 0; return -1; }
    if (g_fk_send_max > 0 && n > (size_t)g_fk_send_max) n = (size_t)g_fk_send_max;
    return send((int)fd, b, n, 0);
}
static kl_ssize_t fk_recv(void *c, KlSocketHandle fd, void *b, size_t n)
{ (void)c; return recv((int)fd, b, n, 0); }
static kl_ssize_t fk_recv_peek(void *c, KlSocketHandle fd, void *b, size_t n)
{ (void)c; return recv((int)fd, b, n, MSG_PEEK); }
static int fk_close(void *c, KlSocketHandle fd)
{
    (void)c; int a = (int)fd;
    g_fk_close_n++;
    FkSlot *s = fk_slot_for(a);
    if (s) { close(s->b); s->used = 0; }
    return close(a);
}
static KlIoStatus fk_io_status(void *c) { (void)c; return g_fk_io_status_val; }

/* Two op tables: FK_OPS has NO io_status (transport uses the hosted-errno
 * fallback); FK_OPS_IOS supplies io_status (transport uses it, never errno). */
#define FK_OPS_COMMON \
    .set_nonblocking = fk_set_nb, .set_blocking = fk_set_blocking, \
    .set_cloexec = fk_set_void, .set_nosigpipe = fk_set_void, \
    .set_reuseaddr = fk_set_int, .set_reuseport = fk_set_int, \
    .set_ipv6only = fk_set_int, .set_tcp_nodelay = fk_set_int, .set_cork = fk_set_int, \
    .socket = fk_socket, .connect = fk_connect, .close = fk_close, \
    .get_local_addr = fk_get_local, .get_so_error = fk_get_so_error, \
    .send = fk_send, .recv = fk_recv, .recv_peek = fk_recv_peek

static const KlSocketOps FK_OPS     = { FK_OPS_COMMON };
static const KlSocketOps FK_OPS_IOS = { FK_OPS_COMMON, .io_status = fk_io_status };
static const KlSocketProvider FK_PROVIDER =
    { .ops = &FK_OPS, .context = NULL, .capabilities = KL_SOCK_CAP_NATIVE_FD };
static const KlSocketProvider FK_PROVIDER_IOS =
    { .ops = &FK_OPS_IOS, .context = NULL, .capabilities = KL_SOCK_CAP_NATIVE_FD };

static int g_fk_resolve_called;
static int fk_resolve(PgTransport *t, const char *host, int port)
{
    (void)host;
    g_fk_resolve_called++;
    for (int i = 0; i < g_fk_naddr && i < KL_CONNECT_MAX_ADDRS; i++) {
        uint8_t ip[4] = { 10, 11, 12, (uint8_t)(i + 1) };
        if (kl_sockaddr_from_ipv4(&t->addrs[i], ip, (uint16_t)port) != 0) return i;
    }
    return g_fk_naddr;
}

static void seam_clear(void)
{
    pg_test_resolve         = NULL;
    pg_test_socket_provider = NULL;
    pg_test_checkpoint      = NULL;
    pg_test_force_no_detach = 0;
}

/* Proxy for "confirmed detachment retired everything": every mock socketpair was
 * closed, so no racing fd leaked. */
static int fk_all_fds_closed(void)
{
    for (size_t i = 0; i < sizeof g_fk / sizeof g_fk[0]; i++)
        if (g_fk[i].used) return 0;
    return 1;
}

/* ── pending -> succeed ─────────────────────────────────────────────── */
UTEST(pg_transport_connect, pending_then_succeed)
{
    fk_reset(); tp_arm_watchdog();
    g_fk_naddr = 1; g_fk_disp[0] = FK_SUCCEED;
    g_fk_resolve_called = 0;
    pg_test_resolve = fk_resolve; pg_test_socket_provider = &FK_PROVIDER;

    char err[128] = {0};
    PgTransport *t = hl_pg_transport_connect("db.example", "5432", 0, &FK_PROVIDER, err, sizeof err);
    ASSERT_TRUE(t != NULL);
    ASSERT_GE(g_fk_n, 1);
    ASSERT_EQ(g_fk_succeeded_idx, 0);
    ASSERT_GE(g_fk_blocking_set, 1);              /* the winner was set_blocking()'d */
    ASSERT_TRUE(hl_pg_transport_fd(t) >= 0);
    ASSERT_TRUE(kl_connect_op_is_detached(&t->connect_op));
    ASSERT_EQ(t->ev_ready, 0);                    /* connect-only ev ctx freed       */

    ASSERT_EQ(hl_pg_transport_close(t), 0);
    seam_clear(); tp_disarm_watchdog();
}

/* ── immediate connect() success (rc == 0), not EINPROGRESS ─────────── */
UTEST(pg_transport_connect, immediate_success_rc0)
{
    fk_reset(); tp_arm_watchdog();
    g_fk_naddr = 1; g_fk_disp[0] = FK_IMMEDIATE;
    pg_test_resolve = fk_resolve; pg_test_socket_provider = &FK_PROVIDER;

    PgTransport *t = hl_pg_transport_connect("h", "5432", 0, &FK_PROVIDER, NULL, 0);
    ASSERT_TRUE(t != NULL);
    ASSERT_GE(g_fk_blocking_set, 1);
    ASSERT_TRUE(hl_pg_transport_fd(t) >= 0);
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    seam_clear(); tp_disarm_watchdog();
}

/* ── RFC 8305 stagger: address 0 pends, the stagger starts address 1 which wins ─ */
UTEST(pg_transport_connect, stagger_second_address_wins)
{
    fk_reset(); tp_arm_watchdog();
    g_fk_naddr = 2; g_fk_disp[0] = FK_PENDING; g_fk_disp[1] = FK_SUCCEED;
    pg_test_resolve = fk_resolve; pg_test_socket_provider = &FK_PROVIDER;

    PgTransport *t = hl_pg_transport_connect("h", "5432", 0, &FK_PROVIDER, NULL, 0);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(g_fk_n, 2);
    ASSERT_EQ(g_fk_order[0], 0);
    ASSERT_EQ(g_fk_order[1], 1);
    ASSERT_EQ(g_fk_succeeded_idx, 1);
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    seam_clear(); tp_disarm_watchdog();
}

/* ── every address fails -> NULL + confirmed detachment (no fd leak) ── */
UTEST(pg_transport_connect, all_addresses_fail_detach)
{
    fk_reset(); tp_arm_watchdog();
    g_fk_naddr = 2; g_fk_disp[0] = FK_FAIL; g_fk_disp[1] = FK_FAIL;
    pg_test_resolve = fk_resolve; pg_test_socket_provider = &FK_PROVIDER;

    char err[128] = {0};
    PgTransport *t = hl_pg_transport_connect("h", "5432", 0, &FK_PROVIDER, err, sizeof err);
    ASSERT_TRUE(t == NULL);
    ASSERT_TRUE(err[0] != '\0');
    ASSERT_TRUE(fk_all_fds_closed());   /* teardown disposed every racing fd */
    seam_clear(); tp_disarm_watchdog();
}

/* ── the connect deadline fires -> NULL + confirmed detachment ──────── */
UTEST(pg_transport_connect, deadline_fires_detaches)
{
    fk_reset(); tp_arm_watchdog();
    g_fk_naddr = 1; g_fk_disp[0] = FK_PENDING;   /* never completes on its own */
    pg_test_resolve = fk_resolve; pg_test_socket_provider = &FK_PROVIDER;

    char err[128] = {0};
    /* 150 ms TCP-establishment bound; the address stays pending, so the deadline
     * fires and the op fails + detaches. */
    PgTransport *t = hl_pg_transport_connect("h", "5432", 150, &FK_PROVIDER, err, sizeof err);
    ASSERT_TRUE(t == NULL);
    ASSERT_TRUE(err[0] != '\0');
    ASSERT_TRUE(fk_all_fds_closed());
    seam_clear(); tp_disarm_watchdog();
}

/* ── truly unbounded (timeout_ms <= 0): NO hidden ceiling. A pending attempt is
 *    driven to completion by the test at pump checkpoint 3 (we drain the peer so
 *    the fd becomes writable), proving the loop pumps until completion and does
 *    not impose a 30 s (or any) total deadline. The 5 s watchdog would trip on a
 *    hang; success well under it is the proof. ─────────────────────────────── */
static void tp_complete_pending_at_cp3(PgTransport *t, unsigned idx)
{
    if (idx != 3) return;
    /* Drain the peer of the single in-flight attempt so its fd becomes writable. */
    for (int i = 0; i < t->naddrs; i++) {
        if (kl_handle_valid(t->attempt_fd[i])) {
            FkSlot *s = fk_slot_for((int)t->attempt_fd[i]);
            if (s) { char b[4096]; while (recv(s->b, b, sizeof b, MSG_DONTWAIT) > 0) { } }
        }
    }
}
UTEST(pg_transport_connect, unbounded_no_hidden_ceiling)
{
    fk_reset(); tp_arm_watchdog();
    g_fk_naddr = 1; g_fk_disp[0] = FK_PENDING;
    pg_test_resolve = fk_resolve; pg_test_socket_provider = &FK_PROVIDER;
    pg_test_checkpoint = tp_complete_pending_at_cp3;

    PgTransport *t = hl_pg_transport_connect("h", "5432", 0 /* unbounded */,
                                             &FK_PROVIDER, NULL, 0);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(g_fk_succeeded_idx, 0);
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    seam_clear(); tp_disarm_watchdog();
}

/* ── provider io_status classification with an IRRELEVANT hosted errno ─
 *    connect leaves errno = EPERM (a FATAL-looking code) but io_status reports
 *    KL_IO_PENDING, so the transport must treat it as pending (consulting
 *    io_status, not errno) and complete. ────────────────────────────────────── */
UTEST(pg_transport_io, io_status_overrides_errno)
{
    fk_reset(); tp_arm_watchdog();
    g_fk_naddr = 1; g_fk_disp[0] = FK_PENDING;
    g_fk_bogus_errno = 1;                 /* connect sets errno = EPERM */
    g_fk_io_status_val = KL_IO_PENDING;   /* but io_status says PENDING */
    pg_test_resolve = fk_resolve; pg_test_socket_provider = &FK_PROVIDER_IOS;
    pg_test_checkpoint = tp_complete_pending_at_cp3;

    PgTransport *t = hl_pg_transport_connect("h", "5432", 0, &FK_PROVIDER_IOS, NULL, 0);
    /* On completion get_so_error reports 0 -> success, despite errno == EPERM. */
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(g_fk_succeeded_idx, 0);
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    seam_clear(); tp_disarm_watchdog();
}

/* ── send EINTR retry classified via io_status (irrelevant errno) ───── */
UTEST(pg_transport_io, send_eintr_retry_via_io_status)
{
    fk_reset(); tp_arm_watchdog();
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pg_test_socket_provider = &FK_PROVIDER_IOS;
    char err[64] = {0};
    PgTransport *t = hl_pg_transport_adopt(sv[0], &FK_PROVIDER_IOS, err, sizeof err);
    ASSERT_TRUE(t != NULL);

    /* First send() returns -1 with io_status INTERRUPTED (errno left 0); the retry
     * loop must retry. The one-shot clears after the first call, so the second
     * send() hits the real socket and returns 3 (> 0), exiting the loop - no
     * io_status flip is needed since the loop exits on a positive count. */
    g_fk_send_eintr_once = 1;
    g_fk_io_status_val   = KL_IO_INTERRUPTED;
    const uint8_t msg[3] = { 1, 2, 3 };
    ssize_t n = hl_pg_transport_send(t, msg, sizeof msg);
    ASSERT_EQ((int)n, 3);                 /* retried past the interrupt */
    /* Confirm the 3 bytes arrived after the retry. */
    uint8_t got[3] = {0};
    ssize_t r = recv(sv[1], got, sizeof got, 0);
    ASSERT_EQ((int)r, 3);
    ASSERT_EQ(got[0], 1); ASSERT_EQ(got[2], 3);
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    close(sv[1]);
    seam_clear(); tp_disarm_watchdog();
}

/* ── provider validation: missing a required op fails closed ────────── */
UTEST(pg_transport_provider, missing_required_op_fails)
{
    fk_reset();
    static KlSocketOps ops_no_setblocking = { FK_OPS_COMMON };
    ops_no_setblocking.set_blocking = NULL;   /* drop a required op */
    static KlSocketProvider prov;
    prov.ops = &ops_no_setblocking; prov.context = NULL;
    prov.capabilities = KL_SOCK_CAP_NATIVE_FD;

    char err[128] = {0};
    PgTransport *t = hl_pg_transport_connect("h", "5432", 0, &prov, err, sizeof err);
    ASSERT_TRUE(t == NULL);
    ASSERT_TRUE(strstr(err, "provider") != NULL);
}

/* ── provider validation: missing KL_SOCK_CAP_NATIVE_FD fails closed ── */
UTEST(pg_transport_provider, missing_native_fd_cap_fails)
{
    fk_reset();
    static KlSocketProvider prov;
    prov.ops = &FK_OPS; prov.context = NULL; prov.capabilities = 0;   /* no NATIVE_FD */

    char err[128] = {0};
    PgTransport *t = hl_pg_transport_connect("h", "5432", 0, &prov, err, sizeof err);
    ASSERT_TRUE(t == NULL);
    /* adopt validates too. */
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    PgTransport *ta = hl_pg_transport_adopt(sv[0], &prov, err, sizeof err);
    ASSERT_TRUE(ta == NULL);
    close(sv[0]); close(sv[1]);
}

/* ── set_blocking failure fails the connect closed ──────────────────── */
UTEST(pg_transport_connect, set_blocking_failure_fails)
{
    fk_reset(); tp_arm_watchdog();
    g_fk_naddr = 1; g_fk_disp[0] = FK_SUCCEED;
    g_fk_blocking_fail = 1;               /* set_blocking returns -1 on the winner */
    pg_test_resolve = fk_resolve; pg_test_socket_provider = &FK_PROVIDER;

    char err[128] = {0};
    PgTransport *t = hl_pg_transport_connect("h", "5432", 0, &FK_PROVIDER, err, sizeof err);
    ASSERT_TRUE(t == NULL);
    ASSERT_TRUE(fk_all_fds_closed());     /* the winner was retired, not leaked */
    seam_clear(); tp_disarm_watchdog();
}

/* ── IP-literal fast path: kl_sockaddr_parse, no resolve callback ───── */
UTEST(pg_transport_resolve, ip_literal_no_dns)
{
    fk_reset(); tp_arm_watchdog();
    g_fk_disp[0] = FK_SUCCEED;
    g_fk_resolve_called = 0;
    pg_test_resolve = NULL;               /* force the real resolve_addrs */
    pg_test_socket_provider = &FK_PROVIDER;

    PgTransport *t = hl_pg_transport_connect("127.0.0.1", "5432", 0, &FK_PROVIDER, NULL, 0);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(g_fk_resolve_called, 0);    /* the DNS callback was never consulted */
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    seam_clear(); tp_disarm_watchdog();
}

/* ── adopt: I/O roundtrip + close disposes the descriptor EXACTLY once ─ */
UTEST(pg_transport_io, adopt_roundtrip_close_once)
{
    fk_reset(); tp_arm_watchdog();
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pg_test_socket_provider = &FK_PROVIDER;
    PgTransport *t = hl_pg_transport_adopt(sv[0], &FK_PROVIDER, NULL, 0);
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(hl_pg_transport_fd(t), sv[0]);

    const uint8_t out[4] = { 9, 8, 7, 6 };
    ASSERT_EQ(hl_pg_transport_send_all(t, out, sizeof out), 0);
    uint8_t got[4] = {0};
    ssize_t r = recv(sv[1], got, sizeof got, 0);
    ASSERT_EQ((int)r, 4);
    ASSERT_EQ(got[0], 9); ASSERT_EQ(got[3], 6);

    /* peer -> transport recv */
    const uint8_t rep[2] = { 42, 43 };
    ASSERT_EQ((int)send(sv[1], rep, sizeof rep, 0), 2);
    uint8_t in[2] = {0};
    ASSERT_EQ((int)hl_pg_transport_recv(t, in, sizeof in), 2);
    ASSERT_EQ(in[0], 42); ASSERT_EQ(in[1], 43);

    g_fk_close_n = 0;
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    ASSERT_EQ(g_fk_close_n, 1);           /* the descriptor was closed exactly once */
    close(sv[1]);
    seam_clear(); tp_disarm_watchdog();
}

/* ── partial recv returns the short count ───────────────────────────── */
UTEST(pg_transport_io, partial_recv_short_count)
{
    fk_reset(); tp_arm_watchdog();
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pg_test_socket_provider = &FK_PROVIDER;
    PgTransport *t = hl_pg_transport_adopt(sv[0], &FK_PROVIDER, NULL, 0);
    ASSERT_TRUE(t != NULL);

    const uint8_t two[2] = { 5, 6 };
    ASSERT_EQ((int)send(sv[1], two, sizeof two, 0), 2);
    uint8_t buf[16] = {0};
    ssize_t n = hl_pg_transport_recv(t, buf, sizeof buf);  /* asked 16, gets 2 */
    ASSERT_EQ((int)n, 2);
    ASSERT_EQ(buf[0], 5); ASSERT_EQ(buf[1], 6);
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    close(sv[1]);
    seam_clear(); tp_disarm_watchdog();
}

/* ── send_all completes over FORCED partial writes, preserving order ──
 *    The provider caps every send() at 1 byte, so an 8-byte payload requires 8
 *    send() calls: this proves hl_pg_transport_send_all loops over short writes
 *    (advancing buf + remaining len each time) and that the bytes arrive in order.
 *    The prior adopt roundtrip sent through the raw provider which completes in
 *    one call, so it did not exercise the partial-write path. ─────────────────── */
UTEST(pg_transport_io, send_all_completes_forced_partial_writes)
{
    fk_reset(); tp_arm_watchdog();
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pg_test_socket_provider = &FK_PROVIDER;
    PgTransport *t = hl_pg_transport_adopt(sv[0], &FK_PROVIDER, NULL, 0);
    ASSERT_TRUE(t != NULL);

    const uint8_t payload[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    g_fk_send_max = 1;                    /* 1 byte per send() -> forced partials */
    g_fk_send_n   = 0;
    ASSERT_EQ(hl_pg_transport_send_all(t, payload, sizeof payload), 0);   /* all sent */
    ASSERT_EQ(g_fk_send_n, 8);            /* 8 short sends: the loop ran, not one call */

    /* Read every byte back and assert order is preserved. */
    uint8_t got[8] = {0};
    size_t r = 0;
    while (r < sizeof got) {
        ssize_t k = recv(sv[1], got + r, sizeof got - r, 0);
        if (k <= 0) break;
        r += (size_t)k;
    }
    ASSERT_EQ((int)r, 8);
    for (int i = 0; i < 8; i++) ASSERT_EQ(got[i], payload[i]);

    ASSERT_EQ(hl_pg_transport_close(t), 0);
    close(sv[1]);
    seam_clear(); tp_disarm_watchdog();
}

/* ── TLS attach is one-shot + fallible ──────────────────────────────── */
UTEST(pg_transport_tls, attach_one_shot_and_fallible)
{
    fk_reset(); tp_arm_watchdog();
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pg_test_socket_provider = &FK_PROVIDER;
    PgTransport *t = hl_pg_transport_adopt(sv[0], &FK_PROVIDER, NULL, 0);
    ASSERT_TRUE(t != NULL);

    /* NULL session rejected. */
    ASSERT_EQ(hl_pg_transport_attach_tls(t, NULL), -1);
    ASSERT_TRUE(t->tls == NULL);

    /* A non-NULL (opaque, never dereferenced here) session attaches once. We use a
     * sentinel pointer and NEVER close through it: white-box-clear t->tls before
     * close so the transport does not shutdown/free the sentinel. This tests the
     * attach LOGIC (one-shot / non-NULL / live-fd), not a real TLS handshake. */
    struct HlTlsClient *sentinel = (struct HlTlsClient *)(void *)&sv;   /* any non-NULL */
    ASSERT_EQ(hl_pg_transport_attach_tls(t, sentinel), 0);
    ASSERT_TRUE(t->tls == sentinel);
    /* Second attach rejected; the first is NOT dropped. */
    struct HlTlsClient *other = (struct HlTlsClient *)(void *)&t;
    ASSERT_EQ(hl_pg_transport_attach_tls(t, other), -1);
    ASSERT_TRUE(t->tls == sentinel);      /* unchanged: no silent drop */

    t->tls = NULL;                        /* white-box: do not free the sentinel */
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    close(sv[1]);
    seam_clear(); tp_disarm_watchdog();
}

/* Attach requires a live descriptor. */
UTEST(pg_transport_tls, attach_requires_live_fd)
{
    fk_reset();
    /* An adopt with a real fd, then white-box drop the fd to INVALID and attempt
     * to attach: rejected because the descriptor is not live. */
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pg_test_socket_provider = &FK_PROVIDER;
    PgTransport *t = hl_pg_transport_adopt(sv[0], &FK_PROVIDER, NULL, 0);
    ASSERT_TRUE(t != NULL);
    KlSocketHandle saved = t->fd;
    t->fd = KL_INVALID_SOCKET;
    struct HlTlsClient *sentinel = (struct HlTlsClient *)(void *)&sv;
    ASSERT_EQ(hl_pg_transport_attach_tls(t, sentinel), -1);
    t->fd = saved;
    ASSERT_EQ(hl_pg_transport_close(t), 0);
    close(sv[1]);
    seam_clear();
}

/* Build a transport parked in the "live started raced op" state (an attempt in
 * flight, ev ctx live), stopping before terminal - the exact state the preserve/
 * retry contract must handle. Returns the transport (caller drives close/teardown
 * + frees). */
static PgTransport *tp_make_live_op(void)
{
    g_fk_naddr = 1; g_fk_disp[0] = FK_PENDING;
    pg_test_resolve = fk_resolve; pg_test_socket_provider = &FK_PROVIDER;
    PgTransport *t = transport_new(&FK_PROVIDER);
    if (!t) return NULL;
    t->alloc = kl_allocator_default();
    if (kl_event_ctx_init(&t->ev, &t->alloc) != 0) { free(t); return NULL; }
    t->ev_ready = 1;
    t->naddrs = resolve_addrs(t, "h", 5432);
    if (t->naddrs < 1) { kl_event_ctx_free(&t->ev); free(t); return NULL; }
    if (kl_connect_op_init(&t->connect_op, &PG_HOOKS_NO_DEADLINE, t) != 0 ||
        kl_connect_op_start(&t->connect_op) != 0) { kl_event_ctx_free(&t->ev); free(t); return NULL; }
    t->connect_started = 1;
    (void)kl_event_ctx_run(&t->ev, 64, 10);   /* let the attempt arm */
    return t;
}

/* ── connect_teardown preserve/retry contract at the unit level ──────
 *    A live started op with force_no_detach set -> teardown returns -1 (preserve,
 *    frees nothing); clear -> teardown returns 0 (quiesce + free ev ctx). This is
 *    the exact contract hl_pg_transport_close relies on. ────────────────────────── */
UTEST(pg_transport_close, teardown_returns_status)
{
    fk_reset(); tp_arm_watchdog();
    PgTransport *t = tp_make_live_op();
    ASSERT_TRUE(t != NULL);

    pg_test_force_no_detach = 1;
    ASSERT_EQ(connect_teardown(t), -1);        /* PRESERVE */
    ASSERT_EQ(t->ev_ready, 1);                 /* ev ctx NOT freed */

    pg_test_force_no_detach = 0;
    ASSERT_EQ(connect_teardown(t), 0);         /* quiesce + free ev ctx */
    ASSERT_EQ(t->ev_ready, 0);
    ASSERT_TRUE(fk_all_fds_closed());
    free(t);                                   /* op detached; block freeable */
    seam_clear(); tp_disarm_watchdog();
}

/* ── forced non-detachment: hl_pg_transport_close PRESERVES the whole allocation
 *    and is RETRYABLE. First close (force_no_detach) returns -1 without freeing
 *    (t stays valid); clearing the flag lets a retry detach + free cleanly, so
 *    LSan sees no leak. Proves both preservation and the retryable close. ──────── */
UTEST(pg_transport_close, forced_non_detach_close_preserves_then_retry)
{
    fk_reset(); tp_arm_watchdog();
    PgTransport *t = tp_make_live_op();
    ASSERT_TRUE(t != NULL);

    /* First close cannot confirm detachment -> -1, allocation PRESERVED (t is
     * still a valid pointer; nothing freed). */
    pg_test_force_no_detach = 1;
    ASSERT_EQ(hl_pg_transport_close(t), -1);
    ASSERT_EQ(t->ev_ready, 1);                 /* not freed / not quiesced */

    /* Retry after clearing the forced state: detaches, frees, returns 0. */
    pg_test_force_no_detach = 0;
    ASSERT_EQ(hl_pg_transport_close(t), 0);    /* t freed here */
    seam_clear(); tp_disarm_watchdog();
}

UTEST_MAIN()
