/*
 * test_http.c — Unit tests for HTTP client capability (no network)
 *
 * Tests URL parsing, host allowlist checking, and response parsing
 * through the HlHttpParser vtable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/http.h"
#include "hull/cap/http_parser.h"

#include <keel/allocator.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * URL parsing tests
 * ════════════════════════════════════════════════════════════════════ */

UTEST(url, http_basic)
{
    HlParsedUrl u;
    ASSERT_EQ(0, hl_http_parse_url("http://example.com/path", &u));
    ASSERT_EQ(0, u.is_https);
    ASSERT_EQ(11, (int)u.host_len);
    ASSERT_EQ(0, strncmp(u.host, "example.com", u.host_len));
    ASSERT_EQ(80, u.port);
    ASSERT_EQ(5, (int)u.path_len);
    ASSERT_EQ(0, strncmp(u.path, "/path", u.path_len));
}

UTEST(url, https_basic)
{
    HlParsedUrl u;
    ASSERT_EQ(0, hl_http_parse_url("https://api.stripe.com/v1/charges", &u));
    ASSERT_EQ(1, u.is_https);
    ASSERT_EQ(14, (int)u.host_len);
    ASSERT_EQ(0, strncmp(u.host, "api.stripe.com", u.host_len));
    ASSERT_EQ(443, u.port);
    ASSERT_EQ(11, (int)u.path_len);
    ASSERT_EQ(0, strncmp(u.path, "/v1/charges", u.path_len));
}

UTEST(url, custom_port)
{
    HlParsedUrl u;
    ASSERT_EQ(0, hl_http_parse_url("http://localhost:8080/api", &u));
    ASSERT_EQ(0, u.is_https);
    ASSERT_EQ(9, (int)u.host_len);
    ASSERT_EQ(0, strncmp(u.host, "localhost", u.host_len));
    ASSERT_EQ(8080, u.port);
    ASSERT_EQ(4, (int)u.path_len);
    ASSERT_EQ(0, strncmp(u.path, "/api", u.path_len));
}

UTEST(url, no_path)
{
    HlParsedUrl u;
    ASSERT_EQ(0, hl_http_parse_url("http://example.com", &u));
    ASSERT_EQ(80, u.port);
    ASSERT_EQ(1, (int)u.path_len);
    ASSERT_EQ(0, strncmp(u.path, "/", u.path_len));
}

UTEST(url, query_string)
{
    HlParsedUrl u;
    ASSERT_EQ(0, hl_http_parse_url("http://example.com/search?q=hello&page=1", &u));
    ASSERT_EQ(22, (int)u.path_len);
    ASSERT_EQ(0, strncmp(u.path, "/search?q=hello&page=1", u.path_len));
}

UTEST(url, invalid_scheme)
{
    HlParsedUrl u;
    ASSERT_NE(0, hl_http_parse_url("ftp://example.com/file", &u));
}

UTEST(url, empty_host)
{
    HlParsedUrl u;
    ASSERT_NE(0, hl_http_parse_url("http:///path", &u));
}

UTEST(url, null_url)
{
    HlParsedUrl u;
    ASSERT_NE(0, hl_http_parse_url(NULL, &u));
}

UTEST(url, https_port_443)
{
    HlParsedUrl u;
    ASSERT_EQ(0, hl_http_parse_url("https://example.com:443/", &u));
    ASSERT_EQ(1, u.is_https);
    ASSERT_EQ(443, u.port);
}

UTEST(url, root_path)
{
    HlParsedUrl u;
    ASSERT_EQ(0, hl_http_parse_url("http://example.com/", &u));
    ASSERT_EQ(1, (int)u.path_len);
    ASSERT_EQ('/', u.path[0]);
}

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

/* ════════════════════════════════════════════════════════════════════
 * Response parser tests
 * ════════════════════════════════════════════════════════════════════ */

UTEST(parser, simple_200)
{
    KlAllocator alloc = kl_allocator_default();
    HlHttpParser *p = hl_http_parser_llhttp(0, &alloc);
    ASSERT_TRUE(p != NULL);

    const char *raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    HlHttpResponse resp;
    memset(&resp, 0, sizeof(resp));

    size_t consumed;
    HlHttpParseResult r = p->parse(p, &resp, raw, strlen(raw), &consumed);

    ASSERT_EQ((int)HL_HTTP_PARSE_OK, (int)r);
    ASSERT_EQ(200, resp.status);
    ASSERT_EQ(5, (int)resp.body_len);
    ASSERT_EQ(0, memcmp(resp.body, "hello", 5));
    ASSERT_TRUE(resp.num_headers >= 2);

    /* Check Content-Type header */
    int found_ct = 0;
    for (int i = 0; i < resp.num_headers; i++) {
        if (strcasecmp(resp.headers[i].name, "Content-Type") == 0) {
            ASSERT_EQ(0, strcmp(resp.headers[i].value, "text/plain"));
            found_ct = 1;
        }
    }
    ASSERT_TRUE(found_ct);

    hl_cap_http_free(&resp);
    p->destroy(p);
}

UTEST(parser, no_body_204)
{
    KlAllocator alloc = kl_allocator_default();
    HlHttpParser *p = hl_http_parser_llhttp(0, &alloc);
    ASSERT_TRUE(p != NULL);

    const char *raw =
        "HTTP/1.1 204 No Content\r\n"
        "X-Custom: value\r\n"
        "\r\n";

    HlHttpResponse resp;
    memset(&resp, 0, sizeof(resp));

    size_t consumed;
    HlHttpParseResult r = p->parse(p, &resp, raw, strlen(raw), &consumed);

    ASSERT_EQ((int)HL_HTTP_PARSE_OK, (int)r);
    ASSERT_EQ(204, resp.status);
    ASSERT_EQ(0, (int)resp.body_len);

    hl_cap_http_free(&resp);
    p->destroy(p);
}

UTEST(parser, body_exceeds_max)
{
    KlAllocator alloc = kl_allocator_default();
    HlHttpParser *p = hl_http_parser_llhttp(10, &alloc);  /* max 10 bytes */
    ASSERT_TRUE(p != NULL);

    const char *raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        "01234567890123456789";

    HlHttpResponse resp;
    memset(&resp, 0, sizeof(resp));

    size_t consumed;
    HlHttpParseResult r = p->parse(p, &resp, raw, strlen(raw), &consumed);

    ASSERT_EQ((int)HL_HTTP_PARSE_ERROR, (int)r);

    hl_cap_http_free(&resp);
    p->destroy(p);
}

UTEST(parser, incremental)
{
    KlAllocator alloc = kl_allocator_default();
    HlHttpParser *p = hl_http_parser_llhttp(0, &alloc);
    ASSERT_TRUE(p != NULL);

    const char *part1 = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n";
    const char *part2 = "\r\nabc";

    HlHttpResponse resp;
    memset(&resp, 0, sizeof(resp));

    size_t consumed;
    HlHttpParseResult r;

    r = p->parse(p, &resp, part1, strlen(part1), &consumed);
    ASSERT_EQ((int)HL_HTTP_PARSE_INCOMPLETE, (int)r);

    r = p->parse(p, &resp, part2, strlen(part2), &consumed);
    ASSERT_EQ((int)HL_HTTP_PARSE_OK, (int)r);
    ASSERT_EQ(200, resp.status);
    ASSERT_EQ(3, (int)resp.body_len);
    ASSERT_EQ(0, memcmp(resp.body, "abc", 3));

    hl_cap_http_free(&resp);
    p->destroy(p);
}

UTEST(parser, multiple_headers)
{
    KlAllocator alloc = kl_allocator_default();
    HlHttpParser *p = hl_http_parser_llhttp(0, &alloc);
    ASSERT_TRUE(p != NULL);

    const char *raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "X-Request-Id: abc123\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "{}";

    HlHttpResponse resp;
    memset(&resp, 0, sizeof(resp));

    size_t consumed;
    HlHttpParseResult r = p->parse(p, &resp, raw, strlen(raw), &consumed);

    ASSERT_EQ((int)HL_HTTP_PARSE_OK, (int)r);
    ASSERT_EQ(200, resp.status);
    ASSERT_EQ(3, resp.num_headers);

    hl_cap_http_free(&resp);
    p->destroy(p);
}

UTEST(parser, vtable_lifecycle)
{
    KlAllocator alloc = kl_allocator_default();
    HlHttpParser *p = hl_http_parser_llhttp(1024, &alloc);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(p->parse != NULL);
    ASSERT_TRUE(p->reset != NULL);
    ASSERT_TRUE(p->destroy != NULL);

    p->reset(p);
    p->destroy(p);
}

UTEST_MAIN();
