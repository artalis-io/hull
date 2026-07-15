/*
 * test_http.c — Unit tests for HTTP client capability (no network)
 *
 * Tests host allowlist checking. URL parsing and response parser tests
 * are now in Keel (tests/test_url.c, tests/test_response_parser.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/http.h"

#include <string.h>
#include <stdlib.h>

/* ════════════════════════════════════════════════════════════════════
 * Host allowlist tests
 * ════════════════════════════════════════════════════════════════════ */

UTEST(host, allowed)
{
    const char *hosts[] = { "api.example.com", "example.org" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 2 };
    ASSERT_EQ(0, hl_http_check_host(&cfg, "api.example.com", 15));
}

UTEST(host, denied)
{
    const char *hosts[] = { "api.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    ASSERT_NE(0, hl_http_check_host(&cfg, "evil.com", 8));
}

UTEST(host, case_insensitive)
{
    const char *hosts[] = { "API.Example.COM" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    ASSERT_EQ(0, hl_http_check_host(&cfg, "api.example.com", 15));
}

UTEST(host, empty_list)
{
    HlHttpConfig cfg = { .allowed_hosts = NULL, .count = 0 };
    ASSERT_NE(0, hl_http_check_host(&cfg, "example.com", 11));
}

UTEST(host, partial_match_rejected)
{
    const char *hosts[] = { "example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    /* "evil-example.com" should NOT match "example.com" */
    ASSERT_NE(0, hl_http_check_host(&cfg, "evil-example.com", 16));
}

/* §2.8: the host gate now delegates to the shared glob/CIDR matcher. */
UTEST(host, wildcard_subdomain)
{
    const char *hosts[] = { "*.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    ASSERT_EQ(0, hl_http_check_host(&cfg, "a.example.com", 13));
    ASSERT_EQ(0, hl_http_check_host(&cfg, "a.b.example.com", 15));
    ASSERT_NE(0, hl_http_check_host(&cfg, "example.com", 11));   /* apex */
    ASSERT_NE(0, hl_http_check_host(&cfg, "evil.com", 8));
}

UTEST(host, cidr_ip_literal)
{
    const char *hosts[] = { "10.0.0.0/8" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    ASSERT_EQ(0, hl_http_check_host(&cfg, "10.1.2.3", 8));
    ASSERT_NE(0, hl_http_check_host(&cfg, "11.0.0.1", 8));
    ASSERT_NE(0, hl_http_check_host(&cfg, "host.example.com", 16)); /* no DNS */
}

UTEST(host, wildcard_any)
{
    const char *hosts[] = { "*" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    ASSERT_EQ(0, hl_http_check_host(&cfg, "anything.test", 13));
}

/* §2.8: a "$VAR" entry resolves from the environment at match time; an
 * unset var contributes no match (fail closed). */
UTEST(host, env_ref)
{
    const char *hosts[] = { "$HULL_TEST_ALLOWED_HOST" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };

    setenv("HULL_TEST_ALLOWED_HOST", "api.example.com", 1);
    ASSERT_EQ(0, hl_http_check_host(&cfg, "api.example.com", 15));
    ASSERT_NE(0, hl_http_check_host(&cfg, "evil.com", 8));

    unsetenv("HULL_TEST_ALLOWED_HOST");
    ASSERT_NE(0, hl_http_check_host(&cfg, "api.example.com", 15));
}

UTEST_MAIN()
