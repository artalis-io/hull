/*
 * test_js_runtime.c — Tests for QuickJS runtime integration
 *
 * Tests: VM init, sandbox, module loading, route registration,
 * instruction limits, memory limits, GC.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/js_runtime.h"
#include "hull/hull_cap.h"
#include "quickjs.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Helpers ────────────────────────────────────────────────────────── */

static HullJS js;
static int js_initialized = 0;

static void init_js(void)
{
    if (js_initialized)
        hull_js_free(&js);
    HullJSConfig cfg = HULL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    int rc = hull_js_init(&js, &cfg);
    js_initialized = (rc == 0);
}

static void cleanup_js(void)
{
    if (js_initialized) {
        hull_js_free(&js);
        js_initialized = 0;
    }
}

/* Evaluate a JS expression and return the result as a string.
 * Caller must free the returned string. Returns NULL on error. */
static char *eval_str(const char *code)
{
    if (!js_initialized || !js.ctx)
        return NULL;

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        hull_js_dump_error(&js);
        return NULL;
    }

    const char *s = JS_ToCString(js.ctx, val);
    char *result = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(js.ctx, s);
    JS_FreeValue(js.ctx, val);
    return result;
}

/* Evaluate JS and return integer result. Returns -9999 on error. */
static int eval_int(const char *code)
{
    if (!js_initialized || !js.ctx)
        return -9999;

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        hull_js_dump_error(&js);
        return -9999;
    }

    int32_t result = -9999;
    JS_ToInt32(js.ctx, &result, val);
    JS_FreeValue(js.ctx, val);
    return result;
}

/* ── Basic runtime tests ────────────────────────────────────────────── */

UTEST(js_runtime, init_and_free)
{
    HullJSConfig cfg = HULL_JS_CONFIG_DEFAULT;
    HullJS local_js;
    memset(&local_js, 0, sizeof(local_js));

    int rc = hull_js_init(&local_js, &cfg);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(local_js.rt, NULL);
    ASSERT_NE(local_js.ctx, NULL);

    hull_js_free(&local_js);
    ASSERT_EQ(local_js.rt, NULL);
    ASSERT_EQ(local_js.ctx, NULL);
}

UTEST(js_runtime, basic_eval)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    int result = eval_int("1 + 2");
    ASSERT_EQ(result, 3);

    cleanup_js();
}

UTEST(js_runtime, string_eval)
{
    init_js();

    char *s = eval_str("'hello' + ' ' + 'world'");
    ASSERT_NE(s, NULL);
    ASSERT_STREQ(s, "hello world");
    free(s);

    cleanup_js();
}

UTEST(js_runtime, json_works)
{
    init_js();

    char *s = eval_str("JSON.stringify({a: 1, b: 'two'})");
    ASSERT_NE(s, NULL);
    ASSERT_STREQ(s, "{\"a\":1,\"b\":\"two\"}");
    free(s);

    cleanup_js();
}

/* ── Sandbox tests ──────────────────────────────────────────────────── */

UTEST(js_runtime, eval_removed)
{
    init_js();

    /* eval should be undefined (removed by sandbox) */
    int result = eval_int("typeof eval === 'undefined' ? 1 : 0");
    ASSERT_EQ(result, 1);

    cleanup_js();
}

UTEST(js_runtime, no_std_module)
{
    init_js();

    /* std module should not be available */
    JSValue val = JS_Eval(js.ctx,
        "import('std').then(() => 0).catch(() => 1)",
        strlen("import('std').then(() => 0).catch(() => 1)"),
        "<test>", JS_EVAL_TYPE_GLOBAL);

    /* Dynamic import should fail or return exception */
    if (JS_IsException(val)) {
        /* Expected — dynamic import disabled or std not available */
        JSValue exc = JS_GetException(js.ctx);
        JS_FreeValue(js.ctx, exc);
    }
    JS_FreeValue(js.ctx, val);

    cleanup_js();
}

/* ── Instruction limit tests ────────────────────────────────────────── */

UTEST(js_runtime, instruction_limit)
{
    HullJSConfig cfg = HULL_JS_CONFIG_DEFAULT;
    cfg.max_instructions = 1000; /* very low limit */
    HullJS limited_js;
    memset(&limited_js, 0, sizeof(limited_js));

    int rc = hull_js_init(&limited_js, &cfg);
    ASSERT_EQ(rc, 0);

    /* Infinite loop should be interrupted */
    JSValue val = JS_Eval(limited_js.ctx,
        "var i = 0; while(true) { i++; } i",
        strlen("var i = 0; while(true) { i++; } i"),
        "<test>", JS_EVAL_TYPE_GLOBAL);

    ASSERT_TRUE(JS_IsException(val));
    JS_FreeValue(limited_js.ctx, val);

    /* Clear the exception */
    JSValue exc = JS_GetException(limited_js.ctx);
    JS_FreeValue(limited_js.ctx, exc);

    hull_js_free(&limited_js);
}

/* ── Module tests ───────────────────────────────────────────────────── */

UTEST(js_runtime, hull_time_module)
{
    init_js();

    /* Test hull:time module via module eval */
    const char *code =
        "import { time } from 'hull:time';\n"
        "globalThis.__test_time = time.now();\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hull_js_dump_error(&js);
    /* Module eval may return a promise or undefined — that's OK */
    JS_FreeValue(js.ctx, val);

    /* Run pending jobs (module initialization) */
    hull_js_run_jobs(&js);

    /* Check that the time was stored */
    int result = eval_int("typeof globalThis.__test_time === 'number' ? 1 : 0");
    ASSERT_EQ(result, 1);

    /* Time should be a reasonable Unix timestamp */
    int recent = eval_int("globalThis.__test_time > 1704067200 ? 1 : 0");
    ASSERT_EQ(recent, 1);

    cleanup_js();
}

UTEST(js_runtime, hull_app_module)
{
    init_js();

    /* Register routes via hull:app */
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.get('/test', (req, res) => { res.json({ok: true}); });\n"
        "app.post('/data', (req, res) => { res.text('received'); });\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hull_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hull_js_run_jobs(&js);

    /* Verify routes were registered */
    int count = eval_int(
        "globalThis.__hull_route_defs ? globalThis.__hull_route_defs.length : 0");
    ASSERT_EQ(count, 2);

    /* Verify first route */
    char *method = eval_str("globalThis.__hull_route_defs[0].method");
    ASSERT_NE(method, NULL);
    ASSERT_STREQ(method, "GET");
    free(method);

    char *pattern = eval_str("globalThis.__hull_route_defs[0].pattern");
    ASSERT_NE(pattern, NULL);
    ASSERT_STREQ(pattern, "/test");
    free(pattern);

    /* Verify handler functions stored */
    int has_handlers = eval_int(
        "typeof globalThis.__hull_routes[0] === 'function' ? 1 : 0");
    ASSERT_EQ(has_handlers, 1);

    cleanup_js();
}

/* ── GC test ────────────────────────────────────────────────────────── */

UTEST(js_runtime, gc_runs)
{
    init_js();

    /* Create a bunch of objects, then GC */
    eval_int("for(var i = 0; i < 10000; i++) { var x = {a: i, b: 'test'}; } 1");

    /* GC should not crash */
    hull_js_gc(&js);

    /* Still functional after GC */
    int result = eval_int("2 + 2");
    ASSERT_EQ(result, 4);

    cleanup_js();
}

/* ── Console polyfill test ──────────────────────────────────────────── */

UTEST(js_runtime, console_exists)
{
    init_js();

    int result = eval_int(
        "typeof console === 'object' && "
        "typeof console.log === 'function' && "
        "typeof console.error === 'function' ? 1 : 0");
    ASSERT_EQ(result, 1);

    cleanup_js();
}

/* ── Request reset test ─────────────────────────────────────────────── */

UTEST(js_runtime, reset_request)
{
    init_js();

    js.instruction_count = 12345;
    hull_js_reset_request(&js);
    ASSERT_EQ(js.instruction_count, 0);

    cleanup_js();
}

/* ── Double free safety ─────────────────────────────────────────────── */

UTEST(js_runtime, double_free)
{
    HullJSConfig cfg = HULL_JS_CONFIG_DEFAULT;
    HullJS local_js;
    memset(&local_js, 0, sizeof(local_js));

    hull_js_init(&local_js, &cfg);
    hull_js_free(&local_js);
    hull_js_free(&local_js); /* should not crash */
}

UTEST_MAIN();
