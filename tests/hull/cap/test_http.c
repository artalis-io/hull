/*
 * test_http.c - Unit tests for HTTP client capability (no network)
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

/* ════════════════════════════════════════════════════════════════════
 * Redirect hops (Keel's on_redirect): the same allowlist on every hop
 * ════════════════════════════════════════════════════════════════════ */

UTEST(redirect, a_hop_to_an_allowed_host_is_followed)
{
    const char *hosts[] = { "api.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    ASSERT_EQ(0, hl_http_redirect_allowed("https://api.example.com/v2/x", &cfg));
}

UTEST(redirect, a_hop_elsewhere_is_refused)
{
    /* What an allowed host could do before: send the client to the cloud
     * metadata endpoint or an internal service. */
    const char *hosts[] = { "api.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    ASSERT_EQ(-1, hl_http_redirect_allowed(
        "http://169.254.169.254/latest/meta-data/", &cfg));
    ASSERT_EQ(-1, hl_http_redirect_allowed("http://10.0.0.5:6379/", &cfg));
    ASSERT_EQ(-1, hl_http_redirect_allowed("http://api.example.com.evil.net/", &cfg));
}

UTEST(redirect, an_unparseable_hop_is_refused)
{
    const char *hosts[] = { "*" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    ASSERT_EQ(-1, hl_http_redirect_allowed("not a url", &cfg));
    ASSERT_EQ(-1, hl_http_redirect_allowed(NULL, &cfg));
}

/* ════════════════════════════════════════════════════════════════════
 * Whole-request timeout resolution (hull/limits/http.h)
 * ════════════════════════════════════════════════════════════════════ */

UTEST(timeout, default_is_30s)
{
    ASSERT_EQ(30000, HL_HTTP_DEFAULT_TIMEOUT_MS);
    ASSERT_EQ(30000, hl_http_timeout_resolve(0, 0, HL_HTTP_FETCH_MAX_TIMEOUT_MS));
    ASSERT_EQ(30000, hl_http_timeout_resolve(0, 0, HL_HTTP_SYNC_MAX_TIMEOUT_MS));
    /* <= 0 is "not given" at either tier */
    ASSERT_EQ(30000, hl_http_timeout_resolve(-5, -1, HL_HTTP_FETCH_MAX_TIMEOUT_MS));
}

UTEST(timeout, manifest_default_applies_without_a_per_call_value)
{
    ASSERT_EQ(1500, hl_http_timeout_resolve(0, 1500, HL_HTTP_FETCH_MAX_TIMEOUT_MS));
    ASSERT_EQ(1500, hl_http_timeout_resolve(0, 1500, HL_HTTP_SYNC_MAX_TIMEOUT_MS));
}

UTEST(timeout, per_call_overrides_the_manifest_default)
{
    ASSERT_EQ(250, hl_http_timeout_resolve(250, 1500, HL_HTTP_FETCH_MAX_TIMEOUT_MS));
    ASSERT_EQ(5000, hl_http_timeout_resolve(5000, 1500, HL_HTTP_SYNC_MAX_TIMEOUT_MS));
}

UTEST(timeout, clamped_to_the_path_ceiling)
{
    ASSERT_EQ(600000, HL_HTTP_FETCH_MAX_TIMEOUT_MS);
    ASSERT_EQ(60000, HL_HTTP_SYNC_MAX_TIMEOUT_MS);
    /* async http.fetch: 10 min */
    ASSERT_EQ(HL_HTTP_FETCH_MAX_TIMEOUT_MS,
              hl_http_timeout_resolve(3600000, 0, HL_HTTP_FETCH_MAX_TIMEOUT_MS));
    ASSERT_EQ(HL_HTTP_FETCH_MAX_TIMEOUT_MS,
              hl_http_timeout_resolve(0, 2147483647, HL_HTTP_FETCH_MAX_TIMEOUT_MS));
    /* sync calls: 60 s - per call and the manifest default alike */
    ASSERT_EQ(HL_HTTP_SYNC_MAX_TIMEOUT_MS,
              hl_http_timeout_resolve(120000, 0, HL_HTTP_SYNC_MAX_TIMEOUT_MS));
    ASSERT_EQ(HL_HTTP_SYNC_MAX_TIMEOUT_MS,
              hl_http_timeout_resolve(0, 300000, HL_HTTP_SYNC_MAX_TIMEOUT_MS));
    /* under the fetch ceiling: passes through */
    ASSERT_EQ(300000, hl_http_timeout_resolve(0, 300000, HL_HTTP_FETCH_MAX_TIMEOUT_MS));
}

/* The sync path hands Keel only what is left of the deadline before each
 * redirect hop (audit 11: Keel restarts its timer per hop, so a 60 s ceiling
 * over 10 hops blocked the loop for ~11 min). */
UTEST(timeout, chain_remaining_is_what_is_left_of_the_deadline)
{
    ASSERT_EQ(1500, hl_http_chain_remaining_ms(10000, 8500));
    ASSERT_EQ(1, hl_http_chain_remaining_ms(10000, 9999));
    /* reached or passed: nothing left, the hop is refused */
    ASSERT_EQ(0, hl_http_chain_remaining_ms(10000, 10000));
    ASSERT_EQ(0, hl_http_chain_remaining_ms(10000, 20000));
    /* never wraps past INT32_MAX */
    ASSERT_EQ(2147483647, hl_http_chain_remaining_ms(UINT64_MAX, 0));
}

UTEST(timeout, redirect_hops_default_to_five)
{
    ASSERT_EQ(5, HL_HTTP_MAX_REDIRECTS);
    ASSERT_EQ(5, hl_http_max_redirects(0));
    ASSERT_EQ(5, hl_http_max_redirects(-1));
    ASSERT_EQ(2, hl_http_max_redirects(2));
}

UTEST_MAIN()
