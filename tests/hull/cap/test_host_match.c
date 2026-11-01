/*
 * test_host_match.c: host-allowlist pattern matching (hl_host_match)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/host_match.h"

#include <string.h>

UTEST(host_match, exact_case_insensitive)
{
    ASSERT_TRUE(hl_host_match("db.example.com", "db.example.com"));
    ASSERT_TRUE(hl_host_match("db.example.com", "DB.Example.COM"));
    ASSERT_FALSE(hl_host_match("db.example.com", "db.example.org"));
    ASSERT_FALSE(hl_host_match("db.example.com", "x.db.example.com"));
}

UTEST(host_match, any)
{
    ASSERT_TRUE(hl_host_match("*", "anything.at.all"));
    ASSERT_TRUE(hl_host_match("*", "10.1.2.3"));
}

UTEST(host_match, subdomain_glob)
{
    /* Any-depth subdomain (RDS endpoints are multi-label). */
    ASSERT_TRUE(hl_host_match("*.rds.amazonaws.com", "db.abc.us-east-1.rds.amazonaws.com"));
    ASSERT_TRUE(hl_host_match("*.example.com", "a.example.com"));
    ASSERT_TRUE(hl_host_match("*.example.com", "A.Example.Com"));
    /* Apex is NOT matched by "*.example.com". */
    ASSERT_FALSE(hl_host_match("*.example.com", "example.com"));
    /* Different domain. */
    ASSERT_FALSE(hl_host_match("*.example.com", "example.com.evil.org"));
    ASSERT_FALSE(hl_host_match("*.example.com", "notexample.com"));
}

UTEST(host_match, cidr_v4)
{
    ASSERT_TRUE(hl_host_match("10.0.0.0/8", "10.1.2.3"));
    ASSERT_TRUE(hl_host_match("10.0.0.0/8", "10.255.255.255"));
    ASSERT_FALSE(hl_host_match("10.0.0.0/8", "11.0.0.1"));
    ASSERT_TRUE(hl_host_match("192.168.1.0/24", "192.168.1.42"));
    ASSERT_FALSE(hl_host_match("192.168.1.0/24", "192.168.2.42"));
    ASSERT_TRUE(hl_host_match("127.0.0.1/32", "127.0.0.1"));
    ASSERT_FALSE(hl_host_match("127.0.0.1/32", "127.0.0.2"));
    /* A hostname never matches a CIDR (no DNS resolution). */
    ASSERT_FALSE(hl_host_match("10.0.0.0/8", "db.internal"));
}

UTEST(host_match, cidr_v6)
{
    ASSERT_TRUE(hl_host_match("2001:db8::/32", "2001:db8:1:2::3"));
    ASSERT_FALSE(hl_host_match("2001:db8::/32", "2001:db9::1"));
    /* v4 host against a v6 CIDR (and vice versa) does not match. */
    ASSERT_FALSE(hl_host_match("2001:db8::/32", "10.1.2.3"));
    ASSERT_FALSE(hl_host_match("10.0.0.0/8", "2001:db8::1"));
}

UTEST(host_match, malformed_and_null)
{
    ASSERT_FALSE(hl_host_match(NULL, "x"));
    ASSERT_FALSE(hl_host_match("x", NULL));
    ASSERT_FALSE(hl_host_match("10.0.0.0/", "10.1.2.3"));    /* no prefix */
    ASSERT_FALSE(hl_host_match("10.0.0.0/99", "10.1.2.3"));  /* prefix too big */
    ASSERT_FALSE(hl_host_match("notanip/8", "10.1.2.3"));    /* bad network */
}

UTEST(host_match, match_any)
{
    const char *pats[] = { "*.example.com", "10.0.0.0/8", "exact.host" };
    ASSERT_TRUE(hl_host_match_any(pats, 3, "a.example.com"));
    ASSERT_TRUE(hl_host_match_any(pats, 3, "10.9.9.9"));
    ASSERT_TRUE(hl_host_match_any(pats, 3, "exact.host"));
    ASSERT_FALSE(hl_host_match_any(pats, 3, "nope.org"));
    ASSERT_FALSE(hl_host_match_any(pats, 0, "a.example.com"));
}

/* hl_dsn_host refuses any DSN a backend could parse to a different host than
 * the check saw. */
UTEST(host_match, dsn_host_plain)
{
    char h[256];
    ASSERT_EQ(1, hl_dsn_host("postgres://u:p@db.example.com:5432/x", h, sizeof h));
    ASSERT_STREQ("db.example.com", h);
    ASSERT_EQ(1, hl_dsn_host("redis://cache.local", h, sizeof h));
    ASSERT_STREQ("cache.local", h);
    ASSERT_EQ(1, hl_dsn_host("mysql://[::1]:3306/x", h, sizeof h));
    ASSERT_STREQ("::1", h);
    ASSERT_EQ(1, hl_dsn_host("postgres://u%40corp:p@db.example.com/x?sslmode=require", h, sizeof h));
    ASSERT_STREQ("db.example.com", h);
}

UTEST(host_match, dsn_host_refuses_ambiguity)
{
    char h[256];
    /* Two '@': the checkers took the last, Postgres/MySQL take the first. */
    EXPECT_EQ(0, hl_dsn_host("postgres://u@169.254.169.254%00@db.example.com/x", h, sizeof h));
    EXPECT_EQ(0, hl_dsn_host("postgres://u@evil.com@db.example.com/x", h, sizeof h));
    /* A '#': the Valkey parser does not end the authority there. */
    EXPECT_EQ(0, hl_dsn_host("valkey://cache.internal#@10.0.0.9:6379", h, sizeof h));
    /* Percent-encoding or odd bytes in the host. */
    EXPECT_EQ(0, hl_dsn_host("postgres://evil.com%00.rds.amazonaws.com/x", h, sizeof h));
    EXPECT_EQ(0, hl_dsn_host("postgres://db.example.com\t/x", h, sizeof h));
    /* A port that is not digits; no host. */
    EXPECT_EQ(0, hl_dsn_host("redis://cache.local:63x9", h, sizeof h));
    EXPECT_EQ(0, hl_dsn_host("redis://cache.local:", h, sizeof h));
    EXPECT_EQ(0, hl_dsn_host("redis:///0", h, sizeof h));
    EXPECT_EQ(0, hl_dsn_host("not a dsn", h, sizeof h));
}

/* Audit 42: an IP literal a resolver may read differently from the check
 * (Cosmopolitan: inet_pton decimal, getaddrinfo octal for "0177.0.0.1")
 * matches nothing - no CIDR, no exact entry spelled the same, not even "*". */
static const char *const A42_NONCANON[] = {
    "0177.0.0.1", "012.0.0.1", "010.1.2.3", "10.0.0.010", "177.0.0.01",
    "00.0.0.0", "0x7f.0.0.1", "0X7F.0.0.1", "0xb1.0.0.1", "0x7f000001",
    "127.1", "177.1", "10.1.258", "2130706433", "2969567233", "1.2.3.4.",
    "177.0.0.1.", "1.2.3.4.5", "1234.0.0.1", "256.0.0.1", "08.1.2.3",
    "10.00000001.2.3", "0", "177",
    /* IPv6 spellings */
    "::ffff:0177.0.0.1", "::ffff:010.1.2.3", "::ffff:127.1", "2001:db8::00001",
    "2001:db8:::1", ":2001:db8::1", "2001:db8::1:", "1:2:3:4:5:6:7:8:9",
    "1:2:3:4:5:6:7", "1::2::3", "::ffff:0x7f.0.0.1", "fe80::1%", "fe80::1%a/b",
    "2001:db8::g", ":",
    NULL,
};

UTEST(host_match_audit42, noncanonical_refuses_cidr)
{
    for (int i = 0; A42_NONCANON[i]; i++) {
        const char *h = A42_NONCANON[i];
        EXPECT_FALSE(hl_host_literal_ok(h));
        EXPECT_FALSE(hl_host_match("177.0.0.0/8", h));
        EXPECT_FALSE(hl_host_match("10.0.0.0/8", h));
        EXPECT_FALSE(hl_host_match("127.0.0.0/8", h));
        EXPECT_FALSE(hl_host_match("0.0.0.0/0", h));
        EXPECT_FALSE(hl_host_match("::/0", h));
        EXPECT_FALSE(hl_host_match("2001:db8::/32", h));
    }
}

UTEST(host_match_audit42, noncanonical_refuses_exact_glob_any)
{
    for (int i = 0; A42_NONCANON[i]; i++) {
        const char *h = A42_NONCANON[i];
        EXPECT_FALSE(hl_host_match(h, h));      /* exact, spelled the same */
        EXPECT_FALSE(hl_host_match("*", h));
        EXPECT_FALSE(hl_host_match("*.0.0.1", h));
        const char *pats[] = { "$A42_UNSET_VAR", h, "*" };
        EXPECT_FALSE(hl_host_match_any_env(pats, 3, h));
        EXPECT_FALSE(hl_host_match_any_env_n(pats, 3, h, strlen(h)));
    }
}

UTEST(host_match_audit42, canonical_still_matches)
{
    ASSERT_TRUE(hl_host_match("177.0.0.0/8", "177.0.0.1"));
    ASSERT_TRUE(hl_host_match("10.0.0.0/8", "10.1.2.3"));
    ASSERT_TRUE(hl_host_match("0.0.0.0/0", "0.0.0.0"));
    ASSERT_TRUE(hl_host_match("0.0.0.0/0", "255.255.255.255"));
    ASSERT_TRUE(hl_host_match("10.0.0.0/8", "10.0.0.100"));
    ASSERT_TRUE(hl_host_match("127.0.0.1", "127.0.0.1"));
    ASSERT_TRUE(hl_host_match("*", "127.0.0.1"));
    ASSERT_TRUE(hl_host_match("::/0", "::"));
    ASSERT_TRUE(hl_host_match("::/0", "::1"));
    ASSERT_TRUE(hl_host_match("2001:db8::/32", "2001:DB8:0:0:0:0:0:1"));
    ASSERT_TRUE(hl_host_match("2001:db8::/32", "2001:db8::0001"));
    ASSERT_TRUE(hl_host_match("::ffff:0:0/96", "::ffff:10.1.2.3"));
    ASSERT_TRUE(hl_host_match("1::/16", "1::"));
    ASSERT_TRUE(hl_host_match("1:2:3:4:5:6:1.2.3.4", "1:2:3:4:5:6:1.2.3.4"));
    ASSERT_TRUE(hl_host_match("fe80::1%eth0", "fe80::1%eth0"));
    ASSERT_TRUE(hl_host_match("*", "fe80::1%25eth0"));
    /* A zone never matches a CIDR (as before). */
    ASSERT_FALSE(hl_host_match("fe80::/10", "fe80::1%eth0"));
    /* DNS names with numeric labels are names, not literals. */
    ASSERT_TRUE(hl_host_literal_ok("1e100.net"));
    ASSERT_TRUE(hl_host_literal_ok("0x7f.example.com"));
    ASSERT_TRUE(hl_host_match("*.example.com", "10.0.0.1.example.com"));
    ASSERT_TRUE(hl_host_match("1password.com", "1password.com"));
    ASSERT_FALSE(hl_host_match("10.0.0.0/8", "10.0.0.1.example.com"));
}

UTEST(host_match_audit42, noncanonical_cidr_network_refused)
{
    /* The network side of a grant must be canonical too. */
    ASSERT_FALSE(hl_host_match("0177.0.0.0/8", "177.0.0.1"));
    ASSERT_FALSE(hl_host_match("0177.0.0.0/8", "127.0.0.1"));
    ASSERT_FALSE(hl_host_match("10.1/16", "10.1.0.1"));
    ASSERT_FALSE(hl_host_match("fe80::%eth0/10", "fe80::1"));
}

UTEST_MAIN()
