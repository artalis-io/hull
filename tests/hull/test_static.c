/*
 * test_static.c - Tests for static file serving middleware
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/static.h"
#include "hull/vfs.h"

#include <keel/allocator.h>
#include <keel/http_request.h>
#include <keel/http_response.h>
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

static KlHttpRequest make_request(const char *method, const char *path)
{
    KlHttpRequest req;
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
        { "static/secret.txt", (const unsigned char *)"secret", 6 },
        { NULL, NULL, 0 },
    };
    HlVfs vfs;
    hl_vfs_init(&vfs, entries, NULL);
    HlStaticCtx ctx = { .vfs = &vfs };

    KlHttpRequest req = make_request("GET", "/static/../etc/passwd");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);
}

UTEST(static_serve, path_traversal_middle)
{
    /* Path with /../ in the middle should be rejected */
    static const HlEntry entries[] = {
        { "static/secret.txt", (const unsigned char *)"secret", 6 },
        { NULL, NULL, 0 },
    };
    HlVfs vfs;
    hl_vfs_init(&vfs, entries, NULL);
    HlStaticCtx ctx = { .vfs = &vfs };

    KlHttpRequest req = make_request("GET", "/static/sub/../secret.txt");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);
}

/* ── Embedded lookup ──────────────────────────────────────────────── */

UTEST(static_serve, embedded_found)
{
    static const unsigned char css_data[] = "body { color: red; }";
    static const HlEntry entries[] = {
        { "static/style.css", css_data, sizeof(css_data) - 1 },
        { NULL, NULL, 0 },
    };

    HlVfs vfs;
    hl_vfs_init(&vfs, entries, NULL);

    KlAllocator alloc = kl_allocator_default();
    HlStaticCtx ctx = { .vfs = &vfs };

    KlHttpRequest req = make_request("GET", "/static/style.css");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));
    kl_http_response_init(&res, &alloc);

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(1, rc);
    ASSERT_EQ(200, res.status);
    ASSERT_EQ((int)KL_HTTP_BODY_BUFFER, (int)res.body_mode);
    ASSERT_EQ(sizeof(css_data) - 1, res.body_len);
    ASSERT_EQ(0, memcmp(res.body, "body { color: red; }", res.body_len));

    kl_http_response_free(&res);
}

UTEST(static_serve, embedded_not_found)
{
    static const unsigned char css_data[] = "body {}";
    static const HlEntry entries[] = {
        { "static/style.css", css_data, sizeof(css_data) - 1 },
        { NULL, NULL, 0 },
    };

    HlVfs vfs;
    hl_vfs_init(&vfs, entries, NULL);

    KlAllocator alloc = kl_allocator_default();
    HlStaticCtx ctx = { .vfs = &vfs };

    KlHttpRequest req = make_request("GET", "/static/missing.css");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));
    kl_http_response_init(&res, &alloc);

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);

    kl_http_response_free(&res);
}

UTEST(static_serve, non_static_path)
{
    static const HlEntry empty[] = { { NULL, NULL, 0 } };
    HlVfs vfs;
    hl_vfs_init(&vfs, empty, NULL);
    HlStaticCtx ctx = { .vfs = &vfs };

    KlHttpRequest req = make_request("GET", "/api/users");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);
}

UTEST(static_serve, post_method_skipped)
{
    static const unsigned char data[] = "x";
    static const HlEntry entries[] = {
        { "static/style.css", data, 1 },
        { NULL, NULL, 0 },
    };
    HlVfs vfs;
    hl_vfs_init(&vfs, entries, NULL);
    HlStaticCtx ctx = { .vfs = &vfs };

    KlHttpRequest req = make_request("POST", "/static/style.css");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);
}

UTEST(static_serve, head_serves_same_as_get)
{
    /* Regression: pre-fix, the middleware strict-checked req->method
     * for literal "GET" and returned 0 for HEAD. But Keel's pattern
     * matcher routes HEAD to GET-registered middleware (RFC 7230
     * §4.3.2), so HEAD requests reached our middleware and got
     * silently dropped to 404. The fix accepts both GET and HEAD;
     * Keel itself strips the body on the HEAD path so headers are
     * the same. */
    static const unsigned char css[] = ".x{}";
    static const HlEntry entries[] = {
        { "static/style.css", css, sizeof(css) - 1 },
        { NULL, NULL, 0 },
    };
    HlVfs vfs;
    hl_vfs_init(&vfs, entries, NULL);

    KlAllocator alloc = kl_allocator_default();
    HlStaticCtx ctx = { .vfs = &vfs };

    KlHttpRequest req = make_request("HEAD", "/static/style.css");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));
    kl_http_response_init(&res, &alloc);

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(1, rc);
    ASSERT_EQ(200, res.status);

    kl_http_response_free(&res);
}

/* ── Platform VFS fallback (stdlib-shipped widget assets) ─────────── */

UTEST(static_serve, stdlib_fallback_hit)
{
    /* App VFS empty; stdlib VFS holds a widget's CSS. The middleware
     * should serve from stdlib when the app has nothing at that path. */
    static const HlEntry app_entries[] = { { NULL, NULL, 0 } };
    static const unsigned char css[] = ".toast { display: none; }";
    static const HlEntry stdlib_entries[] = {
        { "static/hull/htmx/toast/toast.css", css, sizeof(css) - 1 },
        { NULL, NULL, 0 },
    };
    HlVfs app_vfs, stdlib_vfs;
    hl_vfs_init(&app_vfs, app_entries, NULL);
    hl_vfs_init(&stdlib_vfs, stdlib_entries, NULL);

    KlAllocator alloc = kl_allocator_default();
    HlStaticCtx ctx = { .vfs = &app_vfs, .stdlib_vfs = &stdlib_vfs };

    KlHttpRequest req = make_request("GET", "/static/hull/htmx/toast/toast.css");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));
    kl_http_response_init(&res, &alloc);

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(1, rc);
    ASSERT_EQ(200, res.status);
    ASSERT_EQ(sizeof(css) - 1, res.body_len);
    ASSERT_EQ(0, memcmp(res.body, css, res.body_len));

    kl_http_response_free(&res);
}

UTEST(static_serve, app_shadows_stdlib)
{
    /* Both VFS have the same path. App wins (override semantics). */
    static const unsigned char app_css[] = ".app-version{}";
    static const unsigned char stdlib_css[] = ".stdlib-version{}";
    static const HlEntry app_entries[] = {
        { "static/hull/htmx/toast/toast.css", app_css, sizeof(app_css) - 1 },
        { NULL, NULL, 0 },
    };
    static const HlEntry stdlib_entries[] = {
        { "static/hull/htmx/toast/toast.css", stdlib_css, sizeof(stdlib_css) - 1 },
        { NULL, NULL, 0 },
    };
    HlVfs app_vfs, stdlib_vfs;
    hl_vfs_init(&app_vfs, app_entries, NULL);
    hl_vfs_init(&stdlib_vfs, stdlib_entries, NULL);

    KlAllocator alloc = kl_allocator_default();
    HlStaticCtx ctx = { .vfs = &app_vfs, .stdlib_vfs = &stdlib_vfs };

    KlHttpRequest req = make_request("GET", "/static/hull/htmx/toast/toast.css");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));
    kl_http_response_init(&res, &alloc);

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(1, rc);
    ASSERT_EQ(200, res.status);
    ASSERT_EQ(sizeof(app_css) - 1, res.body_len);
    ASSERT_EQ(0, memcmp(res.body, app_css, res.body_len));

    kl_http_response_free(&res);
}

UTEST(static_serve, stdlib_miss_returns_zero)
{
    /* Neither VFS has the requested path: pass through (return 0)
     * so the route lookup continues. */
    static const unsigned char css[] = ".x{}";
    static const HlEntry app_entries[] = { { NULL, NULL, 0 } };
    static const HlEntry stdlib_entries[] = {
        { "static/hull/htmx/toast/toast.css", css, sizeof(css) - 1 },
        { NULL, NULL, 0 },
    };
    HlVfs app_vfs, stdlib_vfs;
    hl_vfs_init(&app_vfs, app_entries, NULL);
    hl_vfs_init(&stdlib_vfs, stdlib_entries, NULL);

    HlStaticCtx ctx = { .vfs = &app_vfs, .stdlib_vfs = &stdlib_vfs };

    KlHttpRequest req = make_request("GET", "/static/hull/htmx/missing.css");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(0, rc);
}

UTEST(static_serve, stdlib_vfs_null_is_safe)
{
    /* HlStaticCtx with NULL stdlib_vfs must not crash; it should
     * behave exactly like the legacy single-VFS context. */
    static const unsigned char css[] = ".app{}";
    static const HlEntry entries[] = {
        { "static/style.css", css, sizeof(css) - 1 },
        { NULL, NULL, 0 },
    };
    HlVfs vfs;
    hl_vfs_init(&vfs, entries, NULL);

    KlAllocator alloc = kl_allocator_default();
    HlStaticCtx ctx = { .vfs = &vfs, .stdlib_vfs = NULL };

    KlHttpRequest req = make_request("GET", "/static/style.css");
    KlHttpResponse res;
    memset(&res, 0, sizeof(res));
    kl_http_response_init(&res, &alloc);

    int rc = hl_static_middleware(&req, &res, &ctx);
    ASSERT_EQ(1, rc);
    ASSERT_EQ(200, res.status);
    ASSERT_EQ(sizeof(css) - 1, res.body_len);

    kl_http_response_free(&res);
}

UTEST_MAIN()
