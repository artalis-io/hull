/*
 * test_lua_runtime.c — Tests for Lua 5.4 runtime integration
 *
 * Tests: VM init, sandbox, module loading, route registration,
 * memory limits, GC.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/runtime/lua.h"
#include "hull/cap/db.h"
#include "hull/cap/env.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>

#include "hull/limits.h"

#include <sqlite3.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Helpers ────────────────────────────────────────────────────────── */

static HlLua lua_rt;
static int lua_initialized = 0;

static void init_lua(void)
{
    if (lua_initialized)
        hl_lua_free(&lua_rt);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    memset(&lua_rt, 0, sizeof(lua_rt));
    int rc = hl_lua_init(&lua_rt, &cfg);
    lua_initialized = (rc == 0);
}

static void cleanup_lua(void)
{
    if (lua_initialized) {
        hl_lua_free(&lua_rt);
        lua_initialized = 0;
    }
}

/* Init lua with database and env capabilities for testing */
static sqlite3 *test_db = NULL;
static HlStmtCache test_stmt_cache;
static const char *env_allowed[] = { "HULL_TEST_VAR", NULL };
static HlEnvConfig env_cfg = { .allowed = env_allowed, .count = 1 };

static void init_lua_with_caps(void)
{
    if (lua_initialized)
        hl_lua_free(&lua_rt);
    if (test_db) {
        hl_stmt_cache_destroy(&test_stmt_cache);
        sqlite3_close(test_db);
        test_db = NULL;
    }

    sqlite3_open(":memory:", &test_db);
    hl_cap_db_init(test_db);
    hl_stmt_cache_init(&test_stmt_cache, test_db);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    memset(&lua_rt, 0, sizeof(lua_rt));
    lua_rt.base.db = test_db;
    lua_rt.base.stmt_cache = &test_stmt_cache;
    lua_rt.base.env_cfg = &env_cfg;
    int rc = hl_lua_init(&lua_rt, &cfg);
    lua_initialized = (rc == 0);
}

static void cleanup_lua_caps(void)
{
    if (lua_initialized) {
        hl_lua_free(&lua_rt);
        lua_initialized = 0;
    }
    if (test_db) {
        hl_stmt_cache_destroy(&test_stmt_cache);
        sqlite3_close(test_db);
        test_db = NULL;
    }
}

/* Evaluate a Lua expression and return the result as a string.
 * Caller must free the returned string. Returns NULL on error. */
static char *eval_str(const char *code)
{
    if (!lua_initialized || !lua_rt.L)
        return NULL;

    /* Wrap in return statement for expression evaluation */
    char buf[4096];
    snprintf(buf, sizeof(buf), "return tostring(%s)", code);

    if (luaL_dostring(lua_rt.L, buf) != LUA_OK) {
        const char *err = lua_tostring(lua_rt.L, -1);
        fprintf(stderr, "eval_str error: %s\n", err ? err : "(nil)");
        lua_pop(lua_rt.L, 1);
        return NULL;
    }

    const char *s = lua_tostring(lua_rt.L, -1);
    char *result = s ? strdup(s) : NULL;
    lua_pop(lua_rt.L, 1);
    return result;
}

/* Evaluate Lua and return integer result. Returns -9999 on error. */
static int eval_int(const char *code)
{
    if (!lua_initialized || !lua_rt.L)
        return -9999;

    char buf[4096];
    snprintf(buf, sizeof(buf), "return %s", code);

    if (luaL_dostring(lua_rt.L, buf) != LUA_OK) {
        const char *err = lua_tostring(lua_rt.L, -1);
        fprintf(stderr, "eval_int error: %s\n", err ? err : "(nil)");
        lua_pop(lua_rt.L, 1);
        return -9999;
    }

    int result = (int)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 1);
    return result;
}

/* ── Basic runtime tests ────────────────────────────────────────────── */

UTEST(lua_runtime, init_and_free)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua local_lua;
    memset(&local_lua, 0, sizeof(local_lua));

    int rc = hl_lua_init(&local_lua, &cfg);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(local_lua.L != NULL);

    hl_lua_free(&local_lua);
    ASSERT_TRUE(local_lua.L == NULL);
}

UTEST(lua_runtime, basic_eval)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int result = eval_int("1 + 2");
    ASSERT_EQ(result, 3);

    cleanup_lua();
}

UTEST(lua_runtime, string_eval)
{
    init_lua();

    char *s = eval_str("'hello' .. ' ' .. 'world'");
    ASSERT_NE(s, NULL);
    ASSERT_STREQ(s, "hello world");
    free(s);

    cleanup_lua();
}

UTEST(lua_runtime, table_works)
{
    init_lua();

    /* Tables work — basic serialization check */
    int result = eval_int("(function() local t = {a=1, b=2}; return t.a + t.b end)()");
    ASSERT_EQ(result, 3);

    cleanup_lua();
}

/* ── Sandbox tests ──────────────────────────────────────────────────── */

UTEST(lua_runtime, sandbox_no_io)
{
    init_lua();

    /* io should be nil (removed by sandbox) */
    int result = eval_int("io == nil and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

UTEST(lua_runtime, sandbox_no_os)
{
    init_lua();

    /* os should be nil (removed by sandbox) */
    int result = eval_int("os == nil and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

UTEST(lua_runtime, sandbox_no_loadfile)
{
    init_lua();

    /* loadfile should be nil (removed by sandbox) */
    int result = eval_int("loadfile == nil and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

UTEST(lua_runtime, sandbox_no_dofile)
{
    init_lua();

    /* dofile should be nil (removed by sandbox) */
    int result = eval_int("dofile == nil and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

UTEST(lua_runtime, sandbox_no_load)
{
    init_lua();

    /* load should be nil (removed by sandbox) */
    int result = eval_int("load == nil and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

/* ── Module tests ───────────────────────────────────────────────────── */

UTEST(lua_runtime, hull_time_module)
{
    init_lua();

    /* time.now() should return a number */
    int result = eval_int("type(time.now()) == 'number' and 1 or 0");
    ASSERT_EQ(result, 1);

    /* Should be a reasonable Unix timestamp (> 2024-01-01) */
    int recent = eval_int("time.now() > 1704067200 and 1 or 0");
    ASSERT_EQ(recent, 1);

    /* time.date() should return a string like YYYY-MM-DD */
    char *date = eval_str("time.date()");
    ASSERT_NE(date, NULL);
    ASSERT_EQ(strlen(date), (size_t)10); /* YYYY-MM-DD */
    free(date);

    /* time.datetime() should return ISO 8601 */
    char *dt = eval_str("time.datetime()");
    ASSERT_NE(dt, NULL);
    ASSERT_EQ(strlen(dt), (size_t)20); /* YYYY-MM-DDTHH:MM:SSZ */
    free(dt);

    cleanup_lua();
}

UTEST(lua_runtime, hull_app_module)
{
    init_lua();

    /* Register routes via app.get/app.post */
    int rc = luaL_dostring(lua_rt.L,
        "app.get('/test', function(req, res) res:json({ok=true}) end)\n"
        "app.post('/data', function(req, res) res:text('received') end)\n");
    ASSERT_EQ(rc, LUA_OK);

    /* Verify routes were registered in the registry */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_route_defs");
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));
    int count = (int)luaL_len(lua_rt.L, -1);
    ASSERT_EQ(count, 2);

    /* Verify first route */
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "method");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "GET");
    lua_pop(lua_rt.L, 1);

    lua_getfield(lua_rt.L, -1, "pattern");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "/test");
    lua_pop(lua_rt.L, 1);

    lua_pop(lua_rt.L, 1); /* route def */

    /* Verify handler functions stored */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_routes");
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));
    lua_rawgeti(lua_rt.L, -1, 1);
    ASSERT_TRUE(lua_isfunction(lua_rt.L, -1));
    lua_pop(lua_rt.L, 1); /* handler */
    lua_pop(lua_rt.L, 1); /* routes table */

    lua_pop(lua_rt.L, 1); /* defs table */

    cleanup_lua();
}

/* ── GC test ────────────────────────────────────────────────────────── */

UTEST(lua_runtime, gc_runs)
{
    init_lua();

    /* Create a bunch of tables, then GC */
    luaL_dostring(lua_rt.L,
        "for i = 1, 10000 do local x = {a=i, b='test'} end");

    /* GC should not crash */
    lua_gc(lua_rt.L, LUA_GCCOLLECT);

    /* Still functional after GC */
    int result = eval_int("2 + 2");
    ASSERT_EQ(result, 4);

    cleanup_lua();
}

/* ── Print exists test ──────────────────────────────────────────────── */

UTEST(lua_runtime, print_exists)
{
    init_lua();

    int result = eval_int("type(print) == 'function' and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

/* ── Safe libs available test ──────────────────────────────────────── */

UTEST(lua_runtime, safe_libs_available)
{
    init_lua();

    /* table, string, math should be available */
    int result = eval_int(
        "type(table) == 'table' and "
        "type(string) == 'table' and "
        "type(math) == 'table' and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

/* ── Double free safety ─────────────────────────────────────────────── */

UTEST(lua_runtime, double_free)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua local_lua;
    memset(&local_lua, 0, sizeof(local_lua));

    hl_lua_init(&local_lua, &cfg);
    hl_lua_free(&local_lua);
    hl_lua_free(&local_lua); /* should not crash */
}

/* ── Module loader tests ─────────────────────────────────────────────── */

UTEST(lua_runtime, require_hull_json)
{
    init_lua();

    /* require('hull.json') should return a table with encode/decode */
    int result = eval_int(
        "(function() local j = require('hull.json') "
        "return type(j) == 'table' and type(j.encode) == 'function' "
        "and type(j.decode) == 'function' and 1 or 0 end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

UTEST(lua_runtime, require_caches_module)
{
    init_lua();

    /* require('hull.json') returns the same cached object on second call */
    int result = eval_int(
        "rawequal(require('hull.json'), require('hull.json')) and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

UTEST(lua_runtime, require_vendor_json)
{
    init_lua();

    /* require('vendor.json') should work (internal vendor namespace) */
    int result = eval_int(
        "(function() local j = require('vendor.json') "
        "return type(j) == 'table' and type(j.encode) == 'function' and 1 or 0 end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

UTEST(lua_runtime, require_nonexistent_errors)
{
    init_lua();

    /* require('nonexistent') should raise an error */
    int rc = luaL_dostring(lua_rt.L, "require('nonexistent')");
    ASSERT_NE(rc, LUA_OK);

    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "module not found"), NULL);
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
}

UTEST(lua_runtime, require_non_string_errors)
{
    init_lua();

    /* require with non-string argument should error */
    int rc = luaL_dostring(lua_rt.L, "require(42)");
    ASSERT_NE(rc, LUA_OK);
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
}

UTEST(lua_runtime, json_global_available)
{
    init_lua();

    /* json global should be available (pre-loaded) */
    int result = eval_int(
        "type(json) == 'table' and type(json.encode) == 'function' "
        "and type(json.decode) == 'function' and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

UTEST(lua_runtime, json_encode_decode)
{
    init_lua();

    /* json.encode and json.decode should work */
    char *s = eval_str("json.encode({name='hull'})");
    ASSERT_NE(s, NULL);
    ASSERT_NE(strstr(s, "\"name\""), NULL);
    ASSERT_NE(strstr(s, "\"hull\""), NULL);
    free(s);

    int result = eval_int(
        "json.decode('{\"x\":42}').x");
    ASSERT_EQ(result, 42);

    cleanup_lua();
}

UTEST(lua_runtime, json_roundtrip)
{
    init_lua();

    int result = eval_int(
        "(function() local t = {a=1, b='two'} "
        "local s = json.encode(t) "
        "local t2 = json.decode(s) "
        "return t2.a == 1 and t2.b == 'two' and 1 or 0 end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

/* ── Error reporting ────────────────────────────────────────────────── */

UTEST(lua_runtime, error_reporting)
{
    init_lua();

    /* Trigger an error — should not crash */
    int rc = luaL_dostring(lua_rt.L, "error('test error')");
    ASSERT_NE(rc, LUA_OK);

    /* Error message should be on stack */
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    /* The error message should contain 'test error' */
    ASSERT_NE(strstr(err, "test error"), NULL);
    lua_pop(lua_rt.L, 1);

    /* VM should still be functional */
    int result = eval_int("3 + 4");
    ASSERT_EQ(result, 7);

    cleanup_lua();
}

/* ── Filesystem require helpers ──────────────────────────────────────── */

/* Write a string to a file */
static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

/* Recursively remove a directory (simple: 2-level max) */
static void rm_rf(const char *dir)
{
    char path[1024];
    /* Try to remove known test files and subdirs */
    const char *names[] = {
        "mod.lua", "bad.lua", "big.lua", "nilmod.lua",
        "sub/b.lua", "sub", "c.lua", "sibling.lua",
        NULL
    };
    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        unlink(path);
        rmdir(path);
    }
    rmdir(dir);
}

/* Init lua with app_dir set to a temp directory */
static void init_lua_with_appdir(const char *app_dir)
{
    if (lua_initialized)
        hl_lua_free(&lua_rt);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    memset(&lua_rt, 0, sizeof(lua_rt));
    int rc = hl_lua_init(&lua_rt, &cfg);
    lua_initialized = (rc == 0);
    if (lua_initialized && app_dir) {
        lua_rt.app_dir = strdup(app_dir);
        /* Set __hull_current_module to a dummy entry point in app_dir */
        char entry[1024];
        snprintf(entry, sizeof(entry), "%s/app.lua", app_dir);
        lua_pushstring(lua_rt.L, entry);
        lua_setfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_current_module");
    }
}

/* ── Filesystem require tests ───────────────────────────────────────── */

UTEST(lua_require_fs, basic)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    char path[1024];
    snprintf(path, sizeof(path), "%s/mod.lua", tmpdir);
    write_file(path, "return { answer = 42 }\n");

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    int result = eval_int(
        "(function() local m = require('./mod') "
        "return m.answer end)()");
    ASSERT_EQ(result, 42);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, lua_ext_auto)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    char path[1024];
    snprintf(path, sizeof(path), "%s/mod.lua", tmpdir);
    write_file(path, "return { val = 7 }\n");

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* require('./mod') should auto-append .lua */
    int result = eval_int(
        "(function() local m = require('./mod') return m.val end)()");
    ASSERT_EQ(result, 7);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, nested_relative)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    /* Create sub directory */
    char subdir[1024];
    snprintf(subdir, sizeof(subdir), "%s/sub", tmpdir);
    mkdir(subdir, 0755);

    /* sub/b.lua requires ../c (caller-relative traversal within app_dir) */
    char bpath[1024];
    snprintf(bpath, sizeof(bpath), "%s/sub/b.lua", tmpdir);
    write_file(bpath, "local c = require('../c')\nreturn { from_c = c.val }\n");

    /* c.lua at app root */
    char cpath[1024];
    snprintf(cpath, sizeof(cpath), "%s/c.lua", tmpdir);
    write_file(cpath, "return { val = 99 }\n");

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* require('./sub/b') loads b.lua, which requires('../c') → c.lua */
    int result = eval_int(
        "(function() local b = require('./sub/b') return b.from_c end)()");
    ASSERT_EQ(result, 99);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, cached)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    char path[1024];
    snprintf(path, sizeof(path), "%s/mod.lua", tmpdir);
    write_file(path, "return { x = 1 }\n");

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* require('./mod') twice returns the same object (rawequal) */
    int result = eval_int(
        "rawequal(require('./mod'), require('./mod')) and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, traversal_above_root)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* require('../../etc/passwd') should error — escapes above app_dir */
    int rc = luaL_dostring(lua_rt.L, "require('../../etc/passwd')");
    ASSERT_NE(rc, LUA_OK);

    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "module not found"), NULL);
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, traversal_within_ok)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    /* Create sub directory and sibling file */
    char subdir[1024];
    snprintf(subdir, sizeof(subdir), "%s/sub", tmpdir);
    mkdir(subdir, 0755);

    char spath[1024];
    snprintf(spath, sizeof(spath), "%s/sibling.lua", tmpdir);
    write_file(spath, "return { ok = true }\n");

    /* Set current module to sub/a.lua so ../sibling resolves within app_dir */
    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* Override current module to be inside sub/ */
    char sub_entry[1024];
    snprintf(sub_entry, sizeof(sub_entry), "%s/sub/a.lua", tmpdir);
    lua_pushstring(lua_rt.L, sub_entry);
    lua_setfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_current_module");

    /* require('../sibling') from sub/a.lua → should resolve to sibling.lua */
    int result = eval_int(
        "(function() local s = require('../sibling') "
        "return s.ok and 1 or 0 end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, not_found)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* require('./nonexistent') should give clear error */
    int rc = luaL_dostring(lua_rt.L, "require('./nonexistent')");
    ASSERT_NE(rc, LUA_OK);

    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "module not found"), NULL);
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, no_appdir)
{
    /* Without app_dir set, filesystem fallback is skipped */
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_TRUE(lua_rt.app_dir == NULL);

    int rc = luaL_dostring(lua_rt.L, "require('./some_module')");
    ASSERT_NE(rc, LUA_OK);

    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "module not found"), NULL);
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
}

UTEST(lua_require_fs, syntax_error)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    char path[1024];
    snprintf(path, sizeof(path), "%s/bad.lua", tmpdir);
    write_file(path, "return {{{BROKEN SYNTAX\n");

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* require('./bad') should propagate Lua compile error */
    int rc = luaL_dostring(lua_rt.L, "require('./bad')");
    ASSERT_NE(rc, LUA_OK);

    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    /* Lua compile errors mention the file name */
    ASSERT_NE(strstr(err, "bad.lua"), NULL);
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, returns_nil)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    char path[1024];
    snprintf(path, sizeof(path), "%s/nilmod.lua", tmpdir);
    write_file(path, "-- returns nil implicitly\n");

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* Module that returns nil caches true sentinel */
    int result = eval_int(
        "(function() local m = require('./nilmod') "
        "return m == true and 1 or 0 end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, too_large)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    /* Create a file that exceeds HL_MODULE_MAX_SIZE */
    char path[1024];
    snprintf(path, sizeof(path), "%s/big.lua", tmpdir);
    FILE *f = fopen(path, "w");
    ASSERT_TRUE(f != NULL);
    /* Write just past the limit — use fseek to create a sparse file */
    fseek(f, HL_MODULE_MAX_SIZE + 1, SEEK_SET);
    fputc('x', f);
    fclose(f);

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    int rc = luaL_dostring(lua_rt.L, "require('./big')");
    ASSERT_NE(rc, LUA_OK);

    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "too large"), NULL);
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, embedded_still_first)
{
    char tmpdir[] = "/tmp/hull_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpdir), NULL);

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* Embedded hull.json is found before filesystem even when app_dir is set */
    int result = eval_int(
        "(function() local j = require('hull.json') "
        "return type(j) == 'table' and type(j.encode) == 'function' "
        "and 1 or 0 end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua();
    rm_rf(tmpdir);
}

/* ── Crypto tests ──────────────────────────────────────────────────── */

UTEST(lua_cap, crypto_sha256)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* SHA-256 of "hello" — known hash */
    char *hash = eval_str("crypto.sha256('hello')");
    ASSERT_NE(hash, NULL);
    ASSERT_STREQ(hash,
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    free(hash);

    cleanup_lua_caps();
}

UTEST(lua_cap, crypto_random)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* crypto.random(16) returns a 16-byte string */
    int len = eval_int("#crypto.random(16)");
    ASSERT_EQ(len, 16);

    /* Two calls should produce different values */
    int differ = eval_int(
        "crypto.random(16) ~= crypto.random(16) and 1 or 0");
    ASSERT_EQ(differ, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, crypto_hash_password)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    char *hash = eval_str("crypto.hash_password('secret123')");
    ASSERT_NE(hash, NULL);
    /* PBKDF2 format: starts with "pbkdf2:" */
    ASSERT_EQ(strncmp(hash, "pbkdf2:", 7), 0);
    free(hash);

    cleanup_lua_caps();
}

UTEST(lua_cap, crypto_verify_password)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Correct password verifies */
    int ok = eval_int(
        "(function() "
        "  local h = crypto.hash_password('mypass') "
        "  return crypto.verify_password('mypass', h) and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    /* Wrong password fails */
    int bad = eval_int(
        "(function() "
        "  local h = crypto.hash_password('mypass') "
        "  return crypto.verify_password('wrong', h) and 1 or 0 "
        "end)()");
    ASSERT_EQ(bad, 0);

    cleanup_lua_caps();
}

/* ── Log tests ─────────────────────────────────────────────────────── */

UTEST(lua_cap, log_functions_exist)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int result = eval_int(
        "type(log.info) == 'function' and "
        "type(log.warn) == 'function' and "
        "type(log.error) == 'function' and "
        "type(log.debug) == 'function' and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, log_does_not_error)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Calling all four log functions should not raise a Lua error */
    int rc = luaL_dostring(lua_rt.L,
        "log.info('test info')\n"
        "log.warn('test warn')\n"
        "log.error('test error')\n"
        "log.debug('test debug')\n");
    ASSERT_EQ(rc, LUA_OK);

    cleanup_lua_caps();
}

/* ── Env tests ─────────────────────────────────────────────────────── */

UTEST(lua_cap, env_get_allowed)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    setenv("HULL_TEST_VAR", "test_value_123", 1);
    char *val = eval_str("env.get('HULL_TEST_VAR')");
    ASSERT_NE(val, NULL);
    ASSERT_STREQ(val, "test_value_123");
    free(val);
    unsetenv("HULL_TEST_VAR");

    cleanup_lua_caps();
}

UTEST(lua_cap, env_get_blocked)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* PATH is not in the allowlist — should return nil */
    int result = eval_int("env.get('PATH') == nil and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, env_get_nonexistent)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* HULL_TEST_VAR is allowed but not set — should return nil */
    unsetenv("HULL_TEST_VAR");
    int result = eval_int("env.get('HULL_TEST_VAR') == nil and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

/* ── DB tests ──────────────────────────────────────────────────────── */

UTEST(lua_cap, db_exec_and_query)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int result = eval_int(
        "(function() "
        "  db.exec('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)') "
        "  db.exec('INSERT INTO t (name) VALUES (?)', {'alice'}) "
        "  local rows = db.query('SELECT name FROM t') "
        "  return rows[1].name == 'alice' and 1 or 0 "
        "end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, db_last_id)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int result = eval_int(
        "(function() "
        "  db.exec('CREATE TABLE t2 (id INTEGER PRIMARY KEY, v TEXT)') "
        "  db.exec('INSERT INTO t2 (v) VALUES (?)', {'a'}) "
        "  local id1 = db.last_id() "
        "  db.exec('INSERT INTO t2 (v) VALUES (?)', {'b'}) "
        "  local id2 = db.last_id() "
        "  return (id2 > id1) and 1 or 0 "
        "end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, db_parameterized_query)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int result = eval_int(
        "(function() "
        "  db.exec('CREATE TABLE t3 (id INTEGER PRIMARY KEY, val INTEGER)') "
        "  db.exec('INSERT INTO t3 (val) VALUES (?)', {10}) "
        "  db.exec('INSERT INTO t3 (val) VALUES (?)', {20}) "
        "  db.exec('INSERT INTO t3 (val) VALUES (?)', {30}) "
        "  local rows = db.query('SELECT val FROM t3 WHERE val > ?', {15}) "
        "  return #rows == 2 and 1 or 0 "
        "end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, db_not_available_without_config)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* Without db set, the db global should be nil */
    int result = eval_int("db == nil and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

/* ── Manifest tests ────────────────────────────────────────────────── */

#include "hull/manifest.h"

UTEST(lua_runtime, manifest_not_declared)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    HlManifest m;
    int rc = hl_manifest_extract(lua_rt.L, &m);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(m.present, 0);

    cleanup_lua();
}

UTEST(lua_runtime, manifest_basic)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* Declare a manifest via app.manifest() */
    const char *code =
        "app.manifest({\n"
        "  fs = { read = {'data/', 'config/'}, write = {'uploads/'} },\n"
        "  env = {'PORT', 'DATABASE_URL'},\n"
        "  hosts = {'api.stripe.com', 'api.sendgrid.com'},\n"
        "})\n";
    int rc = luaL_dostring(lua_rt.L, code);
    ASSERT_EQ(rc, LUA_OK);

    HlManifest m;
    rc = hl_manifest_extract(lua_rt.L, &m);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(m.present, 1);

    ASSERT_EQ(m.fs_read_count, 2);
    ASSERT_STREQ(m.fs_read[0], "data/");
    ASSERT_STREQ(m.fs_read[1], "config/");

    ASSERT_EQ(m.fs_write_count, 1);
    ASSERT_STREQ(m.fs_write[0], "uploads/");

    ASSERT_EQ(m.env_count, 2);
    ASSERT_STREQ(m.env[0], "PORT");
    ASSERT_STREQ(m.env[1], "DATABASE_URL");

    ASSERT_EQ(m.hosts_count, 2);
    ASSERT_STREQ(m.hosts[0], "api.stripe.com");
    ASSERT_STREQ(m.hosts[1], "api.sendgrid.com");

    cleanup_lua();
}

/* ── Middleware tests ────────────────────────────────────────────────── */

UTEST(lua_middleware, registration_stores_handler_id)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int rc = luaL_dostring(lua_rt.L,
        "app.use('*', '/*', function(req, res) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    /* Verify middleware entry has handler_id (not handler function) */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));
    int mw_count = (int)luaL_len(lua_rt.L, -1);
    ASSERT_EQ(mw_count, 1);

    lua_rawgeti(lua_rt.L, -1, 1);
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));

    lua_getfield(lua_rt.L, -1, "handler_id");
    ASSERT_TRUE(lua_isinteger(lua_rt.L, -1));
    int handler_id = (int)lua_tointeger(lua_rt.L, -1);
    ASSERT_TRUE(handler_id > 0);
    lua_pop(lua_rt.L, 1); /* handler_id */

    lua_getfield(lua_rt.L, -1, "method");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "*");
    lua_pop(lua_rt.L, 1);

    lua_getfield(lua_rt.L, -1, "pattern");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "/*");
    lua_pop(lua_rt.L, 1);

    lua_pop(lua_rt.L, 1); /* entry table */
    lua_pop(lua_rt.L, 1); /* middleware table */

    /* Verify handler is in __hull_routes at the same index */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_routes");
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));
    lua_rawgeti(lua_rt.L, -1, handler_id);
    ASSERT_TRUE(lua_isfunction(lua_rt.L, -1));
    lua_pop(lua_rt.L, 2); /* handler + routes table */

    cleanup_lua();
}

UTEST(lua_middleware, handler_ids_do_not_collide_with_routes)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* Register a route first, then middleware */
    int rc = luaL_dostring(lua_rt.L,
        "app.get('/test', function(req, res) end)\n"
        "app.use('*', '/*', function(req, res) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    /* Route gets handler_id=1, middleware gets handler_id=2 */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_route_defs");
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "handler_id");
    int route_id = (int)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 3);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "handler_id");
    int mw_id = (int)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 3);

    ASSERT_NE(route_id, mw_id);

    /* Both should be valid function entries */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_routes");
    lua_rawgeti(lua_rt.L, -1, route_id);
    ASSERT_TRUE(lua_isfunction(lua_rt.L, -1));
    lua_pop(lua_rt.L, 1);
    lua_rawgeti(lua_rt.L, -1, mw_id);
    ASSERT_TRUE(lua_isfunction(lua_rt.L, -1));
    lua_pop(lua_rt.L, 2);

    cleanup_lua();
}

UTEST(lua_middleware, dispatch_return_zero_continues)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int rc = luaL_dostring(lua_rt.L,
        "app.use('*', '/*', function(req, res) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    /* Get the handler_id */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "handler_id");
    int handler_id = (int)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 3);

    /* Dispatch with stub request/response */
    KlRequest req = {0};
    KlResponse res = {0};
    int result = hl_lua_dispatch_middleware(&lua_rt, handler_id, &req, &res);
    ASSERT_EQ(result, 0);

    cleanup_lua();
}

UTEST(lua_middleware, dispatch_return_nonzero_short_circuits)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int rc = luaL_dostring(lua_rt.L,
        "app.use('*', '/*', function(req, res) return 1 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "handler_id");
    int handler_id = (int)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 3);

    KlRequest req = {0};
    KlResponse res = {0};
    int result = hl_lua_dispatch_middleware(&lua_rt, handler_id, &req, &res);
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

/* Track allocations from wire_routes_server to free them later */
static void *wiring_allocs_lua[16];
static int   wiring_alloc_count_lua;

static void *tracking_alloc_lua(size_t size)
{
    void *p = malloc(size);
    if (p && wiring_alloc_count_lua < 16)
        wiring_allocs_lua[wiring_alloc_count_lua++] = p;
    return p;
}

UTEST(lua_middleware, wiring_to_server)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* Need at least one route for wire_routes_server to not fail */
    int rc = luaL_dostring(lua_rt.L,
        "app.get('/test', function(req, res) end)\n"
        "app.use('*', '/*', function(req, res) return 0 end)\n"
        "app.use('GET', '/api/*', function(req, res) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    /* Create a minimal KlServer to wire into */
    KlServer server;
    KlConfig cfg = {
        .port = 0,
        .max_connections = 1,
        .alloc = NULL,
    };
    kl_server_init(&server, &cfg);

    wiring_alloc_count_lua = 0;
    rc = hl_lua_wire_routes_server(&lua_rt, &server, tracking_alloc_lua);
    ASSERT_EQ(rc, 0);

    /* Verify middleware was registered */
    ASSERT_EQ(server.router.mw_count, 2);

    /* Free tracked allocations (route + middleware contexts) */
    for (int i = 0; i < wiring_alloc_count_lua; i++)
        free(wiring_allocs_lua[i]);

    kl_server_free(&server);
    cleanup_lua();
}

UTEST(lua_middleware, order_preserved)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* Register two middlewares — order should be preserved */
    int rc = luaL_dostring(lua_rt.L,
        "app.use('*', '/*', function(req, res) return 0 end)\n"
        "app.use('GET', '/api/*', function(req, res) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));
    ASSERT_EQ((int)luaL_len(lua_rt.L, -1), 2);

    /* First middleware: method=*, pattern=/* */
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "method");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "*");
    lua_pop(lua_rt.L, 1);
    lua_getfield(lua_rt.L, -1, "pattern");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "/*");
    lua_pop(lua_rt.L, 2); /* pattern + entry */

    /* Second middleware: method=GET, pattern=/api/* */
    lua_rawgeti(lua_rt.L, -1, 2);
    lua_getfield(lua_rt.L, -1, "method");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "GET");
    lua_pop(lua_rt.L, 1);
    lua_getfield(lua_rt.L, -1, "pattern");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "/api/*");
    lua_pop(lua_rt.L, 2); /* pattern + entry */

    lua_pop(lua_rt.L, 1); /* middleware table */

    cleanup_lua();
}

UTEST(lua_runtime, manifest_get_manifest)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    const char *code =
        "app.manifest({ env = {'FOO'} })\n"
        "local m = app.get_manifest()\n"
        "return m.env[1]\n";

    if (luaL_dostring(lua_rt.L, code) == LUA_OK) {
        const char *val = lua_tostring(lua_rt.L, -1);
        ASSERT_STREQ(val, "FOO");
        lua_pop(lua_rt.L, 1);
    } else {
        const char *err = lua_tostring(lua_rt.L, -1);
        fprintf(stderr, "manifest_get_manifest error: %s\n", err ? err : "(nil)");
        lua_pop(lua_rt.L, 1);
        ASSERT_TRUE(0); /* force fail */
    }

    cleanup_lua();
}

/* ── HMAC-SHA256 / base64url tests ─────────────────────────────────── */

UTEST(lua_cap, crypto_hmac_sha256)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* RFC 4231 Test Case 2: key="Jefe", data="what do ya want for nothing?" */
    char *hmac = eval_str(
        "crypto.hmac_sha256('what do ya want for nothing?', '4a656665')");
    ASSERT_NE(hmac, NULL);
    ASSERT_STREQ(hmac,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    free(hmac);

    cleanup_lua_caps();
}

UTEST(lua_cap, crypto_base64url_roundtrip)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Encode known value */
    char *enc = eval_str("crypto.base64url_encode('Hello, World!')");
    ASSERT_NE(enc, NULL);
    ASSERT_STREQ(enc, "SGVsbG8sIFdvcmxkIQ");
    free(enc);

    /* Decode back */
    char *dec = eval_str("crypto.base64url_decode('SGVsbG8sIFdvcmxkIQ')");
    ASSERT_NE(dec, NULL);
    ASSERT_STREQ(dec, "Hello, World!");
    free(dec);

    /* Roundtrip */
    int ok = eval_int(
        "(function() "
        "  local orig = 'test data 123!@#' "
        "  return crypto.base64url_decode(crypto.base64url_encode(orig)) == orig and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    /* Invalid input returns nil */
    int is_nil = eval_int(
        "crypto.base64url_decode('!!!invalid!!!') == nil and 1 or 0");
    ASSERT_EQ(is_nil, 1);

    cleanup_lua_caps();
}

/* ── hull.cookie tests ─────────────────────────────────────────────── */

UTEST(lua_stdlib, cookie_parse)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local c = require('hull.cookie') "
        "  local r = c.parse('session=abc; theme=dark') "
        "  return r.session == 'abc' and r.theme == 'dark' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    /* Empty string returns empty table */
    int empty = eval_int(
        "(function() "
        "  local c = require('hull.cookie') "
        "  local r = c.parse('') "
        "  return next(r) == nil and 1 or 0 "
        "end)()");
    ASSERT_EQ(empty, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, cookie_serialize)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Default options: HttpOnly, SameSite=Lax, Path=/ (Secure defaults false) */
    char *cookie = eval_str(
        "require('hull.cookie').serialize('sid', 'abc123')");
    ASSERT_NE(cookie, NULL);
    ASSERT_NE(strstr(cookie, "sid=abc123"), NULL);
    ASSERT_NE(strstr(cookie, "HttpOnly"), NULL);
    ASSERT_EQ(strstr(cookie, "Secure"), NULL);
    ASSERT_NE(strstr(cookie, "SameSite=Lax"), NULL);
    ASSERT_NE(strstr(cookie, "Path=/"), NULL);
    free(cookie);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, cookie_clear)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    char *cookie = eval_str(
        "require('hull.cookie').clear('sid')");
    ASSERT_NE(cookie, NULL);
    ASSERT_NE(strstr(cookie, "sid="), NULL);
    ASSERT_NE(strstr(cookie, "Max-Age=0"), NULL);
    free(cookie);

    cleanup_lua_caps();
}

/* ── hull.session tests ────────────────────────────────────────────── */

UTEST(lua_stdlib, session_create_and_load)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local s = require('hull.session') "
        "  s.init({ ttl = 3600 }) "
        "  local id = s.create({ user_id = 42, email = 'test@example.com' }) "
        "  if not id or #id ~= 64 then return 0 end "
        "  local data = s.load(id) "
        "  if not data then return 0 end "
        "  return data.user_id == 42 and data.email == 'test@example.com' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, session_destroy)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local s = require('hull.session') "
        "  s.init() "
        "  local id = s.create({ foo = 'bar' }) "
        "  s.destroy(id) "
        "  return s.load(id) == nil and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* ── hull.jwt tests ────────────────────────────────────────────────── */

UTEST(lua_stdlib, jwt_sign_and_verify)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local jwt = require('hull.jwt') "
        "  local token = jwt.sign({ user_id = 1, exp = 3600 }, 'mysecret') "
        "  if not token then return 0 end "
        "  local payload = jwt.verify(token, 'mysecret') "
        "  if not payload then return 0 end "
        "  return payload.user_id == 1 and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, jwt_tampered_signature)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local jwt = require('hull.jwt') "
        "  local token = jwt.sign({ user_id = 1, exp = 3600 }, 'mysecret') "
        "  local payload, err = jwt.verify(token, 'wrongsecret') "
        "  return payload == nil and err == 'invalid signature' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, jwt_decode_without_verify)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local jwt = require('hull.jwt') "
        "  local token = jwt.sign({ user_id = 99 }, 'secret') "
        "  local payload = jwt.decode(token) "
        "  return payload and payload.user_id == 99 and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, jwt_malformed_rejected)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local jwt = require('hull.jwt') "
        "  local p, err = jwt.verify('not.a.valid.token', 'secret') "
        "  return p == nil and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* ── hull.csrf tests ───────────────────────────────────────────────── */

UTEST(lua_stdlib, csrf_generate_and_verify)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local csrf = require('hull.csrf') "
        "  local token = csrf.generate('session123', 'my_csrf_secret') "
        "  if not token then return 0 end "
        "  return csrf.verify(token, 'session123', 'my_csrf_secret') and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, csrf_wrong_session_rejected)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local csrf = require('hull.csrf') "
        "  local token = csrf.generate('session123', 'secret') "
        "  return csrf.verify(token, 'other_session', 'secret') and 0 or 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* ── hull.auth tests (smoke test — modules load and expose API) ───── */

UTEST(lua_cap, crypto_hmac_sha256_verify)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Correct MAC → true */
    int ok = eval_int(
        "(function() "
        "  local mac = crypto.hmac_sha256('what do ya want for nothing?', '4a656665') "
        "  return crypto.hmac_sha256_verify('what do ya want for nothing?', '4a656665', mac) and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    /* Wrong MAC → false */
    int bad_mac = eval_int(
        "crypto.hmac_sha256_verify('what do ya want for nothing?', '4a656665', "
        "  '0000000000000000000000000000000000000000000000000000000000000000') and 1 or 0");
    ASSERT_EQ(bad_mac, 0);

    /* Wrong key → false */
    int bad_key = eval_int(
        "(function() "
        "  local mac = crypto.hmac_sha256('hello', '4a656665') "
        "  return crypto.hmac_sha256_verify('hello', 'deadbeef', mac) and 1 or 0 "
        "end)()");
    ASSERT_EQ(bad_key, 0);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, auth_module_loads)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local auth = require('hull.auth') "
        "  return type(auth.session_middleware) == 'function' "
        "     and type(auth.jwt_middleware) == 'function' "
        "     and type(auth.login) == 'function' "
        "     and type(auth.logout) == 'function' "
        "     and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST_MAIN();
