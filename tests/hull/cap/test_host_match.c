/*
 * test_host_match.c: host-allowlist pattern matching (hl_host_match)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/host_match.h"

#include <stdlib.h>
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
        EXPECT_FALSE(hl_host_match_any_env_n(pats, 3, h, strlen(h), 443));
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

UTEST(dsn_query_param, finds_each_occurrence_decoded)
{
    char v[64];
    const char *d = "mysql://u:p%3Fx@h/db?a=1&server_public_key=k%20y.pem&a=2&b";
    EXPECT_EQ(7, hl_dsn_query_param(d, "server_public_key", 0, v, sizeof v));
    EXPECT_STREQ("k y.pem", v);
    EXPECT_EQ(1, hl_dsn_query_param(d, "a", 1, v, sizeof v));
    EXPECT_STREQ("2", v);
    EXPECT_EQ(-1, hl_dsn_query_param(d, "a", 2, v, sizeof v));
    EXPECT_EQ(-1, hl_dsn_query_param(d, "b", 0, v, sizeof v));       /* no '=' */
    EXPECT_EQ(-1, hl_dsn_query_param(d, "server", 0, v, sizeof v));  /* exact key */
    EXPECT_EQ(-1, hl_dsn_query_param("mysql://u@h/db", "a", 0, v, sizeof v));
    EXPECT_EQ(-2, hl_dsn_query_param("mysql://u@h/db?a=%zz", "a", 0, v, sizeof v));
    EXPECT_EQ(-2, hl_dsn_query_param("mysql://u@h/db?a=x%00y", "a", 0, v, sizeof v));
    EXPECT_EQ(-2, hl_dsn_query_param("mysql://u@h/db?a=0123456789", "a", 0, v, 4));
}

/* ── #1227: a pattern may name the one port it admits ─────────────── */

UTEST(host_match_port, ported_pattern_admits_only_its_port)
{
    EXPECT_TRUE(hl_host_match_port("127.0.0.1:8081", "127.0.0.1", 8081));
    EXPECT_FALSE(hl_host_match_port("127.0.0.1:8081", "127.0.0.1", 8082));
    EXPECT_FALSE(hl_host_match_port("127.0.0.1:8081", "127.0.0.2", 8081));
    EXPECT_TRUE(hl_host_match_port("API.example.com:443", "api.EXAMPLE.com", 443));
    EXPECT_TRUE(hl_host_match_port("*.internal:9000", "a.b.internal", 9000));
    EXPECT_FALSE(hl_host_match_port("*.internal:9000", "a.b.internal", 9001));
    EXPECT_FALSE(hl_host_match_port("*.internal:9000", "internal", 9000));  /* apex */
    EXPECT_TRUE(hl_host_match_port("*:443", "anything.test", 443));
    EXPECT_FALSE(hl_host_match_port("*:443", "anything.test", 80));
    EXPECT_TRUE(hl_host_match_port("10.0.0.0/8:5432", "10.9.8.7", 5432));
    EXPECT_FALSE(hl_host_match_port("10.0.0.0/8:5432", "10.9.8.7", 5433));
    EXPECT_FALSE(hl_host_match_port("10.0.0.0/8:5432", "11.0.0.1", 5432));
}

UTEST(host_match_port, ipv6_takes_a_port_only_in_brackets)
{
    EXPECT_TRUE(hl_host_match_port("[::1]:8081", "::1", 8081));
    EXPECT_FALSE(hl_host_match_port("[::1]:8081", "::1", 8080));
    EXPECT_TRUE(hl_host_match_port("[2001:db8::/32]:443", "2001:db8::5", 443));
    EXPECT_FALSE(hl_host_match_port("[2001:db8::/32]:443", "2001:db8::5", 80));
    /* Bare IPv6: every ':' is the address's, so no port and any port. */
    EXPECT_TRUE(hl_host_match_port("::1", "::1", 8081));
    EXPECT_TRUE(hl_host_match_port("2001:db8::/32", "2001:db8::5", 22));
    /* Brackets with no port admit any port; brackets need IPv6. */
    EXPECT_TRUE(hl_host_match_port("[::1]", "::1", 1));
    EXPECT_FALSE(hl_host_pattern_valid("[127.0.0.1]:80"));
}

UTEST(host_match_port, bare_host_admits_every_port)
{
    EXPECT_TRUE(hl_host_match_port("127.0.0.1", "127.0.0.1", 1));
    EXPECT_TRUE(hl_host_match_port("127.0.0.1", "127.0.0.1", 65535));
    EXPECT_TRUE(hl_host_match_port("*", "x.test", 22));
}

UTEST(host_match_port, unknown_port_never_matches_a_ported_pattern)
{
    /* The port-less entry points (a peer address: trusted_proxies). */
    EXPECT_FALSE(hl_host_match("127.0.0.1:8081", "127.0.0.1"));
    EXPECT_FALSE(hl_host_match_port("127.0.0.1:8081", "127.0.0.1", 0));
    EXPECT_TRUE(hl_host_match("127.0.0.1", "127.0.0.1"));
}

/* Audit: a CIDR entry whose network or prefix cannot match anything passed
 * the load check (only the port was looked at), then matched nothing. */
UTEST(host_match_port, cidr_audit_a_malformed_cidr_is_invalid_at_load)
{
    static const char *const bad[] = {
        "1.2.3.4/33:80", "1.2.3.4/33", "[::1/200]:443", "::1/129",
        "1.2.3/8", "01.2.3.4/8", "1.2.3.4/", "1.2.3.4/+8", "1.2.3.4/ 8",
        "1.2.3.4/8x", "1.2.3.4/0008", "fe80::1%en0/64", "/8", "host/8",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        EXPECT_FALSE(hl_host_pattern_valid(bad[i]));
    EXPECT_TRUE(hl_host_pattern_valid("10.0.0.0/8"));
    EXPECT_TRUE(hl_host_pattern_valid("10.0.0.0/8:443"));
    EXPECT_TRUE(hl_host_pattern_valid("[2001:db8::/32]:443"));
    EXPECT_TRUE(hl_host_pattern_valid("2001:db8::/32"));
    EXPECT_TRUE(hl_host_match_port("10.0.0.0/8:443", "10.1.2.3", 443));
    EXPECT_FALSE(hl_host_match_port("1.2.3.4/+8", "1.2.3.4", 0));
}

UTEST(host_match_port, malformed_ports_match_nothing)
{
    static const char *const bad[] = {
        "h:", "h:0", "h:65536", "h:99999", "h:123456", "h:08", "h:8a",
        "h:-1", "h: 80", "[::1]:", "[::1]x", "[::1", "[]:80", ":80",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EXPECT_FALSE(hl_host_pattern_valid(bad[i]));
        EXPECT_FALSE(hl_host_match_port(bad[i], "h", 80));
    }
    EXPECT_TRUE(hl_host_pattern_valid("h:1"));
    EXPECT_TRUE(hl_host_pattern_valid("h:65535"));
    EXPECT_TRUE(hl_host_pattern_has_port("h:65535"));
    EXPECT_FALSE(hl_host_pattern_has_port("h"));
    EXPECT_FALSE(hl_host_pattern_has_port("::1"));
}

UTEST(host_match_port, env_ref_value_may_name_a_port)
{
    setenv("HL_TEST_HM_SIDECAR", "127.0.0.1:8081", 1);
    const char *pats[] = { "$HL_TEST_HM_SIDECAR" };
    EXPECT_TRUE(hl_host_match_any_env_port(pats, 1, "127.0.0.1", 8081));
    EXPECT_FALSE(hl_host_match_any_env_port(pats, 1, "127.0.0.1", 5432));
    EXPECT_TRUE(hl_host_match_any_env_n(pats, 1, "127.0.0.1xyz", 9, 8081));
    unsetenv("HL_TEST_HM_SIDECAR");
}

UTEST(dsn_host_port, reads_the_authority_port)
{
    char h[64];
    int port = -1;
    EXPECT_TRUE(hl_dsn_host_port("postgres://u:p@db.local:5433/app", h, sizeof h, &port));
    EXPECT_STREQ("db.local", h);
    EXPECT_EQ(5433, port);
    EXPECT_TRUE(hl_dsn_host_port("valkey://cache", h, sizeof h, &port));
    EXPECT_EQ(0, port);                        /* none: caller's default */
    EXPECT_TRUE(hl_dsn_host_port("mysql://[::1]:3307/x", h, sizeof h, &port));
    EXPECT_STREQ("::1", h);
    EXPECT_EQ(3307, port);
    EXPECT_FALSE(hl_dsn_host_port("postgres://h:0/x", h, sizeof h, &port));
    EXPECT_FALSE(hl_dsn_host_port("postgres://h:65536/x", h, sizeof h, &port));
    EXPECT_FALSE(hl_dsn_host_port("postgres://h:99999999999999999999/x", h, sizeof h, &port));
}

UTEST_MAIN()
