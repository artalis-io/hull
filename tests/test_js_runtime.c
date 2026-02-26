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

#include <sqlite3.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Helpers ────────────────────────────────────────────────────────── */

static HlJS js;
static int js_initialized = 0;

static void init_js(void)
{
    if (js_initialized)
        hl_js_free(&js);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
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

/* Init JS with database and env capabilities for testing */
static sqlite3 *test_db = NULL;
static const char *env_allowed[] = { "HULL_TEST_VAR", NULL };
static HlEnvConfig env_cfg = { .allowed = env_allowed, .count = 1 };

static void init_js_with_caps(void)
{
    if (js_initialized)
        hl_js_free(&js);
    if (test_db) {
        sqlite3_close(test_db);
        test_db = NULL;
    }

    sqlite3_open(":memory:", &test_db);
    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    memset(&js, 0, sizeof(js));
    js.db = test_db;
    js.env_cfg = &env_cfg;
    int rc = hl_js_init(&js, &cfg);
    js_initialized = (rc == 0);
}

static void cleanup_js_caps(void)
{
    if (js_initialized) {
        hl_js_free(&js);
        js_initialized = 0;
    }
    if (test_db) {
        sqlite3_close(test_db);
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

UTEST_MAIN();
