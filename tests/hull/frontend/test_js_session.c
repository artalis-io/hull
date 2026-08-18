/*
 * test_js_session.c — Slice 1: the restricted QuickJS tooling runtime.
 *
 * Proves, independently of any parser (Slice 2): the crossing (source bytes reach the
 * bundled JS length-aware + NUL-safe), multi-module loading from the cli-js VFS, the
 * security boundary (no application authority in the tooling VM), never-raise transport
 * (exceptions/limits become structured indeterminate diagnostics), limits fire, malformed
 * transport fails closed, and clean lifecycle (create/destroy under ASan/UBSan/LSan).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_session.h"

#include <string.h>
#include <stdlib.h>

/* tiny substring helper for the JSON result assertions (the result is small, flat JSON) */
static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }

UTEST(js_session, create_destroy)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    hl_js_session_destroy(s);
    hl_js_session_destroy(NULL);   /* NULL-safe */
}

/* The crossing: source bytes arrive length-correct + the bundle loads its helper module. */
UTEST(js_session, crossing_and_module_loading)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const char *src = "hello world";
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze",
                                   (const uint8_t *)src, strlen(src), "worker.js", NULL,
                                   &out, &out_len);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(out_len, strlen(out));
    EXPECT_TRUE(has(out, "\"status\":\"ok\""));
    EXPECT_TRUE(has(out, "\"schema_version\":1"));
    EXPECT_TRUE(has(out, "\"byte_length\":11"));       /* exact source length */
    EXPECT_TRUE(has(out, "\"path\":\"worker.js\""));
    EXPECT_TRUE(has(out, "\"util_ok\":true"));         /* imported hull:_probe_util ran */
    free(out);
    hl_js_session_destroy(s);
}

/* Embedded NUL must not truncate the byte transport. */
UTEST(js_session, embedded_nul_not_truncated)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    const uint8_t src[] = { 'a', 'b', 0x00, 'c', 'd' };   /* 5 bytes incl a NUL */
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", src, sizeof(src), "x.js", NULL, &out, &out_len);
    ASSERT_EQ(rc, 0);
    EXPECT_TRUE(has(out, "\"byte_length\":5"));      /* all 5 bytes crossed, NUL did not truncate */
    EXPECT_TRUE(has(out, "\"first_byte\":97"));      /* 'a' */
    EXPECT_TRUE(has(out, "\"last_byte\":100"));      /* 'd' */
    free(out);
    hl_js_session_destroy(s);
}

/* The security boundary: the tooling VM exposes NO application authority. */
UTEST(js_session, sandbox_clean)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"x", 1, "a.js", NULL, &out, &out_len);
    ASSERT_EQ(rc, 0);
    EXPECT_TRUE(has(out, "\"sandbox_clean\":true"));
    EXPECT_TRUE(has(out, "\"present_authorities\":[]"));
    free(out);
    hl_js_session_destroy(s);
}

/* Options JSON crosses + is parsed (JSON in). */
UTEST(js_session, options_transport)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"x", 1, "a.js",
                                   "{\"echo\":42}", &out, &out_len);
    ASSERT_EQ(rc, 0);
    EXPECT_TRUE(has(out, "\"options_echo\":42"));
    free(out);
    hl_js_session_destroy(s);
}

/* Reuse across many calls (the entry module loads once, is cached). */
UTEST(js_session, reuse_across_calls)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    for (int i = 0; i < 50; i++) {
        char *out = NULL; size_t out_len = 0;
        int rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"y", 1, "a.js", NULL, &out, &out_len);
        ASSERT_EQ(rc, 0);
        EXPECT_TRUE(has(out, "\"status\":\"ok\""));
        free(out);
    }
    hl_js_session_destroy(s);
}

/* max_source_bytes fires -> structured indeterminate js.limit.bytes (fail closed). */
UTEST(js_session, source_size_limit)
{
    HlJsSessionLimits lim = HL_JS_SESSION_LIMITS_DEFAULT;
    lim.max_source_bytes = 8;
    HlJsSession *s = hl_js_session_create(&lim);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze",
                                   (const uint8_t *)"way too long", 12, "a.js", NULL, &out, &out_len);
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(has(out, "\"status\":\"indeterminate\""));
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.bytes\""));
    free(out);
    hl_js_session_destroy(s);
}

/* An unknown entry module -> structured js.internal, never a crash or raw exception. */
UTEST(js_session, unknown_module_fails_closed)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:does-not-exist", "analyze", (const uint8_t *)"x", 1, "a.js", NULL, &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"status\":\"indeterminate\""));
    EXPECT_TRUE(has(out, "\"code\":\"js.internal\""));
    free(out);
    hl_js_session_destroy(s);
}

/* A missing method -> structured js.internal. */
UTEST(js_session, unknown_method_fails_closed)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:probe", "no_such_method", (const uint8_t *)"x", 1, "a.js", NULL, &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.internal\""));
    free(out);
    hl_js_session_destroy(s);
}

UTEST_MAIN()
