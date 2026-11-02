/*
 * test_req_limit.c - the request concurrency limiter (hull/shared/req_limit.h,
 * audit convergence §6.4): client keys, the trusted-proxy X-Forwarded-For
 * rule, route matching, admission / release and the sweep of connections
 * that are gone. The server side (Keel middleware, 429 / 503) is covered by
 * tests/e2e_concurrency.sh.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/shared/req_limit.h"

#include <string.h>

static HlReqKey key(const char *ip)
{
    HlReqKey k;
    if (hl_req_key_from_ip(ip, &k) != 0) memset(&k, 0xee, sizeof k);
    return k;
}

static int same(const char *a, const char *b)
{
    HlReqKey ka = key(a), kb = key(b);
    return memcmp(&ka, &kb, sizeof ka) == 0;
}

UTEST(req_limit, keys)
{
    HlReqKey k;
    EXPECT_EQ(hl_req_key_from_ip("192.0.2.1", &k), 0);
    EXPECT_EQ(hl_req_key_from_ip("2001:db8::1", &k), 0);
    EXPECT_EQ(hl_req_key_from_ip("not-an-ip", &k), -1);
    EXPECT_EQ(hl_req_key_from_ip("", &k), -1);
    EXPECT_EQ(hl_req_key_from_ip(NULL, &k), -1);
    /* IPv4 as itself, an IPv4-mapped IPv6 address as its IPv4 */
    EXPECT_TRUE(same("192.0.2.1", "::ffff:192.0.2.1"));
    EXPECT_FALSE(same("192.0.2.1", "192.0.2.2"));
    /* IPv6 by /64 */
    EXPECT_TRUE(same("2001:db8:1:2::1", "2001:db8:1:2:ffff::9"));
    EXPECT_FALSE(same("2001:db8:1:2::1", "2001:db8:1:3::1"));
}

UTEST(req_limit, xff_last_entry)
{
    char out[64];
    EXPECT_EQ(hl_req_xff_last("1.1.1.1, 2.2.2.2", 16, out, sizeof out), 1);
    EXPECT_STREQ(out, "2.2.2.2");
    const char *t = "  9.9.9.9 \t";
    EXPECT_EQ(hl_req_xff_last(t, strlen(t), out, sizeof out), 1);
    EXPECT_STREQ(out, "9.9.9.9");
    const char *v6 = "spoofed, 2001:db8::7";
    EXPECT_EQ(hl_req_xff_last(v6, strlen(v6), out, sizeof out), 1);
    EXPECT_STREQ(out, "2001:db8::7");
    /* the last entry must be an address: anything else is not a key */
    const char *bad = "1.1.1.1, c";
    EXPECT_EQ(hl_req_xff_last(bad, strlen(bad), out, sizeof out), 0);
    EXPECT_EQ(hl_req_xff_last("1.1.1.1,", 8, out, sizeof out), 0);
    EXPECT_EQ(hl_req_xff_last("", 0, out, sizeof out), 0);
    /* longer than the buffer: refused, never truncated into another address */
    char big[200];
    memset(big, '1', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    EXPECT_EQ(hl_req_xff_last(big, strlen(big), out, sizeof out), 0);
}

UTEST(req_limit, route_match)
{
    EXPECT_TRUE(hl_req_route_match("/upload", "/upload", 7));
    EXPECT_TRUE(hl_req_route_match("/upload", "/upload/", 8));
    EXPECT_FALSE(hl_req_route_match("/upload", "/uploads", 8));
    EXPECT_TRUE(hl_req_route_match("/files/*", "/files", 6));
    EXPECT_TRUE(hl_req_route_match("/files/*", "/files/a/b", 10));
    EXPECT_FALSE(hl_req_route_match("/files/*", "/filesx", 7));
    EXPECT_FALSE(hl_req_route_match("/files/*", "/fil", 4));
}

/* conns are any distinct addresses */
static char conns[16];

UTEST(req_limit, per_client_cap_and_release)
{
    HlReqLimiter l;
    ASSERT_EQ(hl_req_limit_init(&l, 16, 3, NULL, 0), 0);
    HlReqKey a = key("10.0.0.1"), b = key("10.0.0.2");
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[0], &a, -1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[1], &a, -1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[2], &a, -1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[3], &a, -1), HL_REQ_REFUSE_CLIENT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[4], &b, -1), HL_REQ_ADMIT);
    /* the refused request recorded nothing; one release makes room */
    hl_req_limit_release(&l, &conns[1]);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[3], &a, -1), HL_REQ_ADMIT);
    /* a new request on a connection replaces its old entry */
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[0], &a, -1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[5], &a, -1), HL_REQ_REFUSE_CLIENT);
    EXPECT_EQ(l.refused_ip, 2u);
    hl_req_limit_free(&l);
}

UTEST(req_limit, per_client_off)
{
    HlReqLimiter l;
    ASSERT_EQ(hl_req_limit_init(&l, 16, 0, NULL, 0), 0);
    HlReqKey a = key("10.0.0.1");
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(hl_req_limit_admit(&l, &conns[i], &a, -1), HL_REQ_ADMIT);
    hl_req_limit_free(&l);
}

UTEST(req_limit, route_cap)
{
    static const uint32_t rmax[2] = { 2, 5 };
    HlReqLimiter l;
    ASSERT_EQ(hl_req_limit_init(&l, 16, 8, rmax, 2), 0);
    HlReqKey a = key("10.0.0.1"), b = key("10.0.0.2"), c = key("10.0.0.3");
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[0], &a, 0), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[1], &b, 0), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[2], &c, 0), HL_REQ_REFUSE_ROUTE);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[2], &c, 1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[3], &c, -1), HL_REQ_ADMIT);
    /* an out-of-range index is "no route" */
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[4], &c, 7), HL_REQ_ADMIT);
    EXPECT_EQ(l.refused_route, 1u);
    hl_req_limit_free(&l);
}

static int dead_mask;
static int alive_cb(const void *conn, void *user)
{
    (void)user;
    int i = (int)((const char *)conn - conns);
    return !(dead_mask & (1 << i));
}

UTEST(req_limit, gone_connections_are_swept)
{
    HlReqLimiter l;
    ASSERT_EQ(hl_req_limit_init(&l, 16, 2, NULL, 0), 0);
    l.alive = alive_cb;
    dead_mask = 0;
    HlReqKey a = key("10.0.0.1");
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[0], &a, -1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[1], &a, -1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[2], &a, -1), HL_REQ_REFUSE_CLIENT);
    dead_mask = 1 << 0;   /* conn 0 closed while its response was in flight */
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[2], &a, -1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[3], &a, -1), HL_REQ_REFUSE_CLIENT);
    hl_req_limit_free(&l);
}

UTEST(req_limit, full_table_admits_uncounted)
{
    HlReqLimiter l;
    ASSERT_EQ(hl_req_limit_init(&l, 2, 8, NULL, 0), 0);
    HlReqKey a = key("10.0.0.1");
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[0], &a, -1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[1], &a, -1), HL_REQ_ADMIT);
    EXPECT_EQ(hl_req_limit_admit(&l, &conns[2], &a, -1), HL_REQ_ADMIT);
    hl_req_limit_free(&l);
}

UTEST_MAIN()
