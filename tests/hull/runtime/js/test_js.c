/*
 * test_js_runtime.c — Tests for QuickJS runtime integration
 *
 * Tests: VM init, sandbox, module loading, route registration,
 * instruction limits, memory limits, GC.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/runtime/js.h"
#include "hull/reqctx.h"
#include "hull/manifest.h"
#include "hull/vfs.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/env.h"
#include "quickjs.h"

#include <keel/keel.h>

#include <sqlite3.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Helpers ────────────────────────────────────────────────────────── */

static HlJS js;
static int js_initialized = 0;
static HlVfs platform_vfs;

/* Tests use lots of inline JS snippets that reference modules as
 * globals. Phase 2b removes globals from production — apps must
 * import. This helper restores them for testing convenience by
 * evaluating a module that imports each native module and assigns to
 * globalThis. Modules that aren't available are silently skipped. */
static void install_test_js_globals(HlJS *jsp)
{
    static const char *PRELUDE =
        "const _names = ['crypto','db','env','time','fs','http','smtp',"
        "                'ws','compute','gpu','worker','server','image'];\n"
        "for (const n of _names) {\n"
        "  try {\n"
        "    const mod = await import('hull:' + n);\n"
        "    globalThis[n] = mod[n] || mod.default || mod;\n"
        "  } catch (_) { /* not available — skip */ }\n"
        "}\n";
    JSValue v = JS_Eval(jsp->ctx, PRELUDE, strlen(PRELUDE), "<test-globals>",
                        JS_EVAL_TYPE_MODULE);
    JS_FreeValue(jsp->ctx, v);
    hl_js_run_jobs(jsp);
}

static void init_js(void)
{
    if (js_initialized)
        hl_js_free(&js);
    extern const HlEntry hl_stdlib_entries[];
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    js.base.platform_vfs = &platform_vfs;
    int rc = hl_js_init(&js, &cfg);
    js_initialized = (rc == 0);
    if (js_initialized) install_test_js_globals(&js);
}

/* Variant for tests that need to assert on the module gate. The
 * global-installer in init_js() runs `import 'hull:X'` for every
 * native module — that runs each module's init callback exactly once
 * with module_set = NULL (permissive). Once a module is initialized,
 * QuickJS caches it and the gate never fires again on later imports.
 * Tests that want to observe the gate must skip the installer. */
static void init_js_bare(void)
{
    if (js_initialized)
        hl_js_free(&js);
    extern const HlEntry hl_stdlib_entries[];
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    js.base.platform_vfs = &platform_vfs;
    int rc = hl_js_init(&js, &cfg);
    js_initialized = (rc == 0);
}

static void cleanup_js(void)
{
    if (js_initialized) {
        hl_js_free(&js);
        js_initialized = 0;
    }
}

/* Free HlReqCtx stored on req->ctx by middleware dispatch */
static void free_req_ctx(KlRequest *req)
{
    if (!req->ctx) return;
    HlReqCtx *rctx = (HlReqCtx *)req->ctx;
    if (rctx->kind == HL_REQCTX_JS_VAL && js_initialized) {
        JSValue val;
        memcpy(&val, rctx->js_val_bytes, sizeof(val));
        JS_FreeValue(js.ctx, val);
    } else if (rctx->kind == HL_REQCTX_JSON) {
        free(rctx->json.data);
    }
    free(rctx);
    req->ctx = NULL;
}

/* Init JS with database and env capabilities for testing */
static sqlite3 *test_db = NULL;
static HlStmtCache test_stmt_cache;
static HlDbHandle test_db_handle;
static const char *env_allowed[] = { "HULL_TEST_VAR", NULL };
static HlEnvConfig env_cfg = { .allowed = env_allowed, .count = 1 };

static void init_js_with_caps(void)
{
    extern const HlEntry hl_stdlib_entries[];
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);
    if (js_initialized)
        hl_js_free(&js);
    if (test_db_handle.ctx) {
        hl_db_backend_sqlite.close(test_db_handle.ctx);
        test_db_handle.ctx = NULL;
        test_db = NULL;
    }

    test_db_handle.backend = &hl_db_backend_sqlite;
    if (hl_db_backend_sqlite.open(&test_db_handle.ctx, ":memory:", NULL) != 0)
        return;
    test_db = hl_db_sqlite_raw(&test_db_handle);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    js.base.db_handle = &test_db_handle;
    js.base.hull_db_handle = &test_db_handle;
    js.base.env_cfg = &env_cfg;
    js.base.platform_vfs = &platform_vfs;
    int rc = hl_js_init(&js, &cfg);
    js_initialized = (rc == 0);
    if (js_initialized) install_test_js_globals(&js);
}

static void cleanup_js_caps(void)
{
    if (js_initialized) {
        hl_js_free(&js);
        js_initialized = 0;
    }
    if (test_db_handle.ctx) {
        hl_db_backend_sqlite.close(test_db_handle.ctx);
        test_db_handle.ctx = NULL;
        test_db = NULL;
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
        hl_js_dump_error(&js);
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
        hl_js_dump_error(&js);
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
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    HlJS local_js;
    memset(&local_js, 0, sizeof(local_js));

    int rc = hl_js_init(&local_js, &cfg);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(local_js.rt != NULL);
    ASSERT_TRUE(local_js.ctx != NULL);

    hl_js_free(&local_js);
    ASSERT_TRUE(local_js.rt == NULL);
    ASSERT_TRUE(local_js.ctx == NULL);
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
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    cfg.max_instructions = 1000; /* very low limit */
    HlJS limited_js;
    memset(&limited_js, 0, sizeof(limited_js));

    int rc = hl_js_init(&limited_js, &cfg);
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

    hl_js_free(&limited_js);
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
        hl_js_dump_error(&js);
    /* Module eval may return a promise or undefined — that's OK */
    JS_FreeValue(js.ctx, val);

    /* Run pending jobs (module initialization) */
    hl_js_run_jobs(&js);

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
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

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

/* ── app.router tests ─────────────────────────────────────────────── */

UTEST(js_runtime, app_router_prefixes_routes)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "const r = app.router('/api/v1');\n"
        "r.get('/items', (req, res) => {});\n"
        "r.post('/items', (req, res) => {});\n"
        "r.put('/items/:id', (req, res) => {});\n"
        "r.delete('/items/:id', (req, res) => {});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_route_defs.length"), 4);
    char *p0 = eval_str("globalThis.__hull_route_defs[0].pattern");
    ASSERT_STREQ(p0, "/api/v1/items"); free(p0);
    char *p2 = eval_str("globalThis.__hull_route_defs[2].pattern");
    ASSERT_STREQ(p2, "/api/v1/items/:id"); free(p2);
    char *m3 = eval_str("globalThis.__hull_route_defs[3].method");
    ASSERT_STREQ(m3, "DELETE"); free(m3);

    cleanup_js();
}

UTEST(js_runtime, app_router_nested_composes_prefixes)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "const api = app.router('/api/v1');\n"
        "const admin = api.router('/admin');\n"
        "admin.get('/users', (req, res) => {});\n"
        "admin.get('/audit', (req, res) => {});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_route_defs.length"), 2);
    char *p0 = eval_str("globalThis.__hull_route_defs[0].pattern");
    ASSERT_STREQ(p0, "/api/v1/admin/users"); free(p0);
    char *p1 = eval_str("globalThis.__hull_route_defs[1].pattern");
    ASSERT_STREQ(p1, "/api/v1/admin/audit"); free(p1);

    cleanup_js();
}

UTEST(js_runtime, app_router_use_with_handler_only)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "const r = app.router('/api');\n"
        "r.use((req, res) => 0);\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_middleware.length"), 1);
    char *m0 = eval_str("globalThis.__hull_middleware[0].method");
    ASSERT_STREQ(m0, "*"); free(m0);
    char *p0 = eval_str("globalThis.__hull_middleware[0].pattern");
    ASSERT_STREQ(p0, "/api/*"); free(p0);

    cleanup_js();
}

UTEST(js_runtime, app_router_use_with_explicit_method_pattern)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "const r = app.router('/api');\n"
        "r.use('POST', '/items', (req, res) => 0);\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_middleware.length"), 1);
    char *m0 = eval_str("globalThis.__hull_middleware[0].method");
    ASSERT_STREQ(m0, "POST"); free(m0);
    char *p0 = eval_str("globalThis.__hull_middleware[0].pattern");
    ASSERT_STREQ(p0, "/api/items"); free(p0);

    cleanup_js();
}

UTEST(js_runtime, app_router_chainable)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.router('/api')\n"
        "  .get('/a', () => {})\n"
        "  .post('/b', () => {})\n"
        "  .delete('/c', () => {});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_route_defs.length"), 3);

    cleanup_js();
}

UTEST(js_runtime, app_router_empty_prefix)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "const r = app.router();\n"
        "r.get('/items', () => {});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *p0 = eval_str("globalThis.__hull_route_defs[0].pattern");
    ASSERT_STREQ(p0, "/items"); free(p0);

    cleanup_js();
}

/* ── app.main (CLI mode) tests ─────────────────────────────────────── */

UTEST(js_runtime, app_main_registers)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.main((ctx) => { return 0; });\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int has = eval_int("typeof globalThis.__hull_main === 'function' ? 1 : 0");
    ASSERT_EQ(has, 1);
    ASSERT_TRUE(hl_js_vtable.has_main(&js.base));
    cleanup_js();
}

UTEST(js_runtime, app_main_coexists_with_route_after)
{
    /* app.main + routes are no longer mutually exclusive: app.main
     * is a startup hook, routes are served after it returns. See
     * docs/cli_mode.md and CLAUDE.md "App Lifecycle". */
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "let caught = null;\n"
        "app.main((ctx) => { return 0; });\n"
        "try { app.get('/x', () => {}); } catch (e) { caught = e.message; }\n"
        "globalThis.__test_caught = caught;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *msg = eval_str("globalThis.__test_caught");
    /* No exception expected — globalThis.__test_caught is JS null,
     * which eval_str stringifies to "null". */
    ASSERT_NE(msg, NULL);
    ASSERT_STREQ(msg, "null");
    free(msg);
    cleanup_js();
}

UTEST(js_runtime, route_coexists_with_app_main_after)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "let caught = null;\n"
        "app.get('/x', () => {});\n"
        "try { app.main(() => 0); } catch (e) { caught = e.message; }\n"
        "globalThis.__test_caught = caught;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *msg = eval_str("globalThis.__test_caught");
    /* No exception expected — globalThis.__test_caught is JS null,
     * which eval_str stringifies to "null". */
    ASSERT_NE(msg, NULL);
    ASSERT_STREQ(msg, "null");
    free(msg);
    cleanup_js();
}

UTEST(js_runtime, app_main_via_vtable_runs)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.main((ctx) => { return 5; });\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int exit_code = 99;
    int run_rc = hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 5);
    cleanup_js();
}

UTEST(js_runtime, app_main_undefined_return_yields_zero)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.main(() => {});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int exit_code = 99;
    int run_rc = hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 0);
    cleanup_js();
}

UTEST(js_runtime, app_main_promise_resolved_unwraps)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.main(() => Promise.resolve(11));\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int exit_code = 99;
    int run_rc = hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 11);
    cleanup_js();
}

UTEST(js_runtime, app_main_clamps_large_return)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.main(() => 300);\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int exit_code = 0;
    int run_rc = hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 44);  /* 300 & 0xff */
    cleanup_js();
}

UTEST(js_runtime, has_main_false_when_not_registered)
{
    init_js();
    ASSERT_FALSE(hl_js_vtable.has_main(&js.base));
    cleanup_js();
}

UTEST(js_runtime, app_main_ctx_args_and_env)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.main((ctx) => {\n"
        "  globalThis.__test_args = ctx.args;\n"
        "  globalThis.__test_env = ctx.env.TEST_VAR_JS;\n"
        "  return 0;\n"
        "});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    setenv("TEST_VAR_JS", "jvalue", 1);
    char *argv_in[] = { "one", "two" };
    const char *env_allow[] = { "TEST_VAR_JS", NULL };
    int exit_code = 99;
    int run_rc = hl_js_vtable.run_main(&js.base, NULL, 2, argv_in, env_allow,
                                        &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 0);

    int len = eval_int("globalThis.__test_args.length");
    ASSERT_EQ(len, 2);
    char *first = eval_str("globalThis.__test_args[0]");
    ASSERT_STREQ(first, "one");
    free(first);

    char *envv = eval_str("globalThis.__test_env");
    ASSERT_STREQ(envv, "jvalue");
    free(envv);
    unsetenv("TEST_VAR_JS");
    cleanup_js();
}

/* ── JSON module tests ───────────────────────────────────────────────── */

UTEST(js_runtime, hull_json_encode)
{
    init_js();

    const char *code =
        "import { json } from 'hull:json';\n"
        "globalThis.__test_json = json.encode({a: 1, b: 'two'});\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *s = eval_str("globalThis.__test_json");
    ASSERT_NE(s, NULL);
    ASSERT_STREQ(s, "{\"a\":1,\"b\":\"two\"}");
    free(s);

    cleanup_js();
}

UTEST(js_runtime, hull_json_decode)
{
    init_js();

    const char *code =
        "import { json } from 'hull:json';\n"
        "const t = json.decode('{\"x\":42}');\n"
        "globalThis.__test_val = t.x;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int result = eval_int("globalThis.__test_val");
    ASSERT_EQ(result, 42);

    cleanup_js();
}

UTEST(js_runtime, hull_json_roundtrip)
{
    init_js();

    const char *code =
        "import { json } from 'hull:json';\n"
        "const original = {name: 'hull', count: 7};\n"
        "const decoded = json.decode(json.encode(original));\n"
        "globalThis.__test_rt = (decoded.name === 'hull' && decoded.count === 7) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int result = eval_int("globalThis.__test_rt");
    ASSERT_EQ(result, 1);

    cleanup_js();
}

/* ── GC test ────────────────────────────────────────────────────────── */

UTEST(js_runtime, gc_runs)
{
    init_js();

    /* Create a bunch of objects, then GC */
    eval_int("for(var i = 0; i < 10000; i++) { var x = {a: i, b: 'test'}; } 1");

    /* GC should not crash */
    hl_js_gc(&js);

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
    hl_js_reset_request(&js);
    ASSERT_EQ(js.instruction_count, 0);

    cleanup_js();
}

/* ── Double free safety ─────────────────────────────────────────────── */

UTEST(js_runtime, double_free)
{
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    HlJS local_js;
    memset(&local_js, 0, sizeof(local_js));

    hl_js_init(&local_js, &cfg);
    hl_js_free(&local_js);
    hl_js_free(&local_js); /* should not crash */
}

/* ── GC cleanup on free ─────────────────────────────────────────────── */

UTEST(js_runtime, free_after_modules_no_gc_leak)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.get('/a', (req, res) => {});\n"
        "app.post('/b', (req, res) => {});\n"
        "app.manifest({ env: ['FOO'] });\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* Verify globals are populated */
    int routes = eval_int(
        "globalThis.__hull_route_defs ? globalThis.__hull_route_defs.length : 0");
    ASSERT_EQ(routes, 2);

    /* hl_js_free must clean up all globals without GC assertion */
    cleanup_js();
}

/* ── Crypto tests ──────────────────────────────────────────────────── */

UTEST(js_cap, crypto_sha256)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "globalThis.__test_hash = crypto.sha256('hello');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *hash = eval_str("globalThis.__test_hash");
    ASSERT_NE(hash, NULL);
    ASSERT_STREQ(hash,
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    free(hash);

    cleanup_js_caps();
}

UTEST(js_cap, crypto_random)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "const buf = crypto.random(16);\n"
        "globalThis.__test_rlen = buf.byteLength;\n"
        "const buf2 = crypto.random(16);\n"
        "const a = new Uint8Array(buf);\n"
        "const b = new Uint8Array(buf2);\n"
        "globalThis.__test_rdiffer = a.some((v, i) => v !== b[i]) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int len = eval_int("globalThis.__test_rlen");
    ASSERT_EQ(len, 16);

    int differ = eval_int("globalThis.__test_rdiffer");
    ASSERT_EQ(differ, 1);

    cleanup_js_caps();
}

UTEST(js_cap, crypto_hash_password)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "globalThis.__test_ph = crypto.hashPassword('secret123');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *hash = eval_str("globalThis.__test_ph");
    ASSERT_NE(hash, NULL);
    ASSERT_EQ(strncmp(hash, "pbkdf2:", 7), 0);
    free(hash);

    cleanup_js_caps();
}

UTEST(js_cap, crypto_verify_password)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "const h = crypto.hashPassword('mypass');\n"
        "globalThis.__test_vp_ok = crypto.verifyPassword('mypass', h) ? 1 : 0;\n"
        "globalThis.__test_vp_bad = crypto.verifyPassword('wrong', h) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int ok = eval_int("globalThis.__test_vp_ok");
    ASSERT_EQ(ok, 1);

    int bad = eval_int("globalThis.__test_vp_bad");
    ASSERT_EQ(bad, 0);

    cleanup_js_caps();
}

/* ── Log tests ─────────────────────────────────────────────────────── */

UTEST(js_cap, log_functions_exist)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { log } from 'hull:log';\n"
        "globalThis.__test_log_types = (\n"
        "  typeof log.info === 'function' &&\n"
        "  typeof log.warn === 'function' &&\n"
        "  typeof log.error === 'function' &&\n"
        "  typeof log.debug === 'function'\n"
        ") ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int result = eval_int("globalThis.__test_log_types");
    ASSERT_EQ(result, 1);

    cleanup_js_caps();
}

UTEST(js_cap, log_does_not_throw)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { log } from 'hull:log';\n"
        "log.info('test info');\n"
        "log.warn('test warn');\n"
        "log.error('test error');\n"
        "log.debug('test debug');\n"
        "globalThis.__test_log_ok = 1;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    int is_exc = JS_IsException(val);
    if (is_exc)
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_FALSE(is_exc);
    int result = eval_int("globalThis.__test_log_ok");
    ASSERT_EQ(result, 1);

    cleanup_js_caps();
}

/* ── Env tests ─────────────────────────────────────────────────────── */

UTEST(js_cap, env_get_allowed)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    setenv("HULL_TEST_VAR", "js_test_value", 1);

    const char *code =
        "import { env } from 'hull:env';\n"
        "globalThis.__test_env = env.get('HULL_TEST_VAR');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *v = eval_str("globalThis.__test_env");
    ASSERT_NE(v, NULL);
    ASSERT_STREQ(v, "js_test_value");
    free(v);

    unsetenv("HULL_TEST_VAR");
    cleanup_js_caps();
}

UTEST(js_cap, env_get_blocked)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { env } from 'hull:env';\n"
        "globalThis.__test_env_blocked = (env.get('PATH') === null) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int result = eval_int("globalThis.__test_env_blocked");
    ASSERT_EQ(result, 1);

    cleanup_js_caps();
}

UTEST(js_cap, env_get_nonexistent)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    unsetenv("HULL_TEST_VAR");

    const char *code =
        "import { env } from 'hull:env';\n"
        "globalThis.__test_env_none = (env.get('HULL_TEST_VAR') === null) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int result = eval_int("globalThis.__test_env_none");
    ASSERT_EQ(result, 1);

    cleanup_js_caps();
}

/* ── DB tests ──────────────────────────────────────────────────────── */

UTEST(js_cap, db_exec_and_query)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { db } from 'hull:db';\n"
        "db.exec('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)');\n"
        "db.exec('INSERT INTO t (name) VALUES (?)', ['alice']);\n"
        "const rows = db.query('SELECT name FROM t');\n"
        "globalThis.__test_db_name = rows[0].name;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *name = eval_str("globalThis.__test_db_name");
    ASSERT_NE(name, NULL);
    ASSERT_STREQ(name, "alice");
    free(name);

    cleanup_js_caps();
}

UTEST(js_cap, db_last_id)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { db } from 'hull:db';\n"
        "db.exec('CREATE TABLE t2 (id INTEGER PRIMARY KEY, v TEXT)');\n"
        "db.exec('INSERT INTO t2 (v) VALUES (?)', ['a']);\n"
        "const id1 = db.lastId();\n"
        "db.exec('INSERT INTO t2 (v) VALUES (?)', ['b']);\n"
        "const id2 = db.lastId();\n"
        "globalThis.__test_db_ids = (id2 > id1) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int result = eval_int("globalThis.__test_db_ids");
    ASSERT_EQ(result, 1);

    cleanup_js_caps();
}

UTEST(js_cap, db_parameterized_query)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { db } from 'hull:db';\n"
        "db.exec('CREATE TABLE t3 (id INTEGER PRIMARY KEY, val INTEGER)');\n"
        "db.exec('INSERT INTO t3 (val) VALUES (?)', [10]);\n"
        "db.exec('INSERT INTO t3 (val) VALUES (?)', [20]);\n"
        "db.exec('INSERT INTO t3 (val) VALUES (?)', [30]);\n"
        "const rows = db.query('SELECT val FROM t3 WHERE val > ?', [15]);\n"
        "globalThis.__test_db_pq = rows.length;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int count = eval_int("globalThis.__test_db_pq");
    ASSERT_EQ(count, 2);

    cleanup_js_caps();
}

UTEST(js_cap, db_not_available_without_config)
{
    /* Use default init (no db) — hull:db module should not be registered */
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { db } from 'hull:db';\n"
        "globalThis.__test_db_avail = 1;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    int is_exc = JS_IsException(val);
    if (is_exc) {
        /* Expected — module not registered */
        JSValue exc = JS_GetException(js.ctx);
        JS_FreeValue(js.ctx, exc);
    }
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* The import should have thrown */
    ASSERT_TRUE(is_exc);

    cleanup_js();
}

/* ── DB namespace protection tests ──────────────────────────────────── */

UTEST(js_cap, db_namespace_blocks_hull_tables)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { db } from 'hull:db';\n"
        "try {\n"
        "  db.exec('CREATE TABLE _hull_test (id INT)');\n"
        "  globalThis.__test_ns_block = 0;\n"
        "} catch (e) {\n"
        "  globalThis.__test_ns_block = String(e).includes('reserved') ? 1 : 0;\n"
        "}\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int result = eval_int("globalThis.__test_ns_block");
    ASSERT_EQ(result, 1);

    cleanup_js_caps();
}

UTEST(js_cap, db_namespace_blocks_hull_query)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { db } from 'hull:db';\n"
        "try {\n"
        "  db.query('SELECT * FROM _hull_outbox');\n"
        "  globalThis.__test_ns_qblock = 0;\n"
        "} catch (e) {\n"
        "  globalThis.__test_ns_qblock = String(e).includes('reserved') ? 1 : 0;\n"
        "}\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int result = eval_int("globalThis.__test_ns_qblock");
    ASSERT_EQ(result, 1);

    cleanup_js_caps();
}

UTEST(js_cap, db_namespace_no_internal_bypass)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* db._exec and db._query must not exist — no bypass possible */
    const char *code =
        "import { db } from 'hull:db';\n"
        "globalThis.__test_ns_nobypass = "
        "  (db._exec === undefined && db._query === undefined) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int result = eval_int("globalThis.__test_ns_nobypass");
    ASSERT_EQ(result, 1);

    cleanup_js_caps();
}

UTEST(js_cap, db_namespace_allows_normal_tables)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { db } from 'hull:db';\n"
        "db.exec('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)');\n"
        "db.exec('INSERT INTO users (name) VALUES (?)', ['alice']);\n"
        "const rows = db.query('SELECT name FROM users');\n"
        "globalThis.__test_ns_normal = rows[0].name;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *name = eval_str("globalThis.__test_ns_normal");
    ASSERT_NE(name, NULL);
    ASSERT_STREQ(name, "alice");
    free(name);

    cleanup_js_caps();
}

/* ── Manifest tests ────────────────────────────────────────────────── */

UTEST(js_cap, app_manifest_store_and_get)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({\n"
        "  fs: { read: ['/tmp', '/data'], write: ['/uploads'] },\n"
        "  env: ['PORT', 'DATABASE_URL'],\n"
        "  hosts: ['api.stripe.com'],\n"
        "});\n"
        "const m = app.getManifest();\n"
        "globalThis.__test_manifest_present = (m !== null) ? 1 : 0;\n"
        "globalThis.__test_manifest_env_count = m.env.length;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int present = eval_int("globalThis.__test_manifest_present");
    ASSERT_EQ(present, 1);

    int env_count = eval_int("globalThis.__test_manifest_env_count");
    ASSERT_EQ(env_count, 2);

    cleanup_js();
}

UTEST(js_cap, manifest_extract_js)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({\n"
        "  fs: { read: ['/tmp', '/data'], write: ['/uploads'] },\n"
        "  env: ['PORT', 'DATABASE_URL'],\n"
        "  hosts: ['api.stripe.com', 'api.sendgrid.com'],\n"
        "});\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* Extract manifest via C API */
    HlManifest manifest;
    int rc = hl_manifest_extract_js(js.ctx, &manifest, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(manifest.present, 1);

    ASSERT_EQ(manifest.fs_read_count, 2);
    ASSERT_STREQ(manifest.fs_read[0], "/tmp");
    ASSERT_STREQ(manifest.fs_read[1], "/data");

    ASSERT_EQ(manifest.fs_write_count, 1);
    ASSERT_STREQ(manifest.fs_write[0], "/uploads");

    ASSERT_EQ(manifest.env_count, 2);
    ASSERT_STREQ(manifest.env[0], "PORT");
    ASSERT_STREQ(manifest.env[1], "DATABASE_URL");

    ASSERT_EQ(manifest.hosts_count, 2);
    ASSERT_STREQ(manifest.hosts[0], "api.stripe.com");
    ASSERT_STREQ(manifest.hosts[1], "api.sendgrid.com");

    hl_manifest_free(&manifest);
    cleanup_js();
}

UTEST(js_cap, manifest_extract_js_no_manifest)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    /* No app.manifest() called — extraction should fail */
    HlManifest manifest;
    int rc = hl_manifest_extract_js(js.ctx, &manifest, NULL);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(manifest.present, 0);

    cleanup_js();
}

UTEST(js_cap, manifest_extract_js_partial)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    /* Manifest with only env — no fs or hosts */
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ env: ['PORT'] });\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    HlManifest manifest;
    int rc = hl_manifest_extract_js(js.ctx, &manifest, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(manifest.present, 1);
    ASSERT_EQ(manifest.fs_read_count, 0);
    ASSERT_EQ(manifest.fs_write_count, 0);
    ASSERT_EQ(manifest.env_count, 1);
    ASSERT_STREQ(manifest.env[0], "PORT");
    ASSERT_EQ(manifest.hosts_count, 0);

    hl_manifest_free(&manifest);
    cleanup_js();
}

/* ── Middleware tests ────────────────────────────────────────────────── */

UTEST(js_middleware, registration_stores_handler_id)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.use('*', '/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* Verify __hull_middleware has handler_id */
    int mw_count = eval_int(
        "globalThis.__hull_middleware ? globalThis.__hull_middleware.length : 0");
    ASSERT_EQ(mw_count, 1);

    char *method = eval_str("globalThis.__hull_middleware[0].method");
    ASSERT_NE(method, NULL);
    ASSERT_STREQ(method, "*");
    free(method);

    char *pattern = eval_str("globalThis.__hull_middleware[0].pattern");
    ASSERT_NE(pattern, NULL);
    ASSERT_STREQ(pattern, "/*");
    free(pattern);

    int handler_id = eval_int("globalThis.__hull_middleware[0].handler_id");
    ASSERT_TRUE(handler_id >= 0);

    /* Verify handler is in __hull_routes */
    int has_handler = eval_int(
        "typeof globalThis.__hull_routes[globalThis.__hull_middleware[0].handler_id] === 'function' ? 1 : 0");
    ASSERT_EQ(has_handler, 1);

    cleanup_js();
}

UTEST(js_middleware, handler_ids_do_not_collide_with_routes)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.get('/test', (req, res) => {});\n"
        "app.use('*', '/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int route_id = eval_int("globalThis.__hull_route_defs[0].handler_id");
    int mw_id = eval_int("globalThis.__hull_middleware[0].handler_id");
    ASSERT_NE(route_id, mw_id);

    /* Both should be valid function entries */
    int route_fn = eval_int(
        "typeof globalThis.__hull_routes[globalThis.__hull_route_defs[0].handler_id] === 'function' ? 1 : 0");
    ASSERT_EQ(route_fn, 1);
    int mw_fn = eval_int(
        "typeof globalThis.__hull_routes[globalThis.__hull_middleware[0].handler_id] === 'function' ? 1 : 0");
    ASSERT_EQ(mw_fn, 1);

    cleanup_js();
}

UTEST(js_middleware, dispatch_return_zero_continues)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.use('*', '/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int handler_id = eval_int("globalThis.__hull_middleware[0].handler_id");

    KlRequest req = {0};
    KlResponse res = {0};
    int result = hl_js_dispatch_middleware(&js, handler_id, &req, &res);
    ASSERT_EQ(result, 0);

    free_req_ctx(&req);
    cleanup_js();
}

UTEST(js_middleware, dispatch_return_nonzero_short_circuits)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.use('*', '/*', (req, res) => 1);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int handler_id = eval_int("globalThis.__hull_middleware[0].handler_id");

    KlRequest req = {0};
    KlResponse res = {0};
    int result = hl_js_dispatch_middleware(&js, handler_id, &req, &res);
    ASSERT_EQ(result, 1);

    free_req_ctx(&req);
    cleanup_js();
}

/* Track allocations from wire_routes_server to free them later */
static void *wiring_allocs_js[16];
static int   wiring_alloc_count_js;

static void *tracking_alloc_js(size_t size)
{
    void *p = malloc(size);
    if (p && wiring_alloc_count_js < 16)
        wiring_allocs_js[wiring_alloc_count_js++] = p;
    return p;
}

UTEST(js_middleware, wiring_to_server)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.get('/test', (req, res) => {});\n"
        "app.use('*', '/*', (req, res) => 0);\n"
        "app.use('GET', '/api/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    KlServer server;
    KlConfig cfg = {
        .port = 0,
        .max_connections = 1,
        .alloc = NULL,
    };
    kl_server_init(&server, &cfg);

    wiring_alloc_count_js = 0;
    int rc = hl_js_wire_routes_server(&js, &server, tracking_alloc_js);
    ASSERT_EQ(rc, 0);

    /* Verify middleware was registered */
    ASSERT_EQ(server.router.mw_count, 2);

    /* Free tracked allocations (route + middleware contexts) */
    for (int i = 0; i < wiring_alloc_count_js; i++)
        free(wiring_allocs_js[i]);

    kl_server_free(&server);
    cleanup_js();
}

UTEST(js_middleware, order_preserved)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.use('*', '/*', (req, res) => 0);\n"
        "app.use('GET', '/api/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int mw_count = eval_int("globalThis.__hull_middleware.length");
    ASSERT_EQ(mw_count, 2);

    char *m1 = eval_str("globalThis.__hull_middleware[0].method");
    ASSERT_STREQ(m1, "*");
    free(m1);

    char *p1 = eval_str("globalThis.__hull_middleware[0].pattern");
    ASSERT_STREQ(p1, "/*");
    free(p1);

    char *m2 = eval_str("globalThis.__hull_middleware[1].method");
    ASSERT_STREQ(m2, "GET");
    free(m2);

    char *p2 = eval_str("globalThis.__hull_middleware[1].pattern");
    ASSERT_STREQ(p2, "/api/*");
    free(p2);

    cleanup_js();
}

/* ── HMAC-SHA256 / base64url tests ─────────────────────────────────── */

UTEST(js_cap, crypto_hmac_sha256)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* RFC 4231 Test Case 2: key="Jefe", data="what do ya want for nothing?" */
    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "globalThis.__test_hmac = crypto.hmacSha256("
        "  'what do ya want for nothing?', '4a656665');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *hmac = eval_str("globalThis.__test_hmac");
    ASSERT_NE(hmac, NULL);
    ASSERT_STREQ(hmac,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    free(hmac);

    cleanup_js_caps();
}

UTEST(js_cap, crypto_base64url_roundtrip)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "globalThis.__test_b64_enc = crypto.base64urlEncode('Hello, World!');\n"
        "globalThis.__test_b64_dec = crypto.base64urlDecode('SGVsbG8sIFdvcmxkIQ');\n"
        "const orig = 'test data 123!@#';\n"
        "globalThis.__test_b64_rt = crypto.base64urlDecode(crypto.base64urlEncode(orig)) === orig ? 1 : 0;\n"
        "globalThis.__test_b64_inv = crypto.base64urlDecode('!!!invalid!!!') === null ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *enc = eval_str("globalThis.__test_b64_enc");
    ASSERT_NE(enc, NULL);
    ASSERT_STREQ(enc, "SGVsbG8sIFdvcmxkIQ");
    free(enc);

    char *dec = eval_str("globalThis.__test_b64_dec");
    ASSERT_NE(dec, NULL);
    ASSERT_STREQ(dec, "Hello, World!");
    free(dec);

    ASSERT_EQ(eval_int("globalThis.__test_b64_rt"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_b64_inv"), 1);

    cleanup_js_caps();
}

/* ── hull:cookie tests ─────────────────────────────────────────────── */

UTEST(js_stdlib, cookie_parse)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { cookie } from 'hull:cookie';\n"
        "const r = cookie.parse('session=abc; theme=dark');\n"
        "globalThis.__test_cp = (r.session === 'abc' && r.theme === 'dark') ? 1 : 0;\n"
        "const e = cookie.parse('');\n"
        "globalThis.__test_ce = Object.keys(e).length === 0 ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_cp"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_ce"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, cookie_serialize)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { cookie } from 'hull:cookie';\n"
        "globalThis.__test_cs = cookie.serialize('sid', 'abc123');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *cookie = eval_str("globalThis.__test_cs");
    ASSERT_NE(cookie, NULL);
    ASSERT_NE(strstr(cookie, "sid=abc123"), NULL);
    ASSERT_NE(strstr(cookie, "HttpOnly"), NULL);
    ASSERT_NE(strstr(cookie, "Secure"), NULL);  /* default is true */
    ASSERT_NE(strstr(cookie, "SameSite=Lax"), NULL);
    ASSERT_NE(strstr(cookie, "Path=/"), NULL);
    free(cookie);

    cleanup_js_caps();
}

UTEST(js_stdlib, cookie_clear)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { cookie } from 'hull:cookie';\n"
        "globalThis.__test_cc = cookie.clear('sid');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *cookie = eval_str("globalThis.__test_cc");
    ASSERT_NE(cookie, NULL);
    ASSERT_NE(strstr(cookie, "sid="), NULL);
    ASSERT_NE(strstr(cookie, "Max-Age=0"), NULL);
    free(cookie);

    cleanup_js_caps();
}

/* ── hull:middleware:session tests ─────────────────────────────────── */

UTEST(js_stdlib, session_create_and_load)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { session } from 'hull:middleware:session';\n"
        "session.init({ ttl: 3600 });\n"
        "const id = session.create({ userId: 42, email: 'test@example.com' });\n"
        "globalThis.__test_sid_len = id ? id.length : 0;\n"
        "const data = session.load(id);\n"
        "globalThis.__test_sl = (data && data.userId === 42 && data.email === 'test@example.com') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_sid_len"), 64);
    ASSERT_EQ(eval_int("globalThis.__test_sl"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, session_destroy)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { session } from 'hull:middleware:session';\n"
        "session.init();\n"
        "const id = session.create({ foo: 'bar' });\n"
        "session.destroy(id);\n"
        "globalThis.__test_sd = session.load(id) === null ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_sd"), 1);

    cleanup_js_caps();
}

/* ── hull:jwt tests ────────────────────────────────────────────────── */

UTEST(js_stdlib, jwt_sign_and_verify)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { jwt } from 'hull:jwt';\n"
        "const token = jwt.sign({ userId: 1, exp: 9999999999 }, 'mysecret');\n"
        "globalThis.__test_jt = token ? 1 : 0;\n"
        "const result = jwt.verify(token, 'mysecret');\n"
        "globalThis.__test_jv = (result && result[0] && result[0].userId === 1) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_jt"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_jv"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, jwt_tampered_signature)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { jwt } from 'hull:jwt';\n"
        "const token = jwt.sign({ userId: 1, exp: 9999999999 }, 'mysecret');\n"
        "const result = jwt.verify(token, 'wrongsecret');\n"
        "globalThis.__test_jts = (Array.isArray(result) && result[0] === null && "
        "  result[1] === 'invalid signature') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_jts"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, jwt_decode_without_verify)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { jwt } from 'hull:jwt';\n"
        "const token = jwt.sign({ userId: 99 }, 'secret');\n"
        "const payload = jwt.decode(token);\n"
        "globalThis.__test_jd = (payload && payload.userId === 99) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_jd"), 1);

    cleanup_js_caps();
}

/* ── hull:middleware:csrf tests ────────────────────────────────────── */

UTEST(js_stdlib, csrf_generate_and_verify)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csrf } from 'hull:middleware:csrf';\n"
        "const token = csrf.generate('session123', 'my_csrf_secret');\n"
        "globalThis.__test_cg = token ? 1 : 0;\n"
        "globalThis.__test_cv = csrf.verify(token, 'session123', 'my_csrf_secret') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_cg"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_cv"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, csrf_wrong_session_rejected)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csrf } from 'hull:middleware:csrf';\n"
        "const token = csrf.generate('session123', 'secret');\n"
        "globalThis.__test_cws = csrf.verify(token, 'other_session', 'secret') ? 0 : 1;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_cws"), 1);

    cleanup_js_caps();
}

/* ── hull:middleware:auth tests (smoke — modules load and expose API) */

UTEST(js_cap, crypto_hmac_sha256_verify)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "const mac = crypto.hmacSha256('what do ya want for nothing?', '4a656665');\n"
        "globalThis.__test_hv_ok = crypto.hmacSha256Verify('what do ya want for nothing?', '4a656665', mac) ? 1 : 0;\n"
        "globalThis.__test_hv_bad_mac = crypto.hmacSha256Verify('what do ya want for nothing?', '4a656665', "
        "  '0000000000000000000000000000000000000000000000000000000000000000') ? 1 : 0;\n"
        "const mac2 = crypto.hmacSha256('hello', '4a656665');\n"
        "globalThis.__test_hv_bad_key = crypto.hmacSha256Verify('hello', 'deadbeef', mac2) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* Correct MAC → true */
    ASSERT_EQ(eval_int("globalThis.__test_hv_ok"), 1);

    /* Wrong MAC → false */
    ASSERT_EQ(eval_int("globalThis.__test_hv_bad_mac"), 0);

    /* Wrong key → false */
    ASSERT_EQ(eval_int("globalThis.__test_hv_bad_key"), 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, auth_module_loads)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { auth } from 'hull:middleware:auth';\n"
        "globalThis.__test_am = ("
        "  typeof auth.sessionMiddleware === 'function' &&\n"
        "  typeof auth.jwtMiddleware === 'function' &&\n"
        "  typeof auth.login === 'function' &&\n"
        "  typeof auth.logout === 'function'\n"
        ") ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_am"), 1);

    cleanup_js_caps();
}

/* ── hull:form tests ─────────────────────────────────────────────────── */

UTEST(js_stdlib, form_parse)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { form } from 'hull:form';\n"
        "const r = form.parse('email=a%40b.com&pass=hello+world');\n"
        "globalThis.__test_fp = (r.email === 'a@b.com' && r.pass === 'hello world') ? 1 : 0;\n"
        "const e = form.parse('');\n"
        "globalThis.__test_fe = Object.keys(e).length === 0 ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_fp"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_fe"), 1);

    cleanup_js();
}

/* ── hull:validate tests ─────────────────────────────────────────────── */

UTEST(js_stdlib, validate_check_required)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { validate } from 'hull:validate';\n"
        "var r1 = validate.check({}, { name: { required: true } });\n"
        "globalThis.__test_vr1 = (r1[0] === false && r1[1].name === 'is required') ? 1 : 0;\n"
        "var r2 = validate.check({ name: 'alice' }, { name: { required: true } });\n"
        "globalThis.__test_vr2 = r2[0] ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_vr1"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_vr2"), 1);

    cleanup_js();
}

UTEST(js_stdlib, validate_check_min_max)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { validate } from 'hull:validate';\n"
        "var r1 = validate.check({ pw: 'abc' }, { pw: { min: 8 } });\n"
        "globalThis.__test_vmm1 = (r1[0] === false && r1[1].pw === 'must be at least 8 characters') ? 1 : 0;\n"
        "var r2 = validate.check({ n: 'toolong' }, { n: { max: 3 } });\n"
        "globalThis.__test_vmm2 = (r2[0] === false && r2[1].n === 'must be at most 3 characters') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_vmm1"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_vmm2"), 1);

    cleanup_js();
}

UTEST(js_stdlib, validate_check_email)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { validate } from 'hull:validate';\n"
        "var r1 = validate.check({ e: 'a@b.com' }, { e: { email: true } });\n"
        "globalThis.__test_ve1 = r1[0] ? 1 : 0;\n"
        "var r2 = validate.check({ e: 'notanemail' }, { e: { email: true } });\n"
        "globalThis.__test_ve2 = (r2[0] === false && r2[1].e === 'is not a valid email') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_ve1"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_ve2"), 1);

    cleanup_js();
}

/* ── hull:i18n tests ─────────────────────────────────────────────────── */

UTEST(js_stdlib, i18n_load_and_translate)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { i18n } from 'hull:i18n';\n"
        "i18n.reset();\n"
        "i18n.load('en', { greeting: 'Hello', nav: { home: 'Home' } });\n"
        "i18n.locale('en');\n"
        "globalThis.__test_i18n_t = (\n"
        "  i18n.t('greeting') === 'Hello' &&\n"
        "  i18n.t('nav.home') === 'Home' &&\n"
        "  i18n.t('missing') === 'missing'\n"
        ") ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_i18n_t"), 1);

    cleanup_js();
}

UTEST(js_stdlib, i18n_interpolation)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { i18n } from 'hull:i18n';\n"
        "i18n.reset();\n"
        "i18n.load('en', { total: 'Total: ${amount}' });\n"
        "i18n.locale('en');\n"
        "globalThis.__test_i18n_interp = "
        "  (i18n.t('total', { amount: '42' }) === 'Total: 42') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_i18n_interp"), 1);

    cleanup_js();
}

UTEST(js_stdlib, i18n_number_and_date)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { i18n } from 'hull:i18n';\n"
        "i18n.reset();\n"
        "i18n.load('en', { format: { decimalSep: '.', thousandsSep: ',', datePattern: 'YYYY-MM-DD' } });\n"
        "i18n.locale('en');\n"
        "globalThis.__test_i18n_num = (i18n.number(1500) === '1,500') ? 1 : 0;\n"
        "globalThis.__test_i18n_date = (i18n.date(0) === '1970-01-01') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_i18n_num"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_i18n_date"), 1);

    cleanup_js();
}

UTEST(js_stdlib, i18n_detect)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { i18n } from 'hull:i18n';\n"
        "i18n.reset();\n"
        "i18n.load('en', {});\n"
        "i18n.load('hu', {});\n"
        "globalThis.__test_i18n_det = (\n"
        "  i18n.detect('hu,en;q=0.9') === 'hu' &&\n"
        "  i18n.detect('en-US') === 'en' &&\n"
        "  i18n.detect('ja') === null\n"
        ") ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_i18n_det"), 1);

    cleanup_js();
}

/* ── hull:email tests ─────────────────────────────────────────────── */

UTEST(js_stdlib, email_validation)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { email } from 'hull:email';\n"
        "var r1 = await email.send(null);\n"
        "globalThis.__test_ev1 = (r1.ok === false && r1.error === 'opts required') ? 1 : 0;\n"
        "var r2 = await email.send({ to: 'x@y.com', subject: 's', body: 'b' });\n"
        "globalThis.__test_ev2 = (r2.ok === false && r2.error === 'from required') ? 1 : 0;\n"
        "var r3 = await email.send({ from: 'x@y.com', subject: 's', body: 'b' });\n"
        "globalThis.__test_ev3 = (r3.ok === false && r3.error === 'to required') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_ev1"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_ev2"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_ev3"), 1);

    cleanup_js();
}

UTEST(js_stdlib, email_unknown_provider)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { email } from 'hull:email';\n"
        "var r = await email.send({ provider: 'foo', from: 'a@b.com', "
        "to: 'c@d.com', subject: 's', body: 'b' });\n"
        "globalThis.__test_eup = (r.ok === false && r.error.indexOf('unknown provider') >= 0) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_eup"), 1);

    cleanup_js();
}

UTEST(js_stdlib, email_api_key_required)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { email } from 'hull:email';\n"
        "var r = await email.send({ provider: 'postmark', from: 'a@b.com', "
        "to: 'c@d.com', subject: 's', body: 'b' });\n"
        "globalThis.__test_eak = (r.ok === false && r.error.indexOf('api_key required') >= 0) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_eak"), 1);

    cleanup_js();
}

/* ── hull:csv tests ──────────────────────────────────────────────────── */

UTEST(js_stdlib, csv_parse_basic)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csv } from 'hull:csv';\n"
        "const rows = csv.parse('a,b,c\\n1,2,3\\n');\n"
        "globalThis.__test_cpb = (rows.length === 2 && rows[0][0] === 'a' && rows[1][2] === '3') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_cpb"), 1);

    cleanup_js();
}

UTEST(js_stdlib, csv_parse_headers)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csv } from 'hull:csv';\n"
        "const rows = csv.parse('name,age\\nalice,30\\n', { headers: true });\n"
        "globalThis.__test_cph = (rows.length === 1 && rows[0].name === 'alice' && rows[0].age === '30') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_cph"), 1);

    cleanup_js();
}

UTEST(js_stdlib, csv_parse_quoted)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csv } from 'hull:csv';\n"
        "const rows = csv.parse('\"a,b\",c\\n');\n"
        "globalThis.__test_cpq = (rows[0][0] === 'a,b' && rows[0][1] === 'c') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_cpq"), 1);

    cleanup_js();
}

UTEST(js_stdlib, csv_encode_basic)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csv } from 'hull:csv';\n"
        "globalThis.__test_ceb = csv.encode([['a','b','c'],['1','2','3']]);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *s = eval_str("globalThis.__test_ceb");
    ASSERT_NE(s, NULL);
    ASSERT_STREQ(s, "a,b,c\n1,2,3\n");
    free(s);

    cleanup_js();
}

UTEST(js_stdlib, csv_encode_headers)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csv } from 'hull:csv';\n"
        "const result = csv.encode([{name:'alice', age:'30'}], { headers: true });\n"
        "const rows = csv.parse(result, { headers: true });\n"
        "globalThis.__test_ceh = (rows.length === 1 && rows[0].name === 'alice') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_ceh"), 1);

    cleanup_js();
}

/* ── hull:search tests ───────────────────────────────────────────────── */

UTEST(js_stdlib, search_create_and_query)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { search } from 'hull:search';\n"
        "search.createIndex('test_articles', ['title', 'body']);\n"
        "search.index('test_articles', '1', {title: 'Hello World', body: 'Test article about searching'});\n"
        "search.index('test_articles', '2', {title: 'JS Guide', body: 'Learn JavaScript programming'});\n"
        "const results = search.query('test_articles', 'javascript');\n"
        "globalThis.__test_scq = (results.length === 1 && results[0].id === '2') ? 1 : 0;\n"
        "search.dropIndex('test_articles');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_scq"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, search_remove)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { search } from 'hull:search';\n"
        "search.createIndex('test_rm', ['title']);\n"
        "search.index('test_rm', '1', {title: 'hello'});\n"
        "search.index('test_rm', '2', {title: 'world'});\n"
        "search.remove('test_rm', '1');\n"
        "const results = search.query('test_rm', 'hello');\n"
        "globalThis.__test_srm = (results.length === 0) ? 1 : 0;\n"
        "search.dropIndex('test_rm');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_srm"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, search_snippet)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { search } from 'hull:search';\n"
        "search.createIndex('test_snip', ['title', 'content']);\n"
        "search.index('test_snip', '1', {title: 'Guide', content: 'A comprehensive guide to searching'});\n"
        "const results = search.query('test_snip', 'guide', {\n"
        "  snippet: { column: 2, tokens: 10, before: '<b>', after: '</b>' }\n"
        "});\n"
        "globalThis.__test_ssn = (results.length >= 1) ? 1 : 0;\n"
        "search.dropIndex('test_snip');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_ssn"), 1);

    cleanup_js_caps();
}

/* ── hull:middleware:rbac tests ───────────────────────────────────────── */

UTEST(js_stdlib, rbac_init_and_assign)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { rbac } from 'hull:middleware:rbac';\n"
        "rbac.init();\n"
        "rbac.defineRole('admin');\n"
        "rbac.definePermission('users.read');\n"
        "rbac.grant('admin', 'users.read');\n"
        "rbac.assign('user1', 'admin');\n"
        "globalThis.__test_ria = 1;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_ria"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, rbac_has_role)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { rbac } from 'hull:middleware:rbac';\n"
        "rbac.init();\n"
        "rbac.defineRole('admin');\n"
        "rbac.assign('user1', 'admin');\n"
        "globalThis.__test_rhr = (rbac.hasRole('user1', 'admin') === true &&\n"
        "  rbac.hasRole('user1', 'editor') === false) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_rhr"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, rbac_has_permission)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { rbac } from 'hull:middleware:rbac';\n"
        "rbac.init();\n"
        "rbac.defineRole('admin');\n"
        "rbac.definePermission('users.read');\n"
        "rbac.definePermission('users.write');\n"
        "rbac.grant('admin', 'users.read');\n"
        "rbac.assign('user1', 'admin');\n"
        "globalThis.__test_rhp = (rbac.hasPermission('user1', 'users.read') === true &&\n"
        "  rbac.hasPermission('user1', 'users.write') === false) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_rhp"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, rbac_middleware_deny)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { rbac } from 'hull:middleware:rbac';\n"
        "rbac.init();\n"
        "const mw = rbac.requireRole('admin');\n"
        "globalThis.__test_rmd = (typeof mw === 'function') ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_rmd"), 1);

    cleanup_js_caps();
}

/* ── Module-set gating in import resolver ─────────────────────────── */
/* Phase-2a gate in hl_js_module_loader: when the runtime has a
 * non-NULL module_set, hull:* names that map to a known first-party
 * module must be in that set or the import throws.
 *
 * Note: the gate only catches stdlib .js modules (loaded via VFS).
 * Native C modules registered at init time (hull:db, hull:crypto, ...)
 * still resolve through QuickJS' own module cache and bypass the
 * loader; their gating arrives in phase 2b together with the
 * import-only refactor. */

#include "hull/manifest.h"
#include "hull/module_registry.h"
#include "hull/module_resolver.h"

UTEST(js_runtime, import_gated_undeclared_stdlib_fails)
{
    init_js();

    HlResolvedModuleSet set;
    hl_module_set_clear(&set);
    js.base.module_set = &set;

    /* hull:validate is a .js stdlib file — gating must intercept. */
    const char *code = "import { validate } from 'hull:validate';\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    ASSERT_TRUE(JS_IsException(val));
    JSValue exc = JS_GetException(js.ctx);
    const char *msg = JS_ToCString(js.ctx, exc);
    ASSERT_NE(msg, NULL);
    ASSERT_NE(strstr(msg, "hull:validate"), NULL);
    ASSERT_NE(strstr(msg, "app.manifest"), NULL);
    ASSERT_NE(strstr(msg, "hull modules available"), NULL);
    JS_FreeCString(js.ctx, msg);
    JS_FreeValue(js.ctx, exc);
    JS_FreeValue(js.ctx, val);

    js.base.module_set = NULL;
    cleanup_js();
}

UTEST(js_runtime, import_gated_declared_stdlib_succeeds)
{
    init_js();

    HlManifest m;
    memset(&m, 0, sizeof(m));
    m.modules[0].name = "validate";
    m.modules[0].api_major = 1;
    m.modules_count = 1;
    m.modules_declared = 1;

    HlResolvedModuleSet set;
    char err[256] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &set, err, sizeof(err)), 0);
    js.base.module_set = &set;

    const char *code =
        "import { validate } from 'hull:validate';\n"
        "globalThis.__gate_ok = (validate && typeof validate.check === 'function') ? 1 : 0;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    ASSERT_EQ(eval_int("globalThis.__gate_ok"), 1);

    js.base.module_set = NULL;
    cleanup_js();
}

UTEST(js_runtime, import_null_module_set_is_permissive)
{
    /* NULL module_set = legacy entry points: gating disabled. */
    init_js();
    ASSERT_EQ(js.base.module_set, NULL);

    const char *code =
        "import { validate } from 'hull:validate';\n"
        "globalThis.__legacy_ok = (validate && typeof validate.check === 'function') ? 1 : 0;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    ASSERT_EQ(eval_int("globalThis.__legacy_ok"), 1);

    cleanup_js();
}

UTEST(js_runtime, import_gated_undeclared_native_module_fails)
{
    /* Phase 2c: native C modules (hull:crypto, hull:time, ...) now
     * self-gate via hl_js_check_module_declared inside their init
     * callbacks. Undeclared imports throw ReferenceError on first use.
     *
     * Use init_js_bare() so the init callback for hull:crypto hasn't
     * yet been triggered — the gate fires there exactly once per VM. */
    init_js_bare();

    HlResolvedModuleSet set;
    hl_module_set_clear(&set);
    js.base.module_set = &set;

    /* Module evaluation in QuickJS returns a Promise. A failed init
     * callback rejects that promise; the rejection reason is the gate
     * error. Inspect it directly via JS_PromiseResult instead of
     * relying on JS_Eval to return JS_EXCEPTION. */
    const char *code = "import { crypto } from 'hull:crypto';\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    hl_js_run_jobs(&js);

    JSPromiseStateEnum st = JS_PromiseState(js.ctx, val);
    ASSERT_EQ(st, JS_PROMISE_REJECTED);

    JSValue reason = JS_PromiseResult(js.ctx, val);
    const char *msg = JS_ToCString(js.ctx, reason);
    ASSERT_NE(msg, NULL);
    ASSERT_NE(strstr(msg, "hull:crypto"), NULL);
    ASSERT_NE(strstr(msg, "app.manifest"), NULL);
    ASSERT_NE(strstr(msg, "hull modules available"), NULL);
    JS_FreeCString(js.ctx, msg);
    JS_FreeValue(js.ctx, reason);
    JS_FreeValue(js.ctx, val);

    js.base.module_set = NULL;
    cleanup_js();
}

UTEST(js_runtime, import_gated_declared_native_module_succeeds)
{
    init_js_bare();

    HlManifest m;
    memset(&m, 0, sizeof(m));
    m.modules[0].name = "crypto";
    m.modules[0].api_major = 1;
    m.modules_count = 1;
    m.modules_declared = 1;

    HlResolvedModuleSet set;
    char err[256] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &set, err, sizeof(err)), 0);
    js.base.module_set = &set;

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "globalThis.__native_ok = (crypto && typeof crypto.sha256 === 'function') ? 1 : 0;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    ASSERT_EQ(eval_int("globalThis.__native_ok"), 1);

    js.base.module_set = NULL;
    cleanup_js();
}

UTEST(js_runtime, import_image_is_a_real_hull_module)
{
    /* Phase 2c: image is no longer a global on globalThis — it must be
     * imported from hull:image like every other native module. */
    init_js_bare();

    /* Without declaration: import rejects the module-eval promise. */
    HlResolvedModuleSet empty;
    hl_module_set_clear(&empty);
    js.base.module_set = &empty;

    const char *fail_code = "import { image } from 'hull:image';\n";
    JSValue v1 = JS_Eval(js.ctx, fail_code, strlen(fail_code), "<test>",
                         JS_EVAL_TYPE_MODULE);
    hl_js_run_jobs(&js);

    JSPromiseStateEnum st = JS_PromiseState(js.ctx, v1);
    ASSERT_EQ(st, JS_PROMISE_REJECTED);
    JSValue reason = JS_PromiseResult(js.ctx, v1);
    const char *msg = JS_ToCString(js.ctx, reason);
    ASSERT_NE(strstr(msg, "hull:image"), NULL);
    JS_FreeCString(js.ctx, msg);
    JS_FreeValue(js.ctx, reason);
    JS_FreeValue(js.ctx, v1);
    js.base.module_set = NULL;
    cleanup_js();
}

UTEST_MAIN();
