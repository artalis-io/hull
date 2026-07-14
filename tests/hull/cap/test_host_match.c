/*
 * test_host_match.c: host-allowlist pattern matching (hl_host_match)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/host_match.h"

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

UTEST_MAIN()
