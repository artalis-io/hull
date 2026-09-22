/*
 * test_net_stream.c - the hull/net transport: connect, close, free.
 *
 * Against a real loopback listener, driving a real HlAsyncBackend loop. The
 * capability gate has its own suite (test_net_policy.c) and is deliberately
 * not repeated here: this file asks whether the transport does what the
 * contract in cap/net_stream.h says, once authorization has already passed.
 *
 * Why a real socket rather than a mock: the interesting failures in this layer
 * are a connect that resolves but is refused, a connect that never completes,
 * and teardown while an operation is still in flight. None of those are
 * reachable through a stub, and all three are the ones that corrupt state if
 * the ownership protocol is wrong.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/cap/net_stream.h"
#include "hull/shared/async_backend.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Fixture ────────────────────────────────────────────────────────── */

typedef struct {
    const HlAsyncBackend *be;
    HlAsyncBackendCtx    *ctx;
    HlAsyncBackendPool   *pool;   /* resolution runs here, not on the loop */
    int                   listen_fd;
    int                   peer_fd;   /* the accepted server side, for I/O */
    int                   port;
} NetFix;

/* A listener that accepts nothing. Enough for a connect to succeed: the
 * kernel completes the handshake from the backlog without any accept(). */
static int fix_listen(NetFix *f)
{
    f->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->listen_fd < 0) return -1;

    int on = 1;
    setsockopt(f->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;                    /* kernel picks a free port */

    if (bind(f->listen_fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    if (listen(f->listen_fd, 4) < 0) return -1;

    socklen_t alen = sizeof a;
    if (getsockname(f->listen_fd, (struct sockaddr *)&a, &alen) < 0) return -1;
    f->port = ntohs(a.sin_port);
    return 0;
}

static int fix_init(NetFix *f)
{
    memset(f, 0, sizeof *f);
    f->listen_fd = -1;
    f->peer_fd   = -1;
    f->be = hl_async_backend();
    if (!f->be) return -1;
    if (f->be->init(&f->ctx, NULL) != 0) return -1;
    /* Small on purpose: two workers is enough to show resolution is not
     * serialised behind the loop, and a tiny queue keeps a runaway test from
     * hiding behind capacity. */
    if (f->be->pool_create(&f->pool, f->ctx, 2, 16) != 0) return -1;
    return 0;
}

/* Defined below, next to the other pump helpers; fix_open needs it here. */
static int pump_connect(NetFix *f, HlNetStream *s, int max_ticks);

/* Take the server side of a connection that has already completed. The
 * handshake finishes from the backlog, so this returns immediately and the
 * whole exchange stays single-threaded and deterministic. */
static int fix_accept(NetFix *f)
{
    f->peer_fd = accept(f->listen_fd, NULL, NULL);
    return f->peer_fd >= 0 ? 0 : -1;
}

/* Connect + pump to open + accept, which every I/O case needs first. */
static int fix_open(NetFix *f, HlNetStream **out)
{
    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = f->ctx;
    cfg.pool       = f->pool;
    cfg.host       = "127.0.0.1";
    cfg.port       = f->port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    if (!s) return -1;
    if (pump_connect(f, s, 400) != HL_NET_OK) { hl_net_stream_free(s); return -1; }
    if (fix_accept(f) != 0) { hl_net_stream_free(s); return -1; }
    *out = s;
    return 0;
}

static void fix_free(NetFix *f)
{
    if (f->peer_fd >= 0) close(f->peer_fd);
    if (f->listen_fd >= 0) close(f->listen_fd);
    /* Pool first: pool_free drains and joins, so in-flight resolution finishes
     * (or is cancelled) while the loop it completes on is still alive. */
    if (f->be && f->pool) f->be->pool_free(f->pool);
    if (f->be && f->ctx) f->be->free(f->ctx);
}

/* Drive the loop until the connect reaches a terminal state or we run out of
 * patience. Bounded so a broken transport fails the test instead of hanging
 * the suite, which is the failure mode that wastes the most time. */
static int pump_connect(NetFix *f, HlNetStream *s, int max_ticks)
{
    int rc = hl_net_stream_connect_result(s);
    for (int i = 0; i < max_ticks && rc == HL_NET_E_AGAIN; i++) {
        f->be->tick(f->ctx, 20);
        rc = hl_net_stream_connect_result(s);
    }
    return rc;
}

/* ── connect ────────────────────────────────────────────────────────── */

UTEST(net_stream, connects_to_a_live_listener)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = f.ctx;
    cfg.pool       = f.pool;
    cfg.host       = "127.0.0.1";
    cfg.port       = f.port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    int rc = hl_net_stream_connect(&s, &cfg);
    ASSERT_NE(s, NULL);
    /* Either shape is legal: loopback often completes synchronously. */
    ASSERT_TRUE(rc == HL_NET_OK || rc == HL_NET_E_AGAIN);

    ASSERT_EQ(pump_connect(&f, s, 400), HL_NET_OK);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, refused_port_reports_connect_not_timeout)
{
    /* A closed port must surface as CONNECT. Reporting it as TIMEOUT is the
     * bug that reading KlConnectResult alone would have produced, since a
     * deadline and a refusal both arrive as KL_CONNECT_FAILED. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    int port = f.port;
    close(f.listen_fd);            /* nothing is listening there now */
    f.listen_fd = -1;

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = f.ctx;
    cfg.pool       = f.pool;
    cfg.host       = "127.0.0.1";
    cfg.port       = port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    int rc = hl_net_stream_connect(&s, &cfg);
    if (s) {
        rc = pump_connect(&f, s, 400);
        ASSERT_EQ(rc, HL_NET_E_CONNECT);
        hl_net_stream_free(s);
    } else {
        ASSERT_EQ(rc, HL_NET_E_CONNECT);
    }
    fix_free(&f);
}

UTEST(net_stream, unresolvable_host_reports_resolve)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = f.ctx;
    cfg.pool       = f.pool;
    cfg.host       = "no-such-host.invalid";   /* .invalid is reserved, RFC 2606 */
    cfg.port       = 80;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    int rc = hl_net_stream_connect(&s, &cfg);
    if (s) {
        rc = pump_connect(&f, s, 400);
        hl_net_stream_free(s);
    }
    ASSERT_EQ(rc, HL_NET_E_RESOLVE);
    fix_free(&f);
}

/* ── argument bounds ────────────────────────────────────────────────── */

UTEST(net_stream, rejects_bad_arguments_without_touching_the_network)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async = f.ctx;
    cfg.pool  = f.pool;
    cfg.host  = "127.0.0.1";
    cfg.port  = 80;

    HlNetStream *s = (HlNetStream *)0x1;
    ASSERT_EQ(hl_net_stream_connect(NULL, &cfg), HL_NET_E_INVAL);

    s = (HlNetStream *)0x1;
    ASSERT_EQ(hl_net_stream_connect(&s, NULL), HL_NET_E_INVAL);
    ASSERT_EQ(s, NULL);                      /* *out cleared even on refusal */

    HlNetStreamConfig bad = cfg;
    bad.host = NULL;
    ASSERT_EQ(hl_net_stream_connect(&s, &bad), HL_NET_E_INVAL);
    bad = cfg; bad.host = "";
    ASSERT_EQ(hl_net_stream_connect(&s, &bad), HL_NET_E_INVAL);
    bad = cfg; bad.port = 0;
    ASSERT_EQ(hl_net_stream_connect(&s, &bad), HL_NET_E_INVAL);
    bad = cfg; bad.port = 65536;
    ASSERT_EQ(hl_net_stream_connect(&s, &bad), HL_NET_E_INVAL);
    bad = cfg; bad.async = NULL;
    ASSERT_EQ(hl_net_stream_connect(&s, &bad), HL_NET_E_INVAL);
    /* No pool means nowhere to resolve without blocking the loop. Refused
     * rather than silently falling back to a blocking lookup. */
    bad = cfg; bad.pool = NULL;
    ASSERT_EQ(hl_net_stream_connect(&s, &bad), HL_NET_E_INVAL);
    /* A hostname longer than DNS permits is rejected before it is copied. */
    char longhost[300];
    memset(longhost, 'a', sizeof longhost - 1);
    longhost[sizeof longhost - 1] = '\0';
    bad = cfg; bad.host = longhost;
    ASSERT_EQ(hl_net_stream_connect(&s, &bad), HL_NET_E_INVAL);

    fix_free(&f);
}

/* ── teardown, which is where the ownership protocol is exercised ───── */

UTEST(net_stream, free_mid_connect_does_not_crash)
{
    /* The case the heap-allocated opaque handle exists for: free() while the
     * connect op may not have detached. The stream must go away without
     * touching storage a live op still references. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = f.ctx;
    cfg.pool       = f.pool;
    cfg.host       = "127.0.0.1";
    cfg.port       = f.port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    ASSERT_NE(s, NULL);
    hl_net_stream_free(s);          /* no pump: teardown races the connect */

    /* Keep ticking afterwards, and generously: teardown completes when the
     * connect op confirms detachment, which arrives through the loop. A late
     * callback into freed storage shows up here under ASan, and too few ticks
     * shows up as a LeakSanitizer report rather than silence. */
    for (int i = 0; i < 100; i++) f.be->tick(f.ctx, 5);
    fix_free(&f);
}

UTEST(net_stream, cancel_then_free_is_safe)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = f.ctx;
    cfg.pool       = f.pool;
    cfg.host       = "127.0.0.1";
    cfg.port       = f.port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    ASSERT_NE(s, NULL);

    hl_net_stream_cancel(s);
    ASSERT_EQ(hl_net_stream_connect_result(s), HL_NET_E_CLOSED);
    hl_net_stream_cancel(s);        /* idempotent */
    hl_net_stream_free(s);

    for (int i = 0; i < 100; i++) f.be->tick(f.ctx, 5);
    fix_free(&f);
}

UTEST(net_stream, close_after_connect_then_free)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = f.ctx;
    cfg.pool       = f.pool;
    cfg.host       = "127.0.0.1";
    cfg.port       = f.port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    ASSERT_NE(s, NULL);
    ASSERT_EQ(pump_connect(&f, s, 400), HL_NET_OK);

    hl_net_stream_close(s);
    ASSERT_EQ(hl_net_stream_connect_result(s), HL_NET_E_CLOSED);
    hl_net_stream_close(s);         /* idempotent */
    hl_net_stream_free(s);

    fix_free(&f);
}

UTEST(net_stream, free_is_safe_on_null)
{
    hl_net_stream_free(NULL);
    hl_net_stream_close(NULL);
    hl_net_stream_cancel(NULL);
    ASSERT_EQ(hl_net_stream_connect_result(NULL), HL_NET_E_INVAL);
    ASSERT_EQ(hl_net_stream_pending_op(NULL), NULL);
}


/* ── I/O ────────────────────────────────────────────────────────────── */

/* Drive the loop until a read is satisfiable or patience runs out. */
static long pump_read(NetFix *f, HlNetStream *s, void *buf, size_t len, int ticks)
{
    long rc = hl_net_stream_read(s, buf, len);
    for (int i = 0; i < ticks && rc == HL_NET_E_AGAIN; i++) {
        f->be->tick(f->ctx, 20);
        rc = hl_net_stream_read(s, buf, len);
    }
    return rc;
}

UTEST(net_stream, reads_what_the_peer_sent)
{
    NetFix f; HlNetStream *s = NULL;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);
    ASSERT_EQ(fix_open(&f, &s), 0);

    ASSERT_EQ(send(f.peer_fd, "hello", 5, 0), (ssize_t)5);

    char buf[16];
    long n = pump_read(&f, s, buf, sizeof buf, 200);
    ASSERT_EQ(n, 5L);
    ASSERT_EQ(memcmp(buf, "hello", 5), 0);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, read_parks_when_nothing_has_arrived)
{
    /* The park path itself: with an open connection and an idle peer, a read
     * must report AGAIN rather than block or spin. */
    NetFix f; HlNetStream *s = NULL;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);
    ASSERT_EQ(fix_open(&f, &s), 0);

    char buf[16];
    ASSERT_EQ(hl_net_stream_read(s, buf, sizeof buf), (long)HL_NET_E_AGAIN);
    ASSERT_NE(hl_net_stream_pending_op(s), NULL);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, read_returns_zero_on_clean_eof)
{
    NetFix f; HlNetStream *s = NULL;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);
    ASSERT_EQ(fix_open(&f, &s), 0);

    close(f.peer_fd);
    f.peer_fd = -1;

    char buf[16];
    ASSERT_EQ(pump_read(&f, s, buf, sizeof buf, 200), 0L);
    /* And stays at EOF rather than parking again. */
    ASSERT_EQ(hl_net_stream_read(s, buf, sizeof buf), 0L);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, buffered_bytes_are_drained_before_eof_is_reported)
{
    /* A peer that writes then immediately closes: the data must come out
     * first. Reporting EOF while bytes are still buffered would silently
     * truncate a protocol. */
    NetFix f; HlNetStream *s = NULL;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);
    ASSERT_EQ(fix_open(&f, &s), 0);

    ASSERT_EQ(send(f.peer_fd, "tail", 4, 0), (ssize_t)4);
    close(f.peer_fd);
    f.peer_fd = -1;

    char buf[16];
    ASSERT_EQ(pump_read(&f, s, buf, sizeof buf, 200), 4L);
    ASSERT_EQ(memcmp(buf, "tail", 4), 0);
    ASSERT_EQ(pump_read(&f, s, buf, sizeof buf, 200), 0L);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, read_can_be_served_in_pieces)
{
    /* A read returns what is there, not "one record". Asking for less than
     * arrived leaves the rest for the next call. */
    NetFix f; HlNetStream *s = NULL;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);
    ASSERT_EQ(fix_open(&f, &s), 0);

    ASSERT_EQ(send(f.peer_fd, "abcdef", 6, 0), (ssize_t)6);

    char buf[4];
    ASSERT_EQ(pump_read(&f, s, buf, 2, 200), 2L);
    ASSERT_EQ(memcmp(buf, "ab", 2), 0);
    ASSERT_EQ(pump_read(&f, s, buf, 4, 200), 4L);
    ASSERT_EQ(memcmp(buf, "cdef", 4), 0);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, writes_reach_the_peer)
{
    NetFix f; HlNetStream *s = NULL;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);
    ASSERT_EQ(fix_open(&f, &s), 0);

    ASSERT_EQ(hl_net_stream_write(s, "ping", 4), HL_NET_OK);
    for (int i = 0; i < 50; i++) f.be->tick(f.ctx, 5);

    char got[8];
    ssize_t n = recv(f.peer_fd, got, sizeof got, 0);
    ASSERT_EQ(n, (ssize_t)4);
    ASSERT_EQ(memcmp(got, "ping", 4), 0);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, write_bigger_than_the_send_capacity_is_refused)
{
    /* Never admittable, so it is refused outright rather than parked forever
     * waiting for room that cannot exist. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = f.ctx;
    cfg.pool       = f.pool;
    cfg.host       = "127.0.0.1";
    cfg.port       = f.port;
    cfg.connect_ms = 5000;
    cfg.write_cap  = 1024;

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    ASSERT_NE(s, NULL);
    ASSERT_EQ(pump_connect(&f, s, 400), HL_NET_OK);
    ASSERT_EQ(fix_accept(&f), 0);

    static char big[4096];
    memset(big, 'x', sizeof big);
    ASSERT_EQ(hl_net_stream_write(s, big, sizeof big), HL_NET_E_INVAL);
    /* And a buffer that does fit still works. */
    ASSERT_EQ(hl_net_stream_write(s, big, 512), HL_NET_OK);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, io_after_close_fails_closed)
{
    NetFix f; HlNetStream *s = NULL;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);
    ASSERT_EQ(fix_open(&f, &s), 0);

    hl_net_stream_close(s);

    char buf[8];
    ASSERT_EQ(hl_net_stream_read(s, buf, sizeof buf), (long)HL_NET_E_CLOSED);
    ASSERT_EQ(hl_net_stream_write(s, "x", 1), HL_NET_E_CLOSED);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, io_rejects_bad_arguments)
{
    char buf[8];
    ASSERT_EQ(hl_net_stream_read(NULL, buf, sizeof buf), (long)HL_NET_E_INVAL);
    ASSERT_EQ(hl_net_stream_write(NULL, "x", 1), HL_NET_E_INVAL);
}

/* ── the binding seam ───────────────────────────────────────────────── */

UTEST(net_stream, a_pending_op_leads_back_to_its_stream)
{
    /* A resume callback is handed only its op. Without this walk it has no way
     * to reach the stream, and through it whatever the binding attached. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = f.ctx;
    cfg.pool       = f.pool;
    cfg.host       = "127.0.0.1";
    cfg.port       = f.port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    int rc = hl_net_stream_connect(&s, &cfg);
    ASSERT_NE(s, NULL);
    ASSERT_EQ(rc, HL_NET_E_AGAIN);          /* resolution is off-thread */

    struct HlAsyncOp *op = hl_net_stream_pending_op(s);
    ASSERT_NE(op, NULL);
    ASSERT_EQ_MSG((void *)hl_net_stream_from_op(op), (void *)s,
                  "the op must lead back to the stream that owns it");

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, the_user_pointer_round_trips_and_is_not_touched)
{
    NetFix f; HlNetStream *s = NULL;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);
    ASSERT_EQ(fix_open(&f, &s), 0);

    ASSERT_EQ(hl_net_stream_user(s), NULL);   /* nothing attached by default */

    int marker = 0;
    hl_net_stream_set_user(s, &marker);
    ASSERT_EQ((void *)hl_net_stream_user(s), (void *)&marker);

    /* I/O must not disturb it: the transport attaches no meaning to it. */
    ASSERT_EQ(hl_net_stream_write(s, "x", 1), HL_NET_OK);
    ASSERT_EQ((void *)hl_net_stream_user(s), (void *)&marker);
    ASSERT_EQ_MSG(marker, 0, "the transport must never dereference it");

    hl_net_stream_set_user(s, NULL);
    ASSERT_EQ(hl_net_stream_user(s), NULL);

    hl_net_stream_free(s);
    fix_free(&f);
}

/* ── TLS ────────────────────────────────────────────────────────────────
 *
 * Driven by a FAKE KlTls rather than mbedTLS. What is under test is the state
 * machine this file wraps around the vtable - when the handshake runs, what
 * connect_result reports while it does, which call moves bytes, and the
 * buffered-plaintext case below - and every one of those is reachable through
 * a stub. Whether mbedTLS negotiates correctly is mbedTLS's concern, and is
 * covered live against a real server.
 *
 * The fake passes bytes straight through to the socket, so a test can still
 * assert against the real peer on the other end.
 */

typedef struct {
    KlTls   base;              /* first: the session pointer IS the vtable */
    int     handshakes_left;   /* WANT_* this many times, then OK          */
    int     handshake_fails;
    unsigned want;             /* what an unfinished handshake asks for    */
    int     hostname_calls;
    char    hostname[128];
    int     shutdown_calls;
    size_t  fake_pending;      /* plaintext the "engine" is sitting on     */
    int     reads, writes;
    int    *destroyed_flag;    /* outlives the session, for teardown tests */
    int     eof_seen;          /* the -1 just returned was a clean close  */
    int     clean_eof;         /* make the next read report one           */
    int     hard_error;        /* make the next read report a failure     */
} FakeTls;

static void *fake_alloc_fn(void *ud, size_t n) { (void)ud; return malloc(n); }
static void *fake_realloc_fn(void *ud, void *p, size_t o, size_t n)
{ (void)ud; (void)o; return realloc(p, n); }
static void  fake_free_fn(void *ud, void *p, size_t n) { (void)ud; (void)n; free(p); }

static KlTlsResult fake_handshake(KlTls *self, KlSocketHandle fd)
{
    (void)fd;
    FakeTls *f = (FakeTls *)self;
    if (f->handshake_fails) return KL_TLS_ERROR;
    if (f->handshakes_left > 0) {
        f->handshakes_left--;
        return f->want == HL_ASYNC_WRITE ? KL_TLS_WANT_WRITE : KL_TLS_WANT_READ;
    }
    return KL_TLS_OK;
}

static kl_ssize_t fake_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len)
{
    FakeTls *f = (FakeTls *)self;
    f->reads++;
    if (f->fake_pending) {
        /* Served from the "engine", not the socket: this is exactly the case
         * the pending() drain exists for. */
        size_t n = f->fake_pending < len ? f->fake_pending : len;
        memset(buf, 0x50, n);                     /* 'P' */
        f->fake_pending -= n;
        return (kl_ssize_t)n;
    }
    if (f->hard_error) { f->eof_seen = 0; return -1; }
    if (f->clean_eof) {
        /* What the mbedTLS adapter really does: a close_notify comes back as
         * -1 with at_eof set, NOT as 0. The header's prose reads the other
         * way, and following it turned every finished response into a
         * transport error until a live server proved otherwise. */
        f->eof_seen = 1;
        return -1;
    }
    kl_ssize_t n = recv((int)fd, buf, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;  /* WANT_READ */
    if (n == 0) { f->eof_seen = 1; return -1; }
    return n;
}

static int fake_at_eof(KlTls *self) { return ((FakeTls *)self)->eof_seen; }

static kl_ssize_t fake_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len)
{
    FakeTls *f = (FakeTls *)self;
    f->writes++;
    kl_ssize_t n = send((int)fd, buf, len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;  /* WANT_WRITE */
    return n;
}

static KlTlsResult fake_shutdown(KlTls *self, KlSocketHandle fd)
{
    (void)fd;
    ((FakeTls *)self)->shutdown_calls++;
    return KL_TLS_OK;
}

static size_t fake_pending_fn(KlTls *self) { return ((FakeTls *)self)->fake_pending; }
static void   fake_reset(KlTls *self)      { (void)self; }

static void fake_destroy(KlTls *self)
{
    FakeTls *f = (FakeTls *)self;
    if (f->destroyed_flag) *f->destroyed_flag = 1;
    free(f);
}

static int fake_set_hostname(KlTls *self, const char *host)
{
    FakeTls *f = (FakeTls *)self;
    f->hostname_calls++;
    snprintf(f->hostname, sizeof f->hostname, "%s", host ? host : "");
    return 0;
}

/* What the factory builds. Set per test, then read back through g_fake_last. */
static FakeTls  g_fake_template;
static FakeTls *g_fake_last;
static int      g_fake_no_hostname;   /* omit set_hostname from the vtable */

static KlTls *fake_factory(KlTlsCtx *ctx, KlAllocator *alloc)
{
    (void)ctx; (void)alloc;
    FakeTls *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    *f = g_fake_template;
    f->base.handshake    = fake_handshake;
    f->base.read         = fake_read;
    f->base.write        = fake_write;
    f->base.shutdown     = fake_shutdown;
    f->base.pending      = fake_pending_fn;
    f->base.reset        = fake_reset;
    f->base.destroy      = fake_destroy;
    f->base.at_eof       = fake_at_eof;
    f->base.set_hostname = g_fake_no_hostname ? NULL : fake_set_hostname;
    g_fake_last = f;
    return &f->base;
}

static KlAllocator g_fake_alloc;
static KlTlsConfig g_fake_tlscfg;

static void fake_tls_cfg(HlNetStreamConfig *cfg)
{
    memset(&g_fake_alloc, 0, sizeof g_fake_alloc);
    g_fake_alloc.malloc  = fake_alloc_fn;
    g_fake_alloc.realloc = fake_realloc_fn;
    g_fake_alloc.free    = fake_free_fn;
    memset(&g_fake_tlscfg, 0, sizeof g_fake_tlscfg);
    g_fake_tlscfg.factory = fake_factory;
    cfg->tls       = &g_fake_tlscfg;
    cfg->tls_alloc = &g_fake_alloc;
}

/* Connect with TLS wired, pumping to a terminal connect result. */
static int fix_open_tls(NetFix *f, HlNetStream **out, const char *sni, int *rc_out)
{
    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async        = f->ctx;
    cfg.pool         = f->pool;
    cfg.host         = "127.0.0.1";
    cfg.port         = f->port;
    cfg.connect_ms   = 5000;
    cfg.tls_hostname = sni;
    fake_tls_cfg(&cfg);

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    if (!s) return -1;
    int rc = pump_connect(f, s, 400);
    if (rc_out) *rc_out = rc;
    *out = s;
    return 0;
}

UTEST(net_stream, tls_handshake_completes_before_the_stream_is_usable)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_template.handshakes_left = 3;          /* three steps, not one  */
    /* WANT_WRITE, because a connected socket is writable and the loop will
     * therefore deliver readiness. A WANT_READ fake would wait on a peer that
     * never speaks - which is a real hang, and has its own test below. */
    g_fake_template.want = HL_ASYNC_WRITE;
    g_fake_no_hostname = 0;

    HlNetStream *s = NULL; int rc = 0;
    ASSERT_EQ(fix_open_tls(&f, &s, NULL, &rc), 0);
    EXPECT_EQ(rc, HL_NET_OK);
    /* It really took several steps rather than short-circuiting. */
    EXPECT_EQ(g_fake_last->handshakes_left, 0);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, a_stalled_handshake_times_out_rather_than_parking_forever)
{
    /* The connect OP's deadline is cancelled the moment the TCP connection
     * completes, and the handshake runs after that. Without a deadline of its
     * own, a peer that accepts and then says nothing would park the caller
     * forever. Found by a fake that asked for READ from a peer that never
     * sends - which is exactly the real case. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_template.handshakes_left = 1000000;
    g_fake_template.want = HL_ASYNC_READ;     /* nothing will ever arrive */
    g_fake_no_hostname = 0;

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async = f.ctx; cfg.pool = f.pool; cfg.host = "127.0.0.1";
    cfg.port = f.port;
    cfg.connect_ms = 120;                     /* short, so the test is quick */
    fake_tls_cfg(&cfg);

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    ASSERT_TRUE(s != NULL);

    int rc = pump_connect(&f, s, 400);
    EXPECT_EQ(rc, HL_NET_E_TIMEOUT);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, tls_connect_reports_again_while_the_handshake_runs)
{
    /* The whole reason the handshake runs inside the connect park: a caller
     * must not be handed a stream whose first write would go nowhere. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_template.handshakes_left = 1000000;    /* never finishes */
    g_fake_template.want = HL_ASYNC_READ;
    g_fake_no_hostname = 0;

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async = f.ctx; cfg.pool = f.pool; cfg.host = "127.0.0.1";
    cfg.port = f.port; cfg.connect_ms = 5000;
    fake_tls_cfg(&cfg);

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    ASSERT_TRUE(s != NULL);
    for (int i = 0; i < 20; i++) f.be->tick(f.ctx, 5);
    EXPECT_EQ(hl_net_stream_connect_result(s), HL_NET_E_AGAIN);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, a_failed_tls_handshake_is_reported_as_tls_not_io)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_template.handshake_fails = 1;
    g_fake_no_hostname = 0;

    HlNetStream *s = NULL; int rc = 0;
    ASSERT_EQ(fix_open_tls(&f, &s, NULL, &rc), 0);
    EXPECT_EQ(rc, HL_NET_E_TLS);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, tls_refuses_a_backend_that_cannot_verify_a_hostname)
{
    /* Negotiating an encrypted connection to a peer whose name was never
     * checked is the failure TLS exists to prevent, so it fails closed. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_no_hostname = 1;

    HlNetStream *s = NULL; int rc = 0;
    ASSERT_EQ(fix_open_tls(&f, &s, NULL, &rc), 0);
    EXPECT_EQ(rc, HL_NET_E_TLS);

    g_fake_no_hostname = 0;
    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, the_certificate_name_defaults_to_the_host)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_no_hostname = 0;

    HlNetStream *s = NULL; int rc = 0;
    ASSERT_EQ(fix_open_tls(&f, &s, NULL, &rc), 0);
    EXPECT_EQ(rc, HL_NET_OK);
    EXPECT_EQ(g_fake_last->hostname_calls, 1);
    EXPECT_STREQ(g_fake_last->hostname, "127.0.0.1");
    hl_net_stream_free(s);

    /* ...and an explicit name wins, which is what a caller reaching an
     * address while expecting a name needs. */
    HlNetStream *s2 = NULL;
    ASSERT_EQ(fix_open_tls(&f, &s2, "edge.example.com", &rc), 0);
    EXPECT_STREQ(g_fake_last->hostname, "edge.example.com");
    hl_net_stream_free(s2);

    fix_free(&f);
}

UTEST(net_stream, tls_moves_bytes_through_the_session_not_the_socket)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_no_hostname = 0;

    HlNetStream *s = NULL; int rc = 0;
    ASSERT_EQ(fix_open_tls(&f, &s, NULL, &rc), 0);
    ASSERT_EQ(rc, HL_NET_OK);
    ASSERT_EQ(fix_accept(&f), 0);

    ASSERT_EQ(hl_net_stream_write(s, "through-tls", 11), HL_NET_OK);
    char got[32] = {0};
    ASSERT_TRUE(recv(f.peer_fd, got, sizeof got, 0) == 11);
    EXPECT_STREQ(got, "through-tls");
    EXPECT_GT(g_fake_last->writes, 0);

    ASSERT_TRUE(send(f.peer_fd, "back-again", 10, 0) == 10);
    char in[32] = {0};
    long n = HL_NET_E_AGAIN;
    for (int i = 0; i < 200 && n == HL_NET_E_AGAIN; i++) {
        n = hl_net_stream_read(s, in, sizeof in);
        if (n == HL_NET_E_AGAIN) f.be->tick(f.ctx, 20);
    }
    ASSERT_EQ(n, 10L);
    EXPECT_STREQ(in, "back-again");
    EXPECT_GT(g_fake_last->reads, 0);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, a_write_during_the_handshake_is_queued_and_sent_after)
{
    /* OK from write means ADMITTED TO THE QUEUE, not "on the wire" - already
     * true for plaintext whenever the socket is busy. The handshake is the
     * same case: nothing may be encrypted yet, so the bytes wait, and the
     * flush that follows the handshake carries them. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_template.handshakes_left = 2;
    g_fake_template.want = HL_ASYNC_WRITE;
    g_fake_no_hostname = 0;

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async = f.ctx; cfg.pool = f.pool; cfg.host = "127.0.0.1";
    cfg.port = f.port; cfg.connect_ms = 5000;
    fake_tls_cfg(&cfg);

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    ASSERT_TRUE(s != NULL);

    /* Wait for the TCP connect, then write while the handshake is mid-flight. */
    for (int i = 0; i < 50 && hl_net_stream_connect_result(s) == HL_NET_E_AGAIN; i++) {
        if (hl_net_stream_write(s, "early", 5) == HL_NET_OK) break;
        f.be->tick(f.ctx, 5);
    }

    int rc = pump_connect(&f, s, 400);
    ASSERT_EQ(rc, HL_NET_OK);
    ASSERT_EQ(fix_accept(&f), 0);

    char got[16] = {0};
    ssize_t n = 0;
    for (int i = 0; i < 200 && n <= 0; i++) {
        n = recv(f.peer_fd, got, sizeof got, MSG_DONTWAIT);
        if (n <= 0) f.be->tick(f.ctx, 20);
    }
    EXPECT_TRUE(n == 5);
    EXPECT_STREQ(got, "early");

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, buffered_plaintext_is_drained_without_socket_readiness)
{
    /* The classic edge-triggered TLS stall: the engine decrypted several
     * records out of one segment, so the bytes sit inside the session and the
     * socket never becomes readable again. A reader that only armed a watcher
     * would wait forever.
     *
     * The peer sends NOTHING here, so anything read can only have come from
     * pending(), and no tick is pumped either. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_no_hostname = 0;

    HlNetStream *s = NULL; int rc = 0;
    ASSERT_EQ(fix_open_tls(&f, &s, NULL, &rc), 0);
    ASSERT_EQ(rc, HL_NET_OK);
    ASSERT_EQ(fix_accept(&f), 0);

    g_fake_last->fake_pending = 24;

    char in[64] = {0};
    long n = hl_net_stream_read(s, in, sizeof in);
    ASSERT_EQ(n, 24L);
    for (int i = 0; i < 24; i++) EXPECT_EQ((unsigned char)in[i], 0x50u);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, a_clean_tls_close_is_eof_not_a_transport_error)
{
    /* The mbedTLS adapter reports close_notify as -1 with at_eof set, not as
     * 0. Reading -1 as a hard error turned every finished response into
     * "transport error" - which the fake, written to the header's prose
     * rather than the adapter, happily agreed with until a live server did
     * not. The fake now follows the adapter. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_no_hostname = 0;

    HlNetStream *s = NULL; int rc = 0;
    ASSERT_EQ(fix_open_tls(&f, &s, NULL, &rc), 0);
    ASSERT_EQ(rc, HL_NET_OK);
    ASSERT_EQ(fix_accept(&f), 0);

    /* A real close_notify arrives on a socket that then closes, which is what
     * delivers the readiness that drives the read. */
    g_fake_last->clean_eof = 1;
    close(f.peer_fd); f.peer_fd = -1;

    char in[32];
    long n = HL_NET_E_AGAIN;
    for (int i = 0; i < 200 && n == HL_NET_E_AGAIN; i++) {
        n = hl_net_stream_read(s, in, sizeof in);
        if (n == HL_NET_E_AGAIN) f.be->tick(f.ctx, 20);
    }
    EXPECT_EQ(n, 0L);                       /* clean EOF, not HL_NET_E_IO */

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, a_tls_transport_error_is_still_an_error)
{
    /* The other half of the same branch: -1 WITHOUT at_eof must not be
     * mistaken for a tidy close. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_no_hostname = 0;

    HlNetStream *s = NULL; int rc = 0;
    ASSERT_EQ(fix_open_tls(&f, &s, NULL, &rc), 0);
    ASSERT_EQ(rc, HL_NET_OK);
    ASSERT_EQ(fix_accept(&f), 0);

    /* Peer vanishes and the session reports a hard failure. */
    close(f.peer_fd); f.peer_fd = -1;
    g_fake_last->hard_error = 1;

    char in[32];
    long n = HL_NET_E_AGAIN;
    for (int i = 0; i < 200 && n == HL_NET_E_AGAIN; i++) {
        n = hl_net_stream_read(s, in, sizeof in);
        if (n == HL_NET_E_AGAIN) f.be->tick(f.ctx, 20);
    }
    EXPECT_EQ(n, (long)HL_NET_E_IO);

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, closing_a_tls_stream_takes_the_session_with_it)
{
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    memset(&g_fake_template, 0, sizeof g_fake_template);
    g_fake_no_hostname = 0;
    int destroyed = 0;

    HlNetStream *s = NULL; int rc = 0;
    ASSERT_EQ(fix_open_tls(&f, &s, NULL, &rc), 0);
    ASSERT_EQ(rc, HL_NET_OK);
    g_fake_last->destroyed_flag = &destroyed;

    hl_net_stream_close(s);
    EXPECT_EQ(destroyed, 1);
    EXPECT_GT(g_fake_last->shutdown_calls, 0);   /* close_notify attempted */

    hl_net_stream_free(s);
    fix_free(&f);
}

UTEST(net_stream, asking_for_tls_without_the_means_is_refused_not_downgraded)
{
    /* A caller that wanted an encrypted stream and quietly got a plaintext one
     * would never find out. The creators in tls_transport.h return NULL when
     * TLS is not composed, so this is the path a TLS-less build takes. */
    NetFix f;
    ASSERT_EQ(fix_init(&f), 0);
    ASSERT_EQ(fix_listen(&f), 0);

    KlAllocator alloc;
    memset(&alloc, 0, sizeof alloc);
    alloc.malloc  = fake_alloc_fn;
    alloc.realloc = fake_realloc_fn;
    alloc.free    = fake_free_fn;

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async = f.ctx; cfg.pool = f.pool; cfg.host = "127.0.0.1";
    cfg.port = f.port; cfg.connect_ms = 5000;

    KlTlsConfig no_factory;
    memset(&no_factory, 0, sizeof no_factory);
    cfg.tls = &no_factory; cfg.tls_alloc = &alloc;
    HlNetStream *s = NULL;
    EXPECT_EQ(hl_net_stream_connect(&s, &cfg), HL_NET_E_INVAL);
    EXPECT_TRUE(s == NULL);

    KlTlsConfig with_factory;
    memset(&with_factory, 0, sizeof with_factory);
    with_factory.factory = fake_factory;
    cfg.tls = &with_factory; cfg.tls_alloc = NULL;      /* no allocator */
    EXPECT_EQ(hl_net_stream_connect(&s, &cfg), HL_NET_E_INVAL);
    EXPECT_TRUE(s == NULL);

    fix_free(&f);
}

UTEST(net_stream, the_seam_is_null_safe)
{
    ASSERT_EQ(hl_net_stream_from_op(NULL), NULL);
    ASSERT_EQ(hl_net_stream_user(NULL), NULL);
    hl_net_stream_set_user(NULL, (void *)0x1);   /* must not crash */
}

/* ── error strings ──────────────────────────────────────────────────── */

UTEST(net_stream, every_error_has_its_own_message)
{
    const int codes[] = {
        HL_NET_OK, HL_NET_E_DENIED, HL_NET_E_RESOLVE, HL_NET_E_CONNECT,
        HL_NET_E_TIMEOUT, HL_NET_E_CLOSED, HL_NET_E_CANCELLED, HL_NET_E_IO,
        HL_NET_E_NOMEM, HL_NET_E_INVAL, HL_NET_E_AGAIN,
    };
    const int n = (int)(sizeof codes / sizeof codes[0]);
    for (int i = 0; i < n; i++) {
        const char *a = hl_net_stream_strerror(codes[i]);
        ASSERT_NE(a, NULL);
        ASSERT_GT(strlen(a), (size_t)0);
        for (int j = i + 1; j < n; j++)
            ASSERT_NE(strcmp(a, hl_net_stream_strerror(codes[j])), 0);
    }
}

UTEST_MAIN()
