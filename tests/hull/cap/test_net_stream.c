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

/* ── Fixture ────────────────────────────────────────────────────────── */

typedef struct {
    const HlAsyncBackend *be;
    HlAsyncBackendCtx    *ctx;
    int                   listen_fd;
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
    f->be = hl_async_backend();
    if (!f->be) return -1;
    if (f->be->init(&f->ctx, NULL) != 0) return -1;
    return 0;
}

static void fix_free(NetFix *f)
{
    if (f->listen_fd >= 0) close(f->listen_fd);
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
    cfg.host       = "127.0.0.1";
    cfg.port       = f.port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    int rc = hl_net_stream_connect(&s, &cfg);
    ASSERT_NE(s, NULL);
    /* Either shape is legal: loopback often completes synchronously. */
    ASSERT_TRUE(rc == HL_NET_OK || rc == HL_NET_E_AGAIN);

    ASSERT_EQ(pump_connect(&f, s, 200), HL_NET_OK);

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
    cfg.host       = "127.0.0.1";
    cfg.port       = port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    int rc = hl_net_stream_connect(&s, &cfg);
    if (s) {
        rc = pump_connect(&f, s, 200);
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
    cfg.host       = "no-such-host.invalid";   /* .invalid is reserved, RFC 2606 */
    cfg.port       = 80;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    int rc = hl_net_stream_connect(&s, &cfg);
    if (s) {
        rc = pump_connect(&f, s, 200);
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
    cfg.host       = "127.0.0.1";
    cfg.port       = f.port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    ASSERT_NE(s, NULL);
    hl_net_stream_free(s);          /* no pump: teardown races the connect */

    /* Keep ticking afterwards: a late callback into freed storage would show
     * up here under ASan rather than silently. */
    for (int i = 0; i < 20; i++) f.be->tick(f.ctx, 5);
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

    for (int i = 0; i < 20; i++) f.be->tick(f.ctx, 5);
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
    cfg.host       = "127.0.0.1";
    cfg.port       = f.port;
    cfg.connect_ms = 5000;

    HlNetStream *s = NULL;
    hl_net_stream_connect(&s, &cfg);
    ASSERT_NE(s, NULL);
    ASSERT_EQ(pump_connect(&f, s, 200), HL_NET_OK);

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
