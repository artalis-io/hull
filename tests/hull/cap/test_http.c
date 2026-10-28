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
    HlHttpRedirectGuard g = { .cfg = &cfg, .origin_https = 0 };
    ASSERT_EQ(0, hl_http_redirect_allowed("https://api.example.com/v2/x", &g));
}

UTEST(redirect, a_hop_elsewhere_is_refused)
{
    /* What an allowed host could do before: send the client to the cloud
     * metadata endpoint or an internal service. */
    const char *hosts[] = { "api.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    HlHttpRedirectGuard g = { .cfg = &cfg, .origin_https = 0 };
    ASSERT_EQ(-1, hl_http_redirect_allowed(
        "http://169.254.169.254/latest/meta-data/", &g));
    ASSERT_EQ(-1, hl_http_redirect_allowed("http://10.0.0.5:6379/", &g));
    ASSERT_EQ(-1, hl_http_redirect_allowed("http://api.example.com.evil.net/", &g));
}

UTEST(redirect, an_unparseable_hop_is_refused)
{
    const char *hosts[] = { "*" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    HlHttpRedirectGuard g = { .cfg = &cfg, .origin_https = 0 };
    ASSERT_EQ(-1, hl_http_redirect_allowed("not a url", &g));
    ASSERT_EQ(-1, hl_http_redirect_allowed(NULL, &g));
}

/* Audit 25: an https chain redirected to http:// re-sent a 307/308 body and
 * every custom credential header (X-Api-Key) in cleartext. */
UTEST(redirect, an_https_chain_may_not_drop_to_http)
{
    const char *hosts[] = { "api.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    HlHttpRedirectGuard tls = { .cfg = &cfg, .origin_https = 1 };
    ASSERT_EQ(-1, hl_http_redirect_allowed("http://api.example.com/x", &tls));
    ASSERT_EQ(0, hl_http_redirect_allowed("https://api.example.com/x", &tls));
    /* A chain that began on http may go either way. */
    HlHttpRedirectGuard plain = { .cfg = &cfg, .origin_https = 0 };
    ASSERT_EQ(0, hl_http_redirect_allowed("http://api.example.com/x", &plain));
    ASSERT_EQ(0, hl_http_redirect_allowed("https://api.example.com/x", &plain));
}

/* Audit 26: between two ALLOWED hosts Keel still re-sends a 307/308 body and
 * every header but Authorization / Cookie / Proxy-Authorization. */
static void guard_for(HlHttpRedirectGuard *g, const HlHttpConfig *cfg,
                      const char *url, const HlHttpHeader *h, int n,
                      size_t body_len)
{
    KlUrl u;
    if (kl_url_parse(url, &u) != 0) { memset(g, 0, sizeof *g); return; }
    hl_http_redirect_guard_init(g, cfg, &u, h, n, body_len);
}

UTEST(redirect, a_credential_header_may_not_change_origin)
{
    const char *hosts[] = { "a.example.com", "b.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 2 };
    HlHttpHeader h[] = { { "Accept", "*/*" }, { "X-Api-Key", "s3cret" } };
    HlHttpRedirectGuard g;
    guard_for(&g, &cfg, "https://a.example.com/x", h, 2, 0);
    ASSERT_EQ(1, g.carries_secrets);
    ASSERT_EQ(-1, hl_http_redirect_allowed("https://b.example.com/y", &g));
    ASSERT_EQ(-1, hl_http_redirect_allowed("https://a.example.com:8443/y", &g));
    /* Same origin (host case aside) is fine. */
    ASSERT_EQ(0, hl_http_redirect_allowed("https://A.example.com/y", &g));
}

UTEST(redirect, a_body_may_not_change_origin)
{
    const char *hosts[] = { "a.example.com", "b.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 2 };
    HlHttpRedirectGuard g;
    guard_for(&g, &cfg, "https://a.example.com/x", NULL, 0, 12);
    ASSERT_EQ(-1, hl_http_redirect_allowed("https://b.example.com/y", &g));
    ASSERT_EQ(0, hl_http_redirect_allowed("https://a.example.com/z", &g));
}

UTEST(redirect, safe_headers_may_change_origin)
{
    const char *hosts[] = { "a.example.com", "b.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 2 };
    /* Authorization is dropped by Keel on a cross-origin hop. */
    HlHttpHeader h[] = { { "accept", "*/*" }, { "User-Agent", "x" },
                         { "If-None-Match", "\"1\"" },
                         { "Authorization", "Bearer t" } };
    HlHttpRedirectGuard g;
    guard_for(&g, &cfg, "https://a.example.com/x", h, 4, 0);
    ASSERT_EQ(0, g.carries_secrets);
    ASSERT_EQ(0, hl_http_redirect_allowed("https://b.example.com/y", &g));
}

UTEST(redirect, an_http_to_https_upgrade_keeps_the_origin)
{
    const char *hosts[] = { "a.example.com" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    HlHttpHeader h[] = { { "X-Api-Key", "s3cret" } };
    HlHttpRedirectGuard g;
    guard_for(&g, &cfg, "http://a.example.com/x", h, 1, 0);
    ASSERT_EQ(0, hl_http_redirect_allowed("https://a.example.com/x", &g));
    ASSERT_EQ(-1, hl_http_redirect_allowed("https://a.example.com:8443/x", &g));
}

UTEST(redirect, no_guard_refuses)
{
    ASSERT_EQ(-1, hl_http_redirect_allowed("https://api.example.com/x", NULL));
    HlHttpRedirectGuard g = { .cfg = NULL, .origin_https = 0 };
    ASSERT_EQ(-1, hl_http_redirect_allowed("https://api.example.com/x", &g));
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

/* The audit log wrote URLs verbatim, userinfo and query included
 * (audit 17). */
static const char *redact(const char *url)
{
    static char buf[128];
    hl_http_url_redact(url, buf, sizeof buf);
    return buf;
}

UTEST(audit_url, credentials_and_query_dropped)
{
    ASSERT_STREQ("https://api.example.com/v1/x",
                 redact("https://user:pw@api.example.com/v1/x?token=s3cret#f"));
    ASSERT_STREQ("https://api.example.com",
                 redact("https://u:p@api.example.com?k=v"));
    ASSERT_STREQ("http://host:8080/a/b", redact("http://host:8080/a/b"));
    ASSERT_STREQ("http://host/p", redact("http://a@b@host/p#x"));
    /* A '@' after the first '/' could be a password holding '/'. */
    ASSERT_STREQ("http://[redacted]", redact("http://u:p/w@host/x"));
    ASSERT_STREQ("host/x", redact("u:p@host/x"));
    /* ... and one holding '?' or '#' (audit 28, fuzz_http_guard): the '@'
     * was looked for before the query only, so "u:hunter2" was printed. */
    ASSERT_STREQ("https://[redacted]", redact("https://u:hunter2#x@h/"));
    ASSERT_STREQ("https://[redacted]", redact("https://u:pw?x@h/"));
    ASSERT_STREQ("https://h/p", redact("https://u:p@h/p?q=1"));
    ASSERT_STREQ("", redact(NULL));
}

UTEST(audit_url, truncates_and_terminates)
{
    char small[8];
    size_t n = hl_http_url_redact("https://example.com/long", small, sizeof small);
    ASSERT_EQ((size_t)7, n);
    ASSERT_STREQ("https:/", small);
    ASSERT_EQ((size_t)0, hl_http_url_redact("x", small, 0));
}

/* Audit 22: a caller Host went out as a second Host line, and a caller
 * Transfer-Encoding beside Keel's Content-Length smuggled a request. */
UTEST(request_headers, hulls_own_and_malformed_names_are_refused)
{
    HlHttpHeader ok[] = { { "Authorization", "Bearer x" }, { "X-Req", "1" },
                          { "Content-Type", "application/json" } };
    EXPECT_TRUE(hl_http_reserved_header(ok, 3) == NULL);
    EXPECT_TRUE(hl_http_reserved_header(NULL, 0) == NULL);
    static const char *const bad[] = {
        "Host", "host", "Content-Length", "TRANSFER-ENCODING", "TE",
        "Trailer", "Connection", "Keep-Alive", "Upgrade",
        "Proxy-Authorization", "proxy-connection",
        "Bad Name", "X:Y", "", "X\tY",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        HlHttpHeader h[] = { { "X-Ok", "1" }, { bad[i], "v" } };
        const char *r = hl_http_reserved_header(h, 2);
        EXPECT_TRUE_MSG(r != NULL && strcmp(r, bad[i]) == 0, bad[i]);
    }
}

/* Audit 23: Keel refuses only CR / LF in the method; a space or a control
 * byte reached the request line. */
UTEST(request_method, only_rfc9110_tokens_are_accepted)
{
    static const char *const good[] = { "GET", "POST", "PATCH", "M-SEARCH", "x!#$%&'*+.^_`|~" };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++)
        EXPECT_TRUE_MSG(hl_http_method_valid(good[i]) == 1, good[i]);
    static const char *const bad[] = {
        "", "GET /x HTTP/1.1", "GE T", "GET\t", "G\x01T", "GET:", "G(T", "G\x7fT", "G\xc3\xa9T",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        EXPECT_TRUE_MSG(hl_http_method_valid(bad[i]) == 0, bad[i]);
    EXPECT_EQ(0, hl_http_method_valid(NULL));

    /* The cap entry point refuses it before any network work. */
    const char *hosts[] = { "*" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    KlHttpClientResponse resp;
    EXPECT_EQ(-1, hl_cap_http_request(&cfg, 1000, "GET /evil HTTP/1.1\r\nX:",
                                      "http://127.0.0.1:1/", NULL, 0, NULL, 0,
                                      &resp));
    EXPECT_EQ(-1, hl_cap_http_request(&cfg, 1000, "",
                                      "http://127.0.0.1:1/", NULL, 0, NULL, 0,
                                      &resp));
}

/* ── In-flight http.fetch admission (audit 31) ──────────────────────── */

/* The N+1th fetch past HL_HTTP_MAX_INFLIGHT is refused, with nothing held. */
UTEST(fetch_admit, refuses_past_the_inflight_cap)
{
    size_t total = 0;
    int inflight = 0;
    HlOffheapHold holds[HL_HTTP_MAX_INFLIGHT + 1];
    memset(holds, 0, sizeof holds);
    for (int i = 0; i < HL_HTTP_MAX_INFLIGHT; i++) {
        ASSERT_EQ(HL_HTTP_ADMIT_OK,
                  hl_http_fetch_admit(inflight, &holds[i], &total, 0, 0, 1024));
        inflight++;
    }
    EXPECT_EQ((size_t)HL_HTTP_MAX_INFLIGHT * 1024, total);
    EXPECT_EQ(HL_HTTP_ADMIT_INFLIGHT,
              hl_http_fetch_admit(inflight, &holds[HL_HTTP_MAX_INFLIGHT],
                                  &total, 0, 0, 1024));
    EXPECT_EQ((size_t)0, holds[HL_HTTP_MAX_INFLIGHT].bytes);
    EXPECT_EQ((size_t)HL_HTTP_MAX_INFLIGHT * 1024, total);
    for (int i = 0; i < HL_HTTP_MAX_INFLIGHT; i++)
        hl_offheap_drop(&holds[i]);
    EXPECT_EQ((size_t)0, total);
}

/* Each fetch holds its body plus the response limit (Keel's 4 MiB default
 * when none is configured): a 64 MB heap admits 16 default fetches, not the
 * 17th, which holds nothing. */
UTEST(fetch_admit, response_reservation_counts_against_the_heap_limit)
{
    const size_t heap = (size_t)64 * 1024 * 1024;
    size_t total = 0;
    HlOffheapHold holds[17];
    memset(holds, 0, sizeof holds);
    for (int i = 0; i < 16; i++)
        ASSERT_EQ(HL_HTTP_ADMIT_OK,
                  hl_http_fetch_admit(i, &holds[i], &total, heap, 0, 0));
    EXPECT_EQ(heap, total);
    EXPECT_EQ(HL_HTTP_ADMIT_HEAP,
              hl_http_fetch_admit(16, &holds[16], &total, heap, 0, 0));
    EXPECT_EQ((size_t)0, holds[16].bytes);
    hl_offheap_drop(&holds[0]);
    /* One back: the next one fits again, its body counted too. */
    EXPECT_EQ(HL_HTTP_ADMIT_HEAP,
              hl_http_fetch_admit(15, &holds[16], &total, heap, 1, 0));
    EXPECT_EQ(HL_HTTP_ADMIT_OK,
              hl_http_fetch_admit(15, &holds[16], &total, heap, 0, 0));
    for (int i = 1; i < 17; i++)
        hl_offheap_drop(&holds[i]);
    EXPECT_EQ((size_t)0, total);
}

UTEST(fetch_admit, an_overflowing_body_is_refused)
{
    size_t total = 0;
    HlOffheapHold h = {0};
    EXPECT_EQ(HL_HTTP_ADMIT_HEAP,
              hl_http_fetch_admit(0, &h, &total, 0, (size_t)-1, 0));
    EXPECT_EQ((size_t)0, total);
}

UTEST_MAIN()
