/*
 * test_pg_transport.c - Focused tests for the PostgreSQL-over-Keel-v3 transport
 * (Checkpoint 2 of docs/pg_keel_transport_slice3.md).
 *
 * These drive the transport's connect machinery + blocking I/O + lifecycle
 * through the -DHL_PG_TEST_HOOKS seam, WITHOUT any network or DNS:
 *
 *   - a connect attempt that is PENDING, then SUCCEEDS (the connect op races to a
 *     winning fd and set_blocking()s it);
 *   - a connect that FAILS on every address (all dispositions fail);
 *   - the IP-literal fast path (kl_sockaddr_parse, no DNS resolve callback);
 *   - a partial send that completes via hl_pg_transport_send_all;
 *   - hl_pg_transport_recv returns data written by the peer;
 *   - hl_pg_transport_adopt(fd) routes I/O + closes exactly once;
 *   - hl_pg_transport_close is idempotent (double-close is safe);
 *   - confirmed connect-op detachment on a failed connect (no leaked op / fd).
 *
 * The static helpers are reached by direct-including the capability .c; the
 * Makefile rule for this binary therefore EXCLUDES cap_pg_transport.o from the
 * common link (mirrors test_smtp_transport / test_pg_conn's direct-source
 * pattern). This test source is never in the shipped binary.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

/* Direct-source include: the static helpers (resolve_addrs, the connect hooks,
 * the pump, connect_teardown) plus the public API are the unit under test. */
#include "../../../src/hull/cap/pg_transport.c"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <keel/sockaddr.h>

/* ════════════════════════════════════════════════════════════════════
 *  A watchdog so a wedged pump can never hang CI: SIGALRM aborts the test.
 * ════════════════════════════════════════════════════════════════════ */
static void tp_watchdog_fired(int sig) { (void)sig; _exit(77); }

/* ════════════════════════════════════════════════════════════════════
 *  Fake socket provider (pg_test_socket_provider). It hands the connect op real
 *  fds the event loop can watch, keeping ONE clock domain: a "pending" address is
 *  a socketpair whose send buffer is filled so its fd is never write-ready (both
 *  epoll and kqueue respect a full send buffer), so the attempt stays pending
 *  under kernel semantics. "succeed" / "fail" leave the fd writable and let
 *  get_so_error report SO_ERROR. Attempts are recorded (address index) so a test
 *  can assert the winner and the attempt count.
 * ════════════════════════════════════════════════════════════════════ */

enum { FK_PENDING = 0, FK_SUCCEED = 1, FK_FAIL = 2 };

typedef struct { int used; int a; int b; int disp; int idx; } FkSlot;
static FkSlot g_fk[32];
static int    g_fk_disp[KL_CONNECT_MAX_ADDRS];  /* per-address disposition       */
static int    g_fk_order[KL_CONNECT_MAX_ADDRS]; /* attempt order (address index) */
static int    g_fk_n;                           /* number of connect() attempts  */
static int    g_fk_succeeded_idx;               /* address that read SO_ERROR==0  */
static int    g_fk_naddr;                        /* injected address count        */
static int    g_fk_blocking_set;                 /* count of set_blocking calls    */

static void fk_reset(void)
{
    memset(g_fk, 0, sizeof g_fk);
    memset(g_fk_disp, 0, sizeof g_fk_disp);
    memset(g_fk_order, 0, sizeof g_fk_order);
    g_fk_n = 0;
    g_fk_succeeded_idx = -1;
    g_fk_naddr = 0;
    g_fk_blocking_set = 0;
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
{ (void)c; g_fk_blocking_set++; int a = (int)fd; int fl = fcntl(a, F_GETFL, 0); return fcntl(a, F_SETFL, fl & ~O_NONBLOCK); }
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
    if (disp == FK_PENDING) {
        /* Fill the send buffer so 'a' never becomes write-ready. */
        char buf[2048];
        memset(buf, 'x', sizeof buf);
        int fl = fcntl(a, F_GETFL, 0); fcntl(a, F_SETFL, fl | O_NONBLOCK);
        while (write(a, buf, sizeof buf) > 0) { }
    }
    errno = EINPROGRESS;
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
{ (void)c; return send((int)fd, b, n, 0); }
static kl_ssize_t fk_recv(void *c, KlSocketHandle fd, void *b, size_t n)
{ (void)c; return recv((int)fd, b, n, 0); }
static kl_ssize_t fk_recv_peek(void *c, KlSocketHandle fd, void *b, size_t n)
{ (void)c; return recv((int)fd, b, n, MSG_PEEK); }
static int fk_close(void *c, KlSocketHandle fd)
{
    (void)c; int a = (int)fd;
    FkSlot *s = fk_slot_for(a);
    if (s) { close(s->b); s->used = 0; }
    return close(a);
}

static const KlSocketOps FK_OPS = {
    .set_nonblocking = fk_set_nb,
    .set_blocking    = fk_set_blocking,
    .set_cloexec     = fk_set_void,
    .set_nosigpipe   = fk_set_void,
    .set_reuseaddr   = fk_set_int,
    .set_reuseport   = fk_set_int,
    .set_ipv6only    = fk_set_int,
    .set_tcp_nodelay = fk_set_int,
    .set_cork        = fk_set_int,
    .socket          = fk_socket,
    .connect         = fk_connect,
    .close           = fk_close,
    .get_local_addr  = fk_get_local,
    .get_so_error    = fk_get_so_error,
    .send            = fk_send,
    .recv            = fk_recv,
    .recv_peek       = fk_recv_peek,
};
static const KlSocketProvider FK_PROVIDER = { .ops = &FK_OPS, .context = NULL };

/* Inject N addresses 10.11.12.(i+1) in order; disposition comes from g_fk_disp.
 * Also records whether the resolve hook was actually called (so the IP-literal
 * fast path can assert it was NOT). */
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

/* ════════════════════════════════════════════════════════════════════
 *  Connect: pending -> succeed.
 * ════════════════════════════════════════════════════════════════════ */
UTEST(pg_transport_connect, pending_then_succeed)
{
    fk_reset();
    g_fk_naddr   = 1;
    g_fk_disp[0] = FK_SUCCEED;
    g_fk_resolve_called = 0;
    pg_test_resolve         = fk_resolve;
    pg_test_socket_provider = &FK_PROVIDER;

    void (*prev)(int) = signal(SIGALRM, tp_watchdog_fired); alarm(30);

    PgTransport t;
    char err[128] = {0};
    int rc = hl_pg_transport_connect(&t, "db.example.test", "5432", 5000,
                                     &FK_PROVIDER, err, sizeof err);

    alarm(0); signal(SIGALRM, prev);
    pg_test_resolve         = NULL;
    pg_test_socket_provider = NULL;

    ASSERT_EQ(rc, 0);                          /* the race won */
    ASSERT_GE(g_fk_n, 1);                       /* at least one attempt */
    ASSERT_EQ(g_fk_succeeded_idx, 0);           /* address 0 connected */
    ASSERT_GE(g_fk_blocking_set, 1);            /* the winner was set_blocking()'d */
    ASSERT_TRUE(hl_pg_transport_fd(&t) >= 0);   /* a live descriptor is held */
    /* The connect-only event ctx is freed after a successful connect + detach. */
    ASSERT_EQ(t.ev_ready, 0);
    ASSERT_TRUE(kl_connect_op_is_detached(&t.connect_op));

    hl_pg_transport_close(&t);
    ASSERT_TRUE(hl_pg_transport_fd(&t) < 0);    /* fd retired by close */
}

/* Two addresses: the first is FK_PENDING, the second FK_SUCCEED. The RFC 8305
 * stagger must start address 1 while 0 is pending, and address 1 wins. */
UTEST(pg_transport_connect, stagger_second_address_wins)
{
    fk_reset();
    g_fk_naddr   = 2;
    g_fk_disp[0] = FK_PENDING;
    g_fk_disp[1] = FK_SUCCEED;
    pg_test_resolve         = fk_resolve;
    pg_test_socket_provider = &FK_PROVIDER;

    void (*prev)(int) = signal(SIGALRM, tp_watchdog_fired); alarm(30);

    PgTransport t;
    int rc = hl_pg_transport_connect(&t, "db.example.test", "5432", 5000,
                                     &FK_PROVIDER, NULL, 0);

    alarm(0); signal(SIGALRM, prev);
    pg_test_resolve         = NULL;
    pg_test_socket_provider = NULL;

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(g_fk_n, 2);               /* both addresses attempted */
    ASSERT_EQ(g_fk_order[0], 0);
    ASSERT_EQ(g_fk_order[1], 1);
    ASSERT_EQ(g_fk_succeeded_idx, 1);   /* address 1 connected */

    hl_pg_transport_close(&t);
}

/* ════════════════════════════════════════════════════════════════════
 *  Connect: all addresses fail. The transport reports -1 AND the connect op
 *  reaches confirmed detachment (no leaked op / fd).
 * ════════════════════════════════════════════════════════════════════ */
UTEST(pg_transport_connect, all_addresses_fail_detach)
{
    fk_reset();
    g_fk_naddr   = 2;
    g_fk_disp[0] = FK_FAIL;
    g_fk_disp[1] = FK_FAIL;
    pg_test_resolve         = fk_resolve;
    pg_test_socket_provider = &FK_PROVIDER;

    void (*prev)(int) = signal(SIGALRM, tp_watchdog_fired); alarm(30);

    PgTransport t;
    char err[128] = {0};
    int rc = hl_pg_transport_connect(&t, "db.example.test", "5432", 5000,
                                     &FK_PROVIDER, err, sizeof err);

    alarm(0); signal(SIGALRM, prev);
    pg_test_resolve         = NULL;
    pg_test_socket_provider = NULL;

    ASSERT_EQ(rc, -1);                             /* connect failed */
    ASSERT_TRUE(err[0] != '\0');                    /* a message was written */
    /* connect_teardown pumped to confirmed detachment before returning: the op is
     * detached and the event ctx freed (no leaked op / fd). */
    ASSERT_TRUE(kl_connect_op_is_detached(&t.connect_op));
    ASSERT_EQ(t.ev_ready, 0);
    ASSERT_TRUE(hl_pg_transport_fd(&t) < 0);

    hl_pg_transport_close(&t);   /* idempotent: safe after a failed connect */
}

/* A resolve that yields zero addresses fails cleanly (the op never starts). */
UTEST(pg_transport_connect, resolve_empty_fails)
{
    fk_reset();
    g_fk_naddr   = 0;             /* fk_resolve returns 0 */
    pg_test_resolve         = fk_resolve;
    pg_test_socket_provider = &FK_PROVIDER;

    PgTransport t;
    char err[128] = {0};
    int rc = hl_pg_transport_connect(&t, "nope.example.test", "5432", 5000,
                                     &FK_PROVIDER, err, sizeof err);

    pg_test_resolve         = NULL;
    pg_test_socket_provider = NULL;

    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(err[0] != '\0');
    ASSERT_EQ(t.ev_ready, 0);      /* event ctx torn down */
}

/* ════════════════════════════════════════════════════════════════════
 *  IP-literal fast path: with NO resolve hook installed, resolve_addrs takes the
 *  kl_sockaddr_parse (no-DNS) branch for a numeric host. Prove the fast path via
 *  resolve_addrs directly (no socket needed) and via a full connect.
 * ════════════════════════════════════════════════════════════════════ */
UTEST(pg_transport_resolve, ip_literal_no_dns)
{
    pg_test_resolve         = NULL;   /* force the real resolve_addrs */
    pg_test_socket_provider = NULL;

    PgTransport t;
    transport_zero(&t);
    t.sp = &FK_PROVIDER;
    int n = resolve_addrs(&t, "127.0.0.1", 5432);
    ASSERT_EQ(n, 1);                         /* exactly one address, no DNS */
    ASSERT_EQ((int)t.addrs[0].addr_len, 4);   /* an IPv4 literal */

    /* An IPv6 literal too. */
    int n6 = resolve_addrs(&t, "::1", 5432);
    ASSERT_EQ(n6, 1);
    ASSERT_EQ((int)t.addrs[0].addr_len, 16);
}

/* Full connect to an IP literal with NO resolve hook installed: the real
 * resolve_addrs must take the kl_sockaddr_parse (no-DNS) branch, and the connect
 * still races through the fake socket provider and wins. Proves the fast path
 * drives a real connect end to end without any getaddrinfo / resolve callback. */
UTEST(pg_transport_resolve, ip_literal_connect_no_dns)
{
    fk_reset();
    /* g_fk_disp is indexed by (ip[3]-1); a "127.0.0.1" literal parses to
     * addr_len 4 with ip[3]==1, so index 0. Make index 0 succeed. */
    g_fk_disp[0] = FK_SUCCEED;
    g_fk_resolve_called = 0;
    pg_test_resolve         = NULL;            /* force the real resolve_addrs */
    pg_test_socket_provider = &FK_PROVIDER;    /* fake connect, no real network */

    void (*prev)(int) = signal(SIGALRM, tp_watchdog_fired); alarm(30);

    PgTransport t;
    int rc = hl_pg_transport_connect(&t, "127.0.0.1", "5432", 5000,
                                     &FK_PROVIDER, NULL, 0);

    alarm(0); signal(SIGALRM, prev);
    pg_test_socket_provider = NULL;

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(g_fk_resolve_called, 0);   /* the resolve callback never ran (fast path) */
    ASSERT_EQ(g_fk_succeeded_idx, 0);    /* the parsed 127.0.0.1 address connected */

    hl_pg_transport_close(&t);
}

/* ════════════════════════════════════════════════════════════════════
 *  Blocking I/O over an ADOPTED descriptor (design Amendment 2): a real
 *  socketpair, the default POSIX provider, no event ctx / resolve / race.
 * ════════════════════════════════════════════════════════════════════ */
UTEST(pg_transport_io, adopt_send_recv_roundtrip)
{
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    PgTransport t;
    ASSERT_EQ(hl_pg_transport_adopt(&t, sv[0], NULL), 0);
    ASSERT_EQ(hl_pg_transport_fd(&t), sv[0]);
    ASSERT_EQ(t.ev_ready, 0);                 /* adopt never inits an event ctx */
    ASSERT_EQ(t.connect_started, 0);

    /* send_all -> the peer sees every byte. */
    const uint8_t msg[] = "SELECT 1";
    ASSERT_EQ(hl_pg_transport_send_all(&t, msg, sizeof msg), 0);
    uint8_t peer[64] = {0};
    ssize_t got = recv(sv[1], peer, sizeof peer, 0);
    ASSERT_EQ(got, (ssize_t)sizeof msg);
    ASSERT_EQ(memcmp(peer, msg, sizeof msg), 0);

    /* recv -> the transport reads what the peer wrote. */
    const uint8_t reply[] = "ReadyForQuery";
    ASSERT_EQ(write(sv[1], reply, sizeof reply), (ssize_t)sizeof reply);
    uint8_t rbuf[64] = {0};
    ssize_t rn = hl_pg_transport_recv(&t, rbuf, sizeof rbuf);
    ASSERT_EQ(rn, (ssize_t)sizeof reply);
    ASSERT_EQ(memcmp(rbuf, reply, sizeof reply), 0);

    hl_pg_transport_close(&t);
    ASSERT_TRUE(hl_pg_transport_fd(&t) < 0);   /* fd retired */
    close(sv[1]);
}

/* A partial send (the peer's receive buffer is tiny, so one send returns short)
 * still completes via hl_pg_transport_send_all's full-write loop. A background
 * drain keeps the pipe moving so the loop makes progress. */
static int   g_drain_fd;
static size_t g_drain_target;
static void *drain_thread(void *arg)
{
    (void)arg;
    size_t got = 0;
    uint8_t buf[4096];
    while (got < g_drain_target) {
        ssize_t n = recv(g_drain_fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return NULL;
}

UTEST(pg_transport_io, send_all_completes_partial)
{
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    int small = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);

    PgTransport t;
    ASSERT_EQ(hl_pg_transport_adopt(&t, sv[0], NULL), 0);

    /* A payload larger than the socket buffers, so a single send goes short and
     * send_all must loop. A background reader drains the far end. */
    size_t big = 256 * 1024;
    uint8_t *payload = malloc(big);
    ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < big; i++) payload[i] = (uint8_t)(i & 0xff);

    g_drain_fd = sv[1]; g_drain_target = big;
    pthread_t th;
    ASSERT_EQ(pthread_create(&th, NULL, drain_thread, NULL), 0);

    void (*prev)(int) = signal(SIGALRM, tp_watchdog_fired); alarm(30);
    int rc = hl_pg_transport_send_all(&t, payload, big);
    alarm(0); signal(SIGALRM, prev);

    pthread_join(th, NULL);
    ASSERT_EQ(rc, 0);   /* every byte written despite short sends */

    free(payload);
    hl_pg_transport_close(&t);
    close(sv[1]);
}

/* ════════════════════════════════════════════════════════════════════
 *  Close is idempotent: a second close (and a close on a fresh-zeroed transport)
 *  is safe and does not touch a retired fd twice.
 * ════════════════════════════════════════════════════════════════════ */
UTEST(pg_transport_close, idempotent_double_close)
{
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    PgTransport t;
    ASSERT_EQ(hl_pg_transport_adopt(&t, sv[0], NULL), 0);

    hl_pg_transport_close(&t);
    ASSERT_TRUE(hl_pg_transport_fd(&t) < 0);
    /* Second close: the guard makes it a no-op (no double sp_close of the fd). */
    hl_pg_transport_close(&t);
    ASSERT_TRUE(hl_pg_transport_fd(&t) < 0);

    close(sv[1]);
}

/* Close on an adopted-then-never-used transport, and a NULL-safe close. */
UTEST(pg_transport_close, null_and_fresh_safe)
{
    hl_pg_transport_close(NULL);   /* NULL-safe */

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    PgTransport t;
    ASSERT_EQ(hl_pg_transport_adopt(&t, sv[0], NULL), 0);
    hl_pg_transport_close(&t);
    close(sv[1]);
}

UTEST_MAIN()
