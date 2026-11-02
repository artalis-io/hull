/*
 * test_conn_limit.c - open connections per client and the header / idle
 * deadlines (hull/cap/conn_limit.h, audit convergence §6.4, confirming audit
 * M1). macOS loopback has only 127.0.0.1, so the per-client separation the
 * e2e cannot always show is driven here directly. The server side (the
 * decorating socket provider, the middleware, the shutdown of an expired
 * socket) is covered by tests/e2e_concurrency.sh.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/cap/conn_limit.h"

#include <string.h>

static HlReqKey key(const char *ip)
{
    HlReqKey k;
    if (hl_req_key_from_ip(ip, &k) != 0) memset(&k, 0xee, sizeof k);
    return k;
}

typedef struct { int fds[64]; int whys[64]; int n; } Expired;

static void on_expire(int fd, int why, void *user)
{
    Expired *e = (Expired *)user;
    if (e->n < 64) { e->fds[e->n] = fd; e->whys[e->n] = why; e->n++; }
}

/* One client holding its cap of idle connections does not keep another
 * client out (the confirming audit's attack: before, nothing counted them). */
UTEST(conn_convergence_cap, idle_connections_count_per_client)
{
    HlConnLimit l;
    ASSERT_EQ(hl_conn_limit_init(&l, 8, 4, 0, 0), 0);
    HlReqKey a = key("203.0.113.5"), b = key("198.51.100.9");
    for (int fd = 10; fd < 14; fd++) {
        EXPECT_EQ(hl_conn_limit_open(&l, fd, &a, 0), HL_CONN_ADMIT);
        hl_conn_limit_request(&l, fd);
        hl_conn_limit_response_done(&l, fd, 200, 0);   /* now idle */
    }
    EXPECT_EQ(hl_conn_limit_count(&l, &a), 4u);
    EXPECT_EQ(hl_conn_limit_open(&l, 14, &a, 0), HL_CONN_REFUSE);
    EXPECT_EQ(hl_conn_limit_open(&l, 15, &b, 0), HL_CONN_ADMIT);
    EXPECT_EQ(l.refused, (uint64_t)1);
    /* A close gives the slot back. */
    hl_conn_limit_close(&l, 10);
    EXPECT_EQ(hl_conn_limit_count(&l, &a), 3u);
    EXPECT_EQ(hl_conn_limit_open(&l, 16, &a, 0), HL_CONN_ADMIT);
    hl_conn_limit_free(&l);
}

/* The client key is the request limiter's: an IPv4-mapped address is the
 * IPv4 one, an IPv6 address counts by /64. */
UTEST(conn_convergence_cap, keys_follow_the_request_limiter)
{
    HlConnLimit l;
    ASSERT_EQ(hl_conn_limit_init(&l, 8, 2, 0, 0), 0);
    HlReqKey v4 = key("192.0.2.1"), mapped = key("::ffff:192.0.2.1");
    HlReqKey h1 = key("2001:db8:1:2::1"), h2 = key("2001:db8:1:2:ffff::9");
    HlReqKey other = key("2001:db8:1:3::1");
    EXPECT_EQ(hl_conn_limit_open(&l, 3, &v4, 0), HL_CONN_ADMIT);
    EXPECT_EQ(hl_conn_limit_open(&l, 4, &mapped, 0), HL_CONN_ADMIT);
    EXPECT_EQ(hl_conn_limit_open(&l, 5, &v4, 0), HL_CONN_REFUSE);
    EXPECT_EQ(hl_conn_limit_open(&l, 6, &h1, 0), HL_CONN_ADMIT);
    EXPECT_EQ(hl_conn_limit_open(&l, 7, &h2, 0), HL_CONN_ADMIT);
    EXPECT_EQ(hl_conn_limit_open(&l, 8, &h1, 0), HL_CONN_REFUSE);
    EXPECT_EQ(hl_conn_limit_open(&l, 9, &other, 0), HL_CONN_ADMIT);
    hl_conn_limit_free(&l);
}

/* An uncounted connection (a trusted proxy: NULL key) never uses the cap;
 * a cap of 0 counts nothing. */
UTEST(conn_convergence_cap, uncounted_and_off)
{
    HlConnLimit l;
    ASSERT_EQ(hl_conn_limit_init(&l, 8, 1, 0, 0), 0);
    for (int fd = 3; fd < 10; fd++)
        EXPECT_EQ(hl_conn_limit_open(&l, fd, NULL, 0), HL_CONN_ADMIT);
    hl_conn_limit_free(&l);

    ASSERT_EQ(hl_conn_limit_init(&l, 8, 0, 0, 0), 0);
    HlReqKey a = key("203.0.113.5");
    for (int fd = 3; fd < 20; fd++)
        EXPECT_EQ(hl_conn_limit_open(&l, fd, &a, 0), HL_CONN_ADMIT);
    hl_conn_limit_free(&l);
}

/* An upgraded WebSocket counts until it closes, and no deadline closes it. */
UTEST(conn_convergence_cap, websocket_counts_until_close)
{
    HlConnLimit l;
    ASSERT_EQ(hl_conn_limit_init(&l, 8, 1, 1000, 1000), 0);
    HlReqKey a = key("203.0.113.5");
    EXPECT_EQ(hl_conn_limit_open(&l, 5, &a, 0), HL_CONN_ADMIT);
    hl_conn_limit_request(&l, 5);
    hl_conn_limit_response_done(&l, 5, 101, 0);
    EXPECT_EQ(hl_conn_limit_phase(&l, 5), HL_CONN_WS);
    Expired e = { {0}, {0}, 0 };
    EXPECT_EQ(hl_conn_limit_sweep(&l, 100000, on_expire, &e), 0);
    EXPECT_EQ(hl_conn_limit_open(&l, 6, &a, 0), HL_CONN_REFUSE);
    hl_conn_limit_close(&l, 5);
    EXPECT_EQ(hl_conn_limit_open(&l, 6, &a, 0), HL_CONN_ADMIT);
    hl_conn_limit_free(&l);
}

/* The header deadline runs from the accept (or the first byte after idle)
 * and is not restarted by more bytes; a complete head stops it. */
UTEST(conn_convergence_cap, header_deadline_is_not_restarted_by_bytes)
{
    HlConnLimit l;
    ASSERT_EQ(hl_conn_limit_init(&l, 8, 0, 1000, 0), 0);
    EXPECT_EQ(hl_conn_limit_open(&l, 7, NULL, 0), HL_CONN_ADMIT);   /* drips */
    EXPECT_EQ(hl_conn_limit_open(&l, 8, NULL, 0), HL_CONN_ADMIT);   /* prompt */
    for (uint64_t t = 100; t < 1000; t += 100)
        hl_conn_limit_bytes_in(&l, 7, t);
    hl_conn_limit_request(&l, 8);
    Expired e = { {0}, {0}, 0 };
    EXPECT_EQ(hl_conn_limit_sweep(&l, 999, on_expire, &e), 0);
    EXPECT_EQ(hl_conn_limit_sweep(&l, 1000, on_expire, &e), 1);
    EXPECT_EQ(e.fds[0], 7);
    EXPECT_EQ(e.whys[0], HL_CONN_EXPIRE_HEADERS);
    /* Reported once: it waits for its close. */
    EXPECT_EQ(hl_conn_limit_sweep(&l, 5000, on_expire, &e), 0);
    EXPECT_EQ(hl_conn_limit_phase(&l, 8), HL_CONN_BUSY);
    hl_conn_limit_free(&l);
}

/* An idle keep-alive connection expires; bytes start a new request head
 * with the header deadline instead. */
UTEST(conn_convergence_cap, idle_deadline)
{
    HlConnLimit l;
    ASSERT_EQ(hl_conn_limit_init(&l, 8, 0, 5000, 500), 0);
    EXPECT_EQ(hl_conn_limit_open(&l, 3, NULL, 0), HL_CONN_ADMIT);
    EXPECT_EQ(hl_conn_limit_open(&l, 4, NULL, 0), HL_CONN_ADMIT);
    hl_conn_limit_request(&l, 3);
    hl_conn_limit_request(&l, 4);
    hl_conn_limit_response_done(&l, 3, 200, 1000);
    hl_conn_limit_response_done(&l, 4, 200, 1000);
    hl_conn_limit_bytes_in(&l, 4, 1200);            /* next request starts */
    Expired e = { {0}, {0}, 0 };
    EXPECT_EQ(hl_conn_limit_sweep(&l, 1500, on_expire, &e), 1);
    EXPECT_EQ(e.fds[0], 3);
    EXPECT_EQ(e.whys[0], HL_CONN_EXPIRE_IDLE);
    EXPECT_EQ(hl_conn_limit_phase(&l, 4), HL_CONN_HEADERS);
    hl_conn_limit_free(&l);
}

/* A descriptor reused without a close we saw drops the stale entry's count;
 * many keys fill and empty the count table correctly. */
UTEST(conn_convergence_cap, stale_entries_and_many_keys)
{
    HlConnLimit l;
    ASSERT_EQ(hl_conn_limit_init(&l, 600, 1, 0, 0), 0);
    HlReqKey a = key("203.0.113.5");
    EXPECT_EQ(hl_conn_limit_open(&l, 9, &a, 0), HL_CONN_ADMIT);
    EXPECT_EQ(hl_conn_limit_open(&l, 9, &a, 0), HL_CONN_ADMIT);   /* reused */
    EXPECT_EQ(hl_conn_limit_count(&l, &a), 1u);
    char ip[32];
    for (int i = 0; i < 500; i++) {
        snprintf(ip, sizeof ip, "10.0.%d.%d", i / 250, i % 250);
        HlReqKey k = key(ip);
        EXPECT_EQ(hl_conn_limit_open(&l, 100 + i, &k, 0), HL_CONN_ADMIT);
    }
    for (int i = 0; i < 500; i += 2) hl_conn_limit_close(&l, 100 + i);
    for (int i = 0; i < 500; i++) {
        snprintf(ip, sizeof ip, "10.0.%d.%d", i / 250, i % 250);
        HlReqKey k = key(ip);
        EXPECT_EQ(hl_conn_limit_count(&l, &k), (uint32_t)(i % 2));
    }
    hl_conn_limit_free(&l);
}

UTEST_MAIN()
