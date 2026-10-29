/*
 * test_js_runtime.c - Tests for QuickJS runtime integration
 *
 * Tests: VM init, sandbox, module loading, route registration,
 * instruction limits, memory limits, GC.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* FTW_DEPTH / FTW_PHYS are XSI extensions to nftw; on glibc they're
 * only declared when _XOPEN_SOURCE >= 500. macOS exposes them
 * unconditionally AND uses _XOPEN_SOURCE to gate Darwin extensions
 * the other way (defining it hides clock_gettime_nsec_np /
 * CLOCK_UPTIME_RAW that utest.h needs), so this define has to stay
 * Linux-only. Matches the convention in tests/hull/test_tools_install.c
 * and tests/hull/cap/test_fs.c. */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "../../client_ip_matrix.h"
#include "utest.h"
#include "hull/runtime/js.h"
#include "hull/runtime/js_bytecode_cache.h"
#include "hull/runtime/js_template_cache.h"
#include "hull/reqctx.h"
#include "hull/manifest.h"
#include "hull/vfs.h"
#include "hull/stdlib_feature.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"
#include "hull/cap/db_registry.h"
#include "hull/shared/async_backend.h"
#include "hull/shared/req_life.h"
#include <stdatomic.h>
#include <pthread.h>
#include "hull/cap/env.h"
#include "quickjs.h"

#include <keel/keel.h>

#include <sqlite3.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "hull/worker_db.h"   /* hl_worker_db_init */
#include "log.h"              /* log_add_callback (js_task tests) */
#include "hull/entry.h"       /* HlEntry (js_audit9 compute.stream) */
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm.h"    /* HlWasmCache (js_audit9 compute.stream) */
#endif
#include "../../test_tmpdir.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

static HlJS js;
static int js_initialized = 0;
static HlVfs platform_vfs;
/* Merged baseUruntime stdlib array for platform_vfs; disposed before each
 * re-init so priors don't accumulate. The live one stays reachable via this
 * static (LSan-clean). */
static void *platform_vfs_owned = NULL;

/* Tests use lots of inline JS snippets that reference modules as
 * globals. The runtime removes globals from production - apps must
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
        "  } catch (_) { /* not available - skip */ }\n"
        "}\n"
        /* The db module now exposes only connect/default; the test snippets
         * use db.query/exec/... directly, so expose the default connection as
         * the `db` global (mirrors app code doing dbModule.default()). */
        "if (globalThis.db && globalThis.db.default)"
        " globalThis.db = globalThis.db.default();\n";
    JSValue v = JS_Eval(jsp->ctx, PRELUDE, strlen(PRELUDE), "<test-globals>",
                        JS_EVAL_TYPE_MODULE);
    JS_FreeValue(jsp->ctx, v);
    hl_js_run_jobs(jsp);

    /* __testHooks(name): the test hooks stdlib module `name` registered with
     * hull:_test_hooks (audit 16: no longer exported on the module, where app
     * code could call them). Only a hull: module may import that internal
     * module, so the harness evaluates its own under a hull: name - from C,
     * which no script can do. A module registers its hooks when it loads: the
     * script asking for them has imported it first. */
    static const char *HOOKS =
        "import { _test_hooks } from 'hull:_test_hooks';\n"
        "globalThis.__testHooks = (name) => _test_hooks.get(name);\n";
    v = JS_Eval(jsp->ctx, HOOKS, strlen(HOOKS), "hull:__test_harness",
                JS_EVAL_TYPE_MODULE);
    JS_FreeValue(jsp->ctx, v);
    hl_js_run_jobs(jsp);
}

/* The runtime's __hull_* tables live in a store held in C (audit 15), out
 * of app code's reach. The tests read and tamper with them as before through
 * a test-only global onto that store. */
JSValue hl_js_internals(JSContext *ctx);   /* runtime/js/internal.h */

static void expose_internals(HlJS *jsp)
{
    JSValue global = JS_GetGlobalObject(jsp->ctx);
    JS_SetPropertyStr(jsp->ctx, global, "__hull_internals",
                      hl_js_internals(jsp->ctx));
    JS_FreeValue(jsp->ctx, global);
}

static void init_js(void)
{
    if (js_initialized)
        hl_js_free(&js);
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    js.base.platform_vfs = &platform_vfs;
    int rc = hl_js_init(&js, &cfg);
    js_initialized = (rc == 0);
    if (js_initialized) expose_internals(&js);
    if (js_initialized) install_test_js_globals(&js);
}

/* Variant for tests that need to assert on the module gate. The
 * global-installer in init_js() runs `import 'hull:X'` for every
 * native module - that runs each module's init callback exactly once
 * with module_set = NULL (permissive). Once a module is initialized,
 * QuickJS caches it and the gate never fires again on later imports.
 * Tests that want to observe the gate must skip the installer. */
static void init_js_bare(void)
{
    if (js_initialized)
        hl_js_free(&js);
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    js.base.platform_vfs = &platform_vfs;
    int rc = hl_js_init(&js, &cfg);
    js_initialized = (rc == 0);
    if (js_initialized) expose_internals(&js);
}

static void cleanup_js(void)
{
    if (js_initialized) {
        hl_js_free(&js);
        js_initialized = 0;
    }
}

#ifdef HL_ENABLE_HTTP_SERVER
/* Free HlReqCtx stored on req->ctx by middleware dispatch */
static void free_req_ctx(KlHttpRequest *req)
{
    if (!req->ctx) return;
    HlReqCtx *rctx = (HlReqCtx *)req->ctx;
    if (js_initialized)
        hl_reqctx_untrack(&js.req_ctxs, rctx);   /* dispatch.c tracks it */
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
#endif /* HL_ENABLE_HTTP_SERVER */

/* Init JS with database and env capabilities for testing */
static sqlite3 *test_db = NULL;
static HlDbHandle test_db_handle;
static HlDbRegistry *test_db_registry;
static const char *env_allowed[] = { "HULL_TEST_VAR", NULL };
static HlEnvConfig env_cfg = { .allowed = env_allowed, .count = 1 };

/* A loop + pool for the NEXT init_js_with_caps: hull:worker registers
 * only when a thread pool exists at init. Consumed (cleared) there. */
static HlAsyncBackendCtx  *pending_async_ctx;
static HlAsyncBackendPool *pending_thread_pool;

static void init_js_with_caps(void)
{
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    if (js_initialized)
        hl_js_free(&js);
    if (test_db_registry) {
        hl_db_registry_destroy(test_db_registry);
        test_db_registry = NULL;
    }
    if (test_db_handle.ctx) {
        hl_db_backend_sqlite.close(&test_db_handle);
        test_db_handle.ctx = NULL;
        test_db = NULL;
    }

    test_db_handle.backend = &hl_db_backend_sqlite;
    if (hl_db_backend_sqlite.open(&test_db_handle.ctx, ":memory:", NULL) != 0)
        return;
    test_db = hl_db_sqlite_raw(&test_db_handle);
    /* Wrap the test connection as the registry's "default" (seeded, so the
     * registry does not own/close it; this harness does). */
    test_db_registry = hl_db_registry_create(NULL, NULL, NULL);
    if (test_db_registry)
        hl_db_registry_seed(test_db_registry, "default", &test_db_handle);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    js.base.db_registry = test_db_registry;
    js.base.env_cfg = &env_cfg;
    js.base.platform_vfs = &platform_vfs;
    js.base.async_ctx   = pending_async_ctx;
    js.base.thread_pool = pending_thread_pool;
    pending_async_ctx   = NULL;
    pending_thread_pool = NULL;
    int rc = hl_js_init(&js, &cfg);
    js_initialized = (rc == 0);
    if (js_initialized) expose_internals(&js);
    if (js_initialized) install_test_js_globals(&js);
}

static void cleanup_js_caps(void)
{
    if (js_initialized) {
        hl_js_free(&js);
        js_initialized = 0;
    }
    if (test_db_registry) {
        hl_db_registry_destroy(test_db_registry);
        test_db_registry = NULL;
    }
    if (test_db_handle.ctx) {
        hl_db_backend_sqlite.close(&test_db_handle);
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

/* Run a co-located JS test script from disk in the caps-bearing test context.
 *
 * The JS counterpart to run_lua_test (tests/hull/lua_script_test.h), with one
 * deliberate difference: that one uses a VANILLA lua_State with no capability
 * layer, whereas this evaluates inside the context init_js_with_caps() built,
 * so a script may use db and the rest. `hull:*` imports resolve through the
 * platform VFS that context already installs -- the same path the inline
 * js_stdlib tests take when they import hull:search.
 *
 * Contract. Path is REPO-ROOT-RELATIVE (tests run from the repo root) and the
 * script must finish by publishing its counts on the global object:
 *
 *     globalThis.__test_pass = pass;
 *     globalThis.__test_fail = fail;
 *
 * NOT `export default { pass, fail }` -- a module's default export is not
 * reachable through globalThis, so the harness would read nothing and the
 * suite would look empty rather than broken.
 *
 * Returns 0 when the script evaluated and published counts; -1 on any
 * harness-level failure (unreadable file, a throw during evaluation, or
 * missing counts). Callers assert on the return code -- no utest macros here,
 * they are only valid inside a UTEST body. */
static int run_js_test(const char *script_path, int *pass_out, int *fail_out)
{
    *pass_out = 0;
    *fail_out = -1;

    FILE *f = fopen(script_path, "rb");
    if (!f) {
        fprintf(stderr, "\n%s: cannot open\n", script_path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    /* Clear any counts a previous script left, so a script that throws before
     * publishing is reported as a harness failure rather than silently
     * inheriting the last suite's numbers. */
    JSValue reset = JS_Eval(js.ctx,
        "globalThis.__test_pass = undefined; globalThis.__test_fail = undefined;",
        strlen("globalThis.__test_pass = undefined; globalThis.__test_fail = undefined;"),
        "<reset>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(js.ctx, reset);

    /* A script is a whole suite in one uninterrupted run, not one entry, so
     * the run watchdog's per-run deadline (60 s) does not apply to it: under
     * MSan the audit-16 suites run past it. Off for the script (and the runs
     * nested in it), any deadline already armed cleared. */
    hl_run_watchdog_configure(0);
    hl_run_watch_disarm(&js.run_watch);
    JSValue val = JS_Eval(js.ctx, buf, got, script_path, JS_EVAL_TYPE_MODULE);
    int failed = JS_IsException(val);
    if (failed) hl_js_dump_error(&js);
    free(buf);

    if (!failed) {
        hl_js_run_jobs(&js);

        /* Module evaluation returns a PROMISE, so a throw in the script body
         * -- or a module-resolver gate refusing one of its imports -- rejects
         * that promise instead of making JS_Eval return JS_EXCEPTION. Checking
         * only JS_IsException therefore sails straight past a script that blew
         * up, and the caller is told "published no counts", which points at
         * the script's tail when the real fault was anywhere above it. Read
         * the rejection reason and say so. (Same treatment as the module-gate
         * tests further down this file.) */
        if (JS_IsObject(val) &&
            JS_PromiseState(js.ctx, val) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(js.ctx, val);
            const char *msg = JS_ToCString(js.ctx, reason);
            fprintf(stderr, "\n%s: module evaluation REJECTED: %s\n",
                    script_path, msg ? msg : "(no message)");
            if (msg) JS_FreeCString(js.ctx, msg);
            JS_FreeValue(js.ctx, reason);
            failed = 1;
        }
    }

    JS_FreeValue(js.ctx, val);
    hl_run_watchdog_configure(-1);
    if (failed) return -1;

    if (eval_int("typeof globalThis.__test_pass === 'number' ? 1 : 0") != 1) {
        fprintf(stderr, "\n%s: published no __test_pass/__test_fail counts\n",
                script_path);
        return -1;
    }
    *pass_out = eval_int("globalThis.__test_pass");
    *fail_out = eval_int("globalThis.__test_fail");

    fprintf(stderr, "  %s: %d passed, %d failed\n", script_path, *pass_out, *fail_out);
    return 0;
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
        /* Expected - dynamic import disabled or std not available */
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
    /* Module eval may return a promise or undefined - that's OK */
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

/* ── script names decide trust, so they are not caller-supplied ───── */

UTEST(js_template_bridge, compile_cannot_forge_a_stdlib_script_name)
{
    /* js_is_stdlib_caller reads JS_GetScriptOrModuleName and grants _hull_*
     * table access to any frame whose name starts with "hull:". The name
     * handed to _template.compile becomes exactly that, because the cache
     * passes it to JS_Eval - so taking it verbatim would let any code that
     * can reach this bridge mint a script that claims to be stdlib.
     *
     * The name is built instead: the caller's string lands after a
     * "template:" prefix, so the result cannot begin with "hull:". */
    init_js();

    const char *code =
        "import { _template } from 'hull:_template';\n"
        "const f = _template.compile(\n"
        "  '(function(){ return function(){ throw new Error(\"boom\"); }; })()',\n"
        "  'hull:forged');\n"
        "try { f(); } catch (e) { globalThis.__tpl_stack = String(e.stack); }\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "hull:tests:template_bridge",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *stack = eval_str("globalThis.__tpl_stack || ''");
    ASSERT_NE(stack, NULL);

    /* QuickJS names the frame with the script name, so the stack reports
     * what the compiled script ended up being called. */
    ASSERT_TRUE_MSG(strstr(stack, "template:hull:forged") != NULL,
                    "script name should carry the forced template prefix");
    free(stack);

    cleanup_js();
}

UTEST(js_template_bridge, compile_without_a_name_still_works)
{
    init_js();

    const char *code =
        "import { _template } from 'hull:_template';\n"
        "const f = _template.compile("
        "'(function(){ return function(){ return 1; }; })()');\n"
        "globalThis.__tpl_ok = (typeof f === 'function' && f() === 1) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "hull:tests:template_bridge",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ_MSG(eval_int("globalThis.__tpl_ok|0"), 1,
                  "a nameless compile keeps working");

    cleanup_js();
}

UTEST(js_runtime, csv_encode_sanitize_formulas)
{
    init_js();

    /* Import hull:csv and encode with the (default-on) formula sanitizer,
     * and with it switched off. */
    const char *code =
        "import { csv } from 'hull:csv';\n"
        "globalThis.__csv_plain = csv.encode([['=cmd|calc']], { sanitizeFormulas: false });\n"
        "globalThis.__csv_safe = csv.encode("
        "  [['=cmd|calc'],['@x'],['-2+3+cmd|x'],['-5'],['+3.2'],['1e-3'],['ok']]);\n"
        "globalThis.__csv_alias = csv.encode([['=x']], { sanitize_formulas: false });\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* Opted out: the formula cell is emitted verbatim. */
    ASSERT_EQ(eval_int("globalThis.__csv_plain === '=cmd|calc\\n' ? 1 : 0"), 1);
    /* Default: leading = / @ / - prefixed with '; plain numbers and a
     * non-formula cell untouched. */
    ASSERT_EQ(eval_int(
        "globalThis.__csv_safe === \"'=cmd|calc\\n'@x\\n'-2+3+cmd|x\\n-5\\n+3.2\\n1e-3\\nok\\n\" ? 1 : 0"), 1);
    /* snake_case alias (Lua parity) also switches it. */
    ASSERT_EQ(eval_int("globalThis.__csv_alias === '=x\\n' ? 1 : 0"), 1);

    cleanup_js();
}

UTEST(js_runtime, hull_app_module)
{
    init_js();

    /* Register routes via hull:app */
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
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
        "globalThis.__hull_internals.__hull_route_defs ? globalThis.__hull_internals.__hull_route_defs.length : 0");
    ASSERT_EQ(count, 2);

    /* Verify first route */
    char *method = eval_str("globalThis.__hull_internals.__hull_route_defs[0].method");
    ASSERT_NE(method, NULL);
    ASSERT_STREQ(method, "GET");
    free(method);

    char *pattern = eval_str("globalThis.__hull_internals.__hull_route_defs[0].pattern");
    ASSERT_NE(pattern, NULL);
    ASSERT_STREQ(pattern, "/test");
    free(pattern);

    /* Verify handler functions stored */
    int has_handlers = eval_int(
        "typeof globalThis.__hull_internals.__hull_routes[0] === 'function' ? 1 : 0");
    ASSERT_EQ(has_handlers, 1);

    cleanup_js();
}

/* ── app.router tests ─────────────────────────────────────────────── */

UTEST(js_runtime, app_router_prefixes_routes)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
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

    ASSERT_EQ(eval_int("globalThis.__hull_internals.__hull_route_defs.length"), 4);
    char *p0 = eval_str("globalThis.__hull_internals.__hull_route_defs[0].pattern");
    ASSERT_STREQ(p0, "/api/v1/items"); free(p0);
    char *p2 = eval_str("globalThis.__hull_internals.__hull_route_defs[2].pattern");
    ASSERT_STREQ(p2, "/api/v1/items/:id"); free(p2);
    char *m3 = eval_str("globalThis.__hull_internals.__hull_route_defs[3].method");
    ASSERT_STREQ(m3, "DELETE"); free(m3);

    cleanup_js();
}

UTEST(js_runtime, app_router_nested_composes_prefixes)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "const api = app.router('/api/v1');\n"
        "const admin = api.router('/admin');\n"
        "admin.get('/users', (req, res) => {});\n"
        "admin.get('/audit', (req, res) => {});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_internals.__hull_route_defs.length"), 2);
    char *p0 = eval_str("globalThis.__hull_internals.__hull_route_defs[0].pattern");
    ASSERT_STREQ(p0, "/api/v1/admin/users"); free(p0);
    char *p1 = eval_str("globalThis.__hull_internals.__hull_route_defs[1].pattern");
    ASSERT_STREQ(p1, "/api/v1/admin/audit"); free(p1);

    cleanup_js();
}

UTEST(js_runtime, app_router_use_with_handler_only)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "const r = app.router('/api');\n"
        "r.use((req, res) => 0);\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_internals.__hull_middleware.length"), 1);
    char *m0 = eval_str("globalThis.__hull_internals.__hull_middleware[0].method");
    ASSERT_STREQ(m0, "*"); free(m0);
    char *p0 = eval_str("globalThis.__hull_internals.__hull_middleware[0].pattern");
    ASSERT_STREQ(p0, "/api/*"); free(p0);

    cleanup_js();
}

UTEST(js_runtime, app_router_use_with_explicit_method_pattern)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "const r = app.router('/api');\n"
        "r.use('POST', '/items', (req, res) => 0);\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_internals.__hull_middleware.length"), 1);
    char *m0 = eval_str("globalThis.__hull_internals.__hull_middleware[0].method");
    ASSERT_STREQ(m0, "POST"); free(m0);
    char *p0 = eval_str("globalThis.__hull_internals.__hull_middleware[0].pattern");
    ASSERT_STREQ(p0, "/api/items"); free(p0);

    cleanup_js();
}

UTEST(js_runtime, app_router_chainable)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.router('/api')\n"
        "  .get('/a', () => {})\n"
        "  .post('/b', () => {})\n"
        "  .delete('/c', () => {});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_internals.__hull_route_defs.length"), 3);

    cleanup_js();
}

/* ── hull/timers decoration tests ────────────────────────────────────
 *
 * app.every / app.daily are conditionally installed by app.manifest
 * when the manifest's modules array contains "hull/timers@*". Without
 * the declaration the methods literally don't exist on `app` -
 * accessing them returns undefined, calling them throws TypeError. */

UTEST(js_runtime, app_timers_absent_without_declaration)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "globalThis.__test_every_type = typeof app.every;\n"
        "globalThis.__test_daily_type = typeof app.daily;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *e = eval_str("globalThis.__test_every_type");
    ASSERT_STREQ(e, "undefined"); free(e);
    char *d = eval_str("globalThis.__test_daily_type");
    ASSERT_STREQ(d, "undefined"); free(d);
    cleanup_js();
}

UTEST(js_runtime, app_timers_present_when_declared)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/timers@1'] });\n"
        "globalThis.__test_every_type = typeof app.every;\n"
        "globalThis.__test_daily_type = typeof app.daily;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *e = eval_str("globalThis.__test_every_type");
    ASSERT_STREQ(e, "function"); free(e);
    char *d = eval_str("globalThis.__test_daily_type");
    ASSERT_STREQ(d, "function"); free(d);
    cleanup_js();
}

UTEST(js_runtime, app_timers_register_timer_when_declared)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/timers@1'] });\n"
        "app.every(1000, () => {});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__hull_internals.__hull_timer_defs.length"), 1);
    cleanup_js();
}

UTEST(js_runtime, app_router_empty_prefix)
{
    init_js();
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "const r = app.router();\n"
        "r.get('/items', () => {});\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *p0 = eval_str("globalThis.__hull_internals.__hull_route_defs[0].pattern");
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

    int has = eval_int("typeof globalThis.__hull_internals.__hull_main === 'function' ? 1 : 0");
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
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "let caught = null;\n"
        "app.main((ctx) => { return 0; });\n"
        "try { app.get('/x', () => {}); } catch (e) { caught = e.message; }\n"
        "globalThis.__test_caught = caught;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *msg = eval_str("globalThis.__test_caught");
    /* No exception expected - globalThis.__test_caught is JS null,
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
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "let caught = null;\n"
        "app.get('/x', () => {});\n"
        "try { app.main(() => 0); } catch (e) { caught = e.message; }\n"
        "globalThis.__test_caught = caught;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *msg = eval_str("globalThis.__test_caught");
    /* No exception expected - globalThis.__test_caught is JS null,
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

static void stop_main_loop(void *user)
{
    hl_async_backend()->stop(user);
}

static int register_cli_main(const char *body)
{
    JSValue v = JS_Eval(js.ctx, body, strlen(body), "<cli-main>", JS_EVAL_TYPE_MODULE);
    int ok = !JS_IsException(v);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);
    return ok;
}

UTEST(js_runtime, async_main_needs_a_backend_not_a_server)
{
    init_js();
    HlAsyncBackendCtx *loop = NULL;
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_EQ(be->init(&loop, NULL), 0);
    js.base.async_ctx = loop;
    ASSERT_TRUE(register_cli_main("import { app } from 'hull:app';"
        "app.main(async () => { await hull.sleep(1); return 7; });"));
    int exit_code = 99;
    EXPECT_EQ(hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code), 0);
    EXPECT_EQ(exit_code, 7);
    EXPECT_TRUE(js.sleep_timers == NULL);
    cleanup_js();
    be->free(loop);
}

UTEST(js_runtime, unfinished_main_is_failure_and_releases_sleep)
{
    init_js();
    HlAsyncBackendCtx *loop = NULL;
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_EQ(be->init(&loop, NULL), 0);
    js.base.async_ctx = loop;
    ASSERT_TRUE(register_cli_main("import { app } from 'hull:app';"
        "app.main(async () => { await hull.sleep(60000); return 7; });"));
    ASSERT_GT(be->timer_add(loop, 0, stop_main_loop, loop), (uint64_t)0);
    int exit_code = 99;
    EXPECT_EQ(hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code), -1);
    EXPECT_EQ(exit_code, 1);
    EXPECT_TRUE(js.sleep_timers == NULL);
    EXPECT_TRUE(js.cli_main_value == NULL);
    EXPECT_EQ(js.cli_main_active, 0);
    cleanup_js();  /* QuickJS must have no outstanding C-owned promise refs. */
    EXPECT_EQ(be->tick(loop, 0), 0);
    be->free(loop);
}

UTEST(js_runtime, throwing_main_releases_its_unawaited_sleep)
{
    init_js();
    HlAsyncBackendCtx *loop = NULL;
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_EQ(be->init(&loop, NULL), 0);
    js.base.async_ctx = loop;
    ASSERT_TRUE(register_cli_main("import { app } from 'hull:app';"
        "app.main(() => { hull.sleep(60000); throw new Error('main failed'); });"));
    int exit_code = 99;
    EXPECT_EQ(hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code), -1);
    EXPECT_EQ(exit_code, 1);
    EXPECT_TRUE(js.sleep_timers == NULL);
    cleanup_js();
    be->free(loop);
}

UTEST(js_runtime, runtime_teardown_releases_an_unawaited_sleep)
{
    init_js();
    HlAsyncBackendCtx *loop = NULL;
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_EQ(be->init(&loop, NULL), 0);
    js.base.async_ctx = loop;
    ASSERT_TRUE(register_cli_main("import { app } from 'hull:app';"
        "app.main(() => { hull.sleep(60000); return 0; });"));
    int exit_code = 99;
    EXPECT_EQ(hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code), 0);
    EXPECT_EQ(exit_code, 0);
    cleanup_js();
    EXPECT_EQ(be->tick(loop, 0), 0);
    be->free(loop);
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
        /* Manifest must come first so app.get/post are installed
         * before they are called (hull/http-server@1 decoration). */
        "app.manifest({ env: ['FOO'], modules: ['hull/http-server@1'] });\n"
        "app.get('/a', (req, res) => {});\n"
        "app.post('/b', (req, res) => {});\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* Verify globals are populated */
    int routes = eval_int(
        "globalThis.__hull_internals.__hull_route_defs ? globalThis.__hull_internals.__hull_route_defs.length : 0");
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
        "import { encoding } from 'hull:encoding';\n"
        "globalThis.__test_hash = encoding.hex.encode(crypto.sha256('hello'));\n";

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
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
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
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
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

/* One statement is charged to the run's instruction budget (audit 9 H4): a
 * recursive CTE in one db.query held the event loop for good. The trip is
 * the uncatchable limit, not a SQL error a catch block could loop on. */
UTEST(db_audit9, js_a_runaway_query_hits_the_instruction_limit)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    js.max_instructions = 1000000;
    hl_js_budget_arm(&js);

    const char *code =
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
        "try { db.query('WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) AS n FROM c'); } catch (e) { globalThis.__caught = 1; }\n"
        "globalThis.__after = 1;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    EXPECT_EQ(1, js.budget_tripped);

    js.max_instructions = 0;
    hl_js_budget_arm(&js);
    EXPECT_EQ(0, eval_int("globalThis.__caught === undefined ? 0 : 1"));
    EXPECT_EQ(0, eval_int("globalThis.__after === undefined ? 0 : 1"));
    cleanup_js_caps();
}

/* Audit 10 H3: SQL allocations are charged by size, so a loop of
 * allocation-heavy one-opcode queries hits the limit. */
UTEST(db_audit10, js_sql_allocations_are_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    js.max_instructions = 2000000;
    hl_js_budget_arm(&js);

    const char *code =
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
        "try { for (let i = 0; i < 5000; i++) db.query('SELECT length(randomblob(200000)) AS n'); } catch (e) { globalThis.__caught = 1; }\n"
        "globalThis.__after = 1;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    EXPECT_EQ(1, js.budget_tripped);

    js.max_instructions = 0;
    hl_js_budget_arm(&js);
    EXPECT_EQ(0, eval_int("globalThis.__caught === undefined ? 0 : 1"));
    EXPECT_EQ(0, eval_int("globalThis.__after === undefined ? 0 : 1"));
    cleanup_js_caps();
}

UTEST(js_cap, db_parameterized_query)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
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
    /* Use default init (no db) - hull:db module should not be registered */
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
        "globalThis.__test_db_avail = 1;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    int is_exc = JS_IsException(val);
    if (is_exc) {
        /* Expected - module not registered */
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
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
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
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
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

    /* db._exec and db._query must not exist - no bypass possible */
    const char *code =
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
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
        "import { db as dbMod } from 'hull:db';\nconst db = dbMod.default();\n"
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

    /* No app.manifest() called - extraction should fail */
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

    /* Manifest with only env - no fs or hosts */
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
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* Verify __hull_middleware has handler_id */
    int mw_count = eval_int(
        "globalThis.__hull_internals.__hull_middleware ? globalThis.__hull_internals.__hull_middleware.length : 0");
    ASSERT_EQ(mw_count, 1);

    char *method = eval_str("globalThis.__hull_internals.__hull_middleware[0].method");
    ASSERT_NE(method, NULL);
    ASSERT_STREQ(method, "*");
    free(method);

    char *pattern = eval_str("globalThis.__hull_internals.__hull_middleware[0].pattern");
    ASSERT_NE(pattern, NULL);
    ASSERT_STREQ(pattern, "/*");
    free(pattern);

    int handler_id = eval_int("globalThis.__hull_internals.__hull_middleware[0].handler_id");
    ASSERT_TRUE(handler_id >= 0);

    /* Verify handler is in __hull_routes */
    int has_handler = eval_int(
        "typeof globalThis.__hull_internals.__hull_routes[globalThis.__hull_internals.__hull_middleware[0].handler_id] === 'function' ? 1 : 0");
    ASSERT_EQ(has_handler, 1);

    cleanup_js();
}

UTEST(js_middleware, handler_ids_do_not_collide_with_routes)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.get('/test', (req, res) => {});\n"
        "app.use('*', '/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int route_id = eval_int("globalThis.__hull_internals.__hull_route_defs[0].handler_id");
    int mw_id = eval_int("globalThis.__hull_internals.__hull_middleware[0].handler_id");
    ASSERT_NE(route_id, mw_id);

    /* Both should be valid function entries */
    int route_fn = eval_int(
        "typeof globalThis.__hull_internals.__hull_routes[globalThis.__hull_internals.__hull_route_defs[0].handler_id] === 'function' ? 1 : 0");
    ASSERT_EQ(route_fn, 1);
    int mw_fn = eval_int(
        "typeof globalThis.__hull_internals.__hull_routes[globalThis.__hull_internals.__hull_middleware[0].handler_id] === 'function' ? 1 : 0");
    ASSERT_EQ(mw_fn, 1);

    cleanup_js();
}

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_middleware, dispatch_return_zero_continues)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int handler_id = eval_int("globalThis.__hull_internals.__hull_middleware[0].handler_id");

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    int result = hl_js_dispatch_middleware(&js, handler_id, &req, &res);
    ASSERT_EQ(result, 0);

    free_req_ctx(&req);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* A `res` kept past its request fails closed: middleware stashes it, the
 * request finishes, and using it later throws instead of writing into a
 * response that was sent (on a connection that may be gone). */
UTEST(js_middleware, res_kept_past_its_request_fails_closed)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', (req, res) => { globalThis.kept = res; res.status(201); return 0; });\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int handler_id = eval_int("globalThis.__hull_internals.__hull_middleware[0].handler_id");
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    ASSERT_EQ(hl_js_dispatch_middleware(&js, handler_id, &req, &res), 0);
    EXPECT_EQ(res.status, 201);   /* usable while the request runs */

    EXPECT_EQ(eval_int("(() => { try { globalThis.kept.status(500); return 0; }"
                       " catch (e) { return String(e).includes('has finished') ? 1 : 2; } })()"),
              1);
    EXPECT_EQ(res.status, 201);   /* and nothing was written after */

    free_req_ctx(&req);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* A handler left pending with no Hull continuation was never suspended: the
 * response goes out when dispatch returns. Its `res` used to stay live
 * afterwards, onto a finished request. */
static int js_dispatch_last_route(const char *handler_src, KlHttpResponse *res,
                                  KlHttpRequest *req)
{
    char code[1024];
    snprintf(code, sizeof code,
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.get('/x', %s);\n", handler_src);
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    int id = eval_int("globalThis.__hull_internals.__hull_routes.length - 1");
    return hl_js_dispatch(&js, id, req, res);
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_dispatch, a_microtask_only_handler_completes_and_closes_res)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    EXPECT_EQ(js_dispatch_last_route(
        "async (req, res) => { globalThis.kept = res; await Promise.resolve();"
        " res.status(203); }", &res, &req), 0);
    EXPECT_EQ(res.status, 203);
    EXPECT_EQ(eval_int("(() => { try { globalThis.kept.status(500); return 0; }"
                       " catch (e) { return String(e).includes('has finished') ? 1 : 2; } })()"),
              1);
    EXPECT_EQ(res.status, 203);
    free_req_ctx(&req);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* A body that is not valid UTF-8 also arrives as its bytes (audit 24):
 * req.body replaces every invalid sequence with U+FFFD, so the idempotency
 * fingerprint read two different binary bodies as the same text. */
static int js_dispatch_body(const char *body, size_t len)
{
    char buf[16];
    memcpy(buf, body, len);
    KlHttpBufReader br;
    memset(&br, 0, sizeof br);
    br.data = buf;
    br.len = len;
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    req.body_reader = &br.base;
    int rc = js_dispatch_last_route(
        "(req, res) => { const b = req.bodyBytes;"
        " res.status(b === undefined ? 200"
        " : b instanceof ArrayBuffer ? 300 + b.byteLength : 500); }", &res, &req);
    free_req_ctx(&req);
    return rc == 0 ? res.status : -1;
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_audit24, a_body_that_is_not_utf8_also_arrives_as_bytes)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(js_dispatch_body("ab\xff" "cd", 5), 305);     /* lossy: bytes too */
    EXPECT_EQ(js_dispatch_body("h\xc3\xa9", 3), 200);      /* valid: text only */
    EXPECT_EQ(js_dispatch_body("\xed\xa0\x80", 3), 303);  /* a surrogate */
    EXPECT_EQ(js_dispatch_body("\xc0\xaf", 2), 302);         /* an overlong */
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_dispatch, a_handler_awaiting_an_undriven_promise_ends_its_request)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    EXPECT_EQ(js_dispatch_last_route(
        "async (req, res) => { globalThis.kept = res; res.status(202);"
        " await new Promise(() => {}); res.status(500); }", &res, &req), 0);
    EXPECT_EQ(res.status, 202);
    EXPECT_EQ(eval_int("(() => { try { globalThis.kept.status(500); return 0; }"
                       " catch (e) { return String(e).includes('has finished') ? 1 : 2; } })()"),
              1);
    EXPECT_EQ(res.status, 202);   /* nothing written after the request ended */
    free_req_ctx(&req);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_middleware, dispatch_return_nonzero_short_circuits)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', (req, res) => 1);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int handler_id = eval_int("globalThis.__hull_internals.__hull_middleware[0].handler_id");

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    int result = hl_js_dispatch_middleware(&js, handler_id, &req, &res);
    ASSERT_EQ(result, 1);

    free_req_ctx(&req);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
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
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.get('/test', (req, res) => {});\n"
        "app.use('*', '/*', (req, res) => 0);\n"
        "app.use('GET', '/api/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    KlHttpServer server;
    KlHttpServerConfig cfg = {
        .port = 0,
        .max_connections = 1,
        .alloc = NULL,
    };
    kl_http_server_init(&server, &cfg);

    wiring_alloc_count_js = 0;
    int rc = hl_js_wire_routes_server(&js, &server, tracking_alloc_js);
    ASSERT_EQ(rc, 0);

    /* Verify middleware was registered */
    ASSERT_EQ(server.router.mw_count, 2);

    /* Free tracked allocations (route + middleware contexts) */
    for (int i = 0; i < wiring_alloc_count_js; i++)
        free(wiring_allocs_js[i]);

    kl_http_server_free(&server);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

UTEST(js_middleware, order_preserved)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', (req, res) => 0);\n"
        "app.use('GET', '/api/*', (req, res) => 0);\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    int mw_count = eval_int("globalThis.__hull_internals.__hull_middleware.length");
    ASSERT_EQ(mw_count, 2);

    char *m1 = eval_str("globalThis.__hull_internals.__hull_middleware[0].method");
    ASSERT_STREQ(m1, "*");
    free(m1);

    char *p1 = eval_str("globalThis.__hull_internals.__hull_middleware[0].pattern");
    ASSERT_STREQ(p1, "/*");
    free(p1);

    char *m2 = eval_str("globalThis.__hull_internals.__hull_middleware[1].method");
    ASSERT_STREQ(m2, "GET");
    free(m2);

    char *p2 = eval_str("globalThis.__hull_internals.__hull_middleware[1].pattern");
    ASSERT_STREQ(p2, "/api/*");
    free(p2);

    cleanup_js();
}

/* ── HMAC-SHA256 tests ───────────────────────────────────────────── */

UTEST(js_cap, crypto_hmac_sha256)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* RFC 4231 Test Case 2: key="Jefe", data="what do ya want for nothing?" */
    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "import { encoding } from 'hull:encoding';\n"
        "globalThis.__test_hmac = encoding.hex.encode(crypto.hmacSha256("
        "  'what do ya want for nothing?', 'Jefe'));\n";

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

#ifdef HL_LINK_TLS
UTEST(js_cap, crypto_sign_and_rsa_private_pem)
{
    /* The EC key and the RSA components are the ones test_asym.c signs with
     * (made by openssl). JS side of crypto.sign / crypto.rsaPrivatePem: an
     * ES256 signature is raw r||s (64 bytes) and verifies; a PEM rebuilt from
     * the RSA components signs RS256 that verifies against the original's
     * public key; the wrong key family throws. */
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "import { encoding } from 'hull:encoding';\n"
        "const ecPriv = '-----BEGIN EC PRIVATE KEY-----\\nMHcCAQEEIL2d49Dj0W0CG8otDWmMNcm96RQqL8ACWBbKQ688SzXdoAoGCCqGSM49\\nAwEHoUQDQgAE68vsW2ypQeT3oQeUWUZpKeFZ5blnRMKJiofxB4tIQiosja/MWXzA\\n6yt/w0rtpatPPAWNKjoXQy+LkoL830ZUuw==\\n-----END EC PRIVATE KEY-----\\n';\n"
        "const ecPub = '-----BEGIN PUBLIC KEY-----\\nMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE68vsW2ypQeT3oQeUWUZpKeFZ5bln\\nRMKJiofxB4tIQiosja/MWXzA6yt/w0rtpatPPAWNKjoXQy+LkoL830ZUuw==\\n-----END PUBLIC KEY-----\\n';\n"
        "const rsaPub = '-----BEGIN PUBLIC KEY-----\\nMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEArXHJgrJxPWh9qZV29faS\\nsiK5aUcAbigfmLy3/7tr29f1yo8Hhd3UqX0N87FDqbQbFCKhQ3V2EfNxqSHt4cRP\\ndSIFHRGQW539veaMgfmNQNU4cGUWXwLV4mZHehU7in06CgI6CO/PrG+biQRxSARN\\nYdH0s0/5D5w2h/5I2I+lQX+S2G3y2GrM4M99LBqYqOSXnQIz/JrPHVefCRXnyMIf\\nN41sHzoH3+Igu3OY6VMTQz6oB7nyKsJXJ7+sbl8QED6ijNJ1aKkkefZL8GpY/4WL\\nMI+swk3X1WXd4ET7VUIjx6g3hlRC89SteRzAwJvYFi88A9rmsg5vyDLDjyDRQwoi\\nawIDAQAB\\n-----END PUBLIC KEY-----\\n';\n"
        "const u8 = (s) => encoding.bytes.toU8(encoding.base64.decode(s));\n"
        "const msg = 'hull js sign';\n"
        "const r = [];\n"
        "const sig = crypto.sign('ES256', ecPriv, msg);\n"
        "r.push(sig.byteLength, crypto.verify('ES256', ecPub, msg, sig));\n"
        "const pem = crypto.rsaPrivatePem(u8('rXHJgrJxPWh9qZV29faSsiK5aUcAbigfmLy3/7tr29f1yo8Hhd3UqX0N87FDqbQbFCKhQ3V2EfNxqSHt4cRPdSIFHRGQW539veaMgfmNQNU4cGUWXwLV4mZHehU7in06CgI6CO/PrG+biQRxSARNYdH0s0/5D5w2h/5I2I+lQX+S2G3y2GrM4M99LBqYqOSXnQIz/JrPHVefCRXnyMIfN41sHzoH3+Igu3OY6VMTQz6oB7nyKsJXJ7+sbl8QED6ijNJ1aKkkefZL8GpY/4WLMI+swk3X1WXd4ET7VUIjx6g3hlRC89SteRzAwJvYFi88A9rmsg5vyDLDjyDRQwoiaw=='), u8('AQAB'), u8('EYvCdMAkLYVTD7ieQAlRfhzTgJTICdzG6YF55Fsrm6IFbELmN212qdi2nxxOedii/qP23WdWFOskPesVRggWeQUtMEDKckI8MkhO7DJ4eYJjGG0ELi4QFhGpEcQbxwmeY/clXim9wAt1rWRE03dqnpvfSH6Cyx359EL4pT55hh5nUDUhD79aeAcomDFLJ4xC6c1nCmogvUSnR7xLm3a6Dgaqmuy9TeGE3IF6X36bz4RYlH1sWDQuVLlihE3k0pCx0I/DeqfSNLHc/d9Ih0oCe83+pPzTcCeqTL36wyCVMvQdj+4HAIx2hzYyuFHqVR9FoQ1kcz2KWaC+Vk4x+suebQ=='), u8('7aLVtTp231IPyFICovo8RQGMV0BOTxt5NltvKAfHNK/ZDTZF74c5ODxlOtRXjq0n+7oJGfoJf9antpKfuD+6S6aBzxU64HbuSeQ5OW0m2aGvUqzw1MImukvtiw6QtsWF0/J3jqrjo+pa2H+DbI1RAYFLCqAWCSZ8Ah5pejT8OH8='), u8('utkME6YpVbCC5GQEsOypMWddgNfFq09K7GLi91cxQ4E4y+sYmgL/GmWLm61/3BrhVn3eA/+prXUHIOerZpk8H9B09SHlxDUfe/DbHAks2+X7gwvkLk21rV/utm0PuEicX9SRdLYpJV8u+s0EulE447AyFkmczNWvD198p/TmgBU='));\n"
        "const rs = crypto.sign('RS256', pem, msg);\n"
        "r.push(rs.byteLength, crypto.verify('RS256', rsaPub, msg, rs));\n"
        "let threw = false;\n"
        "try { crypto.sign('ES256', pem, msg); } catch (e) { threw = true; }\n"
        "r.push(threw);\n"
        "globalThis.__test_sign = r.join(',');\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *got = eval_str("globalThis.__test_sign");
    ASSERT_NE(got, NULL);
    ASSERT_STREQ(got, "64,true,256,true,true");
    free(got);

    cleanup_js_caps();
}
#endif

UTEST(js_cap, crypto_hmac_sha1)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* RFC 2202 Test Case 2 + RFC 6238 TOTP counter=1 reference vector.
     * The HMAC-SHA1 cap dispatches through HlCryptoHmacBackend; this
     * proves the binding routes through the vtable to mbedTLS
     * correctly. Pre-TOTP smoke check before the TOTP module ships.
     *
     * For the TOTP vector, the 8-byte big-endian counter is built
     * via Uint8Array - js_get_buffer's TypedArray branch (fixed in
     * the prior commit) makes that pass cleanly through hmacSha1's
     * data argument as a string. The counter is the literal bytes
     * 00 00 00 00 00 00 00 01 - encoded as Latin-1 string so each
     * char code maps to one byte (TOTP message is short so the
     * UTF-8-vs-bytes pitfall doesn't fire; bytes are all < 0x80
     * except possibly the last position). */
    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "import { encoding } from 'hull:encoding';\n"
        /* Test 1: RFC 2202 case 2 - text input, no binary pitfalls. */
        "globalThis.__test_h1_rfc = encoding.hex.encode(crypto.hmacSha1("
        "  'what do ya want for nothing?', 'Jefe'));\n"
        /* Test 2: RFC 6238 TOTP counter=1, key='12345678901234567890'.
         * Counter built as 8-byte BE string via String.fromCharCode. */
        "const counter = String.fromCharCode(0,0,0,0,0,0,0,1);\n"
        "globalThis.__test_h1_totp = encoding.hex.encode(crypto.hmacSha1("
        "  counter, '12345678901234567890'));\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *rfc = eval_str("globalThis.__test_h1_rfc");
    ASSERT_NE(rfc, NULL);
    ASSERT_STREQ(rfc, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    free(rfc);

    char *totp = eval_str("globalThis.__test_h1_totp");
    ASSERT_NE(totp, NULL);
    ASSERT_STREQ(totp, "75a48a19d4cbe100644e8ac1397eea747a2d33ab");
    free(totp);

    cleanup_js_caps();
}

/* ── hull:qrcode tests ─────────────────────────────────────────────────── */

/* ── hull:archive:tar tests (parse/create marshalling) ──────────────── */

UTEST(js_stdlib, tar_create_parse_roundtrip)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { tar } from 'hull:archive:tar';\n"
        "const ascii = (ab) => String.fromCharCode.apply(null, new Uint8Array(ab));\n"
        "const b = tar.create([\n"
        "  { name: 'greet.txt', data: 'hello', mode: 0o644 },\n"
        "  { name: 'sub', isDir: true, mode: 0o755 },\n"
        "  { name: 'sub/x.bin', data: 'abc' },\n"
        "]);\n"
        "const e = tar.parse(b);\n"
        "globalThis.__tar_ok = (e.length === 3 && e[0].name === 'greet.txt' &&\n"
        "  ascii(e[0].data) === 'hello' && e[0].mode === 0o644 &&\n"
        "  e[1].isDir === true && e[2].name === 'sub/x.bin' &&\n"
        "  e[2].data.byteLength === 3) ? 1 : 0;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__tar_ok"), 1);
    cleanup_js_caps();
}

UTEST(js_stdlib, tar_parse_rejects_truncated)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* A valid archive truncated mid-data throws rather than over-reading. */
    const char *code =
        "import { tar } from 'hull:archive:tar';\n"
        "let threw = 0;\n"
        "const b = tar.create([{ name: 'f', data: 'x'.repeat(1000) }]);\n"
        "try { tar.parse(b.slice(0, 600)); }\n"
        "catch (e) { threw = 1; }\n"
        "globalThis.__tar_threw = threw;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__tar_threw"), 1);
    cleanup_js_caps();
}

UTEST(js_stdlib, tar_create_rejects_unsafe_name)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { tar } from 'hull:archive:tar';\n"
        "let threw = 0;\n"
        "try { tar.create([{ name: '../escape', data: 'x' }]); }\n"
        "catch (e) { threw = 1; }\n"
        "globalThis.__tar_unsafe = threw;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__tar_unsafe"), 1);
    cleanup_js_caps();
}

UTEST(js_stdlib, qrcode_hello_v1)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* "Hello" at EC M fits in v1 (21x21). Pin structural properties +
     * one data-cell. Full matrix cross-verified against Python's
     * `qrcode` library on 48 input/EC/mask combos during development. */
    const char *code =
        "import { qrcode } from 'hull:qrcode';\n"
        "const q = qrcode.encode('Hello', { ecLevel: 'M', mask: 0 });\n"
        "globalThis.__qr_size = q.size;\n"
        "globalThis.__qr_ver  = q.version;\n"
        "globalThis.__qr_mask = q.mask;\n"
        "globalThis.__qr_cell = q.matrix[9][17];\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__qr_size"), 21);
    ASSERT_EQ(eval_int("globalThis.__qr_ver"),   1);
    ASSERT_EQ(eval_int("globalThis.__qr_mask"),  0);
    ASSERT_EQ(eval_int("globalThis.__qr_cell"),  1);

    cleanup_js_caps();
}

UTEST(js_stdlib, qrcode_auto_mask_and_version)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { qrcode } from 'hull:qrcode';\n"
        /* No mask → 8-mask scoring picks best. */
        "globalThis.__qr_auto_mask = qrcode.encode('Hello', { ecLevel: 'M' }).mask;\n"
        /* Auto version → 73-byte URL fits in v5 at EC M. */
        "const url = 'otpauth://totp/Hull:alice@example.com?secret=JBSWY3DPEHPK3PXP&issuer=Hull';\n"
        "globalThis.__qr_url_v = qrcode.encode(url, { ecLevel: 'M' }).version;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__qr_auto_mask"), 2);
    ASSERT_EQ(eval_int("globalThis.__qr_url_v"),     5);

    cleanup_js_caps();
}

UTEST(js_stdlib, qrcode_svg)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { qrcode } from 'hull:qrcode';\n"
        "globalThis.__qr_svg_prefix = qrcode.svg('Hi', { scale: 2 }).substring(0, 4);\n"
        "globalThis.__qr_has_path = qrcode.svg('Hi').includes('<path') ? 1 : 0;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *prefix = eval_str("globalThis.__qr_svg_prefix");
    ASSERT_NE(prefix, NULL);
    ASSERT_STREQ(prefix, "<svg");
    free(prefix);

    ASSERT_EQ(eval_int("globalThis.__qr_has_path"), 1);

    cleanup_js_caps();
}

/* ── hull:web:middleware:totp tests ────────────────────────────────────── */

UTEST(js_stdlib, totp_rfc_vectors)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* RFC 4648 Base32 + RFC 6238 Appendix B vectors. Mirrors the Lua
     * suite so any divergence between runtimes shows up immediately. */
    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "const t = __testHooks('hull:web:middleware:totp');\n"
        "globalThis.__b32 = t.base32Encode('foobar');\n"
        "globalThis.__b32_rt = "
        "  t.base32Decode(t.base32Encode('12345678901234567890')) === "
        "  '12345678901234567890' ? 1 : 0;\n"
        "const s = '12345678901234567890';\n"
        "globalThis.__v1 = t.totpAtStep(s,        1, 8);\n"
        "globalThis.__v2 = t.totpAtStep(s, 37037036, 8);\n"
        "globalThis.__v3 = t.totpAtStep(s, 41152263, 8);\n"
        "globalThis.__v4 = t.totpAtStep(s, 66666666, 8);\n"
        "globalThis.__v1_6 = t.totpAtStep(s, 1, 6);\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *b = eval_str("globalThis.__b32");
    ASSERT_NE(b, NULL); ASSERT_STREQ(b, "MZXW6YTBOI"); free(b);
    ASSERT_EQ(eval_int("globalThis.__b32_rt"), 1);

    char *v1 = eval_str("globalThis.__v1");
    ASSERT_NE(v1, NULL); ASSERT_STREQ(v1, "94287082"); free(v1);
    char *v2 = eval_str("globalThis.__v2");
    ASSERT_NE(v2, NULL); ASSERT_STREQ(v2, "07081804"); free(v2);
    char *v3 = eval_str("globalThis.__v3");
    ASSERT_NE(v3, NULL); ASSERT_STREQ(v3, "89005924"); free(v3);
    char *v4 = eval_str("globalThis.__v4");
    ASSERT_NE(v4, NULL); ASSERT_STREQ(v4, "69279037"); free(v4);
    char *v1_6 = eval_str("globalThis.__v1_6");
    ASSERT_NE(v1_6, NULL); ASSERT_STREQ(v1_6, "287082"); free(v1_6);

    cleanup_js_caps();
}

UTEST(js_stdlib, totp_enroll_confirm_verify)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "function run() {\n"
        "  __testHooks('hull:web:middleware:totp').reset();\n"
        "  totp.init({ issuer: 'TestApp' });\n"
        "  const r = totp.enroll('user-1');\n"
        "  if (typeof r.secretBase32 !== 'string') return 0;\n"
        "  if (!r.qrSvg.includes('<svg')) return 0;\n"
        "  if (r.recoveryCodes.length !== 10) return 0;\n"
        "  if (!r.otpauthUrl.includes('otpauth://totp/TestApp:user-1')) return 0;\n"
        "  const secret = __testHooks('hull:web:middleware:totp').base32Decode(r.secretBase32);\n"
        "  const step = __testHooks('hull:web:middleware:totp').currentStep();\n"
        "  const code = __testHooks('hull:web:middleware:totp').totpAtStep(secret, step, 6);\n"
        "  if (!totp.confirm('user-1', code)) return 0;\n"
        "  if (!totp.enrolled('user-1')) return 0;\n"
        "  if (totp.verify('user-1', code)) return 0;\n"
        "  const nextCode = __testHooks('hull:web:middleware:totp').totpAtStep(secret, step + 1, 6);\n"
        "  const v = totp.verifyWithKind('user-1', nextCode);\n"
        "  if (!v[0] || v[1] !== 'totp') return 0;\n"
        "  if (totp.verify('user-1', nextCode)) return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__totp_flow = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__totp_flow"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, totp_pending_cleanup)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* Round-8 LOW-13: cleanup() prunes orphaned pending rows. */
    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "function run() {\n"
        "  __testHooks('hull:web:middleware:totp').reset();\n"
        "  totp.init({ pendingTtl: 60, cleanup: false, window: 10, "
        "               recoveryCodes: 0 });\n"
        "  const r = totp.enroll('u-fresh');\n"
        "  totp.enroll('u-stale');\n"
        "  __testHooks('hull:web:middleware:totp').forcePendingStale('u-stale');\n"
        "  if (totp.cleanup() !== 1) return 0;\n"
        "  if (totp.cleanup() !== 0) return 0;\n"
        "  const secret = __testHooks('hull:web:middleware:totp').base32Decode(r.secretBase32);\n"
        "  const step = __testHooks('hull:web:middleware:totp').currentStep();\n"
        "  const good = __testHooks('hull:web:middleware:totp').totpAtStep(secret, step, 6);\n"
        "  if (!totp.confirm('u-fresh', good)) return 0;\n"
        "  if (totp.confirm('u-stale', good)) return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__totp_cl = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__totp_cl"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, totp_brute_force_lockout)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* Round-8 HIGH-3: brute-force lockout baked into the module.
     * Mirror of lua_stdlib.totp_brute_force_lockout (wrong codes chosen to
     * differ from every real code near the current step; audit 29). */
    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "function run() {\n"
        "  __testHooks('hull:web:middleware:totp').reset();\n"
        "  totp.init({ maxFailedAttempts: 3, lockoutDuration: 60, "
        "               window: 3, recoveryCodes: 0 });\n"
        "  const r = totp.enroll('u1');\n"
        "  const secret = __testHooks('hull:web:middleware:totp').base32Decode(r.secretBase32);\n"
        "  const step = __testHooks('hull:web:middleware:totp').currentStep();\n"
        "  const at = __testHooks('hull:web:middleware:totp').totpAtStep;\n"
        "  const wrong = (seed) => {\n"
        "    for (let c = seed; c <= 999999; c++) {\n"
        "      const code = String(c).padStart(6, '0');\n"
        "      let hit = false;\n"
        "      for (let s = step - 5; s <= step + 6; s++)\n"
        "        if (at(secret, s, 6) === code) { hit = true; break; }\n"
        "      if (!hit) return code;\n"
        "    }\n"
        "  };\n"
        "  const w0 = wrong(0), w1 = wrong(100), w2 = wrong(200);\n"
        "  if (!totp.confirm('u1', __testHooks('hull:web:middleware:totp').totpAtStep(secret, step, 6))) return 0;\n"
        "  if (totp.verify('u1', w0)) return 0;\n"
        "  if (totp.verify('u1', w1)) return 0;\n"
        "  if (totp.lockoutRemaining('u1') !== 0) return 0;\n"
        "  if (totp.verify('u1', w2)) return 0;\n"
        "  const remain = totp.lockoutRemaining('u1');\n"
        "  if (remain < 1 || remain > 61) return 0;\n"
        "  const good = __testHooks('hull:web:middleware:totp').totpAtStep(secret, step + 2, 6);\n"
        "  if (totp.verify('u1', good)) return 0;\n"
        "  __testHooks('hull:web:middleware:totp').clearFailedAttempts('u1');\n"
        "  if (totp.lockoutRemaining('u1') !== 0) return 0;\n"
        "  if (totp.verify('u1', w0)) return 0;\n"
        "  if (totp.verify('u1', w1)) return 0;\n"
        "  const good2 = __testHooks('hull:web:middleware:totp').totpAtStep(secret, step + 3, 6);\n"
        "  if (!totp.verify('u1', good2)) return 0;\n"
        "  if (totp.verify('u1', w0)) return 0;\n"
        "  if (totp.verify('u1', w1)) return 0;\n"
        "  if (totp.lockoutRemaining('u1') !== 0) return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__totp_bfl = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__totp_bfl"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, totp_recovery_and_disable)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "function run() {\n"
        "  __testHooks('hull:web:middleware:totp').reset();\n"
        "  totp.init({ issuer: 'TestApp', recoveryCodes: 3 });\n"
        "  const r = totp.enroll('user-2');\n"
        "  const secret = __testHooks('hull:web:middleware:totp').base32Decode(r.secretBase32);\n"
        "  const code = __testHooks('hull:web:middleware:totp').totpAtStep(secret, "
        "    __testHooks('hull:web:middleware:totp').currentStep(), 6);\n"
        "  if (!totp.confirm('user-2', code)) return 0;\n"
        "  const rc = r.recoveryCodes[0];\n"
        "  const v1 = totp.verifyWithKind('user-2', rc);\n"
        "  if (!v1[0] || v1[1] !== 'recovery') return 0;\n"
        "  if (totp.verify('user-2', rc)) return 0;\n"
        "  const v2 = totp.verifyWithKind('user-2', r.recoveryCodes[1]);\n"
        "  if (!v2[0] || v2[1] !== 'recovery') return 0;\n"
        "  if (!totp.disable('user-2')) return 0;\n"
        "  if (totp.enrolled('user-2')) return 0;\n"
        "  if (totp.disable('user-2')) return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__totp_rec = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__totp_rec"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, totp_ct_eq_and_normalize)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "const t = __testHooks('hull:web:middleware:totp');\n"
        "function run() {\n"
        "  if (!t.ctEq('287082', '287082')) return 0;\n"
        "  if ( t.ctEq('287082', '287083')) return 0;\n"
        "  if ( t.ctEq('287082', '2870820')) return 0;\n"
        "  if (!t.ctEq('', '')) return 0;\n"
        "  if ( t.ctEq(null, 'x')) return 0;\n"
        "  if ( t.ctEq('x', undefined)) return 0;\n"
        "  if ( t.ctEq(42, 42)) return 0;\n"
        "  if (t.normalizeRecoveryCode('ABCD-EFGH-IJKL') !== 'ABCDEFGHIJKL') return 0;\n"
        "  if (t.normalizeRecoveryCode('abcdefghijkl')   !== 'ABCDEFGHIJKL') return 0;\n"
        "  if (t.normalizeRecoveryCode('  abcd efgh ijkl  ') !== 'ABCDEFGHIJKL') return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__ct = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__ct"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, totp_recovery_accepts_user_typed_forms)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "function run() {\n"
        "  __testHooks('hull:web:middleware:totp').reset();\n"
        "  totp.init({ issuer: 'TestApp', recoveryCodes: 4 });\n"
        "  const r = totp.enroll('rec-user');\n"
        "  const secret = __testHooks('hull:web:middleware:totp').base32Decode(r.secretBase32);\n"
        "  const code = __testHooks('hull:web:middleware:totp').totpAtStep(secret,\n"
        "    __testHooks('hull:web:middleware:totp').currentStep(), 6);\n"
        "  if (!totp.confirm('rec-user', code)) return 0;\n"
        "  const rc = r.recoveryCodes[0];\n"
        "  const plain = rc.replace(/-/g, '');\n"
        "  if (!totp.verify('rec-user', plain)) return 0;\n"
        "  if (!totp.verify('rec-user', r.recoveryCodes[1].toLowerCase())) return 0;\n"
        "  if (!totp.verify('rec-user', '  ' + r.recoveryCodes[2] + '  ')) return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__rec_forms = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__rec_forms"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, totp_encryption_round_trip)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* Encryption end-to-end: enroll with a 32-byte KEK stores an
     * encrypted secret blob, confirm + verify decrypt successfully,
     * and the encrypt/decrypt helpers round-trip. Direct row
     * inspection is blocked by Hull's _hull_* guard, same as Lua. */
    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "function run() {\n"
        "  __testHooks('hull:web:middleware:totp').reset();\n"
        "  totp.init({ issuer: 'TestApp', encryptionKey: 'k'.repeat(32) });\n"
        "  const r = totp.enroll('user-4');\n"
        "  const secret = __testHooks('hull:web:middleware:totp').base32Decode(r.secretBase32);\n"
        "  const code = __testHooks('hull:web:middleware:totp').totpAtStep(secret, "
        "    __testHooks('hull:web:middleware:totp').currentStep(), 6);\n"
        "  if (!totp.confirm('user-4', code)) return 0;\n"
        "  const next = __testHooks('hull:web:middleware:totp').totpAtStep(secret, "
        "    __testHooks('hull:web:middleware:totp').currentStep() + 1, 6);\n"
        "  if (!totp.verify('user-4', next)) return 0;\n"
        "  const enc = __testHooks('hull:web:middleware:totp').encryptSecret(secret);\n"
        "  if (enc[1] !== 1) return 0;\n"
        "  if (enc[0].length <= secret.length) return 0;\n"
        "  const dec = __testHooks('hull:web:middleware:totp').decryptSecret(enc[0], 1);\n"
        "  if (dec[0] !== secret || dec[1] !== 1) return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__totp_enc = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__totp_enc"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, totp_key_rotation_lazy_on_verify_js)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* JS mirror of the Lua rotation test: enroll under v1, init
     * with v1+v2 (current=2), verify → lazy rekey → row on v2. */
    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "function run() {\n"
        "  __testHooks('hull:web:middleware:totp').reset();\n"
        "  const k1 = 'a'.repeat(32);\n"
        "  const k2 = 'b'.repeat(32);\n"
        "  totp.init({ encryptionKeys: {1: k1}, current: 1 });\n"
        "  const r = totp.enroll('u');\n"
        "  const secret = __testHooks('hull:web:middleware:totp').base32Decode(r.secretBase32);\n"
        "  const step = __testHooks('hull:web:middleware:totp').currentStep();\n"
        "  if (!totp.confirm('u', __testHooks('hull:web:middleware:totp').totpAtStep(secret, step, 6))) return 2;\n"
        "  totp.init({ encryptionKeys: {1: k1, 2: k2}, current: 2 });\n"
        "  if (!totp.verify('u', __testHooks('hull:web:middleware:totp').totpAtStep(secret, step + 1, 6))) return 3;\n"
        "  const r2 = totp.rekey();\n"
        "  if (r2.scanned !== 1) return 100 + r2.scanned;\n"
        "  if (r2.rekeyed !== 0) return 200 + r2.rekeyed;\n"
        "  if (r2.failed !== 0) return 300 + r2.failed;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__totp_rot = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__totp_rot"), 1);

    cleanup_js_caps();
}

/* A db.batch inside another is a savepoint (audit 4 C-M2). */
UTEST(js_stdlib, nested_batch_is_a_savepoint)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { db as dbm } from 'hull:db';\n"
        "function run() {\n"
        "  const db = dbm.default();\n"
        "  db.exec('CREATE TABLE nbj (x INTEGER)');\n"
        "  db.batch(() => {\n"
        "    db.exec('INSERT INTO nbj VALUES (1)');\n"
        "    let innerOk = true;\n"
        "    try { db.batch(() => { db.exec('INSERT INTO nbj VALUES (2)'); throw new Error('inner'); }); }\n"
        "    catch (e) { innerOk = false; }\n"
        "    if (innerOk) throw new Error('the inner batch should have failed');\n"
        "    db.batch(() => { db.exec('INSERT INTO nbj VALUES (3)'); });\n"
        "  });\n"
        "  let r = db.query('SELECT x FROM nbj ORDER BY x');\n"
        "  if (r.length !== 2 || r[0].x !== 1 || r[1].x !== 3) return 0;\n"
        "  let outerOk = true;\n"
        "  try { db.batch(() => { db.exec('INSERT INTO nbj VALUES (4)');\n"
        "        db.batch(() => { db.exec('INSERT INTO nbj VALUES (5)'); });\n"
        "        throw new Error('outer'); }); }\n"
        "  catch (e) { outerOk = false; }\n"
        "  if (outerOk) return 0;\n"
        "  r = db.query('SELECT COUNT(*) AS n FROM nbj');\n"
        "  if (r[0].n !== 2) return 0;\n"
        "  db.batch(() => { db.exec('INSERT INTO nbj VALUES (6)'); });\n"
        "  r = db.query('SELECT COUNT(*) AS n FROM nbj');\n"
        "  return r[0].n === 3 ? 1 : 0;\n"
        "}\n"
        "globalThis.__nested_batch = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    ASSERT_EQ(eval_int("globalThis.__nested_batch"), 1);
    cleanup_js_caps();
}

/* db.batch(fn) refuses an async fn / a returned thenable (audit 5 M2): it
 * used to COMMIT as soon as JS_Call returned the Promise, so the statements
 * after the first await ran in autocommit and a rejection became an
 * unhandled one after the COMMIT. Now it rolls back and throws a TypeError. */
UTEST(js_stdlib, batch_refuses_an_async_fn)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { db as dbm } from 'hull:db';\n"
        "function run() {\n"
        "  const db = dbm.default();\n"
        "  db.exec('CREATE TABLE abj (x INTEGER)');\n"
        "  let err = null;\n"
        "  let ran = 0;\n"
        "  try { db.batch(async () => { ran++; db.exec('INSERT INTO abj VALUES (1)'); }); }\n"
        "  catch (e) { err = e; }\n"
        "  if (!(err instanceof TypeError)) return 2;\n"
        "  if (!String(err.message).includes('must be synchronous')) return 3;\n"
        "  if (db.query('SELECT COUNT(*) AS n FROM abj')[0].n !== 0) return 4;\n"
        /* audit 6 L4: refused before it runs, so nothing after an await can
         * run later outside the transaction */
        "  if (ran !== 0) return 8;\n"
        "  err = null;\n"
        "  try { db.batch(async function* () { ran++; }); } catch (e) { err = e; }\n"
        "  if (!(err instanceof TypeError) || ran !== 0) return 9;\n"
        "  err = null;\n"
        "  err = null;\n"
        "  try { db.batch(() => { db.exec('INSERT INTO abj VALUES (2)'); return { then() {} }; }); }\n"
        "  catch (e) { err = e; }\n"
        "  if (!(err instanceof TypeError)) return 5;\n"
        "  if (db.query('SELECT COUNT(*) AS n FROM abj')[0].n !== 0) return 6;\n"
        "  db.batch(() => { db.exec('INSERT INTO abj VALUES (3)'); return 42; });\n"
        "  if (db.query('SELECT COUNT(*) AS n FROM abj')[0].n !== 1) return 7;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__async_batch = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    ASSERT_EQ(eval_int("globalThis.__async_batch"), 1);
    cleanup_js_caps();
}

/* A wait while a registry connection is inside a transaction is refused
 * (audit 5 M1): another request would run on the shared connection while
 * this one is parked. After COMMIT the wait is allowed again. */
UTEST(js_stdlib, a_wait_inside_a_transaction_is_refused)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    js.base.async_ctx = actx;

    const char *code =
        "import { db as dbm } from 'hull:db';\n"
        "function run() {\n"
        "  const db = dbm.default();\n"
        "  db.exec('CREATE TABLE wtj (x INTEGER)');\n"
        "  db.exec('BEGIN');\n"
        "  db.exec('INSERT INTO wtj VALUES (1)');\n"
        "  let msg = '';\n"
        "  try { hull.sleep(5); } catch (e) { msg = String(e && e.message); }\n"
        "  if (!msg.includes('transaction is open') || !msg.includes(\"'default'\")) return 2;\n"
        "  db.exec('COMMIT');\n"
        "  globalThis.__slept = hull.sleep(5).then(() => 1);\n"
        "  if (db.query('SELECT COUNT(*) AS n FROM wtj')[0].n !== 1) return 3;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__wait_txn = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    for (int i = 0; i < 20; i++) {   /* let the allowed sleep finish */
        be->tick(actx, 20);
        hl_js_run_jobs(&js);
    }
    ASSERT_EQ(eval_int("globalThis.__wait_txn"), 1);
    be->tick(actx, 0);
    cleanup_js_caps();
    be->free(actx);
}

UTEST(js_stdlib, totp_rekey_batch_helper_js)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "function run() {\n"
        "  __testHooks('hull:web:middleware:totp').reset();\n"
        "  const k1 = 'a'.repeat(32);\n"
        "  const k2 = 'b'.repeat(32);\n"
        "  totp.init({ encryptionKeys: {1: k1}, current: 1 });\n"
        "  for (let i = 1; i <= 3; i++) {\n"
        "    const r = totp.enroll('u' + i);\n"
        "    const secret = __testHooks('hull:web:middleware:totp').base32Decode(r.secretBase32);\n"
        "    const code = __testHooks('hull:web:middleware:totp').totpAtStep(secret, __testHooks('hull:web:middleware:totp').currentStep(), 6);\n"
        "    if (!totp.confirm('u' + i, code)) return 0;\n"
        "  }\n"
        "  totp.init({ encryptionKeys: {1: k1, 2: k2}, current: 2 });\n"
        "  const r1 = totp.rekey();\n"
        "  if (r1.scanned !== 3 || r1.rekeyed !== 3 || r1.failed !== 0) return 0;\n"
        "  const r2 = totp.rekey();\n"
        "  if (r2.scanned !== 3 || r2.rekeyed !== 0 || r2.failed !== 0) return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__totp_rekey = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__totp_rekey"), 1);

    cleanup_js_caps();
}

/* ── hull:web:auth-flows tests ─────────────────────────────────────────── */

#define AF_INIT_JS \
    "import { authFlows } from 'hull:web:auth-flows';\n" \
    "__testHooks('hull:web:auth-flows').reset();\n" \
    "globalThis._users = {}; globalThis._byId = {}; globalThis._sent = [];\n" \
    "function defaults() { return { " \
    "  stateSecret: 'k'.repeat(32), " \
    "  trustRequestHost: true, " \
    "  emailSend: (to, sub, html, text) => " \
    "    globalThis._sent.push({to, sub, html, text}), " \
    "  templates: { " \
    "    welcome:        c => ({subject:'w', text:'link:' + c.verify_url}), " \
    "    verify:         () => ({subject:'v', text:'x'}), " \
    "    magic_link:     c => ({subject:'m', text:'link:' + c.link}), " \
    "    password_reset: c => ({subject:'p', text:'link:' + c.link}), " \
    "    email_change:   c => ({subject:'e', text:'link:' + c.link}), " \
    "  }, " \
    "  userFindByEmail: e => globalThis._users[e], " \
    "  userGet: id => globalThis._byId[id], " \
    "  userCreate: (e, ph) => { " \
    "    const id = 'u' + (Object.keys(globalThis._byId).length + 1); " \
    "    const u = {id, email: e, password_hash: ph, email_verified: false}; " \
    "    globalThis._users[e] = u; globalThis._byId[id] = u; return id; " \
    "  }, " \
    "  userSetPassword: (id, ph) => globalThis._byId[id].password_hash = ph, " \
    "  userSetEmail: (id, e) => { " \
    "    const u = globalThis._byId[id]; delete globalThis._users[u.email]; " \
    "    u.email = e; globalThis._users[e] = u; " \
    "  }, " \
    "  userSetEmailVerified: (id, v) => globalThis._byId[id].email_verified = v, " \
    "  onLogin: (req, res, user) => res.json({ok: true, id: user.id}), " \
    "}; }\n"

UTEST(js_stdlib, crypto_envelope_round_trip)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { envelope } from 'hull:crypto:envelope';\n"
        "function run() {\n"
        "  const secret = 'aa'.repeat(32);\n"
        "  const tok = envelope.sign({sub:'u1',action:'verify',exp:99}, secret);\n"
        "  if (typeof tok !== 'string' || tok.indexOf('.') < 0) return 0;\n"
        "  const r = envelope.verify(tok, secret);\n"
        "  if (!r[0] || r[1] !== null) return 0;\n"
        "  if (r[0].sub !== 'u1' || r[0].action !== 'verify' || r[0].exp !== 99) return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__env_rt = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__env_rt"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, crypto_envelope_failure_modes)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { envelope } from 'hull:crypto:envelope';\n"
        "function run() {\n"
        "  const secret = 'bb'.repeat(32);\n"
        "  if (envelope.verify('', secret)[1] !== 'missing') return 0;\n"
        "  if (envelope.verify('no-dot', secret)[1] !== 'malformed') return 0;\n"
        "  const tok = envelope.sign({x:1}, secret);\n"
        "  const tampered = tok.substring(0, tok.length - 2) + 'zz';\n"
        "  if (envelope.verify(tampered, secret)[1] !== 'bad tag') return 0;\n"
        "  if (envelope.verify('body.junkhex', secret)[1] !== 'bad tag') return 0;\n"
        "  const wrong = 'cc'.repeat(32);\n"
        "  if (envelope.verify(tok, wrong)[1] !== 'bad tag') return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__env_fm = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__env_fm"), 1);

    cleanup_js_caps();
}

/* hull:crypto:sealbox + encrypted hull:kv. Each run() returns 0 when every
 * check passes, else the number of the first check that failed. */
static int js_run_steps(const char *code, const char *global)
{
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    return eval_int(global);
}

/* hull:encoding's C fast path (hull:encoding:_native) against its pure-JS
 * codecs. The module cannot be loaded without the native import, so the pure
 * path is reached through the shapes the native side declines: a DataView for
 * encoding, and lenient base64 decoding (identical to strict on text with no
 * whitespace). Hex decoding is checked against a regex oracle. What this
 * mostly guards is the byte-string marshalling: characters 0x80..0xFF cross
 * into C as two UTF-8 bytes and must come back as one character. */
UTEST(js_stdlib, encoding_native_matches_pure)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { encoding } from 'hull:encoding';\n"
        "const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/-_=aF09';\n"
        "let seed = 7;\n"
        "function rnd(n) { seed = (seed * 1103515245 + 12345) % 2147483648; return seed % n; }\n"
        "function bytes(n) { let s = ''; for (let i = 0; i < n; i++) s += String.fromCharCode(rnd(256)); return s; }\n"
        "function text(n) { let s = ''; for (let i = 0; i < n; i++) s += A[rnd(A.length)]; return s; }\n"
        "function view(s) { return new DataView(encoding.bytes.toU8(s).buffer); }\n"
        "function run() {\n"
        "  let accepted = 0;\n"
        "  for (let i = 0; i < 4000; i++) {\n"
        "    const b = bytes(rnd(24));\n"
        /* 1-3: encoders, native vs pure, every option */
        "    if (encoding.hex.encode(b) !== encoding.hex.encode(view(b))) return 1;\n"
        "    if (encoding.hex.encode(encoding.bytes.toU8(b)) !== encoding.hex.encode(view(b))) return 1;\n"
        "    for (const o of [undefined, { url: true }, { pad: false }, { url: true, pad: true }]) {\n"
        "      if (encoding.base64.encode(b, o) !== encoding.base64.encode(view(b), o)) return 2;\n"
        "    }\n"
        "    if (encoding.hex.decode(encoding.hex.encode(b).toUpperCase()) !== b) return 3;\n"
        /* 4-5: decoders, native vs pure, on random text */
        "    let t = text(rnd(24));\n"
        "    if (rnd(4) === 0) t += '='.repeat(rnd(3));\n"
        "    for (const url of [false, true]) {\n"
        "      const fast = encoding.base64.decode(t, { url });\n"
        "      if (fast !== encoding.base64.decode(t, { url, lenient: true })) return 4;\n"
        "      if (fast !== null) accepted++;\n"
        "    }\n"
        "    const want = /^([0-9a-fA-F]{2})*$/.test(t);\n"
        "    if ((encoding.hex.decode(t) !== null) !== want) return 5;\n"
        "  }\n"
        "  if (accepted < 200) return 6;\n"
        /* 7-8: what native declines still gets the usual answer */
        "  try { encoding.hex.encode('\\u0100'); return 7; } catch (e) {}\n"
        "  if (encoding.hex.decode('\\u00e9\\u00e9') !== null) return 8;\n"
        /* 9: a wider typed array is its raw bytes */
        "  if (encoding.hex.encode(new Uint16Array([0x0201])) !== '0102') return 9;\n"
        /* 10: a value of a few MB round trips (the pure codecs are far too slow) */
        "  const big = encoding.bytes.fromBuffer(new Uint8Array(4 * 1024 * 1024).fill(0xfa));\n"
        "  if (encoding.base64.decode(encoding.base64.encode(big)) !== big) return 10;\n"
        "  if (encoding.hex.decode(encoding.hex.encode(big)) !== big) return 10;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__encnative = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__encnative"), 0);
    cleanup_js_caps();
}

/* crypto.randomToken: the shape of each format, the bounds, and that two
 * calls differ. run() returns 0, or the number of the first check that failed. */
UTEST(js_stdlib, crypto_random_token)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "function run() {\n"
        "  const t = crypto.randomToken(16);\n"
        "  if (t.length !== 22 || /[^A-Za-z0-9_-]/.test(t)) return 1;\n"
        "  if (crypto.randomToken(16) === t) return 2;\n"
        "  const h = crypto.randomToken(32, 'hex');\n"
        "  if (h.length !== 64 || /[^0-9a-f]/.test(h)) return 3;\n"
        "  if (crypto.randomToken(1).length !== 2 || crypto.randomToken(1024).length !== 1366) return 4;\n"
        "  if (!threw(() => crypto.randomToken(0))) return 5;\n"
        "  if (!threw(() => crypto.randomToken(1025))) return 6;\n"
        "  if (!threw(() => crypto.randomToken(16, 'base32'))) return 7;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__token = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__token"), 0);
    cleanup_js_caps();
}

/* HKDF-SHA256 against RFC 5869 appendix A, test cases 1-3, through the real
 * crypto.hmacSha256. Byte strings and buffers are both accepted as input. */
UTEST(js_stdlib, hkdf_rfc5869_vectors)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { hkdf } from 'hull:crypto:hkdf';\n"
        "import { encoding } from 'hull:encoding';\n"
        "const hex = (b) => encoding.hex.encode(b);\n"
        "const range = (a, b) => { let s = ''; for (let i = a; i <= b; i++) s += String.fromCharCode(i); return s; };\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "function run() {\n"
        "  const ikm = '\\x0b'.repeat(22);\n"
        "  if (hex(hkdf.extract(range(0, 12), ikm)) !== '077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5') return 1;\n"
        "  if (hex(hkdf.derive(ikm, 42, { salt: range(0, 12), info: range(0xf0, 0xf9) }))\n"
        "      !== '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865') return 2;\n"
        "  if (hex(hkdf.derive(encoding.bytes.toU8(range(0, 0x4f)), 82,\n"
        "          { salt: range(0x60, 0xaf), info: range(0xb0, 0xff) }))\n"
        "      !== 'b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87') return 3;\n"
        "  if (hex(hkdf.derive(ikm, 42)) !== '8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8') return 4;\n"
        "  if (!threw(() => hkdf.derive(ikm, 0)) || !threw(() => hkdf.derive(ikm, 8161))) return 5;\n"
        "  if (hkdf.derive(ikm, 8160).byteLength !== 8160) return 6;\n"
        "  if (hex(hkdf.derive(ikm, 32, { info: 'enc' })) === hex(hkdf.derive(ikm, 32, { info: 'mac' }))) return 7;\n"
        "  if (!threw(() => hkdf.derive(42, 32))) return 8;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__hkdf = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__hkdf"), 0);
    cleanup_js_caps();
}

/* encoding.*.why gives the reason Lua's decoder returns second, and null
 * exactly when decode succeeds. (A generated corpus of 9000 inputs was also
 * compared value-for-value and reason-for-reason against the Lua module.) */
UTEST(js_stdlib, encoding_why_reasons)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { encoding } from 'hull:encoding';\n"
        "function run() {\n"
        "  const e = encoding;\n"
        "  if (e.hex.why('abc') !== 'bad_length') return 1;\n"
        "  if (e.hex.why('zz') !== 'invalid_char') return 2;\n"
        "  if (e.hex.why('00ff') !== null || e.hex.decode('00ff') !== '\\x00\\xff') return 3;\n"
        "  if (e.base64.why('ab!=') !== 'invalid_char') return 4;\n"
        "  if (e.base64.why('a') !== 'bad_length') return 5;\n"
        "  if (e.base64.why('ab=c') !== 'bad_padding') return 6;\n"
        "  if (e.base64.why('ab=') !== 'bad_padding') return 7;\n"
        "  if (e.base64.why('QR==') !== 'non_canonical') return 8;\n"
        "  if (e.base64.why('QQ==') !== null || e.base64.decode('QQ==') !== 'A') return 9;\n"
        "  if (e.base64.why('QQ', { url: true }) !== null) return 10;\n"
        "  if (e.base64.why('QQ==', { url: true }) !== 'bad_padding') return 11;\n"
        "  if (e.base64.why('Q Q==', { lenient: true }) !== null) return 12;\n"
        "  if (e.base32.why('MZ') !== 'non_canonical') return 13;\n"
        "  if (e.base32.why('M') !== 'bad_length') return 14;\n"
        "  if (e.base32.why('M1') !== 'invalid_char') return 15;\n"
        "  if (e.utf8.why('\\xc3') !== 'invalid_utf8' || e.utf8.why('\\xc3\\xa9') !== null) return 16;\n"
        "  for (const s of ['', 'QQ==', 'QR==', 'a', '!!', 'Zm9v']) {\n"
        "    if ((e.base64.decode(s) === null) !== (e.base64.why(s) !== null)) return 17;\n"
        "  }\n"
        "  let threw = false; try { e.base64.why(42); } catch (x) { threw = true; }\n"
        "  if (!threw) return 18;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__why = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__why"), 0);
    cleanup_js_caps();
}

/* The JS twin of lua_stdlib.crypto_key_from_env: crypto.keyFromEnv end to
 * end through sealbox, kv and the env allowlist. */
UTEST(js_stdlib, crypto_key_from_env)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    setenv("HULL_TEST_VAR", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 1);
    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "import { sealbox } from 'hull:crypto:sealbox';\n"
        "import { kv } from 'hull:kv';\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "function run() {\n"
        "  let raw = ''; for (let i = 0; i < 32; i++) raw += String.fromCharCode(i);\n"
        "  const k = crypto.keyFromEnv('HULL_TEST_VAR');\n"
        "  if (String(k) !== 'crypto.key(HULL_TEST_VAR)') return 1;\n"
        "  if (Object.keys(k).length !== 0) return 2;\n"
        "  const rh = sealbox.keyring({ keys: { 1: k }, current: 1 });\n"
        "  const rr = sealbox.keyring({ keys: { 1: raw }, current: 1 });\n"
        "  if (sealbox.open(rr, sealbox.seal(rh, 'v', ['ctx']), ['ctx'])[0] !== 'v') return 3;\n"
        "  if (sealbox.open(rh, sealbox.seal(rr, 'w', ['ctx']), ['ctx'])[0] !== 'w') return 4;\n"
        "  const re = sealbox.keyringFromEnv({ keys: { 1: 'HULL_TEST_VAR' }, current: 1 });\n"
        "  if (sealbox.open(re, sealbox.seal(rr, 'x'))[0] !== 'x') return 5;\n"
        "  const h = kv.open({ namespace: 'held', encrypt: { keys: { 1: k }, current: 1 } });\n"
        "  h.set('a', 'secret');\n"
        "  if (h.get('a') !== 'secret') return 6;\n"
        "  if (!threw(() => crypto.keyFromEnv('PATH'))) return 7;\n"
        "  const k2 = crypto.keyFromEnv('HULL_TEST_VAR');\n"
        "  k2.destroy();\n"
        "  if (!threw(() => k2.secretbox('x', new Uint8Array(24)))) return 8;\n"
        "  if (!String(k2).includes('destroyed')) return 9;\n"
        "  if (!threw(() => k.secretbox('x', new Uint8Array(5)))) return 10;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__heldkey = run();\n";
    int rc = js_run_steps(code, "globalThis.__heldkey");
    unsetenv("HULL_TEST_VAR");
    ASSERT_EQ(rc, 0);
    cleanup_js_caps();
}

/* Binding lifetimes: app code a binding runs while converting its arguments
 * (valueOf, toString, a getter, a prototype setter) must not free what the
 * binding already resolved. Each case crashed or wrote freed memory before;
 * run() returns 0, or the number of the first check that failed. */
/* A stdlib helper that calls a function the app handed it does not lend that
 * function its stdlib identity, whether the app passes conn.exec itself (its
 * `this` is then not a connection) or a copy bound to the connection (QuickJS
 * gives a bound call its own frame - HULL PATCH 0002). The dialect helpers check
 * every identifier, case-insensitively. */
UTEST(js_cap, stdlib_helpers_do_not_lend_their_identity)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { db as dbMod } from 'hull:db';\n"
        "import { retry } from 'hull:retry';\n"
        "const db = dbMod.default();\n"
        "const sql = () => 'CREATE TABLE _hull_probe (x)';\n"
        "function count() {\n"
        "  return db.query(\"SELECT count(*) AS n FROM sqlite_master \" +\n"
        "                  \"WHERE name = '_' || 'hull_probe'\")[0].n;\n"
        "}\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "async function run() {\n"
        "  for (const retryOn of [db.exec, db.exec.bind(db)]) {\n"
        "    let rejected = false;\n"
        "    try { await retry.run(sql, { maxAttempts: 1, retryOn }); }\n"
        "    catch (e) { rejected = true; }\n"
        "    if (!rejected) return 1;\n"
        "    if (count() !== 0) return 2;\n"
        "  }\n"
        "  db.exec('CREATE TABLE life (id INTEGER PRIMARY KEY, v TEXT)');\n"
        "  if (!threw(() => db.upsert('_HULL_sessions', ['id'], ['id'], [1]))) return 3;\n"
        "  if (!threw(() => db.upsert('life', ['id'], ['id', '_hull_x'], [1, 2]))) return 4;\n"
        "  if (threw(() => db.insertIfAbsent('life', ['id'], ['id'], [1]))) return 5;\n"
        "  if (!threw(() => db.tableColumns('main._hull_sessions'))) return 6;\n"
        "  return 0;\n"
        "}\n"
        "run().then(v => { globalThis.__lend = v; }, () => { globalThis.__lend = 99; });\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__lend"), 0);
    cleanup_js_caps();
}

/* retry.run awaits its predicates (audit 5): an async retryOn returned a
 * truthy Promise, so every success was repeated maxAttempts times, and an
 * async retryOnError made a non-retryable error retryable. */
UTEST(js_cap, retry_awaits_async_predicates)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { retry } from 'hull:retry';\n"
        "async function run() {\n"
        "  let calls = 0;\n"
        "  const v = await retry.run(() => { calls++; return 7; },\n"
        "    { maxAttempts: 3, retryOn: async (r) => r !== 7 });\n"
        "  if (v !== 7 || calls !== 1) return 1;\n"
        "  calls = 0;\n"
        "  let threw = false;\n"
        "  try {\n"
        "    await retry.run(() => { calls++; throw new Error('x'); },\n"
        "      { maxAttempts: 3, retryOnError: async () => false });\n"
        "  } catch (e) { threw = true; }\n"
        "  if (!threw || calls !== 1) return 2;\n"
        "  return 0;\n"
        "}\n"
        "run().then(v => { globalThis.__retry = v; }, () => { globalThis.__retry = 99; });\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__retry"), 0);
    cleanup_js_caps();
}

/* pagination.render coerces a string page (audit 5): "5" + 2 was "52", so
 * the window loop ran to ~page*10 and the page link was duplicated. */
UTEST(js_cap, pagination_render_coerces_string_page)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { pagination } from 'hull:web:pagination';\n"
        "function run() {\n"
        "  const r = pagination.render(100, { page: '5', per_page: 10 });\n"
        "  if (r.page !== 5) return 1;\n"
        "  const nums = r.links.filter(l => !l.ellipsis).map(l => l.page);\n"
        "  if (nums.join(',') !== '1,3,4,5,6,7,10') return 2;\n"
        "  const big = pagination.render(10, { page: '1', per_page: '0', window: 1e9 });\n"
        "  if (big.pages !== 10 || big.links.length !== 10) return 3;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__pg = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__pg"), 0);
    cleanup_js_caps();
}

/* No route back to the Function constructors: deleting the global left
 * (() => 0).constructor - and the async / generator ones - compiling strings
 * into code. Their names survive, for the usual "is this async?" test. */
UTEST(js_cap, function_constructors_are_unreachable)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "function threw(f) { try { f(); return false; } catch (e) { return e instanceof TypeError; } }\n"
        "function run() {\n"
        "  const fns = [() => 0, async () => 0, function* () {}, async function* () {}];\n"
        "  for (const f of fns) {\n"
        "    if (!threw(() => f.constructor('globalThis.__escaped = 1'))) return 1;\n"
        "    if (!threw(() => new f.constructor('return 1'))) return 2;\n"
        "    if (Object.getPrototypeOf(f).constructor !== f.constructor) return 3;\n"
        "    try { Object.getPrototypeOf(f).constructor = null; } catch (e) {}\n"
        "    if (typeof f.constructor !== 'function') return 4;\n"
        "  }\n"
        "  if (globalThis.__escaped) return 5;\n"
        "  if ((async () => 0).constructor.name !== 'AsyncFunction') return 6;\n"
        "  if ((() => 0).constructor.name !== 'Function') return 7;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__fnctor = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__fnctor"), 0);
    cleanup_js_caps();
}

/* Stdlib fixes from the second audit (D2): jwt verify options and strict
 * splitting; the safe_url filter. run() returns 0, or the number of the first
 * check that failed. */
UTEST(js_stdlib, audit2_stdlib_fixes)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { jwt } from 'hull:jwt';\n"
        "import { template } from 'hull:template';\n"
        "import { time } from 'hull:time';\n"
        "function run() {\n"
        "  const now = time.now();\n"
        "  const k = 'k'.repeat(32);\n"
        "  const t = jwt.sign({ sub: 'u', iss: 'me', aud: ['api', 'x'], exp: now + 60 }, k);\n"
        "  if (!jwt.verify(t, k, { iss: 'me', aud: 'api' })[0]) return 1;\n"
        "  if (jwt.verify(t, k, { iss: 'other' })[0]) return 2;\n"
        "  if (jwt.verify(t, k, { aud: ['nope', 'no'] })[0]) return 3;\n"
        "  const p = t.split('.');\n"
        "  if (jwt.verify(p[0] + '.' + p[1] + '..' + p[2], k)[0]) return 4;\n"
        "  const old = jwt.sign({ sub: 'u', exp: now - 5 }, k);\n"
        "  if (jwt.verify(old, k)[0]) return 5;\n"
        "  if (!jwt.verify(old, k, { leeway: 30 })[0]) return 6;\n"
        "  if (jwt.verify(jwt.sign({ sub: 'u', exp: now + 60 }, k), k, { aud: 'api' })[0]) return 7;\n"
        "  if (template.renderString('{{ u | safe_url }}', { u: 'java\\tscript:alert(1)' }) !== '#') return 8;\n"
        "  if (template.renderString('{{ u | safe_url }}', { u: 'DATA:text/html,x' }) !== '#') return 9;\n"
        "  if (template.renderString('{{ u | safe_url }}', { u: '/a?b=1' }) !== '/a?b=1') return 10;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__audit2 = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__audit2"), 0);
    cleanup_js_caps();
}

/* Template loop variables named after the globals the generated code calls
 * (audit 5): `{% for Array in xs %}` hit the TDZ in its own header, and a
 * variable named String or Object broke the loop body. */
UTEST(js_stdlib, template_loop_vars_do_not_shadow_codegen_globals)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { template } from 'hull:template';\n"
        "function run() {\n"
        "  const d = { xs: ['a', 'b'], m: { k: 'v' } };\n"
        "  if (template.renderString('{% for Array in xs %}{{ Array }}{% endfor %}', d) !== 'ab') return 1;\n"
        "  if (template.renderString('{% for String in xs %}{{ String | raw }}{% endfor %}', d) !== 'ab') return 2;\n"
        "  if (template.renderString('{% for Object, v in m %}{{ Object }}={{ v }}{% endfor %}', d) !== 'k=v') return 3;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__tplshadow = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__tplshadow"), 0);
    cleanup_js_caps();
}

/* The third audit's stdlib fixes, as in lua_stdlib.audit3_stdlib_fixes, plus
 * the template json filter on a missing value (it threw, failing the render). */
UTEST(js_stdlib, audit3_stdlib_fixes)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { session } from 'hull:web:middleware:session';\n"
        "import { auditLog } from 'hull:web:middleware:audit-log';\n"
        "import { inbox } from 'hull:web:middleware:inbox';\n"
        "import { ratelimit } from 'hull:web:middleware:ratelimit';\n"
        "import { idempotency } from 'hull:web:middleware:idempotency';\n"
        "import { jwt } from 'hull:jwt';\n"
        "import { template } from 'hull:template';\n"
        "import { time } from 'hull:time';\n"
        "function run() {\n"
        "  session.init({ ttl: 3600 });\n"
        "  const sid = session.create({ user_id: 42 });\n"
        "  if (session.listForUser(42).length !== 1) return 1;\n"
        "  if (session.destroyAll(42) !== 1) return 2;\n"
        "  if (session.load(sid) !== null) return 3;\n"
        "  if (session.destroyAll(4.5) !== 0 || session.destroyAll('') !== 0) return 4;\n"
        "  auditLog.init({ fingerprintSalt: 'test-salt-123' });\n"
        "  auditLog.record(7, 'login', { headers: { 'user-agent': 'curl/8' }, remoteAddr: '10.0.0.1' });\n"
        "  if (auditLog.list(7).length !== 1 || auditLog.list('7').length !== 1) return 5;\n"
        "  inbox.init();\n"
        "  if (inbox.checkAndMark('m1', 'src') !== false) return 6;\n"
        "  if (inbox.checkAndMark('m1', 'src') !== true) return 7;\n"
        "  try { inbox.checkAndMark('m2', 'src', { ttl: -1 }); return 8; } catch (_e) { }\n"
        "  if (inbox.checkAndMark('m2', 'src') !== false) return 8;\n"
        "  __testHooks('hull:web:middleware:inbox').expire('m2', 'src');\n"
        "  if (inbox.checkAndMark('m2', 'src') !== false) return 9;\n"
        "  const quiet = { header() { return this; }, status() { return this; }, json() { return this; } };\n"
        "  const shared = (a, b) => {\n"
        "    const m = ratelimit.middleware({ limit: 1 });\n"
        "    m({ headers: {}, remote_addr: a }, quiet);\n"
        "    return m({ headers: {}, remote_addr: b }, quiet) === 1;\n"
        "  };\n"
        "  if (!shared('2001:db8:0:1:aaaa::1', '2001:DB8:0000:0001:1:2:3:4')) return 10;\n"
        "  if (shared('2001:db8:0:1::1', '2001:db8:0:2::1')) return 11;\n"
        "  if (!shared('::ffff:192.0.2.7', '192.0.2.7')) return 12;\n"
        "  if (shared('192.0.2.7', '192.0.2.8')) return 13;\n"
        "  if (!shared('fe80::1%eth0', 'fe80::2')) return 14;\n"
        "  const b = new Map();\n"
        "  const cache = { get: (k) => b.get(k), set: (k, v) => { b.set(k, v); if (b.size > 2) b.delete(b.keys().next().value); } };\n"
        "  const sat = new Map();\n"
        "  ratelimit.check(cache, 'a', 1, 60, 100, sat);\n"
        "  if (ratelimit.check(cache, 'a', 1, 60, 100, sat).allowed) return 17;\n"
        "  for (const k of ['b', 'c', 'd']) ratelimit.check(cache, k, 1, 60, 100, sat);\n"
        "  if (b.has('a')) return 99;\n"
        "  if (ratelimit.check(cache, 'a', 1, 60, 101, sat).allowed) return 18;\n"
        "  if (!ratelimit.check(cache, 'a', 1, 60, 200, sat).allowed) return 19;\n"
        "  idempotency.init();\n"
        "  const mw = idempotency.middleware();\n"
        "  const res = () => ({ status(c) { this.code = c; return this; },\n"
        "                       json(d) { this.err = d.error; return this; },\n"
        "                       header() { return this; } });\n"
        "  const rq = (to) => ({ method: 'POST', path: '/transfer', query: { to },\n"
        "                        body: 'amount=5', ctx: {}, headers: {},\n"
        "                        header: (n) => n === 'idempotency-key' ? 'k1' : undefined });\n"
        "  if (mw(rq('alice'), res()) !== 0) return 20;\n"
        "  const r2 = res();\n"
        "  if (mw(rq('bob'), r2) !== 1 || r2.code !== 409) return 21;\n"
        "  if (String(r2.err).indexOf('different request') < 0) return 22;\n"
        "  const pem = '-----BEGIN PUBLIC KEY-----\\nMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE\\n-----END PUBLIC KEY-----\\n';\n"
        "  const forged = jwt.sign({ sub: 'admin', exp: time.now() + 60 }, pem);\n"
        "  if (jwt.verify(forged, pem, { algs: ['HS256', 'RS256'] })[0]) return 23;\n"
        "  if (template.renderString('[{{ x | json }}]', {}) !== '[null]') return 24;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__audit3 = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__audit3"), 0);
    cleanup_js_caps();
}

/* The fourth audit's stdlib fixes, as in lua_stdlib.audit4_stdlib_fixes,
 * plus JS-only ones: user id 0, prototype names as template filters. */
UTEST(js_stdlib, audit4_stdlib_fixes)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { totp } from 'hull:web:middleware:totp';\n"
        "import { session } from 'hull:web:middleware:session';\n"
        "import { rbac } from 'hull:web:middleware:rbac';\n"
        "import { search } from 'hull:search';\n"
        "import { ratelimit } from 'hull:web:middleware:ratelimit';\n"
        "import { inbox } from 'hull:web:middleware:inbox';\n"
        "import { sort } from 'hull:web:htmx:sort';\n"
        "import { auditLog } from 'hull:web:middleware:audit-log';\n"
        "import { template } from 'hull:template';\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "function run() {\n"
        "  totp.init({ issuer: 'T', encryptionKey: 'k'.repeat(32) });\n"
        "  if (threw(() => totp.enroll(42))) return 1;\n"
        "  if (totp.enrolled(42) !== totp.enrolled('42')) return 2;\n"
        "  if (totp.disable(42) !== true) return 3;\n"
        "  session.init({ ttl: 3600 });\n"
        "  const sid = session.create({});\n"
        "  session.update(sid, { user_id: 42 });\n"
        "  if (session.listForUser(42).length !== 1) return 4;\n"
        "  rbac.init(); rbac.defineRole('admin');\n"
        "  rbac.assign(7, 'admin');\n"
        "  if (!rbac.hasRole('7', 'admin')) return 5;\n"
        "  if (!threw(() => rbac.assign(7.5, 'admin'))) return 6;\n"
        "  search.createIndex('docs', ['body']);\n"
        "  if (!threw(() => search.reindex('docs', '_HULL_SESSIONS', { columns: { body: 'data' } }))) return 7;\n"
        "  const r = search.query('docs', 'foo AND');\n"
        "  if (r.length !== 0 || !r.error) return 8;\n"
        "  if (!threw(() => search.query('docs', 'w '.repeat(70)))) return 9;\n"
        "  const b = new Map();\n"
        "  const cache = { get: (k) => b.get(k), set: (k, v) => { b.set(k, v); if (b.size > 2) b.delete(b.keys().next().value); } };\n"
        "  const sat = new Map();\n"
        "  ratelimit.check(cache, 'a', 1, 60, 100, sat); ratelimit.check(cache, 'a', 1, 60, 100, sat);\n"
        "  ratelimit.check(cache, 'a', 1, 60, 200, sat); ratelimit.check(cache, 'a', 1, 60, 200, sat);\n"
        "  for (const k of ['b', 'c', 'd']) ratelimit.check(cache, k, 1, 60, 200, sat);\n"
        "  if (ratelimit.check(cache, 'a', 1, 60, 201, sat).allowed) return 10;\n"
        "  inbox.init();\n"
        "  if (inbox.checkAndMark(12345, 'w') !== false) return 11;\n"
        "  if (inbox.checkAndMark(12345, 'w') !== true) return 12;\n"
        "  const h = sort.headerAttrs('name', { column: 'name', direction: 'x\" onmouseover=\"y' }, { url: '/t' });\n"
        "  if (h.indexOf('onmouseover') >= 0) return 15;\n"
        "  auditLog.init({ fingerprintSalt: 'test-salt-123' });\n"
        "  const fp = (ip) => auditLog.fingerprint({ headers: { 'user-agent': 'curl/8' }, remote_addr: ip });\n"
        "  if (fp('2001:db8::1') !== fp('2001:db8::2')) return 16;\n"
        "  if (fp('::ffff:10.1.2.3') !== fp('10.1.2.99')) return 18;\n"
        "  if (!threw(() => template.renderString('{{ x | constructor }}', { x: 1 }))) return 20;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__audit4 = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__audit4"), 0);
    cleanup_js_caps();
}

UTEST(js_cap, conversions_cannot_free_resolved_objects)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    setenv("HULL_TEST_VAR", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 1);
    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "import { db as dbMod } from 'hull:db';\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "function run() {\n"
        /* crypto: a message object whose toString destroys the key is not a
         * buffer - it is refused, and never stringified */
        "  const k = crypto.keyFromEnv('HULL_TEST_VAR');\n"
        "  let ran = false;\n"
        "  const evil = { toString() { ran = true; k.destroy(); return 'x'; } };\n"
        "  if (!threw(() => k.secretbox(evil, new Uint8Array(24)))) return 2;\n"
        "  if (ran) return 3;\n"
        "  if (k.secretbox('x', new Uint8Array(24)).byteLength !== 17) return 4;\n"
        /* db: rows are defined, so an Object.prototype setter never runs
         * inside the statement's row loop */
        "  const db = dbMod.default();\n"
        "  db.exec('CREATE TABLE life (name TEXT)');\n"
        "  db.exec('INSERT INTO life (name) VALUES (?)', ['alice']);\n"
        "  let setter = 0;\n"
        "  Object.defineProperty(Object.prototype, 'name',\n"
        "      { set(v) { setter++; }, configurable: true });\n"
        "  Object.defineProperty(Array.prototype, '0',\n"
        "      { set(v) { setter++; }, configurable: true });\n"
        "  let rows;\n"
        "  try { rows = db.query('SELECT name FROM life'); }\n"
        "  finally { delete Object.prototype.name; delete Array.prototype[0]; }\n"
        "  if (setter !== 0) return 5;\n"
        "  if (rows.length !== 1 || rows[0].name !== 'alice') return 6;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__life = run();\n";
    int rc = js_run_steps(code, "globalThis.__life");
    unsetenv("HULL_TEST_VAR");
    ASSERT_EQ(rc, 0);
    cleanup_js_caps();
}

/* Regressions from docs/crypto_encoding_ssh_audit.md (PR 1). run() returns 0,
 * or the number of the first check that failed. */
UTEST(js_stdlib, crypto_encoding_audit_fixes)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "import { kv } from 'hull:kv';\n"
        "import { cache } from 'hull:cache';\n"
        "import { sealbox } from 'hull:crypto:sealbox';\n"
        "import { otp } from 'hull:crypto:otp';\n"
        "import { encoding } from 'hull:encoding';\n"
        "function code(f) { try { f(); return null; } catch (e) { return e.code || e.message; } }\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "const K1 = 'a'.repeat(32), K2 = 'b'.repeat(32);\n"
        "function run() {\n"
        /* 1-3: crypto takes buffers, and a buffer is not the text '[object ArrayBuffer]' */
        "  const key = '6b6579';\n"
        "  const text = 'The quick brown fox';\n"
        "  const u8 = encoding.bytes.toU8(text);\n"
        "  const hm = (d) => encoding.hex.encode(crypto.hmacSha256(d, key));\n"
        "  if (hm(u8.buffer) !== hm(text)) return 1;\n"
        "  if (hm(u8.buffer) === hm('[object ArrayBuffer]')) return 2;\n"
        "  if (!crypto.constantTimeEq(u8, text) || crypto.constantTimeEq(u8, 'x')) return 3;\n"
        /* 4-5: a string `current` is normalised, so rekey reaches 0 */
        "  const h = kv.open({ namespace: 'cur', encrypt: { keys: { 1: K1, 2: K2 }, current: '2' } });\n"
        "  kv.open({ namespace: 'cur', encrypt: { keys: { 1: K1 }, current: 1 } }).set('k', 'v');\n"
        "  if (h.rekey() !== 1) return 4;\n"
        "  if (h.rekey() !== 0) return 5;\n"
        /* 6-7: rekey keeps each value's expiry */
        "  const h1 = kv.open({ namespace: 'ttl', encrypt: { keys: { 1: K1 }, current: 1 } });\n"
        "  h1.set('t', 'v', { ttl: 3600 });\n"
        "  const before = h1._store.data.get('t').exp;\n"
        "  const h2 = kv.open({ namespace: 'ttl', encrypt: { keys: { 1: K1, 2: K2 }, current: 2 } });\n"
        "  if (!before || h2.rekey() !== 1) return 6;\n"
        "  if (h1._store.data.get('t').exp !== before) return 7;\n"
        /* 8-9: key ids in canonical decimal only */
        "  if (!threw(() => sealbox.keyring({ keys: { ' 1': K1 }, current: 1 }))) return 8;\n"
        "  if (!threw(() => sealbox.keyring({ keys: { '1e0': K1 }, current: 1 }))) return 9;\n"
        /* 10: cache refuses encrypt */
        "  if (code(() => cache.open({ encrypt: { keys: { 1: K1 }, current: 1 } })) !== 'invalid_argument') return 10;\n"
        /* 11-12: sealbox checks its arguments; open of a non-byte string fails cleanly */
        "  const r = sealbox.keyring({ keys: { 1: K1 }, current: 1 });\n"
        "  if (!threw(() => sealbox.seal(r, new Uint8Array([1, 2, 3])))) return 11;\n"
        "  if (sealbox.open(r, '\\u0100'.repeat(64))[1] !== 'open_failed') return 12;\n"
        /* 13: otp.step refuses a zero period */
        "  if (!threw(() => otp.step(60, 0))) return 13;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__audit1 = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__audit1"), 0);
    cleanup_js_caps();
}

UTEST(js_stdlib, otp_rfc4226_vectors)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { otp } from 'hull:crypto:otp';\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "function run() {\n"
        "  const want = ['755224', '287082', '359152', '969429', '338314',\n"
        "                '254676', '287922', '162583', '399871', '520489'];\n"
        "  for (let i = 0; i < want.length; i++)\n"
        "    if (otp.hotp('12345678901234567890', i) !== want[i]) return i + 1;\n"
        "  if (otp.hotp('12345678901234567890', 1, 8) !== '94287082') return 11;\n"
        "  if (!threw(() => otp.hotp('k', -1)) || !threw(() => otp.hotp('k', 1.5))) return 12;\n"
        "  if (!threw(() => otp.hotp('k', 1, 9))) return 13;\n"
        "  if (otp.step(59, 30) !== 1 || otp.step(60, 30) !== 2) return 14;\n"
        "  if (otp.hotp('12345678901234567890', otp.step(20000000000, 30), 8) !== '65353130') return 15;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__otp = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__otp"), 0);
    cleanup_js_caps();
}

UTEST(js_stdlib, sealbox_seal_open)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { sealbox } from 'hull:crypto:sealbox';\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "function run() {\n"
        "  if (!threw(() => sealbox.keyring({ keys: { 1: 'short' }, current: 1 }))) return 1;\n"
        "  if (!threw(() => sealbox.keyring({ keys: { 1: 'a'.repeat(32) }, current: 2 }))) return 2;\n"
        "  const r1 = sealbox.keyring({ keys: { 1: 'a'.repeat(32) }, current: 1 });\n"
        "  const v = '\\x00\\x01\\xfe\\xff secret';\n"
        "  const b = sealbox.seal(r1, v, ['ns', 'key']);\n"
        "  let [ov, oe, over] = sealbox.open(r1, b, ['ns', 'key']);\n"
        "  if (oe !== null || ov !== v || over !== 1) return 3;\n"
        "  if (b.length !== sealbox.MIN_LEN + 4 + 2 + 4 + 3 + v.length) return 4;\n"
        "  const t = b.substring(0, 30) + String.fromCharCode(b.charCodeAt(30) ^ 1) + b.substring(31);\n"
        "  if (sealbox.open(r1, t, ['ns', 'key'])[1] !== 'open_failed') return 5;\n"
        "  if (sealbox.open(r1, b, ['ns', 'other'])[1] !== 'open_failed') return 6;\n"
        "  if (sealbox.open(r1, b, ['n', 'skey'])[1] !== 'open_failed') return 7;\n"
        "  if (sealbox.open(r1, b)[0] === v) return 8;\n"
        "  if (sealbox.open(r1, 'short')[1] !== 'open_failed') return 9;\n"
        "  const r3 = sealbox.keyring({ keys: { 3: 'c'.repeat(32) }, current: 3 });\n"
        "  const [nv, ne] = sealbox.open(r3, b, ['ns', 'key']);\n"
        "  if (nv !== null || ne !== 'unknown_version') return 10;\n"
        "  const r12 = sealbox.keyring({ keys: { 1: 'a'.repeat(32), 2: 'b'.repeat(32) }, current: 2 });\n"
        "  [ov, oe, over] = sealbox.open(r12, b, ['ns', 'key']);\n"
        "  if (oe !== null || over !== 1) return 11;\n"
        "  [ov, oe, over] = sealbox.open(r12, sealbox.seal(r12, v), undefined);\n"
        "  if (oe !== null || ov !== v || over !== 2) return 12;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__sealbox = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__sealbox"), 0);
    cleanup_js_caps();
}

UTEST(js_stdlib, kv_encrypted_handle)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { kv } from 'hull:kv';\n"
        "function code(f) { try { f(); return null; } catch (e) { return e.code; } }\n"
        "const K1 = 'a'.repeat(32), K2 = 'b'.repeat(32);\n"
        "function run() {\n"
        "  const enc = kv.open({ namespace: 'sec', encrypt: { keys: { 1: K1 }, current: 1 } });\n"
        "  const raw = kv.open({ namespace: 'sec' });\n"
        "  const v = '\\x00\\xff pass';\n"
        "  enc.set('k', v);\n"
        "  if (enc.get('k') !== v) return 1;\n"
        "  if (enc.get('miss') !== null) return 2;\n"
        "  const stored = raw.get('k');\n"
        "  if (stored === v || stored.indexOf('pass') >= 0) return 3;\n"
        "  raw.set('moved', stored);\n"
        "  if (code(() => enc.get('moved')) !== 'decrypt_failed') return 4;\n"
        "  kv.open({ namespace: 'other' }).set('k', stored);\n"
        "  const encOther = kv.open({ namespace: 'other', encrypt: { keys: { 1: K1 }, current: 1 } });\n"
        "  if (code(() => encOther.get('k')) !== 'decrypt_failed') return 5;\n"
        "  raw.set('planted', 'plain');\n"
        "  if (code(() => enc.get('planted')) !== 'decrypt_failed') return 6;\n"
        "  if (code(() => enc.incr('n', 1)) !== 'unsupported') return 7;\n"
        "  if (!enc.cas('c', null, 'a') || enc.cas('c', null, 'b')) return 8;\n"
        "  if (enc.cas('c', 'x', 'b') || !enc.cas('c', 'a', 'b') || enc.get('c') !== 'b') return 9;\n"
        "  raw.delete('moved'); raw.delete('planted');\n"
        "  const both = kv.open({ namespace: 'sec', encrypt: { keys: { 1: K1, 2: K2 }, current: 2 } });\n"
        "  if (both.get('k') !== v) return 10;\n"
        "  if (both.rekey() !== 2 || both.rekey() !== 0) return 11;\n"
        "  const only2 = kv.open({ namespace: 'sec', encrypt: { keys: { 2: K2 }, current: 2 } });\n"
        "  if (only2.get('k') !== v || only2.get('c') !== 'b') return 12;\n"
        "  if (code(() => enc.get('k')) !== 'decrypt_failed') return 13;\n"
        "  raw.set('old', 'legacy');\n"
        "  const mig = kv.open({ namespace: 'sec', encrypt: { keys: { 2: K2 }, current: 2, allowPlaintext: true } });\n"
        "  if (mig.get('old') !== 'legacy') return 14;\n"
        "  if (mig.rekey() !== 1 || only2.get('old') !== 'legacy') return 15;\n"
        "  if (code(() => raw.rekey()) !== 'invalid_argument') return 16;\n"
        "  if (code(() => kv.open({ encrypt: 'x' })) !== 'invalid_argument') return 17;\n"
        "  if (code(() => kv.open({ encrypt: { keys: { 1: 'short' }, current: 1 } })) !== 'invalid_argument') return 18;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__kvenc = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__kvenc"), 0);
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, auth_flows_token_round_trip)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code = AF_INIT_JS
        "authFlows.init(defaults());\n"
        "const A = __testHooks('hull:web:auth-flows').ACTIONS;\n"
        /* Indexed access instead of array destructuring - QuickJS's
         * js_parse_destructuring_element trips a MSan false-
         * positive that also bit qrcode.js and jwt.js earlier. */
        "function run() {\n"
        "  const tok = __testHooks('hull:web:auth-flows').issueToken('u1', A.verify_email, 60);\n"
        "  const r1 = __testHooks('hull:web:auth-flows').consumeToken(tok, A.verify_email);\n"
        "  if (!r1[0] || r1[0].sub !== 'u1') return 0;\n"
        "  const r2 = __testHooks('hull:web:auth-flows').consumeToken(tok, A.verify_email);\n"
        "  if (r2[0] || r2[1] !== 'replayed') return 0;\n"
        "  return 1;\n"
        "}\n"
        "function rejections() {\n"
        "  const tok = __testHooks('hull:web:auth-flows').issueToken('u1', A.verify_email, 60);\n"
        "  const r1 = __testHooks('hull:web:auth-flows').consumeToken(tok, A.password_reset);\n"
        "  if (r1[1] !== 'wrong action') return 0;\n"
        "  const tampered = tok.slice(0, -2) + (tok.slice(-1) === 'a' ? 'bb' : 'aa');\n"
        "  const r2 = __testHooks('hull:web:auth-flows').consumeToken(tampered, A.verify_email);\n"
        "  if (r2[1] !== 'bad tag') return 0;\n"
        "  const r3 = __testHooks('hull:web:auth-flows').consumeToken('garbage', A.verify_email);\n"
        "  if (r3[1] !== 'malformed') return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__af_rt = run();\n"
        "globalThis.__af_rj = rejections();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__af_rt"), 1);
    ASSERT_EQ(eval_int("globalThis.__af_rj"), 1);

    cleanup_js_caps();
}
#endif

/* Login CSRF (audit 8): the guard on every session-setting POST - see the
 * Lua twin auth_flows_cross_site_guard. */
UTEST(js_stdlib, auth_flows_cross_site_guard)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code = AF_INIT_JS
        "authFlows.init(defaults());\n"
        "const g = __testHooks('hull:web:auth-flows').sameOriginRequest;\n"
        "const st = __testHooks('hull:web:auth-flows').state;\n"
        "st.trustRequestHost = false; st.trustedHosts = ['app.example.com'];\n"
        "const form = 'application/x-www-form-urlencoded';\n"
        "function req(h, body) { if (!h['content-type']) h['content-type'] = form;\n"
        "  return { headers: h, body: body === undefined ? 'email=a%40b.co&password=x' : body }; }\n"
        "function run() {\n"
        "  if (!g(req({ 'sec-fetch-site': 'same-origin' }))) return 1;\n"
        "  if (g(req({ 'sec-fetch-site': 'cross-site' }))) return 2;\n"
        "  if (g(req({ 'sec-fetch-site': 'same-site' }))) return 3;\n"
        "  if (g(req({ host: 'app.example.com' }))) return 4;\n"
        "  if (!g(req({ host: 'app.example.com', origin: 'https://app.example.com' }))) return 5;\n"
        "  if (g(req({ host: 'app.example.com', origin: 'https://evil.example' }))) return 6;\n"
        "  if (g(req({ host: 'app.example.com', origin: 'null' }))) return 7;\n"
        "  if (!g(req({ host: 'x.test:81', referer: 'http://x.test:81/login' }))) return 8;\n"
        "  if (g(req({ 'content-type': 'text/plain; x=application/json' }, '{\"email\":\"a\"}'))) return 9;\n"
        "  if (!g(req({ 'content-type': 'application/json; charset=utf-8' }, '{\"email\":\"a\"}'))) return 10;\n"
        "  if (g(req({ 'content-type': 'application/json' }, 'email=a'))) return 11;\n"
        "  if (g(req({ 'sec-fetch-site': 'cross-site', origin: 'https://app.example.com' }))) return 12;\n"
        "  if (!g({ headers: {}, body: '' }, true)) return 13;\n"
        "  if (g(req({ origin: 'https://app.example.com.evil.test' }))) return 14;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__af_xs = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__af_xs"), 0);

    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* An emailSend that returns a Promise (email.send is async) has its failure
 * observed - logged - rather than dropped unhandled (audit 8). */
UTEST(js_stdlib, auth_flows_async_email_send_failure_observed)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code = AF_INIT_JS
        "globalThis.__af_obs = 0;\n"
        "const o = defaults();\n"
        "o.emailSend = () => ({ then(ok, fail) {\n"
        "  if (typeof fail === 'function') { globalThis.__af_obs = 1; fail(new Error('smtp 554')); } } });\n"
        "authFlows.init(o);\n"
        "authFlows.sendVerifyEmail({ id: 'u1', email: 'a@x.com' }, 'http://t.io');\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__af_obs"), 1);

    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, auth_flows_register_verify_login)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code = AF_INIT_JS
        "authFlows.init(defaults());\n"
        "const A = __testHooks('hull:web:auth-flows').ACTIONS;\n"
        "function run() {\n"
        "  authFlows.sendVerifyEmail({id: 'u1', email: 'a@x.com'}, 'http://t.io');\n"
        "  if (globalThis._sent.length !== 1) return 0;\n"
        "  const link = globalThis._sent[0].text;\n"
        "  const tok = link.match(/token=(.+)/)[1];\n"
        "  const r = __testHooks('hull:web:auth-flows').consumeToken(tok, A.verify_email);\n"
        "  return (r[0] && r[0].sub === 'u1') ? 1 : 0;\n"
        "}\n"
        "globalThis.__af_flow = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__af_flow"), 1);

    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, auth_flows_input_validation)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { authFlows } from 'hull:web:auth-flows';\n"
        "__testHooks('hull:web:auth-flows').reset();\n"
        "const t = __testHooks('hull:web:auth-flows');\n"
        "function run() {\n"
        "  if (!t.isEmailIsh('a@b.co')) return 0;\n"
        "  if ( t.isEmailIsh('')) return 0;\n"
        "  if ( t.isEmailIsh('no-at-sign')) return 0;\n"
        "  if ( t.isEmailIsh('@leading')) return 0;\n"
        "  if ( t.isEmailIsh('trailing@')) return 0;\n"
        "  if ( t.isEmailIsh('a@b')) return 0;\n"
        "  if ( t.isEmailIsh('a@b.')) return 0;\n"
        "  return 1;\n"
        "}\n"
        "globalThis.__af_iv = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__af_iv"), 1);

    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, auth_flows_magic_link_auto_signup_opt_in)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code = AF_INIT_JS
        "function run() {\n"
        "  authFlows.init(defaults());\n"
        "  authFlows.sendMagicLink('unknown@x.com', 'http://t.io');\n"
        "  if (globalThis._sent.length !== 0) return 0;\n"
        "  __testHooks('hull:web:auth-flows').reset();\n"
        "  globalThis._sent = []; globalThis._users = {}; globalThis._byId = {};\n"
        "  const o = defaults();\n"
        "  o.magicLinkAutoSignup = true;\n"
        "  authFlows.init(o);\n"
        "  authFlows.sendMagicLink('new@x.com', 'http://t.io');\n"
        "  return (globalThis._sent.length === 1 "
        "          && globalThis._users['new@x.com']) ? 1 : 0;\n"
        "}\n"
        "globalThis.__af_ml = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__af_ml"), 1);

    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, auth_flows_state_secret_non_ascii_round_trip)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* Round-8 HIGH-2: the JS-side bytesToHex local already handled
     * code points >= 0x80 correctly (long-standing). This test pins
     * the contract so a future refactor that hexes the secret any other
     * way than hull:encoding (byte for byte) can't regress parity. Pairs with the
     * Lua counterpart auth_flows_state_secret_non_ascii_round_trip. */
    const char *code = AF_INIT_JS
        "function run() {\n"
        "  const o = defaults();\n"
        "  o.stateSecret = String.fromCharCode(0x80).repeat(32);\n"
        "  authFlows.init(o);\n"
        "  const A = __testHooks('hull:web:auth-flows').ACTIONS;\n"
        "  const tok = __testHooks('hull:web:auth-flows').issueToken('u1', A.verify_email, 60);\n"
        "  const r = __testHooks('hull:web:auth-flows').parseToken(tok, A.verify_email);\n"
        "  return (r && r[0] && r[1] === null && r[0].sub === 'u1') ? 1 : 0;\n"
        "}\n"
        "globalThis.__af_state = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__af_state"), 1);

    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, auth_flows_email_rate_limit_per_recipient)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* Round-8 HIGH-1: per-recipient email rate limit closes the
     * attacker-chosen-recipient email-storm class. Gate sits inside
     * sendEmail; blocked sends are silently dropped so the response
     * shape stays enumeration-safe. Buckets are per (lower-cased)
     * recipient with a sliding window. */
    const char *code = AF_INIT_JS
        "function run() {\n"
        "  const o = defaults();\n"
        "  o.emailRateLimit = { limit: 2, window: 60 };\n"
        "  authFlows.init(o);\n"
        "  const a1 = __testHooks('hull:web:auth-flows').emailRateAllow('victim@x.com');\n"
        "  const a2 = __testHooks('hull:web:auth-flows').emailRateAllow('victim@x.com');\n"
        "  const a3 = __testHooks('hull:web:auth-flows').emailRateAllow('victim@x.com');\n"
        "  const b1 = __testHooks('hull:web:auth-flows').emailRateAllow('other@x.com');\n"
        "  const c1 = __testHooks('hull:web:auth-flows').emailRateAllow('VICTIM@x.com');\n"
        "  __testHooks('hull:web:auth-flows').emailRateReset();\n"
        "  const d1 = __testHooks('hull:web:auth-flows').emailRateAllow('victim@x.com');\n"
        "  /* Disabled cfg: every call allowed. */\n"
        "  const o2 = defaults();\n"
        "  o2.emailRateLimit = false;\n"
        "  authFlows.init(o2);\n"
        "  const off1 = __testHooks('hull:web:auth-flows').emailRateAllow('victim@x.com');\n"
        "  const off2 = __testHooks('hull:web:auth-flows').emailRateAllow('victim@x.com');\n"
        "  const off3 = __testHooks('hull:web:auth-flows').emailRateAllow('victim@x.com');\n"
        "  return (a1 && a2 && !a3 && b1 && !c1 && d1 "
        "          && off1 && off2 && off3) ? 1 : 0;\n"
        "}\n"
        "globalThis.__af_rl = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__af_rl"), 1);

    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, auth_flows_email_rate_limit_drops_send)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* Integration check: confirm the gate actually drops the
     * downstream sendEmail call (not just returns false from
     * emailRateAllow). The fixture exercises the path via
     * sendMagicLink + magicLinkAutoSignup so we don't need
     * route plumbing here. */
    const char *code = AF_INIT_JS
        "function run() {\n"
        "  const o = defaults();\n"
        "  o.emailRateLimit = { limit: 2, window: 60 };\n"
        "  o.magicLinkAutoSignup = true;\n"
        "  authFlows.init(o);\n"
        "  authFlows.sendMagicLink('flood@x.com', 'http://t.io');\n"
        "  authFlows.sendMagicLink('flood@x.com', 'http://t.io');\n"
        "  authFlows.sendMagicLink('flood@x.com', 'http://t.io');\n"
        "  authFlows.sendMagicLink('flood@x.com', 'http://t.io');\n"
        "  /* Other recipient must still go through. */\n"
        "  authFlows.sendMagicLink('clean@x.com', 'http://t.io');\n"
        "  return globalThis._sent.length === 3 ? 1 : 0;\n"
        "}\n"
        "globalThis.__af_rl2 = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__af_rl2"), 1);

    cleanup_js_caps();
}
#endif

/* ── hull:web:cookie tests ─────────────────────────────────────────────── */

UTEST(js_stdlib, cookie_parse)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { cookie } from 'hull:web:cookie';\n"
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
        "import { cookie } from 'hull:web:cookie';\n"
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
        "import { cookie } from 'hull:web:cookie';\n"
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

/* ── hull:web:middleware:session tests ─────────────────────────────────── */

UTEST(js_stdlib, session_create_and_load)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { session } from 'hull:web:middleware:session';\n"
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
        "import { session } from 'hull:web:middleware:session';\n"
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

/* ── hull:web:middleware:csrf tests ────────────────────────────────────── */

UTEST(js_stdlib, csrf_generate_and_verify)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csrf } from 'hull:web:middleware:csrf';\n"
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

UTEST(js_stdlib, crypto_constant_time_eq)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "globalThis.__cte1 = crypto.constantTimeEq('abc','abc') ? 1 : 0;\n"  /* equal */
        "globalThis.__cte2 = crypto.constantTimeEq('abc','abd') ? 1 : 0;\n"  /* differ */
        "globalThis.__cte3 = crypto.constantTimeEq('abc','ab') ? 1 : 0;\n"   /* length */
        "globalThis.__cte4 = crypto.constantTimeEq('','') ? 1 : 0;\n";       /* empty */

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__cte1"), 1);
    ASSERT_EQ(eval_int("globalThis.__cte2"), 0);
    ASSERT_EQ(eval_int("globalThis.__cte3"), 0);
    ASSERT_EQ(eval_int("globalThis.__cte4"), 1);

    cleanup_js_caps();
}

UTEST(js_stdlib, csrf_wrong_session_rejected)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csrf } from 'hull:web:middleware:csrf';\n"
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

/* Cross-runtime wire-format fixture. The reference token below was
 * precomputed for sessionId="s1", secret="k", tsHex="1" - i.e. the
 * HMAC of "s1:1" keyed by hex("k")="6b". The same fixture lives in
 * tests/hull/runtime/lua/test_lua.c; both must accept it byte-for-byte
 * or the Lua and JS sibling middlewares have drifted out of parity. */
UTEST(js_stdlib, csrf_cross_runtime_reference_token)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { csrf } from 'hull:web:middleware:csrf';\n"
        "const ref = '1.6ae78d056ed813a207a55074947fdbeef0ae8c7850acab486cb52bae058956da';\n"
        "globalThis.__test_csrf_ref_ok = csrf.verify(ref, 's1', 'k', 4294967295) ? 1 : 0;\n"
        /* Flip one bit of the MAC - must reject. */
        "const bad = '1.7ae78d056ed813a207a55074947fdbeef0ae8c7850acab486cb52bae058956da';\n"
        "globalThis.__test_csrf_ref_bad = csrf.verify(bad, 's1', 'k', 4294967295) ? 0 : 1;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_csrf_ref_ok"), 1);
    ASSERT_EQ(eval_int("globalThis.__test_csrf_ref_bad"), 1);

    cleanup_js_caps();
}

/* ── hull:web:middleware:auth tests (smoke - modules load and expose API) */

/* The byte API: every key, nonce, signature, tag and digest is an ArrayBuffer
 * of its exact size, round trips work, and a wrong-length argument throws.
 * run() returns 0, or the number of the first check that failed. */
UTEST(js_cap, crypto_bytes_contract)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "import { encoding } from 'hull:encoding';\n"
        "const hex = (b) => encoding.hex.encode(b);\n"
        "const len = (b) => b.byteLength;\n"
        "function threw(f) { try { f(); return false; } catch (e) { return true; } }\n"
        "function run() {\n"
        "  if (len(crypto.sha256('x')) !== 32 || len(crypto.sha512('x')) !== 64) return 1;\n"
        "  if (len(crypto.hmacSha256('x', 'k')) !== 32 || len(crypto.hmacSha1('x', 'k')) !== 20) return 2;\n"
        "  const h = crypto.createSha256(); h.update('he'); h.update(new Uint8Array([108, 108, 111]));\n"
        "  if (hex(h.digest()) !== hex(crypto.sha256('hello'))) return 3;\n"
        "  const kp = crypto.ed25519Keypair();\n"
        "  if (len(kp.publicKey) !== 32 || len(kp.secretKey) !== 64) return 4;\n"
        "  const sig = crypto.ed25519Sign('msg', kp.secretKey);\n"
        "  if (len(sig) !== 64 || !crypto.ed25519Verify('msg', sig, kp.publicKey)) return 5;\n"
        "  if (crypto.ed25519Verify('msh', sig, kp.publicKey)) return 6;\n"
        "  const key = crypto.random(32), nonce = crypto.random(24);\n"
        "  const ct = crypto.secretbox('secret', nonce, key);\n"
        "  const pt = crypto.secretboxOpen(ct, nonce, key);\n"
        "  if (len(ct) !== 6 + 16 || encoding.bytes.fromBuffer(pt) !== 'secret') return 7;\n"
        "  const bad = new Uint8Array(ct); bad[0] ^= 1;\n"
        "  if (crypto.secretboxOpen(bad, nonce, key) !== null) return 8;\n"
        "  const a = crypto.boxKeypair(), b = crypto.boxKeypair();\n"
        "  const bct = crypto.box('hi', nonce, b.publicKey, a.secretKey);\n"
        "  if (encoding.bytes.fromBuffer(crypto.boxOpen(bct, nonce, a.publicKey, b.secretKey)) !== 'hi') return 9;\n"
        "  const xa = crypto.x25519Keypair(), xb = crypto.x25519Keypair();\n"
        "  const s1 = crypto.x25519(xa.secretKey, xb.publicKey), s2 = crypto.x25519(xb.secretKey, xa.publicKey);\n"
        "  if (len(s1) !== 32 || hex(s1) !== hex(s2)) return 10;\n"
        "  const tag = crypto.auth('m', key);\n"
        "  if (len(tag) !== 32 || !crypto.authVerify(tag, 'm', key)) return 11;\n"
        "  if (!threw(() => crypto.ed25519Sign('m', new Uint8Array(63)))) return 12;\n"
        "  if (!threw(() => crypto.secretbox('m', nonce, new Uint8Array(31)))) return 13;\n"
        "  if (!threw(() => crypto.hmacSha256('m', ''))) return 14;\n"
        "  if (crypto.hmacSha256Verify('m', 'k', new Uint8Array(3))) return 15;\n"
        "  return 0;\n"
        "}\n"
        "globalThis.__bytes = run();\n";
    ASSERT_EQ(js_run_steps(code, "globalThis.__bytes"), 0);
    cleanup_js_caps();
}

UTEST(js_cap, crypto_hmac_sha256_verify)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "const mac = crypto.hmacSha256('what do ya want for nothing?', 'Jefe');\n"
        "globalThis.__test_hv_ok = crypto.hmacSha256Verify('what do ya want for nothing?', 'Jefe', mac) ? 1 : 0;\n"
        "globalThis.__test_hv_bad_mac = crypto.hmacSha256Verify('what do ya want for nothing?', 'Jefe', "
        "  new Uint8Array(32)) ? 1 : 0;\n"
        "const mac2 = crypto.hmacSha256('hello', 'Jefe');\n"
        "globalThis.__test_hv_bad_key = crypto.hmacSha256Verify('hello', 'Jeff', mac2) ? 1 : 0;\n";

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
        "import { auth } from 'hull:web:middleware:auth';\n"
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

/* ── hull:web:form tests ─────────────────────────────────────────────────── */

UTEST(js_stdlib, form_parse)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { form } from 'hull:web:form';\n"
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

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, email_validation)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-client@1'] });\n"
        "import { email } from 'hull:email';\n"
        "async function code(o){ try { await email.send(o); return '_ok'; } "
        "catch (e) { return (e.code||'_nocode') + ':' + (e.message||''); } }\n"
        "globalThis.__test_ev1 = ((await code(null)) === 'invalid_argument:opts required') ? 1 : 0;\n"
        "globalThis.__test_ev2 = ((await code({ to: 'x@y.com', subject: 's', body: 'b' })) === 'invalid_argument:from required') ? 1 : 0;\n"
        "globalThis.__test_ev3 = ((await code({ from: 'x@y.com', subject: 's', body: 'b' })) === 'invalid_argument:to required') ? 1 : 0;\n";

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
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, email_unknown_provider)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-client@1'] });\n"
        "import { email } from 'hull:email';\n"
        "async function code(o){ try { await email.send(o); return '_ok'; } "
        "catch (e) { return (e.code||'_nocode') + '|' + (e.message||''); } }\n"
        "var r = await code({ provider: 'foo', from: 'a@b.com', "
        "to: 'c@d.com', subject: 's', body: 'b' });\n"
        "globalThis.__test_eup = (r.indexOf('unknown_provider|') === 0 && r.indexOf('unknown provider') >= 0) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_eup"), 1);

    cleanup_js();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, email_api_key_required)
{
    init_js();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-client@1'] });\n"
        "import { email } from 'hull:email';\n"
        "async function code(o){ try { await email.send(o); return '_ok'; } "
        "catch (e) { return (e.code||'_nocode') + '|' + (e.message||''); } }\n"
        "var r = await code({ provider: 'postmark', from: 'a@b.com', "
        "to: 'c@d.com', subject: 's', body: 'b' });\n"
        "globalThis.__test_eak = (r.indexOf('invalid_argument|') === 0 && r.indexOf('api_key required') >= 0) ? 1 : 0;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__test_eak"), 1);

    cleanup_js();
}
#endif

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

/* The user-facing JS stdlib ships 12 co-located test scripts under
 * stdlib/js/hull/tests/. Until now NOTHING ran them: no Makefile glob collects
 * that directory -- every glob that names it does so only to EXCLUDE any
 * path under a tests directory -- and no harness loaded them. Note the
 * contrast with the cli-js tree, whose tests ARE collected:
 * STDLIB_JS_CLI_TEST_ONLY_FILES
 * embeds it into a test-only registry -- so the machinery existed; these
 * directories were simply never wired to anything.
 *
 * First one wired. csv is pure (no capability use), which keeps this leg about
 * the seam rather than about caps; the harness supplies them regardless, so
 * db-using scripts can follow without a second mechanism. */
#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* test_email.js wraps its assertions in an async IIFE around await
 * email.send(...). Every case is a validation rejection -- the script does no
 * network I/O -- so the chain settles on the microtask queue that
 * hl_js_run_jobs already drains, and the counts are published before the
 * harness reads them. A case that did real I/O would need the async backend
 * driven, which this harness does not do. */
UTEST(js_stdlib, email_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_email.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}
#endif

UTEST(js_stdlib, encoding_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_encoding.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, csv_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_csv.js", &pass, &fail);
    ASSERT_EQ(rc, 0);        /* evaluated and published counts */
    EXPECT_EQ(fail, 0);      /* every assertion in the script held */
    EXPECT_GT(pass, 0);      /* and it actually executed cases */

    cleanup_js_caps();
}

/* The middleware suites under web/middleware/tests, which no harness
 * ran (they ended in `export default`, which the loader cannot read). */
UTEST(js_stdlib, middleware_auth_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_auth.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, middleware_cors_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_cors.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, middleware_csp_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_csp.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, middleware_csrf_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_csrf.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, middleware_idempotency_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_idempotency.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, middleware_inbox_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_inbox.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, middleware_logger_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_logger.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, middleware_outbox_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_outbox.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}
#endif

UTEST(js_stdlib, middleware_ratelimit_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_ratelimit.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, middleware_rbac_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_rbac.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, middleware_session_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_session.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, middleware_transaction_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/web/middleware/tests/test_transaction.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

/* The rest of the co-located JS stdlib suites -- same story as csv_suite
 * above: written, committed, and never run by anything.
 *
 * Four published their counts via `export default`, which the harness cannot
 * reach through globalThis, and six ended by throwing. Both forms are now the
 * globalThis convention the harness reads.
 *
 * test_email.js is deliberately absent: it wraps its assertions in an async
 * IIFE around real `await email.send(...)` calls, so its counts are published
 * only once genuine async work settles -- more than hl_js_run_jobs pumping
 * microtasks will deliver. It needs the async backend driven, separately. */
UTEST(js_stdlib, confirm_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_confirm.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, form_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_form.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, htmx_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_htmx.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, htmx_form_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_htmx_form.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, htmx_inline_edit_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_htmx_inline_edit.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, htmx_search_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_htmx_search.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, i18n_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_i18n.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, search_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_search.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

/* Stdlib regressions from audit 10 (logx / _logfmt escaping, cache.fetch
 * misses, i18n, qrcode, csv): logx needs hull:log, cache hull:time. */
UTEST(js_stdlib, stdlib_audit10_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit10.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

/* hull:cache / hull:kv memory + SQL stores, rbac names (audit 5 DA-L3..L6). */
UTEST(js_stdlib, kv_cache_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_kv_cache.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* auth loginPath redirect, ratelimit keys, health ping, auditLog kinds,
 * csrf safeMethods (audit 6 JS M2 / L1 / L5 / L6, c_js audit-log kinds). */
UTEST(js_stdlib, middleware_audit6_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_middleware_audit6.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Web stdlib regressions from audit 9 (auth-flows email-change undo, lockout
 * id keys, sync setters, ratelimit, cookie, session logout, idempotency,
 * oauth). */
UTEST(js_stdlib, web_audit9_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_web_audit9.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Auth stdlib regressions from audit 10 (the vacated address of an undoable
 * email change is reserved, undo resets / totpDisable, deferred magic-link
 * signup, logout origins, idempotency principal, inbox source). */
UTEST(js_stdlib, auth_audit10_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_auth_audit10.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Auth-flows regressions from audit 12 (token mails go to the stored address,
 * exact standardUsers lookup, unsuppressible email-change notice, persistent
 * recovery lock, keyed revoke / confirm writes, init requirements). */
UTEST(js_stdlib, auth_audit12_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_auth_audit12.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}
#endif

/* The _hull_jobs / _hull_cron tables as a database from before jobs.init's
 * additive columns has them (progress .. claim_started_at, tz_offset,
 * last_error). Created by the harness: app code may not touch _hull_*. */
static const char AUDIT15_OLD_JOBS_SCHEMA[] =
    "CREATE TABLE _hull_jobs ("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " queue VARCHAR(255) NOT NULL DEFAULT 'default',"
    " type VARCHAR(255) NOT NULL, payload TEXT,"
    " status VARCHAR(32) NOT NULL DEFAULT 'pending',"
    " priority INTEGER NOT NULL DEFAULT 0,"
    " attempts INTEGER NOT NULL DEFAULT 0,"
    " max_attempts INTEGER NOT NULL DEFAULT 25,"
    " run_at INTEGER NOT NULL DEFAULT 0,"
    " claim_token VARCHAR(255), claimed_at INTEGER, dedup_key VARCHAR(255),"
    " last_error TEXT, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
    "CREATE TABLE _hull_cron ("
    " name VARCHAR(255) NOT NULL PRIMARY KEY, spec VARCHAR(255) NOT NULL,"
    " type VARCHAR(255) NOT NULL, payload TEXT,"
    " queue VARCHAR(255) NOT NULL DEFAULT 'default',"
    " priority INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER,"
    " next_run_at INTEGER NOT NULL, last_run_at INTEGER,"
    " updated_at INTEGER NOT NULL);";

/* Statements that prepare only once jobs.init has added every column. */
static const char *const AUDIT15_NEW_COLUMNS[] = {
    "SELECT progress, trace_context, concurrency_key, concurrency_limit, "
    "concurrency_strict, claim_started_at FROM _hull_jobs",
    "SELECT tz_offset, last_error FROM _hull_cron",
};

/* Stdlib regressions from audit 15: jobs.init upgrades an old schema (the
 * concurrency index came before the column it indexes), ttl options are
 * positive numbers, TOTP secrets are bound to their account. */
UTEST(js_stdlib, stdlib_audit15_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(sqlite3_exec(test_db, AUDIT15_OLD_JOBS_SCHEMA, NULL, NULL, NULL), SQLITE_OK);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit15.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    for (size_t i = 0; i < sizeof AUDIT15_NEW_COLUMNS / sizeof AUDIT15_NEW_COLUMNS[0]; i++) {
        sqlite3_stmt *st = NULL;
        EXPECT_EQ(sqlite3_prepare_v2(test_db, AUDIT15_NEW_COLUMNS[i], -1, &st, NULL), SQLITE_OK);
        sqlite3_finalize(st);
    }

    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 16: no module exports its test hooks to app
 * code (hull:_test_hooks is stdlib-internal), totp's allowUnboundSecrets and
 * rekeyStatus().unbound, and 42.0 / 42 as one user id. */
UTEST(js_stdlib, stdlib_audit16_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit16.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}
#endif

/* Stdlib regressions from audit 22: totp's allowPlaintextSecrets and
 * rekeyStatus().plaintext. */
UTEST(js_stdlib, stdlib_audit22_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit22.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 23: template.compile's (data) => string
 * wrapper, config's finite numbers, the idempotency principal, the jobs.dead
 * and attachment.sweep limits. */
UTEST(js_stdlib, stdlib_audit23_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit23.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 24: csv columns / nil cells, clamped LIMITs,
 * session device refs, the outbox delivery timeout, the idempotency lossy
 * body, jwt / totp / validate number checks, trusted-host ports, template
 * own-property paths, the email-change user id. */
UTEST(js_stdlib, stdlib_audit24_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit24.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 25: jobs / outbox schedule values are
 * finite whole seconds, outbox maxAttempts a whole number. */
UTEST(js_stdlib, stdlib_audit25_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit25.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}
#endif

/* Stdlib regressions from audit 26: safe_url raw output, template dot paths
 * render no function, search snippet / highlight escaping. */
UTEST(js_stdlib, stdlib_audit26_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit26.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib option validation from audit 26: auditLog retainDays, attachment
 * maxSize, search limit / offset, jobs maxAttempts / cleanup before,
 * idempotency / inbox ttl bounds, email C1 controls, htmx form own props. */
UTEST(js_stdlib, stdlib_options_audit26_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_options_audit26.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}
#endif

/* TOTP regressions from audit 26: a stripped (v1-shaped) bound blob is not
 * another account's secret, unbound / legacy secrets are 20 bytes, a success
 * keeps the per-IP count, maxLockoutDuration / pendingTtl are numbers. */
UTEST(js_stdlib, totp_audit26_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_totp_audit26.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}

/* Stdlib regressions from audit 27: csv headers in first-seen order, search
 * paging clamps, jobs time options as whole seconds, null maxAttempts is
 * unset, count options read as Lua's tonumber, attachment maxSize: false. */
UTEST(js_stdlib, stdlib_audit27_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit27.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 28: unpaired surrogates in addresses,
 * safe_url encodes a backslash, jobs durations read as Lua's tonumber. */
UTEST(js_stdlib, stdlib_audit28_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit28.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}
#endif

/* Stdlib regressions from audit 29: jobs keys stored as strings and capped
 * at the column, csrf 413 / empty header, one Bearer rule. */
UTEST(js_stdlib, stdlib_audit29_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit29.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* auth-flows needs both HTTP halves. */
/* Stdlib regressions from audit 30: the login lockout and TOTP attempt
 * counters incremented by the database (semantics unchanged), and
 * rbac.hasAnyRole / hasAnyPermission taking a string as one name. */
UTEST(js_stdlib, stdlib_audit30_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit30.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}

/* Stdlib regressions from audit 31: lockout counters decide their INSERT
 * with a non-locking read (MySQL gap-lock deadlock), bump the account row
 * before the per-IP one, a TOTP per-IP failure keeps a running lock, and
 * /login counts an unknown address under its own key. */
UTEST(js_stdlib, stdlib_audit31_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit31.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}
#endif

/* TOTP regressions from audit 28: a success takes only the user's own
 * failures off the per-IP count; other accounts' failures still lock. */
UTEST(js_stdlib, totp_audit28_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_totp_audit28.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 17: etag helpers with an empty body, and
 * jobs' live config moved to hull:_test_hooks. */
UTEST(js_stdlib, stdlib_audit17_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit17.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}
#endif

/* Stdlib regressions from audit 18: jobs.events / purge / cleanup / subscribe
 * build SQL placeholders from a counted loop over checked strings, never by a
 * method on the caller's array (an own `map` / `join` returned SQL run from
 * the stdlib frame). */
UTEST(js_stdlib, stdlib_audit18_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit18.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);
    cleanup_js_caps();
}

/* hull:jobs through its clock-injectable test hooks (audit 17: jobs._tick /
 * _cronNext / _eventsDrain left the module for hull:_test_hooks, so the
 * deterministic cron and subscription-drain cases moved here from
 * tests/e2e_jobs.sh). Each case runs the script in a fresh context and a
 * fresh :memory: database, named in globalThis.__jobs_case. */
static int run_jobs_hooks_case(const char *name, int *pass, int *fail)
{
    init_js_with_caps();
    if (!js_initialized) return -1;
    char set[128];
    snprintf(set, sizeof set, "globalThis.__jobs_case = '%s';", name);
    JSValue v = JS_Eval(js.ctx, set, strlen(set), "<jobs-case>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(js.ctx, v);
    int rc = run_js_test("stdlib/js/hull/tests/test_jobs_hooks.js", pass, fail);
    cleanup_js_caps();
    return rc;
}

#define JOBS_HOOKS_CASE(c)                                         UTEST(js_stdlib, jobs_hooks_##c)                               {                                                                  int pass = 0, fail = -1;                                       ASSERT_EQ(run_jobs_hooks_case(#c, &pass, &fail), 0);           EXPECT_EQ(fail, 0);                                            EXPECT_GT(pass, 1);                                        }
JOBS_HOOKS_CASE(cron)
JOBS_HOOKS_CASE(events)
JOBS_HOOKS_CASE(poison)
JOBS_HOOKS_CASE(horizon)
JOBS_HOOKS_CASE(audit20)
JOBS_HOOKS_CASE(audit23)
JOBS_HOOKS_CASE(audit30)
#undef JOBS_HOOKS_CASE

UTEST(js_stdlib, toast_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_toast.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}

UTEST(js_stdlib, validate_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_validate.js", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_GT(pass, 0);

    cleanup_js_caps();
}


/* hull:search rejects a SQL keyword as an identifier.
 *
 * A keyword passes the IDENT_RE and the _hull_ prefix check -- it IS a plain
 * identifier -- so only this branch catches it, and `reindex` interpolates the
 * source table and column names UNPREFIXED. Without it the failure surfaces as
 * a bare SQLite syntax error naming neither the caller nor the word.
 *
 * The positive control runs FIRST and is not optional: every call here goes
 * through requireSqlite(), so on a harness without a SQLite-backed db EVERY
 * call throws and the three rejection counters below would all trip for the
 * wrong reason -- a test that passes precisely when it is measuring nothing.
 * A valid identifier must be accepted for the rejections to mean anything. */
UTEST(js_stdlib, search_rejects_sql_keyword)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { search } from 'hull:search';\n"
        "function run() {\n"
        "  try { search.createIndex('posts_ok', ['title']); }\n"
        "  catch (e) { return -1; }\n"
        "  let n = 0;\n"
        "  try { search.createIndex('posts_kw', ['from']); } catch (e) { n++; }\n"
        "  try { search.reindex('posts_ok', 'order', { columns: { title: 'title' } }); }\n"
        "  catch (e) { n++; }\n"
        "  try { search.createIndex('Select', ['col']); } catch (e) { n++; }\n"
        "  return n;\n"
        "}\n"
        "globalThis.__search_kw = run();\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* -1 = the positive control itself threw: the harness has no usable db and
     * this test measured nothing. Distinct from a missed rejection. */
    ASSERT_NE(eval_int("globalThis.__search_kw"), -1);
    ASSERT_EQ(eval_int("globalThis.__search_kw"), 3);

    cleanup_js_caps();
}

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

/* Tokenize grammar parity with Lua. First token is an identifier;
 * subsequent space-separated tokens may be identifiers or positive
 * integers. Leading/trailing/double spaces, leading underscores, etc.
 * are rejected. */
UTEST(js_stdlib, search_tokenize_grammar_parity)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { search } from 'hull:search';\n"
        "function tryCreate(tok) {\n"
        "  try { search.createIndex('tk_x', ['t'], { tokenize: tok });\n"
        "        search.dropIndex('tk_x'); return true; }\n"
        "  catch (_e) { return false; }\n"
        "}\n"
        /* Valid: identifier; identifier + identifier args;
         * identifier + identifier + integer arg. */
        "globalThis.__tk_v1 = tryCreate('unicode61') ? 1 : 0;\n"
        "globalThis.__tk_v2 = tryCreate('porter ascii') ? 1 : 0;\n"
        "globalThis.__tk_v3 = tryCreate('unicode61 remove_diacritics 1') ? 1 : 0;\n"
        /* Invalid: space variants, leading digit, leading underscore, empty. */
        "globalThis.__tk_b1 = tryCreate(' ') ? 0 : 1;\n"
        "globalThis.__tk_b2 = tryCreate('  ') ? 0 : 1;\n"
        "globalThis.__tk_b3 = tryCreate(' unicode61') ? 0 : 1;\n"
        "globalThis.__tk_b4 = tryCreate('unicode61 ') ? 0 : 1;\n"
        "globalThis.__tk_b5 = tryCreate('unicode61  porter') ? 0 : 1;\n"
        "globalThis.__tk_b6 = tryCreate('123abc') ? 0 : 1;\n"
        "globalThis.__tk_b7 = tryCreate('_foo') ? 0 : 1;\n"
        "globalThis.__tk_b8 = tryCreate('') ? 0 : 1;\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("globalThis.__tk_v1"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_v2"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_v3"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_b1"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_b2"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_b3"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_b4"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_b5"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_b6"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_b7"), 1);
    ASSERT_EQ(eval_int("globalThis.__tk_b8"), 1);

    cleanup_js_caps();
}

/* Audit 4 (stdlib, jobs / kv / cache / rbac). __test_a4 is 0, or the
 * number of the first check that failed. */
UTEST(js_stdlib, audit4_jobs_kv_cache_rbac)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { rbac } from 'hull:web:middleware:rbac';\n"
        "import { cache } from 'hull:cache';\n"
        "import { jobs } from 'hull:jobs';\n"
        "import { db as dbm } from 'hull:db';\n"
        "globalThis.__test_a4 = (() => {\n"
        "  const db = dbm.default();\n"
        /* 1-3: grant / assign create the rows their foreign keys need */
        "  db.exec('PRAGMA foreign_keys = ON');\n"
        "  rbac.init();\n"
        "  try { rbac.grant('ghost', 'ghost.read'); } catch (e) { return 1; }\n"
        "  try { rbac.assign('u1', 'ghost'); } catch (e) { return 2; }\n"
        "  if (!rbac.hasPermission('u1', 'ghost.read')) return 3;\n"
        "  db.exec('PRAGMA foreign_keys = OFF');\n"
        /* 4-6: cache.new is an LRU: a read refreshes, the oldest goes */
        "  const c = cache.new({ maxEntries: 2 });\n"
        "  c.set('a', 1); c.set('b', 2);\n"
        "  if (c.get('a') !== 1) return 4;\n"
        "  c.set('c', 3);\n"
        "  if (c.has('b') || !c.has('a') || !c.has('c')) return 5;\n"
        "  if (c.size() !== 2) return 6;\n"
        /* 7-8: cache.open is bounded by default; an explicit 0 is not */
        "  const o = cache.open({ namespace: 'a4bound' });\n"
        "  if (!(o._store.maxItems > 0)) return 7;\n"
        "  const u = cache.open({ namespace: 'a4unbound', maxItems: 0 });\n"
        "  if (u._store.maxItems !== 0) return 8;\n"
        /* 9-11: a job whose worker vanished on its last attempt is
         *       dead-lettered by the reaper */
        "  jobs.init();\n"
        "  const id = jobs.enqueue('a4', {}, { maxAttempts: 1, dedupKey: 'k1' });\n"
        "  if (!id) return 9;\n"
        "  if (jobs.claim({ batch: 1 }).length !== 1) return 10;\n"
        "  jobs.reap({ visibilityTimeout: 0 });\n"
        "  const j = jobs.get(id);\n"
        "  if (!j || j.status !== 'dead') return 11;\n"
        /* 12-13: a finished job's dedupKey no longer blocks a re-enqueue */
        "  const id2 = jobs.enqueue('a4', {}, { dedupKey: 'k1' });\n"
        "  if (!id2 || id2 === id) return 12;\n"
        "  if (jobs.enqueue('a4', {}, { dedupKey: 'k1' }) !== null) return 13;\n"
        "  return 0;\n"
        "})();\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    /* typeof guard: an undefined (a module that threw) also reads as 0 */
    ASSERT_EQ(eval_int("typeof globalThis.__test_a4 === 'number' ? globalThis.__test_a4 : -1"), 0);

    cleanup_js_caps();
}

/* Audit 5 (jobs reaper). __test_a5 is 0, or the number of the first check
 * that failed (-2: the async body threw). */
UTEST(js_stdlib, audit5_jobs_reaper)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { jobs } from 'hull:jobs';\n"
        "(async () => {\n"
        "  jobs.init({ backoff: () => 0 });\n"
        /* 1-2: more than one reaper pass of exhausted rows (500) are all
         *      dead-lettered; none is re-pended by the reclaim */
        "  for (let i = 0; i < 505; i++)\n"
        "    jobs.enqueue('a5bulk', {}, { queue: 'a5bulk', maxAttempts: 1 });\n"
        "  while (jobs.claim({ queue: 'a5bulk', batch: 200 }).length > 0) {}\n"
        "  jobs.reap({ visibilityTimeout: 0 });\n"
        "  const st = jobs.stats({ queue: 'a5bulk' });\n"
        "  if (st.dead !== 505) return 1;\n"
        "  if (st.pending !== 0 || st.running !== 0) return 2;\n"
        /* 3-7: a workflow whose worker is lost on its last attempt still
         *      runs its saga compensations, once, and dead-letters */
        "  const log = [];\n"
        "  jobs.workflow('a5wf', async (ctx) => {\n"
        "    await ctx.step('charge', () => { log.push('charge'); return 1; },\n"
        "      { compensate: () => { log.push('refund'); } });\n"
        "    await ctx.step('ship', () => { log.push('ship'); throw new Error('boom'); });\n"
        "  });\n"
        "  const id = jobs.start('a5wf', {}, { queue: 'a5wf', maxAttempts: 2 });\n"
        "  await jobs.work({ queue: 'a5wf' });\n"
        "  if (log.join(',') !== 'charge,ship') return 3;\n"
        "  if (jobs.claim({ queue: 'a5wf', batch: 1 }).length !== 1) return 4;\n"
        "  jobs.reap({ visibilityTimeout: 0 });\n"
        "  let j = jobs.get(id);\n"
        "  if (!j || j.status !== 'pending') return 5;\n"
        "  await jobs.work({ queue: 'a5wf' });\n"
        "  if (log.join(',') !== 'charge,ship,refund') return 6;\n"
        "  j = jobs.get(id);\n"
        "  if (!j || j.status !== 'dead') return 7;\n"
        /* 8-10: a compensation run that is itself lost dead-letters */
        "  const id2 = jobs.start('a5wf', {}, { queue: 'a5wf2', maxAttempts: 1 });\n"
        "  if (jobs.claim({ queue: 'a5wf2', batch: 1 }).length !== 1) return 8;\n"
        "  jobs.reap({ visibilityTimeout: 0 });\n"
        "  if (jobs.claim({ queue: 'a5wf2', batch: 1 }).length !== 1) return 9;\n"
        "  jobs.reap({ visibilityTimeout: 0 });\n"
        "  const j2 = jobs.get(id2);\n"
        "  if (!j2 || j2.status !== 'dead') return 10;\n"
        "  return 0;\n"
        "})().then((v) => { globalThis.__test_a5 = v; },\n"
        "          (e) => { console.log(String(e && e.stack || e)); globalThis.__test_a5 = -2; });\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    ASSERT_EQ(eval_int("typeof globalThis.__test_a5 === 'number' ? globalThis.__test_a5 : -1"), 0);

    cleanup_js_caps();
}

/* Audit 10 follow-ups (jobs): the JS twin of lua_stdlib.audit10_followup_jobs.
 * A durable-workflow wait inside db.batch (a sync fn) is refused instead of
 * unwinding the batch and its own record; the reaper keeps a claim one whole
 * second behind and reaps one two seconds behind (vt = 1). */
UTEST(js_stdlib, audit10_followup_jobs)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { jobs } from 'hull:jobs';\n"
        "import { db as dbModule } from 'hull:db';\n"
        "(async () => {\n"
        "  const db = dbModule.default();\n"
        "  jobs.init();\n"
        "  if (db.inTransaction()) return 1;\n"
        "  let inside;\n"
        "  db.batch(() => { inside = db.inTransaction(); });\n"
        "  if (inside !== true) return 2;\n"
        "  let e1, e2;\n"
        "  jobs.workflow('a10wf', async (ctx) => {\n"
        "    try { db.batch(() => { ctx.sleep(60); }); return 'slept in a batch'; }\n"
        "    catch (e) { e1 = String(e && e.message); }\n"
        "    try { db.batch(() => { ctx.waitSignal('go'); }); return 'waited in a batch'; }\n"
        "    catch (e) { e2 = String(e && e.message); }\n"
        "    ctx.sleep(60);\n"
        "    return 'woke';\n"
        "  });\n"
        "  const id = jobs.start('a10wf', {}, { queue: 'a10wf' });\n"
        "  await jobs.work({ queue: 'a10wf' });\n"
        "  if (!(e1 && e1.includes('ctx.sleep cannot wait inside db.batch'))) return 3;\n"
        "  if (!(e2 && e2.includes('ctx.waitSignal cannot wait inside db.batch'))) return 4;\n"
        "  const j = jobs.get(id);\n"
        "  if (!j || j.status !== 'pending') return 5;\n"
        "  const rid = jobs.enqueue('a10vt', {}, { queue: 'a10vt', maxAttempts: 1 });\n"
        "  if (jobs.claim({ queue: 'a10vt', batch: 1 }).length !== 1) return 7;\n"
        "  globalThis.__a10_rid = rid;\n"
        "  return 0;\n"
        "})().then((v) => { globalThis.__test_a10 = v; },\n"
        "          (e) => { console.log(String(e && e.stack || e)); globalThis.__test_a10 = -2; });\n";

    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    ASSERT_EQ(eval_int("typeof globalThis.__test_a10 === 'number' ? globalThis.__test_a10 : -1"), 0);

    const char *reap =
        "import { jobs } from 'hull:jobs';\n"
        "jobs.reap({ visibilityTimeout: 1 });\n"
        "globalThis.__a10_st = jobs.get(globalThis.__a10_rid).status;\n";

    /* One second behind: may be a heartbeat made a moment ago - kept. */
    ASSERT_EQ(sqlite3_exec(test_db,
        "UPDATE _hull_jobs SET claimed_at = CAST(strftime('%s','now') AS INTEGER) - 1 "
        "WHERE queue = 'a10vt'", NULL, NULL, NULL), SQLITE_OK);
    val = JS_Eval(js.ctx, reap, strlen(reap), "<test-reap1>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    EXPECT_EQ(eval_int("globalThis.__a10_st === 'running' ? 0 : 1"), 0);

    /* Two seconds behind: held at least one full second - reaped. */
    ASSERT_EQ(sqlite3_exec(test_db,
        "UPDATE _hull_jobs SET claimed_at = CAST(strftime('%s','now') AS INTEGER) - 2 "
        "WHERE queue = 'a10vt'", NULL, NULL, NULL), SQLITE_OK);
    val = JS_Eval(js.ctx, reap, strlen(reap), "<test-reap2>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    EXPECT_EQ(eval_int("globalThis.__a10_st === 'dead' ? 0 : 1"), 0);

    cleanup_js_caps();
}

/* ── hull:web:middleware:rbac tests ───────────────────────────────────────── */

UTEST(js_stdlib, rbac_init_and_assign)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    const char *code =
        "import { rbac } from 'hull:web:middleware:rbac';\n"
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
        "import { rbac } from 'hull:web:middleware:rbac';\n"
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
        "import { rbac } from 'hull:web:middleware:rbac';\n"
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
        "import { rbac } from 'hull:web:middleware:rbac';\n"
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

    /* hull:validate is a .js stdlib file - gating must intercept. */
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

/* Internal modules (":_" segments) are the stdlib's plumbing - hull:_template
 * compiles strings into code - so once the module set is wired an app module
 * may not import one. */
UTEST(js_runtime, internal_modules_are_stdlib_only)
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
        "import { _template } from 'hull:_template';\n"
        "globalThis.__internal = 1;\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_MODULE);
    int threw = JS_IsException(val);
    if (threw) JS_FreeValue(js.ctx, JS_GetException(js.ctx));
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    EXPECT_TRUE(threw || eval_int("globalThis.__internal === 1 ? 1 : 0") == 0);
    EXPECT_EQ(eval_int("globalThis.__internal === 1 ? 1 : 0"), 0);

    js.base.module_set = NULL;
    cleanup_js();
}

/* ...and before it is wired, too. Static imports all run before the manifest
 * is read, and an internal name is not in the registry, so the import tracker
 * never sees one: a gate that waited for the module set let an app's top-level
 * import of hull:_template (or hull:db:_internal_conn) straight through. A
 * hull: module may still import one. */
UTEST(js_runtime, internal_modules_are_stdlib_only_before_wiring)
{
    init_js();
    ASSERT_TRUE(js.base.module_set == NULL);

    const char *app =
        "import { _template } from 'hull:_template';\n"
        "globalThis.__early = 1;\n";
    JSValue val = JS_Eval(js.ctx, app, strlen(app), "<test>", JS_EVAL_TYPE_MODULE);
    int threw = JS_IsException(val);
    if (threw) JS_FreeValue(js.ctx, JS_GetException(js.ctx));
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    EXPECT_EQ(eval_int("globalThis.__early === 1 ? 1 : 0"), 0);

    const char *lib =
        "import { _template } from 'hull:_template';\n"
        "globalThis.__lib = (typeof _template.compile === 'function') ? 1 : 0;\n";
    val = JS_Eval(js.ctx, lib, strlen(lib), "hull:tests:internal_import",
                  JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    EXPECT_EQ(eval_int("globalThis.__lib|0"), 1);

    cleanup_js();
}

/* hull:web:_request.clientIp is the one place a request's source IP is derived
 * under trust_proxy; four middleware delegate to it. The Lua twin asserts the
 * same string (lua_stdlib.client_ip_matrix), so the two cannot drift. */
UTEST(js_stdlib, client_ip_matrix)
{
    init_js();
    const char *code =
        "import { _request } from 'hull:web:_request';\n"
        "const s = (v) => (v === null || v === undefined) ? '(nil)' : v;\n"
        "const cases = [\n"
        "  [{ headers: {}, remote_addr: '10.0.0.1' }, false],\n"
        "  [{ headers: { 'x-forwarded-for': '1.1.1.1' }, remote_addr: '10.0.0.1' }, false],\n"
        "  [{ headers: { 'x-forwarded-for': 'a, b, c' }, remote_addr: '10.0.0.1' }, true],\n"
        "  [{ headers: { 'x-forwarded-for': ' 1.2.3.4 , x' }, remote_addr: '10.0.0.1' }, true],\n"
        "  [{ headers: {}, remote_addr: '10.0.0.1' }, true],\n"
        "  [{ headers: { 'x-forwarded-for': '' }, remote_addr: '10.0.0.1' }, true],\n"
        "  [{ headers: {} }, false],\n"
        "  [{ headers: {}, remote_addr: 'a'.repeat(100) }, false],\n"
        "];\n"
        "const out = cases.map((c) => s(_request.clientIp(c[0], c[1])));\n"
        "out.push(s(_request.clientIp(null, true)));\n"
        "globalThis.__ip = out.join('|');\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "hull:tests:client_ip",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    char *got = eval_str("globalThis.__ip || ''");
    ASSERT_NE(got, NULL);
    EXPECT_STREQ(got, HL_TEST_CLIENT_IP_MATRIX);
    free(got);
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
    /* Native C modules (hull:crypto, hull:time, ...) now
     * self-gate via hl_js_check_module_declared inside their init
     * callbacks. Undeclared imports throw ReferenceError on first use.
     *
     * Use init_js_bare() so the init callback for hull:crypto hasn't
     * yet been triggered - the gate fires there exactly once per VM. */
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

#ifdef HL_ENABLE_IMAGE
UTEST(js_runtime, import_image_is_a_real_hull_module)
{
    /* image is no longer a global on globalThis - it must be
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
#endif /* HL_ENABLE_IMAGE */

/* ── Async test-runner regression coverage ──────────────────────────
 *
 * Before May 2026 the JS test runner invoked each test body via
 * `JS_Call` and only checked `JS_IsException(ret)`. For async test
 * bodies (`async () => {...}`) JS_Call returns the Promise object
 * immediately - non-exception → silent PASS - without awaiting the
 * body. Every async test "passed" regardless of its assertions.
 *
 * The runner is now Promise-aware: it pumps microtasks (and the
 * async backend tick) until the promise settles, then maps
 * fulfilled→PASS and rejected→FAIL. These tests lock that in.
 *
 * We exercise hl_js_test_run directly with a synthesized test list
 * - no router, no real HTTP. Synchronous and async, passing and
 * failing variants. The async-rejecting case is the one the old
 * runner got wrong; if it ever regresses, it'll trip
 * `js_test_runner.async_rejecting_test_fails` here. */

#include "hull/runtime/test.h"  /* HlTestCaseResult, hl_js_test_run */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_test_runner, sync_passing_test_marked_pass)
{
    init_js();
    KlHttpRouter router;
    KlAllocator alloc = kl_allocator_default();
    kl_http_router_init(&router, &alloc);
    hl_js_test_register(js.ctx, &router, &js);

    /* Sync test that returns undefined - should PASS. */
    JSValue rv = JS_Eval(js.ctx, "test('sync ok', () => {})", 24,
                         "test", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(js.ctx, rv);

    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[4] = {{0}};
    hl_js_test_run(js.ctx, &total, &passed, &failed, NULL, results, 4);

    ASSERT_EQ(total, 1);
    ASSERT_EQ(passed, 1);
    ASSERT_EQ(failed, 0);
    ASSERT_TRUE(results[0].passed);

    kl_http_router_free(&router);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_test_runner, sync_throwing_test_marked_fail)
{
    init_js();
    KlHttpRouter router;
    KlAllocator alloc = kl_allocator_default();
    kl_http_router_init(&router, &alloc);
    hl_js_test_register(js.ctx, &router, &js);

    /* Sync test that throws - should FAIL with the message. */
    const char *src = "test('sync throw', () => { throw new Error('boom') })";
    JSValue rv = JS_Eval(js.ctx, src, strlen(src), "test",
                         JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(js.ctx, rv);

    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[4] = {{0}};
    hl_js_test_run(js.ctx, &total, &passed, &failed, NULL, results, 4);

    ASSERT_EQ(total, 1);
    ASSERT_EQ(passed, 0);
    ASSERT_EQ(failed, 1);
    ASSERT_FALSE(results[0].passed);
    ASSERT_NE(strstr(results[0].error, "boom"), NULL);

    kl_http_router_free(&router);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_test_runner, async_resolving_test_marked_pass)
{
    init_js();
    KlHttpRouter router;
    KlAllocator alloc = kl_allocator_default();
    kl_http_router_init(&router, &alloc);
    hl_js_test_register(js.ctx, &router, &js);

    /* Async test that resolves cleanly - should PASS once the
     * runner awaits the returned promise. */
    const char *src = "test('async ok', async () => { return 1 })";
    JSValue rv = JS_Eval(js.ctx, src, strlen(src), "test",
                         JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(js.ctx, rv);

    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[4] = {{0}};
    hl_js_test_run(js.ctx, &total, &passed, &failed, NULL, results, 4);

    ASSERT_EQ(total, 1);
    ASSERT_EQ(passed, 1);
    ASSERT_EQ(failed, 0);
    ASSERT_TRUE(results[0].passed);

    kl_http_router_free(&router);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_test_runner, async_rejecting_test_fails)
{
    /* THE regression test. Pre-fix this would have shown PASS
     * because JS_Call returned the (rejecting) Promise as a
     * non-exception value and the runner never awaited it. */
    init_js();
    KlHttpRouter router;
    KlAllocator alloc = kl_allocator_default();
    kl_http_router_init(&router, &alloc);
    hl_js_test_register(js.ctx, &router, &js);

    const char *src =
        "test('async fail', async () => { throw new Error('assert failed') })";
    JSValue rv = JS_Eval(js.ctx, src, strlen(src), "test",
                         JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(js.ctx, rv);

    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[4] = {{0}};
    hl_js_test_run(js.ctx, &total, &passed, &failed, NULL, results, 4);

    ASSERT_EQ(total, 1);
    ASSERT_EQ(passed, 0);
    ASSERT_EQ(failed, 1);
    ASSERT_FALSE(results[0].passed);
    ASSERT_NE(strstr(results[0].error, "assert failed"), NULL);

    kl_http_router_free(&router);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_test_runner, mixed_results_in_one_file)
{
    /* Multiple tests, mixed sync/async, mixed pass/fail. The runner
     * must aggregate totals correctly and not let one async failure
     * mask later results. */
    init_js();
    KlHttpRouter router;
    KlAllocator alloc = kl_allocator_default();
    kl_http_router_init(&router, &alloc);
    hl_js_test_register(js.ctx, &router, &js);

    const char *src =
        "test('a sync pass',  () => {});\n"
        "test('b sync fail',  () => { throw new Error('sync') });\n"
        "test('c async pass', async () => { return 'ok' });\n"
        "test('d async fail', async () => { throw new Error('async') });\n";
    JSValue rv = JS_Eval(js.ctx, src, strlen(src), "test",
                         JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(js.ctx, rv);

    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[8] = {{0}};
    hl_js_test_run(js.ctx, &total, &passed, &failed, NULL, results, 8);

    ASSERT_EQ(total,  4);
    ASSERT_EQ(passed, 2);
    ASSERT_EQ(failed, 2);
    ASSERT_TRUE (results[0].passed);  /* a sync pass  */
    ASSERT_FALSE(results[1].passed);  /* b sync fail  */
    ASSERT_TRUE (results[2].passed);  /* c async pass */
    ASSERT_FALSE(results[3].passed);  /* d async fail */
    ASSERT_NE(strstr(results[1].error, "sync"),  NULL);
    ASSERT_NE(strstr(results[3].error, "async"), NULL);

    kl_http_router_free(&router);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* ── JS bytecode cache ──────────────────────────────────────────
 *
 * Mirrors the lua_bytecode_cache.* tests. Uses a stock QuickJS
 * runtime (no Hull host wiring) so the cache helper is the only
 * piece under test. */

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

static int jbc_rm_entry(const char *p, const struct stat *st,
                        int t, struct FTW *f)
{
    (void)st; (void)t; (void)f;
    return remove(p);
}

static void jbc_with_tmp_home(char *tmpdir, size_t n)
{
    if (hl_test_path(tmpdir, n, "hull_jbc_cache_XXXXXX") != 0 || !mkdtemp(tmpdir)) {
        fprintf(stderr, "jbc_with_tmp_home: no usable temp dir\n");
        tmpdir[0] = 0;
        return;
    }
    setenv("HOME", tmpdir, 1);
    unsetenv("HULL_NO_CACHE");
    unsetenv("HULL_NO_JS_BYTECODE_CACHE");
    hl_js_bytecode_cache_reset();
}

static int jbc_count(const char *dir)
{
    char root[512];
    snprintf(root, sizeof(root),
             "%s/.hull/blobs/runtime/js-bytecode/blobs", dir);
    DIR *r = opendir(root);
    if (!r) return 0;
    int n = 0;
    struct dirent *sh;
    while ((sh = readdir(r))) {
        if (sh->d_name[0] == '.') continue;
        char shard[512];
        snprintf(shard, sizeof(shard), "%s/%s", root, sh->d_name);
        DIR *d = opendir(shard);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strlen(e->d_name) == 64) n++;
        }
        closedir(d);
    }
    closedir(r);
    return n;
}

/* A module that comfortably clears the 256-byte minimum cache
 * threshold. Pure ES module syntax - exports a default function
 * that returns a deterministic value we can assert on. */
static const char *JBC_PROBE =
    "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "export default function probe(x) {\n"
    "    return x * 2 + 3;\n"
    "}\n";

UTEST(js_bytecode_cache, miss_then_hit_populates_disk)
{
    char tmp[256];
    jbc_with_tmp_home(tmp, sizeof tmp);

    JSRuntime *rt = JS_NewRuntime();
    ASSERT_NE(rt, NULL);
    JSContext *ctx = JS_NewContext(rt);
    ASSERT_NE(ctx, NULL);

    ASSERT_EQ(0, jbc_count(tmp));

    JSValue v = hl_js_compile_module_cached(ctx, JBC_PROBE,
                                            strlen(JBC_PROBE),
                                            "test:probe");
    ASSERT_FALSE(JS_IsException(v));
    JS_FreeValue(ctx, v);
    ASSERT_EQ(1, jbc_count(tmp));

    /* Second call: cache hit, same key, no extra file. */
    v = hl_js_compile_module_cached(ctx, JBC_PROBE,
                                    strlen(JBC_PROBE),
                                    "test:probe");
    ASSERT_FALSE(JS_IsException(v));
    JS_FreeValue(ctx, v);
    ASSERT_EQ(1, jbc_count(tmp));

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    nftw(tmp, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(js_bytecode_cache, opt_out_via_env_skips_disk)
{
    char tmp[256];
    jbc_with_tmp_home(tmp, sizeof tmp);
    setenv("HULL_NO_JS_BYTECODE_CACHE", "1", 1);
    hl_js_bytecode_cache_reset();

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JSValue v = hl_js_compile_module_cached(ctx, JBC_PROBE,
                                            strlen(JBC_PROBE),
                                            "test:probe");
    ASSERT_FALSE(JS_IsException(v));
    JS_FreeValue(ctx, v);
    ASSERT_EQ_MSG(0, jbc_count(tmp),
                  "no entry written when opted out");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    unsetenv("HULL_NO_JS_BYTECODE_CACHE");
    nftw(tmp, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(js_bytecode_cache, tiny_source_skips_cache)
{
    char tmp[256];
    jbc_with_tmp_home(tmp, sizeof tmp);

    const char *tiny = "export default 1;\n";  /* < 256 bytes */
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JSValue v = hl_js_compile_module_cached(ctx, tiny, strlen(tiny),
                                            "test:tiny");
    ASSERT_FALSE(JS_IsException(v));
    JS_FreeValue(ctx, v);
    ASSERT_EQ_MSG(0, jbc_count(tmp),
                  "tiny modules bypass cache");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    nftw(tmp, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(js_bytecode_cache, module_name_in_key)
{
    /* The cache key folds in module_name because QuickJS bakes the
     * name into the bytecode. Same source under two different
     * names → two distinct entries. */
    char tmp[256];
    jbc_with_tmp_home(tmp, sizeof tmp);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JSValue v1 = hl_js_compile_module_cached(ctx, JBC_PROBE,
                                             strlen(JBC_PROBE),
                                             "test:name_a");
    ASSERT_FALSE(JS_IsException(v1));
    JS_FreeValue(ctx, v1);

    JSValue v2 = hl_js_compile_module_cached(ctx, JBC_PROBE,
                                             strlen(JBC_PROBE),
                                             "test:name_b");
    ASSERT_FALSE(JS_IsException(v2));
    JS_FreeValue(ctx, v2);

    ASSERT_EQ_MSG(2, jbc_count(tmp),
                  "distinct module names produce distinct entries");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    nftw(tmp, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(js_bytecode_cache, parse_error_returns_no_cache_write)
{
    char tmp[256];
    jbc_with_tmp_home(tmp, sizeof tmp);

    const char *bad =
        "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "this is = not = valid JS )( syntax error here\n";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JSValue v = hl_js_compile_module_cached(ctx, bad, strlen(bad),
                                            "test:bad");
    ASSERT_TRUE(JS_IsException(v));
    /* Drain the exception. */
    JSValue exc = JS_GetException(ctx);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, v);
    ASSERT_EQ(0, jbc_count(tmp));

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    nftw(tmp, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* ── JS template cache ──────────────────────────────────────────
 *
 * Mirrors the js_bytecode_cache tests but for the IIFE-returns-fn
 * pattern that stdlib/js/hull/template.js generates. The cache
 * helper here uses JS_EVAL_TYPE_GLOBAL (not MODULE) and caches
 * the post-eval render function (skipping both parse and the
 * IIFE execute on hit). */

static void jtc_with_tmp_home(char *tmpdir, size_t n)
{
    if (hl_test_path(tmpdir, n, "hull_jtc_cache_XXXXXX") != 0 || !mkdtemp(tmpdir)) {
        fprintf(stderr, "jtc_with_tmp_home: no usable temp dir\n");
        tmpdir[0] = 0;
        return;
    }
    setenv("HOME", tmpdir, 1);
    unsetenv("HULL_NO_CACHE");
    unsetenv("HULL_NO_JS_TEMPLATE_CACHE");
    hl_js_template_cache_reset();
}

static int jtc_count(const char *dir)
{
    char root[512];
    snprintf(root, sizeof(root),
             "%s/.hull/blobs/runtime/js-templates/blobs", dir);
    DIR *r = opendir(root);
    if (!r) return 0;
    int n = 0;
    struct dirent *sh;
    while ((sh = readdir(r))) {
        if (sh->d_name[0] == '.') continue;
        char shard[512];
        snprintf(shard, sizeof(shard), "%s/%s", root, sh->d_name);
        DIR *d = opendir(shard);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strlen(e->d_name) == 64) n++;
        }
        closedir(d);
    }
    closedir(r);
    return n;
}

/* Stand-in for what stdlib/js/hull/template.js's compileSource
 * would produce: an IIFE that returns the render function.
 * Padded to comfortably clear the 256-byte minimum. */
static const char *JTC_PROBE =
    "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "(function() {\n"
    "    return function probe(data) {\n"
    "        const x = (data && data.x) || 0;\n"
    "        return String(x * 2 + 3);\n"
    "    };\n"
    "})();\n";

UTEST(js_template_cache, miss_then_hit_populates_disk)
{
    char tmp[256];
    jtc_with_tmp_home(tmp, sizeof tmp);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    ASSERT_EQ(0, jtc_count(tmp));
    JSValue v = hl_js_template_compile_cached(ctx, JTC_PROBE,
                                              strlen(JTC_PROBE),
                                              "=tpl_probe");
    ASSERT_FALSE(JS_IsException(v));

    /* The returned value should be a callable function. */
    ASSERT_TRUE(JS_IsFunction(ctx, v));

    /* Invoke and check the result. */
    JSValue data = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, data, "x", JS_NewInt32(ctx, 7));
    JSValue rv = JS_Call(ctx, v, JS_UNDEFINED, 1, &data);
    ASSERT_FALSE(JS_IsException(rv));
    const char *s = JS_ToCString(ctx, rv);
    ASSERT_STREQ("17", s);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, v);
    ASSERT_EQ(1, jtc_count(tmp));

    /* Second call: cache hit, same key, no extra file. */
    v = hl_js_template_compile_cached(ctx, JTC_PROBE,
                                      strlen(JTC_PROBE), "=tpl_probe");
    ASSERT_FALSE(JS_IsException(v));
    ASSERT_TRUE(JS_IsFunction(ctx, v));
    JS_FreeValue(ctx, v);
    ASSERT_EQ(1, jtc_count(tmp));

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    nftw(tmp, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(js_template_cache, opt_out_via_env_skips_disk)
{
    char tmp[256];
    jtc_with_tmp_home(tmp, sizeof tmp);
    setenv("HULL_NO_JS_TEMPLATE_CACHE", "1", 1);
    hl_js_template_cache_reset();

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JSValue v = hl_js_template_compile_cached(ctx, JTC_PROBE,
                                              strlen(JTC_PROBE),
                                              "=tpl_probe");
    ASSERT_FALSE(JS_IsException(v));
    JS_FreeValue(ctx, v);
    ASSERT_EQ_MSG(0, jtc_count(tmp),
                  "no entry written when opted out");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    unsetenv("HULL_NO_JS_TEMPLATE_CACHE");
    nftw(tmp, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(js_template_cache, parse_error_returns_no_cache_write)
{
    char tmp[256];
    jtc_with_tmp_home(tmp, sizeof tmp);

    const char *bad =
        "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "// pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "this is = not = valid JS )( syntax error\n";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JSValue v = hl_js_template_compile_cached(ctx, bad, strlen(bad),
                                              "=tpl_bad");
    ASSERT_TRUE(JS_IsException(v));
    JSValue exc = JS_GetException(ctx);
    JS_FreeValue(ctx, exc);
    JS_FreeValue(ctx, v);
    ASSERT_EQ(0, jtc_count(tmp));

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    nftw(tmp, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(js_template_cache, name_in_key)
{
    /* Different chunk names → distinct entries (parallel to the
     * js_bytecode_cache.module_name_in_key check). */
    char tmp[256];
    jtc_with_tmp_home(tmp, sizeof tmp);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JSValue v1 = hl_js_template_compile_cached(ctx, JTC_PROBE,
                                               strlen(JTC_PROBE),
                                               "=name_a");
    ASSERT_FALSE(JS_IsException(v1));
    JS_FreeValue(ctx, v1);

    JSValue v2 = hl_js_template_compile_cached(ctx, JTC_PROBE,
                                               strlen(JTC_PROBE),
                                               "=name_b");
    ASSERT_FALSE(JS_IsException(v2));
    JS_FreeValue(ctx, v2);

    ASSERT_EQ_MSG(2, jtc_count(tmp),
                  "distinct chunk names produce distinct entries");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    nftw(tmp, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* ── app.X phase-gate (registration_closed) tests ─────────────────────
 *
 * Mirrors the Lua-side tests in tests/hull/runtime/lua/test_lua.c.
 * Once serve.c sets runtime.registration_closed = 1, all
 * app.{get,post,...,use,usePost,ws,sse,every,daily} bindings must
 * throw a TypeError naming the call and explaining the rule. */

/* Pattern: a module body that calls into a binding is evaluated
 * asynchronously by QuickJS - JS_IsException on the eval value gives
 * promise-pending, not the inner throw.  These tests use a two-step
 * dance instead:
 *   1. eval a module that stashes the `app` import on globalThis;
 *      this completes via hl_js_run_jobs.
 *   2. flip registration_closed.
 *   3. eval a SYNCHRONOUS (JS_EVAL_TYPE_GLOBAL) try/catch that calls
 *      the binding and returns the caught error.toString() (or "" on
 *      success), so we observe the throw via the returned string. */

UTEST(js_runtime, app_get_rejected_after_registration_closed)
{
    init_js();

    const char *setup =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "globalThis.__app = app;\n";
    JSValue v = JS_Eval(js.ctx, setup, strlen(setup),
                        "<reg-closed-get-setup>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    js.base.registration_closed = 1;

    char *err = eval_str(
        "(() => { try {"
        "  globalThis.__app.get('/late', (req, res) => { res.json({}); });"
        "  return '';"
        "} catch (e) { return String(e); } })()");
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "app.get"), NULL);
    ASSERT_NE(strstr(err, "app startup"), NULL);
    free(err);
    cleanup_js();
}

UTEST(js_runtime, app_use_rejected_after_registration_closed)
{
    init_js();
    const char *setup =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "globalThis.__app = app;\n";
    JSValue v = JS_Eval(js.ctx, setup, strlen(setup),
                        "<reg-closed-use-setup>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    js.base.registration_closed = 1;

    char *err = eval_str(
        "(() => { try {"
        "  globalThis.__app.use('*', '/api/*', (req, res) => 0);"
        "  return '';"
        "} catch (e) { return String(e); } })()");
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "app.use"), NULL);
    free(err);
    cleanup_js();
}

UTEST(js_runtime, app_every_rejected_after_registration_closed)
{
    init_js();
    const char *setup =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1', 'hull/timers@1'] });\n"
        "globalThis.__app = app;\n";
    JSValue v = JS_Eval(js.ctx, setup, strlen(setup),
                        "<reg-closed-every-setup>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    js.base.registration_closed = 1;

    char *err = eval_str(
        "(() => { try {"
        "  globalThis.__app.every(5000, () => {});"
        "  return '';"
        "} catch (e) { return String(e); } })()");
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "app.every"), NULL);
    free(err);
    cleanup_js();
}

UTEST(js_runtime, app_get_allowed_before_registration_closed)
{
    /* Sanity: the same call succeeds (returns "") with the flag
     * clear.  Guards against an over-eager gate. */
    init_js();
    ASSERT_EQ(js.base.registration_closed, 0);
    const char *setup =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "globalThis.__app = app;\n";
    JSValue v = JS_Eval(js.ctx, setup, strlen(setup),
                        "<reg-open-allowed>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    char *err = eval_str(
        "(() => { try {"
        "  globalThis.__app.get('/ok', (req, res) => { res.json({}); });"
        "  return '';"
        "} catch (e) { return String(e); } })()");
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "");
    free(err);
    cleanup_js();
}

/* ── Manifest-extraction loader-liveness / death tests ──
 *
 * These exercise hl_js_load_app on the EXTRACTION path (manifest_extract_lenient
 * = 1, the mode `hull build`/`tool.extract_manifest_js` run in). They prove the
 * loader TERMINATES and FAILS CLOSED against adversarial module evaluation - a
 * runaway microtask, a never-resolving top-level await - and that the permissive
 * lenient stub cannot wedge the loader by being mistaken for a thenable. A test
 * merely RETURNING proves the bounded drain terminated (a regression to the
 * unbounded drain would hang the whole suite). Run under `make debug` (ASan),
 * these also assert balanced cleanup on every fail-closed path. */
static int run_lenient_load(const char *src)
{
    char path[HL_TEST_PATH_MAX];
    int fd = hl_test_mkstemp(path, sizeof path, "hull_js_live", NULL);
    if (fd < 0) return -999;
    size_t n = strlen(src);
    ssize_t w = write(fd, src, n);
    close(fd);
    if (w != (ssize_t)n) { unlink(path); return -998; }

    /* Bare init (no install_test_js_globals): matches the transient extractor's
     * minimal global graph. hull:app still resolves via native-module
     * registration in hl_js_init. The full-globals harness carries a larger
     * object graph that independently trips QuickJS's teardown GC on a
     * runtime that ran a microtask runaway. */
    init_js_bare();
    int rc = -997;
    if (js_initialized) {
        js.manifest_extract_lenient = 1;     /* the build/extraction mode */
        rc = hl_js_load_app(&js, path);
        js.manifest_extract_lenient = 0;
    }
    cleanup_js();
    unlink(path);
    return rc;
}

/* A never-resolving top-level await leaves the module-eval promise PENDING with
 * no pending jobs. The extractor must fail closed, not silently treat a
 * half-evaluated module as a clean load. */
UTEST(js_extract_liveness, never_settling_await_is_fatal)
{
    const char *src =
        "import { app } from 'hull:app';\n"
        "await new Promise(() => {});\n"          /* never settles */
        "app.manifest({ modules: [] });\n";       /* unreachable */
    ASSERT_EQ(run_lenient_load(src), -1);
}

/* The endlessly-requeued-microtask case (a runaway that never settles) is
 * covered end to end by tests/e2e_modular_resolution.sh, which asserts a real
 * `hull build` of such an app TERMINATES (a wall-clock guard fails a hang) and
 * fails closed with no binary. It is intentionally NOT a unit test here: any
 * app that runs a microtask runaway and is then torn down trips a PRE-EXISTING
 * QuickJS teardown-GC double-free on the churned promise graph (orthogonal to
 * loader liveness - it fires even with zero jobs drained). A fresh single-shot
 * `hull build` process is robustly stable against it (0/60 under load), but the
 * shared multi-test unit-harness process is not, so pinning it here would be
 * flaky. The bounded-drain + PENDING-fail path it would exercise is the same one
 * never_settling_await_is_fatal already pins deterministically. */

/* The permissive lenient stub (a missing hull:* under extraction) must be
 * non-thenable: awaiting it settles immediately, so the module completes and the
 * load succeeds. If the stub were a thenable that returned itself, `await` would
 * assimilate it forever and the module would never settle - caught here as a -1
 * (or, without the bounded drain, a hang). Expecting 0 pins the invariant. */
UTEST(js_extract_liveness, lenient_stub_proxy_is_non_thenable)
{
    const char *src =
        "import { app } from 'hull:app';\n"
        "import zzz from 'hull:zzz_feature_absent';\n"  /* → lenient stub proxy */
        "const _ = await zzz;\n"                         /* must not hang */
        "app.manifest({ modules: [] });\n";
    ASSERT_EQ(run_lenient_load(src), 0);
}

/* Control: a well-formed extraction load succeeds - the liveness guards do not
 * false-positive a normal app. */
UTEST(js_extract_liveness, valid_lenient_load_succeeds)
{
    const char *src =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: [] });\n";
    ASSERT_EQ(run_lenient_load(src), 0);
}

/* ── db.async submit-failure: the shared last_async_cont ownership fix ──────
 * Forces the ACTUAL JS db.async binding down its pool-submit-failure path via a
 * real, saturated thread pool (queue-saturation - no compiled-in production hook,
 * so there is nothing to strip from shipped objects). The pool is created through
 * the ACTIVE async backend (hl_async_backend() - keel when Keel is composed, as in
 * this test binary; poll on a Keel-free base) so its concrete type MATCHES what the
 * db.async binding submits through. Using a fixed poll pool here while the binding
 * submits via the active (keel) backend is a type confusion: keel_pool_submit reads
 * a poll pool's bytes as `p->kpool` and locks a garbage mutex -> deadlock. Both
 * backends' submit fail-fast (return -1 when the bounded queue is full), so a
 * matched-backend saturated pool yields the deterministic submit failure with no
 * blocking. The binding creates a
 * Promise + continuation and registers it as js->last_async_cont, then the submit
 * fails; the failure cleanup MUST destroy the continuation AND clear
 * last_async_cont so a later dispatch cannot attach a handler promise to freed
 * memory.
 *
 * Asserts the API contract (the call throws / rejects), last_async_cont is
 * cleared, and - run under ASan/LSan - that the continuation + Promise are freed
 * exactly once with no pending driver / pool item leaked.
 *
 * REVERT PROOF: drop `js->last_async_cont = NULL;` from mod_db.c's submit-failure
 * path and the last_async_cont assertion below flips (it stays the dangling freed
 * pointer), and ASan trips on the later use. */
static atomic_int g_dbsat_release;
static atomic_int g_dbsat_running;
static void dbsat_block(void *u) { (void)u; atomic_fetch_add(&g_dbsat_running, 1);
    while (!atomic_load(&g_dbsat_release)) usleep(500); }
static void dbsat_noop(void *u) { (void)u; }

UTEST(js_db_async, submit_failure_clears_last_async_cont)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* The pool MUST be the active backend's concrete type, because the db.async
     * binding submits through hl_async_backend() (keel here). A poll pool passed to
     * keel_pool_submit is a type confusion -> garbage p->kpool -> deadlock. */
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx  *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    HlAsyncBackendPool *pool = NULL;
    ASSERT_EQ(be->pool_create(&pool, actx, 1, 1), 0);   /* 1 worker, queue 1 */
    js.base.thread_pool = pool;
    js.base.async_ctx   = actx;

    /* Saturate: one job occupies the worker, one fills the queue -> the next
     * submit (the db.async op) is rejected fail-fast (-1). */
    atomic_store(&g_dbsat_release, 0);
    atomic_store(&g_dbsat_running, 0);
    ASSERT_EQ(be->pool_submit(pool, dbsat_block, dbsat_noop, dbsat_noop, NULL), 0);
    while (atomic_load(&g_dbsat_running) < 1) usleep(500);   /* worker busy */
    ASSERT_EQ(be->pool_submit(pool, dbsat_block, dbsat_noop, dbsat_noop, NULL), 0); /* queue full */

    js.last_async_cont = NULL;   /* baseline: nothing registered before the call */

    /* Drive the real binding: db is the default connection (install_test_js_globals). */
    const char *src = "db.async.query('SELECT 1');";
    JSValue rv = JS_Eval(js.ctx, src, strlen(src), "dbfail", JS_EVAL_TYPE_GLOBAL);

    /* Contract: the submit failure surfaces as a thrown error (a rejected await
     * in an async function). */
    ASSERT_TRUE(JS_IsException(rv));
    JSValue exc = JS_GetException(js.ctx);
    const char *msg = JS_ToCString(js.ctx, exc);
    ASSERT_TRUE(msg != NULL && strstr(msg, "thread pool") != NULL);   /* stable message */
    JS_FreeCString(js.ctx, msg);
    JS_FreeValue(js.ctx, exc);
    JS_FreeValue(js.ctx, rv);

    /* THE FIX: the destroyed continuation's registry pointer was cleared, so no
     * dangling last_async_cont remains (no handler-promise attach to freed mem). */
    ASSERT_TRUE(js.last_async_cont == NULL);

    /* Release the saturating jobs and tear down. */
    atomic_store(&g_dbsat_release, 1);
    be->pool_free(pool);
    be->free(actx);
    js.base.thread_pool = NULL;
    js.base.async_ctx   = NULL;
    cleanup_js_caps();
}


/* ── hull.map (docs/task_join_design.md) ─────────────────────────────
 *
 * On a real loop: the module sets globalThis.__map to "ok" or to what went
 * wrong, and the test ticks the loop and drains QuickJS jobs until it does.
 * Concurrency is measured with an in-flight counter's peak, not timed. */
static int js_map_case(const char *body, char *out, size_t outsz)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    if (be->init(&actx, NULL) != 0) return 1;
    js.base.async_ctx = actx;

    static const char pre[] =
        "let inflight = 0, peak = 0;\n"
        "async function busy(ms) {\n"
        "  inflight++; if (inflight > peak) peak = inflight;\n"
        "  await hull.sleep(ms);\n"
        "  inflight--;\n"
        "}\n"
        "function check(c, what) { if (!c) throw new Error(what); }\n"
        "(async () => {\n";
    static const char post[] =
        "})().then(() => { globalThis.__map = 'ok'; },\n"
        "          (e) => { globalThis.__map = String(e && e.message || e); });\n";
    size_t n = strlen(pre) + strlen(body) + strlen(post) + 1;
    char *src = malloc(n);
    if (!src) return 2;
    snprintf(src, n, "%s%s%s", pre, body, post);
    JSValue v = JS_Eval(js.ctx, src, strlen(src), "<map>", JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);

    for (int i = 0; i < 400; i++) {
        hl_js_run_jobs(&js);
        JSValue g = JS_GetGlobalObject(js.ctx);
        JSValue m = JS_GetPropertyStr(js.ctx, g, "__map");
        JS_FreeValue(js.ctx, g);
        if (!JS_IsUndefined(m)) {
            const char *c = JS_ToCString(js.ctx, m);
            snprintf(out, outsz, "%s", c ? c : "(unprintable)");
            if (c) JS_FreeCString(js.ctx, c);
            JS_FreeValue(js.ctx, m);
            break;
        }
        JS_FreeValue(js.ctx, m);
        be->tick(actx, 20);
    }
    be->tick(actx, 0);
    be->free(actx);
    js.base.async_ctx = NULL;
    return 0;
}

#define JS_MAP_CASE(name, body)                                  \
    UTEST(js_map, name)                                          \
    {                                                            \
        init_js_with_caps();                                     \
        ASSERT_TRUE(js_initialized);                             \
        char out[512] = "(no verdict)";                          \
        ASSERT_EQ(js_map_case(body, out, sizeof out), 0);        \
        ASSERT_STREQ(out, "ok");                                 \
        cleanup_js_caps();                                       \
    }

JS_MAP_CASE(respects_the_limit_and_keeps_order,
    "  const items = [1,2,3,4,5,6,7,8,9,10];\n"
    "  const r = await hull.map(items, async (x) => { await busy(10 + (11 - x)); return x * x; },\n"
    "                           { limit: 3 });\n"
    "  check(peak === 3, 'peak ' + peak);\n"
    "  for (let i = 0; i < 10; i++) check(r[i] === (i + 1) * (i + 1), 'r[' + i + ']');\n")

JS_MAP_CASE(default_limit_is_16,
    "  const items = Array.from({ length: 40 }, (_, i) => i);\n"
    "  await hull.map(items, () => busy(5));\n"
    "  check(peak === 16, 'peak ' + peak);\n")

JS_MAP_CASE(a_failure_waits_for_the_rest_and_reports_all,
    "  let finished = 0;\n"
    "  const items = [1,2,3,4,5,6,7,8];\n"
    "  try {\n"
    "    await hull.map(items, async (x) => {\n"
    "      await busy(10);\n"
    "      if (x === 2 || x === 5) throw new Error('bad ' + x);\n"
    "      finished++;\n"
    "    }, { limit: 4 });\n"
    "    check(false, 'did not throw');\n"
    "  } catch (e) {\n"
    "    check(e.message === 'bad 2', 'message ' + e.message);\n"
    "    check(e.errors[1].message === 'bad 2' && e.errors[4].message === 'bad 5', 'errors');\n"
    "    check(!(0 in e.errors), 'sparse');\n"
    "  }\n"
    "  check(finished === 6 && inflight === 0, 'finished ' + finished);\n")

JS_MAP_CASE(sync_functions_and_empty_lists,
    "  const r = await hull.map([1, 2], (x, i) => x + i);\n"
    "  check(r[0] === 1 && r[1] === 3, 'sync');\n"
    "  check((await hull.map([], () => 1)).length === 0, 'empty');\n")

JS_MAP_CASE(bad_arguments_are_refused,
    "  for (const bad of [() => hull.map('x', () => 1), () => hull.map([], 7),\n"
    "                     () => hull.map([1], () => 1, { limit: 0 })]) {\n"
    "    let threw = false;\n"
    "    try { await bad(); } catch (e) { threw = true; }\n"
    "    check(threw, 'accepted a bad argument');\n"
    "  }\n"
    "  check((await hull.map([1], () => 2, { limit: Infinity }))[0] === 2, 'Infinity');\n")

JS_MAP_CASE(null_options_mean_the_defaults,
    "  check((await hull.map([1], (x) => x, null))[0] === 1, 'opts null');\n"
    "  check((await hull.map([1], (x) => x, { limit: null }))[0] === 1, 'limit null');\n"
    "  let threw = false;\n"
    "  try { await hull.map([1], (x) => x, 5); } catch (e) { threw = e instanceof TypeError; }\n"
    "  check(threw, 'opts not an object');\n")

JS_MAP_CASE(a_failure_that_cannot_be_printed_is_still_reported,
    "  const odd = Object.create(null);\n"
    "  try {\n"
    "    await hull.map([1, 2], async (x) => { if (x === 1) throw odd; return x; });\n"
    "    check(false, 'did not throw');\n"
    "  } catch (e) {\n"
    "    check(e.message === '(an error)', 'message ' + e.message);\n"
    "    check(e.errors[0] === odd, 'errors');\n"
    "  }\n")

/* Run a module that leaves its verdict (0 = pass, else the failing step) in
 * globalThis.__verdict. */
static int run_verdict_module(const char *code)
{
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    return eval_int("globalThis.__verdict === undefined ? -1 : globalThis.__verdict");
}

UTEST(js_stdlib, crypto_envelope_tag_is_lowercase_only)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    int v = run_verdict_module(
        "import { envelope } from 'hull:crypto:envelope';\n"
        "const secret = 'bb'.repeat(32);\n"
        "const tok = envelope.sign({x: 1}, secret);\n"
        "const dot = tok.indexOf('.');\n"
        "const up = tok.slice(0, dot + 1) + tok.slice(dot + 1).toUpperCase();\n"
        "const r = envelope.verify(up, secret);\n"
        "globalThis.__verdict = up === tok ? 1\n"
        "  : (r[0] !== null || r[1] !== 'bad tag') ? 2\n"
        "  : envelope.verify(tok, secret)[0] === null ? 3 : 0;\n");
    EXPECT_EQ(v, 0);
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(js_stdlib, auth_flows_origin_is_built_from_the_allowlist)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    int v = run_verdict_module(
        AF_INIT_JS
        "authFlows.init(defaults());\n"
        "const o = __testHooks('hull:web:auth-flows').originFor;\n"
        "const req = h => ({ headers: h });\n"
        "let step = 0;\n"
        "if (o(req({host: 'app.example.com:8443'})) !== 'http://app.example.com:8443') step = 1;\n"
        "else if (o(req({host: 'app.example.com:@evil.com'})) !== null) step = 2;\n"
        "else if (o(req({host: 'evil.com/x'})) !== null) step = 3;\n"
        "else if (o(req({host: 'app.example.com', 'x-forwarded-host': 'evil.com'})) !== 'http://app.example.com') step = 4;\n"
        "if (!step) {\n"
        "  const st = __testHooks('hull:web:auth-flows').state;\n"
        "  st.trustRequestHost = false; st.trustedHosts = ['app.example.com'];\n"
        "  if (o(req({host: 'app.example.com:@evil.com'})) !== null) step = 5;\n"
        "  else if (o(req({host: 'app.example.com:8443'})) !== 'https://app.example.com') step = 6;\n"
        "  else if (o(req({host: 'evil.com'})) !== null) step = 7;\n"
        "  else if (o(req({host: 'app.example.com:99999'})) !== null) step = 8;\n"
        "  else {\n"
        "    st.trustedHosts = ['app.example.com:8443'];\n"
        "    if (o(req({host: 'app.example.com:8443'})) !== 'https://app.example.com:8443') step = 10;\n"
        "    else if (o(req({host: 'app.example.com:8444'})) !== null) step = 11;\n"
        "    else if (o(req({host: 'app.example.com'})) !== null) step = 12;\n"
        "    st.trustedHosts = ['app.example.com'];\n"
        "  }\n"
        "  if (!step) {\n"
        "    st.trustProxy = true;\n"
        "    if (o(req({host: 'x', 'x-forwarded-host': 'app.example.com', 'x-forwarded-proto': 'javascript'})) !== 'https://app.example.com') step = 9;\n"
        "  }\n"
        "}\n"
        "globalThis.__verdict = step;\n");
    EXPECT_EQ(v, 0);
    cleanup_js_caps();
}
#endif

UTEST(js_stdlib, jwt_absolute_exp_is_kept)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    int v = run_verdict_module(
        "import { jwt } from 'hull:jwt';\n"
        "const abs = 1900000000;\n"
        "const p = jwt.verify(jwt.sign({ exp: abs }, 'mysecret'), 'mysecret')[0];\n"
        "const r = jwt.verify(jwt.sign({ exp: 3600 }, 'mysecret'), 'mysecret')[0];\n"
        "globalThis.__verdict = (!p || p.exp !== abs) ? 1\n"
        "  : (!r || r.exp < 1000000000 || r.exp > abs) ? 2 : 0;\n");
    EXPECT_EQ(v, 0);
    cleanup_js_caps();
}

UTEST(js_stdlib, template_filter_arg_refuses_backslash)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    int v = run_verdict_module(
        "import { template } from 'hull:template';\n"
        "let msg = '';\n"
        "try { template.renderString('{{ x | default: \"a\\\\\" | default: \"b\" }}', {}); }\n"
        "catch (e) { msg = String(e && e.message); }\n"
        "const ok = template.renderString('{{ x | default: \"b\" }}', {});\n"
        "globalThis.__verdict = !msg.includes('invalid filter argument') ? 1\n"
        "  : ok !== 'b' ? 2 : 0;\n");
    EXPECT_EQ(v, 0);
    cleanup_js_caps();
}

/* ── worker.dispatch VMs ─────────────────────────────────────────────
 *
 * A ONE-thread pool, so consecutive dispatches share a thread: where a
 * reused context leaked state. Same verdict protocol as js_map_case. */
static int js_worker_open(const HlAsyncBackend **be, HlAsyncBackendCtx **actx,
                          HlAsyncBackendPool **pool)
{
    *be = hl_async_backend();
    *actx = NULL;
    *pool = NULL;
    if ((*be)->init(actx, NULL) != 0) return -1;
    if ((*be)->pool_create(pool, *actx, 1, 16) != 0) return -1;
    pending_async_ctx   = *actx;
    pending_thread_pool = *pool;
    init_js_with_caps();
    return js_initialized ? 0 : -1;
}

static void js_worker_run(const HlAsyncBackend *be, HlAsyncBackendCtx *actx,
                          const char *body, char *out, size_t outsz)
{
    static const char pre[] =
        "function check(c, what) { if (!c) throw new Error(what); }\n"
        /* a failed dispatch resolves to { error: msg } */
        "async function fails(fn) {\n"
        "  const r = await worker.dispatch(fn);\n"
        "  return r && typeof r.error === 'string' ? r.error : '(no error)';\n"
        "}\n"
        "(async () => {\n";
    static const char post[] =
        "})().then(() => { globalThis.__wk = 'ok'; },\n"
        "          (e) => { globalThis.__wk = String(e && e.message || e); });\n";
    size_t n = strlen(pre) + strlen(body) + strlen(post) + 1;
    char *src = malloc(n);
    if (!src) return;
    snprintf(src, n, "%s%s%s", pre, body, post);
    JSValue v = JS_Eval(js.ctx, src, strlen(src), "<worker>", JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);

    for (int i = 0; i < 400; i++) {
        hl_js_run_jobs(&js);
        JSValue g = JS_GetGlobalObject(js.ctx);
        JSValue m = JS_GetPropertyStr(js.ctx, g, "__wk");
        JS_FreeValue(js.ctx, g);
        if (!JS_IsUndefined(m)) {
            const char *c = JS_ToCString(js.ctx, m);
            snprintf(out, outsz, "%s", c ? c : "(unprintable)");
            if (c) JS_FreeCString(js.ctx, c);
            JS_FreeValue(js.ctx, m);
            break;
        }
        JS_FreeValue(js.ctx, m);
        be->tick(actx, 20);
    }
}

static void js_worker_close(const HlAsyncBackend *be, HlAsyncBackendCtx *actx,
                            HlAsyncBackendPool *pool)
{
    cleanup_js_caps();
    if (actx) be->tick(actx, 0);
    if (pool) be->pool_free(pool);
    if (actx) be->free(actx);
}

#define JS_WORKER_CASE(name, setup, body)                        \
    UTEST(js_worker, name)                                       \
    {                                                            \
        const HlAsyncBackend *be;                                \
        HlAsyncBackendCtx *actx;                                 \
        HlAsyncBackendPool *pool;                                \
        ASSERT_EQ(js_worker_open(&be, &actx, &pool), 0);         \
        setup;                                                   \
        char out[512] = "(no verdict)";                          \
        js_worker_run(be, actx, body, out, sizeof out);          \
        EXPECT_STREQ(out, "ok");                                 \
        js_worker_close(be, actx, pool);                         \
    }

/* Audit 14 G1: a db.async and a worker.dispatch nobody awaited, still queued
 * behind a busy worker when the pool is freed. Their cancel_fn freed the op
 * and the ctx but never the continuation, so its resolving functions stayed
 * referenced and JS_FreeRuntime aborted (exit 134). Now the cancel_fn
 * releases it, and stop_async finds nothing left. */
static void *release_dbsat_later(void *u)
{
    (void)u;
    usleep(50000);   /* after pool_free has set shutdown */
    atomic_store(&g_dbsat_release, 1);
    return NULL;
}

UTEST(js_worker, unawaited_queued_ops_are_released_at_teardown)
{
    const HlAsyncBackend *be;
    HlAsyncBackendCtx *actx;
    HlAsyncBackendPool *pool;
    ASSERT_EQ(js_worker_open(&be, &actx, &pool), 0);

    atomic_store(&g_dbsat_release, 0);
    atomic_store(&g_dbsat_running, 0);
    ASSERT_EQ(be->pool_submit(pool, dbsat_block, dbsat_noop, dbsat_noop, NULL), 0);
    while (atomic_load(&g_dbsat_running) < 1) usleep(500);   /* worker busy */

    const char *src = "db.async.query('SELECT 1'); worker.dispatch(() => 1);";
    JSValue rv = JS_Eval(js.ctx, src, strlen(src), "<unawaited>", JS_EVAL_TYPE_GLOBAL);
    EXPECT_FALSE(JS_IsException(rv));
    JS_FreeValue(js.ctx, rv);
    hl_js_run_jobs(&js);

    pthread_t releaser;
    ASSERT_EQ(pthread_create(&releaser, NULL, release_dbsat_later, NULL), 0);
    js.base.thread_pool = NULL;
    be->pool_free(pool);             /* both ops still queued: cancel_fn */
    pthread_join(releaser, NULL);
    hl_js_vtable.stop_async(&js.base);
    EXPECT_TRUE(js.base.async_live == NULL);
    EXPECT_EQ(js.base.shutting_down, 1);
    cleanup_js_caps();               /* aborted here on a leaked continuation */
    be->free(actx);
}

JS_WORKER_CASE(a_dispatch_does_not_see_what_the_last_one_left, (void)0,
    "  await worker.dispatch(() => { globalThis.LEAK = 42;\n"
    "                                Array.prototype.leak = 1; return 0; });\n"
    "  const r = await worker.dispatch(() => typeof LEAK + ',' + typeof [].leak);\n"
    "  check(r === 'undefined,undefined', r);\n")

#ifdef HL_ENABLE_IMAGE
/* Audit 9 M2: an image's pixels were plain malloc (decoded ones stb's),
 * outside JS_SetMemoryLimit - an app could hold any number of 256 MB images.
 * They now come from js_malloc_rt: counted, refused past the limit, and
 * handed back when the image is closed. */
static size_t js_test_malloc_size(void)
{
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(js.rt, &u);
    return (size_t)u.malloc_size;
}

UTEST(js_runtime, image_pixels_count_against_the_heap_limit)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    if (eval_int("typeof image === 'object' && image !== null ? 1 : 0") != 1) {
        cleanup_js();   /* image compiled out */
        return;
    }
    ASSERT_EQ(eval_int("globalThis.px = new ArrayBuffer(1 << 20); 1"), 1);

    size_t base = js_test_malloc_size();
    ASSERT_EQ(eval_int("globalThis.img = image.new(1024, 1024, 'r8', px); 1"), 1);
    EXPECT_GE(js_test_malloc_size(), base + (1u << 20));
    ASSERT_EQ(eval_int("img.close(); globalThis.img = null; 1"), 1);
    EXPECT_LT(js_test_malloc_size(), base + (512u << 10));

    JSMemoryUsage u;
    JS_ComputeMemoryUsage(js.rt, &u);
    JS_SetMemoryLimit(js.rt, (size_t)u.malloc_size + (512u << 10));
    char *r = eval_str("(() => { try { image.new(1024, 1024, 'r8', px); "
                       "return 'made'; } catch (e) { return 'refused'; } })()");
    JS_SetMemoryLimit(js.rt, (size_t)u.malloc_limit);
    EXPECT_STREQ(r ? r : "(null)", "refused");
    free(r);
    cleanup_js();
}

/* Audit 18 L7: stb_image_write works off the VM heap, a few times the image
 * at its peak, and nothing refused an encode before the work. An image whose
 * encode input is larger than the heap limit is refused with
 * image_too_large (here the limit Hull checks is lowered under a 1 MB
 * image); under the limit it encodes. */
UTEST(js_audit18, image_encode_refused_past_the_heap_limit)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    if (eval_int("typeof image === 'object' && image !== null ? 1 : 0") != 1) {
        cleanup_js();   /* image compiled out */
        return;
    }
    ASSERT_EQ(eval_int("globalThis.img = image.new(1024, 1024, 'r8',"
                       " new ArrayBuffer(1 << 20)); 1"), 1);
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 512u << 10;
    char *r = eval_str("(() => { try { image.encode(img, 'png'); "
                       "return 'encoded'; } catch (e) { return String(e.message); } })()");
    js.max_heap_bytes = saved;
    EXPECT_TRUE(r && strstr(r, "image_too_large") != NULL);
    if (r && !strstr(r, "image_too_large")) printf("  got: %s\n", r);
    free(r);
    EXPECT_EQ(eval_int("image.encode(img, 'png').byteLength > 0 ? 1 : 0"), 1);
    ASSERT_EQ(eval_int("img.close(); 1"), 1);
    cleanup_js();
}
#endif /* HL_ENABLE_IMAGE */

JS_WORKER_CASE(the_function_s_own_toString_is_not_what_runs, (void)0,
    /* An overridden toString was compiled in the worker: an eval. */
    "  const f = Object.assign(() => 'real',\n"
    "                          { toString: () => \"() => 'injected'\" });\n"
    "  const r = await worker.dispatch(f);\n"
    "  check(r === 'real', 'ran ' + r);\n")

JS_WORKER_CASE(a_runaway_dispatch_is_stopped, js.max_instructions = 1000000,   /* polls weigh 10000 each */
    "  const m = await fails(() => { for (;;) {} });\n"
    "  check(m.includes('interrupted'), m);\n")

JS_WORKER_CASE(a_dispatch_is_held_to_the_heap_limit,
    js.max_heap_bytes = 16u << 20,
    "  const m = await fails(() => {\n"
    "    const a = []; for (let i = 0; i < 64; i++) a.push('x'.repeat(1 << 20) + i);\n"
    "    return a.length; });\n"
    /* At the limit QuickJS may not manage even the out-of-memory error
     * object and throws null, so assert the failure, not its text. */
    "  check(m !== '(no error)', 'no limit');\n")

JS_WORKER_CASE(db_is_absent_unless_declared, (void)0,
    "  const r = await worker.dispatch(() => typeof db);\n"
    "  check(r === 'undefined', r);\n")

/* ── audit 3: runtime fixes ──────────────────────────────────────────── */

UTEST(js_audit3, verify_password_refuses_an_absurd_iteration_count)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { crypto } from 'hull:crypto';\n"
        "const h = 'pbkdf2:2000000000:' + '00'.repeat(16) + ':' + '00'.repeat(32);\n"
        "globalThis.__a3_vp = crypto.verifyPassword('x', h) ? 1 : 0;\n";
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    EXPECT_EQ(eval_int("globalThis.__a3_vp"), 0);
    EXPECT_LT((double)(t1.tv_sec - t0.tv_sec), 5.0);
    cleanup_js_caps();
}


/* db.async rows were built differently from db.query rows: a BLOB came back
 * as a (lossy) string, and columns were SET, so a column named __proto__
 * replaced the row's prototype instead of becoming a column. */
/* A file, not ":memory:": the worker keeps the DSN pointer, and on Windows
 * cosmo's realpath turns ":memory:" into a path it cannot open. */
static char a3_worker_dsn[HL_TEST_PATH_MAX + 16];
static void a3_worker_db(void)
{
    char dir[HL_TEST_PATH_MAX];
    if (!hl_test_mkdtemp(dir, sizeof dir, "hull_a3db")) return;
    snprintf(a3_worker_dsn, sizeof a3_worker_dsn, "%s/w.db", dir);
    hl_worker_db_init(a3_worker_dsn);
}

JS_WORKER_CASE(db_async_rows_are_built_like_db_query_rows,
    a3_worker_db(),
    "  const rows = await db.async.query(\n"
    "    \"SELECT X'00FF10' AS b, 7 AS \\\"__proto__\\\"\");\n"
    "  const r = rows[0];\n"
    "  check(r.b instanceof ArrayBuffer && r.b.byteLength === 3 &&\n"
    "        new Uint8Array(r.b)[1] === 0xFF, 'blob came back as ' + typeof r.b);\n"
    "  check(Object.getPrototypeOf(r) === Object.prototype, 'prototype replaced');\n"
    "  const d = Object.getOwnPropertyDescriptor(r, '__proto__');\n"
    "  check(d && d.value === 7, 'no own __proto__ column');\n")

/* Audit 7 M4: a transaction a job left open stayed open on the worker
 * thread's pooled connection (write lock held, later jobs inside it, rolled
 * back with their writes by the next db.async op). It ends with the job. */
static HlResolvedModuleSet a7_worker_set;
static void a7_worker_db(void)
{
    a3_worker_db();
    /* The dispatch gives the worker VM `db` only for an app declaring it. */
    hl_module_set_clear(&a7_worker_set);
    static const char *const mods[] = { "db", "worker" };
    for (size_t i = 0; i < 2; i++) {
        int idx = hl_module_registry_index(hl_module_registry_find_short(mods[i]));
        if (idx < 0) return;
        a7_worker_set.bits[idx / 64] |= (uint64_t)1 << (idx % 64);
    }
    js.base.module_set = &a7_worker_set;
}

JS_WORKER_CASE(a_transaction_does_not_outlive_its_job,
    a7_worker_db(),
    "  await worker.dispatch(() => { db.exec('CREATE TABLE a7 (x INTEGER)'); return 0; });\n"
    "  const m1 = await fails(() => {\n"
    "    db.exec('BEGIN'); db.exec('INSERT INTO a7 VALUES (1)'); return 1; });\n"
    "  check(m1.includes('cannot outlive a worker.dispatch job'), m1);\n"
    "  const m2 = await fails(() => {\n"
    "    db.exec('BEGIN'); db.exec('INSERT INTO a7 VALUES (2)'); throw new Error('boom'); });\n"
    "  check(m2.includes('boom'), m2);\n"
    /* BEGIN fails inside an open transaction */
    "  const n = await worker.dispatch(() => { db.exec('BEGIN'); db.exec('COMMIT');\n"
    "    return db.query('SELECT count(*) AS n FROM a7')[0].n; });\n"
    "  check(n === 0, 'count ' + JSON.stringify(n));\n")

/* Audit 8 c_db M2: the worker db.batch ran raw BEGIN / COMMIT, so a nested
 * batch was no savepoint (SQLite refused it; Postgres / MySQL committed the
 * outer batch's writes early) and a throwing outer batch stayed committed.
 * It now runs the event loop's batch machinery. */
JS_WORKER_CASE(a_nested_batch_is_a_savepoint,
    a7_worker_db(),
    "  await worker.dispatch(() => { db.exec('CREATE TABLE a8b (x INTEGER)'); return 0; });\n"
    "  const m = await fails(() => db.batch(() => {\n"
    "    db.exec('INSERT INTO a8b VALUES (1)');\n"
    "    db.batch(() => db.exec('INSERT INTO a8b VALUES (2)'));\n"
    "    db.exec('INSERT INTO a8b VALUES (3)');\n"
    "    throw new Error('outer'); }));\n"
    "  check(m.includes('outer'), m);\n"
    "  const r = await worker.dispatch(() => {\n"
    "    db.batch(() => {\n"
    "      db.exec('INSERT INTO a8b VALUES (10)');\n"
    "      db.batch(() => db.exec('INSERT INTO a8b VALUES (11)'));\n"
    "      try { db.batch(() => { db.exec('INSERT INTO a8b VALUES (12)');\n"
    "                             throw new Error('inner'); }); } catch (e) {}\n"
    "      db.exec('INSERT INTO a8b VALUES (13)'); });\n"
    "    const row = db.query('SELECT count(*) AS n, sum(x) AS s FROM a8b')[0];\n"
    "    return row.n + ',' + row.s; });\n"
    "  check(r === '3,34', 'rows ' + JSON.stringify(r));\n")

/* Audit 10: a db call that failed because the dispatch went over its budget
 * threw a catchable InternalError, and the dispatch's own catch went on. It
 * throws the uncatchable interrupt, from a query, an exec and a batch. */
static void a10_worker_db_limited(void)
{
    a7_worker_db();
    js.max_instructions = 2000000;   /* small: three runaway queries must fit the harness wait under MSan */
    hl_js_budget_arm(&js);
}

JS_WORKER_CASE(a_db_call_over_the_limit_is_not_catchable,
    a10_worker_db_limited(),
    /* the dispatched fn runs in its own VM: no closure over the SQL */
    "  const m1 = await fails(() => {\n"
    "    try { db.query('WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) AS n FROM c'); }\n"
    "    catch (e) { return 'caught: ' + e; } return 'ran'; });\n"
    "  check(m1.includes('instruction limit'), 'query: ' + m1);\n"
    "  const m2 = await fails(() => {\n"
    "    try { db.exec('CREATE TABLE IF NOT EXISTS a10 AS WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) AS n FROM c'); }\n"
    "    catch (e) { return 'caught: ' + e; } return 'ran'; });\n"
    "  check(m2.includes('instruction limit'), 'exec: ' + m2);\n"
    "  const m3 = await fails(() => {\n"
    "    try { db.batch(() => db.query('WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) AS n FROM c')); }\n"
    "    catch (e) { return 'caught: ' + e; } return 'ran'; });\n"
    "  check(m3.includes('instruction limit'), 'batch: ' + m3);\n")

/* ── Audit 4: the JS runtime ──────────────────────────────────────────── */

/* Run a module in the caps-bearing context; 0 on success. */
static int a4_module(const char *code)
{
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    int bad = JS_IsException(val);
    if (bad) hl_js_dump_error(&js);
    hl_js_run_jobs(&js);
    /* Module evaluation returns a promise: a throw rejects it. */
    if (!bad && JS_IsObject(val) &&
        JS_PromiseState(js.ctx, val) == JS_PROMISE_REJECTED) {
        JSValue err = JS_PromiseResult(js.ctx, val);
        const char *m = JS_ToCString(js.ctx, err);
        fprintf(stderr, "module rejected: %s\n", m ? m : "?");
        if (m) JS_FreeCString(js.ctx, m);
        JS_FreeValue(js.ctx, err);
        bad = 1;
    }
    JS_FreeValue(js.ctx, val);
    return bad ? -1 : 0;
}

/* The limit counts what it says: QuickJS polls the handler once per 10000
 * steps, and each poll used to count as one - a 1M limit allowed ~10^10. */
UTEST(js_audit4, instruction_limit_is_not_ten_thousand_times_weaker)
{
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    cfg.max_instructions = 1000000;
    HlJS lim;
    memset(&lim, 0, sizeof lim);
    ASSERT_EQ(hl_js_init(&lim, &cfg), 0);
    const char *code = "let i = 0; while (i < 50000000) i++; i";
    JSValue v = JS_Eval(lim.ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_GLOBAL);
    EXPECT_TRUE(JS_IsException(v));
    JS_FreeValue(lim.ctx, v);
    JS_FreeValue(lim.ctx, JS_GetException(lim.ctx));
    hl_js_free(&lim);
}

/* A UDF querying its own connection: the statement cache evicted (or reset)
 * the statement still being stepped. Refused now, with a clear error. */
UTEST(js_audit4, udf_cannot_query_its_own_connection)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a4_module(
        "import { db as dbMod } from 'hull:db';\n"
        "const c = dbMod.default();\n"
        "c.exec('CREATE TABLE a4u (x INTEGER)');\n"
        "c.exec('INSERT INTO a4u VALUES (1), (2)');\n"
        "c.udf.register('hull_a4f', () => {\n"
        "  for (let i = 0; i < 40; i++) c.query('SELECT ' + i);\n"
        "  return 1;\n"
        "});\n"
        "let msg = 'no error';\n"
        "try { c.query('SELECT hull_a4f() FROM a4u'); } catch (e) { msg = String(e && e.message || e); }\n"
        "globalThis.__a4_udf = msg;\n"
        "globalThis.__a4_after = c.query('SELECT count(*) AS n FROM a4u')[0].n;\n"
        /* Before teardown: SQLite releases the function after the runtime. */
        "c.udf.unregister('hull_a4f');\n"), 0);
    char *msg = eval_str("globalThis.__a4_udf");
    ASSERT_NE(msg, NULL);
    EXPECT_NE(strstr(msg, "own connection"), NULL);
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__a4_after"), 2);   /* the connection still works */
    cleanup_js_caps();
}

/* A params element whose getter throws fails the call; it was bound NULL. */
UTEST(js_audit4, db_param_getter_that_throws_fails_the_call)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a4_module(
        "import { db as dbMod } from 'hull:db';\n"
        "const c = dbMod.default();\n"
        "const p = [1];\n"
        "Object.defineProperty(p, 0, { get() { throw new Error('boom'); } });\n"
        "let threw = 0;\n"
        "try { c.query('SELECT ? AS v', p); } catch (e) { threw = 1; }\n"
        "let big = 0;\n"
        "const sparse = []; sparse.length = 1e9;\n"
        "try { c.query('SELECT 1', sparse); } catch (e) { big = 1; }\n"
        "globalThis.__a4_p = threw * 10 + big;\n"), 0);
    EXPECT_EQ(eval_int("globalThis.__a4_p"), 11);
    cleanup_js_caps();
}

/* A typed array is stored as its bytes by the buffer protocol, and a
 * failed ArrayBuffer probe leaves no exception behind. */
UTEST(js_audit4, buffer_probe_leaves_no_pending_exception)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a4_module(
        "import { crypto } from 'hull:crypto';\n"
        "const a = new Uint8Array(crypto.sha256(new Uint8Array([97, 98, 99])));\n"
        "const b = new Uint8Array(crypto.sha256('abc'));\n"
        "let same = a.length === b.length;\n"
        "for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) same = false;\n"
        "globalThis.__a4_buf = same ? 1 : 0;\n"), 0);
    EXPECT_EQ(eval_int("globalThis.__a4_buf"), 1);
    cleanup_js_caps();
}

/* ── Audit 5: the JS runtime ──────────────────────────────────────────── */

/* Evaluate a module under @p name; 0 when it ran, -1 when it threw or its
 * evaluation promise rejected. *msg gets the error text (caller frees). */
static int a5_module_named(const char *name, const char *code, char **msg)
{
    if (msg) *msg = NULL;
    JSValue val = JS_Eval(js.ctx, code, strlen(code), name, JS_EVAL_TYPE_MODULE);
    JSValue err = JS_UNDEFINED;
    int bad = 0;
    if (JS_IsException(val)) {
        bad = 1;
        err = JS_GetException(js.ctx);
    } else {
        hl_js_run_jobs(&js);
        /* Pending counts too: an interrupted run settles nothing. */
        int st = JS_IsObject(val) ? (int)JS_PromiseState(js.ctx, val) : -1;
        if (st == JS_PROMISE_REJECTED) {
            bad = 1;
            err = JS_PromiseResult(js.ctx, val);
        } else if (st == JS_PROMISE_PENDING) {
            bad = 1;
        }
    }
    if (bad && msg) {
        const char *m = JS_ToCString(js.ctx, err);
        *msg = strdup(m ? m : "?");
        if (m) JS_FreeCString(js.ctx, m);
    }
    JS_FreeValue(js.ctx, err);
    JS_FreeValue(js.ctx, val);
    return bad ? -1 : 0;
}

/* H1: a relative specifier normalized to a bare "hull:..." name, which
 * QuickJS then found among the native modules - the stdlib's internal
 * hull:_template / hull:db:_internal_conn - past the ':_' rule that only
 * looked at a raw "hull:" specifier. */
UTEST(js_audit5, relative_import_cannot_reach_internal_modules)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    static const struct { const char *importer, *spec; } cases[] = {
        { "./app.js",        "./hull:_template" },
        { "routes/x.js",     "./../hull:db:_internal_conn" },
        { "./app.js",        "./hull:kv:_native" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char code[256];
        snprintf(code, sizeof code,
                 "import * as m from '%s';\nglobalThis.__a5_h1 = 1;\n",
                 cases[i].spec);
        char *msg = NULL;
        EXPECT_EQ(a5_module_named(cases[i].importer, code, &msg), -1);
        EXPECT_TRUE(msg && strstr(msg, "invalid module path"));
        free(msg);
    }
    /* Dynamic import resolves against the calling script the same way. */
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("./dyn.js",
        "await import('./hull:_template');\n", &msg), -1);
    EXPECT_TRUE(msg && strstr(msg, "invalid module path"));
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__a5_h1 === undefined ? 1 : 0"), 1);
    cleanup_js_caps();
}

/* An HlJS with a small budget (polls weigh 10000 each). */
static int a5_limited(HlJS *lim, int64_t max)
{
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    cfg.max_instructions = max;
    memset(lim, 0, sizeof *lim);
    return hl_js_init(lim, &cfg);
}

static int a5_eval_throws(HlJS *lim, const char *code)
{
    JSValue v = JS_Eval(lim->ctx, code, strlen(code), "<test>",
                        JS_EVAL_TYPE_GLOBAL);
    int threw = JS_IsException(v);
    JS_FreeValue(lim->ctx, v);
    JS_FreeValue(lim->ctx, JS_GetException(lim->ctx));
    return threw;
}

/* H2: an async function body turns the interrupt into a rejection and its
 * caller carried on - polled 10000 steps later, inside the next call of the
 * same function, forever. Guard for QuickJS HULL PATCH 0003: without it
 * this test never returns. */
UTEST(js_audit5, async_bodies_do_not_escape_the_instruction_limit)
{
    HlJS lim;
    ASSERT_EQ(a5_limited(&lim, 1000000), 0);
    EXPECT_TRUE(a5_eval_throws(&lim,
        "const burn = async () => { for (;;) {} };\n"
        "for (;;) burn();\n"));
    EXPECT_EQ(lim.budget_tripped, 1);

    /* Sticky until the next entry re-arms it: then the VM works again. */
    EXPECT_TRUE(a5_eval_throws(&lim, "1 + 1"));
    hl_js_reset_request(&lim);
    EXPECT_EQ(lim.budget_tripped, 0);
    EXPECT_FALSE(a5_eval_throws(&lim, "1 + 1"));
    hl_js_free(&lim);
}

/* Audit 6 H5: catastrophic regexp backtracking runs inside one native
 * RegExp.prototype.exec call, where the bytecode poll never fires. QuickJS
 * 2025-04+ polls the interrupt handler from the backtracking loop
 * (lre_check_timeout); a tripped budget aborts the match, uncatchably, and
 * the trip is sticky afterwards (HULL PATCH 0003 in JS_ThrowInterrupted). */
UTEST(js_audit6, catastrophic_regexp_trips_the_instruction_limit)
{
    HlJS lim;
    ASSERT_EQ(a5_limited(&lim, 1000000), 0);
    EXPECT_TRUE(a5_eval_throws(&lim,
        "let caught = 0;\n"
        "try { /^(a+)+$/.test('a'.repeat(40) + '!'); } catch (e) { caught = 1; }\n"
        "globalThis.after = caught;\n"));
    EXPECT_EQ(lim.budget_tripped, 1);
    hl_js_reset_request(&lim);
    JSValue v = JS_Eval(lim.ctx, "globalThis.after", 16, "<t>", JS_EVAL_TYPE_GLOBAL);
    EXPECT_TRUE(JS_IsUndefined(v));   /* the catch never ran */
    JS_FreeValue(lim.ctx, v);
    /* An ordinary regexp still works once re-armed. */
    EXPECT_FALSE(a5_eval_throws(&lim, "if (!/^(a+)+$/.test('aaa')) throw 1;"));
    hl_js_free(&lim);
}

/* H2 / L1: the same through promise jobs - a loop that catches the
 * rejection of each interrupted call and starts another. The drain ends,
 * the tripped run's jobs are discarded, and nothing is left pending. */
UTEST(js_audit5, a_tripped_run_cannot_continue_through_promise_jobs)
{
    HlJS lim;
    ASSERT_EQ(a5_limited(&lim, 1000000), 0);
    /* Nothing trips while the script runs: the loop lives in the jobs. */
    EXPECT_FALSE(a5_eval_throws(&lim,
        "globalThis.after = 0;\n"
        "(async () => {\n"
        "  await null;\n"
        "  for (;;) { try { await (async () => { for (;;) {} })(); } catch (e) {} }\n"
        "})();\n"
        "Promise.resolve().then(() => null).then(() => { globalThis.after = 1; });\n"));
    hl_js_run_jobs(&lim);
    EXPECT_EQ(lim.budget_tripped, 1);
    EXPECT_FALSE(JS_IsJobPending(lim.rt));
    hl_js_reset_request(&lim);
    JSValue v = JS_Eval(lim.ctx, "globalThis.after", 16, "<t>", JS_EVAL_TYPE_GLOBAL);
    int32_t after = -1;
    JS_ToInt32(lim.ctx, &after, v);
    JS_FreeValue(lim.ctx, v);
    EXPECT_EQ(after, 0);   /* the discarded job never ran */
    hl_js_free(&lim);
}

/* H2: a UDF that hits the limit made conn.query throw an ordinary, catchable
 * SQL error - `for (;;) try { conn.query('SELECT spin()') } catch {}` held
 * the event loop for good. The interrupt is re-raised, uncatchable. */
UTEST(js_audit5, udf_interrupt_is_not_a_catchable_sql_error)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    js.max_instructions = 1000000;
    hl_js_reset_request(&js);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "import { db as dbMod } from 'hull:db';\n"
        "const c = dbMod.default();\n"
        "c.udf.register('hull_a5_spin', () => { for (;;) {} });\n"
        "let caught = 0;\n"
        "for (let i = 0; i < 1000000; i++) {\n"
        "  try { c.query('SELECT hull_a5_spin()'); }\n"
        "  catch (e) { globalThis.__a5_caught = 1; caught++; }\n"
        "}\n"
        "globalThis.__a5_udf = caught;\n", &msg), -1);
    free(msg);
    EXPECT_EQ(js.budget_tripped, 1);
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int("globalThis.__a5_udf === undefined ? 1 : 0"), 1);
    /* Not even the catch block ran: the query re-raised the interrupt. */
    EXPECT_EQ(eval_int("globalThis.__a5_caught === undefined ? 1 : 0"), 1);
    EXPECT_EQ(eval_int("(() => { const c = globalThis.db; "
                       "c.udf.unregister('hull_a5_spin'); return 1; })()"), 1);
    cleanup_js_caps();
}

#ifdef HL_ENABLE_HTTP_SERVER
/* Register `handler_src` as middleware and dispatch it. */
static int a5_middleware(const char *handler_src, KlHttpRequest *req,
                         KlHttpResponse *res)
{
    char code[1024];
    snprintf(code, sizeof code,
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', %s);\n", handler_src);
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    int id = eval_int("globalThis.__hull_internals.__hull_middleware["
                      "globalThis.__hull_internals.__hull_middleware.length - 1].handler_id");
    return hl_js_dispatch_middleware(&js, id, req, res);
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* H3: an async middleware's Promise coerced to 0, "continue" - an auth
 * middleware that awaited its check let every request through. */
UTEST(js_audit5, async_middleware_fails_closed)
{
    static const char *const srcs[] = {
        "async (req, res) => { res.status(401); return 1; }",
        "(req, res) => ({ then(ok) { ok(1); } })",
        "(req, res) => Promise.resolve(0)",
    };
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        KlHttpRequest req = {0};
        KlHttpResponse res = {0};
        EXPECT_EQ(a5_middleware(srcs[i], &req, &res), -1);
        free_req_ctx(&req);
        cleanup_js();
    }
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* Audit 17: a middleware's number is truncated toward zero, sign kept, as
 * the Lua runtime reads it. ToInt32 wrapped 2^32 to 0 ("continue"), and a
 * valueOf that threw continued too. */
UTEST(js_audit17, middleware_number_return_matches_lua)
{
    static const struct { const char *src; int want; } cases[] = {
        { "(req, res) => 1.5", 1 },         { "(req, res) => 0.5", 0 },
        { "(req, res) => -0.5", 0 },        { "(req, res) => -1.5", -1 },
        { "(req, res) => 2 ** 32", 1 },     { "(req, res) => -(2 ** 32)", -1 },
        { "(req, res) => NaN", 0 },         { "(req, res) => Infinity", 1 },
        { "(req, res) => '1.5'", 1 },       { "(req, res) => undefined", 0 },
        { "(req, res) => true", 1 },        { "(req, res) => 2", 1 },
        { "(req, res) => ({ valueOf() { throw new Error('x'); } })", -1 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        KlHttpRequest req = {0};
        KlHttpResponse res = {0};
        EXPECT_EQ_MSG(a5_middleware(cases[i].src, &req, &res), cases[i].want,
                      cases[i].src);
        free_req_ctx(&req);
        cleanup_js();
    }
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* H3: a Hull async op in middleware suspended the connection Keel then ran
 * the rest of the chain and the handler on. Refused, with a clear error. */
UTEST(js_audit5, async_op_in_middleware_is_refused)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    init_js();
    ASSERT_TRUE(js_initialized);
    js.base.async_ctx = actx;
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => {\n"
        "  try { hull.sleep(5); globalThis.__a5_mw = 'slept'; }\n"
        "  catch (e) { globalThis.__a5_mw = String(e.message); }\n"
        "  return 0; }", &req, &res), 0);
    char *m = eval_str("globalThis.__a5_mw");
    EXPECT_TRUE(m && strstr(m, "cannot run in middleware"));
    free(m);
    EXPECT_EQ(js.in_middleware, 0);
    free_req_ctx(&req);
    cleanup_js();
    be->tick(actx, 0);
    be->free(actx);
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* M1: an async op cannot start while req.multipart() is parked for more
 * body on the same request (each takes over the connection's state), and
 * the gate is what the ops consult. */
UTEST(js_audit5, async_op_is_refused_while_a_multipart_read_is_parked)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    init_js();
    ASSERT_TRUE(js_initialized);
    js.base.async_ctx = actx;
    HlReqLife *life = hl_req_life_new();
    ASSERT_TRUE(life != NULL);
    life->parked = 1;
    int dummy_conn;
    js.active_conn = (KlHttpConn *)(void *)&dummy_conn;
    js.active_life = life;
    char *m = eval_str("(() => { try { hull.sleep(5); return 'slept'; }"
                       " catch (e) { return String(e.message); } })()");
    js.active_conn = NULL;
    js.active_life = NULL;
    EXPECT_TRUE(m && strstr(m, "req.multipart()"));
    free(m);
    hl_req_life_end(life);
    cleanup_js();
    be->free(actx);
}

#ifdef HL_ENABLE_HTTP_SERVER
/* H2: a handler over its budget failed only if the trip reached it as an
 * exception; one that tripped inside an un-awaited async call returned
 * normally and its request was answered as a success. */
UTEST(js_audit5, a_handler_over_budget_is_answered_500)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    js.max_instructions = 1000000;
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.get('/burn', (req, res) => { (async () => { for (;;) {} })(); });\n"
        "app.get('/ok', (req, res) => { res.status(204); });\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    int last = eval_int("globalThis.__hull_internals.__hull_routes.length - 1");
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    EXPECT_EQ(hl_js_dispatch(&js, last - 1, &req, &res), -1);
    free_req_ctx(&req);
    /* The next request runs on a fresh budget. */
    KlHttpRequest req2 = {0};
    KlHttpResponse res2 = {0};
    EXPECT_EQ(hl_js_dispatch(&js, last, &req2, &res2), 0);
    EXPECT_EQ(res2.status, 204);
    free_req_ctx(&req2);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* L2: methods returned JS_EXCEPTION with nothing thrown - the app saw
 * `null` as the error. L4: req.header / req.headers resolved names through
 * Object.prototype. L3: a query key was cut at an encoded NUL. */
/* The manifest JSON --verify-sig compares (and hull build signs) is encoded
 * in C from own data properties, and the stored copy's plain objects have no
 * prototype: an app that later sets Object.prototype.toJSON or
 * Object.prototype.hosts changes neither the JSON nor the enforced policy
 * (audit 5 H1, JS side). */
UTEST(js_audit5, manifest_ignores_prototype_tampering)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ env: ['PORT'], fs: { read: ['/data'] } });\n"
        "Object.prototype.toJSON = function () { return {}; };\n"
        "Object.prototype.hosts = ['*'];\n"
        "Object.prototype.write = ['/'];\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>",
                          JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);

    char *j = NULL;
    size_t jl = 0;
    ASSERT_EQ(hl_manifest_json_js(js.ctx, &j, &jl), 0);
    ASSERT_NE(j, NULL);
    EXPECT_NE(strstr(j, "\"env\":[\"PORT\"]"), NULL);
    EXPECT_EQ(strstr(j, "hosts"), NULL);
    EXPECT_EQ(strstr(j, "write"), NULL);
    free(j);

    HlManifest m;
    ASSERT_EQ(hl_manifest_extract_js(js.ctx, &m, NULL), 0);
    EXPECT_EQ(m.hosts_count, 0);
    EXPECT_EQ(m.fs_write_count, 0);
    EXPECT_EQ(m.env_count, 1);
    hl_manifest_free(&m);

    static const char undo[] =
        "delete Object.prototype.toJSON; delete Object.prototype.hosts;"
        "delete Object.prototype.write;";
    JS_FreeValue(js.ctx, JS_Eval(js.ctx, undo, sizeof undo - 1, "<c>",
                                 JS_EVAL_TYPE_GLOBAL));
    cleanup_js();
}

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_audit5, request_and_response_binding_edges)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    static const char q[] = "admin%00x=1&b=2";
    KlHttpRequest req = {0};
    req.query = q;
    req.query_len = sizeof q - 1;
    KlHttpResponse res = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => {\n"
        "  let s = '';\n"
        "  try { res.status(); s += 'x'; } catch (e) { s += e instanceof TypeError ? 'T' : 'n'; }\n"
        "  try { res.status.call({}, 200); s += 'x'; } catch (e) { s += e instanceof TypeError ? 'T' : 'n'; }\n"
        "  s += req.header('constructor') === undefined ? 'U' : 'p';\n"
        "  s += req.headers.toString === undefined ? 'U' : 'p';\n"
        "  s += req.query.admin === undefined ? 'U' : 'p';\n"
        "  s += Object.keys(req.query).includes('admin\\u0000x') ? 'K' : 'k';\n"
        "  globalThis.__a5_edges = s;\n"
        "  return 0; }", &req, &res), 0);
    char *s = eval_str("globalThis.__a5_edges");
    EXPECT_STREQ(s ? s : "(null)", "TTUUUK");
    free(s);
    free_req_ctx(&req);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* M4 + H2 in the worker VM: an async dispatch function's result is its
 * settled value (it came back as {}), a runaway async body is stopped (its
 * interrupt was a rejection, and the dispatch "succeeded"), and jobs a
 * dispatch leaves behind are discarded instead of holding its context -
 * 40 x 1 MiB against a 16 MiB heap ran the pool thread out of memory. */
JS_WORKER_CASE(an_async_dispatch_settles_and_leaves_nothing_behind,
    (js.max_instructions = 1000000, js.max_heap_bytes = 16u << 20),
    "  const r = await worker.dispatch(async () => { await null; return 7; });\n"
    "  check(r === 7, 'async result ' + JSON.stringify(r));\n"
    "  const m = await fails(async () => { await null; for (;;) {} });\n"
    "  check(m.includes('interrupted'), m);\n"
    "  for (let i = 0; i < 40; i++) {\n"
    "    const v = await worker.dispatch(() => {\n"
    "      const big = 'x'.repeat(1 << 20);\n"
    "      Promise.resolve().then(() => big.length);\n"
    "      return 1; });\n"
    "    check(v === 1, 'dispatch ' + i + ': ' + JSON.stringify(v));\n"
    "  }\n")

/* ── Audit 6: the JS runtime ──────────────────────────────────────────── */

#include "../../../../src/hull/runtime/js/internal.h"   /* hl_js_run_yield_check */

/* H1: a manifest field read through the prototype chain. `fs: []` plus
 * Array.prototype.write = ["."] widened fs.write while both the signed and
 * the runtime JSON said "fs":[]. Fields are read as own properties, and an
 * array where an object belongs counts as absent. */
UTEST(js_audit6, manifest_fields_are_read_as_own_properties)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "import { app } from 'hull:app';\n"
        "app.manifest({ fs: [], databases: [], kv: [], cors: [], env: ['A'] });\n"
        /* Since audit 18 the prototypes are frozen: every pollution is
         * refused, and the extractor's own-property reads still hold. */
        "let refused = 0;\n"
        "const tryit = (f) => { try { f(); } catch (e) { if (e instanceof TypeError) refused++; } };\n"
        "tryit(() => { Array.prototype.write = ['.']; });\n"
        "tryit(() => { Array.prototype.read = ['/']; });\n"
        "tryit(() => { Array.prototype.origins = ['*']; });\n"
        "tryit(() => { Array.prototype.dynamic = { schemes: ['sqlite'], hosts: ['*'] }; });\n"
        "tryit(() => { Array.prototype.named = { x: ':memory:' }; });\n"
        "tryit(() => { Object.prototype.hosts = ['*']; });\n"
        "globalThis.__a6_refused = refused;\n", &msg), 0);
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__a6_refused"), 6);
    HlManifest m;
    ASSERT_EQ(hl_manifest_extract_js(js.ctx, &m, NULL), 0);
    EXPECT_EQ(m.fs_read_count, 0);
    EXPECT_EQ(m.fs_write_count, 0);
    EXPECT_EQ(m.hosts_count, 0);
    EXPECT_EQ(m.cors_origin_count, 0);
    EXPECT_EQ(m.databases.declared, 0);
    EXPECT_EQ(m.databases.dynamic.declared, 0);
    EXPECT_EQ(m.databases.named_count, 0);
    EXPECT_EQ(m.kv.dynamic.declared, 0);
    EXPECT_EQ(m.env_count, 1);
    hl_manifest_free(&m);
    cleanup_js();
}

/* H1: an array is not a manifest (its fields came from Array.prototype). */
UTEST(js_audit6, an_array_manifest_is_refused)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "import { app } from 'hull:app';\n"
        "let r = 0;\n"
        /* Frozen since audit 18; the array manifest is refused regardless. */
        "try { Array.prototype.hosts = ['*']; r = 8; } catch (e) {}\n"
        "try { app.manifest([]); } catch (e) { r |= e instanceof TypeError ? 1 : 2; }\n"
        "globalThis.__a6 = r;\n", &msg), 0);
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__a6"), 1);
    HlManifest m;
    EXPECT_EQ(hl_manifest_extract_js(js.ctx, &m, NULL), -1);
    hl_manifest_free(&m);
    cleanup_js();
}

/* H1: globalThis.__hull_manifest was an ordinary global an app could set
 * itself - to a Proxy that showed the JSON encoder one policy and the
 * extractor another. It is a reserved, read-only view of what app.manifest()
 * stored; the extractor and encoder read only that stored value. */
UTEST(js_audit6, hull_manifest_global_is_reserved)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "let r = 0;\n"
        "const evil = new Proxy({}, { get: () => ['*'] });\n"
        "try { globalThis.__hull_manifest = evil; } catch (e) { r |= 1; }\n"
        "try { Object.defineProperty(globalThis, '__hull_manifest',\n"
        "        { value: { hosts: ['*'] } }); } catch (e) { r |= 2; }\n"
        "try { delete globalThis.__hull_manifest; } catch (e) { r |= 4; }\n"
        "if (globalThis.__hull_manifest === undefined) r |= 8;\n"
        "globalThis.__a6 = r;\n", &msg), 0);
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__a6"), 15);
    HlManifest m;
    EXPECT_EQ(hl_manifest_extract_js(js.ctx, &m, NULL), -1);   /* none declared */
    hl_manifest_free(&m);
    char *j = NULL;
    size_t jl = 0;
    EXPECT_EQ(hl_manifest_json_js(js.ctx, &j, &jl), 0);
    EXPECT_TRUE(j == NULL);
    free(j);

    EXPECT_EQ(a5_module_named("<test2>",
        "import { app } from 'hull:app';\n"
        "app.manifest({ hosts: ['a.example'] });\n"
        "globalThis.__a6 = globalThis.__hull_manifest.hosts[0] === 'a.example'\n"
        "  && app.getManifest().hosts[0] === 'a.example' ? 1 : 0;\n", &msg), 0);
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__a6"), 1);
    ASSERT_EQ(hl_manifest_extract_js(js.ctx, &m, NULL), 0);
    EXPECT_EQ(m.hosts_count, 1);
    hl_manifest_free(&m);
    cleanup_js();
}

/* L3: a key with a NUL was cut at the NUL - {"hosts\0": [], hosts: ["*"]}
 * encoded as two "hosts" keys. Refused. */
UTEST(js_audit6, manifest_key_with_nul_is_refused)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "import { app } from 'hull:app';\n"
        "app.manifest({ 'hosts\\u0000': [], hosts: ['*'] });\n", &msg), 0);
    free(msg);
    char *j = NULL;
    size_t jl = 0;
    EXPECT_EQ(hl_manifest_json_js(js.ctx, &j, &jl), -1);
    free(j);
    cleanup_js();
}

/* H2: a connection object was extensible: `c.retryOn = c.exec` handed to a
 * stdlib helper that calls opts.retryOn(...) ran app SQL with stdlib
 * identity (a `hull:` frame, `this` a real connection). Connection objects
 * and their sub-objects are sealed. */
UTEST(js_audit6, connection_objects_are_tamper_proof)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "import { db } from 'hull:db';\n"
        "const c = db.default();\n"
        "let r = 0;\n"
        "try { c.retryOn = c.exec; } catch (e) { r |= 1; }\n"
        "try { c.exec = () => 0; } catch (e) { r |= 2; }\n"
        "try { c.async.retryOn = c.async.exec; } catch (e) { r |= 4; }\n"
        "try { delete c.query; } catch (e) { r |= 8; }\n"
        "if (c.udf) { try { c.udf.x = 1; } catch (e) { r |= 16; } } else r |= 16;\n"
        "if (!Object.isExtensible(c) && c.retryOn === undefined) r |= 32;\n"
        "globalThis.__a6 = r;\n", &msg), 0);
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__a6"), 63);
    cleanup_js_caps();
}

#ifdef HL_ENABLE_HTTP_SERVER
/* H4: req.ctx was freed only by a synchronous handler; a middleware that
 * short-circuited (an auth reject) left its ctx object pinned in the JS
 * heap for good once Keel reset the request. */
UTEST(js_audit6, short_circuit_frees_req_ctx)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    EXPECT_EQ(a5_middleware("(req, res) => { req.ctx.user = { id: 1 }; return 1; }",
                            &req, &res), 1);
    EXPECT_TRUE(req.ctx == NULL);
    free_req_ctx(&req);
    cleanup_js();

    /* A middleware that continues hands it on to the handler. */
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpRequest req2 = {0};
    KlHttpResponse res2 = {0};
    EXPECT_EQ(a5_middleware("(req, res) => { req.ctx.user = 1; return 0; }",
                            &req2, &res2), 0);
    EXPECT_TRUE(req2.ctx != NULL);
    hl_js_req_ctx_free(&js, &req2);
    EXPECT_TRUE(req2.ctx == NULL);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* M2: the no-transaction-across-a-wait check ran only when an op was made;
 * `const p = http.fetch(..); conn.exec("BEGIN"); await p` held the
 * transaction across the wait. The yield point checks again: the
 * transaction is rolled back and the run is marked failed. */
UTEST(js_audit6, a_run_that_waits_in_a_transaction_is_failed)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "import { db } from 'hull:db';\n"
        "const c = db.default();\n"
        "c.exec('CREATE TABLE IF NOT EXISTS a6_t (v INTEGER)');\n"
        "c.exec('BEGIN');\n"
        "c.exec('INSERT INTO a6_t (v) VALUES (1)');\n", &msg), 0);
    free(msg);
    EXPECT_TRUE(hl_db_registry_open_txn(js.base.db_registry) != NULL);
    HlJsRunOnce run = {0};
    EXPECT_EQ(hl_js_run_yield_check(&js, &run, 4242), 1);
    EXPECT_EQ(run.txn_held, 1);
    /* Audit 33: its tag is failed too, so a job of it queued later is
     * dropped. */
    EXPECT_EQ(hl_js_run_tag_failed(&js, 4242), 1);
    EXPECT_TRUE(hl_db_registry_open_txn(js.base.db_registry) == NULL);
    /* Without a transaction open: nothing to fail. */
    HlJsRunOnce run2 = {0};
    EXPECT_EQ(hl_js_run_yield_check(&js, &run2, 4343), 0);
    EXPECT_EQ(run2.txn_held, 0);
    EXPECT_EQ(hl_js_run_tag_failed(&js, 4343), 0);
    cleanup_js_caps();
}

/* Audit 33: a failed run's tag is kept while code can still run under it -
 * here a reaction on a pending promise registered under that tag - and only
 * tags with nothing left are forgotten (the sweep): no live failed run is
 * evicted, however many runs fail after it. */
UTEST(js_audit33, failed_tag_kept_while_a_reaction_can_run)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    /* A reaction registered under tag 777 on a promise that never settles
     * by itself. */
    JS_SetJobTag(js.rt, 777);
    EXPECT_EQ(a5_module_named("<a33-hold>",
        "globalThis.__a33 = new Promise(() => {});\n"
        "globalThis.__a33.then(() => {});\n", &msg), 0);
    free(msg);
    JS_SetJobTag(js.rt, 0);
    EXPECT_EQ(a5_module_named("<a33-init>",
        "import { db } from 'hull:db';\n"
        "globalThis.__a33c = db.default();\n"
        "globalThis.__a33c.exec('CREATE TABLE IF NOT EXISTS a33_t (v INTEGER)');\n",
        &msg), 0);
    free(msg);
    for (uint64_t tag = 777; tag < 777 + 400; tag++) {
        char name[32];
        snprintf(name, sizeof name, "<a33-%u>", (unsigned)tag);
        EXPECT_EQ(a5_module_named(name, "globalThis.__a33c.exec('BEGIN');\n",
                                  &msg), 0);
        free(msg);
        HlJsRunOnce run = {0};
        EXPECT_EQ(hl_js_run_yield_check(&js, &run, tag), 1);
    }
    /* 777 still has a live reaction: kept through every sweep. */
    EXPECT_EQ(hl_js_run_tag_failed(&js, 777), 1);
    /* The newest are still recorded; the set did not keep all 400. */
    EXPECT_EQ(hl_js_run_tag_failed(&js, 777 + 399), 1);
    EXPECT_LT(js.txn_dead_len, (size_t)400);
    cleanup_js_caps();
}

#ifdef HL_ENABLE_HTTP_SERVER
/* L5: res.text / res.html cut the body at the first NUL. */
UTEST(js_audit6, res_text_keeps_embedded_nul)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(a5_middleware("(req, res) => { res.text('a\\u0000b'); return 1; }",
                            &req, &res), 1);
    EXPECT_EQ(res.body_len, (size_t)3);
    free_req_ctx(&req);
    kl_http_response_free(&res);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* ── Audit 7: the JS runtime ──────────────────────────────────────────── */

#ifdef HL_ENABLE_HTTP_SERVER
static size_t a7_tracked_ctxs(void)
{
    size_t n = 0;
    for (HlReqCtx *c = js.req_ctxs.head; c; c = c->next) n++;
    return n;
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* H2: a middleware's req.ctx was freed only once a handler (or SSE route)
 * ran, or the middleware short-circuited. A request that passed middleware
 * and then 404'd (or upgraded, or failed its body) never reached one; Keel
 * zeroed the request and the ctx stayed in the JS heap for good. The next
 * middleware on the same request slot now frees it. */
UTEST(js_audit7, a_ctx_whose_request_ended_early_is_freed)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    EXPECT_EQ(a5_middleware("(req, res) => { req.ctx.big = 'x'.repeat(200000);"
                            " return 0; }", &req, &res), 0);
    EXPECT_TRUE(req.ctx != NULL);
    EXPECT_EQ(a7_tracked_ctxs(), (size_t)1);
    int id = eval_int("globalThis.__hull_internals.__hull_middleware["
                      "globalThis.__hull_internals.__hull_middleware.length - 1].handler_id");
    JSMemoryUsage before, after;
    JS_ComputeMemoryUsage(js.rt, &before);
    for (int i = 0; i < 100; i++) {
        /* Keel resets the slot for its next request: no handler ran. */
        memset(&req, 0, sizeof req);
        EXPECT_EQ(hl_js_dispatch_middleware(&js, id, &req, &res), 0);
    }
    JS_RunGC(js.rt);
    JS_ComputeMemoryUsage(js.rt, &after);
    EXPECT_EQ(a7_tracked_ctxs(), (size_t)1);
    /* 100 x 200 KB kept would be 20 MB. */
    EXPECT_LT(after.malloc_size, before.malloc_size + (int64_t)(2 << 20));
    hl_js_req_ctx_free(&js, &req);
    EXPECT_TRUE(req.ctx == NULL);
    EXPECT_EQ(a7_tracked_ctxs(), (size_t)0);

    /* One left for a slot nothing reuses is freed with the runtime. */
    KlHttpRequest req2 = {0};
    EXPECT_EQ(hl_js_dispatch_middleware(&js, id, &req2, &res), 0);
    EXPECT_EQ(a7_tracked_ctxs(), (size_t)1);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* L1: app.manifest() was re-entrant through its argument's toJSON: the
 * nested call stored its own manifest and installed its decorations, then
 * the outer call overwrote the stored one. */
UTEST(js_audit7, app_manifest_is_not_re_entrant)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "import { app } from 'hull:app';\n"
        "let inner = 'none';\n"
        "app.manifest({ toJSON() {\n"
        "  try { app.manifest({ modules: ['hull/http-server@1', 'hull/timers@1'] });\n"
        "        inner = 'stored'; } catch (e) { inner = 'refused'; }\n"
        "  return { modules: [] }; } });\n"
        "globalThis.__a7 = inner + ':' + typeof app.every + ':' + typeof app.get;\n",
        &msg), 0);
    free(msg);
    char *s = eval_str("globalThis.__a7");
    EXPECT_STREQ(s, "refused:undefined:undefined");
    free(s);
    cleanup_js();
}

/* c_core L1: the wasm limits went through ToPrimitive: an array reached the
 * app-replaceable Array.prototype.valueOf (the signed JSON showed the array,
 * the runtime enforced what valueOf said), `{}` left an exception pending,
 * and -1 was enforced as the maximum. Only a positive number counts. */
UTEST(js_audit7, wasm_limits_are_numbers_only)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "import { app } from 'hull:app';\n"
        "app.manifest({ wasm: { heap: [5], stack: {}, gas: -1,\n"
        "                       timeoutMs: 250, maxInput: 1e12, maxOutput: '9' } });\n"
        "Array.prototype.valueOf = () => 123456789;\n", &msg), 0);
    free(msg);
    HlManifest m;
    ASSERT_EQ(hl_manifest_extract_js(js.ctx, &m, NULL), 0);
    EXPECT_FALSE(JS_HasException(js.ctx));
    EXPECT_EQ(m.wasm_heap, (uint32_t)0);
    EXPECT_EQ(m.wasm_stack, (uint32_t)0);
    EXPECT_EQ(m.wasm_gas, (int64_t)0);
    EXPECT_EQ(m.wasm_timeout_ms, (uint32_t)250);
    EXPECT_EQ(m.wasm_max_input, (uint32_t)UINT32_MAX);
    EXPECT_EQ(m.wasm_max_output, (uint32_t)0);
    hl_manifest_free(&m);
    cleanup_js();
}

/* http: { timeoutMs }: the app-wide outbound HTTP timeout, read like the
 * wasm limits - a number of at least 1 (capped at INT32_MAX, so it fits
 * HlHttpConfig.timeout_ms), never through ToPrimitive; anything else is
 * absent (0 = 30 s). */
UTEST(js_runtime, manifest_http_timeout)
{
    static const struct { const char *decl; uint32_t want; } cases[] = {
        { "app.manifest({ http: { timeoutMs: 1500 } });",      1500 },
        { "app.manifest({ http: { timeoutMs: 1500.9 } });",    1500 },
        { "app.manifest({ http: { timeoutMs: 0 } });",         0 },
        { "app.manifest({ http: { timeoutMs: -1 } });",        0 },
        { "app.manifest({ http: { timeoutMs: '9' } });",       0 },
        { "app.manifest({ http: { timeoutMs: [5] } });",       0 },
        { "app.manifest({ http: { timeoutMs: 1e15 } });",      (uint32_t)INT32_MAX },
        { "app.manifest({ http: { timeout_ms: 1500 } });",     0 },
        { "app.manifest({ hosts: ['a.test'] });",              0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        char src[512];
        snprintf(src, sizeof src, "import { app } from 'hull:app';\n%s\n",
                 cases[i].decl);
        char *msg = NULL;
        EXPECT_EQ(a5_module_named("<test>", src, &msg), 0);
        free(msg);
        HlManifest m;
        ASSERT_EQ(hl_manifest_extract_js(js.ctx, &m, NULL), 0);
        EXPECT_EQ_MSG(m.http_timeout_ms, cases[i].want, cases[i].decl);
        hl_manifest_free(&m);
        cleanup_js();
    }
}

/* Audit 24: gpu.devices entries of the wrong type were skipped and not
 * counted as listed, so a list of nothing but strings / fractions was the
 * empty list - every device (/dev/dri unveiled whole). Each element counts
 * now, so a list with no valid index disables gpu; a devices value that is
 * not an array does too. */
UTEST(js_runtime, manifest_gpu_devices_fail_closed)
{
    static const struct { const char *decl; int gpu; int count; } cases[] = {
        { "app.manifest({ gpu: { devices: [0, 2] } });",       1, 2 },
        { "app.manifest({ gpu: { devices: [] } });",           1, 0 },
        { "app.manifest({ gpu: {} });",                        1, 0 },
        { "app.manifest({ gpu: { devices: ['0'] } });",        0, 0 },
        { "app.manifest({ gpu: { devices: [0.5] } });",        0, 0 },
        { "app.manifest({ gpu: { devices: [, ] } });",         0, 0 },
        { "app.manifest({ gpu: { devices: [1, '0'] } });",     1, 1 },
        { "app.manifest({ gpu: { devices: '0' } });",          0, 0 },
        { "app.manifest({ gpu: { devices: { 0: 0 } } });",     0, 0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        char src[512];
        snprintf(src, sizeof src, "import { app } from 'hull:app';\n%s\n",
                 cases[i].decl);
        char *msg = NULL;
        EXPECT_EQ(a5_module_named("<test>", src, &msg), 0);
        free(msg);
        HlManifest m;
        ASSERT_EQ(hl_manifest_extract_js(js.ctx, &m, NULL), 0);
        EXPECT_EQ_MSG(m.gpu, cases[i].gpu, cases[i].decl);
        EXPECT_EQ_MSG(m.gpu_device_count, cases[i].count, cases[i].decl);
        hl_manifest_free(&m);
        cleanup_js();
    }
}

#ifdef HL_ENABLE_HTTP_SERVER
/* H2 follow-up: the ctx of a request that passed middleware and was then
 * answered by Keel itself (a 404 / 405) stayed until its connection slot
 * ran another middleware - one per slot, for good on an idle keep-alive or
 * a slot nothing reuses. serve.c reports every sent response
 * (HlRuntimeVtable.request_done) and JS frees the ctx there. */
UTEST(js_audit7, req_ctx_freed_when_the_response_is_sent)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    ASSERT_TRUE(hl_js_vtable.request_done != NULL);
    enum { N = 64 };                 /* N connection slots, one 404 each */
    static KlHttpRequest reqs[N];
    memset(reqs, 0, sizeof reqs);
    KlHttpResponse res = {0};
    JSMemoryUsage base, held, after;
    JS_RunGC(js.rt);
    JS_ComputeMemoryUsage(js.rt, &base);
    ASSERT_EQ(a5_middleware("(req, res) => { req.ctx.big = 'x'.repeat(200000);"
                            " return 0; }", &reqs[0], &res), 0);
    int id = eval_int("globalThis.__hull_internals.__hull_middleware["
                      "globalThis.__hull_internals.__hull_middleware.length - 1].handler_id");
    for (int i = 1; i < N; i++)
        ASSERT_EQ(hl_js_dispatch_middleware(&js, id, &reqs[i], &res), 0);
    EXPECT_EQ(a7_tracked_ctxs(), (size_t)N);
    JS_RunGC(js.rt);
    JS_ComputeMemoryUsage(js.rt, &held);
    /* 64 x 200 KB */
    EXPECT_GT(held.malloc_size, base.malloc_size + (int64_t)(10 << 20));

    for (int i = 0; i < N; i++)      /* Keel sent each 404 */
        hl_js_vtable.request_done(&js.base, &reqs[i]);
    EXPECT_EQ(a7_tracked_ctxs(), (size_t)0);
    for (int i = 0; i < N; i++)
        EXPECT_TRUE(reqs[i].ctx == NULL);
    JS_RunGC(js.rt);
    JS_ComputeMemoryUsage(js.rt, &after);
    EXPECT_LT(after.malloc_size, base.malloc_size + (int64_t)(2 << 20));
    /* a request with nothing kept, and a repeat, are no-ops */
    hl_js_vtable.request_done(&js.base, &reqs[0]);
    EXPECT_EQ(a7_tracked_ctxs(), (size_t)0);
    cleanup_js();
}

/* A handler that is still running when its response goes out (a parked
 * run, an un-awaited promise) keeps req.ctx: its req object holds its own
 * reference, and request_done frees only the request's copy. */
UTEST(js_audit7, request_done_leaves_a_running_handler_its_ctx)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    ASSERT_EQ(a5_module_named("<test>",
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', (req, res) => { req.ctx.v = { n: 7 }; return 0; });\n"
        "app.get('/x', (req, res) => { globalThis.__kept = req; res.status(204); });\n",
        &msg), 0);
    free(msg);
    int mw = eval_int("globalThis.__hull_internals.__hull_middleware["
                      "globalThis.__hull_internals.__hull_middleware.length - 1].handler_id");
    int route = eval_int("globalThis.__hull_internals.__hull_routes.length - 1");
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    ASSERT_EQ(hl_js_dispatch_middleware(&js, mw, &req, &res), 0);
    ASSERT_EQ(hl_js_dispatch(&js, route, &req, &res), 0);
    hl_js_vtable.request_done(&js.base, &req);
    JS_RunGC(js.rt);
    EXPECT_EQ(eval_int("globalThis.__kept.ctx.v.n"), 7);
    EXPECT_EQ(a7_tracked_ctxs(), (size_t)0);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* L3 (JS side): hl_maybe_compress reports a body it could not store, and
 * res.json / res.html / res.text ignored that: the response went out with
 * its status and headers and no body. They raise now, as Lua's do. */
static void *a7_small_malloc(void *ctx, size_t size)
{
    (void)ctx;
    return size >= 4096 ? NULL : malloc(size);
}

static void *a7_small_realloc(void *ctx, void *ptr, size_t old_size,
                              size_t new_size)
{
    (void)ctx; (void)old_size;
    return new_size >= 4096 ? NULL : realloc(ptr, new_size);
}

static void a7_small_free(void *ctx, void *ptr, size_t size)
{
    (void)ctx; (void)size;
    free(ptr);
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_audit7, a_body_that_cannot_be_stored_raises)
{
    static const char *const srcs[] = {
        "(req, res) => { try { res.json({ s: 'x'.repeat(8000) }); }"
        " catch (e) { globalThis.__err = String(e); } return 1; }",
        "(req, res) => { try { res.html('x'.repeat(8000)); }"
        " catch (e) { globalThis.__err = String(e); } return 1; }",
        "(req, res) => { try { res.text('x'.repeat(8000)); }"
        " catch (e) { globalThis.__err = String(e); } return 1; }",
    };
    static const char *const names[] = { "res.json", "res.html", "res.text" };
    KlAllocator alloc = { a7_small_malloc, a7_small_realloc, a7_small_free, NULL };
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        KlHttpResponse res;
        ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
        KlHttpRequest req = {0};
        EXPECT_EQ(a5_middleware(srcs[i], &req, &res), 1);
        EXPECT_EQ(res.body_len, (size_t)0);
        char check[128];
        snprintf(check, sizeof check,
                 "String(globalThis.__err).includes('%s: out of memory') ? 1 : 0",
                 names[i]);
        EXPECT_EQ(eval_int(check), 1);
        free_req_ctx(&req);
        kl_http_response_free(&res);
        cleanup_js();
    }

    /* A body that fits is stored as before. */
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(a5_middleware("(req, res) => { res.text('ok'); return 1; }",
                            &req, &res), 1);
    EXPECT_EQ(res.body_len, (size_t)2);
    free_req_ctx(&req);
    kl_http_response_free(&res);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* res.json used to swallow a throwing JSON.stringify (a cycle): it returned
 * undefined with the exception left pending. */
UTEST(js_audit7, res_json_propagates_a_stringify_error)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => { const o = {}; o.o = o;"
        " try { res.json(o); } catch (e) { globalThis.__err = e instanceof TypeError; }"
        " return 1; }", &req, &res), 1);
    EXPECT_EQ(eval_int("globalThis.__err === true ? 1 : 0"), 1);
    EXPECT_EQ(res.body_len, (size_t)0);
    free_req_ctx(&req);
    kl_http_response_free(&res);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* ── Audit 8 ─────────────────────────────────────────────────────────── */

struct HlSuspendOp;
int hl_js_op_suspend(HlJS *jsp, struct HlSuspendOp *op);   /* internal.h */

/* H1: an op made while a connection is active but no request life is (app
 * code a resume's resolve or an inherited setter ran) took the connection
 * as its own - holds = 1, counted nowhere - and suspended it. It runs
 * detached now: no suspend is even attempted. */
UTEST(js_audit8, an_op_made_with_no_request_life_runs_detached)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    init_js();
    ASSERT_TRUE(js_initialized);
    js.base.async_ctx = actx;
    int dummy_conn;
    js.active_conn = (KlHttpConn *)(void *)&dummy_conn;
    js.active_life = NULL;
    char *m = eval_str(
        "(() => { try { hull.sleep(5).then(() => { globalThis.__a8_done = 1; });"
        " return 'detached'; } catch (e) { return String(e.message); } })()");
    js.active_conn = NULL;
    js.last_async_cont = NULL;
    EXPECT_TRUE(m && strcmp(m, "detached") == 0);
    free(m);
    /* hl_js_op_suspend refuses outright without a life, too. */
    js.active_conn = (KlHttpConn *)(void *)&dummy_conn;
    KlAsyncOp op;
    memset(&op, 0, sizeof op);
    EXPECT_EQ(hl_js_op_suspend(&js, (struct HlSuspendOp *)&op), -1);
    js.active_conn = NULL;
    for (int i = 0; i < 200 && eval_int("globalThis.__a8_done ? 1 : 0") != 1; i++)
        be->tick(actx, 5);
    EXPECT_EQ(eval_int("globalThis.__a8_done ? 1 : 0"), 1);
    cleanup_js();
    be->free(actx);
}

#ifdef HL_ENABLE_HTTP_SERVER
/* H1: `req` was built with property SETS, which ran a setter an app put on
 * Object.prototype - app code, with the connection active, before the
 * handler (or, in middleware, before the async gate was armed). Every
 * property is defined now. */
UTEST(js_audit8, building_req_runs_no_inherited_setter)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    static const char reg[] =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', (req, res) => { globalThis.__a8_m ="
        " typeof req.method + ':' + typeof req.headers + ':' +"
        " typeof req.header; return 0; });\n";
    JSValue val = JS_Eval(js.ctx, reg, strlen(reg), "<test>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    int id = eval_int("globalThis.__hull_internals.__hull_middleware["
                      "globalThis.__hull_internals.__hull_middleware.length - 1].handler_id");
    /* Armed after registration: only the request build can reach them. */
    /* Since audit 18 Object.prototype is frozen: each setter is refused (9
     * of 9), and the request build still defines rather than sets. */
    char *m = eval_str(
        "globalThis.__a8_set = 0;\n"
        "let refused = 0;\n"
        "for (const k of ['method', 'path', 'query', 'params', 'headers',\n"
        "                 'body', 'ctx', 'header', 'remote_addr'])\n"
        "  try { Object.defineProperty(Object.prototype, k, { configurable: true,\n"
        "    set(v) { globalThis.__a8_set++; } }); } catch (e) { refused++; }\n"
        "String(refused)");
    EXPECT_TRUE(m && strcmp(m, "9") == 0);
    free(m);
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    EXPECT_EQ(hl_js_dispatch_middleware(&js, id, &req, &res), 0);
    EXPECT_EQ(eval_int("globalThis.__a8_set"), 0);
    char *seen = eval_str("globalThis.__a8_m");
    EXPECT_TRUE(seen && strcmp(seen, "string:object:function") == 0);
    free(seen);
    free_req_ctx(&req);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* L1: res.json's code argument and res.redirect's code / url conversions
 * were not checked - the body went out with the old status, or nothing was
 * sent and undefined returned - with the exception left pending. */
UTEST(js_audit8, res_conversions_that_throw_propagate)
{
    static const char *const srcs[] = {
        "(req, res) => { try { res.json({ a: 1 }, { valueOf() { throw new"
        " Error('code'); } }); globalThis.__a8_e = 'none'; } catch (e) {"
        " globalThis.__a8_e = e.message; } return 1; }",
        "(req, res) => { try { res.redirect('/x', { valueOf() { throw new"
        " Error('code'); } }); globalThis.__a8_e = 'none'; } catch (e) {"
        " globalThis.__a8_e = e.message; } return 1; }",
        "(req, res) => { try { res.redirect({ toString() { throw new"
        " Error('url'); } }); globalThis.__a8_e = 'none'; } catch (e) {"
        " globalThis.__a8_e = e.message; } return 1; }",
    };
    static const char *const want[] = { "code", "code", "url" };
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        KlAllocator alloc = kl_allocator_default();
        KlHttpResponse res;
        ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
        KlHttpRequest req = {0};
        EXPECT_EQ(a5_middleware(srcs[i], &req, &res), 1);
        char *e = eval_str("globalThis.__a8_e");
        EXPECT_TRUE(e && strcmp(e, want[i]) == 0);
        free(e);
        EXPECT_EQ(res.body_len, (size_t)0);   /* nothing was written */
        EXPECT_FALSE(JS_HasException(js.ctx));
        free_req_ctx(&req);
        kl_http_response_free(&res);
        cleanup_js();
    }
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* L1: a middleware that made req.ctx a throwing accessor left the exception
 * pending past the dispatch (it surfaced in the next unrelated error). The
 * request fails instead, and nothing is left pending. */
UTEST(js_audit8, a_throwing_req_ctx_getter_fails_the_middleware)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => { Object.defineProperty(req, 'ctx', { get() {"
        " throw new Error('boom'); } }); return 0; }", &req, &res), -1);
    EXPECT_FALSE(JS_HasException(js.ctx));
    EXPECT_TRUE(req.ctx == NULL);
    free_req_ctx(&req);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* ── hull:_task - detached tasks ───────────────────────────────────── */

/* The task errors the runtime logged (log.c has no callback removal, so the
 * collector is registered once and stays). */
static char g_task_log[4096];
static void task_log_collect(log_Event *ev)
{
    if (!ev->fmt || (!strstr(ev->fmt, "spawned task error") &&
                     !strstr(ev->fmt, "async callback error")))
        return;
    size_t n = strlen(g_task_log);
    if (n + 2 >= sizeof g_task_log) return;
    vsnprintf(g_task_log + n, sizeof g_task_log - n - 1, ev->fmt, ev->ap);
    n = strlen(g_task_log);
    g_task_log[n] = '\n';
    g_task_log[n + 1] = '\0';
}

static void task_log_reset(void)
{
    static int registered;
    if (!registered) {
        log_add_callback(task_log_collect, NULL, LOG_ERROR);
        registered = 1;
    }
    g_task_log[0] = '\0';
}

/* Tick the loop until `cond` (a JS expression) is true, or give up. */
static int task_tick_until(const HlAsyncBackend *be, HlAsyncBackendCtx *actx,
                           const char *cond)
{
    for (int i = 0; i < 400; i++) {
        if (eval_int(cond) == 1) return 1;
        be->tick(actx, 5);
    }
    return eval_int(cond) == 1;
}

/* The task runs after the entry that spawned it, and as no request: an op it
 * makes runs detached even though a request was active when it was spawned
 * (and is still marked active when the loop runs the task). */
UTEST(js_task, runs_after_its_entry_and_detached)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    init_js();
    ASSERT_TRUE(js_initialized);
    js.base.async_ctx = actx;
    HlReqLife *life = hl_req_life_new();
    ASSERT_TRUE(life != NULL);
    int dummy_conn;
    js.active_conn = (KlHttpConn *)(void *)&dummy_conn;
    js.active_life = life;
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("hull:tests:task_order",
        "import { _task } from 'hull:_task';\n"
        "globalThis.__tk = [];\n"
        "_task.spawn(() => {\n"
        "  globalThis.__tk.push('task');\n"
        "  hull.sleep(5).then(() => { globalThis.__tk.push('slept'); });\n"
        "});\n"
        "Promise.resolve().then(() => globalThis.__tk.push('job'));\n"
        "globalThis.__tk.push('entry');\n", &msg), 0);
    free(msg);
    char *seen = eval_str("globalThis.__tk.join(',')");
    EXPECT_TRUE(seen && strcmp(seen, "entry,job") == 0);
    free(seen);
    /* The request is still marked active when the loop runs the task. */
    js.active_conn = (KlHttpConn *)(void *)&dummy_conn;
    js.active_life = life;
    EXPECT_TRUE(task_tick_until(be, actx, "globalThis.__tk.length === 4 ? 1 : 0"));
    seen = eval_str("globalThis.__tk.join(',')");
    EXPECT_TRUE(seen && strcmp(seen, "entry,job,task,slept") == 0);
    free(seen);
    EXPECT_EQ(life->attached, 0);   /* the sleep never belonged to it */
    EXPECT_EQ(life->held, 0);
    js.active_conn = NULL;
    js.active_life = NULL;
    hl_req_life_end(life);
    cleanup_js();
    be->free(actx);
}

/* A throw, an early rejection and a rejection after a wait are logged; none
 * reaches the code that spawned the task, which carried on. */
UTEST(js_task, errors_are_logged_not_raised)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    init_js();
    ASSERT_TRUE(js_initialized);
    js.base.async_ctx = actx;
    task_log_reset();
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("hull:tests:task_errors",
        "import { _task } from 'hull:_task';\n"
        "_task.spawn(() => { throw new Error('tk-sync'); });\n"
        "_task.spawn(async () => { throw new Error('tk-early'); });\n"
        "_task.spawn(async () => { await hull.sleep(2);"
        " globalThis.__tk_late = 1; throw new Error('tk-late'); });\n"
        "globalThis.__tk_spawner = 'done';\n", &msg), 0);
    free(msg);
    char *s = eval_str("globalThis.__tk_spawner");
    EXPECT_TRUE(s && strcmp(s, "done") == 0);
    free(s);
    EXPECT_TRUE(task_tick_until(be, actx, "globalThis.__tk_late === 1 ? 1 : 0"));
    for (int i = 0; i < 20 && !strstr(g_task_log, "tk-late"); i++)
        be->tick(actx, 5);
    EXPECT_TRUE(strstr(g_task_log, "tk-sync") != NULL);
    EXPECT_TRUE(strstr(g_task_log, "tk-early") != NULL);
    EXPECT_TRUE(strstr(g_task_log, "tk-late") != NULL);
    EXPECT_FALSE(JS_HasException(js.ctx));
    cleanup_js();
    be->free(actx);
}

/* Each task is a run with a budget of its own: a runaway one is stopped and
 * logged, and neither its spawner nor the next task is charged for it. */
UTEST(js_task, has_a_budget_of_its_own)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    init_js();
    ASSERT_TRUE(js_initialized);
    js.base.async_ctx = actx;
    js.max_instructions = 1000000;   /* polls weigh 10000 each */
    hl_js_reset_request(&js);
    task_log_reset();
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("hull:tests:task_budget",
        "import { _task } from 'hull:_task';\n"
        "_task.spawn(() => { globalThis.__tk_spin = 1; for (;;) {} });\n"
        "_task.spawn(() => { globalThis.__tk_next = 1; });\n"
        "globalThis.__tk_spawned = 1;\n", &msg), 0);
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__tk_spawned|0"), 1);
    EXPECT_EQ(eval_int("globalThis.__tk_spin === undefined ? 1 : 0"), 1);
    EXPECT_TRUE(task_tick_until(be, actx, "globalThis.__tk_next === 1 ? 1 : 0"));
    EXPECT_EQ(eval_int("globalThis.__tk_spin|0"), 1);
    EXPECT_TRUE(strstr(g_task_log, "instruction limit exceeded") != NULL);
    cleanup_js();
    be->free(actx);
}

/* A transaction a task leaves open is rolled back when it ends. */
UTEST(js_task, an_open_transaction_is_rolled_back)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    pending_async_ctx = actx;
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("hull:tests:task_txn",
        "import { _task } from 'hull:_task';\n"
        "import { db as dbMod } from 'hull:db';\n"
        "const c = dbMod.default();\n"
        "c.exec('CREATE TABLE tk_txn (x INTEGER)');\n"
        "_task.spawn(() => { c.exec('BEGIN');"
        " c.exec('INSERT INTO tk_txn VALUES (1)'); globalThis.__tk_txn = 1; });\n",
        &msg), 0);
    free(msg);
    EXPECT_TRUE(task_tick_until(be, actx, "globalThis.__tk_txn === 1 ? 1 : 0"));
    EXPECT_TRUE(hl_db_registry_open_txn(js.base.db_registry) == NULL);
    EXPECT_EQ(eval_int("(() => { const r = globalThis.db.query("
                       "'SELECT COUNT(*) AS n FROM tk_txn'); return r[0].n; })()"), 0);
    cleanup_js_caps();
    be->free(actx);
}

#ifdef HL_ENABLE_HTTP_SERVER
/* A task runs after its spawner's request is over: a `res` its closure
 * captured is closed by then, and writes nothing into that response. */
UTEST(js_task, cannot_write_its_spawners_response)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    init_js();
    ASSERT_TRUE(js_initialized);
    js.base.async_ctx = actx;
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("hull:tests:task_res",
        "import { _task } from 'hull:_task';\n"
        "globalThis.__tk_spawn = _task.spawn;\n", &msg), 0);
    free(msg);
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => { globalThis.__tk_spawn(() => {"
        " try { res.json({ late: 1 }); globalThis.__tk_res = 'wrote'; }"
        " catch (e) { globalThis.__tk_res = 'refused'; } }); return 0; }",
        &req, &res), 0);
    EXPECT_TRUE(task_tick_until(be, actx,
                                "globalThis.__tk_res !== undefined ? 1 : 0"));
    char *r = eval_str("globalThis.__tk_res");
    EXPECT_TRUE(r && strcmp(r, "refused") == 0);
    free(r);
    EXPECT_EQ(res.body_len, (size_t)0);
    free_req_ctx(&req);
    kl_http_response_free(&res);
    cleanup_js();
    be->free(actx);
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* Stdlib-only; refused without a loop; a task that never ran is released
 * with the runtime (the context would otherwise free with a live object). */
UTEST(js_task, stdlib_only_needs_a_loop_and_is_freed_unrun)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("<test>",
        "import { _task } from 'hull:_task';\n"
        "globalThis.__tk_app = 1;\n", &msg), -1);
    EXPECT_TRUE(msg && strstr(msg, "internal to the Hull stdlib") != NULL);
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__tk_app === undefined ? 1 : 0"), 1);

    EXPECT_EQ(a5_module_named("hull:tests:task_noloop",
        "import { _task } from 'hull:_task';\n"
        "_task.spawn(() => {});\n", &msg), -1);
    EXPECT_TRUE(msg && strstr(msg, "requires an active event loop") != NULL);
    free(msg);

    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    js.base.async_ctx = actx;
    EXPECT_EQ(a5_module_named("hull:tests:task_unrun",
        "import { _task } from 'hull:_task';\n"
        "const big = { data: new Array(1000).fill('x') };\n"
        "_task.spawn(() => big);\n", &msg), 0);
    free(msg);
    EXPECT_TRUE(js.tasks != NULL);
    cleanup_js();
    be->free(actx);
}

/* ── Audit 9 ───────────────────────────────────────────────────────── */

#ifdef HL_ENABLE_WASM
/* echo.wasm (tests/hull/cap/test_wasm.c): copies its input to its output. */
static const unsigned char a9_echo_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14, 0x03, 0x60,
  0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f,
  0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x11, 0x01, 0x03, 0x65, 0x6e,
  0x76, 0x09, 0x68, 0x6f, 0x73, 0x74, 0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00,
  0x00, 0x03, 0x03, 0x02, 0x01, 0x02, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07,
  0x28, 0x03, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c,
  0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73,
  0x00, 0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73,
  0x69, 0x6f, 0x6e, 0x00, 0x02, 0x0a, 0x20, 0x02, 0x19, 0x00, 0x20, 0x01,
  0x20, 0x03, 0x4b, 0x04, 0x40, 0x41, 0x7e, 0x0f, 0x0b, 0x20, 0x02, 0x20,
  0x00, 0x20, 0x01, 0xfc, 0x0a, 0x00, 0x00, 0x20, 0x01, 0x0b, 0x04, 0x00,
  0x41, 0x01, 0x0b
};
static const HlEntry a9_wasm_entries[] = {
    { "compute/echo.wasm", a9_echo_wasm, sizeof a9_echo_wasm },
    { 0, 0, 0 }
};

/* hull:compute registers only when a WASM cache exists at init. */
static void a9_init_js_wasm(HlWasmCache *cache, HlVfs *vfs)
{
    if (js_initialized)
        hl_js_free(&js);
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    js.base.platform_vfs = &platform_vfs;
    js.base.wasm_cache = cache;
    js.base.app_vfs = vfs;
    js_initialized = (hl_js_init(&js, &cfg) == 0);
    if (js_initialized) install_test_js_globals(&js);
}

/* H1: compute.stream read a plain ArrayBuffer input through a pointer taken
 * before the output callback - app code that runs between chunks - and
 * ArrayBuffer.prototype.transfer() there freed the backing store: every
 * later chunk was a use-after-free read (ASan). The input is copied now, so
 * the stream carries on with the bytes it was given. */
UTEST(js_audit9, compute_stream_copies_an_arraybuffer_input)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    char *r = eval_str(
        "(() => {\n"
        "  const ab = new ArrayBuffer(1024);\n"
        "  const u = new Uint8Array(ab);\n"
        "  for (let i = 0; i < 1024; i++) u[i] = (i * 7) & 255;\n"
        "  const out = []; let calls = 0;\n"
        "  compute.stream('echo', ab, (chunk) => {\n"
        "    calls++;\n"
        "    if (ab.byteLength) ab.transfer(0);   /* frees the backing store */\n"
        "    new Array(4096).fill(0x5a);           /* reuse the freed memory */\n"
        "    for (const b of new Uint8Array(chunk)) out.push(b);\n"
        "  }, { chunkSize: 256 });\n"
        "  if (out.length !== 1024) return 'len ' + out.length;\n"
        "  for (let i = 0; i < 1024; i++)\n"
        "    if (out[i] !== ((i * 7) & 255)) return 'byte ' + i;\n"
        "  return 'ok ' + calls + ' ' + (ab.byteLength === 0);\n"
        "})()");
    EXPECT_TRUE(r && strcmp(r, "ok 4 true") == 0);
    if (r && strcmp(r, "ok 4 true") != 0) printf("  got: %s\n", r);
    free(r);
    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}

/* H1: the input is resolved after the options - a getter there that
 * detaches the input no longer leaves a pointer to freed memory behind. */
UTEST(js_audit9, compute_stream_resolves_input_after_options)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    char *r = eval_str(
        "(() => {\n"
        "  const ab = new ArrayBuffer(512);\n"
        "  new Uint8Array(ab).fill(0x41);\n"
        "  const opts = { get chunkSize() { ab.transfer(0); return 256; } };\n"
        "  /* Detached first: the input is no longer the 512 bytes of 'A' -\n"
        "   * since audit 29 a detached buffer is refused, not stringified. */\n"
        "  let got;\n"
        "  try { got = new Uint8Array(compute.stream('echo', ab, opts)); }\n"
        "  catch (e) { return e instanceof TypeError ? 'fresh' : 'other'; }\n"
        "  return got.length === 512 ? 'stale' : 'fresh';\n"
        "})()");
    EXPECT_TRUE(r && strcmp(r, "fresh") == 0);
    free(r);
    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}
/* Audit 15: compute.stream ran every chunk inside one binding call, each
 * with its own gas and timeout, and charged nothing to the run: a stream of
 * many chunks held the event loop for as long as UINT32_MAX chunk timeouts.
 * Each chunk is charged now (1 unit + 1 per 64 input bytes), and the trip
 * is thrown from inside compute.stream. 1 MB in 256-byte chunks is 4096
 * chunks, 20480 units; under a 15000 limit the stream stops part-way, so the
 * statement after it never runs. (Unfixed, the stream ran to the end and
 * returned: its 1 MB result is charged too, but only the next call or loop
 * polls, so `after` was set.) */
UTEST(js_audit15, compute_stream_chunks_are_charged_to_the_run)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(eval_int("globalThis.ab = new ArrayBuffer(1 << 20); 1"), 1);

    int64_t saved = js.max_instructions;
    js.max_instructions = 15000;
    hl_js_budget_arm(&js);
    static const char code[] =
        "compute.stream('echo', ab, { chunkSize: 256 });\n"
        "globalThis.after = 1;\n";
    JSValue r = JS_Eval(js.ctx, code, sizeof code - 1, "<a15>",
                        JS_EVAL_TYPE_GLOBAL);
    EXPECT_TRUE(JS_IsException(r));
    JS_FreeValue(js.ctx, r);
    JS_FreeValue(js.ctx, JS_GetException(js.ctx));
    EXPECT_TRUE(js.budget_tripped);
    EXPECT_FALSE(js.budget_timed_out);
    JSValue g = JS_GetGlobalObject(js.ctx);
    JSValue a = JS_GetPropertyStr(js.ctx, g, "after");
    EXPECT_TRUE(JS_IsUndefined(a));
    JS_FreeValue(js.ctx, a);
    JS_FreeValue(js.ctx, g);

    /* Under a budget that covers it, the same stream completes. */
    js.max_instructions = saved;
    hl_js_budget_arm(&js);
    EXPECT_EQ(eval_int("new Uint8Array(compute.stream('echo', ab,"
                       " { chunkSize: 256 })).length === (1 << 20) ? 1 : 0"), 1);

    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}

/* Audit 15: buffer-mode output is capped at the VM heap limit (it was grown
 * off-heap without bound, then copied into the heap in one piece). */
UTEST(js_audit15, compute_stream_output_is_capped_at_the_heap_limit)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 64 * 1024;
    char *r = eval_str(
        "(() => {\n"
        "  try { compute.stream('echo', 'a'.repeat(256 * 1024)); }\n"
        "  catch (e) { return String(e.message); }\n"
        "  return 'not refused';\n"
        "})()");
    EXPECT_TRUE(r && strstr(r, "output_too_large") != NULL);
    if (r && !strstr(r, "output_too_large")) printf("  got: %s\n", r);
    free(r);
    js.max_heap_bytes = saved;
    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}

/* Audit 18 M4: an OWNED WasmBuffer holds its bytes on the server allocator,
 * which no VM limit covers; they are counted against the heap limit now
 * (refused past it, given back on close), and compute.buffer's copy is
 * charged 1 unit per 8 bytes. */
UTEST(js_audit18, owned_buffers_are_bounded_by_the_heap_limit)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 8u << 20;
    char *r = eval_str(
        "(() => {\n"
        "  const ab = new ArrayBuffer(1 << 20);\n"
        "  const held = []; let msg = 'not refused';\n"
        "  try { for (let i = 0; i < 64; i++) held.push(compute.buffer(ab)); }\n"
        "  catch (e) { msg = String(e.message); }\n"
        "  const n = held.length;\n"
        "  for (const b of held) b.close();\n"
        "  const b = compute.buffer(ab); const len = b.length; b.close();\n"
        "  return (msg.includes('heap limit') ? 'refused' : msg) + ' ' + n +\n"
        "         ' ' + len;\n"
        "})()");
    EXPECT_TRUE(r && strcmp(r, "refused 8 1048576") == 0);
    if (r && strcmp(r, "refused 8 1048576") != 0) printf("  got: %s\n", r);
    free(r);
    EXPECT_EQ(js.wasm_buf_bytes, (size_t)0);
    js.max_heap_bytes = saved;

    /* The copy is charged: 1 MB is 131072 units, over a 100000 budget. */
    EXPECT_EQ(eval_int("globalThis.ab = new ArrayBuffer(1 << 20); 1"), 1);
    int64_t saved_ins = js.max_instructions;
    js.max_instructions = 100000;
    hl_js_budget_arm(&js);
    static const char code[] =
        "compute.buffer(ab);\n"
        "globalThis.after = 1;\n";
    JSValue v = JS_Eval(js.ctx, code, sizeof code - 1, "<a18>",
                        JS_EVAL_TYPE_GLOBAL);
    EXPECT_TRUE(JS_IsException(v));
    JS_FreeValue(js.ctx, v);
    JS_FreeValue(js.ctx, JS_GetException(js.ctx));
    EXPECT_TRUE(js.budget_tripped);
    JSValue g = JS_GetGlobalObject(js.ctx);
    JSValue a = JS_GetPropertyStr(js.ctx, g, "after");
    EXPECT_TRUE(JS_IsUndefined(a));
    JS_FreeValue(js.ctx, a);
    JS_FreeValue(js.ctx, g);
    js.max_instructions = saved_ins;
    hl_js_budget_arm(&js);

    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}
#endif /* HL_ENABLE_WASM */

/* M1: a rejection's toString is app code, and the resume ran it after its
 * last drain - a job it queued ran in the NEXT entry's drain, attached to
 * that entry's request. The resume drains after it now. */
UTEST(js_audit9, a_rejections_tostring_jobs_run_in_its_own_resume)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    init_js();
    ASSERT_TRUE(js_initialized);
    js.base.async_ctx = actx;
    task_log_reset();
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("hull:tests:a9_m1",
        "import { _task } from 'hull:_task';\n"
        "_task.spawn(async () => {\n"
        "  await hull.sleep(2);\n"
        "  throw { toString() {\n"
        "    Promise.resolve().then(() => { globalThis.__a9_job = 1; });\n"
        "    return 'a9-m1-reason'; } };\n"
        "});\n", &msg), 0);
    free(msg);
    for (int i = 0; i < 400 && !strstr(g_task_log, "a9-m1-reason"); i++)
        be->tick(actx, 5);
    EXPECT_TRUE(strstr(g_task_log, "a9-m1-reason") != NULL);
    /* No entry ran since the resume: only its own drain can have run it. */
    EXPECT_EQ(eval_int("globalThis.__a9_job === 1 ? 1 : 0"), 1);
    cleanup_js();
    be->free(actx);
}

/* L1: a task fired inside `hull test`'s promise pump re-armed the parked
 * case's budget and cleared its active state. It puts both back now - even
 * after tripping its own budget. */
UTEST(js_audit9, a_task_restores_the_run_it_interrupted)
{
    const HlAsyncBackend *be = hl_async_backend();
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    init_js();
    ASSERT_TRUE(js_initialized);
    js.base.async_ctx = actx;
    js.max_instructions = 1000000;   /* polls weigh 10000 each */
    hl_js_reset_request(&js);
    task_log_reset();
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("hull:tests:a9_l1",
        "import { _task } from 'hull:_task';\n"
        "_task.spawn(() => { globalThis.__a9_spin = 1; for (;;) {} });\n",
        &msg), 0);
    free(msg);
    HlReqLife *life = hl_req_life_new();
    ASSERT_TRUE(life != NULL);
    int dummy_conn, dummy_timer;
    js.active_conn  = (KlHttpConn *)(void *)&dummy_conn;
    js.active_life  = life;
    js.active_timer = &dummy_timer;
    js.instruction_count = 4242;
    js.budget_tripped = 0;
    for (int i = 0; i < 400 && !strstr(g_task_log, "instruction limit"); i++)
        be->tick(actx, 5);
    EXPECT_TRUE(strstr(g_task_log, "instruction limit exceeded") != NULL);
    EXPECT_TRUE(js.active_conn == (KlHttpConn *)(void *)&dummy_conn);
    EXPECT_TRUE(js.active_life == life);
    EXPECT_TRUE(js.active_timer == (void *)&dummy_timer);
    EXPECT_EQ(js.instruction_count, (int64_t)4242);
    EXPECT_EQ(js.budget_tripped, 0);
    js.active_conn = NULL;
    js.active_life = NULL;
    js.active_timer = NULL;
    EXPECT_EQ(eval_int("globalThis.__a9_spin|0"), 1);
    hl_req_life_end(life);
    cleanup_js();
    be->free(actx);
}

/* L2: conn.dialect inherited from Object.prototype - a key the backend
 * leaves out (identitySequence on SQLite) read whatever app code had put
 * there. It has no prototype now, and is frozen with the connection. */
UTEST(js_audit9, conn_dialect_has_no_prototype)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    char *r = eval_str(
        "(() => {\n"
        "  Object.prototype.identitySequence = 'evil';\n"
        "  const d = globalThis.db.dialect;\n"
        "  const out = [Object.getPrototypeOf(d) === null,\n"
        "               d.identitySequence === undefined,\n"
        "               typeof d.supportsReturning === 'boolean',\n"
        "               Object.isFrozen(d)].join(',');\n"
        "  delete Object.prototype.identitySequence;\n"
        "  return out;\n"
        "})()");
    EXPECT_TRUE(r && strcmp(r, "true,true,true,true") == 0);
    if (r && strcmp(r, "true,true,true,true") != 0) printf("  got: %s\n", r);
    free(r);
    cleanup_js_caps();
}

/* Parity with Lua (G5 H3): verifyPassword ran a PBKDF2 of up to 10M
 * iterations - the count read from the stored string - in one uncharged
 * call, and a loop of digests over megabytes was charged a step per call.
 * Both are charged now, before the work: over the budget, the call is an
 * uncatchable interrupt and does no derivation. */
UTEST(js_audit9, crypto_work_is_charged_to_the_budget)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    js.max_instructions = 1000000;
    hl_js_reset_request(&js);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    EXPECT_EQ(eval_int(
        "(() => { try { return crypto.verifyPassword('x', 'pbkdf2:10000000:'"
        " + '0'.repeat(32) + ':' + '0'.repeat(64)) ? 1 : 2; }"
        " catch (e) { return 3; } })()"), -9999);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    EXPECT_EQ(js.budget_tripped, 1);
    /* Refused before deriving: 10M iterations take seconds. */
    long ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 +
              (long)(t1.tv_nsec - t0.tv_nsec) / 1000000;
    EXPECT_LT(ms, 1500L);

    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int(
        "(() => { const s = 'x'.repeat(1 << 20); let n = 0;"
        " try { for (let i = 0; i < 1000; i++) { crypto.sha256(s); n++; } }"
        " catch (e) {} return n; })()"), -9999);
    EXPECT_EQ(js.budget_tripped, 1);

    /* An ordinary hash-and-verify fits a normal budget. */
    js.max_instructions = 100000000;
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int(
        "crypto.verifyPassword('pw', crypto.hashPassword('pw')) ? 1 : 0"), 1);
    cleanup_js();
}

/* ── Audit 9 follow-up: the JS twins of the Lua response fixes ─────────── */

#include "hull/limits/runtime.h"   /* HL_RES_HEADER_BYTES_MAX */
#include <strings.h>               /* strncasecmp */

#ifdef HL_ENABLE_HTTP_SERVER
/* How many @p name headers the response carries (case-insensitive). */
static int a9_header_count(const KlHttpResponse *res, const char *name)
{
    int n = 0;
    size_t nl = strlen(name);
    const char *p = res->hdr_buf, *end = res->hdr_buf + res->hdr_len;
    while (p && p < end) {
        if ((size_t)(end - p) > nl && p[nl] == ':' && strncasecmp(p, name, nl) == 0)
            n++;
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) break;
        p = eol + 1;
    }
    return n;
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* Does the response's header block contain @p needle? */
static int a9_headers_contain(const KlHttpResponse *res, const char *needle)
{
    if (!res->hdr_buf) return 0;
    char *copy = malloc(res->hdr_len + 1);
    if (!copy) return 0;
    memcpy(copy, res->hdr_buf, res->hdr_len);
    copy[res->hdr_len] = 0;
    int found = strstr(copy, needle) != NULL;
    free(copy);
    return found;
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* M1: res.header appended to Keel's header buffer, outside the script heap
 * and with no cap. Past HL_RES_HEADER_BYTES_MAX it raises now. */
UTEST(js_audit9, response_headers_are_capped)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => { const v = 'v'.repeat(100); let n = 0;"
        " try { for (let i = 0; i < 100000; i++) { res.header('X-A', v); n++; }"
        "   globalThis.__a9_e = 'none'; }"
        " catch (e) { globalThis.__a9_e = e.message; }"
        " globalThis.__a9_n = n; return 1; }", &req, &res), 1);
    char *e = eval_str("globalThis.__a9_e");
    EXPECT_TRUE(e && strstr(e, "would exceed") != NULL);
    free(e);
    EXPECT_LT(eval_int("globalThis.__a9_n"), 1000);
    EXPECT_LE(res.hdr_len, (size_t)HL_RES_HEADER_BYTES_MAX);
    free_req_ctx(&req);
    kl_http_response_free(&res);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* res.json / html / text appended a Content-Type on every call - after an
 * app's own, and once more per repeated call. They set one only when the
 * response has none now (the app's wins), so a loop of res.text neither
 * stacks headers nor hits the cap. */
UTEST(js_audit9, one_content_type_and_the_apps_wins)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => { res.header('Content-Type', 'application/problem+json');"
        " res.json({ a: 1 }); res.html('<p>'); "
        " for (let i = 0; i < 10000; i++) res.text('x'); return 1; }",
        &req, &res), 1);
    EXPECT_FALSE(JS_HasException(js.ctx));
    EXPECT_EQ(a9_header_count(&res, "Content-Type"), 1);
    EXPECT_TRUE(a9_headers_contain(&res, "application/problem+json"));
    EXPECT_EQ(res.body_len, (size_t)1);
    free_req_ctx(&req);
    kl_http_response_free(&res);

    /* With none set, the helper's own type, once (a fresh runtime: the
     * manifest is declared once per app). */
    cleanup_js();
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpResponse res2;
    ASSERT_EQ(kl_http_response_init(&res2, &alloc), 0);
    KlHttpRequest req2 = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => { res.text('a'); res.text('b'); return 1; }",
        &req2, &res2), 1);
    EXPECT_EQ(a9_header_count(&res2, "Content-Type"), 1);
    EXPECT_TRUE(a9_headers_contain(&res2, "text/plain"));
    free_req_ctx(&req2);
    kl_http_response_free(&res2);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* M2: res.text / json / html / bytes copy (and may gzip) the body inside one
 * call, which the interrupt handler never saw: a loop of them over a large
 * string ran unbounded. The body is charged before the copy now. */
UTEST(js_audit9, response_bodies_are_charged)
{
    static const char *const srcs[] = {
        "(req, res) => { const s = 'x'.repeat(1 << 20);"
        " for (let i = 0; i < 1000; i++) res.text(s); return 1; }",
        "(req, res) => { const s = 'x'.repeat(1 << 20);"
        " for (let i = 0; i < 1000; i++) res.html(s); return 1; }",
        "(req, res) => { const s = 'x'.repeat(1 << 20);"
        " for (let i = 0; i < 1000; i++) res.json(s); return 1; }",
        "(req, res) => { const b = new ArrayBuffer(1 << 22);"
        " for (let i = 0; i < 1000; i++) res.bytes(b); return 1; }",
    };
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        js.max_instructions = 1000000;
        KlAllocator alloc = kl_allocator_default();
        KlHttpResponse res;
        ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
        KlHttpRequest req = {0};
        EXPECT_NE(a5_middleware(srcs[i], &req, &res), 1);
        EXPECT_EQ_MSG(js.budget_tripped, 1, srcs[i]);
        free_req_ctx(&req);
        kl_http_response_free(&res);
        cleanup_js();
    }
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* ── Audit 10 (the JS twins of the Lua runtime items) ─────────────────── */

#include "hull/cap/smtp.h"   /* HlSmtpConfig */
#include "hull/cap/http.h"   /* HlHttpConfig */
#include "hull/cap/fs.h"     /* HlFsConfig */

/* Run @p code as a module under a budget of @p limit; 1 if the run went
 * over it. */
static int a10_run_tripped(const char *code, int64_t limit)
{
    js.max_instructions = limit;
    hl_js_budget_arm(&js);
    JSValue v = JS_Eval(js.ctx, code, strlen(code), "<a10>", JS_EVAL_TYPE_MODULE);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);
    int tripped = js.budget_tripped;
    js.max_instructions = 0;
    hl_js_budget_arm(&js);
    return tripped;
}

/* L: fs.write wrote up to the whole heap as one interrupt step. Charged
 * before the write now (one unit per 8 bytes), so the charge holds even for
 * a write the policy then refuses. */
UTEST(js_audit10, fs_write_is_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_a10"), NULL);
    HlFsConfig fs = { tmpdir, strlen(tmpdir), NULL };   /* no grants */
    js.base.fs_cfg = &fs;
    EXPECT_EQ(a10_run_tripped("globalThis.BIG = 'x'.repeat(8 * 1000000);", 0), 0);
    EXPECT_EQ(a10_run_tripped(
        "for (let i = 0; i < 10; i++) { try { fs.write('a.bin', BIG); } catch (e) {} }",
        200000), 1);
    EXPECT_EQ(a10_run_tripped(
        "try { fs.write('a.bin', 'x'); } catch (e) {} globalThis.__a10_small = 1;",
        200000), 0);
    EXPECT_EQ(eval_int("globalThis.__a10_small|0"), 1);
    js.base.fs_cfg = NULL;
    cleanup_js_caps();
    rmdir(tmpdir);
}

/* ── Audit 15: off-heap whole-object reads, tar.create / tar.pack ──────
 *
 * fs.read, blob.get and tar.pack read whole files into buffers from the
 * server's allocator (no VM limit) and charged nothing for the read; a blob
 * reader's read(n) was not charged; tar.create sized arrays by an app
 * array's `length` and tar.create / tar.pack built the archive off-heap,
 * uncapped. Each is held to the heap limit before it allocates and charged
 * before it reads now. The budgets below sit between the old cost (the copy
 * into the heap, 1 unit per 64 bytes) and the new one (1 per 8 bytes read). */

#include "hull/cap/fs_policy.h"   /* HlFsPolicy (a15 fixture) */
#include "hull/cap/blob.h"        /* hl_cap_blob_live_handles (a34) */
#include "hull/utils/alloc.h"     /* hl_alloc_init */

typedef struct {
    char        dir[HL_TEST_PATH_MAX];
    HlAllocator alloc;
    HlFsPolicy  policy;
    HlFsConfig  fs;
} A15Fs;

#define A15_BIG (8 * 1024 * 1024)

/* A temp app root with fs.read + fs.write over all of it and an 8 MB
 * big.bin, wired into the caps runtime. */
static int a15_fs_open(A15Fs *f)
{
    static const char *const all[] = { "." };
    memset(f, 0, sizeof *f);
    if (!hl_test_mkdtemp(f->dir, sizeof f->dir, "hull_a15")) return -1;
    hl_alloc_init(&f->alloc, 0);
    f->policy = HL_FS_POLICY_INIT;
    const char *perr = NULL;
    if (hl_fs_policy_compile_manifest(f->dir, &f->alloc, all, 1, all, 1,
                                      &f->policy, &perr) != 0)
        return -1;
    f->fs.base_dir = f->dir;
    f->fs.base_len = strlen(f->dir);
    f->fs.policy = &f->policy;
    char p[HL_TEST_PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/big.bin", f->dir);
    FILE *fp = fopen(p, "wb");
    if (!fp) return -1;
    static char chunk[65536];
    memset(chunk, 'x', sizeof chunk);
    for (int i = 0; i < A15_BIG / (int)sizeof chunk; i++)
        fwrite(chunk, 1, sizeof chunk, fp);
    fclose(fp);
    js.base.fs_cfg = &f->fs;
    return 0;
}

static void a15_fs_close(A15Fs *f)
{
    js.base.fs_cfg = NULL;
    hl_fs_policy_free(&f->policy);
    nftw(f->dir, jbc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* The message of what @p expr throws, or "no throw" (caller frees). */
static char *a15_throws(const char *expr)
{
    char code[1024];
    snprintf(code, sizeof code,
             "(() => { try { %s; } catch (e) { return String(e && e.message); } "
             "return 'no throw'; })()", expr);
    return eval_str(code);
}

UTEST(js_audit15, fs_read_is_charged_and_bounded)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    /* One 8 MB read: 1M units now, ~131k for the copy before. */
    EXPECT_EQ(a10_run_tripped("fs.read('big.bin');", 500000), 1);
    EXPECT_EQ(eval_int("fs.read('big.bin').byteLength === 8 * 1024 * 1024 ? 1 : 0"), 1);
    /* A file larger than the heap limit is refused before any allocation. */
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 4 * 1024 * 1024;
    char *r = a15_throws("fs.read('big.bin')");
    EXPECT_TRUE(r && strstr(r, "file_too_large") != NULL);
    if (r && !strstr(r, "file_too_large")) printf("  got: %s\n", r);
    free(r);
    js.max_heap_bytes = saved;
    cleanup_js_caps();
    a15_fs_close(&f);
}

UTEST(js_audit15, blob_reads_are_charged_and_clamped)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    ASSERT_EQ(a10_run_tripped(
        "import { blob } from 'hull:blob';\n"
        "globalThis.blob = blob; blob.init({ dir: 'blobs' });\n"
        "globalThis.ID = blob.put('b'.repeat(4 * 1024 * 1024)).id;\n", 0), 0);
    EXPECT_EQ(eval_int("typeof ID === 'string' ? 1 : 0"), 1);
    /* blob.get of 4 MB: 512k units for the read + hash now. */
    EXPECT_EQ(a10_run_tripped("blob.get(ID);", 300000), 1);
    /* reader.read(n): charged by n (clamped) before the read. */
    EXPECT_EQ(a10_run_tripped("blob.reader(ID).read(8 * 1024 * 1024);", 500000), 1);
    /* ...and clamped: a terabyte was allocated as asked (and failed). */
    EXPECT_EQ(eval_int("(() => { const r = blob.reader(ID);"
                       " const n = r.read(2 ** 40).byteLength; r.close();"
                       " return n === 4 * 1024 * 1024 ? 1 : 0; })()"), 1);
    /* A blob larger than the heap limit is refused before it is read. */
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 2 * 1024 * 1024;
    char *r = a15_throws("blob.get(ID)");
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    if (r && !strstr(r, "heap limit")) printf("  got: %s\n", r);
    free(r);
    js.max_heap_bytes = saved;
    cleanup_js_caps();
    a15_fs_close(&f);
}

/* Audit 34: a blob writer holds two descriptors and a reader one, and
 * opening one cost almost nothing - 61433 writers exhausted the process's
 * descriptor limit (accept(), SQLite temp files and TLS with it). Live
 * handles are capped now, a dropped one is collected to make room, and the
 * open is charged. */
UTEST(js_audit34, blob_handles_are_capped_collected_and_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    ASSERT_EQ(hl_cap_blob_live_handles(), 0);
    ASSERT_EQ(a10_run_tripped(
        "import { blob } from 'hull:blob';\n"
        "globalThis.blob = blob; blob.init({ dir: 'blobs' });\n"
        "globalThis.ID = blob.put('abc').id;\n", 0), 0);
    /* Held handles: the cap, then a clear refusal. */
    EXPECT_EQ(eval_int("(() => { globalThis.H = []; try {"
                       " for (let i = 0; i < 100000; i++)"
                       "   H.push(i % 2 ? blob.writer() : blob.reader(ID));"
                       " } catch (e) { globalThis.E = String(e.message); }"
                       " return H.length; })()"), HL_BLOB_MAX_LIVE_HANDLES);
    char *e = eval_str("E");
    EXPECT_TRUE(e && strstr(e, "too many open blob") != NULL);
    free(e);
    EXPECT_EQ(hl_cap_blob_live_handles(), HL_BLOB_MAX_LIVE_HANDLES);
    /* Closing gives the slot back exactly once (a second abort / close is a
     * no-op). */
    EXPECT_EQ(eval_int("(() => { H[1].abort(); H[1].abort(); H[0].close();"
                       " H[0].close(); return 1; })()"), 1);
    EXPECT_EQ(hl_cap_blob_live_handles(), HL_BLOB_MAX_LIVE_HANDLES - 2);
    /* Dropped without close: the open collects them and goes ahead. */
    EXPECT_EQ(eval_int("(() => { H = null; const w = blob.writer();"
                       " w.write('x'); const r = w.finalize();"
                       " return typeof r.id === 'string' ? 1 : 0; })()"), 1);
    EXPECT_TRUE(hl_cap_blob_live_handles() < 8);
    /* The open is charged: 200 opens are over a 100k budget. */
    EXPECT_EQ(a10_run_tripped(
        "for (let i = 0; i < 200; i++) blob.writer().abort();", 100000), 1);
    cleanup_js_caps();
    EXPECT_EQ(hl_cap_blob_live_handles(), 0);
    a15_fs_close(&f);
}

UTEST(js_audit15, tar_is_capped_at_the_heap_and_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    ASSERT_EQ(a10_run_tripped(
        "import { tar } from 'hull:archive:tar';\n"
        "globalThis.tar = tar; globalThis.MB = new ArrayBuffer(1024 * 1024);\n", 0), 0);
    /* 100 entries naming one 1 MB buffer: a 100 MB archive, built off-heap
     * before. Refused before it is built now. */
    char *r = a15_throws("tar.create(Array.from({ length: 100 },"
                         " (_, i) => ({ name: 'f' + i, data: MB })))");
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    if (r && !strstr(r, "heap limit")) printf("  got: %s\n", r);
    free(r);
    /* A sparse array's length sized six arrays (2e8: gigabytes, off-heap). */
    r = a15_throws("(() => { const a = []; a.length = 2e8; tar.create(a); })()");
    EXPECT_TRUE(r && strstr(r, "too many entries") != NULL);
    if (r && !strstr(r, "too many entries")) printf("  got: %s\n", r);
    free(r);
    /* The build is charged (1 unit per 64 bytes). */
    EXPECT_EQ(a10_run_tripped("tar.create(Array.from({ length: 16 },"
                              " (_, i) => ({ name: 'f' + i, data: MB })));", 200000), 1);
    /* tar.pack: the same 8 MB file ten times is 80 MB, read into malloc'd
     * buffers before; refused by the running total now (here against a
     * 20 MB limit: at the third file, two read). */
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 20 * 1024 * 1024;
    r = a15_throws("tar.pack(Array.from({ length: 10 },"
                   " (_, i) => ({ path: 'big.bin', name: 'b' + i })))");
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    if (r && !strstr(r, "heap limit")) printf("  got: %s\n", r);
    free(r);
    js.max_heap_bytes = saved;
    /* ...and each read is charged: one 8 MB file is 1M units now. */
    EXPECT_EQ(a10_run_tripped("tar.pack(['big.bin']);", 500000), 1);
    EXPECT_EQ(eval_int("tar.pack(['big.bin']).byteLength > 8 * 1024 * 1024 ? 1 : 0"), 1);
    cleanup_js_caps();
    a15_fs_close(&f);
}

/* tar.extract wrote every entry to disk with no charge: an archive of any
 * size (a MappedBuffer of up to 1 GiB) cost a handful of steps. Each entry
 * is charged before it is written now, as fs.write is (audit 16). */
UTEST(js_audit16, tar_extract_is_charged_per_entry)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    ASSERT_EQ(a10_run_tripped(
        "import { tar } from 'hull:archive:tar';\n"
        "globalThis.tar = tar;\n"
        "const mb = new ArrayBuffer(1024 * 1024);\n"
        "globalThis.TB = tar.create(Array.from({ length: 8 },"
        " (_, i) => ({ name: 'f' + (i + 1), data: mb })));\n", 0), 0);
    /* 8 MB to write: ~1M units; each 1 MB entry is 131073, so the budget
     * runs out at the fourth entry, before it is written. */
    EXPECT_EQ(a10_run_tripped("tar.extract(TB, 'out');", 500000), 1);
    char p[HL_TEST_PATH_MAX + 32];
    struct stat st;
    snprintf(p, sizeof p, "%s/out/f1", f.dir);
    EXPECT_EQ(0, stat(p, &st));
    snprintf(p, sizeof p, "%s/out/f8", f.dir);
    EXPECT_NE(0, stat(p, &st));
    EXPECT_EQ(eval_int("tar.extract(TB, 'out2') === true ? 1 : 0"), 1);
    snprintf(p, sizeof p, "%s/out2/f8", f.dir);
    EXPECT_EQ(0, stat(p, &st));
    EXPECT_EQ(eval_int("(globalThis.TB = undefined, 1)"), 1);
    cleanup_js_caps();
    a15_fs_close(&f);
}

/* Headers of a typeflag the parser skips (here '1', a hard link) were
 * checksummed and never surfaced, so the per-entry charges missed them: a
 * 10 MB archive of them cost nothing. The input is charged up front now,
 * one unit per 64 bytes (audit 17). */
UTEST(js_audit17, tar_skipped_headers_are_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    ASSERT_EQ(a10_run_tripped(
        "import { tar } from 'hull:archive:tar';\n"
        "globalThis.tar = tar;\n"
        "const h = new Uint8Array(tar.create([{ name: 'l', data: new ArrayBuffer(0) }])"
        ".slice(0, 512));\n"
        "let s = ''; for (let i = 148; i < 156; i++) s += String.fromCharCode(h[i]);\n"
        "const f = (parseInt(s.match(/[0-7]+/)[0], 8) + 1).toString(8).padStart(6, '0');\n"
        "for (let i = 0; i < 6; i++) h[148 + i] = f.charCodeAt(i);\n"
        "h[154] = 0; h[155] = 32; h[156] = 49;\n"
        "const big = new Uint8Array(512 * 20000);\n"
        "for (let i = 0; i < 20000; i++) big.set(h, i * 512);\n"
        "globalThis.LB = big.buffer;\n"
        "globalThis.__a17_n = tar.parse(LB).length;\n", 0), 0);
    EXPECT_EQ(eval_int("globalThis.LB && globalThis.__a17_n === 0 ? 1 : 0"), 1);
    /* 10 MB walked: 160000 units. */
    EXPECT_EQ(a10_run_tripped("tar.parse(LB);", 100000), 1);
    EXPECT_EQ(a10_run_tripped("tar.extract(LB, 'out');", 100000), 1);
    EXPECT_EQ(eval_int("tar.extract(LB, 'out') === true ? 1 : 0"), 1);
    EXPECT_EQ(eval_int("(globalThis.LB = undefined, 1)"), 1);
    cleanup_js_caps();
    a15_fs_close(&f);
}

/* hull:encoding's C codecs malloc'd every buffer off the heap and charged
 * only the final string: an 8 MB hex encode built 16 MB outside the heap
 * limit for ~262k units of allocation. The output is checked against the
 * heap limit and the bytes charged before anything is allocated now
 * (audit 17). */
UTEST(js_audit17, encoding_codecs_are_bounded_and_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a10_run_tripped(
        "import { encoding } from 'hull:encoding';\n"
        "globalThis.enc = encoding;\n"
        "globalThis.EB = new ArrayBuffer(8 * 1024 * 1024);\n", 0), 0);
    /* 8 MB in, 16 MB out: 3M units. */
    EXPECT_EQ(a10_run_tripped("enc.hex.encode(EB);", 1000000), 1);
    EXPECT_EQ(a10_run_tripped("enc.base64.encode(EB);", 1000000), 1);
    EXPECT_EQ(eval_int("enc.hex.encode(new Uint8Array([1, 255])) === '01ff' ? 1 : 0"), 1);
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 4 * 1024 * 1024;
    char *r = a15_throws("enc.hex.encode(EB)");
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    if (r && !strstr(r, "heap limit")) printf("  got: %s\n", r);
    free(r);
    js.max_heap_bytes = saved;
    EXPECT_EQ(eval_int("(globalThis.EB = undefined, 1)"), 1);
    cleanup_js_caps();
}

#ifdef HL_ENABLE_HTTP_SERVER
/* ── Audit 14: SSE and WebSocket writes are charged ────────────────────
 *
 * stream.event / stream.comment wrote every line of their text as three
 * stream writes, conn.send / ws.broadcast framed and queued their message
 * (to every connection), all inside one uncharged call. Each is charged
 * before the write now: 1 unit per 8 bytes, plus 1 per SSE line / per frame,
 * per destination. A tripped charge stops the call before Keel is reached. */

#include "../../../../src/hull/runtime/js/mod_buffer.h"   /* HlJSSseStreamUD */
#include "hull/cap/ws.h"                                  /* HlWsRegistry */

static int a14_sse_writes;
static int a14_sse_write(void *ctx, const char *data, size_t len)
{
    (void)ctx; (void)data; (void)len;
    a14_sse_writes++;
    return 0;
}

static void a14_set_global(const char *name, JSValue v)
{
    JSValue g = JS_GetGlobalObject(js.ctx);
    JS_SetPropertyStr(js.ctx, g, name, v);
    JS_FreeValue(js.ctx, g);
}

UTEST(js_audit14, sse_and_ws_writes_are_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);

    /* A stream whose writes land in a counter (no request: a NULL life is
     * live). */
    JSValue s = JS_NewObjectClass(js.ctx, (int)js_sse_stream_class_id);
    ASSERT_FALSE(JS_IsException(s));
    HlJSSseStreamUD *ud = js_mallocz(js.ctx, sizeof *ud);
    ASSERT_TRUE(ud != NULL);
    ud->sse.write_fn = a14_sse_write;
    JS_SetOpaque(s, ud);
    a14_set_global("S", s);
    ASSERT_EQ(a10_run_tripped("globalThis.NL = '\\n'.repeat(1000000);"
                              " globalThis.BIG = 'w'.repeat(8000000);", 0), 0);
    a14_sse_writes = 0;
    EXPECT_EQ(a10_run_tripped("S.event('e', NL);", 200000), 1);
    EXPECT_EQ(a10_run_tripped("S.comment(NL);", 200000), 1);
    EXPECT_EQ(a14_sse_writes, 0);   /* stopped before Keel wrote a byte */
    EXPECT_EQ(a10_run_tripped("S.event('e', 'a\\nb', '7'); S.comment('hi');", 200000), 0);
    EXPECT_GT(a14_sse_writes, 0);

    /* ws.broadcast is charged per connection on the path (these have no Keel
     * connection, so nothing is sent): 100 x 1 MB. */
    HlAllocator wa;
    hl_alloc_init(&wa, 0);
    HlWsRegistry reg;
    hl_ws_registry_init(&reg, &wa);
    for (int i = 0; i < 100; i++)
        ASSERT_TRUE(hl_ws_registry_add(&reg, "/ws", NULL) != NULL);
    js.base.ws_registry = &reg;
    ASSERT_EQ(a10_run_tripped(
        "import { wsServer } from 'hull:web:ws-server';\n"
        "globalThis.ws = wsServer; globalThis.MB = 'm'.repeat(1024 * 1024);\n", 0), 0);
    EXPECT_EQ(a10_run_tripped("ws.broadcast('/ws', MB);", 1000000), 1);
    EXPECT_EQ(a10_run_tripped("globalThis.__a14_sent = ws.broadcast('/ws', 'hi');",
                              1000000), 0);
    EXPECT_EQ(eval_int("globalThis.__a14_sent"), 0);

    /* conn.send: the charge trips before Keel is reached, so a connection
     * whose Keel handle is a placeholder is never written to. */
    static char placeholder;
    HlWsConn *c = hl_ws_registry_add(&reg, "/one", (KlWsServerConn *)(void *)&placeholder);
    ASSERT_TRUE(c != NULL);
    JSValue cv = hl_js_ws_push_conn(js.ctx, c);
    ASSERT_FALSE(JS_IsException(cv));
    a14_set_global("C", cv);
    EXPECT_EQ(a10_run_tripped("C.send(BIG);", 200000), 1);
    EXPECT_EQ(a10_run_tripped("C.sendBinary(BIG);", 200000), 1);
    EXPECT_EQ(a10_run_tripped("C.sendBinary(new ArrayBuffer(8000000));", 200000), 1);
    hl_js_ws_invalidate_conn(js.ctx, c);

    js.base.ws_registry = NULL;
    hl_ws_registry_free(&reg);
    cleanup_js_caps();
}

/* Audit 18 L11: when the conn object (or the message string) could not be
 * made - out of memory - on_open / on_message / on_close passed JS_EXCEPTION
 * to the handler as its connection. They skip the call now, with the
 * exception cleared, and on_close still removes the connection. The heap is
 * pinned at what it holds, so the conn object cannot be allocated. */
static void a18_ws_route_handler(int id, const char *src)
{
    /* The routes table is made by the first app.get / app.ws; a bare test
     * context has none yet. */
    JSValue store = hl_js_internals(js.ctx);
    JSValue routes = JS_GetPropertyStr(js.ctx, store, "__hull_routes");
    if (!JS_IsArray(js.ctx, routes)) {
        JS_FreeValue(js.ctx, routes);
        routes = JS_NewArray(js.ctx);
        JS_SetPropertyStr(js.ctx, store, "__hull_routes",
                          JS_DupValue(js.ctx, routes));
    }
    JSValue fn = JS_Eval(js.ctx, src, strlen(src), "<a18ws>",
                         JS_EVAL_TYPE_GLOBAL);
    JS_SetPropertyUint32(js.ctx, routes, (uint32_t)id, fn);
    JS_FreeValue(js.ctx, routes);
    JS_FreeValue(js.ctx, store);
}

UTEST(js_audit18, ws_callbacks_skip_a_conn_that_could_not_be_made)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    HlAllocator wa;
    hl_alloc_init(&wa, 0);
    HlWsRegistry reg;
    hl_ws_registry_init(&reg, &wa);
    js.base.ws_registry = &reg;
    ASSERT_EQ(eval_int("globalThis.calls = 0; 1"), 1);
    a18_ws_route_handler(0, "(c) => { calls++; }");
    a18_ws_route_handler(1, "(c, m) => { calls++; }");
    a18_ws_route_handler(2, "(c, code) => { calls++; }");
    ASSERT_FALSE(JS_HasException(js.ctx));
    HlJSWsRoute route;
    memset(&route, 0, sizeof route);
    route.js = &js;
    route.on_open_id = 0;
    route.on_message_id = 1;
    route.on_close_id = 2;
    snprintf(route.path, sizeof route.path, "/a18");
    static char p1, p2;
    KlWsServerConn *kc = (KlWsServerConn *)(void *)&p1;

    /* Control: with memory to spare, every callback reaches its handler. */
    KlWsServerConn *ok = (KlWsServerConn *)(void *)&p2;
    hl_js_ws_on_open(ok, &route);
    hl_js_ws_on_message(ok, "hello", 5, 0, &route);
    hl_js_ws_on_close(ok, 1000, NULL, 0, &route);
    EXPECT_EQ(eval_int("calls"), 3);
    EXPECT_TRUE(hl_ws_registry_find(&reg, "/a18", ok) == NULL);
    ASSERT_EQ(eval_int("globalThis.calls = 0; 1"), 1);

    /* QuickJS serves small blocks from arenas, and the memory limit is
     * checked only when a new arena is needed: pin the limit, then fill the
     * free object slots with a chain of objects (kept alive) until the next
     * arena is refused. The conn object then cannot be made. */
    ASSERT_EQ(eval_int("globalThis.keep = null;"
                       " globalThis.fill = function () { let h = null;"
                       "   try { for (;;) h = { n: h }; } catch (e) {}"
                       "   globalThis.keep = h; };"
                       " 1"), 1);
    JS_RunGC(js.rt);
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(js.rt, &u);
    JS_SetMemoryLimit(js.rt, (size_t)u.malloc_size + (256u << 10));
    JSValue g = JS_GetGlobalObject(js.ctx);
    JSValue fill = JS_GetPropertyStr(js.ctx, g, "fill");
    JS_FreeValue(js.ctx, JS_Call(js.ctx, fill, JS_UNDEFINED, 0, NULL));
    JS_FreeValue(js.ctx, fill);
    JS_FreeValue(js.ctx, g);
    JS_FreeValue(js.ctx, JS_GetException(js.ctx));
    hl_js_ws_on_open(kc, &route);
    EXPECT_FALSE(JS_HasException(js.ctx));
    HlWsConn *c = hl_ws_registry_find(&reg, "/a18", kc);
    EXPECT_TRUE(c != NULL);
    if (c) EXPECT_TRUE(c->user_data == NULL);
    hl_js_ws_on_message(kc, "hello", 5, 0, &route);
    EXPECT_FALSE(JS_HasException(js.ctx));
    hl_js_ws_on_close(kc, 1000, NULL, 0, &route);
    EXPECT_FALSE(JS_HasException(js.ctx));
    JS_SetMemoryLimit(js.rt, (size_t)u.malloc_limit);
    ASSERT_EQ(eval_int("globalThis.keep = null; 1"), 1);
    JS_RunGC(js.rt);

    EXPECT_TRUE(hl_ws_registry_find(&reg, "/a18", kc) == NULL);
    EXPECT_EQ(eval_int("calls"), 0);

    js.base.ws_registry = NULL;
    hl_ws_registry_free(&reg);
    cleanup_js();
}

/* The outbound twin (hull:web:ws-client): conn.send / conn.sendBinary are
 * charged the same way before Keel is reached, so a client whose Keel handle
 * is a placeholder is never written to (audit 15 follow-up: the charge #748
 * added had no test). */
UTEST(js_audit14, ws_client_sends_are_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a10_run_tripped("globalThis.BIG = 'w'.repeat(8000000);", 0), 0);

    static char placeholder;
    JSValue o = JS_NewObjectClass(js.ctx, (int)js_ws_client_conn_class_id);
    ASSERT_FALSE(JS_IsException(o));
    HlJSWsClientUD *ud = js_mallocz(js.ctx, sizeof *ud);
    ASSERT_TRUE(ud != NULL);
    ud->client = (struct KlWsClientConn *)(void *)&placeholder;
    ud->on_open = ud->on_message = JS_UNDEFINED;
    ud->on_close = ud->on_error = JS_UNDEFINED;
    ud->self_ref = JS_UNDEFINED;
    ud->ctx = js.ctx;
    ud->js = &js;
    JS_SetOpaque(o, ud);
    a14_set_global("WC", o);

    EXPECT_EQ(a10_run_tripped("WC.send(BIG);", 200000), 1);
    EXPECT_EQ(a10_run_tripped("WC.sendBinary(BIG);", 200000), 1);
    EXPECT_EQ(a10_run_tripped("WC.sendBinary(new ArrayBuffer(8000000));", 200000), 1);

    /* Nothing reached Keel: the finalizer must not free the placeholder. */
    ud->client = NULL;
    ud->closed = 1;
    cleanup_js_caps();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_CLIENT
/* L: smtp.send dropped, silently, a cc entry that was not a string or whose
 * conversion failed, and never checked the copy: the message went out to
 * fewer recipients than asked. Every entry is copied now, or it throws. */
UTEST(js_audit10, smtp_cc_is_copied_whole_or_refused)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlSmtpConfig smtp = {0};
    js.base.smtp_cfg = &smtp;
    static const char *const pre =
        "(() => { const o = { host: 'mail.invalid', from: 'a@x.test', "
        "  to: 'b@x.test', subject: 's', body: 'b' }; ";
    char code[1024];

    snprintf(code, sizeof code, "%s o.cc = ['c@x.test', 42]; "
             "try { smtp.send(o); return 0; } catch (e) { "
             "  return /cc\\[1\\] must be a string/.test(e.message) ? 1 : 2; } })()",
             pre);
    EXPECT_EQ(eval_int(code), 1);

    snprintf(code, sizeof code, "%s o.cc = 'c@x.test'; "
             "try { smtp.send(o); return 0; } catch (e) { "
             "  return /cc must be an array/.test(e.message) ? 1 : 2; } })()", pre);
    EXPECT_EQ(eval_int(code), 1);

    snprintf(code, sizeof code, "%s o.cc = ['c@x.test', { toString() { "
             "throw new Error('boom'); } }]; "
             "try { smtp.send(o); return 0; } catch (e) { return 1; } })()", pre);
    EXPECT_EQ(eval_int(code), 1);

    js.base.smtp_cfg = NULL;
    cleanup_js_caps();
}

/* httpClient opts.timeoutMs: a positive number of milliseconds, or absent /
 * undefined / null. Anything else throws before the request starts (a
 * mistyped timeout must not silently become the 30 s default). The clamp
 * itself is the cap layer's (test_http.c timeout.*). */
UTEST(js_http_timeout, per_call_option_is_validated)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlHttpConfig cfg = {0};          /* no hosts: every request is refused */
    js.base.http_cfg = &cfg;
    static const char *const bad[] = {
        "http.get('http://x.invalid/', { timeoutMs: 0 })",
        "http.get('http://x.invalid/', { timeoutMs: -5 })",
        "http.get('http://x.invalid/', { timeoutMs: NaN })",
        "http.get('http://x.invalid/', { timeoutMs: '1000' })",
        "http.post('http://x.invalid/', 'b', { timeoutMs: {} })",
        "http.delete('http://x.invalid/', { timeoutMs: true })",
        "http.request('GET', 'http://x.invalid/', { timeoutMs: 0 })",
    };
    static const char *const good[] = {
        "http.get('http://x.invalid/', { timeoutMs: 1000 })",
        "http.get('http://x.invalid/', { timeoutMs: 1500.7 })",
        "http.put('http://x.invalid/', 'b', { timeoutMs: 1e12 })",
        "http.get('http://x.invalid/', { timeoutMs: undefined })",
        "http.get('http://x.invalid/', { timeoutMs: null })",
    };
    char code[1024];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(code, sizeof code,
                 "import { httpClient as http } from 'hull:http-client';\n"
                 "let r = 0;\n"
                 "try { %s; r = 2; } catch (e) {\n"
                 "  r = /opts\\.timeoutMs must be a positive number/.test(e.message)"
                 " ? 1 : 3; }\n"
                 "globalThis.__ht = r;\n", bad[i]);
        JSValue v = JS_Eval(js.ctx, code, strlen(code), "<ht>", JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) hl_js_dump_error(&js);
        JS_FreeValue(js.ctx, v);
        hl_js_run_jobs(&js);
        EXPECT_EQ_MSG(eval_int("globalThis.__ht|0"), 1, bad[i]);
    }
    /* A valid value (any size: the cap clamps it) gets as far as the host
     * check, which refuses the host. */
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        snprintf(code, sizeof code,
                 "import { httpClient as http } from 'hull:http-client';\n"
                 "let r = 0;\n"
                 "try { %s; r = 2; } catch (e) {\n"
                 "  r = /failed/.test(e.message) && !/timeoutMs/.test(e.message)"
                 " ? 1 : 3; }\n"
                 "globalThis.__ht = r;\n", good[i]);
        JSValue v = JS_Eval(js.ctx, code, strlen(code), "<ht>", JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) hl_js_dump_error(&js);
        JS_FreeValue(js.ctx, v);
        hl_js_run_jobs(&js);
        EXPECT_EQ_MSG(eval_int("globalThis.__ht|0"), 1, good[i]);
    }
    js.base.http_cfg = NULL;
    cleanup_js_caps();
}

/* Audit 11: http.async.post / put / patch read opts.headers and
 * opts.timeoutMs through getters that run app code, and did not check for an
 * exception: the pending exception was stored as an opts property and the
 * request went on. The getter's own error is what the call throws now. */
UTEST(js_http_timeout, a_throwing_opts_getter_stops_the_async_call)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlHttpConfig cfg = {0};
    js.base.http_cfg = &cfg;
    static const char *const calls[] = {
        "http.async.post('http://x.invalid/', 'b', "
        "  { get headers() { throw new Error('boom-getter'); } })",
        "http.async.put('http://x.invalid/', 'b', "
        "  { get timeoutMs() { throw new Error('boom-getter'); } })",
        "http.async.patch('http://x.invalid/', 'b', { headers: {}, "
        "  get timeoutMs() { throw new Error('boom-getter'); } })",
    };
    char code[1024];
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        snprintf(code, sizeof code,
                 "import { httpClient as http } from 'hull:http-client';\n"
                 "let r = 0;\n"
                 "try { %s; r = 2; } catch (e) {\n"
                 "  r = e.message === 'boom-getter' ? 1 : 3; }\n"
                 "globalThis.__hg = r;\n", calls[i]);
        JSValue v = JS_Eval(js.ctx, code, strlen(code), "<hg>", JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) hl_js_dump_error(&js);
        JS_FreeValue(js.ctx, v);
        hl_js_run_jobs(&js);
        EXPECT_EQ_MSG(eval_int("globalThis.__hg|0"), 1, calls[i]);
    }
    js.base.http_cfg = NULL;
    cleanup_js_caps();
}

/* Audit 17: a header whose getter or toString throws was skipped with its
 * exception left pending, and the request went on without it. The header's
 * own error is what the call throws now, before the request starts (the
 * host check's "failed" would show it went on). */
UTEST(js_http_timeout, a_throwing_header_value_stops_the_call)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlHttpConfig cfg = {0};
    js.base.http_cfg = &cfg;
    static const char *const calls[] = {
        "http.get('http://x.invalid/', { headers: { a: 'ok', "
        "  b: { toString() { throw new Error('boom-hdr'); } } } })",
        "http.post('http://x.invalid/', 'b', { headers: "
        "  { get b() { throw new Error('boom-hdr'); } } })",
        "http.delete('http://x.invalid/', { headers: "
        "  { b: { toString() { throw new Error('boom-hdr'); } } } })",
        "http.request('GET', 'http://x.invalid/', { headers: "
        "  { get b() { throw new Error('boom-hdr'); } } })",
        "http.get('http://x.invalid/', "
        "  { get headers() { throw new Error('boom-hdr'); } })",
    };
    char code[1024];
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        snprintf(code, sizeof code,
                 "import { httpClient as http } from 'hull:http-client';\n"
                 "let r = 0;\n"
                 "try { %s; r = 2; } catch (e) {\n"
                 "  r = e.message === 'boom-hdr' ? 1 : 3; }\n"
                 "globalThis.__hh = r;\n", calls[i]);
        JSValue v = JS_Eval(js.ctx, code, strlen(code), "<hh>", JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) hl_js_dump_error(&js);
        JS_FreeValue(js.ctx, v);
        hl_js_run_jobs(&js);
        EXPECT_EQ_MSG(eval_int("globalThis.__hh|0"), 1, calls[i]);
    }
    js.base.http_cfg = NULL;
    cleanup_js_caps();
}
#endif

#ifdef HL_ENABLE_HTTP_SERVER
/* L: test.get runs the app's dispatch, an entry: it re-armed the budget (a
 * case looping over test.get never hit the limit) and its stale-transaction
 * guard rolled back a transaction the CASE had open. The case's budget is
 * kept and charged with the request's work now, and the guard is held off
 * while the case has a transaction. */
UTEST(js_audit10, nested_test_request_keeps_the_case_budget_and_txn)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *app =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "db.exec('CREATE TABLE a10 (x INTEGER)');\n"
        "app.get('/spin', (req, res) => { "
        "  for (let i = 0; i < 300000; i++) {} res.json({ ok: true }); });\n"
        "app.get('/write', (req, res) => { "
        "  db.exec('INSERT INTO a10 VALUES (2)'); res.json({ ok: true }); });\n";
    JSValue v = JS_Eval(js.ctx, app, strlen(app), "<a10app>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    KlHttpRouter router;
    KlAllocator kalloc = kl_allocator_default();
    kl_http_router_init(&router, &kalloc);
    ASSERT_EQ(hl_js_wire_routes(&js, &router), 0);
    hl_js_test_register(js.ctx, &router, &js);
    const char *cases =
        "test('loops over test.get', () => { "
        "  for (let i = 0; i < 10; i++) test.get('/spin'); });\n"
        "test('one request fits', () => { "
        "  if (test.get('/spin').status !== 200) throw new Error('status'); });\n"
        "test('inside db.batch', () => { "
        "  db.batch(() => { "
        "    db.exec('INSERT INTO a10 VALUES (1)'); "
        "    if (test.get('/write').status !== 200) throw new Error('status'); "
        "  }); "
        "  const rows = db.query('SELECT x FROM a10 ORDER BY x'); "
        "  if (rows.length !== 2) throw new Error('rows: ' + rows.length); });\n";
    v = JS_Eval(js.ctx, cases, strlen(cases), "<a10cases>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);

    js.max_instructions = 1000000;   /* polls weigh 10000 each */
    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[4];
    memset(results, 0, sizeof results);
    hl_js_test_run(js.ctx, &total, &passed, &failed, NULL, results, 4);
    js.max_instructions = 0;
    hl_js_budget_arm(&js);
    EXPECT_EQ(total, 3);
    EXPECT_FALSE(results[0].passed);
    EXPECT_NE(strstr(results[0].error, "instruction limit"), NULL);
    EXPECT_TRUE_MSG(results[1].passed, results[1].error);
    EXPECT_TRUE_MSG(results[2].passed, results[2].error);

    kl_http_router_free(&router);
    cleanup_js_caps();
}
#endif
/* ── Audit 10 ────────────────────────────────────────────────────────── */

#include "hull/http_feature.h"   /* hl_js_http_error_response */
#include "hull/shared/res_headers.h" /* HlResBaseList, hl_res_base_* */

/* Milliseconds since @p t0. */
static long a10_ms_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (long)(t1.tv_sec - t0->tv_sec) * 1000 +
           (long)(t1.tv_nsec - t0->tv_nsec) / 1000000;
}

/* H2: work a builtin does inside one step - a scan, compare or copy of a
 * large operand, an allocation - cost one interrupt-poll step, so each of
 * these held the event loop for minutes to forever under any instruction
 * limit. QuickJS HULL PATCH 0005 charges it: each trips a 1M budget fast,
 * and the trip is the usual uncatchable interrupt (the catch never runs). */
UTEST(js_audit10, builtin_work_is_charged)
{
    static const char *const srcs[] = {
        /* scans over a 16 MB string */
        "const s = 'a'.repeat(1 << 22); for (;;) s.indexOf('b');",
        "const s = 'a'.repeat(1 << 22); for (;;) s.lastIndexOf('b');",
        "const s = 'a'.repeat(1 << 22); for (;;) s.includes('b');",
        "const s = 'a'.repeat(1 << 22); for (;;) s.split('b');",
        "const s = 'a'.repeat(1 << 22); for (;;) s.replace('b', 'c');",
        "const s = ' '.repeat(1 << 22); for (;;) s.trim();",
        /* one quadratic search: 2^31 compares in a single call */
        "('a'.repeat(1 << 16)).indexOf('a'.repeat(1 << 15) + 'b');",
        /* compares of large equal strings, and a large Map key */
        "const a = 'x'.repeat(1 << 23), b = a.slice(0, -1) + 'x';"
        " for (;;) a === b;",
        "const a = 'x'.repeat(1 << 23), b = a.slice(0, -1) + 'y';"
        " for (;;) a < b;",
        "const m = new Map(); const k = 'k'.repeat(1 << 23);"
        " for (;;) m.get(k);",
        /* typed arrays */
        "const u = new Uint8Array(1 << 22); for (;;) u.fill(1);",
        "const u = new Uint8Array(1 << 22); for (;;) u.indexOf(2);",
        "const u = new Uint8Array(1 << 22); for (;;) u.copyWithin(0, 1);",
        "const u = new Uint8Array(1 << 22); for (;;) u.reverse();",
        "const u = new Uint8Array(1 << 22); for (;;) u.set(u);",
        "const u = new Uint8Array(1 << 20); for (;;) u.sort();",
        /* arrays: a fast array, and array-likes whose length no heap
         * bounds (each one call, never ending before) */
        "const a = new Array(1 << 16).fill(0); for (;;) a.indexOf(1);",
        "const a = new Array(1 << 16).fill(0); for (;;) a.includes(1);",
        "Array.prototype.indexOf.call({ length: 2 ** 53 - 1 }, 1);",
        "Array.prototype.lastIndexOf.call({ length: 2 ** 53 - 1 }, 1);",
        "Array.prototype.includes.call({ length: 2 ** 53 - 1 }, 1);",
        "Array.prototype.join.call({ length: 2 ** 32 - 1 });",
        "new Array(2 ** 32 - 1).forEach(() => {});",
        "[].concat(new Array(2 ** 32 - 1)).length;",
        /* JSON, and plain allocation */
        "const w = ' '.repeat(1 << 22) + '1'; for (;;) JSON.parse(w);",
        "for (;;) new ArrayBuffer(1 << 20);",
    };
    HlJS lim;
    ASSERT_EQ(a5_limited(&lim, 1000000), 0);
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        char code[1024];
        snprintf(code, sizeof code,
                 "globalThis.__a10 = 0;\n"
                 "try { %s } catch (e) { globalThis.__a10 = 1; }\n", srcs[i]);
        hl_js_reset_request(&lim);
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        EXPECT_TRUE_MSG(a5_eval_throws(&lim, code), srcs[i]);
        long ms = a10_ms_since(&t0);
        EXPECT_EQ_MSG(lim.budget_tripped, 1, srcs[i]);
        EXPECT_LT_MSG(ms, 5000L, srcs[i]);
        hl_js_reset_request(&lim);
        JSValue v = JS_Eval(lim.ctx, "globalThis.__a10", 16, "<t>",
                            JS_EVAL_TYPE_GLOBAL);
        int32_t caught = -1;
        JS_ToInt32(lim.ctx, &caught, v);
        JS_FreeValue(lim.ctx, v);
        EXPECT_EQ_MSG(caught, 0, srcs[i]);   /* the catch never ran */
    }
    /* Ordinary work on ordinary data still fits a normal budget. */
    hl_js_free(&lim);
    ASSERT_EQ(a5_limited(&lim, 100000000), 0);
    EXPECT_FALSE(a5_eval_throws(&lim,
        "const s = 'abc,'.repeat(10000);\n"
        "if (s.split(',').length !== 10001) throw 1;\n"
        "if (s.indexOf('abc', 400) !== 400) throw 2;\n"
        "if (JSON.parse(JSON.stringify({ a: [1, 2, 3] })).a[2] !== 3) throw 3;\n"
        "const a = []; for (let i = 0; i < 100000; i++) a.push({ i });\n"
        "if (a.indexOf(a[99999]) !== 99999) throw 4;\n"
        "a.sort((x, y) => y.i - x.i); if (a[0].i !== 99999) throw 5;\n"));
    EXPECT_EQ(lim.budget_tripped, 0);
    hl_js_free(&lim);
}

#ifdef HL_ENABLE_HTTP_SERVER
/* M (#712 regression): res.json / html / text kept the FIRST Content-Type
 * whoever set it, so res.html then res.json sent the JSON as text/html. Hull's
 * own default is replaced by the next body call; only an app-set one stays,
 * and there is always exactly one. */
UTEST(js_audit10, a_body_call_replaces_hulls_own_content_type)
{
    static const struct { const char *src, *want; } cases[] = {
        { "(req, res) => { res.html('<p>'); res.json({ a: 1 }); return 1; }",
          "Content-Type: application/json\r\n" },
        { "(req, res) => { res.json(1); res.text('x'); return 1; }",
          "Content-Type: text/plain; charset=utf-8\r\n" },
        { "(req, res) => { res.header('Content-Type', 'text/csv');"
          " res.text('a'); res.json(1); return 1; }",
          "Content-Type: text/csv\r\n" },
        /* the app's header after a body call replaces Hull's default */
        { "(req, res) => { res.text('a'); res.header('Content-Type', 'image/png');"
          " res.bytes(new ArrayBuffer(4)); return 1; }",
          "Content-Type: image/png\r\n" },
        /* res.bytes drops a default an earlier body call left */
        { "(req, res) => { res.html('<p>'); res.bytes(new ArrayBuffer(4));"
          " return 1; }", NULL },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        KlAllocator alloc = kl_allocator_default();
        KlHttpResponse res;
        ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
        KlHttpRequest req = {0};
        EXPECT_EQ(a5_middleware(cases[i].src, &req, &res), 1);
        EXPECT_EQ_MSG(a9_header_count(&res, "Content-Type"),
                      cases[i].want ? 1 : 0, cases[i].src);
        if (cases[i].want)
            EXPECT_TRUE_MSG(a9_headers_contain(&res, cases[i].want), cases[i].src);
        free_req_ctx(&req);
        kl_http_response_free(&res);
        cleanup_js();
    }
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* L: the 500 a failed handler gets kept every header it had set - a
 * Set-Cookie, a Location, its own Content-Type next to the error's - and
 * res.bytes after a gzipped body kept the gzip's Content-Encoding / Vary. */
UTEST(js_audit10, error_response_and_bytes_drop_stale_headers)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => { res.header('Set-Cookie', 'sid=1');"
        " res.header('Location', '/x'); res.html('<p>'); return 1; }",
        &req, &res), 1);
    hl_js_http_error_response(&js, &res);
    EXPECT_EQ(res.status, 500);
    EXPECT_EQ(a9_header_count(&res, "Set-Cookie"), 0);
    EXPECT_EQ(a9_header_count(&res, "Location"), 0);
    EXPECT_EQ(a9_header_count(&res, "Content-Security-Policy"), 0);
    EXPECT_EQ(a9_header_count(&res, "Content-Type"), 1);
    EXPECT_TRUE(a9_headers_contain(&res, "Content-Type: text/plain\r\n"));
    free_req_ctx(&req);
    kl_http_response_free(&res);
    cleanup_js();

    /* res.bytes after a body Keel gzipped (a fake Content-Encoding / Vary
     * pair, as http_compress adds). */
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpResponse res2;
    ASSERT_EQ(kl_http_response_init(&res2, &alloc), 0);
    ASSERT_EQ(kl_http_response_header(&res2, "Content-Encoding", "gzip"), 0);
    ASSERT_EQ(kl_http_response_header(&res2, "Vary", "Accept-Encoding"), 0);
    KlHttpRequest req2 = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => { res.bytes(new ArrayBuffer(8)); return 1; }",
        &req2, &res2), 1);
    EXPECT_EQ(a9_header_count(&res2, "Content-Encoding"), 0);
    EXPECT_EQ(a9_header_count(&res2, "Vary"), 0);
    EXPECT_EQ(res2.body_len, (size_t)8);
    free_req_ctx(&req2);
    kl_http_response_free(&res2);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* L: WeakRef / FinalizationRegistry are not in the app runtime (a cleanup
 * callback ran in whatever request a GC happened in), nor in a
 * worker.dispatch VM; and a hull: name that is not a registry module is not
 * the app's to import (hull:verify, gone from the embed). */
UTEST(js_audit10, gc_observers_and_stray_stdlib_modules_are_absent)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(eval_int("(typeof WeakRef === 'undefined' &&"
                       " typeof FinalizationRegistry === 'undefined') ? 1 : 0"), 1);
    char *msg = NULL;
    EXPECT_EQ(a5_module_named("./app.js",
        "import * as v from 'hull:verify';\nglobalThis.__a10_v = 1;\n", &msg), -1);
    EXPECT_TRUE(msg && strstr(msg, "hull:verify"));
    free(msg);
    EXPECT_EQ(eval_int("globalThis.__a10_v === undefined ? 1 : 0"), 1);
    cleanup_js_caps();
}

/* M: public-key operations ran in one call each, charged nothing beyond
 * their message bytes, so a loop of ed25519 / x25519 / box held the event
 * loop. Each charges 2^14 units before the work: a 1M budget allows about
 * 60 of them, not ~300k. Counted, not timed: a sanitizer build runs each
 * operation many times slower, so a wall-clock bound is noise. */
UTEST(js_audit10, public_key_operations_are_charged)
{
    static const char *const srcs[] = {
        "for (;;) { globalThis.__a10_n++; crypto.ed25519Keypair(); }",
        "const k = crypto.ed25519Keypair();"
        " for (;;) { globalThis.__a10_n++; crypto.ed25519Sign('m', k.secretKey); }",
        "const k = crypto.ed25519Keypair(); const s = crypto.ed25519Sign('m', k.secretKey);"
        " for (;;) { globalThis.__a10_n++; crypto.ed25519Verify('m', s, k.publicKey); }",
        "const a = crypto.x25519Keypair(), b = crypto.x25519Keypair();"
        " for (;;) { globalThis.__a10_n++; crypto.x25519(a.secretKey, b.publicKey); }",
        "const a = crypto.boxKeypair(), b = crypto.boxKeypair();"
        " const n = new Uint8Array(24);"
        " for (;;) { globalThis.__a10_n++; crypto.box('m', n, b.publicKey, a.secretKey); }",
    };
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        js.max_instructions = 1000000;
        hl_js_reset_request(&js);
        char code[512];
        snprintf(code, sizeof code,
                 "(() => { globalThis.__a10_n = 0;"
                 " try { %s } catch (e) {} return 0; })()", srcs[i]);
        EXPECT_EQ_MSG(eval_int(code), -9999, srcs[i]);
        EXPECT_EQ_MSG(js.budget_tripped, 1, srcs[i]);
        /* Read the count back in a fresh run (the trip is sticky). */
        hl_js_reset_request(&js);
        int ops = eval_int("globalThis.__a10_n");
        EXPECT_GT_MSG(ops, 0, srcs[i]);
        EXPECT_LT_MSG(ops, 1000, srcs[i]);
        cleanup_js();
    }
}

/* Audit 34: ed25519Sign / ed25519Verify copy the message off the heap (verify
 * twice over), and a MappedBuffer / WasmBuffer message is not on the heap at
 * all. A message larger than the heap limit is refused before the copy; auth
 * hashes the message in place and still takes it. */
UTEST(js_audit34, ed25519_refuses_a_message_past_the_heap_limit)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(eval_int("(() => { globalThis.K = crypto.ed25519Keypair();"
                       " globalThis.BIG = new Uint8Array(2 * 1024 * 1024);"
                       " globalThis.S = crypto.ed25519Sign(BIG, K.secretKey);"
                       " globalThis.AK = new Uint8Array(32);"
                       " return crypto.ed25519Verify(BIG, S, K.publicKey) ? 1 : 0; })()"), 1);
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 1024 * 1024;   /* the field only: BIG already exists */
    char *r = eval_str("(() => { try { crypto.ed25519Verify(BIG, S, K.publicKey); }"
                       " catch (e) { return String(e.message); } return 'no throw'; })()");
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    if (r && !strstr(r, "heap limit")) printf("  got: %s\n", r);
    free(r);
    r = eval_str("(() => { try { crypto.ed25519Sign(BIG, K.secretKey); }"
                 " catch (e) { return String(e.message); } return 'no throw'; })()");
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    free(r);
    /* auth streams the message: no copy, so no refusal. */
    EXPECT_EQ(eval_int("(() => { const t = crypto.auth(BIG, AK);"
                       " return crypto.authVerify(t, BIG, AK) ? 1 : 0; })()"), 1);
    js.max_heap_bytes = saved;
    cleanup_js();
}

/* ── Audit 11 ────────────────────────────────────────────────────────── */

/* Work QuickJS HULL PATCH 0005 still missed: string-to-number (the
 * JS_ToCStringLen ASCII path scans and allocates nothing), the app-supplied
 * RegExp `flags` string, a "$<" replacement rescanned per "$<", quadratic
 * BigInt arithmetic, and the generic per-index loops of typed-array
 * construction. Each loop counts its iterations (no wall-clock bound: the
 * sanitizer jobs are slow) and must trip a 1M budget, uncatchably, within a
 * handful of them - without the charge each ran hundreds to hundreds of
 * thousands, or one call ran for seconds. */
UTEST(js_audit11, builtin_work_is_charged)
{
    static const char *const srcs[] = {
        /* string -> number, over a 4 MB string */
        "const s = ' '.repeat(1 << 22); for (;;) { __a11++; +s; }",
        "const s = ' '.repeat(1 << 22); for (;;) { __a11++; Number(s); }",
        "const s = ' '.repeat(1 << 22); for (;;) { __a11++; parseFloat(s); }",
        "const s = ' '.repeat(1 << 22); for (;;) { __a11++; parseInt(s); }",
        "const s = ' '.repeat(1 << 22); for (;;) { __a11++; BigInt(s); }",
        "const s = '1'.repeat(1 << 22); for (;;) { __a11++; s * 1; }",
        /* an app `flags` string, scanned per call */
        "const re = /a/; const f = 'x'.repeat(1 << 22);"
        " Object.defineProperty(re, 'flags', { get: () => f });"
        " for (;;) { __a11++; re[Symbol.replace]('a', 'b'); }",
        "const re = /a/; const f = 'x'.repeat(1 << 22);"
        " Object.defineProperty(re, 'flags', { get: () => f });"
        " for (;;) { __a11++; re[Symbol.match]('a'); }",
        /* "$<" with no '>' rescans the rest of the replacement: quadratic */
        "for (;;) { __a11++; 'a'.replace(/(?<n>a)/, '$<'.repeat(1 << 16)); }",
        /* BigInt: quadratic in limbs */
        "const a = (1n << 500000n) - 1n; for (;;) { __a11++; a * a; }",
        "const a = (1n << 500000n) - 1n, b = (1n << 250000n) + 1n;"
        " for (;;) { __a11++; a / b; }",
        "const a = (1n << 500000n) - 1n, b = (1n << 250000n) + 1n;"
        " for (;;) { __a11++; a % b; }",
        "const a = (1n << 200000n) - 1n; for (;;) { __a11++; a.toString(); }",
        "const a = (1n << 200000n) - 1n; for (;;) { __a11++; String(a); }",
        "const s = '9'.repeat(200000); for (;;) { __a11++; BigInt(s); }",
        "for (;;) { __a11++; 3n ** 300000n; }",
        /* typed-array construction from an array-like */
        "for (;;) { __a11++; new Uint8Array({ length: 1 << 24 }); }",
        "for (;;) { __a11++; Uint8Array.from({ length: 1 << 24 }); }",
    };
    static const char readback[] =
        "globalThis.__a11c * 1000000 + globalThis.__a11";
    HlJS lim;
    ASSERT_EQ(a5_limited(&lim, 1000000), 0);
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        char code[1024];
        snprintf(code, sizeof code,
                 "globalThis.__a11 = 0; globalThis.__a11c = 0;\n"
                 "try { %s } catch (e) { globalThis.__a11c = 1; }\n", srcs[i]);
        hl_js_reset_request(&lim);
        EXPECT_TRUE_MSG(a5_eval_throws(&lim, code), srcs[i]);
        EXPECT_EQ_MSG(lim.budget_tripped, 1, srcs[i]);
        hl_js_reset_request(&lim);
        JSValue v = JS_Eval(lim.ctx, readback, strlen(readback), "<t>",
                            JS_EVAL_TYPE_GLOBAL);
        int32_t r = -1;
        JS_ToInt32(lim.ctx, &r, v);
        JS_FreeValue(lim.ctx, v);
        EXPECT_GT_MSG(r, 0, srcs[i]);    /* it ran, the catch never did, */
        EXPECT_LT_MSG(r, 50, srcs[i]);   /* and it stopped within 49 */
    }
    /* Ordinary work on ordinary data still fits a normal budget. */
    hl_js_free(&lim);
    ASSERT_EQ(a5_limited(&lim, 100000000), 0);
    EXPECT_FALSE(a5_eval_throws(&lim,
        "let t = 0;\n"
        "for (let i = 0; i < 100000; i++) t += +' 12.5 ' + parseInt('42', 10);\n"
        "if (t !== 5450000) throw 1;\n"
        "if (BigInt('123456789012345678901234567890') % 7n !== 0n) throw 2;\n"
        "const p = 2n ** 4096n; if ((p * p / p) !== p) throw 3;\n"
        "if (p.toString().length !== 1234) throw 4;\n"
        "if ('a-b-c'.replace(/-/g, '+') !== 'a+b+c') throw 5;\n"
        "if ('x1y2'.split(/\\d/).length !== 3) throw 6;\n"
        "if ([...'abab'.matchAll(/a/g)].length !== 2) throw 7;\n"
        "if (new Uint8Array({ length: 100000 }).length !== 100000) throw 8;\n"
        "if (Uint8Array.from([1, 2, 3])[2] !== 3) throw 9;\n"
        "if ('a'.replace(/(?<n>a)/, '[$<n>]') !== '[a]') throw 10;\n"));
    EXPECT_EQ(lim.budget_tripped, 0);
    hl_js_free(&lim);
}

/* L: an RSA verify was charged by the signature's length, which is the
 * attacker's: a 1024-byte signature cost an RSA-8192's 8^3 * 2^14 units
 * whatever the key. Bounded by the public key's PEM now (at most 6 bits per
 * PEM byte), and a signature longer than any key mbedTLS takes is false,
 * uncharged. The same numbers as the Lua runtime. */
UTEST(js_audit11, rsa_verify_is_charged_by_the_key)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    /* 100 verifies of a 1024-byte signature against a 300-byte PEM (at most
     * 1800 bits: 2^3 * 2^14 units each, 13M in all) fit a 100M budget; at
     * the signature's 8192 bits they were 860M. */
    js.max_instructions = 100000000;
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int(
        "(() => { const pem = 'A'.repeat(300), sig = 's'.repeat(1024);"
        " for (let i = 0; i < 100; i++)"
        "   if (crypto.verify('RS256', pem, 'm', sig)) return 2;"
        " return 1; })()"), 1);
    EXPECT_EQ(js.budget_tripped, 0);
    /* An oversized signature is false and costs nothing beyond its data. */
    js.max_instructions = 1000000;
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int(
        "(() => { const pem = 'A'.repeat(20000), sig = 's'.repeat(4096);"
        " for (let i = 0; i < 1000; i++)"
        "   if (crypto.verify('PS256', pem, 'm', sig)) return 2;"
        " return 1; })()"), 1);
    EXPECT_EQ(js.budget_tripped, 0);
    /* A long key with a signature its length is still charged in full. */
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int(
        "crypto.verify('RS256', 'A'.repeat(2000), 'm', 's'.repeat(1024)) ? 1 : 0"),
        -9999);
    EXPECT_EQ(js.budget_tripped, 1);
}

/* ── Audit 11: Content-Type and error headers across middleware ────────── */

#ifdef HL_ENABLE_HTTP_SERVER
/* L: whether the Content-Type is Hull's default was a flag on the per-call
 * `res` object, so a middleware's res.html followed by the handler's res.json
 * (two objects, one response) kept text/html for the JSON. And the 500 a
 * failed handler gets dropped the headers earlier middleware set (CSP, HSTS,
 * CORS, a request id) along with the handler's own. */
UTEST(js_audit11, content_type_and_error_headers_span_middleware)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', (req, res) => {\n"
        "  res.header('Strict-Transport-Security', 'max-age=1'); res.html('<p>');\n"
        "  return 0; });\n"
        "app.use('*', '/*', (req, res) => {\n"
        "  res.header('Content-Type', 'application/json'); return 0; });\n"
        "app.get('/json', (req, res) => { res.json({ a: 1 }); });\n"
        "app.get('/boom', (req, res) => { res.header('Set-Cookie', 'sid=1');\n"
        "  res.json(1); throw new Error('boom'); });\n"
        "app.get('/html', (req, res) => { res.html('<p>'); });\n";
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<test>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(val))
        hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, val);
    hl_js_run_jobs(&js);
    int mw1 = eval_int("globalThis.__hull_internals.__hull_middleware[0].handler_id");
    int mw2 = eval_int("globalThis.__hull_internals.__hull_middleware[1].handler_id");
    int last = eval_int("globalThis.__hull_internals.__hull_routes.length - 1");
    KlAllocator alloc = kl_allocator_default();

    /* Middleware res.html, handler res.json: one Content-Type, the JSON's. */
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(hl_js_dispatch_middleware(&js, mw1, &req, &res), 0);
    EXPECT_EQ(hl_js_dispatch(&js, last - 2, &req, &res), 0);
    EXPECT_EQ(a9_header_count(&res, "Content-Type"), 1);
    EXPECT_TRUE(a9_headers_contain(&res, "Content-Type: application/json\r\n"));
    free_req_ctx(&req);
    kl_http_response_free(&res);

    /* The handler fails: the middleware's HSTS stays, the handler's
     * Set-Cookie and Content-Type go, one Content-Type (the error's). */
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req2 = {0};
    EXPECT_EQ(hl_js_dispatch_middleware(&js, mw1, &req2, &res), 0);
    EXPECT_EQ(hl_js_dispatch(&js, last - 1, &req2, &res), -1);
    hl_js_http_error_response(&js, &res);
    EXPECT_EQ(res.status, 500);
    EXPECT_EQ(a9_header_count(&res, "Strict-Transport-Security"), 1);
    EXPECT_EQ(a9_header_count(&res, "Set-Cookie"), 0);
    EXPECT_EQ(a9_header_count(&res, "Content-Type"), 1);
    EXPECT_TRUE(a9_headers_contain(&res, "Content-Type: text/plain\r\n"));
    free_req_ctx(&req2);
    kl_http_response_free(&res);

    /* An app Content-Type spelled like a default is still the app's: the
     * handler's res.html keeps it. */
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req3 = {0};
    EXPECT_EQ(hl_js_dispatch_middleware(&js, mw2, &req3, &res), 0);
    EXPECT_EQ(hl_js_dispatch(&js, last, &req3, &res), 0);
    EXPECT_EQ(a9_header_count(&res, "Content-Type"), 1);
    EXPECT_TRUE(a9_headers_contain(&res, "content-type: application/json\r\n"));
    free_req_ctx(&req3);
    kl_http_response_free(&res);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
/* ── Audit 11 H1: route / middleware patterns outlive their JS strings ──
 *
 * Keel stores the method / pattern pointers it is given without copying.
 * They were JS_ToCString results freed right after registration: for a
 * non-ASCII pattern a freed buffer (use-after-free from startup), for an
 * ASCII one an alias of a string the app could drop by clearing the
 * __hull_* globals. Wiring now hands Keel Hull-owned copies: after the
 * globals are deleted and the GC has run, the router's patterns are intact
 * (under ASan any stale read fails the test) and requests still route. */

static int a11_pattern_is(const char *p, size_t len, const char *want)
{
    return p && len == strlen(want) && memcmp(p, want, len) == 0;
}

static void a11_drop_route_globals(void)
{
    const char *drop =
        "delete globalThis.__hull_internals.__hull_route_defs;"
        "delete globalThis.__hull_internals.__hull_middleware;"
        "delete globalThis.__hull_internals.__hull_post_middleware;"
        "delete globalThis.__hull_internals.__hull_ws_defs;"
        "delete globalThis.__hull_internals.__hull_sse_defs;";
    JSValue v = JS_Eval(js.ctx, drop, strlen(drop), "<a11drop>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    JS_RunGC(js.rt);
}

UTEST(js_audit11, router_patterns_survive_dropped_defs)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *app =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "globalThis.__a11_pre = 0; globalThis.__a11_post = 0;\n"
        "app.use('GET', '/caf\xc3\xa9', (req, res) => { globalThis.__a11_pre++; return 0; });\n"
        "app.usePost('GET', '/caf\xc3\xa9', (req, res) => { globalThis.__a11_post++; return 0; });\n"
        "app.get('/caf\xc3\xa9', (req, res) => { res.json({ ok: 'caf\xc3\xa9' }); });\n"
        "app.get('/plain', (req, res) => { res.json({ ok: 1 }); });\n";
    JSValue v = JS_Eval(js.ctx, app, strlen(app), "<a11app>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    KlHttpRouter router;
    KlAllocator kalloc = kl_allocator_default();
    kl_http_router_init(&router, &kalloc);
    ASSERT_EQ(hl_js_wire_routes(&js, &router), 0);
    hl_js_test_register(js.ctx, &router, &js);

    /* The app (or anything) drops every def the strings came from. */
    a11_drop_route_globals();

    ASSERT_EQ(router.count, 2);
    EXPECT_TRUE(a11_pattern_is(router.routes[0].pattern, router.routes[0].pattern_len,
                               "/caf\xc3\xa9"));
    EXPECT_TRUE(a11_pattern_is(router.routes[0].method, router.routes[0].method_len, "GET"));
    EXPECT_TRUE(a11_pattern_is(router.routes[1].pattern, router.routes[1].pattern_len,
                               "/plain"));
    /* Middleware entries are opaque: their patterns are read by the
     * requests dispatched through them below. */
    ASSERT_EQ(router.mw_count, 1);
    ASSERT_EQ(router.post_mw_count, 1);

    const char *cases =
        "test('non-ASCII route still routes', () => { "
        "  const r = test.get('/caf\xc3\xa9', { middleware: true }); "
        "  if (r.status !== 200) throw new Error('status ' + r.status); "
        "  if (globalThis.__a11_pre < 1) throw new Error('middleware did not run'); "
        "  if (globalThis.__a11_post < 1) throw new Error('post-body middleware did not run'); });\n"
        "test('ASCII route still routes', () => { "
        "  if (test.get('/plain').status !== 200) throw new Error('status'); });\n";
    v = JS_Eval(js.ctx, cases, strlen(cases), "<a11cases>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);

    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[2];
    memset(results, 0, sizeof results);
    hl_js_test_run(js.ctx, &total, &passed, &failed, NULL, results, 2);
    EXPECT_EQ(total, 2);
    EXPECT_TRUE_MSG(results[0].passed, results[0].error);
    EXPECT_TRUE_MSG(results[1].passed, results[1].error);

    kl_http_router_free(&router);
    cleanup_js_caps();
}

UTEST(js_audit11, server_ws_sse_patterns_survive_dropped_defs)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    const char *app =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1', 'hull/web/ws-server@1',"
        " 'hull/web/sse@1'] });\n"
        "app.get('/r\xc3\xa9', (req, res) => {});\n"
        "app.use('*', '/m\xc3\xa9/*', (req, res) => 0);\n"
        "app.ws('/ws\xc3\xa9', { onMessage: (c, m) => {} });\n"
        "app.sse('/sse\xc3\xa9', (req, stream) => {});\n";
    JSValue v = JS_Eval(js.ctx, app, strlen(app), "<a11srv>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    KlHttpServer server;
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 1, .alloc = NULL };
    kl_http_server_init(&server, &cfg);
    ASSERT_EQ(hl_js_wire_routes_server(&js, &server, NULL), 0);

    a11_drop_route_globals();

    /* route, ws upgrade, sse - in registration order */
    ASSERT_EQ(server.router.count, 3);
    EXPECT_TRUE(a11_pattern_is(server.router.routes[0].pattern,
                               server.router.routes[0].pattern_len, "/r\xc3\xa9"));
    EXPECT_TRUE(a11_pattern_is(server.router.routes[1].pattern,
                               server.router.routes[1].pattern_len, "/ws\xc3\xa9"));
    EXPECT_TRUE(a11_pattern_is(server.router.routes[2].pattern,
                               server.router.routes[2].pattern_len, "/sse\xc3\xa9"));
    ASSERT_EQ(server.router.mw_count, 1);

    kl_http_server_free(&server);
    cleanup_js();
}

/* Audit 11 L: a pre-body middleware that cannot be registered (here: its
 * pattern no longer converts to a string) fails the wiring - the app
 * refuses to start rather than serving without it. */
UTEST(js_audit11, unregistrable_middleware_fails_wiring)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *app =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/admin/*', (req, res) => 1);\n"
        "app.get('/admin/x', (req, res) => { res.json({ ok: 1 }); });\n"
        "globalThis.__hull_internals.__hull_middleware[0].pattern = Symbol('unconvertible');\n";
    JSValue v = JS_Eval(js.ctx, app, strlen(app), "<a11fail>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    KlHttpRouter router;
    KlAllocator kalloc = kl_allocator_default();
    kl_http_router_init(&router, &kalloc);
    EXPECT_EQ(hl_js_wire_routes(&js, &router), -1);
    EXPECT_EQ(router.mw_count, 0);
    /* no exception is left pending on the context */
    EXPECT_FALSE(JS_HasException(js.ctx));
    kl_http_router_free(&router);

    KlHttpServer server;
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 1, .alloc = NULL };
    kl_http_server_init(&server, &cfg);
    EXPECT_EQ(hl_js_wire_routes_server(&js, &server, NULL), -1);
    EXPECT_EQ(server.router.mw_count, 0);
    kl_http_server_free(&server);
    cleanup_js_caps();
}
#endif

/* ── The run watchdog (cap/run_watchdog.h, audit 12) ──────────────────
 *
 * A wall-clock deadline per uninterrupted run, the backstop for work one
 * QuickJS step does that the instruction budget does not charge. Each case
 * runs with NO instruction limit, so only the watchdog can stop it, and a
 * short deadline (200 ms); assertions are on the trip, not on timing (the
 * sanitizer jobs are slow). */
#define WD_TEST_MS 200

static void wd_sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) {}
}

/* Evaluate `code` in a VM with no instruction limit under a fresh deadline.
 * Returns 1 when the run was stopped by its deadline, 0 when it completed,
 * -1 on a setup failure or any other outcome; `after` (if non-NULL) gets
 * the global `after` (1 when code after a caught trip ran). */
static int wd_js_run(const char *code, int *after)
{
    hl_run_watchdog_configure(WD_TEST_MS);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    cfg.max_instructions = 0;
    HlJS v;
    memset(&v, 0, sizeof v);
    if (hl_js_init(&v, &cfg) != 0) {
        hl_run_watchdog_configure(-1);
        return -1;
    }
    hl_js_budget_arm(&v);
    JSValue r = JS_Eval(v.ctx, code, strlen(code), "<wd>", JS_EVAL_TYPE_GLOBAL);
    int out;
    if (JS_IsException(r)) {
        JS_FreeValue(v.ctx, JS_GetException(v.ctx));
        out = v.budget_tripped && v.budget_timed_out ? 1 : -1;
    } else {
        out = 0;
    }
    JS_FreeValue(v.ctx, r);
    if (after) {
        /* Read without running code: the run is still tripped. */
        JSValue g = JS_GetGlobalObject(v.ctx);
        JSValue a = JS_GetPropertyStr(v.ctx, g, "after");
        *after = JS_IsUndefined(a) ? 0 : 1;
        JS_FreeValue(v.ctx, a);
        JS_FreeValue(v.ctx, g);
    }
    hl_js_free(&v);
    hl_run_watchdog_configure(-1);
    return out;
}

UTEST(js_run_watchdog, loops_over_uncharged_work_are_stopped)
{
    static const char *const cases[] = {
        "for (;;) {}",
        /* round-12 triggers: a deep prototype-chain miss, Proxy ownKeys
         * validation, a long-literal regexp */
        "let o = {}; for (let i = 0; i < 20000; i++) o = Object.create(o);\n"
        "for (;;) { o.missing; }",
        "const keys = Array.from({ length: 3000 }, (_, i) => 'k' + i);\n"
        "const p = new Proxy({}, { ownKeys: () => keys });\n"
        "for (;;) { try { Reflect.ownKeys(p); } catch (e) {} }",
        "const re = new RegExp('a'.repeat(5000));\n"
        "const s = 'a'.repeat(4999) + 'b';\n"
        "for (;;) re.test(s);",
        NULL
    };
    for (int i = 0; cases[i]; i++)
        EXPECT_EQ_MSG(wd_js_run(cases[i], NULL), 1, cases[i]);
}

/* The trip is uncatchable: a try / catch, an async body or a promise job
 * does not let the run go on. */
UTEST(js_run_watchdog, the_trip_cannot_be_caught)
{
    int after = -1;
    EXPECT_EQ(wd_js_run("try { for (;;) {} } catch (e) {}\n"
                        "globalThis.after = 1;", &after), 1);
    EXPECT_EQ(after, 0);
    after = -1;
    EXPECT_EQ(wd_js_run("for (;;) { try { for (;;) {} } catch (e) {} }", &after), 1);
}

UTEST(js_run_watchdog, a_run_under_its_deadline_is_not_stopped)
{
    EXPECT_EQ(wd_js_run("let n = 0; for (let i = 0; i < 100000; i++) n += i;", NULL), 0);
}

/* A run that waits (a parked handler) is not over its deadline when it is
 * resumed: each entry re-arms it. Simulated: the deadline passes while the
 * VM is idle, and the next arm clears it. */
UTEST(js_run_watchdog, an_idle_vm_is_rearmed_by_the_next_entry)
{
    hl_run_watchdog_configure(WD_TEST_MS);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    cfg.max_instructions = 0;
    HlJS v;
    memset(&v, 0, sizeof v);
    ASSERT_EQ(hl_js_init(&v, &cfg), 0);
    hl_js_budget_arm(&v);
    wd_sleep_ms(2 * WD_TEST_MS + 100);
    EXPECT_TRUE(hl_run_watch_stopped(&v.run_watch));
    hl_js_budget_arm(&v);
    EXPECT_FALSE(hl_run_watch_stopped(&v.run_watch));
    JSValue r = JS_Eval(v.ctx, "1 + 1", 5, "<wd>", JS_EVAL_TYPE_GLOBAL);
    EXPECT_FALSE(JS_IsException(r));
    JS_FreeValue(v.ctx, r);
    hl_js_free(&v);
    hl_run_watchdog_configure(-1);
}

/* A VM freed while its deadline is pending: the watchdog must never write to
 * the freed watch (ASan reports it if it does). */
UTEST(js_run_watchdog, a_vm_freed_while_armed_is_never_touched)
{
    hl_run_watchdog_configure(WD_TEST_MS);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    for (int i = 0; i < 4; i++) {
        HlJS *v = calloc(1, sizeof *v);
        ASSERT_TRUE(v != NULL);
        ASSERT_EQ(hl_js_init(v, &cfg), 0);
        hl_js_budget_arm(v);
        hl_js_free(v);
        free(v);
    }
    wd_sleep_ms(2 * WD_TEST_MS + 100);
    hl_run_watchdog_configure(-1);
}

/* A worker.dispatch job has its own deadline; the dispatching run, parked
 * meanwhile, is resumed and completes. */
UTEST(js_run_watchdog, a_runaway_worker_dispatch_is_stopped)
{
    const HlAsyncBackend *be;
    HlAsyncBackendCtx *actx;
    HlAsyncBackendPool *pool;
    ASSERT_EQ(js_worker_open(&be, &actx, &pool), 0);
    js.max_instructions = 0;
    hl_run_watchdog_configure(WD_TEST_MS);
    char out[512] = "(no verdict)";
    js_worker_run(be, actx,
        "  const m = await fails(() => { for (;;) {} });\n"
        "  check(m.includes('time limit'), m);\n", out, sizeof out);
    EXPECT_STREQ(out, "ok");
    hl_run_watchdog_configure(-1);
    js_worker_close(be, actx, pool);
}

/* ── Audit 12: runtime lows ──────────────────────────────────────────── */

#ifdef HL_ENABLE_HTTP_SERVER
/* Evaluate an app module (errors dumped). */
static void a12_eval_app(const char *code)
{
    JSValue v = JS_Eval(js.ctx, code, strlen(code), "<a12>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);
}

/* res.header handed Keel C strings it measures with strlen, after Hull's
 * Content-Type check had looked at the whole JS string: a name with a NUL
 * ("Content-Type\0x") went out as a second, unchecked Content-Type. A name
 * must be a token now and a value may not hold a NUL; both throw. */
UTEST(js_audit12, header_name_must_be_a_token_and_nul_is_refused)
{
    static const char *const bad[] = {
        "res.header('Content-Type\\0x', 'text/html')",
        "res.header('X-A\\0', 'v')",
        "res.header('Bad Name', 'v')",
        "res.header('', 'v')",
        "res.header('X-A:', 'v')",
        "res.header('X-\\u00e9', 'v')",
        "res.header('X-A', 'v\\0w')",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        init_js();
        ASSERT_TRUE(js_initialized);
        KlAllocator alloc = kl_allocator_default();
        KlHttpResponse res;
        ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
        KlHttpRequest req = {0};
        char src[256];
        snprintf(src, sizeof src,
                 "(req, res) => { res.json(1); %s; return 1; }", bad[i]);
        EXPECT_EQ_MSG(a5_middleware(src, &req, &res), -1, bad[i]);
        /* only Hull's own Content-Type; nothing the bad call added */
        EXPECT_EQ_MSG(a9_header_count(&res, "Content-Type"), 1, bad[i]);
        EXPECT_FALSE_MSG(a9_headers_contain(&res, "X-A"), bad[i]);
        free_req_ctx(&req);
        kl_http_response_free(&res);
        cleanup_js();
    }
    /* An ordinary header still goes out. */
    init_js();
    ASSERT_TRUE(js_initialized);
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(a5_middleware("(req, res) => { res.header(\"X-Ok_1!#$%&'*+.^`|~\","
                            " 'a b'); return 1; }", &req, &res), 1);
    EXPECT_TRUE(a9_headers_contain(&res, "X-Ok_1!#$%&'*+.^`|~: a b\r\n"));
    free_req_ctx(&req);
    kl_http_response_free(&res);
    cleanup_js();
}

/* The kept-headers snapshots: a middleware that fails keeps the headers the
 * middleware before it set (its 500 dropped them all); the list is the
 * runtime's own and hl_js_free empties it; and a request whose dispatch
 * fails before its handler runs never gets an earlier request's snapshot
 * (the snapshot was taken after those early error returns, so the 500
 * restored whatever an earlier request on that response had left). */
UTEST(js_audit12, error_reset_snapshots_are_per_request_and_per_runtime)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    a12_eval_app(
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.use('*', '/*', (req, res) => {\n"
        "  res.header('Strict-Transport-Security', 'max-age=1'); return 0; });\n"
        "app.use('*', '/*', (req, res) => {\n"
        "  res.header('Set-Cookie', 'sid=1'); throw new Error('mw boom'); });\n"
        "app.use('*', '/*', (req, res) => 0);\n");
    int mw1 = eval_int("globalThis.__hull_internals.__hull_middleware[0].handler_id");
    int mw2 = eval_int("globalThis.__hull_internals.__hull_middleware[1].handler_id");
    int mw3 = eval_int("globalThis.__hull_internals.__hull_middleware[2].handler_id");
    KlAllocator alloc = kl_allocator_default();

    /* A failing middleware keeps the earlier middleware's header. */
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(hl_js_dispatch_middleware(&js, mw1, &req, &res), 0);
    EXPECT_EQ(hl_js_dispatch_middleware(&js, mw2, &req, &res), -1);
    hl_js_http_error_response(&js, &res);
    EXPECT_EQ(res.status, 500);
    EXPECT_EQ(a9_header_count(&res, "Strict-Transport-Security"), 1);
    EXPECT_EQ(a9_header_count(&res, "Set-Cookie"), 0);
    EXPECT_EQ(js.res_bases.count, (size_t)0);   /* taken by the 500 */
    free_req_ctx(&req);
    kl_http_response_free(&res);

    /* Request A leaves a snapshot (its connection went away mid-run, say);
     * request B on the same response fails before any handler runs: its
     * 500 must not restore A's header. */
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest ra = {0};
    EXPECT_EQ(hl_js_dispatch_middleware(&js, mw1, &ra, &res), 0);
    EXPECT_EQ(hl_js_dispatch_middleware(&js, mw3, &ra, &res), 0);
    EXPECT_EQ(js.res_bases.count, (size_t)1);
    free_req_ctx(&ra);
    kl_http_response_free(&res);
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest rb = {0};
    EXPECT_EQ(hl_js_dispatch(&js, 99999, &rb, &res), -1);   /* no handler */
    hl_js_http_error_response(&js, &res);
    EXPECT_EQ(res.status, 500);
    EXPECT_EQ(a9_header_count(&res, "Strict-Transport-Security"), 0);
    free_req_ctx(&rb);
    kl_http_response_free(&res);

    /* A snapshot left behind is the runtime's, and goes with it. */
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest rc = {0};
    EXPECT_EQ(hl_js_dispatch_middleware(&js, mw1, &rc, &res), 0);
    EXPECT_EQ(hl_js_dispatch_middleware(&js, mw3, &rc, &res), 0);
    EXPECT_EQ(js.res_bases.count, (size_t)1);
    EXPECT_GT(js.res_bases.bytes, (size_t)0);
    free_req_ctx(&rc);
    kl_http_response_free(&res);
    cleanup_js();
    EXPECT_EQ(js.res_bases.count, (size_t)0);
    EXPECT_EQ(js.res_bases.bytes, (size_t)0);
    EXPECT_TRUE(js.res_bases.head == NULL);
}

/* A connection that goes away under a suspended handler (an async cancel)
 * takes its snapshot with it; the list is bounded by bytes as well as
 * entries. */
UTEST(js_audit12, snapshot_forgotten_by_connection_and_capped_by_bytes)
{
    KlAllocator alloc = kl_allocator_default();
    HlResBaseList l = {0};
    static KlHttpResponse rs[40];
    char big[48 * 1024];
    memset(big, 'v', sizeof big - 1);
    big[sizeof big - 1] = 0;
    for (int i = 0; i < 40; i++) {
        ASSERT_EQ(kl_http_response_init(&rs[i], &alloc), 0);
        ASSERT_EQ(kl_http_response_header(&rs[i], "X-Big", big), 0);
        hl_res_base_begin(&l, &rs[i], (const void *)&rs[i]);
        EXPECT_LE(l.bytes, (size_t)HL_RES_BASE_BYTES_MAX);
    }
    EXPECT_LT(l.count, (size_t)40);              /* the oldest went */
    EXPECT_TRUE(hl_res_base_find(&l, &rs[39]) != NULL);
    EXPECT_TRUE(hl_res_base_find(&l, &rs[0]) == NULL);
    size_t before = l.count;
    hl_res_base_forget_conn(&l, (const void *)&rs[39]);
    EXPECT_EQ(l.count, before - 1);
    EXPECT_TRUE(hl_res_base_find(&l, &rs[39]) == NULL);
    hl_res_base_clear(&l);
    EXPECT_EQ(l.count, (size_t)0);
    EXPECT_EQ(l.bytes, (size_t)0);
    for (int i = 0; i < 40; i++)
        kl_http_response_free(&rs[i]);
}

/* __hull_timer_defs / __hull_ws_defs / __hull_sse_defs are app-writable
 * globals: the wiring took them on trust (a 0 ms repeating timer, a daily
 * timer at hour 99, a handler id whose conversion threw and was ignored).
 * Each tampered def now fails the wiring, with no exception left pending. */
UTEST(js_audit12, tampered_timer_and_ws_defs_fail_the_wiring)
{
    static const char *const tamper[] = {
        "globalThis.__hull_internals.__hull_timer_defs[0].interval_ms = 0;",
        "globalThis.__hull_internals.__hull_timer_defs[0].interval_ms = -1;",
        "globalThis.__hull_internals.__hull_timer_defs[0].interval_ms = 99.5;",
        "globalThis.__hull_internals.__hull_timer_defs[0].interval_ms = '1000';",
        "globalThis.__hull_internals.__hull_timer_defs[0].handler_id = -1;",
        "globalThis.__hull_internals.__hull_timer_defs[0].handler_id = 'x';",
        "globalThis.__hull_internals.__hull_timer_defs[0].type = 'hourly';",
        "Object.defineProperty(globalThis.__hull_internals.__hull_timer_defs[0], 'interval_ms',"
        " { get() { throw new Error('getter'); } });",
        "globalThis.__hull_internals.__hull_timer_defs[1].hour = 99;",
        "globalThis.__hull_internals.__hull_timer_defs[1].minute = 60;",
        "globalThis.__hull_internals.__hull_timer_defs[1].hour = -1;",
        "globalThis.__hull_internals.__hull_timer_defs[1] = { get type() { throw 1; } };",
        "globalThis.__hull_internals.__hull_ws_defs[0].on_message_id = 'x';",
        "globalThis.__hull_internals.__hull_ws_defs[0].on_message_id = -5;",
        "Object.defineProperty(globalThis.__hull_internals.__hull_ws_defs[0], 'on_message_id',"
        " { get() { throw new Error('getter'); } });",
        "globalThis.__hull_internals.__hull_sse_defs[0].handler_id = { valueOf() { return 0; } };",
    };
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    for (size_t i = 0; i <= sizeof tamper / sizeof tamper[0]; i++) {
        const char *t = i < sizeof tamper / sizeof tamper[0] ? tamper[i] : "";
        init_js();
        ASSERT_TRUE(js_initialized);
        HlAsyncBackendCtx *actx = NULL;
        ASSERT_EQ(be->init(&actx, NULL), 0);
        js.base.async_ctx = actx;
        char code[1024];
        snprintf(code, sizeof code,
            "import { app } from 'hull:app';\n"
            "app.manifest({ modules: ['hull/http-server@1', 'hull/timers@1',"
            " 'hull/web/ws-server@1', 'hull/web/sse@1'] });\n"
            "app.get('/x', (req, res) => {});\n"
            "app.every(100, () => {});\n"
            "app.daily('03:30', () => {});\n"
            "app.ws('/ws', { onMessage: (c, m) => {} });\n"
            "app.sse('/sse', (req, stream) => {});\n"
            "%s\n", t);
        a12_eval_app(code);
        KlHttpServer server;
        KlHttpServerConfig cfg = { .port = 0, .max_connections = 1, .alloc = NULL };
        kl_http_server_init(&server, &cfg);
        int want = t[0] ? -1 : 0;   /* the last round is untampered */
        EXPECT_EQ_MSG(hl_js_wire_routes_server(&js, &server, NULL), want, t);
        EXPECT_FALSE_MSG(JS_HasException(js.ctx), t);
        if (!t[0])
            EXPECT_EQ(js.timer_count, (size_t)2);
        kl_http_server_free(&server);
        cleanup_js();
        be->tick(actx, 0);
        be->free(actx);
    }
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_CLIENT
/* http.async.post / put / patch built the opts they hand http.fetch as a
 * plain object with JS_SetPropertyStr: an Object.prototype setter for body /
 * headers / timeoutMs swallowed the value, and an Object.prototype getter was
 * read back as the call's own option. The opts object has no prototype now
 * and its properties are defined. */
UTEST(js_audit12, polluted_object_prototype_does_not_reach_fetch_opts)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlHttpConfig cfg = {0};
    js.base.http_cfg = &cfg;
    a12_eval_app(
        "import { httpClient as http } from 'hull:http-client';\n"
        "globalThis.__hits = 0;\n"
        /* Frozen since audit 18: each accessor is refused (3 of 3). */
        "globalThis.__refused = 0;\n"
        "for (const k of ['body', 'headers', 'timeoutMs'])\n"
        "  try { Object.defineProperty(Object.prototype, k, { configurable: true,\n"
        "    get() { globalThis.__hits++; return k === 'timeoutMs' ? -1 : {}; },\n"
        "    set(v) { globalThis.__hits++; } }); } catch (e) { globalThis.__refused++; }\n"
        "let msgs = [];\n"
        "for (const f of [() => http.async.post('http://x.invalid/', 'b'),\n"
        "                 () => http.async.put('http://x.invalid/'),\n"
        "                 () => http.async.patch('http://x.invalid/', 'b')])\n"
        "  try { f(); msgs.push('none'); } catch (e) { msgs.push(String(e.message)); }\n"
        "globalThis.__msgs = msgs;\n");
    EXPECT_EQ(eval_int("globalThis.__refused"), 3);
    /* No inherited accessor ran: not while building opts, nor when fetch
     * read them back (a timeoutMs of -1 would have been refused). */
    EXPECT_EQ(eval_int("globalThis.__hits"), 0);
    /* Each call got as far as fetch's own event-loop check. */
    EXPECT_EQ(eval_int("globalThis.__msgs.every(m => /event loop/.test(m)) ? 1 : 0"), 1);
    js.base.http_cfg = NULL;
    cleanup_js_caps();
}
#endif /* HL_ENABLE_HTTP_CLIENT */

UTEST(js_stdlib, audit13_suite)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    static const unsigned char ok[] = "ok";
    static const unsigned char cycle[] = "{% include \"cycle\" %}";
    static const HlEntry templates[] = {
        { "templates/__proto__", ok, sizeof ok - 1 },
        { "templates/constructor", ok, sizeof ok - 1 },
        { "templates/cycle", cycle, sizeof cycle - 1 },
        { "templates/toString", ok, sizeof ok - 1 },
        { NULL, NULL, 0 },
    };
    HlVfs vfs;
    hl_vfs_init(&vfs, templates, NULL);
    js.base.app_vfs = &vfs;
    int pass = 0, fail = -1;
    int rc = run_js_test("stdlib/js/hull/tests/test_stdlib_audit13.js", &pass, &fail);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(fail, 0);
    EXPECT_EQ(pass, 40);
    js.base.app_vfs = NULL;
    cleanup_js_caps();
}

/* ── Audit 15: the __hull_* tables are not app globals ────────────────
 *
 * Routes, middleware, timers, ws / sse handlers and app.main were kept on
 * globalThis, where app code could define them itself: a route or a timer
 * wired without hull/http-server or hull/timers declared. They live in a
 * store held in C now (hl_js_internals); a global of the same name is
 * ignored. */
UTEST(js_audit15, app_globals_cannot_register_handlers)
{
    init_js_bare();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: [] });\n"
        "globalThis.__hull_routes = [() => {}];\n"
        "globalThis.__hull_route_defs = [{ method: 'GET', pattern: '/x', handler_id: 0 }];\n"
        "globalThis.__hull_middleware = [{ method: '*', pattern: '/*', handler_id: 0 }];\n"
        "globalThis.__hull_timers = [() => {}];\n"
        "globalThis.__hull_timer_defs = [{ type: 'every', interval_ms: 1000, handler_id: 0 }];\n"
        "globalThis.__hull_ws_defs = [{ pattern: '/ws', on_message_id: 0 }];\n"
        "globalThis.__hull_sse_defs = [{ pattern: '/sse', handler_id: 0 }];\n"
        "globalThis.__hull_main = () => 7;\n";
    JSValue v = JS_Eval(js.ctx, code, strlen(code), "<a15>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);
    EXPECT_FALSE(hl_js_vtable.has_server_handlers(&js.base));
    EXPECT_FALSE(hl_js_vtable.has_main(&js.base));
    cleanup_js();
}

/* ── Audit 16: the internal stores are defined into, never set ──────────
 *
 * mod_app.c appended to __hull_routes and the def arrays with an indexed SET,
 * and built the defs with set too: an app setter on Array.prototype["0"] (or
 * Object.prototype.method) then ran with `this` = the internal array / def,
 * which reached the handlers after registration closed. Registration must
 * work and the setters / getters must never fire, also when a hole in a def
 * array is read. */
UTEST(js_audit16, registration_ignores_prototype_setters)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    const char *code =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "const grab = (k) => ({ configurable: true,\n"
        "  get() { globalThis.r = this; return undefined; },\n"
        "  set(v) { globalThis.r = this; } });\n"
        /* Frozen since audit 18: all 8 accessors are refused. */
        "globalThis.__refused = 0;\n"
        "const def = (o, k) => { try { Object.defineProperty(o, k, grab(k)); }\n"
        "  catch (e) { globalThis.__refused++; } };\n"
        "for (const i of ['0', '1', '2']) def(Array.prototype, i);\n"
        "for (const k of ['method', 'pattern', 'handler_id', 'multipart',\n"
        "                 'on_open_id'])\n"
        "  def(Object.prototype, k);\n"
        "app.get('/a', (req, res) => res.text('a'));\n"
        "app.use('*', '/*', (req, res) => 0);\n"
        "app.post('/b', (req, res) => res.text('b'));\n";
    JSValue v = JS_Eval(js.ctx, code, strlen(code), "<a16>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    EXPECT_EQ(eval_int("typeof globalThis.r === 'undefined' ? 1 : 0"), 1);
    EXPECT_EQ(eval_int(
        "Object.getOwnPropertyNames(globalThis.__hull_internals"
        ".__hull_route_defs).length"), 3);   /* 0, 2 and length */
    /* A def sits at its handler id (the middleware took id 1), so the def
     * arrays have holes; with no prototype a hole reads as undefined, not
     * through the Array.prototype getter (routes.c walks every index). */
    EXPECT_EQ(eval_int(
        "Object.getPrototypeOf(globalThis.__hull_internals"
        ".__hull_route_defs) === null ? 1 : 0"), 1);
    EXPECT_EQ(eval_int(
        "globalThis.__hull_internals.__hull_route_defs[1] === undefined"
        " ? 1 : 0"), 1);
    EXPECT_EQ(eval_int(
        "Object.getPrototypeOf(globalThis.__hull_internals"
        ".__hull_route_defs[2]) === null ? 1 : 0"), 1);
    EXPECT_EQ(eval_int(
        "Object.getOwnPropertyDescriptor(globalThis.__hull_internals"
        ".__hull_route_defs[2], 'method').value === 'POST' ? 1 : 0"), 1);
    EXPECT_EQ(eval_int(
        "typeof Object.getOwnPropertyDescriptor(globalThis.__hull_internals"
        ".__hull_routes, '2').value === 'function' ? 1 : 0"), 1);
    /* Reading a key a def leaves out does not reach Object.prototype. */
    EXPECT_EQ(eval_int(
        "(() => { const d = globalThis.__hull_internals.__hull_route_defs;\n"
        "  const o = Object.getOwnPropertyDescriptor(d, '0').value;\n"
        "  return ('multipart' in o) ? 0 : 1; })()"), 1);
    EXPECT_EQ(eval_int("typeof globalThis.r === 'undefined' ? 1 : 0"), 1);
    EXPECT_EQ(eval_int("globalThis.__refused"), 8);
    cleanup_js();
}

/* ── Audit 17: a built binary loads modules / templates only from its VFS ─
 *
 * A non-empty app VFS is a built binary: its imports and its templates come
 * from what it embedded. Unfixed, a module or template not embedded was read
 * from <app_dir> on disk - code planted beside the binary ran, without
 * --verify-sig. With no app VFS (development) the disk fallback stays. */

static const unsigned char a17_tpl[] = "embedded template";
static const HlEntry a17_built_entries[] = {
    { "templates/embedded.html", a17_tpl, sizeof a17_tpl - 1 },
    { NULL, NULL, 0 }
};

static void a17_write(const char *dir, const char *rel, const char *text)
{
    char path[HL_TEST_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static void a17_cleanup(const char *dir)
{
    const char *names[] = { "mod_app.js", "tpl_app.js", "planted.js",
                            "templates/planted.html", "templates", NULL };
    char path[HL_TEST_PATH_MAX + 64];
    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        unlink(path);
        rmdir(path);
    }
    rmdir(dir);
}

/* Load <dir>/<entry> in a fresh runtime, the app VFS embedded or not. */
static int a17_load(const char *dir, const char *entry, int built, HlVfs *vfs)
{
    init_js();
    if (!js_initialized) return -99;
    if (built) {
        hl_vfs_init(vfs, a17_built_entries, dir);
        js.base.app_vfs = vfs;
    }
    char path[HL_TEST_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/%s", dir, entry);
    return hl_js_load_app(&js, path);
}

UTEST(js_audit17, imports_and_templates_read_disk_only_in_development)
{
    char dir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(dir, sizeof dir, "hull_a17"), NULL);
    char tdir[HL_TEST_PATH_MAX + 16];
    snprintf(tdir, sizeof tdir, "%s/templates", dir);
    ASSERT_EQ(mkdir(tdir, 0755), 0);
    a17_write(dir, "planted.js", "export const val = 17;\n");
    a17_write(dir, "templates/planted.html", "planted template");
    a17_write(dir, "mod_app.js",
        "import { val } from './planted.js';\n"
        "globalThis.__a17_mod = val;\n");
    a17_write(dir, "tpl_app.js",
        "import { template } from 'hull:template';\n"
        "const t = (n) => { try { return template.render(n, {}); }\n"
        "                   catch (e) { return 'missing'; } };\n"
        "globalThis.__a17_planted = t('planted.html');\n"
        "globalThis.__a17_embedded = t('embedded.html');\n");
    HlVfs vfs;

    /* Development: the module and the template on disk load. */
    EXPECT_EQ(a17_load(dir, "mod_app.js", 0, &vfs), 0);
    EXPECT_EQ(eval_int("globalThis.__a17_mod | 0"), 17);
    EXPECT_EQ(a17_load(dir, "tpl_app.js", 0, &vfs), 0);
    char *got = eval_str("globalThis.__a17_planted");
    EXPECT_TRUE(got && strcmp(got, "planted template") == 0);
    free(got);

    /* Built: neither does; an embedded template still renders. */
    EXPECT_NE(a17_load(dir, "mod_app.js", 1, &vfs), 0);
    EXPECT_EQ(eval_int("globalThis.__a17_mod === undefined ? 1 : 0"), 1);
    js.base.app_vfs = NULL;
    EXPECT_EQ(a17_load(dir, "tpl_app.js", 1, &vfs), 0);
    got = eval_str("globalThis.__a17_planted");
    EXPECT_TRUE(got && strcmp(got, "missing") == 0);
    free(got);
    got = eval_str("globalThis.__a17_embedded");
    EXPECT_TRUE(got && strcmp(got, "embedded template") == 0);
    free(got);
    js.base.app_vfs = NULL;

    cleanup_js();
    a17_cleanup(dir);
}

/* ───────────────────────────────────────────────────────────────────────
 * audit 18: the JS stdlib shares its intrinsics with app code.
 *
 * The Lua stdlib runs in a private environment; the JS stdlib had none, so an
 * app that patched a shared intrinsic observed or steered values the stdlib
 * built from private data - a leak databases.internal did not stop, because it
 * was in-process, not in the SQL text. These cases drive the attack through a
 * real stdlib path and read what leaked. They are regression tests for
 * hl_js_harden_intrinsics: before it, each attack succeeded; after it, the
 * mutation throws or has no effect and nothing private is observed.
 * ─────────────────────────────────────────────────────────────────────── */

/* Eval a module, drain its jobs, and return whether it loaded cleanly (0) or
 * threw / rejected (-1). The attack cases below read their result from a
 * globalThis flag the module sets. */
static int a18_module(const char *code)
{
    JSValue val = JS_Eval(js.ctx, code, strlen(code), "<a18>",
                          JS_EVAL_TYPE_MODULE);
    int bad = JS_IsException(val);
    if (bad) {
        JS_FreeValue(js.ctx, JS_GetException(js.ctx));
    } else {
        hl_js_run_jobs(&js);
        if (JS_IsObject(val) &&
            JS_PromiseState(js.ctx, val) == JS_PROMISE_REJECTED)
            bad = 1;
    }
    JS_FreeValue(js.ctx, val);
    return bad ? -1 : 0;
}

/* (a) A patched Array.prototype.join alters the SQL the stdlib builds. jobs.enqueue
 * assembles its INSERT column list with `cols.join(", ")`; a global patch sees
 * (and could rewrite) that internal array. After the fix the assignment throws. */
UTEST(js_audit18_intrinsics, array_join_cannot_alter_stdlib_sql)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    int rc = a18_module(
        "import { jobs } from 'hull:jobs';\n"
        "const orig = Array.prototype.join;\n"
        "Array.prototype.join = function (sep) {\n"
        "  const s = orig.call(this, sep);\n"
        "  if (typeof s === 'string' && s.indexOf('queue') >= 0 &&\n"
        "      s.indexOf('dedup_key') >= 0) globalThis.__a18_join = s;\n"
        "  return s;\n"
        "};\n"
        "jobs.init();\n"
        "jobs.enqueue('t', { x: 1 });\n");
    /* After the fix: the module throws at the frozen-prototype assignment, so
     * it never reaches enqueue and nothing is captured. */
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(eval_int("typeof globalThis.__a18_join === 'undefined' ? 1 : 0"), 1);
    /* The real join still works for ordinary code. */
    EXPECT_EQ(eval_int("[1,2,3].join('-') === '1-2-3' ? 1 : 0"), 1);
    cleanup_js_caps();
}

/* (b) A patched array-iterator next captures a value the stdlib destructures
 * from private data. totp.decryptSecret does
 * `const [value, err, version] = sealbox.open(...)`, so %ArrayIteratorPrototype%.next
 * sees the decrypted 20-byte TOTP secret. After the fix the patch throws. */
UTEST(js_audit18_intrinsics, array_iterator_cannot_capture_a_destructured_secret)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    int rc = a18_module(
        "import { totp } from 'hull:web:middleware:totp';\n"
        "const H = __testHooks('hull:web:middleware:totp');\n"
        "const ap = Object.getPrototypeOf([][Symbol.iterator]());\n"
        "const nextOrig = ap.next;\n"
        "let leak = null;\n"
        "ap.next = function () {\n"
        "  const r = nextOrig.call(this);\n"
        "  if (!r.done && typeof r.value === 'string' && r.value.length === 20 &&\n"
        "      leak === null) leak = r.value;\n"
        "  return r;\n"
        "};\n"
        "H.reset();\n"
        "totp.init({ issuer: 'T', encryptionKey: 'k'.repeat(32) });\n"
        "const r = totp.enroll('u1');\n"
        "const secret = H.base32Decode(r.secretBase32);\n"
        "const step = H.currentStep();\n"
        "totp.confirm('u1', H.totpAtStep(secret, step, 6));\n"
        "totp.verify('u1', H.totpAtStep(secret, step + 1, 6));\n"
        "globalThis.__a18_leaked = (leak !== null && leak === secret) ? 1 : 0;\n");
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(eval_int("globalThis.__a18_leaked | 0"), 0);
    /* Ordinary destructuring still works. */
    EXPECT_EQ(eval_int("(() => { const [a, b] = [7, 8]; return a + b; })()"), 15);
    cleanup_js_caps();
}

/* (c) Promise.prototype.then could be replaced, but before the fix no
 * stdlib-internal call went through the replacement on jobs.await's path:
 * QuickJS's native `await` on a native promise does not look `then` up, and
 * the row jobs.await resolves with is its public result. Defence in depth:
 * the replacement now throws, so no future stdlib path can depend on it. */
UTEST(js_audit18_intrinsics, promise_then_cannot_observe_internal_rows)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    int rc = a18_module(
        "import { jobs } from 'hull:jobs';\n"
        "let seen = 0;\n"
        "const thenOrig = Promise.prototype.then;\n"
        "Promise.prototype.then = function (a, b) {\n"
        "  seen++;\n"
        "  return thenOrig.call(this, a, b);\n"
        "};\n"
        "jobs.init();\n"
        "const id = jobs.enqueue('t', { x: 1 });\n"
        "globalThis.__a18_then = jobs.await(id, { timeout: 0.05 }).then(\n"
        "  () => { globalThis.__a18_then_seen = seen; return 1; },\n"
        "  () => { globalThis.__a18_then_seen = seen; return 1; });\n");
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(eval_int("typeof globalThis.__a18_then === 'undefined' ? 1 : 0"), 1);
    cleanup_js_caps();
}

/* (d) A replaced global (globalThis.Array) is called by the stdlib. csv.encode
 * guards its input with `Array.isArray`, resolved through the global binding;
 * replacing the global redirects the stdlib's call. After the fix the
 * reassignment throws. */
UTEST(js_audit18_intrinsics, replaced_global_array_is_not_seen_by_the_stdlib)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    int rc = a18_module(
        "import { csv } from 'hull:csv';\n"
        "const real = Array.isArray.bind(Array);\n"
        "globalThis.Array = { isArray: (x) => { globalThis.__a18_arr = 1; return real(x); } };\n"
        "csv.encode([{ a: 1, b: 2 }], { headers: true });\n");
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(eval_int("typeof globalThis.__a18_arr === 'undefined' ? 1 : 0"), 1);
    /* csv still works. */
    char *out = eval_str(
        "(() => { try { return 'ok'; } catch (e) { return String(e); } })()");
    EXPECT_TRUE(out && strcmp(out, "ok") == 0);
    free(out);
    cleanup_js();
}

/* Every reachable standard intrinsic is frozen, and the standard globals are
 * non-writable + non-configurable. */
UTEST(js_audit18_intrinsics, intrinsics_are_frozen_and_globals_locked)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(eval_int(
        "(() => {\n"
        "  const protos = [Object.prototype, Array.prototype, Object.getPrototypeOf(Object),\n"
        "    String.prototype, Number.prototype, Boolean.prototype,\n"
        "    RegExp.prototype, Error.prototype, TypeError.prototype,\n"
        "    Promise.prototype, Map.prototype, Set.prototype, Date.prototype,\n"
        "    Object.getPrototypeOf([][Symbol.iterator]()),\n"
        "    Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())),\n"
        "    Object.getPrototypeOf(''[Symbol.iterator]()),\n"
        "    Object.getPrototypeOf(new Map()[Symbol.iterator]()),\n"
        "    Object.getPrototypeOf(new Set()[Symbol.iterator]()),\n"
        "    Object.getPrototypeOf(function* () {}),\n"
        "    Object.getPrototypeOf(async function () {}),\n"
        "    Object.getPrototypeOf(Int8Array), Int8Array.prototype,\n"
        "    JSON, Math, Reflect];\n"
        "  for (const p of protos) if (p && !Object.isFrozen(p)) return 0;\n"
        "  return 1;\n"
        "})()"), 1);
    /* Globals are non-writable and non-configurable. */
    EXPECT_EQ(eval_int(
        "(() => {\n"
        "  for (const n of ['Object','Array','JSON','Promise','Math','String',\n"
        "      'Reflect','parseInt','encodeURIComponent','globalThis']) {\n"
        "    const d = Object.getOwnPropertyDescriptor(globalThis, n);\n"
        "    if (!d || d.configurable) return 0;\n"
        "    if ('value' in d && d.writable) return 0;\n"
        "  }\n"
        "  return 1;\n"
        "})()"), 1);
    /* globalThis itself is NOT frozen - apps still define globals. */
    EXPECT_EQ(eval_int(
        "(() => { globalThis.__a18_app_global = 42;\n"
        "  return globalThis.__a18_app_global === 42 ? 1 : 0; })()"), 1);
    cleanup_js();
}

/* The override-mistake enablement: a subclass constructor assigns name/message,
 * and an instance overrides toString, both against frozen prototypes; assigning
 * to the prototype itself throws. */
UTEST(js_audit18_intrinsics, override_mistake_is_enabled)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(eval_int(
        "(() => {\n"
        "  class X extends Error { constructor() { super('boom'); this.name = 'X'; } }\n"
        "  const e = new X();\n"
        "  if (e.name !== 'X' || e.message !== 'boom') return 0;\n"
        "  const o = {}; o.toString = () => 'custom';\n"
        "  if (o.toString() !== 'custom') return 0;\n"
        "  function Foo() {} Foo.prototype.toString = () => 'foo';\n"
        "  if (new Foo().toString() !== 'foo') return 0;\n"
        "  const f = Object.assign(() => 1, { toString: () => 'f' });\n"
        "  if (String(f) !== 'f') return 0;\n"
        "  class E2 extends TypeError { constructor() { super('m'); this.name = 'E2'; } }\n"
        "  if (new E2().name !== 'E2') return 0;\n"
        "  if (({}).toString() !== '[object Object]') return 0;\n"
        "  return 1;\n"
        "})()"), 1);
    /* Assigning to the prototype itself throws. */
    EXPECT_EQ(eval_int(
        "(() => { try { Object.prototype.toString = () => 'x'; return 0; }\n"
        "  catch (e) { return e instanceof TypeError ? 1 : 0; } })()"), 1);
    cleanup_js();
}

/* A native module's export object is shared by app code and the stdlib that
 * imports the same module: an app that replaced crypto.random would run inside
 * totp.enroll's secret generation. The exports are frozen when created. */
UTEST(js_audit18_intrinsics, native_module_exports_are_frozen)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(a18_module(
        "import { crypto } from 'hull:crypto';\n"
        "import { db } from 'hull:db';\n"
        "import { json } from 'hull:json';\n"
        "let r = 0;\n"
        "try { crypto.random = () => 'x'; } catch (e) { if (e instanceof TypeError) r |= 1; }\n"
        "try { db.default = () => null; } catch (e) { if (e instanceof TypeError) r |= 2; }\n"
        "try { json.encode = () => ''; } catch (e) { if (e instanceof TypeError) r |= 4; }\n"
        "if (Object.isFrozen(crypto) && Object.isFrozen(db) && Object.isFrozen(json)) r |= 8;\n"
        "if (typeof crypto.random(4) === 'object') r |= 16;\n"
        "globalThis.__a18_exp = r;\n"), 0);
    EXPECT_EQ(eval_int("globalThis.__a18_exp"), 31);
    cleanup_js_caps();
}

/* A stdlib JS module's export object is shared the same way: totp calls
 * sealbox.open(keyring(), ...) through hull:crypto:sealbox's exported object,
 * so an app that replaced sealbox.open received the keyring. The loader
 * freezes every embedded stdlib module's exports at the end of its body. */
UTEST(js_audit18_intrinsics, stdlib_module_exports_are_frozen)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(a18_module(
        "import { sealbox } from 'hull:crypto:sealbox';\n"
        "import { jobs } from 'hull:jobs';\n"
        "let r = 0;\n"
        "try { sealbox.open = () => [null, 'x']; } catch (e) { if (e instanceof TypeError) r |= 1; }\n"
        "try { jobs.enqueue = () => 0; } catch (e) { if (e instanceof TypeError) r |= 2; }\n"
        "if (Object.isFrozen(sealbox) && Object.isFrozen(jobs)) r |= 4;\n"
        "if (typeof sealbox.open === 'function') r |= 8;\n"
        "globalThis.__a18_sexp = r;\n"), 0);
    EXPECT_EQ(eval_int("globalThis.__a18_sexp"), 15);
    cleanup_js_caps();
}

/* Hull's own globals are locked and frozen with the standard ones: the
 * stdlib awaits the bare `hull.sleep` (retry.run, jobs.await), so an app that
 * replaced it - or the whole `hull` / `console` binding - ran inside the
 * stdlib's calls. The stdlib still reaches the real hull.sleep. */
UTEST(js_audit18_intrinsics, hull_globals_are_locked)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(a18_module(
        "import { retry } from 'hull:retry';\n"
        "const realSleep = hull.sleep;\n"
        "const fake = () => { globalThis.__a18_fake = 1; return Promise.resolve(); };\n"
        "let r = 0;\n"
        "try { hull.sleep = fake; } catch (e) { if (e instanceof TypeError) r |= 1; }\n"
        "try { globalThis.hull = { sleep: fake, map: fake }; } catch (e) { if (e instanceof TypeError) r |= 2; }\n"
        "try { globalThis.console = { log: fake }; } catch (e) { if (e instanceof TypeError) r |= 4; }\n"
        "try { delete globalThis.hull; } catch (e) { if (e instanceof TypeError) r |= 8; }\n"
        "if (Object.isFrozen(hull) && Object.isFrozen(console) &&\n"
        "    hull.sleep === realSleep) r |= 16;\n"
        "globalThis.__a18_hg = r;\n"
        /* One failed attempt, then the stdlib's own `await hull.sleep(...)`. */
        "let n = 0;\n"
        "retry.run(() => { n++; if (n === 1) throw new Error('x'); return 1; },\n"
        "          { maxAttempts: 2, baseMs: 1, jitter: 0 }).catch(() => {});\n"), 0);
    EXPECT_EQ(eval_int("globalThis.__a18_hg"), 31);
    EXPECT_EQ(eval_int("typeof globalThis.__a18_fake === 'undefined' ? 1 : 0"), 1);
    cleanup_js_caps();
}

/* Hardening fails closed: an object it cannot freeze fails the call (and so
 * init), never leaves the object mutable with init succeeding. */
UTEST(js_audit18_intrinsics, harden_fails_closed)
{
    JSRuntime *rt = JS_NewRuntime();
    ASSERT_TRUE(rt != NULL);
    JSContext *ctx = JS_NewContext(rt);
    ASSERT_TRUE(ctx != NULL);
    static const char plant[] =
        "globalThis.unfreezable = new Proxy({}, { preventExtensions() { return false; } });";
    JSValue v = JS_Eval(ctx, plant, sizeof plant - 1, "<a18>", JS_EVAL_TYPE_GLOBAL);
    EXPECT_FALSE(JS_IsException(v));
    JS_FreeValue(ctx, v);
    EXPECT_EQ(hl_js_harden_intrinsics(ctx), -1);
    JS_FreeContext(ctx);

    /* A clean context hardens. */
    ctx = JS_NewContext(rt);
    ASSERT_TRUE(ctx != NULL);
    EXPECT_EQ(hl_js_harden_intrinsics(ctx), 0);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* Audit 19: the walk reaches an intrinsic only through a global or the
 * hidden list, and it missed %IteratorConcatPrototype% - the prototype of
 * every Iterator.concat result - so its `next` / `return` stayed writable
 * for every iterator Iterator.concat makes. It is in the list now, and every
 * class prototype the context holds is frozen and checked from C. */
UTEST(js_audit19_intrinsics, iterator_concat_prototype_is_frozen)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(eval_int(
        "(() => {\n"
        "  if (typeof Iterator === 'undefined' || typeof Iterator.concat !== 'function')\n"
        "    return 1;   /* not in this build */\n"
        "  const p = Object.getPrototypeOf(Iterator.concat([]));\n"
        "  if (!Object.isFrozen(p)) return 0;\n"
        "  const next = p.next;\n"
        "  try { p.next = () => ({ done: true }); } catch (e) { /* strict */ }\n"
        "  return p.next === next ? 1 : 0;\n"
        "})()"), 1);
    cleanup_js();
}

/* The C check: a class prototype harden cannot freeze fails init, and one
 * the walk never reached is frozen. */
UTEST(js_audit19_intrinsics, every_class_prototype_is_frozen_or_init_fails)
{
    JSRuntime *rt = JS_NewRuntime();
    ASSERT_TRUE(rt != NULL);
    JSClassID id = 0;
    JS_NewClassID(&id);
    JSClassDef def = { .class_name = "A19Probe" };
    ASSERT_EQ(JS_NewClass(rt, id, &def), 0);

    /* A plain prototype no global reaches: frozen. */
    JSContext *ctx = JS_NewContext(rt);
    ASSERT_TRUE(ctx != NULL);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "m", JS_NewInt32(ctx, 1));
    JS_SetClassProto(ctx, id, proto);
    EXPECT_EQ(hl_js_harden_intrinsics(ctx), 0);
    JSValue got = JS_GetClassProto(ctx, id);
    EXPECT_EQ(JS_IsExtensible(ctx, got), 0);
    JS_FreeValue(ctx, got);
    JS_FreeContext(ctx);

    /* One that cannot be frozen: init fails. */
    ctx = JS_NewContext(rt);
    ASSERT_TRUE(ctx != NULL);
    static const char mk[] =
        "new Proxy({}, { preventExtensions() { return false; } })";
    JSValue bad = JS_Eval(ctx, mk, sizeof mk - 1, "<a19>", JS_EVAL_TYPE_GLOBAL);
    ASSERT_FALSE(JS_IsException(bad));
    JS_SetClassProto(ctx, id, bad);
    EXPECT_EQ(hl_js_harden_intrinsics(ctx), -1);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* The router used to be built from a temporary global, __hull_app_for_router,
 * which app code could define first; the app is now passed as an argument,
 * and no such global is left behind. */
UTEST(js_audit19_intrinsics, router_needs_no_temporary_global)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(a18_module(
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "let r = 0;\n"
        "if (!('__hull_app_for_router' in globalThis)) r |= 1;\n"
        "if (typeof app.router === 'function' && app.router('/x').prefix === '/x') r |= 2;\n"
        "globalThis.__a19_router = r;\n"), 0);
    EXPECT_EQ(eval_int("globalThis.__a19_router"), 3);
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Audit 19: jobs' listener map and health's check map were plain objects,
 * so jobs.on("toString", fn) found Object.prototype.toString instead of
 * "unknown event", and health.register("__proto__", fn) replaced the map's
 * prototype instead of registering a check. */
UTEST(js_audit19_stdlib, name_maps_have_no_prototype)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(a18_module(
        "import { jobs } from 'hull:jobs';\n"
        "import { health } from 'hull:web:middleware:health';\n"
        "let r = 0;\n"
        "try { jobs.on('toString', () => {}); }\n"
        "catch (e) { if (/unknown event/.test(String(e && e.message))) r |= 1; }\n"
        "health.register('__proto__', () => true);\n"
        "const out = health.runChecks({ dbCheck: false });\n"
        "const c = Object.getOwnPropertyDescriptor(out.checks, '__proto__');\n"
        "if (c && c.value && c.value.status === 'ok') r |= 2;\n"
        "health.unregister('__proto__');\n"
        "globalThis.__a19_maps = r;\n"), 0);
    EXPECT_EQ(eval_int("globalThis.__a19_maps"), 3);
    cleanup_js_caps();
}
#endif

/* Audit 20: retry.run with maxAttempts -1 / 0.5 never called fn (the Lua
 * twin agrees now); cookie.serialize refused only ';' CR LF NUL in a path and
 * wrote Max-Age=Infinity. */
UTEST(js_audit20_stdlib, retry_and_cookie_parity)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(a18_module(
        "import { retry } from 'hull:retry';\n"
        "import { cookie } from 'hull:web:cookie';\n"
        "let r = 0;\n"
        "let calls = 0;\n"
        "for (const m of [-1, 0.5]) {\n"
        "  retry.run(() => { calls++; return 9; }, { maxAttempts: m })\n"
        "    .then((v) => { if (v === 9) r += 1; });\n"
        "}\n"
        "try { cookie.serialize('a', 'b', { path: '/x\\x01' }); } catch (e) { r += 10; }\n"
        "try { cookie.serialize('a', 'b', { path: '/x\\x7f' }); } catch (e) { r += 100; }\n"
        "try { cookie.serialize('a', 'b', { maxAge: Infinity }); } catch (e) { r += 1000; }\n"
        "globalThis.__a20_r = () => r + calls * 10000;\n"), 0);
    EXPECT_EQ(eval_int("globalThis.__a20_r()"), 21112);
    cleanup_js_caps();
}

#ifdef HL_ENABLE_HTTP_SERVER
/* Audit 20: since #786 a cancelled hull.sleep completes its run, which for a
 * timer handler clears the HlJSTimer's in_flight and reschedules it. Freed
 * on its own, hl_js_free freed the timers first and cancelled the pending
 * sleeps after, writing into a freed HlJSTimer (ASan reports it). The
 * shipped teardowns run stop_async first; hl_js_free must be safe without. */
UTEST(js_audit20, free_with_a_timer_parked_on_sleep)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_js();
    ASSERT_TRUE(js_initialized);
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    js.base.async_ctx = actx;
    a12_eval_app(
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1', 'hull/timers@1'] });\n"
        "app.get('/x', (req, res) => {});\n"
        "app.every(100, async () => {\n"
        "  globalThis.__a20_fired = 1;\n"
        "  try { await hull.sleep(60000); }\n"
        "  catch (e) { globalThis.__a20_err = String(e && e.message); }\n"
        "});\n");
    KlHttpServer server;
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 1, .alloc = NULL };
    kl_http_server_init(&server, &cfg);
    ASSERT_EQ(hl_js_wire_routes_server(&js, &server, NULL), 0);
    for (int i = 0; i < 100 && !js.sleep_timers; i++)
        be->tick(actx, 20);
    EXPECT_EQ(eval_int("globalThis.__a20_fired | 0"), 1);
    char *err = eval_str("String(globalThis.__a20_err)");
    EXPECT_STREQ(err, "undefined");
    free(err);
    EXPECT_TRUE(js.sleep_timers != NULL);   /* the handler is parked */
    kl_http_server_free(&server);
    cleanup_js();                            /* no stop_async first */
    /* A timer the cancel rescheduled onto a freed HlJSTimer would fire
     * here; with the runtime shutting down none is added. */
    be->tick(actx, 200);
    be->free(actx);
}
#endif

#ifdef HL_ENABLE_HTTP_SERVER
/* Audit 22: a timer handler whose last Hull op resumed while it still
 * awaited a promise nothing drives was never completed: in_flight stayed 1
 * and the timer never fired again (a ws on_close the same way never tore
 * its conn down). Its run ends at that resume now. */
UTEST(js_audit22, a_timer_waiting_on_nothing_hull_drives_fires_again)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_js();
    ASSERT_TRUE(js_initialized);
    HlAsyncBackendCtx *actx = NULL;
    ASSERT_EQ(be->init(&actx, NULL), 0);
    js.base.async_ctx = actx;
    a12_eval_app(
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1', 'hull/timers@1'] });\n"
        "app.get('/x', (req, res) => {});\n"
        "app.every(100, async () => {\n"
        "  globalThis.__a22_fired = (globalThis.__a22_fired | 0) + 1;\n"
        "  await hull.sleep(5);\n"
        "  await new Promise(() => {});\n"
        "});\n");
    KlHttpServer server;
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 1, .alloc = NULL };
    kl_http_server_init(&server, &cfg);
    ASSERT_EQ(hl_js_wire_routes_server(&js, &server, NULL), 0);
    for (int i = 0; i < 200 && eval_int("globalThis.__a22_fired | 0") < 3; i++)
        be->tick(actx, 20);
    EXPECT_GE(eval_int("globalThis.__a22_fired | 0"), 3);
    kl_http_server_free(&server);
    cleanup_js();
    be->tick(actx, 0);
    be->free(actx);
}
#endif

/* Audit 21: an op a synchronous entry did not await (`http.fetch(x)` in a
 * ws on_close) captured the entry's completion hook - the ws conn teardown.
 * The entry ran that teardown itself when it returned (freeing the conn);
 * cancelling the op later (a teardown, a hull.sleep cancel) ran it again on
 * the freed conn, since hl_js_run_claim lets a never-wired continuation
 * complete. Only a continuation wired into a run completes its handler. */
static int a21_hook_calls;
static void a21_hook(HlJS *j, void *ud) { (void)j; (void)ud; a21_hook_calls++; }

UTEST(js_audit21, cancel_of_an_unawaited_op_runs_no_completion_hook)
{
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js, JSValue resolve,
        JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    init_js();
    ASSERT_TRUE(js_initialized);
    a21_hook_calls = 0;

    /* Never wired: the entry returned without awaiting it. */
    js.active_on_complete     = a21_hook;
    js.active_on_complete_ctx = NULL;
    HlAsyncCont *c = hl_js_async_cont_create(&js, JS_UNDEFINED, JS_UNDEFINED,
                                             NULL, NULL);
    js.active_on_complete = NULL;
    js.last_async_cont    = NULL;     /* as the entry does when it returns */
    ASSERT_TRUE(c != NULL);
    c->cancel(c);
    c->destroy(c);
    EXPECT_EQ(a21_hook_calls, 0);

    /* Wired into a run the entry parked on: its cancel ends the handler. */
    js.active_on_complete = a21_hook;
    c = hl_js_async_cont_create(&js, JS_UNDEFINED, JS_UNDEFINED, NULL, NULL);
    js.active_on_complete = NULL;
    ASSERT_TRUE(c != NULL);
    JSValue p = JS_Eval(js.ctx, "new Promise(() => {})", 21, "<a21>",
                        JS_EVAL_TYPE_GLOBAL);
    hl_js_run_wire(hl_js_cont_link(c), js.ctx, p, NULL);
    JS_FreeValue(js.ctx, p);
    js.last_async_cont = NULL;
    c->cancel(c);
    c->destroy(c);
    EXPECT_EQ(a21_hook_calls, 1);
    cleanup_js();
}

/* ── Audit 21 G6: off-heap copies and store walks are charged / capped ── */

#ifdef HL_ENABLE_HTTP_SERVER
/* res.bytes copied a view of any size onto Keel's (unlimited) allocator. */
UTEST(js_audit21, res_bytes_refuses_a_body_past_the_heap_limit)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 1 << 20;
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(a5_middleware(
        "(req, res) => { globalThis.__a21 = 0;"
        " try { res.bytes(new ArrayBuffer(2 << 20)); }"
        " catch (e) { globalThis.__a21 = String(e.message).includes('heap limit') ? 7 : 8; }"
        " return 1; }", &req, &res), 1);
    EXPECT_EQ(eval_int("globalThis.__a21"), 7);
    js.max_heap_bytes = saved;
    free_req_ctx(&req);
    kl_http_response_free(&res);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* blob.count / totalSize / iter / cleanup walked every shard directory
 * (256, or 65536 with shardDepth 2) for a few instructions of charge. */
UTEST(js_audit21, blob_store_walks_are_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    ASSERT_EQ(a10_run_tripped(
        "import { blob } from 'hull:blob';\n"
        "globalThis.blob = blob; blob.init({ dir: 'blobs' });\n", 0), 0);
    /* 200 walks x 256 shard directories > 40k units. */
    EXPECT_EQ(a10_run_tripped("for (let i = 0; i < 200; i++) blob.count();", 40000), 1);
    EXPECT_EQ(a10_run_tripped("for (let i = 0; i < 200; i++) blob.totalSize();", 40000), 1);
    cleanup_js_caps();
    a15_fs_close(&f);
}

/* A full SIGBUS guard table (256 live mappings) refused fs.mmap for the
 * whole process; buffers held only by dropped cycles waited for the GC. */
UTEST(js_audit21, mmap_collects_and_retries_when_the_guard_table_is_full)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    char *r = a15_throws(
        "for (let i = 0; i < 300; i++) {"
        "  const o = { m: fs.mmap('big.bin') }; o.self = o; }");
    EXPECT_TRUE(r && strcmp(r, "no throw") == 0);
    if (r && strcmp(r, "no throw") != 0) printf("  got: %s\n", r);
    free(r);
    JS_RunGC(JS_GetRuntime(js.ctx));
    cleanup_js_caps();
    a15_fs_close(&f);
}

#ifdef HL_ENABLE_WASM
/* compute.stream's ArrayBuffer copy was neither charged nor held on the
 * VM's off-heap total; compute.segment's copy (up to 1 GiB) was not charged. */
UTEST(js_audit21, compute_stream_and_segment_copies_are_bounded)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 1 << 20;
    char *r = a15_throws("compute.stream('echo', new ArrayBuffer(2 << 20),"
                         " () => {}, { chunkSize: 65536 })");
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    if (r && !strstr(r, "heap limit")) printf("  got: %s\n", r);
    free(r);
    js.max_heap_bytes = saved;
    EXPECT_EQ(js.wasm_buf_bytes, (size_t)0);   /* nothing left held */
    /* 4 MB string: ~65k units to build, 512k more for the segment copy. */
    EXPECT_EQ(a10_run_tripped(
        "compute.segment('echo', 's', 'x'.repeat(4 << 20));", 300000), 1);
    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}

#endif /* HL_ENABLE_WASM */

/* A worker.dispatch VM runs app functions, so it is hardened too. */
UTEST(js_audit18_intrinsics, worker_dispatch_vm_is_hardened)
{
    const HlAsyncBackend *be;
    HlAsyncBackendCtx *actx;
    HlAsyncBackendPool *pool;
    ASSERT_EQ(js_worker_open(&be, &actx, &pool), 0);
    a7_worker_db();   /* declare hull/db, so the worker VM gets its `db` */
    char out[512] = "(no verdict)";
    js_worker_run(be, actx,
        "  const r = await worker.dispatch(() => Object.isFrozen(Array.prototype) &&\n"
        "    Object.isFrozen(Object.prototype) ? 'frozen' : 'open');\n"
        "  check(r === 'frozen', 'worker: ' + r);\n"
        /* The `db` global the worker hooks install is locked and frozen. */
        "  const d = await worker.dispatch(() => {\n"
        "    if (typeof db !== 'object') return 'no db';\n"
        "    const g = Object.getOwnPropertyDescriptor(globalThis, 'db');\n"
        "    return Object.isFrozen(db) && !g.writable && !g.configurable ? 'locked' : 'open';\n"
        "  });\n"
        "  check(d === 'locked', 'worker db: ' + d);\n",
        out, sizeof out);
    EXPECT_STREQ(out, "ok");
    js_worker_close(be, actx, pool);
}

/* Audit 20: the worker built the job's ctx by assignment, and the worker VM's
 * Object.prototype is frozen: a key naming one of its inherited properties
 * threw (left pending into the call, the key dropped), and `__proto__` set
 * the prototype instead of a key. Both arrive as own data properties now. */
UTEST(js_audit20, worker_ctx_keys_are_own_properties)
{
    const HlAsyncBackend *be;
    HlAsyncBackendCtx *actx;
    HlAsyncBackendPool *pool;
    ASSERT_EQ(js_worker_open(&be, &actx, &pool), 0);
    char out[512] = "(no verdict)";
    js_worker_run(be, actx,
        "  const r = await worker.dispatch((c) => {\n"
        "    const g = Object.getOwnPropertyDescriptor(c, '__defineGetter__');\n"
        "    const p = Object.getOwnPropertyDescriptor(c, '__proto__');\n"
        "    return (g ? g.value : 'none') + ',' + (p ? p.value : 'none') + ','\n"
        "      + (Object.getPrototypeOf(c) === Object.prototype);\n"
        "  }, { __defineGetter__: 7, ['__proto__']: 8, plain: 9 });\n"
        "  check(r === '7,8,true', 'worker ctx: ' + r);\n",
        out, sizeof out);
    EXPECT_STREQ(out, "ok");
    js_worker_close(be, actx, pool);
}

/* ── Audit 23 G2: a binding's loop over app strings stops at the budget ─ */

/* JS_ToCStringLen's ASCII path charged its scan but ignored the trip, so a
 * binding converting an array that names one big string N times rescanned
 * it N times with nothing polled in between: 32766 x 16 MB here, hours of
 * one call, past the budget and the run deadline. */
UTEST(js_audit23, a_string_conversion_past_the_budget_fails_the_call)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(a10_run_tripped("globalThis.__a23big = 'x'.repeat(16 << 20);", 0), 0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    EXPECT_EQ(a10_run_tripped(
        "try { db.query('SELECT 1', new Array(32766).fill(__a23big)); } catch (e) {}\n"
        "globalThis.__a23_after = 1;\n", 1000000), 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    EXPECT_LT((double)(t1.tv_sec - t0.tv_sec), 10.0);
    EXPECT_EQ(eval_int("globalThis.__a23_after | 0"), 0);   /* uncatchable */
    cleanup_js_caps();
}

/* A db param that was not a string, number, boolean or null bound as NULL,
 * silently: a blob round-trip stored NULL. Buffers bind as a BLOB, a BigInt
 * in range as an integer, and anything else throws. */
UTEST(js_audit23, db_params_bind_buffers_and_refuse_other_objects)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(eval_int(
        "db.exec('CREATE TABLE a23b (v)');"
        "db.exec('INSERT INTO a23b VALUES (?)', [new Uint8Array([1, 2, 3])]);"
        "db.exec('INSERT INTO a23b VALUES (?)', [new Uint8Array([9, 8, 7, 6]).buffer]);"
        "db.exec('INSERT INTO a23b VALUES (?)', [new Uint16Array([1, 2]).subarray(1)]);"
        "db.exec('INSERT INTO a23b VALUES (?)', [5n]);"
        "const r = db.query('SELECT typeof(v) AS t, hex(v) AS h FROM a23b ORDER BY rowid');"
        "r.map(x => x.t + ':' + x.h).join(',') ==="
        " 'blob:010203,blob:09080706,blob:0200,integer:35' ? 1 : 0"), 1);
    EXPECT_EQ(eval_int(
        "let n = 0;"
        "for (const v of [{}, new Date(0), Symbol('s'), 2n ** 70n, () => 1]) {"
        "  try { db.exec('INSERT INTO a23b VALUES (?)', [v]); } catch (e) {"
        "    if (e instanceof TypeError || e instanceof RangeError) n++; } }"
        "n"), 5);
    EXPECT_EQ(eval_int("db.query('SELECT count(*) AS c FROM a23b')[0].c"), 4);
    /* and through db.async's copy */
    EXPECT_EQ(eval_int("db.query('SELECT ? AS v', [new Uint8Array([7])])[0].v"
                       ".byteLength"), 1);
    cleanup_js_caps();
}

/* The worker.dispatch VM's db binding did the same. */
JS_WORKER_CASE(db_params_bind_buffers_in_a_worker,
    a7_worker_db(),
    "  const r = await worker.dispatch(() => {\n"
    "    db.exec('CREATE TABLE a23w (v)');\n"
    "    db.exec('INSERT INTO a23w VALUES (?)', [new Uint8Array([1, 2])]);\n"
    "    let m = 'none';\n"
    "    try { db.exec('INSERT INTO a23w VALUES (?)', [{}]); } catch (e) { m = e.name; }\n"
    "    const row = db.query('SELECT typeof(v) AS t, hex(v) AS h FROM a23w')[0];\n"
    "    return row.t + ':' + row.h + ',' + m; });\n"
    "  check(r === 'blob:0102,TypeError', String(JSON.stringify(r)));\n")

/* ── Audit 22 G1: async input copies are held to the heap limit ──────── */

/* db.async params and a worker.dispatch ctx were copied into plain malloc,
 * uncounted: an ASCII string converts without a copy, so 64 references to one
 * 1 MB string cost one string on the heap and 64 MB off it. */
UTEST(js_audit22, async_param_and_ctx_copies_are_held_to_the_heap_limit)
{
    const HlAsyncBackend *be;
    HlAsyncBackendCtx *actx;
    HlAsyncBackendPool *pool;
    ASSERT_EQ(js_worker_open(&be, &actx, &pool), 0);
    ASSERT_EQ(eval_int("globalThis.__s = 'x'.repeat(1 << 20); 1"), 1);
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 16u << 20;
    char *r = a15_throws("db.async.exec('SELECT ?', new Array(64).fill(__s))");
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    if (r && !strstr(r, "heap limit")) printf("  got: %s\n", r);
    free(r);
    r = a15_throws("const c = {}; for (let i = 0; i < 32; i++) c['k' + i] = __s;"
                   " worker.dispatch(() => 1, c)");
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    if (r && !strstr(r, "heap limit")) printf("  got: %s\n", r);
    free(r);
    EXPECT_EQ(js.wasm_buf_bytes, (size_t)0);
    /* One copy is held while its op is in flight, and given back with it. */
    ASSERT_EQ(eval_int("globalThis.__p = worker.dispatch(() => 1, { a: __s });"
                       " __p.catch(() => {}); 1"), 1);
    EXPECT_GE(js.wasm_buf_bytes, (size_t)(1u << 20));
    for (int i = 0; i < 200 && js.wasm_buf_bytes; i++) {
        be->tick(actx, 20);
        hl_js_run_jobs(&js);
    }
    EXPECT_EQ(js.wasm_buf_bytes, (size_t)0);
    js.max_heap_bytes = saved;
    js_worker_close(be, actx, pool);
}

/* A wide string param whose UTF-8 copy did not fit the heap was bound as
 * NULL and the statement reported success; and JS_ToCStringLen2 kept its
 * reference to the string (QuickJS HULL PATCH 0007). */
UTEST(js_audit22, a_failed_string_conversion_leaks_nothing)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(eval_int("db.exec('CREATE TABLE a22 (v TEXT)');"
                       " globalThis.__w = '一'.repeat(1 << 20); 1"), 1);
    JSMemoryUsage u0;
    JS_ComputeMemoryUsage(js.rt, &u0);
    JS_SetMemoryLimit(js.rt, (size_t)u0.malloc_size + (1u << 20));
    int rc = eval_int("(() => { try { db.exec('INSERT INTO a22 VALUES (?)', [__w]);"
                      " return 1; } catch (e) { return 2; } })()");
    JS_SetMemoryLimit(js.rt, (size_t)u0.malloc_limit);
    EXPECT_EQ(rc, 2);
    EXPECT_EQ(eval_int("db.query('SELECT count(*) AS n FROM a22')[0].n"), 0);
    ASSERT_EQ(eval_int("globalThis.__w = undefined; 1"), 1);
    JS_RunGC(js.rt);
    JSMemoryUsage u1;
    JS_ComputeMemoryUsage(js.rt, &u1);
    EXPECT_LT(u1.malloc_size, u0.malloc_size - (1 << 20));
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Audit 22: http.* sent a caller Host / Transfer-Encoding / Connection
 * as given - a second Host line, or a smuggled request. */
UTEST(js_audit22, http_refuses_headers_hulls_client_writes)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlHttpConfig cfg = {0};          /* no hosts: every request is refused */
    js.base.http_cfg = &cfg;
    static const char *const cases[] = {
        "http.get('http://x.invalid/', { headers: { Host: 'other.example' } })",
        "http.post('http://x.invalid/', 'b', { headers: { 'Transfer-Encoding': 'chunked' } })",
        "http.request('GET', 'http://x.invalid/', { headers: { 'bad name': 'v' } })",
    };
    char code[1024];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(code, sizeof code,
                 "import { httpClient as http } from 'hull:http-client';\n"
                 "let r = 0;\n"
                 "try { %s; r = 2; } catch (e) {\n"
                 "  r = /may not be set/.test(e.message) ? 1 : 3; }\n"
                 "globalThis.__h22 = r;\n", cases[i]);
        JSValue v = JS_Eval(js.ctx, code, strlen(code), "<h22>", JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) hl_js_dump_error(&js);
        JS_FreeValue(js.ctx, v);
        hl_js_run_jobs(&js);
        EXPECT_EQ_MSG(eval_int("globalThis.__h22|0"), 1, cases[i]);
    }
    js.base.http_cfg = NULL;
    cleanup_js_caps();
}
#endif

#ifdef HL_ENABLE_HTTP_SERVER
/* Audit 22: a test file whose top level threw rejected its module promise
 * rather than raising, and the runner freed it unexamined - the tests before
 * the throw ran, the rest never existed, and the file passed. */
/* Audit 23 (after #809): a test file whose top level awaits what nothing
 * settles stopped there; the tests registered before the await ran and
 * the file passed. */
UTEST(js_audit23, a_test_file_whose_top_level_never_finishes_is_a_load_error)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpRouter router;
    KlAllocator alloc = kl_allocator_default();
    kl_http_router_init(&router, &alloc);
    hl_js_test_register(js.ctx, &router, &js);

    char dir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(dir, sizeof dir, "hull_a23t"), NULL);
    char path[HL_TEST_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/test_pending.js", dir);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fputs("test('before', () => {});\n"
          "await new Promise(() => {});\n"
          "test('after', () => { throw new Error('never registered'); });\n", f);
    fclose(f);

    HlTestCaseResult results[4] = {{0}};
    int total = 0, passed = 0, failed = 0;
    const char *err = NULL;
    int rc = hl_js_vtable.run_test_file(&js.base, path, results, 4,
                                        &total, &passed, &failed, &err);
    EXPECT_EQ(rc, -1);
    EXPECT_TRUE(err && strstr(err, "never finished") != NULL);
    if (err && !strstr(err, "never finished")) printf("  got: %s\n", err);

    remove(path);
    rmdir(dir);
    kl_http_router_free(&router);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(js_audit22, a_test_file_whose_top_level_throws_is_a_load_error)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    KlHttpRouter router;
    KlAllocator alloc = kl_allocator_default();
    kl_http_router_init(&router, &alloc);
    hl_js_test_register(js.ctx, &router, &js);

    char dir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(dir, sizeof dir, "hull_a22t"), NULL);
    char path[HL_TEST_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/test_boom.js", dir);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fputs("test('before', () => {});\n"
          "throw new Error('boom at the top level');\n"
          "test('after', () => { throw new Error('never registered'); });\n", f);
    fclose(f);

    HlTestCaseResult results[4] = {{0}};
    int total = 0, passed = 0, failed = 0;
    const char *err = NULL;
    int rc = hl_js_vtable.run_test_file(&js.base, path, results, 4,
                                        &total, &passed, &failed, &err);
    EXPECT_EQ(rc, -1);
    EXPECT_TRUE(err && strstr(err, "boom at the top level") != NULL);
    if (err && !strstr(err, "boom")) printf("  got: %s\n", err);

    remove(path);
    rmdir(dir);
    kl_http_router_free(&router);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* ── Audit 23 G1: async RESULT copies are held to the heap limit ─────── */

/* A worker.dispatch return value was copied off the heap with no bound (an
 * object of keys naming one big string, once per key), and a db.async
 * result was capped per op, so the ops in flight held that cap many times
 * over. Both now take their bytes from the VM's async_result_bytes. */
UTEST(js_audit23, async_result_copies_are_held_to_the_heap_limit)
{
    const HlAsyncBackend *be;
    HlAsyncBackendCtx *actx;
    HlAsyncBackendPool *pool;
    ASSERT_EQ(js_worker_open(&be, &actx, &pool), 0);
    a3_worker_db();
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 16u << 20;
    char out[512] = "(no verdict)";
    js_worker_run(be, actx,
        "  const m = await fails(() => {\n"
        "    const s = 'x'.repeat(1 << 20), o = {};\n"
        "    for (let i = 0; i < 64; i++) o['k' + i] = s;\n"
        "    return o; });\n"
        "  check(m.includes('result too large'), m);\n"
        "  const small = await worker.dispatch(() => ({ a: 'b' }));\n"
        "  check(small.a === 'b', JSON.stringify(small));\n",
        out, sizeof out);
    EXPECT_STREQ(out, "ok");
    EXPECT_EQ(js.async_result_bytes, (size_t)0);

    /* Other results in flight already hold all but 64 KiB of the limit. */
    js.async_result_bytes = (16u << 20) - (64u << 10);
    snprintf(out, sizeof out, "(no verdict)");
    eval_int("delete globalThis.__wk; 1");
    js_worker_run(be, actx,
        "  let m = '(resolved)';\n"
        "  try { await db.async.query('SELECT zeroblob(262144) AS z'); }\n"
        "  catch (e) { m = String(e && e.message || e); }\n"
        "  check(m.includes('in-flight'), m);\n",
        out, sizeof out);
    EXPECT_STREQ(out, "ok");
    EXPECT_EQ(js.async_result_bytes, (size_t)((16u << 20) - (64u << 10)));
    js.async_result_bytes = 0;
    eval_int("delete globalThis.__wk; 1");
    js_worker_run(be, actx,
        "  const rows = await db.async.query('SELECT zeroblob(262144) AS z');\n"
        "  check(rows[0].z.byteLength === 262144, String(rows[0].z));\n",
        out, sizeof out);
    EXPECT_STREQ(out, "ok");
    EXPECT_EQ(js.async_result_bytes, (size_t)0);
    js.max_heap_bytes = saved;
    js_worker_close(be, actx, pool);
}

/* Audit 22: json.decode parsed up to the string's first NUL. */
UTEST(js_audit22, json_decode_reads_the_whole_string)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a10_run_tripped(
        "import { json } from 'hull:json';\n"
        "let r = 0;\n"
        "try { json.decode('{\"a\":1}\\u0000junk'); r = 2; } catch (e) { r = 1; }\n"
        "globalThis.__a22j = r + (json.decode('{\"b\":2}').b === 2 ? 10 : 0);\n", 0), 0);
    EXPECT_EQ(eval_int("globalThis.__a22j | 0"), 11);
    cleanup_js_caps();
}

/* Audit 23: option getters whose conversion result was ignored - a throwing
 * getter or valueOf left its exception pending while the binding carried on
 * with a default (tar built the archive with mode 0, blob.cleanup made a
 * throwing dryRun a real, deleting run). The call now fails with the
 * getter's own exception. */
UTEST(js_audit23, tar_option_getters_that_throw_fail_the_call)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a10_run_tripped(
        "import { tar } from 'hull:archive:tar';\n"
        "const msg = (f) => { try { f(); return 'no throw'; }"
        " catch (e) { return String(e && e.message); } };\n"
        "globalThis.__a23t = [\n"
        "  msg(() => tar.create([{ name: 'f', data: 'x',"
        " get mode() { throw new Error('mode boom'); } }])),\n"
        "  msg(() => tar.create([{ name: 'f', data: 'x',"
        " mode: { valueOf() { throw new Error('valueOf boom'); } } }])),\n"
        "  msg(() => tar.create([{ get name() { throw new Error('name boom'); } }])),\n"
        "  msg(() => tar.create([{ name: 'f', get isDir() { throw new Error('dir boom'); } }])),\n"
        "  msg(() => tar.create([{ name: 'f', get data() { throw new Error('data boom'); } }])),\n"
        "].join('|');\n", 0), 0);
    char *r = eval_str("globalThis.__a23t");
    EXPECT_TRUE(r && strcmp(r, "mode boom|valueOf boom|name boom|dir boom|data boom") == 0);
    if (r && strcmp(r, "mode boom|valueOf boom|name boom|dir boom|data boom") != 0)
        printf("  got: %s\n", r);
    free(r);
    cleanup_js_caps();
}

UTEST(js_audit23, blob_cleanup_with_a_throwing_dry_run_deletes_nothing)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    ASSERT_EQ(a10_run_tripped(
        "import { blob } from 'hull:blob';\n"
        "globalThis.blob = blob; blob.init({ dir: 'blobs' });\n"
        "blob.put('one'); blob.put('two');\n", 0), 0);
    EXPECT_EQ(eval_int("blob.count()"), 2);
    char *r = a15_throws("blob.cleanup({ maxTotalSize: 0,"
                         " get dryRun() { throw new Error('dry boom'); } })");
    EXPECT_TRUE(r && strcmp(r, "dry boom") == 0);
    if (r && strcmp(r, "dry boom") != 0) printf("  got: %s\n", r);
    free(r);
    EXPECT_EQ(eval_int("blob.count()"), 2);
    r = a15_throws("blob.init({ dir: 'blobs',"
                   " get shardDepth() { throw new Error('depth boom'); } })");
    EXPECT_TRUE(r && strcmp(r, "depth boom") == 0);
    free(r);
    cleanup_js_caps();
    a15_fs_close(&f);
}

#ifdef HL_ENABLE_WASM
UTEST(js_audit23, compute_option_getters_that_throw_fail_the_call)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    static const struct { const char *expr; const char *want; } cases[] = {
        { "compute.call('echo', 'x', { get gas() { throw new Error('gas boom'); } })",
          "gas boom" },
        { "compute.call('echo', 'x', { timeoutMs: { valueOf() {"
          " throw new Error('tmo boom'); } } })", "tmo boom" },
        { "compute.call('echo', 'x', { get buffer() { throw new Error('buf boom'); } })",
          "buf boom" },
        /* spans: the name taken before them was leaked on this path. */
        { "compute.call('echo', 'x', { get spans() { throw new Error('spans boom'); } })",
          "spans boom" },
        { "compute.instance('echo', { get heap() { throw new Error('heap boom'); } })",
          "heap boom" },
        { "compute.stream('echo', 'x', { get chunkSize() {"
          " throw new Error('chunk boom'); } })", "chunk boom" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *r = a15_throws(cases[i].expr);
        EXPECT_TRUE_MSG(r && strcmp(r, cases[i].want) == 0, cases[i].expr);
        if (r && strcmp(r, cases[i].want) != 0) printf("  got: %s\n", r);
        free(r);
    }
    /* The plain call still works. */
    EXPECT_EQ(eval_int("compute.call('echo', 'abc', { gas: 1000000 }).byteLength"), 3);
    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}
#endif /* HL_ENABLE_WASM */

/* Audit 24: worker.dispatch's ctx copy stored a throwing getter as null and
 * dispatched with the exception pending; a getter that threw on the
 * returned object was a null field of a successful result. Both fail now,
 * with the getter's own error. */
JS_WORKER_CASE(a24_throwing_ctx_and_result_getters_fail_the_dispatch, (void)0,
    "  let m = 'no throw';\n"
    "  try { await worker.dispatch(() => 1,"
    " { a: 1, get b() { throw new Error('ctx boom'); } }); }\n"
    "  catch (e) { m = e.message; }\n"
    "  check(m === 'ctx boom', 'ctx getter: ' + m);\n"
    "  const r = await worker.dispatch(() =>"
    " ({ a: 1, get b() { throw new Error('ret boom'); } }));\n"
    "  check(r && typeof r.error === 'string' && r.error.includes('ret boom'),"
    " 'result getter: ' + JSON.stringify(r));\n"
    "  const ok = await worker.dispatch((c) => c.a + 1, { a: 41 });\n"
    "  check(ok === 42, 'plain dispatch: ' + ok);\n")

/* Audit 24: the params array's length was read as int32 - 2**31 wrapped
 * negative and bound no params, 2**32 + 1 bound one - and an element
 * getter's error was replaced by "params must be an array". */
UTEST(js_audit24, db_params_length_is_not_wrapped)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    static const struct { const char *expr; const char *want; } cases[] = {
        { "(() => { const a = []; a.length = 2 ** 31;"
          " return db.query('SELECT 1', a); })()", "too many" },
        { "db.query('SELECT 1', new Proxy([], { get: (t, k) =>"
          " k === 'length' ? 2 ** 32 + 1 : undefined }))", "too many" },
        { "db.exec('SELECT ?', new Proxy([], { get: (t, k) =>"
          " k === 'length' ? 1 : (() => { throw new Error('elem boom'); })() }))",
          "elem boom" },
        { "db.query('SELECT ?', { length: 1, 0: 1 })", "params must be an array" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *r = a15_throws(cases[i].expr);
        EXPECT_TRUE_MSG(r && strstr(r, cases[i].want) != NULL, cases[i].expr);
        if (r && !strstr(r, cases[i].want)) printf("  got: %s\n", r);
        free(r);
    }
    EXPECT_EQ(eval_int("db.query('SELECT ? AS v', [5])[0].v"), 5);
    cleanup_js_caps();
}

/* The [app] records log.c printed while a24_log_collect was on. */
static char g_a24_log[2048];
static int g_a24_log_on;
static void a24_log_collect(log_Event *ev)
{
    if (!g_a24_log_on || !ev->fmt) return;
    size_t n = strlen(g_a24_log);
    if (n + 2 >= sizeof g_a24_log) return;
    vsnprintf(g_a24_log + n, sizeof g_a24_log - n - 1, ev->fmt, ev->ap);
    n = strlen(g_a24_log);
    g_a24_log[n] = '|';
    g_a24_log[n + 1] = '\0';
}

/* Audit 24: log.* / console.* skipped an argument whose toString threw and
 * returned with the exception pending; a CR / LF in app text started a
 * forged log line. */
UTEST(js_audit24, log_and_console_fail_on_a_throwing_tostring_and_escape_newlines)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    static int registered;
    if (!registered) {
        log_add_callback(a24_log_collect, NULL, LOG_TRACE);
        registered = 1;
    }
    ASSERT_EQ(a10_run_tripped(
        "import { log } from 'hull:log';\n"
        "const msg = (f) => { try { f(); return 'no throw'; }"
        " catch (e) { return String(e && e.message); } };\n"
        "const bad = { toString() { throw new Error('str boom'); } };\n"
        "globalThis.__a24l = [msg(() => log.info('a', bad)),"
        " msg(() => console.log(bad)), msg(() => log.warn('fine'))].join('|');\n"
        "globalThis.__a24log = log;\n", 0), 0);
    char *r = eval_str("globalThis.__a24l");
    EXPECT_TRUE(r && strcmp(r, "str boom|str boom|no throw") == 0);
    if (r && strcmp(r, "str boom|str boom|no throw") != 0) printf("  got: %s\n", r);
    free(r);

    g_a24_log[0] = '\0';
    g_a24_log_on = 1;
    char *v = eval_str("(globalThis.__a24log.info('one\\r\\nINFO forged'),"
                       " console.log('two\\nthree'), 'done')");
    g_a24_log_on = 0;
    free(v);
    EXPECT_TRUE(strstr(g_a24_log, "[app] one\\r\\nINFO forged|") != NULL);
    EXPECT_TRUE(strstr(g_a24_log, "[app] two\\nthree|") != NULL);
    EXPECT_TRUE(strchr(g_a24_log, '\n') == NULL && strchr(g_a24_log, '\r') == NULL);
    if (!strstr(g_a24_log, "two\\nthree")) printf("  got: %s\n", g_a24_log);

    /* Charged: a large message trips a small budget. */
    EXPECT_EQ(a10_run_tripped("globalThis.__a24big = 'x'.repeat(4000000);", 0), 0);
    g_a24_log_on = 0;
    EXPECT_EQ(a10_run_tripped("console.log(__a24big);", 100000), 1);
    cleanup_js_caps();
}

/* Audit 24: Hull called whatever main's promise's `then` was - an own
 * `then` ran app code after main's entry was parked and was handed the
 * settle callbacks, so it could end main at once with any exit code. */
UTEST(js_audit24, an_own_then_on_mains_promise_does_not_settle_main)
{
    init_js();
    HlAsyncBackendCtx *loop = NULL;
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_EQ(be->init(&loop, NULL), 0);
    js.base.async_ctx = loop;
    ASSERT_TRUE(register_cli_main("import { app } from 'hull:app';"
        "app.main(() => { const p = hull.sleep(1).then(() => 7);"
        " Object.defineProperty(p, 'then', { value(ok) { globalThis.__a24t = 1;"
        " ok(42); } });"
        " return p; });"));
    int exit_code = 99;
    EXPECT_EQ(hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code), -1);
    EXPECT_NE(exit_code, 42);
    EXPECT_EQ(eval_int("globalThis.__a24t | 0"), 0);
    EXPECT_TRUE(js.sleep_timers == NULL);
    EXPECT_EQ(js.cli_main_active, 0);
    cleanup_js();
    EXPECT_EQ(be->tick(loop, 0), 0);
    be->free(loop);

    /* An untouched async main still waits and exits with its value. */
    init_js();
    ASSERT_EQ(be->init(&loop, NULL), 0);
    js.base.async_ctx = loop;
    ASSERT_TRUE(register_cli_main("import { app } from 'hull:app';"
        "app.main(async () => { await hull.sleep(1); return 9; });"));
    exit_code = 99;
    EXPECT_EQ(hl_js_vtable.run_main(&js.base, NULL, 0, NULL, NULL, &exit_code), 0);
    EXPECT_EQ(exit_code, 9);
    cleanup_js();
    be->free(loop);
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Audit 24: ws.connect passed Keel no config, so every wss:// failed with a
 * generic error. It takes the client TLS config http.fetch uses now, and with
 * none says so up front (the allowlist still runs first). */
UTEST(js_audit24, wss_connect_needs_the_client_tls_config)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    static const char *hosts[] = { "ws.example" };
    HlHttpConfig cfg = {0};
    cfg.allowed_hosts = hosts;
    cfg.count = 1;
    js.base.http_cfg = &cfg;
    static const struct { const char *url; const char *msg; } cases[] = {
        { "wss://ws.example/x",    "wss:// needs TLS" },
        { "wss://other.example/x", "not in allowlist" },
        { "ws://ws.example/x",     "requires running server" },
    };
    char code[1024];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(code, sizeof code,
                 "import { wsClient as ws } from 'hull:web:ws-client';\n"
                 "let r = 0;\n"
                 "try { ws.connect('%s', {}); r = 2; } catch (e) {\n"
                 "  r = String(e.message).includes('%s') ? 1 : 3; }\n"
                 "globalThis.__wss = r;\n", cases[i].url, cases[i].msg);
        JSValue v = JS_Eval(js.ctx, code, strlen(code), "<wss>", JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) hl_js_dump_error(&js);
        JS_FreeValue(js.ctx, v);
        hl_js_run_jobs(&js);
        EXPECT_EQ_MSG(eval_int("globalThis.__wss|0"), 1, cases[i].url);
    }
    js.base.http_cfg = NULL;
    cleanup_js_caps();
}
#endif

/* ── Audit 25 ────────────────────────────────────────────────────────── */

/* M4: db.batch's async probe reads fn's prototype, which runs a Proxy
 * getPrototypeOf trap - app code that can close the db.open connection the
 * batch is on. The handle read again after it was passed to
 * hl_db_batch_enter unchecked: a NULL dereference. */
UTEST(js_audit25, batch_on_a_connection_closed_by_the_async_probe)
{
    /* The cap-level guard: a closed connection is a failure, not a crash. */
    EXPECT_EQ(hl_db_batch_enter(NULL), -1);
    EXPECT_EQ(hl_db_batch_leave(NULL, 1), -1);
    EXPECT_EQ(hl_db_batch_leave(NULL, 0), -1);

    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    static HlManifest m;
    memset(&m, 0, sizeof m);
    m.databases.dynamic.declared = 1;
    m.databases.dynamic.schemes[0] = "sqlite";
    m.databases.dynamic.scheme_count = 1;
    hl_db_registry_set_manifest(test_db_registry, &m);
    ASSERT_EQ(a4_module(
        "import { db as dbMod } from 'hull:db';\n"
        "const c = dbMod.open(':memory:');\n"
        "const fp = Object.getPrototypeOf(() => {});\n"
        "const fn = new Proxy(() => {}, {\n"
        "  getPrototypeOf() { c.close(); return fp; } });\n"
        "let msg = 'no error';\n"
        "try { c.batch(fn); } catch (e) { msg = String(e && e.message || e); }\n"
        "globalThis.__a25_batch = msg;\n"), 0);
    char *msg = eval_str("globalThis.__a25_batch");
    ASSERT_NE(msg, NULL);
    EXPECT_NE_MSG(strstr(msg, "closed"), NULL, msg);
    free(msg);
    hl_db_registry_set_manifest(test_db_registry, NULL);
    cleanup_js_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* L4 / L5: a request body that is an ArrayBuffer or a typed array is sent;
 * anything else that is not a string / undefined / null throws a TypeError
 * - it was dropped silently and the request went out with no body. */
UTEST(js_audit25, http_body_types)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlHttpConfig cfg = {0};          /* no hosts: every request is refused */
    js.base.http_cfg = &cfg;
    static const char *const bad[] = {
        "http.post('http://x.invalid/', {})",
        "http.put('http://x.invalid/', 42)",
        "http.patch('http://x.invalid/', [1, 2])",
        "http.request('POST', 'http://x.invalid/', { body: { a: 1 } })",
        "http.request('POST', 'http://x.invalid/', { body: 7 })",
    };
    static const char *const good[] = {
        "http.post('http://x.invalid/', 'text')",
        "http.post('http://x.invalid/', new Uint8Array([0, 255, 1]))",
        "http.put('http://x.invalid/', new ArrayBuffer(4))",
        "http.post('http://x.invalid/', undefined)",
        "http.post('http://x.invalid/', null)",
        "http.request('POST', 'http://x.invalid/', { body: new Uint8Array(3) })",
        "http.request('POST', 'http://x.invalid/', { body: null })",
    };
    char code[1024];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(code, sizeof code,
                 "import { httpClient as http } from 'hull:http-client';\n"
                 "let r = 0;\n"
                 "try { %s; r = 2; } catch (e) {\n"
                 "  r = e instanceof TypeError && /body must be/.test(e.message)"
                 " ? 1 : 3; }\n"
                 "globalThis.__hb = r;\n", bad[i]);
        JSValue v = JS_Eval(js.ctx, code, strlen(code), "<hb>", JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) hl_js_dump_error(&js);
        JS_FreeValue(js.ctx, v);
        hl_js_run_jobs(&js);
        EXPECT_EQ_MSG(eval_int("globalThis.__hb|0"), 1, bad[i]);
    }
    /* Accepted: the call gets as far as the host check, which refuses. */
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        snprintf(code, sizeof code,
                 "import { httpClient as http } from 'hull:http-client';\n"
                 "let r = 0;\n"
                 "try { %s; r = 2; } catch (e) {\n"
                 "  r = /failed/.test(e.message) && !/body/.test(e.message)"
                 " ? 1 : 3; }\n"
                 "globalThis.__hb = r;\n", good[i]);
        JSValue v = JS_Eval(js.ctx, code, strlen(code), "<hb>", JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) hl_js_dump_error(&js);
        JS_FreeValue(js.ctx, v);
        hl_js_run_jobs(&js);
        EXPECT_EQ_MSG(eval_int("globalThis.__hb|0"), 1, good[i]);
    }
    js.base.http_cfg = NULL;
    cleanup_js_caps();
}
#endif

#ifdef HL_ENABLE_HTTP_SERVER
/* L6: a binary WebSocket frame reaches onMessage as an ArrayBuffer with its
 * bytes intact; it was a UTF-8-decoded string. A text frame stays a string. */
UTEST(js_audit25, ws_binary_frame_is_an_array_buffer)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    HlAllocator wa;
    hl_alloc_init(&wa, 0);
    HlWsRegistry reg;
    hl_ws_registry_init(&reg, &wa);
    js.base.ws_registry = &reg;
    a18_ws_route_handler(0, "(c) => {}");
    a18_ws_route_handler(1,
        "(c, m, b) => { globalThis.__a25ws = (m instanceof ArrayBuffer)"
        "  ? 'ab:' + Array.from(new Uint8Array(m)).join(',') + ':' + b"
        "  : typeof m + ':' + m + ':' + b; }");
    a18_ws_route_handler(2, "(c, code) => {}");
    HlJSWsRoute route;
    memset(&route, 0, sizeof route);
    route.js = &js;
    route.on_open_id = 0;
    route.on_message_id = 1;
    route.on_close_id = 2;
    snprintf(route.path, sizeof route.path, "/a25");
    static char p1;
    KlWsServerConn *kc = (KlWsServerConn *)(void *)&p1;
    hl_js_ws_on_open(kc, &route);
    hl_js_ws_on_message(kc, "\xff\x00\x80", 3, 1, &route);
    char *s = eval_str("globalThis.__a25ws");
    ASSERT_NE(s, NULL);
    EXPECT_STREQ(s, "ab:255,0,128:true");
    free(s);
    hl_js_ws_on_message(kc, "hi", 2, 0, &route);
    s = eval_str("globalThis.__a25ws");
    ASSERT_NE(s, NULL);
    EXPECT_STREQ(s, "string:hi:false");
    free(s);
    hl_js_ws_on_close(kc, 1000, NULL, 0, &route);
    js.base.ws_registry = NULL;
    hl_ws_registry_free(&reg);
    cleanup_js();
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* L7: a row value that cannot be made (out of memory here) fails the query
 * with that exception; it was stored as the column's value and the partial
 * result returned with the exception left pending. */
UTEST(js_audit25, query_row_value_that_cannot_be_made_fails_the_query)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(eval_int("globalThis.__a25q = 'unset'; 1"), 1);
    JS_RunGC(js.rt);
    JSMemoryUsage u;
    JS_ComputeMemoryUsage(js.rt, &u);
    JS_SetMemoryLimit(js.rt, (size_t)u.malloc_size + (2u << 20));
    ASSERT_EQ(a4_module(
        "import { db as dbMod } from 'hull:db';\n"
        "const c = dbMod.default();\n"
        "let r = 'returned';\n"
        "try { c.query('SELECT zeroblob(8000000) AS b'); }\n"
        "catch (e) { r = 'threw'; }\n"
        "globalThis.__a25q = r;\n"), 0);
    JS_SetMemoryLimit(js.rt, (size_t)u.malloc_limit);
    EXPECT_FALSE(JS_HasException(js.ctx));
    char *s = eval_str("globalThis.__a25q");
    ASSERT_NE(s, NULL);
    EXPECT_STREQ(s, "threw");
    free(s);
    /* The connection still works. */
    EXPECT_EQ(eval_int("db.query('SELECT 5 AS n')[0].n"), 5);
    cleanup_js_caps();
}

/* L8: a UDF returning a BigInt binds an integer and a buffer a BLOB; a value
 * with no SQL counterpart is an error. All three were NULL silently. */
UTEST(js_audit25, udf_result_types)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a4_module(
        "import { db as dbMod } from 'hull:db';\n"
        "const c = dbMod.default();\n"
        "c.udf.register('hull_a25_big', () => 9007199254740993n);\n"
        "c.udf.register('hull_a25_huge', () => 2n ** 70n);\n"
        "c.udf.register('hull_a25_buf', () => new Uint8Array([1, 2, 3]));\n"
        "c.udf.register('hull_a25_ab', () => new Uint8Array([9, 8, 7, 6]).buffer);\n"
        "c.udf.register('hull_a25_obj', () => ({ a: 1 }));\n"
        "const out = [];\n"
        "let r = c.query('SELECT hull_a25_big() = 9007199254740993 AS v,'\n"
        "                + ' typeof(hull_a25_big()) AS t')[0];\n"
        "out.push(r.t, String(r.v));\n"
        "r = c.query('SELECT hex(hull_a25_buf()) AS h, typeof(hull_a25_buf()) AS t')[0];\n"
        "out.push(r.t, r.h);\n"
        "r = c.query('SELECT hex(hull_a25_ab()) AS h')[0];\n"
        "out.push(r.h);\n"
        "for (const f of ['hull_a25_huge', 'hull_a25_obj']) {\n"
        "  let m = 'no error';\n"
        "  try { c.query('SELECT ' + f + '() AS v'); } catch (e) { m = 'error'; }\n"
        "  out.push(m);\n"
        "}\n"
        "for (const f of ['hull_a25_big', 'hull_a25_huge', 'hull_a25_buf', 'hull_a25_ab', 'hull_a25_obj'])\n"
        "  c.udf.unregister(f);\n"
        "globalThis.__a25udf = out.join('|');\n"), 0);
    char *s = eval_str("globalThis.__a25udf");
    ASSERT_NE(s, NULL);
    EXPECT_STREQ(s, "integer|1|blob|010203|09080706|error|error");
    free(s);
    EXPECT_FALSE(JS_HasException(js.ctx));
    cleanup_js_caps();
}

#ifdef HL_ENABLE_HTTP_SERVER
/* L9: test.get's opts getters and conversions can throw; the call threw
 * nothing (the exception was swallowed or left pending) and a header whose
 * value did not convert leaked its name. The exception is the call's now. */
UTEST(js_audit25, test_request_opts_exceptions_propagate)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    const char *app =
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "app.get('/a25', (req, res) => { res.json({ ok: true }); });\n";
    JSValue v = JS_Eval(js.ctx, app, strlen(app), "<a25app>", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);
    hl_js_run_jobs(&js);

    KlHttpRouter router;
    KlAllocator kalloc = kl_allocator_default();
    kl_http_router_init(&router, &kalloc);
    ASSERT_EQ(hl_js_wire_routes(&js, &router), 0);
    hl_js_test_register(js.ctx, &router, &js);
    const char *cases =
        "function err(o) { try { test.get('/a25', o); return 'none'; }"
        "  catch (e) { return String(e && e.message); } }\n"
        "test('opts exceptions', () => {\n"
        "  const r = [\n"
        "    err({ get middleware() { throw new Error('mw'); } }),\n"
        "    err({ get body() { throw new Error('body'); } }),\n"
        "    err({ get headers() { throw new Error('hdrs'); } }),\n"
        "    err({ headers: { a: 'x', get b() { throw new Error('hval'); } } }),\n"
        "    err({ headers: { a: { toString() { throw new Error('hstr'); } } } }),\n"
        "    err({ get ctx() { throw new Error('ctx'); } }),\n"
        "    err({ ctx: { toJSON() { throw new Error('json'); } } }),\n"
        "  ].join(',');\n"
        "  if (r !== 'mw,body,hdrs,hval,hstr,ctx,json') throw new Error(r);\n"
        "  if (test.get('/a25', { headers: { a: 'x' } }).status !== 200)\n"
        "    throw new Error('status');\n"
        "});\n";
    v = JS_Eval(js.ctx, cases, strlen(cases), "<a25cases>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) hl_js_dump_error(&js);
    JS_FreeValue(js.ctx, v);

    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[2];
    memset(results, 0, sizeof results);
    hl_js_test_run(js.ctx, &total, &passed, &failed, NULL, results, 2);
    EXPECT_EQ(total, 1);
    EXPECT_TRUE_MSG(results[0].passed, results[0].error);

    kl_http_router_free(&router);
    cleanup_js_caps();
}
#endif

#ifdef HL_ENABLE_IMAGE
/* Audit 25 (Lua parity): image.new, image.fromWasm and image.fromBuffer's
 * copy branch copied w * h * bpp bytes uncharged. The copy is refused past
 * the VM heap limit and charged 1 unit per 8 bytes before it: a large copy
 * trips a small instruction limit, a small one fits. */
UTEST(js_audit25, image_pixel_copies_are_charged)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    if (eval_int("typeof image === 'object' && image !== null ? 1 : 0") != 1) {
        cleanup_js();   /* image compiled out */
        return;
    }
    /* 4096 x 4096 r8 = 16 MiB = 2M units; a 64 x 64 copy is 512. */
    ASSERT_EQ(eval_int(
        "globalThis.big = new ArrayBuffer(1 << 24);"
        " globalThis.small = new ArrayBuffer(64 * 64);"
        " globalThis.wasmImg = (w, h) => { const b = new Uint8Array(9 + w * h);"
        "   b[0] = w & 255; b[1] = w >> 8; b[4] = h & 255; b[5] = h >> 8;"
        "   b[8] = 2; return b.buffer; };"
        " globalThis.bigW = wasmImg(4096, 4096);"
        " globalThis.smallW = wasmImg(64, 64); 1"), 1);
    static const char *const small[] = {
        "image.new(64, 64, 'r8', small).close(); 1",
        "image.fromBuffer(small, 64, 64, 'r8').close(); 1",
        "image.fromWasm(smallW).close(); 1",
    };
    static const char *const large[] = {
        "image.new(4096, 4096, 'r8', big); 1",
        "image.fromBuffer(big, 4096, 4096, 'r8'); 1",
        "image.fromWasm(bigW); 1",
    };
    for (size_t i = 0; i < 3; i++) {
        js.max_instructions = 1000000;
        hl_js_budget_arm(&js);
        EXPECT_EQ_MSG(eval_int(small[i]), 1, small[i]);
        EXPECT_EQ_MSG(js.budget_tripped, 0, small[i]);
        EXPECT_EQ_MSG(eval_int(large[i]), -9999, large[i]);
        EXPECT_EQ_MSG(js.budget_tripped, 1, large[i]);
        js.max_instructions = 0;
        hl_js_budget_arm(&js);
    }
    /* Past the heap limit: refused before the copy, as a RangeError. */
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 1u << 20;
    char *r = eval_str("(() => { try { image.new(4096, 4096, 'r8', big); "
                       "return 'made'; } catch (e) { return e.name + ': ' + e.message; } })()");
    js.max_heap_bytes = saved;
    EXPECT_TRUE(r && strstr(r, "RangeError") && strstr(r, "too large"));
    free(r);
    /* Dimensions hl_image_new rejects are not charged: the usual error. */
    r = eval_str("(() => { try { image.new(0, 4096, 'r8', big); "
                 "return 'made'; } catch (e) { return e.name; } })()");
    EXPECT_STREQ(r ? r : "(null)", "RangeError");
    free(r);
    cleanup_js();
}
#endif /* HL_ENABLE_IMAGE */

/* ── Audit 27 ──────────────────────────────────────────────────────── */

/* verifyPassword charged the PBKDF2 rounds but not the password's own
 * pre-hash (Lua charges both): a 24 MB password over the minimum count cost
 * ~2M units and hashed 24 MB on top. 1 unit per 8 password bytes now. */
UTEST(js_audit27, password_prehash_is_charged)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(eval_int("(globalThis.PW = 'x'.repeat(24 << 20),"
                       " globalThis.ST = 'pbkdf2:100000:' + '0'.repeat(32)"
                       " + ':' + '0'.repeat(64), 1)"), 1);
    js.max_instructions = 3000000;
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int("crypto.verifyPassword(PW, ST) ? 1 : 2"), -9999);
    EXPECT_EQ(js.budget_tripped, 1);
    /* A short password over the same count fits. */
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int("crypto.verifyPassword('pw', ST) ? 1 : 2"), 2);
    EXPECT_EQ(js.budget_tripped, 0);
    js.max_instructions = 0;
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int("(globalThis.PW = undefined, 1)"), 1);
    cleanup_js();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* http.request / post / put / patch never charged a body, and an
 * ArrayBuffer body was copied with malloc before anything sized it. The body
 * is charged (1 unit per 8 bytes) and refused past the heap limit before it
 * is copied, so it trips the budget before the host check refuses it. */
UTEST(js_audit27, http_bodies_are_charged)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlHttpConfig cfg = {0};          /* no hosts: every request is refused */
    js.base.http_cfg = &cfg;
    ASSERT_EQ(a10_run_tripped(
        "import { httpClient as http } from 'hull:http-client';\n"
        "globalThis.http = http;\n"
        "globalThis.HB = new ArrayBuffer(8 << 20);\n"
        "globalThis.HS = 'y'.repeat(8 << 20);\n", 0), 0);
    /* 8 MB is 1M units. */
    static const char *const big[] = {
        "try { http.post('http://x.invalid/', HB); } catch (e) {}",
        "try { http.put('http://x.invalid/', new Uint8Array(HB)); } catch (e) {}",
        "try { http.patch('http://x.invalid/', HS); } catch (e) {}",
        "try { http.request('POST', 'http://x.invalid/', { body: HB }); }"
        " catch (e) {}",
    };
    for (size_t i = 0; i < sizeof big / sizeof big[0]; i++)
        EXPECT_EQ_MSG(a10_run_tripped(big[i], 500000), 1, big[i]);
    EXPECT_EQ(a10_run_tripped(
        "try { http.post('http://x.invalid/', new ArrayBuffer(64)); }"
        " catch (e) {}", 500000), 0);
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 4 << 20;
    char *r = a15_throws("http.post('http://x.invalid/', HB)");
    js.max_heap_bytes = saved;
    EXPECT_TRUE(r && strstr(r, "heap limit") != NULL);
    if (r && !strstr(r, "heap limit")) printf("  got: %s\n", r);
    free(r);
    EXPECT_EQ(eval_int("(globalThis.HB = globalThis.HS = undefined, 1)"), 1);
    js.base.http_cfg = NULL;
    cleanup_js_caps();
}
#endif

/* tar.extract(buf, undefined) converted the directory to the text
 * "undefined" and extracted into ./undefined/. undefined / null is no
 * directory, as in Lua. */
UTEST(js_audit27, tar_extract_undefined_dir_is_the_root)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    ASSERT_EQ(a10_run_tripped(
        "import { tar } from 'hull:archive:tar';\n"
        "globalThis.tar = tar;\n"
        "globalThis.TB = tar.create([{ name: 'a27u', data: 'u' },"
        " { name: 'a27n', data: 'n' }]);\n", 0), 0);
    EXPECT_EQ(eval_int("tar.extract(TB, undefined) === true ? 1 : 0"), 1);
    EXPECT_EQ(eval_int("tar.extract(TB, null) === true ? 1 : 0"), 1);
    char p[HL_TEST_PATH_MAX + 32];
    struct stat st;
    snprintf(p, sizeof p, "%s/a27u", f.dir);
    EXPECT_EQ(0, stat(p, &st));
    snprintf(p, sizeof p, "%s/undefined", f.dir);
    EXPECT_NE(0, stat(p, &st));
    snprintf(p, sizeof p, "%s/null", f.dir);
    EXPECT_NE(0, stat(p, &st));
    EXPECT_EQ(eval_int("(globalThis.TB = undefined, 1)"), 1);
    cleanup_js_caps();
    a15_fs_close(&f);
}

#ifdef HL_ENABLE_WASM
/* The synchronous compute.call / instance.call charged neither input nor
 * output: a 512 KB echo cost a handful of steps. Input and output are each
 * charged 1 unit per 8 bytes now, and a maxOutput above the heap limit comes
 * down to it. */
UTEST(js_audit27, sync_compute_calls_are_charged)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(eval_int("(globalThis.CB = new ArrayBuffer(512 << 10),"
                       " globalThis.CI = compute.instance('echo'), 1)"), 1);
    /* Unbudgeted, the echo works. */
    EXPECT_EQ(eval_int("compute.call('echo', CB).byteLength"), 512 << 10);
    EXPECT_EQ(eval_int("CI.call(CB).byteLength"), 512 << 10);
    /* 512 KB in + 512 KB out: 131072 units. */
    static const char *const big[] = {
        "compute.call('echo', CB).byteLength",
        "CI.call(CB).byteLength",
    };
    for (size_t i = 0; i < sizeof big / sizeof big[0]; i++) {
        js.max_instructions = 100000;
        hl_js_reset_request(&js);
        EXPECT_EQ_MSG(eval_int(big[i]), -9999, big[i]);
        EXPECT_EQ_MSG(js.budget_tripped, 1, big[i]);
    }
    js.max_instructions = 100000;
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int("compute.call('echo', 'abc').byteLength"), 3);
    EXPECT_EQ(eval_int("CI.call('abcd').byteLength"), 4);
    EXPECT_EQ(js.budget_tripped, 0);
    js.max_instructions = 0;
    hl_js_reset_request(&js);
    EXPECT_EQ(eval_int("(CI.close(), globalThis.CB = globalThis.CI ="
                       " undefined, 1)"), 1);
    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}

/* WasmInstance.call without a maxOutput fell to the instance's own default,
 * which the heap clamp never saw: the guest's output was copied off the
 * heap in full before the ArrayBuffer step refused it (audit 28). The cap is
 * now resolved (per call, else the instance's, else the global default) and
 * brought to the VM heap limit before the call, as Lua does. */
UTEST(js_audit28, instance_call_default_output_is_heap_clamped)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(eval_int("(globalThis.CB = new ArrayBuffer(512 << 10),"
                       " globalThis.CI = compute.instance('echo',"
                       " { maxOutput: 1 << 20 }), 1)"), 1);
    EXPECT_EQ(eval_int("CI.call(CB).byteLength"), 512 << 10);
    size_t heap = js.max_heap_bytes;
    js.max_heap_bytes = 256 << 10;
    char *msg = eval_str("(() => { try { CI.call(CB); return 'ok'; }"
                         " catch (e) { return String(e.message); } })()");
    ASSERT_TRUE(msg != NULL);
    EXPECT_TRUE_MSG(strstr(msg, "output_too_small") != NULL, msg);
    free(msg);
    js.max_heap_bytes = heap;
    EXPECT_EQ(eval_int("CI.call(CB).byteLength"), 512 << 10);
    EXPECT_EQ(eval_int("(CI.close(), globalThis.CB = globalThis.CI ="
                       " undefined, 1)"), 1);
    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}
#endif /* HL_ENABLE_WASM */

#ifdef HL_ENABLE_IMAGE
/* image.fromWasm took an ArrayBuffer or converted anything else with
 * toString: a Uint8Array view of compute output was parsed as the text
 * "2,0,0,...". It takes any byte view now (the unified buffer protocol). */
UTEST(js_audit27, image_from_wasm_takes_a_typed_array)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    if (eval_int("typeof image === 'object' && image !== null ? 1 : 0") != 1) {
        cleanup_js();   /* image compiled out */
        return;
    }
    EXPECT_EQ(eval_int(
        "(() => { const b = new Uint8Array(9 + 6);"
        " b[0] = 3; b[4] = 2; b[8] = 2;"
        " const img = image.fromWasm(b);"
        " const ok = img.width === 3 && img.height === 2;"
        " img.close(); return ok ? 1 : 0; })()"), 1);
    /* A view into a larger buffer reads its own window. */
    EXPECT_EQ(eval_int(
        "(() => { const ab = new ArrayBuffer(4 + 9 + 1);"
        " const b = new Uint8Array(ab, 4, 10);"
        " b[0] = 1; b[4] = 1; b[8] = 2;"
        " const img = image.fromWasm(b);"
        " const ok = img.width === 1 && img.height === 1;"
        " img.close(); return ok ? 1 : 0; })()"), 1);
    cleanup_js();
}
#endif /* HL_ENABLE_IMAGE */

/* ── Audit 29 ──────────────────────────────────────────────────────── */

/* H1: db.udf.register / unregister took a db.open (caller-owned) `this` -
 * udf is meant to be absent on a dynamic handle - and resolved the handle
 * before the name's toString and the opts' getters ran: a getter that
 * closed it left the call using a freed handle. A db.open `this` is
 * refused now, and the handle is resolved again after the last app code. */
UTEST(js_audit29, udf_refuses_a_db_open_this)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    static HlManifest m;
    memset(&m, 0, sizeof m);
    m.databases.declared = 1;
    m.databases.dynamic.declared = 1;
    m.databases.dynamic.schemes[0] = "sqlite";
    m.databases.dynamic.scheme_count = 1;
    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    ASSERT_TRUE(reg != NULL);
    hl_db_registry_seed(reg, "default", &test_db_handle);
    js.base.db_registry = reg;
    ASSERT_EQ(a4_module(
        "import { db as dbMod } from 'hull:db';\n"
        "const out = [];\n"
        "const d = dbMod.open(':memory:');\n"
        "out.push(typeof d.udf);\n"
        "const c = dbMod.default();\n"
        "const reg = c.udf.register, unreg = c.udf.unregister;\n"
        "const tryit = (f) => { try { f(); return 'ran'; }\n"
        "  catch (e) { return e instanceof TypeError ? 'type' : 'other'; } };\n"
        "out.push(tryit(() => reg.call(d, 'hull_a29', () => 1)));\n"
        "out.push(tryit(() => reg.call(d, 'hull_a29', () => 1,\n"
        "  { get nargs() { d.close(); return 0; } })));\n"
        "out.push(tryit(() => unreg.call(d, 'hull_a29')));\n"
        "out.push(tryit(() => reg.call(d, 'hull_a29', 'echo')));\n"
        "d.close();\n"
        "out.push(tryit(() => reg.call(d, 'hull_a29', () => 1)));\n"
        "c.udf.register('hull_a29ok', () => 7);\n"
        "out.push(String(c.query('SELECT hull_a29ok() AS v')[0].v));\n"
        "c.udf.unregister('hull_a29ok');\n"
        "globalThis.__a29udf = out.join('|');\n"), 0);
    char *s = eval_str("globalThis.__a29udf");
    ASSERT_NE(s, NULL);
    EXPECT_STREQ(s, "undefined|type|type|type|type|type|7");
    free(s);
    EXPECT_FALSE(JS_HasException(js.ctx));
    js.base.db_registry = test_db_registry;
    hl_db_registry_destroy(reg);
    cleanup_js_caps();
}

#ifdef HL_ENABLE_HTTP_SERVER
/* L1: opts.multipart was stored live and its caps read through the app's
 * getters at wiring (a throw -> 0 = unlimited, exception pending). They are
 * read once at registration now, validated, and stored as plain numbers. */
UTEST(js_audit29, multipart_caps_read_at_registration)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a4_module(
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "const out = [];\n"
        "const h = (req, res) => res.text('x');\n"
        "const tryit = (f) => { try { f(); return 'ok'; }\n"
        "  catch (e) { return e instanceof TypeError ? 'type' : String(e.message); } };\n"
        "out.push(tryit(() => app.post('/a', h, { multipart:\n"
        "  { get maxPartSize() { throw new Error('getter'); } } })));\n"
        "out.push(tryit(() => app.post('/b', h, { multipart: { maxPartSize: '9' } })));\n"
        "out.push(tryit(() => app.post('/c', h, { multipart: { maxParts: -1 } })));\n"
        "globalThis.__a29gets = 0;\n"
        "out.push(tryit(() => app.post('/d', h, { multipart: {\n"
        "  get maxPartSize() { globalThis.__a29gets++; return 1024; },\n"
        "  max_parts: 3 } })));\n"
        "globalThis.__a29mp = out.join('|');\n"), 0);
    char *s = eval_str("globalThis.__a29mp");
    ASSERT_NE(s, NULL);
    EXPECT_STREQ(s, "getter|type|type|ok");
    free(s);
    EXPECT_EQ(eval_int("globalThis.__a29gets"), 1);
    /* The def holds a plain null-prototype object of numbers. */
    EXPECT_EQ(eval_int(
        "(() => { const d = globalThis.__hull_internals.__hull_route_defs;\n"
        "  const def = Object.getOwnPropertyDescriptor(d, '0').value;\n"
        "  const mp = def.multipart;\n"
        "  return Object.getPrototypeOf(mp) === null && mp.max_part_size === 1024\n"
        "    && mp.max_parts === 3 && !('maxPartSize' in mp) ? 1 : 0; })()"), 1);
    EXPECT_EQ(eval_int("globalThis.__a29gets"), 1);
    EXPECT_FALSE(JS_HasException(js.ctx));
    cleanup_js();
}

/* L2: app.use / app.usePost converted the pattern after a method conversion
 * that threw, and threw a TypeError over the app's own error. */
UTEST(js_audit29, app_use_keeps_the_conversion_error)
{
    init_js();
    ASSERT_TRUE(js_initialized);
    ASSERT_EQ(a4_module(
        "import { app } from 'hull:app';\n"
        "app.manifest({ modules: ['hull/http-server@1'] });\n"
        "const out = [];\n"
        "globalThis.__a29pat = 0;\n"
        "const bad = { toString() { throw new Error('own'); } };\n"
        "const pat = { toString() { globalThis.__a29pat++; return '/*'; } };\n"
        "for (const f of [app.use, app.usePost]) {\n"
        "  try { f('*', bad, () => 0); out.push('ran'); } catch (e) { out.push(e.message); }\n"
        "  try { f(bad, pat, () => 0); out.push('ran'); } catch (e) { out.push(e.message); }\n"
        "}\n"
        "globalThis.__a29use = out.join('|');\n"), 0);
    char *s = eval_str("globalThis.__a29use");
    ASSERT_NE(s, NULL);
    EXPECT_STREQ(s, "own|own|own|own");
    free(s);
    EXPECT_EQ(eval_int("globalThis.__a29pat"), 0);   /* never converted */
    cleanup_js();
}

/* M2: ws.broadcast took its data through JS_ToCStringLen, so relaying an
 * ArrayBuffer sent "[object ArrayBuffer]". Byte views are sent as bytes, a
 * primitive as a string, and any other object is refused. */
UTEST(js_audit29, ws_broadcast_takes_byte_views)
{
    init_js_with_caps();
    ASSERT_TRUE(js_initialized);
    HlAllocator wa;
    hl_alloc_init(&wa, 0);
    HlWsRegistry reg;
    hl_ws_registry_init(&reg, &wa);
    js.base.ws_registry = &reg;
    ASSERT_EQ(a4_module(
        "import { wsServer } from 'hull:web:ws-server';\n"
        "const out = [];\n"
        "const tryit = (f) => { try { return String(f()); }\n"
        "  catch (e) { return e instanceof TypeError ? 'type' : 'other'; } };\n"
        "out.push(tryit(() => wsServer.broadcast('/ws', new ArrayBuffer(4), true)));\n"
        "out.push(tryit(() => wsServer.broadcast('/ws', new Uint8Array(4), true)));\n"
        "out.push(tryit(() => wsServer.broadcast('/ws', 'hi')));\n"
        "out.push(tryit(() => wsServer.broadcast('/ws', 42)));\n"
        "out.push(tryit(() => wsServer.broadcast('/ws', { toString() { return 'x'; } })));\n"
        "globalThis.__a29ws = out.join('|');\n"), 0);
    char *s = eval_str("globalThis.__a29ws");
    ASSERT_NE(s, NULL);
    EXPECT_STREQ(s, "0|0|0|0|type");
    free(s);
    js.base.ws_registry = NULL;
    hl_ws_registry_free(&reg);
    cleanup_js_caps();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_WASM
/* M1: compute inputs took an ArrayBuffer, a WasmBuffer or a MappedBuffer and
 * converted anything else with toString: a Uint8Array went in as "1,2,3".
 * A typed array is bytes now (its own window), and any other object is
 * refused. */
UTEST(js_audit29, compute_inputs_take_typed_arrays)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a9_wasm_entries, NULL);
    a9_init_js_wasm(&cache, &vfs);
    ASSERT_TRUE(js_initialized);
    EXPECT_EQ(eval_int("compute.call('echo', new Uint8Array([65, 66, 67])).byteLength"), 3);
    EXPECT_EQ(eval_int(
        "(() => { const ab = new ArrayBuffer(8);\n"
        "  new Uint8Array(ab).set([1, 2, 3, 4, 5, 6, 7, 8]);\n"
        "  const r = new Uint8Array(compute.call('echo', new Uint8Array(ab, 2, 3)));\n"
        "  return r.length === 3 && r[0] === 3 && r[2] === 5 ? 1 : 0; })()"), 1);
    EXPECT_EQ(eval_int("compute.call('echo', new Float32Array([1, 2])).byteLength"), 8);
    EXPECT_EQ(eval_int("(() => { const b = compute.buffer(new Uint16Array([1, 2, 3]));"
                       " const n = b.length; b.close(); return n; })()"), 6);
    EXPECT_EQ(eval_int(
        "(() => { const i = compute.instance('echo');\n"
        "  const n = i.call(new Int8Array(5)).byteLength; i.close(); return n; })()"), 5);
    static const char *const refused[] = {
        "compute.call('echo', { toString() { return 'abc'; } })",
        "compute.call('echo', [1, 2, 3])",
        "compute.buffer({})",
        "(() => { const i = compute.instance('echo');"
        " try { return i.call({}); } finally { i.close(); } })()",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        char code[512];
        snprintf(code, sizeof code,
                 "(() => { try { %s; return 0; }"
                 " catch (e) { return e instanceof TypeError ? 1 : 2; } })()",
                 refused[i]);
        EXPECT_EQ_MSG(eval_int(code), 1, refused[i]);
    }
    EXPECT_EQ(eval_int("compute.call('echo', 'abc').byteLength"), 3);
    js.base.wasm_cache = NULL;
    js.base.app_vfs = NULL;
    cleanup_js();
    hl_cap_wasm_destroy(&cache);
}
#endif /* HL_ENABLE_WASM */

/* ── gpu.* over an in-memory backend ─────────────────────────────────────
 *
 * The bindings are in the base build; only the wgpu backend is a feature.
 * tests/hull/gpu_mock.h stands in for it (the Lua twin is test_lua.c's
 * lua_gpu suite), so these run on every runner. */

#include "../../gpu_mock.h"

static HlGpuCtx *jgpu;

/* hull:gpu registers only when a GPU context exists at init. */
static int jgpu_init(int devices)
{
    if (js_initialized)
        hl_js_free(&js);
    js_initialized = 0;
    jgpu = gpu_mock_ctx(devices);
    if (!jgpu) return -1;
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    js.base.platform_vfs = &platform_vfs;
    js.base.gpu_ctx = jgpu;
    js_initialized = (hl_js_init(&js, &cfg) == 0);
    if (!js_initialized) return -1;
    install_test_js_globals(&js);
    return 0;
}

static void jgpu_cleanup(void)
{
    js.base.gpu_ctx = NULL;
    cleanup_js();
    gpu_mock_free(jgpu);
    jgpu = NULL;
}

/* `body` is a function body returning a string; a throw comes back as
 * "throw: <message>". */
#define JGPU_EXPECT(body, want) do {                                       \
        char *got_ = eval_str("(() => { const s = (b) => String.fromCharCode"\
            "(...new Uint8Array(b)); const u = (t) => Uint8Array.from(t, "   \
            "(c) => c.charCodeAt(0)).buffer; try {" body "} catch (e) {"     \
            " return 'throw: ' + e.message; } })()");                        \
        EXPECT_STREQ(got_, want);                                          \
        free(got_);                                                        \
    } while (0)

static int jgpu_has(int device, int texture, const char *name)
{
    HlGpuDevice *d = &jgpu->devices[device];
    int n = texture ? d->texture_count : d->buffer_count;
    for (int i = 0; i < n; i++)
        if (strcmp(texture ? d->textures[i].name : d->buffers[i].name, name) == 0)
            return 1;
    return 0;
}

UTEST(js_gpu, devices_are_listed)
{
    ASSERT_EQ(jgpu_init(2), 0);
    JGPU_EXPECT("const d = gpu.devices();"
                "return gpu.available() + ' ' + d.length + ' ' +"
                "  d[0].id + d[0].name + ' ' + d[1].id + d[1].name;",
                "true 2 0mock0 1mock1");
    jgpu_cleanup();
}

UTEST(js_gpu, buffers_are_per_device)
{
    ASSERT_EQ(jgpu_init(2), 0);
    JGPU_EXPECT("gpu.buffer('b', u('AAAAAAAA'));"
                "gpu.buffer('b', u('BBBBBBBB'), { device: 1 });"
                "return s(gpu.bufferRead('b')) + s(gpu.bufferRead('b', { device: 1 }));",
                "AAAAAAAABBBBBBBB");
    /* Whole 4-byte words at a 4-aligned offset, as WebGPU writes. */
    JGPU_EXPECT("gpu.buffer('b', u('wxyz'), { device: 1, offset: 4 });"
                "let r = s(gpu.bufferRead('b', { device: 1 }));"
                "try { gpu.buffer('b', u('q'), { device: 1, offset: 1 }); }"
                "catch (e) { r += ' ' + e.message; }"
                "return r;",
                "BBBBwxyz gpu.buffer failed");
    EXPECT_EQ(jgpu->devices[0].live_bytes, (size_t)8);
    EXPECT_EQ(jgpu->devices[1].live_bytes, (size_t)8);
    jgpu_cleanup();
}

/* gpu.texture(name, null, { device: N }) and gpu.buffer(name, null,
 * { device: N }) destroyed the object of that name on the DEFAULT device:
 * the destroy branch ran before opts.device was read (#870). */
UTEST(js_gpu, a_destroy_targets_the_device_it_names)
{
    ASSERT_EQ(jgpu_init(2), 0);
    JGPU_EXPECT("const px = new Uint8Array(16).fill(1).buffer;"
                "for (const d of [0, 1]) {"
                "  gpu.buffer('b', u('data'), { device: d });"
                "  gpu.texture('t', px, { width: 2, height: 2, device: d });"
                "}"
                "gpu.buffer('b', null, { device: 1 });"
                "gpu.texture('t', null, { device: 1 });"
                "gpu.texture('t', null, { device: 7 });"   /* no such device */
                "return 'ok';",
                "ok");
    EXPECT_TRUE(jgpu_has(0, 0, "b"));
    EXPECT_FALSE(jgpu_has(1, 0, "b"));
    EXPECT_TRUE(jgpu_has(0, 1, "t"));
    EXPECT_FALSE(jgpu_has(1, 1, "t"));
    EXPECT_EQ(jgpu->devices[1].live_bytes, (size_t)0);
    EXPECT_EQ(jgpu->devices[0].live_bytes, (size_t)(4 + 16));
    /* A throwing device getter fails the destroy instead of picking one. */
    JGPU_EXPECT("gpu.texture('t', null, { get device() { throw new Error('dev'); } });"
                "return 'no throw';",
                "throw: dev");
    EXPECT_TRUE(jgpu_has(0, 1, "t"));
    JGPU_EXPECT("gpu.buffer('b', null); gpu.texture('t', null); return 'ok';", "ok");
    EXPECT_EQ(jgpu->devices[0].live_bytes, (size_t)0);
    jgpu_cleanup();
}

UTEST(js_gpu, an_unknown_device_is_refused)
{
    ASSERT_EQ(jgpu_init(1), 0);
    JGPU_EXPECT("gpu.buffer('b', u('x'), { device: 3 }); return 'made';",
                "throw: gpu.buffer failed");
    JGPU_EXPECT("gpu.bufferRead('b', { device: 3 }); return 'read';",
                "throw: gpu.bufferRead failed");
    EXPECT_EQ(gpu_mock_buffer_creates, 0);
    jgpu_cleanup();
}

UTEST(js_gpu, dispatch_reads_back_the_buffer_it_names)
{
    ASSERT_EQ(jgpu_init(1), 0);
    JGPU_EXPECT("gpu.compile('id', 'fn main() {}');"
                "const a = gpu.dispatch('id', { buffers: ["
                "  { data: u('hello') }, { size: 3 }] });"          /* default: 0 */
                "const b = gpu.dispatch('id', { buffers: ["
                "  { data: u('hello') }, { size: 3 }], output: 1 });"
                "const c = gpu.dispatch('id', { buffers: ["
                "  { data: u('hello') }], output: false });"
                "return s(a) + ' ' + b.byteLength + ' ' +"
                "  (s(b) === '\\0\\0\\0' ? 'z' : '?') + ' ' + c;",
                "hello 3 z true");
    int before = gpu_mock_dispatches;
    JGPU_EXPECT("gpu.dispatch('nope', { buffers: [{ data: u('x') }] }); return 'ran';",
                "throw: gpu.dispatch: shader_not_found");
    EXPECT_EQ(gpu_mock_dispatches, before);
    jgpu_cleanup();
}

/* compile, bufferRead and textureRead took no device option, unlike their
 * Lua twins: a shader compiled only on the default device (a dispatch on
 * another was shader_not_found), and reads came from the default device. */
UTEST(js_gpu, compile_and_reads_take_the_device)
{
    ASSERT_EQ(jgpu_init(2), 0);
    JGPU_EXPECT("gpu.compile('id1', 'fn main() {}', { device: 1 });"
                "let r = s(gpu.dispatch('id1', { device: 1, buffers: [{ data: u('one') }] }));"
                "try { gpu.dispatch('id1', { buffers: [{ data: u('x') }] }); }"
                "catch (e) { r += ' ' + e.message; }"
                "gpu.buffer('b', u('zero'));"
                "gpu.buffer('b', u('one!'), { device: 1 });"
                "r += ' ' + s(gpu.bufferRead('b', { device: 1 }));"
                "try { gpu.bufferRead('b', { get device() { throw new Error('dev'); } }); }"
                "catch (e) { r += ' ' + e.message; }"
                "return r;",
                "one gpu.dispatch: shader_not_found one! dev");
#ifdef HL_ENABLE_IMAGE
    JGPU_EXPECT("gpu.texture('t', new ArrayBuffer(16), { width: 2, height: 2 });"
                "gpu.texture('t', new ArrayBuffer(16), { width: 4, height: 1, device: 1 });"
                "return gpu.textureRead('t').width + ' ' +"
                "  gpu.textureRead('t', { device: 1 }).width;",
                "2 4");
#endif
    jgpu_cleanup();
}

/* The twin of lua_gpu.load_takes_the_device. */
static const char jgpu_wgsl[] = "fn main() {}";
static const HlEntry jgpu_shader_entries[] = {
    { "shaders/id.wgsl", (const unsigned char *)jgpu_wgsl, sizeof jgpu_wgsl - 1 },
    { 0, 0, 0 }
};

UTEST(js_gpu, load_takes_the_device)
{
    ASSERT_EQ(jgpu_init(2), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, jgpu_shader_entries, NULL);
    js.base.app_vfs = &vfs;
    JGPU_EXPECT("gpu.load('id', { device: 1 });"
                "let r = s(gpu.dispatch('id', { device: 1, buffers: [{ data: u('one') }] }));"
                "try { gpu.dispatch('id', { buffers: [{ data: u('x') }] }); }"
                "catch (e) { r += ' ' + e.message; }"
                "try { gpu.load('id', { get device() { throw new Error('dev'); } }); }"
                "catch (e) { r += ' ' + e.message; }"
                "return r;",
                "one gpu.dispatch: shader_not_found dev");
    js.base.app_vfs = NULL;
    jgpu_cleanup();
}

/* A pipeline with several outputs returns an array of them (audit 29; it
 * returned only the first), one output a single ArrayBuffer. */
UTEST(js_gpu, a_pipeline_with_several_outputs_returns_an_array)
{
    ASSERT_EQ(jgpu_init(1), 0);
    JGPU_EXPECT("gpu.compile('id', 'fn main() {}');"
                "const stages = ["
                "  { shader: 'id', buffers: [{ name: 'a', data: u('xy') }] },"
                "  { shader: 'id', buffers: [{ name: 'a' }, { data: u('zz') }] }];"
                "const one = gpu.pipeline(stages);"
                "const two = gpu.pipeline(stages, { outputs: ["
                "  { stage: 0, buffer: 0 }, { stage: 1, buffer: 1 }] });"
                "return (one instanceof ArrayBuffer) + ':' + s(one) + ' ' +"
                "  Array.isArray(two) + ':' + two.length + ':' + s(two[0]) + s(two[1]);",
                "true:xy true:2:xyzz");
    jgpu_cleanup();
}

/* A read-back larger than the VM heap limit is refused before the backend
 * copies it off the heap (audit 17), in bufferRead and in dispatch. */
UTEST(js_gpu, read_backs_past_the_heap_limit_are_refused)
{
    ASSERT_EQ(jgpu_init(1), 0);
    JGPU_EXPECT("gpu.buffer('big', new ArrayBuffer(2 << 20));"
                "gpu.compile('id', 'fn main() {}'); return 'ok';", "ok");
    size_t saved = js.max_heap_bytes;
    js.max_heap_bytes = 1u << 20;
    JGPU_EXPECT("let r = '';"
                "try { gpu.bufferRead('big'); r += 'read'; } catch (e) { r += e.message; }"
                "try { gpu.dispatch('id', { buffers: [{ name: 'big' }] }); r += ' ran'; }"
                "catch (e) { r += ' | ' + e.message; }"
                "return r;",
                "gpu.bufferRead failed | gpu.dispatch: output_too_large");
    js.max_heap_bytes = saved;
    JGPU_EXPECT("return String(gpu.bufferRead('big').byteLength);", "2097152");
    jgpu_cleanup();
}

UTEST_MAIN();
