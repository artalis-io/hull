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

UTEST_MAIN();
