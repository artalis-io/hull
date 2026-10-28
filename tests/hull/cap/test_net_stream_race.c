/*
 * test_net_stream_race.c - a Happy-Eyeballs race leaves no watcher behind.
 *
 * Audit 33: when one connect attempt won while another was still in flight,
 * co_on_done -> retire_attempts closed the losing descriptor WITHOUT removing
 * its attempt_ready watcher, and the op's cancel_attempt then found the slot
 * already empty. The registration stayed on a closed fd number: on epoll the
 * next socket to get that number could not be watched (EPOLL_CTL_MOD on an fd
 * the kernel dropped fails ENOENT), and on poll its readiness reached a
 * callback for the stream.
 *
 * The race needs a name with two addresses where the first stalls past the
 * 250 ms stagger and the second answers. This binary defines hl_net_resolve
 * itself (cap_net_resolve.o is left out of its link, mk/tests.mk) and returns
 * a blackhole address (192.0.2.1, TEST-NET-1) first and a loopback listener
 * second. Where the network refuses TEST-NET-1 synchronously (no route), the
 * first attempt never parks and there is no race to test: the case skips.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/cap/net_resolve.h"
#include "hull/cap/net_stream.h"
#include "hull/shared/async_backend.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Fake resolver ──────────────────────────────────────────────────── */

int hl_net_resolve(const char *host, int port, KlSockAddr *out, int max)
{
    if (!host || !out || max < 2 || port < 1 || port > 65535) return 0;
    if (kl_sockaddr_parse(&out[0], "192.0.2.1", (uint16_t)port) != 0) return 0;
    if (kl_sockaddr_parse(&out[1], "127.0.0.1", (uint16_t)port) != 0) return 0;
    return 2;
}

/* ── A backend that records which fds are watched ───────────────────── */

#define MAX_FD 4096

static const HlAsyncBackend *g_base;
static HlAsyncBackend        g_wrap;
static unsigned char         g_live[MAX_FD];
static int                   g_first_add = -1;   /* attempt 0's fd, if it parked */

static int rec_watcher_add(HlAsyncBackendCtx *c, int fd, unsigned mask,
                           HlAsyncWatcherFn cb, void *user)
{
    int rc = g_base->watcher_add(c, fd, mask, cb, user);
    if (rc == 0 && fd >= 0 && fd < MAX_FD) {
        g_live[fd] = 1;
        if (g_first_add < 0) g_first_add = fd;
    }
    return rc;
}

static void rec_watcher_del(HlAsyncBackendCtx *c, int fd)
{
    if (fd >= 0 && fd < MAX_FD) g_live[fd] = 0;
    g_base->watcher_del(c, fd);
}

static void wrap_backend(const HlAsyncBackend *base)
{
    g_base = base;
    g_wrap = *base;
    g_wrap.watcher_add = rec_watcher_add;
    g_wrap.watcher_del = rec_watcher_del;
    memset(g_live, 0, sizeof g_live);
    g_first_add = -1;
}

static int g_fired;
static void *g_fired_user;
static void reuse_cb(int fd, unsigned ready, void *user)
{
    (void)fd; (void)ready;
    g_fired++;
    g_fired_user = user;
}

extern const HlAsyncBackend hl_async_backend_poll;

/* Returns 0 on success, -1 to skip (no race on this network), or the number
 * of the step that failed. */
static int race_case(const HlAsyncBackend *base)
{
    HlAsyncBackendCtx  *ctx  = NULL;
    HlAsyncBackendPool *pool = NULL;
    HlNetStream *s = NULL;
    int step = 0, lfd = -1, sp[2] = { -1, -1 }, pinned = 0;
    int marker = 0;

    wrap_backend(base);
    if (g_wrap.init(&ctx, NULL) != 0 ||
        g_wrap.pool_create(&pool, ctx, 2, 16) != 0) { step = 1; goto out; }

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof a;
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&a, &alen) != 0) { step = 2; goto out; }

    HlNetStreamConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = ctx;
    cfg.backend    = &g_wrap;
    cfg.pool       = pool;
    cfg.host       = "dual-stack.race.test";
    cfg.port       = ntohs(a.sin_port);
    cfg.connect_ms = 5000;

    hl_net_stream_connect(&s, &cfg);
    if (!s) { step = 3; goto out; }
    int rc = hl_net_stream_connect_result(s);
    for (int i = 0; i < 200 && rc == HL_NET_E_AGAIN; i++) {
        g_wrap.tick(ctx, 20);
        rc = hl_net_stream_connect_result(s);
    }
    if (rc != HL_NET_OK) { step = 4; goto out; }

    /* Did the blackhole attempt park? Its descriptor never connected, so if
     * the first watched fd is still open it must not have a peer; if it is
     * already closed, it was retired - the race this test is about. */
    if (g_first_add < 0) { step = -1; goto out; }
    {
        struct sockaddr_in pa;
        socklen_t plen = sizeof pa;
        if (fcntl(g_first_add, F_GETFD) != -1 &&
            getpeername(g_first_add, (struct sockaddr *)&pa, &plen) == 0) {
            step = -1;      /* the first watched fd is the winner: no race */
            goto out;
        }
    }

    /* 1. No watcher remains on a closed descriptor. */
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (g_live[fd] && fcntl(fd, F_GETFD) == -1) { step = 5; goto out; }
    }

    /* 2. A new socket that takes the losing attempt's NUMBER can be watched
     *    and its readiness reaches its own callback. dup2 pins the number;
     *    no tick first, so poll's POLLNVAL sweep cannot hide a stale entry. */
    if (fcntl(g_first_add, F_GETFD) != -1) { step = 6; goto out; }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0 ||
        dup2(sp[0], g_first_add) != g_first_add) { step = 7; goto out; }
    pinned = 1;
    g_fired = 0;
    g_fired_user = NULL;
    if (g_wrap.watcher_add(ctx, g_first_add, HL_ASYNC_READ, reuse_cb, &marker) != 0) {
        step = 8; goto out;
    }
    if (write(sp[1], "x", 1) != 1) { step = 9; goto out; }
    for (int i = 0; i < 50 && !g_fired; i++) g_wrap.tick(ctx, 20);
    if (g_fired < 1 || g_fired_user != (void *)&marker) step = 10;
    g_wrap.watcher_del(ctx, g_first_add);

out:
    if (s) { hl_net_stream_close(s); hl_net_stream_free(s); }
    if (pinned) close(g_first_add);
    if (sp[0] >= 0) close(sp[0]);
    if (sp[1] >= 0) close(sp[1]);
    if (lfd >= 0) close(lfd);
    /* Pool first: it drains and joins while the loop it completes on lives. */
    if (pool) g_wrap.pool_free(pool);
    if (ctx) g_wrap.free(ctx);
    return step;
}

UTEST(net_stream_race, losing_attempt_leaves_no_watcher_poll)
{
    int rc = race_case(&hl_async_backend_poll);
    if (rc == -1) { UTEST_SKIP("TEST-NET-1 refused synchronously: no connect race"); }
    EXPECT_EQ(rc, 0);
}

UTEST(net_stream_race, losing_attempt_leaves_no_watcher_linked)
{
    int rc = race_case(hl_async_backend());
    if (rc == -1) { UTEST_SKIP("TEST-NET-1 refused synchronously: no connect race"); }
    EXPECT_EQ(rc, 0);
}

UTEST_MAIN()
