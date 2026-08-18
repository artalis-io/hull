/*
 * test_js_session.c - Slice 1: the restricted QuickJS tooling runtime.
 *
 * Proves, independently of any parser (Slice 2): the crossing (source bytes reach the
 * bundled JS length-aware + NUL-safe), multi-module loading from the cli-js registry, the
 * security boundary (no application authority in the tooling VM), that dynamic code is
 * fully blocked (including prototype-reachable constructors), never-raise transport
 * (exceptions/limits become structured indeterminate diagnostics), the FULL limit contract
 * (bytes / instructions / heap / stack / result + an ordinary throw, each classified),
 * fail-closed input transport (null source, malformed/NUL/trailing-garbage options), the
 * null-output lifecycle, and clean create/destroy under ASan/UBSan.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_session.h"

#include <string.h>
#include <stdlib.h>

/* tiny substring helper for the JSON result assertions (the result is small, flat JSON) */
static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }

/* convenience: analyze with no options */
static int analyze(HlJsSession *s, const char *method,
                   const uint8_t *src, size_t src_len, const char *path,
                   char **out, size_t *out_len)
{
    return hl_js_session_analyze(s, "hull:probe", method, src, src_len, path,
                                 NULL, 0, out, out_len);
}

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
    int rc = analyze(s, "analyze", (const uint8_t *)src, strlen(src), "worker.js", &out, &out_len);
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
    int rc = analyze(s, "analyze", src, sizeof(src), "x.js", &out, &out_len);
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
    int rc = analyze(s, "analyze", (const uint8_t *)"x", 1, "a.js", &out, &out_len);
    ASSERT_EQ(rc, 0);
    EXPECT_TRUE(has(out, "\"sandbox_clean\":true"));
    EXPECT_TRUE(has(out, "\"present_authorities\":[]"));
    free(out);
    hl_js_session_destroy(s);
}

/* Adversarial: every dynamic-code path is blocked, including the prototype-reachable
 * constructors that survive deleting global eval / Function. */
UTEST(js_session, dynamic_code_blocked)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = analyze(s, "probeDynamic", (const uint8_t *)"x", 1, "a.js", &out, &out_len);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(has(out, "\"all_blocked\":true"));
    EXPECT_FALSE(has(out, "\"RAN\""));                 /* nothing executed dynamically */
    EXPECT_TRUE(has(out, "\"object_ctor_ctor\":\"blocked\""));
    EXPECT_TRUE(has(out, "\"async_ctor\":\"blocked\""));
    EXPECT_TRUE(has(out, "\"fn_ctor\":\"blocked\""));
    free(out);
    hl_js_session_destroy(s);
}

/* Options JSON crosses + is parsed (JSON in). */
UTEST(js_session, options_transport)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    const char *opts = "{\"echo\":42}";
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"x", 1, "a.js",
                                   opts, strlen(opts), &out, &out_len);
    ASSERT_EQ(rc, 0);
    EXPECT_TRUE(has(out, "\"options_echo\":42"));
    free(out);
    hl_js_session_destroy(s);
}

/* Malformed options fail closed (js.transport), not silently nulled. */
UTEST(js_session, options_malformed_fails_closed)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    const char *bad = "{not valid json";
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"x", 1, "a.js",
                                   bad, strlen(bad), &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"status\":\"indeterminate\""));
    EXPECT_TRUE(has(out, "\"code\":\"js.transport\""));
    free(out);
    hl_js_session_destroy(s);
}

/* Options with an embedded NUL are length-aware malformed JSON -> fail closed (not
 * truncated at the NUL to a valid prefix). */
UTEST(js_session, options_embedded_nul_fails_closed)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    const char opts[] = { '{', '"', 'e', '"', ':', '1', '}', 0x00, 'x' };  /* valid prefix + NUL + junk */
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"x", 1, "a.js",
                                   opts, sizeof(opts), &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.transport\""));
    free(out);
    hl_js_session_destroy(s);
}

/* A NULL options pointer with a nonzero length is a transport mismatch (would read past
 * the placeholder buffer), rejected like the source case. */
UTEST(js_session, options_null_ptr_with_len_rejected)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"x", 1, "a.js",
                                   NULL, 8, &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.transport\""));
    free(out);
    hl_js_session_destroy(s);
}

/* Options with trailing garbage after a valid value -> fail closed. */
UTEST(js_session, options_trailing_garbage_fails_closed)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    const char *bad = "{\"e\":1} trailing";
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"x", 1, "a.js",
                                   bad, strlen(bad), &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.transport\""));
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
        int rc = analyze(s, "analyze", (const uint8_t *)"y", 1, "a.js", &out, &out_len);
        ASSERT_EQ(rc, 0);
        EXPECT_TRUE(has(out, "\"status\":\"ok\""));
        free(out);
    }
    hl_js_session_destroy(s);
}

/* ── fail-closed input transport ─────────────────────────────────────────── */

/* A NULL source with a nonzero length is rejected before entering QuickJS (would read
 * past the placeholder buffer otherwise). Exercised under ASan in CI. */
UTEST(js_session, null_source_nonzero_len_rejected)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", NULL, 16, "a.js",
                                   NULL, 0, &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.transport\""));
    free(out);
    hl_js_session_destroy(s);
}

/* A NULL source with length 0 is a legitimate empty source. */
UTEST(js_session, null_source_zero_len_ok)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", NULL, 0, "a.js",
                                   NULL, 0, &out, &out_len);
    ASSERT_EQ(rc, 0);
    EXPECT_TRUE(has(out, "\"byte_length\":0"));
    free(out);
    hl_js_session_destroy(s);
}

/* out_json == NULL: the diagnostic is discarded, not leaked (ASan catches a leak). */
UTEST(js_session, null_output_lifecycle)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    /* failure path with no output requested */
    int rc = hl_js_session_analyze(s, "hull:probe", "analyze", NULL, 16, "a.js",
                                   NULL, 0, NULL, NULL);
    ASSERT_EQ(rc, -1);
    /* success path with no output requested */
    rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"x", 1, "a.js",
                               NULL, 0, NULL, NULL);
    ASSERT_EQ(rc, 0);
    hl_js_session_destroy(s);
}

/* ── the full limit contract ─────────────────────────────────────────────── */

/* max_source_bytes -> js.limit.bytes (fail closed before QuickJS). */
UTEST(js_session, limit_source_size)
{
    HlJsSessionLimits lim = HL_JS_SESSION_LIMITS_DEFAULT;
    lim.max_source_bytes = 8;
    HlJsSession *s = hl_js_session_create(&lim);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = analyze(s, "analyze", (const uint8_t *)"way too long", 12, "a.js", &out, &out_len);
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(has(out, "\"status\":\"indeterminate\""));
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.bytes\""));
    free(out);
    hl_js_session_destroy(s);
}

/* An infinite loop trips the instruction budget -> js.limit.instructions (uncatchable). */
UTEST(js_session, limit_instructions)
{
    HlJsSessionLimits lim = HL_JS_SESSION_LIMITS_DEFAULT;
    lim.max_instructions = 50;   /* 50 interrupt polls */
    HlJsSession *s = hl_js_session_create(&lim);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = analyze(s, "spin", (const uint8_t *)"x", 1, "a.js", &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.instructions\""));
    free(out);
    hl_js_session_destroy(s);
}

/* Deep recursion trips the stack limit -> js.limit.stack. */
UTEST(js_session, limit_stack)
{
    HlJsSessionLimits lim = HL_JS_SESSION_LIMITS_DEFAULT;
    lim.max_stack_bytes = 64 * 1024;   /* small stack */
    HlJsSession *s = hl_js_session_create(&lim);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = analyze(s, "recurse", (const uint8_t *)"x", 1, "a.js", &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.stack\""));
    free(out);
    hl_js_session_destroy(s);
}

/* Unbounded allocation trips the heap limit -> js.limit.heap. */
UTEST(js_session, limit_heap)
{
    HlJsSessionLimits lim = HL_JS_SESSION_LIMITS_DEFAULT;
    lim.max_heap_bytes = 8 * 1024 * 1024;   /* loads the tiny bundle, trips on hog() */
    HlJsSession *s = hl_js_session_create(&lim);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = analyze(s, "hog", (const uint8_t *)"x", 1, "a.js", &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.heap\""));
    free(out);
    hl_js_session_destroy(s);
}

/* A large valid source that cannot fit the configured heap trips js.limit.heap during
 * argument construction (ArrayBuffer copy) -- NOT js.transport. */
UTEST(js_session, limit_heap_large_source)
{
    HlJsSessionLimits lim = HL_JS_SESSION_LIMITS_DEFAULT;
    lim.max_source_bytes = (size_t)32 * 1024 * 1024;   /* allow a big source through */
    lim.max_heap_bytes   = (size_t)8 * 1024 * 1024;    /* but the heap can't hold it */
    HlJsSession *s = hl_js_session_create(&lim);
    ASSERT_TRUE(s != NULL);
    size_t big = (size_t)16 * 1024 * 1024;             /* 16 MiB source > 8 MiB heap */
    uint8_t *buf = malloc(big);
    ASSERT_TRUE(buf != NULL);
    memset(buf, 'a', big);
    char *out = NULL; size_t out_len = 0;
    int rc = analyze(s, "analyze", buf, big, "a.js", &out, &out_len);
    free(buf);
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.heap\""));   /* heap, not transport */
    EXPECT_FALSE(has(out, "js.transport"));
    free(out);
    hl_js_session_destroy(s);
}

/* A method returning `undefined` (JSON.stringify -> undefined, not a string) is NOT
 * emitted as the literal bytes "undefined"; the boundary fails closed. */
UTEST(js_session, result_undefined_is_indeterminate)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = analyze(s, "returnsUndefined", (const uint8_t *)"x", 1, "a.js", &out, &out_len);
    ASSERT_EQ(rc, -1);
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(has(out, "\"status\":\"indeterminate\""));
    EXPECT_TRUE(has(out, "\"code\":\"js.internal\""));
    EXPECT_FALSE(has(out, "undefined\"}"));   /* never the raw bytes `undefined` as the result */
    free(out);
    hl_js_session_destroy(s);
}

/* After a heap failure, the session must stay usable: no secondary/stale pending exception
 * (e.g. from a failed exception-message conversion) may contaminate the next invocation. */
UTEST(js_session, reuse_after_heap_failure)
{
    HlJsSessionLimits lim = HL_JS_SESSION_LIMITS_DEFAULT;
    lim.max_heap_bytes = 8 * 1024 * 1024;
    HlJsSession *s = hl_js_session_create(&lim);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = analyze(s, "hog", (const uint8_t *)"x", 1, "a.js", &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.heap\""));
    free(out); out = NULL;
    /* Reuse: a normal call must still succeed. */
    rc = analyze(s, "analyze", (const uint8_t *)"ok", 2, "a.js", &out, &out_len);
    ASSERT_EQ(rc, 0);
    EXPECT_TRUE(has(out, "\"status\":\"ok\""));
    EXPECT_TRUE(has(out, "\"byte_length\":2"));
    free(out);
    hl_js_session_destroy(s);
}

/* An entry module that rejects on load (top-level await) fails via the rejected-promise
 * branch as js.internal, and the session stays usable afterward (no contaminating exception
 * from converting the rejection value to a message). */
UTEST(js_session, entry_rejection_and_reuse)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:_probe_reject", "analyze", (const uint8_t *)"x", 1, "a.js",
                                   NULL, 0, &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"status\":\"indeterminate\""));
    EXPECT_TRUE(has(out, "\"code\":\"js.internal\""));
    free(out); out = NULL;
    /* Reuse with the good entry: must succeed. */
    rc = hl_js_session_analyze(s, "hull:probe", "analyze", (const uint8_t *)"ok", 2, "a.js",
                               NULL, 0, &out, &out_len);
    ASSERT_EQ(rc, 0);
    EXPECT_TRUE(has(out, "\"status\":\"ok\""));
    EXPECT_TRUE(has(out, "\"byte_length\":2"));
    free(out);
    hl_js_session_destroy(s);
}

/* An oversize result is rejected -> js.limit.result. */
UTEST(js_session, limit_result_size)
{
    HlJsSessionLimits lim = HL_JS_SESSION_LIMITS_DEFAULT;
    lim.max_result_bytes = 1024;
    HlJsSession *s = hl_js_session_create(&lim);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    const char *opts = "{\"size\":100000}";
    int rc = hl_js_session_analyze(s, "hull:probe", "bigResult", (const uint8_t *)"x", 1, "a.js",
                                   opts, strlen(opts), &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.limit.result\""));
    free(out);
    hl_js_session_destroy(s);
}

/* An ordinary tooling throw -> js.internal (not misclassified as a limit). */
UTEST(js_session, ordinary_throw_is_internal)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = analyze(s, "boom", (const uint8_t *)"x", 1, "a.js", &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.internal\""));
    free(out);
    hl_js_session_destroy(s);
}

/* An unknown entry module -> structured js.internal, never a crash or raw exception. */
UTEST(js_session, unknown_module_fails_closed)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    char *out = NULL; size_t out_len = 0;
    int rc = hl_js_session_analyze(s, "hull:does-not-exist", "analyze", (const uint8_t *)"x", 1, "a.js",
                                   NULL, 0, &out, &out_len);
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
    int rc = analyze(s, "no_such_method", (const uint8_t *)"x", 1, "a.js", &out, &out_len);
    ASSERT_EQ(rc, -1);
    EXPECT_TRUE(has(out, "\"code\":\"js.internal\""));
    free(out);
    hl_js_session_destroy(s);
}

UTEST_MAIN()
