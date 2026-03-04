/*
 * test_static.c — Tests for static file serving middleware
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/static.h"

#include <keel/allocator.h>
#include <keel/request.h>
#include <keel/response.h>
#include <string.h>

/* ── MIME type detection ──────────────────────────────────────────── */

UTEST(static_serve, mime_html)
{
    const char *m = hl_static_mime_type("index.html", 10);
    ASSERT_STREQ("text/html; charset=utf-8", m);
}

UTEST(static_serve, mime_css)
{
    const char *m = hl_static_mime_type("style.css", 9);
    ASSERT_STREQ("text/css", m);
}

UTEST(static_serve, mime_js)
{
    const char *m = hl_static_mime_type("app.js", 6);
    ASSERT_STREQ("application/javascript", m);
}

UTEST(static_serve, mime_json)
{
    const char *m = hl_static_mime_type("data.json", 9);
    ASSERT_STREQ("application/json", m);
}

UTEST(static_serve, mime_png)
{
    const char *m = hl_static_mime_type("logo.png", 8);
    ASSERT_STREQ("image/png", m);
}

UTEST(static_serve, mime_jpg)
{
    const char *m = hl_static_mime_type("photo.jpg", 9);
    ASSERT_STREQ("image/jpeg", m);
}

UTEST(static_serve, mime_svg)
{
    const char *m = hl_static_mime_type("icon.svg", 8);
    ASSERT_STREQ("image/svg+xml", m);
}

UTEST(static_serve, mime_woff2)
{
    const char *m = hl_static_mime_type("font.woff2", 10);
    ASSERT_STREQ("font/woff2", m);
}

UTEST(static_serve, mime_unknown)
{
    const char *m = hl_static_mime_type("file.xyz", 8);
    ASSERT_STREQ("application/octet-stream", m);
}

UTEST(static_serve, mime_no_extension)
{
    const char *m = hl_static_mime_type("Makefile", 8);
    ASSERT_STREQ("application/octet-stream", m);
}

UTEST(static_serve, mime_nested_path)
{
    const char *m = hl_static_mime_type("js/vendor/app.min.js", 20);
    ASSERT_STREQ("application/javascript", m);
}

UTEST(static_serve, mime_case_insensitive)
{
    const char *m = hl_static_mime_type("STYLE.CSS", 9);
    ASSERT_STREQ("text/css", m);
}

/* ── Path traversal rejection ─────────────────────────────────────── */

static KlRequest make_request(const char *method, const char *path)
{
    KlRequest req;
    memset(&req, 0, sizeof(req));
    req.method = method;
    req.method_len = strlen(method);
    req.path = path;
    req.path_len = strlen(path);
    return req;
}

UTEST(static_serve, path_traversal_dotdot)
{
    /* Path with .. should not match any file */
    static const HlEntry entries[] = {
        { "secret.txt", (const unsigned char *)"secret", 6 },
        { NULL, NULL, 0 },
    };
    HlStaticCtx ctx = { .app_dir = NULL, .entries = entries };

    KlRequest req = make_request("GET", "/static/../etc/passwd");
    KlResponse res;
    memset(&res, 0, sizeof(res));

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);
}

UTEST(static_serve, path_traversal_middle)
{
    /* Path with /../ in the middle should be rejected */
    static const HlEntry entries[] = {
        { "secret.txt", (const unsigned char *)"secret", 6 },
        { NULL, NULL, 0 },
    };
    HlStaticCtx ctx = { .app_dir = NULL, .entries = entries };

    KlRequest req = make_request("GET", "/static/sub/../secret.txt");
    KlResponse res;
    memset(&res, 0, sizeof(res));

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);
}

/* ── Embedded lookup ──────────────────────────────────────────────── */

UTEST(static_serve, embedded_found)
{
    static const unsigned char css_data[] = "body { color: red; }";
    static const HlEntry entries[] = {
        { "style.css", css_data, sizeof(css_data) - 1 },
        { NULL, NULL, 0 },
    };

    KlAllocator alloc = kl_allocator_default();
    HlStaticCtx ctx = { .app_dir = NULL, .entries = entries };

    KlRequest req = make_request("GET", "/static/style.css");
    KlResponse res;
    memset(&res, 0, sizeof(res));
    kl_response_init(&res, &alloc);

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(1, rc);
    ASSERT_EQ(200, res.status);
    ASSERT_EQ((int)KL_BODY_BUFFER, (int)res.body_mode);
    ASSERT_EQ(sizeof(css_data) - 1, res.body_len);
    ASSERT_EQ(0, memcmp(res.body, "body { color: red; }", res.body_len));

    kl_response_free(&res);
}

UTEST(static_serve, embedded_not_found)
{
    static const unsigned char css_data[] = "body {}";
    static const HlEntry entries[] = {
        { "style.css", css_data, sizeof(css_data) - 1 },
        { NULL, NULL, 0 },
    };

    KlAllocator alloc = kl_allocator_default();
    HlStaticCtx ctx = { .app_dir = NULL, .entries = entries };

    KlRequest req = make_request("GET", "/static/missing.css");
    KlResponse res;
    memset(&res, 0, sizeof(res));
    kl_response_init(&res, &alloc);

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);

    kl_response_free(&res);
}

UTEST(static_serve, non_static_path)
{
    HlStaticCtx ctx = { .app_dir = NULL, .entries = NULL };

    KlRequest req = make_request("GET", "/api/users");
    KlResponse res;
    memset(&res, 0, sizeof(res));

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);
}

UTEST(static_serve, post_method_skipped)
{
    static const unsigned char data[] = "x";
    static const HlEntry entries[] = {
        { "style.css", data, 1 },
        { NULL, NULL, 0 },
    };
    HlStaticCtx ctx = { .app_dir = NULL, .entries = entries };

    KlRequest req = make_request("POST", "/static/style.css");
    KlResponse res;
    memset(&res, 0, sizeof(res));

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);
}

UTEST_MAIN()
