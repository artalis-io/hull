/*
 * test_lua_runtime.c - Tests for Lua 5.4 runtime integration
 *
 * Tests: VM init, sandbox, module loading, route registration,
 * memory limits, GC.
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

#include "utest.h"
#include "hull/runtime/lua.h"
#include "hull/runtime/lua_bytecode_cache.h"
#include "hull/runtime/lua_template_cache.h"
#include "hull/reqctx.h"
#include "hull/vfs.h"
#include "hull/stdlib_feature.h"
#include "hull/cap/db.h"

#include "../../lua_script_test.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"
#include "hull/cap/db_registry.h"
#include "hull/cap/env.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>

#include "hull/limits/core.h"  /* HL_MODULE_MAX_SIZE; HL_LUA_* via runtime/lua.h */

#include <sqlite3.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "hull/shared/async_backend.h"
#include "../../test_tmpdir.h"
#include "../../../../src/hull/runtime/lua/internal.h"

/* ── Helpers ────────────────────────────────────────────────────────── */

static HlLua lua_rt;
static int lua_initialized = 0;
static HlVfs platform_vfs;
/* Merged baseUruntime stdlib array for platform_vfs; disposed before each
 * re-init so priors don't accumulate. The live one stays reachable via this
 * static (LSan-clean). */
static void *platform_vfs_owned = NULL;

/* Tests use lots of inline Lua snippets that reference modules as
 * globals (`db.exec(...)`, `crypto.sha256(...)`, ...). The runtime removes
 * those globals from production runtime - apps must `require` instead.
 * This helper restores the globals for testing convenience by trying to
 * require each known native module and assigning to `_G`. Modules that
 * aren't available (compile flag off, etc.) are silently skipped. */
static void install_test_globals(lua_State *L)
{
    static const char *PRELUDE =
        "for _, m in ipairs({"
        "  'crypto','db','env','time','fs','http','smtp',"
        "  'ws','image','compute','gpu','worker','server'"
        "}) do "
        "  local ok, mod = pcall(require, 'hull.' .. m) "
        "  if ok then _G[m] = mod end "
        "end "
        /* The db module now exposes only connect/default; the test snippets
         * use db.query/exec/... directly, so expose the default connection as
         * the `db` global (mirrors app code doing require('hull.db').default()). */
        "if _G.db and _G.db.default then _G.db = _G.db.default() end";
    (void)luaL_dostring(L, PRELUDE);
}

static void init_lua(void)
{
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    if (lua_initialized)
        hl_lua_free(&lua_rt);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    memset(&lua_rt, 0, sizeof(lua_rt));
    lua_rt.base.platform_vfs = &platform_vfs;
    int rc = hl_lua_init(&lua_rt, &cfg);
    lua_initialized = (rc == 0);
    if (lua_initialized) install_test_globals(lua_rt.L);
}

static void cleanup_lua(void)
{
    if (lua_initialized) {
        hl_lua_free(&lua_rt);
        lua_initialized = 0;
    }
}

/* Free HlReqCtx stored on req->ctx by middleware dispatch */
static void free_lua_req_ctx(KlHttpRequest *req)
{
    if (!req->ctx) return;
    HlReqCtx *rctx = (HlReqCtx *)req->ctx;
    if (rctx->kind == HL_REQCTX_LUA_REF && lua_initialized)
        luaL_unref(lua_rt.L, LUA_REGISTRYINDEX, rctx->lua_ref);
    else if (rctx->kind == HL_REQCTX_JSON)
        free(rctx->json.data);
    free(rctx);
    req->ctx = NULL;
}

/* Init lua with database and env capabilities for testing */
static sqlite3 *test_db = NULL;
static HlDbHandle test_db_handle;
static HlDbRegistry *test_db_registry;
static const char *env_allowed[] = { "HULL_TEST_VAR", NULL };
static HlEnvConfig env_cfg = { .allowed = env_allowed, .count = 1 };

static void init_lua_with_caps(void)
{
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    if (lua_initialized)
        hl_lua_free(&lua_rt);
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
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    memset(&lua_rt, 0, sizeof(lua_rt));
    lua_rt.base.db_registry = test_db_registry;
    lua_rt.base.env_cfg = &env_cfg;
    lua_rt.base.platform_vfs = &platform_vfs;
    int rc = hl_lua_init(&lua_rt, &cfg);
    lua_initialized = (rc == 0);
    if (lua_initialized) install_test_globals(lua_rt.L);
}

static void cleanup_lua_caps(void)
{
    if (lua_initialized) {
        hl_lua_free(&lua_rt);
        lua_initialized = 0;
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

/* Evaluate a Lua expression and return the result as a string.
 * Caller must free the returned string. Returns NULL on error. */
static char *eval_str(const char *code)
{
    if (!lua_initialized || !lua_rt.L)
        return NULL;

    /* Wrap in return statement for expression evaluation */
    char buf[16384];
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

    char buf[16384];
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

/* Run a co-located Lua test script INSIDE the Hull runtime (caps present).
 *
 * The sibling of run_lua_test (tests/hull/lua_script_test.h), for the scripts
 * that the vanilla harness cannot host at all. hull.search calls
 * require("hull.db").default() at module load, and hull.email requires
 * hull.http-client + hull.smtp; all three are C-backed with NO .lua file, so a
 * vanilla state cannot resolve them -- the failure is at require, before a
 * single assertion runs.
 *
 * Loading is from C (luaL_loadbuffer), not from Lua: the sandbox removes
 * load / loadfile / dofile from the script environment, and this deliberately
 * does not hand them back. The chunk name is the file path and NOT a "hull."
 * name, so the script is treated as app code -- it does not inherit the
 * stdlib's bypass of the _hull_ table-namespace guard, and a test that reached
 * for an internal table directly would still be refused.
 *
 * Same contract as the vanilla harness: the script ends with
 * `return { pass = pass, fail = fail }`; 0 on a clean run, -1 on any
 * harness-level failure. */
static int run_lua_test_in_runtime(const char *script_path, long long *pass_out,
                                   long long *fail_out)
{
    *pass_out = 0;
    *fail_out = -1;
    if (!lua_initialized || !lua_rt.L) return -1;

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
    buf[got] = 0;

    char chunk[512];
    snprintf(chunk, sizeof chunk, "@%s", script_path);
    int rc = luaL_loadbuffer(lua_rt.L, buf, got, chunk);
    free(buf);
    if (rc != LUA_OK) {
        fprintf(stderr, "\n%s: %s\n", script_path, lua_tostring(lua_rt.L, -1));
        lua_pop(lua_rt.L, 1);
        return -1;
    }
    if (lua_pcall(lua_rt.L, 0, 1, 0) != LUA_OK) {
        fprintf(stderr, "\n%s: %s\n", script_path, lua_tostring(lua_rt.L, -1));
        lua_pop(lua_rt.L, 1);
        return -1;
    }
    if (!lua_istable(lua_rt.L, -1)) {
        fprintf(stderr, "\n%s: did not return a { pass, fail } table\n", script_path);
        lua_pop(lua_rt.L, 1);
        return -1;
    }

    lua_getfield(lua_rt.L, -1, "fail");
    *fail_out = (long long)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 1);
    lua_getfield(lua_rt.L, -1, "pass");
    *pass_out = (long long)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 2);   /* the pass value, then the table */

    fprintf(stderr, "  %s: %lld passed, %lld failed\n", script_path, *pass_out, *fail_out);
    return 0;
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

    /* Tables work - basic serialization check */
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
        "app.manifest({modules = {'hull/http-server@1'}})\napp.get('/test', function(req, res) res:json({ok=true}) end)\n"
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

/* ── app.router tests ────────────────────────────────────────────────
 *
 * app.router(prefix, opts) returns a Router object that batches
 * route registration with a common path prefix. Methods compose on
 * top of app.get/app.post/app.use, so we verify by inspecting
 * __hull_route_defs / __hull_middleware after the calls. */

UTEST(lua_runtime, app_router_prefixes_routes)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\nlocal r = app.router('/api/v1')\n"
        "r:get('/items', function(req, res) end)\n"
        "r:post('/items', function(req, res) end)\n"
        "r:put('/items/:id', function(req, res) end)\n"
        "r:delete('/items/:id', function(req, res) end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_route_defs");
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));
    ASSERT_EQ((int)luaL_len(lua_rt.L, -1), 4);

    const char *expect_methods[]  = {"GET","POST","PUT","DELETE"};
    const char *expect_patterns[] = {"/api/v1/items","/api/v1/items",
                                      "/api/v1/items/:id","/api/v1/items/:id"};
    for (int i = 1; i <= 4; i++) {
        lua_rawgeti(lua_rt.L, -1, i);
        lua_getfield(lua_rt.L, -1, "method");
        ASSERT_STREQ(lua_tostring(lua_rt.L, -1), expect_methods[i-1]);
        lua_pop(lua_rt.L, 1);
        lua_getfield(lua_rt.L, -1, "pattern");
        ASSERT_STREQ(lua_tostring(lua_rt.L, -1), expect_patterns[i-1]);
        lua_pop(lua_rt.L, 2);
    }
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
}

UTEST(lua_runtime, app_router_nested_composes_prefixes)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\nlocal api   = app.router('/api/v1')\n"
        "local admin = api:router('/admin')\n"
        "admin:get('/users', function(req, res) end)\n"
        "admin:get('/audit', function(req, res) end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_route_defs");
    ASSERT_EQ((int)luaL_len(lua_rt.L, -1), 2);

    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "pattern");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "/api/v1/admin/users");
    lua_pop(lua_rt.L, 2);

    lua_rawgeti(lua_rt.L, -1, 2);
    lua_getfield(lua_rt.L, -1, "pattern");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "/api/v1/admin/audit");
    lua_pop(lua_rt.L, 3);

    cleanup_lua();
}

UTEST(lua_runtime, app_router_use_with_handler_only)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\nlocal r = app.router('/api')\n"
        "r:use(function(req, res) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));
    ASSERT_EQ((int)luaL_len(lua_rt.L, -1), 1);

    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "method");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "*");
    lua_pop(lua_rt.L, 1);
    lua_getfield(lua_rt.L, -1, "pattern");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "/api/*");
    lua_pop(lua_rt.L, 3);

    cleanup_lua();
}

UTEST(lua_runtime, app_router_use_with_explicit_method_pattern)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\nlocal r = app.router('/api')\n"
        "r:use('POST', '/items', function(req, res) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    ASSERT_EQ((int)luaL_len(lua_rt.L, -1), 1);

    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "method");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "POST");
    lua_pop(lua_rt.L, 1);
    lua_getfield(lua_rt.L, -1, "pattern");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "/api/items");
    lua_pop(lua_rt.L, 3);

    cleanup_lua();
}

UTEST(lua_runtime, app_router_chainable)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\napp.router('/api')\n"
        "  :get('/a', function() end)\n"
        "  :post('/b', function() end)\n"
        "  :delete('/c', function() end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_route_defs");
    ASSERT_EQ((int)luaL_len(lua_rt.L, -1), 3);
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
}

/* ── hull/timers decoration tests ────────────────────────────────────
 *
 * app.every / app.daily are conditionally installed by app.manifest
 * when the manifest's modules array contains "hull/timers@*". Without
 * the declaration the methods literally don't exist on `app` -
 * calling them raises "attempt to call a nil value". */

UTEST(lua_runtime, app_timers_absent_without_declaration)
{
    init_lua();
    /* No app.manifest call at all → every/daily are nil */
    int every_nil = eval_int("app.every == nil and 1 or 0");
    int daily_nil = eval_int("app.daily == nil and 1 or 0");
    ASSERT_EQ(every_nil, 1);
    ASSERT_EQ(daily_nil, 1);
    cleanup_lua();
}

UTEST(lua_runtime, app_timers_absent_with_empty_modules)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({ modules = {} })\n");
    ASSERT_EQ(rc, LUA_OK);
    int every_nil = eval_int("app.every == nil and 1 or 0");
    int daily_nil = eval_int("app.daily == nil and 1 or 0");
    ASSERT_EQ(every_nil, 1);
    ASSERT_EQ(daily_nil, 1);
    cleanup_lua();
}

UTEST(lua_runtime, app_timers_present_when_declared)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({ modules = { 'hull/timers@1' } })\n");
    ASSERT_EQ(rc, LUA_OK);
    int every_fn = eval_int("type(app.every) == 'function' and 1 or 0");
    int daily_fn = eval_int("type(app.daily) == 'function' and 1 or 0");
    ASSERT_EQ(every_fn, 1);
    ASSERT_EQ(daily_fn, 1);
    cleanup_lua();
}

UTEST(lua_runtime, app_timers_register_timer_when_declared)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({ modules = { 'hull/timers@1' } })\n"
        "app.every(1000, function() end)\n");
    ASSERT_EQ(rc, LUA_OK);
    /* timer registration stores into __hull_timer_defs */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_timer_defs");
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));
    int count = (int)luaL_len(lua_rt.L, -1);
    ASSERT_EQ(count, 1);
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

UTEST(lua_runtime, app_router_empty_prefix)
{
    /* app.router() with no prefix should still work - empty prefix
     * means routes register at the bare paths. */
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\nlocal r = app.router()\n"
        "r:get('/items', function() end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_route_defs");
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "pattern");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "/items");
    lua_pop(lua_rt.L, 3);

    cleanup_lua();
}

/* ── app.main (CLI mode) tests ─────────────────────────────────────── */

UTEST(lua_runtime, app_main_registers)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.main(function(ctx) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    /* __hull_main should be set to a function */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_main");
    ASSERT_TRUE(lua_isfunction(lua_rt.L, -1));
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

UTEST(lua_runtime, app_main_twice_rejected)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.main(function() return 0 end)\n"
        "app.main(function() return 1 end)\n");
    ASSERT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_TRUE(strstr(err, "only be called once") != NULL);
    cleanup_lua();
}

UTEST(lua_runtime, app_main_coexists_with_routes_after)
{
    /* app.main + routes are no longer mutually exclusive: app.main
     * is a startup hook, routes are served after it returns. See
     * docs/cli_mode.md and CLAUDE.md "App Lifecycle". */
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\napp.main(function() return 0 end)\n"
        "app.get('/x', function() end)\n");
    ASSERT_EQ(rc, LUA_OK);
    cleanup_lua();
}

UTEST(lua_runtime, routes_coexist_with_app_main_after)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\napp.get('/x', function() end)\n"
        "app.main(function() return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);
    cleanup_lua();
}

UTEST(lua_runtime, app_main_via_vtable_runs)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.main(function(ctx)\n"
        "  ctx.stderr:write('hi from main\\n')\n"
        "  return 7\n"
        "end)\n");
    ASSERT_EQ(rc, LUA_OK);

    ASSERT_TRUE(hl_lua_vtable.has_main(&lua_rt.base));
    int exit_code = 99;
    int run_rc = hl_lua_vtable.run_main(&lua_rt.base, NULL, 0, NULL, NULL, &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 7);
    cleanup_lua();
}

UTEST(lua_runtime, app_main_nil_return_yields_zero)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.main(function() return end)\n");
    ASSERT_EQ(rc, LUA_OK);

    int exit_code = 99;
    int run_rc = hl_lua_vtable.run_main(&lua_rt.base, NULL, 0, NULL, NULL, &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 0);
    cleanup_lua();
}

UTEST(lua_runtime, app_main_string_return_is_error)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.main(function() return 'oops' end)\n");
    ASSERT_EQ(rc, LUA_OK);

    int exit_code = 0;
    int run_rc = hl_lua_vtable.run_main(&lua_rt.base, NULL, 0, NULL, NULL, &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 1);
    cleanup_lua();
}

UTEST(lua_runtime, app_main_clamps_large_return)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.main(function() return 300 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    int exit_code = 0;
    int run_rc = hl_lua_vtable.run_main(&lua_rt.base, NULL, 0, NULL, NULL, &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 44);   /* 300 & 0xff */
    cleanup_lua();
}

UTEST(lua_runtime, app_main_thrown_error_returns_minus_one)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.main(function() error('boom') end)\n");
    ASSERT_EQ(rc, LUA_OK);

    int exit_code = 0;
    int run_rc = hl_lua_vtable.run_main(&lua_rt.base, NULL, 0, NULL, NULL, &exit_code);
    ASSERT_EQ(run_rc, -1);
    ASSERT_EQ(exit_code, 1);    /* preset to 1 at entry */
    cleanup_lua();
}

UTEST(lua_runtime, has_main_false_when_not_registered)
{
    init_lua();
    ASSERT_FALSE(hl_lua_vtable.has_main(&lua_rt.base));
    cleanup_lua();
}

UTEST(lua_runtime, app_main_ctx_args_and_env)
{
    init_lua();
    /* Register main that captures ctx.args and ctx.env into globals so
     * the test can inspect them after run_main returns. */
    int rc = luaL_dostring(lua_rt.L,
        "_G.test_main_args = nil\n"
        "_G.test_main_env_user = nil\n"
        "app.main(function(ctx)\n"
        "  _G.test_main_args = ctx.args\n"
        "  _G.test_main_env_user = ctx.env.TEST_VAR\n"
        "  return 0\n"
        "end)\n");
    ASSERT_EQ(rc, LUA_OK);

    setenv("TEST_VAR", "test_value", 1);
    char *argv_in[] = { "alpha", "beta", "gamma" };
    const char *env_allow[] = { "TEST_VAR", NULL };
    int exit_code = 99;
    int run_rc = hl_lua_vtable.run_main(&lua_rt.base, NULL, 3, argv_in,
                                         env_allow, &exit_code);
    ASSERT_EQ(run_rc, 0);
    ASSERT_EQ(exit_code, 0);

    lua_getglobal(lua_rt.L, "test_main_args");
    ASSERT_TRUE(lua_istable(lua_rt.L, -1));
    ASSERT_EQ((int)luaL_len(lua_rt.L, -1), 3);
    lua_rawgeti(lua_rt.L, -1, 2);
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "beta");
    lua_pop(lua_rt.L, 2);

    lua_getglobal(lua_rt.L, "test_main_env_user");
    ASSERT_STREQ(lua_tostring(lua_rt.L, -1), "test_value");
    lua_pop(lua_rt.L, 1);
    unsetenv("TEST_VAR");
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

/* ── Module-set gating in require() ────────────────────────────────── */
/* These exercise the phase-2a gate in hl_lua_require: when the runtime
 * has a non-NULL module_set, names that map to a known first-party
 * module must be in that set or require() raises. */

#include "hull/manifest.h"
#include "hull/module_registry.h"
#include "hull/module_resolver.h"

UTEST(lua_runtime, require_gated_undeclared_module_fails)
{
    init_lua();

    /* Wire an empty resolved set (intrinsics only - no crypto/validate). */
    HlResolvedModuleSet set;
    hl_module_set_clear(&set);
    lua_rt.base.module_set = &set;

    int rc = luaL_dostring(lua_rt.L, "require('hull.validate')");
    ASSERT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    /* Error mentions the runtime-form name + the manifest fix + the
     * `hull modules available` hint. */
    ASSERT_NE(strstr(err, "hull.validate"), NULL);
    ASSERT_NE(strstr(err, "app.manifest"), NULL);
    ASSERT_NE(strstr(err, "hull modules available"), NULL);
    lua_pop(lua_rt.L, 1);

    lua_rt.base.module_set = NULL;
    cleanup_lua();
}

UTEST(lua_runtime, require_gated_declared_module_succeeds)
{
    init_lua();

    /* Resolve with hull/validate admitted via the real resolver. */
    HlManifest m;
    memset(&m, 0, sizeof(m));
    m.modules[0].name = "validate";
    m.modules[0].api_major = 1;
    m.modules_count = 1;
    m.modules_declared = 1;

    HlResolvedModuleSet set;
    char err_resolver[256] = {0};
    int rc_resolve = hl_module_resolver_resolve(&m, &set, err_resolver,
                                                 sizeof(err_resolver));
    ASSERT_EQ(rc_resolve, 0);

    lua_rt.base.module_set = &set;

    int rc = luaL_dostring(lua_rt.L,
        "local v = require('hull.validate'); "
        "if type(v) ~= 'table' then error('not a table') end");
    if (rc != LUA_OK) {
        const char *err = lua_tostring(lua_rt.L, -1);
        fprintf(stderr, "load err: %s\n", err ? err : "?");
        lua_pop(lua_rt.L, 1);
    }
    ASSERT_EQ(rc, LUA_OK);

    lua_rt.base.module_set = NULL;
    cleanup_lua();
}

UTEST(lua_runtime, require_gating_skipped_for_user_modules)
{
    /* Names that don't map to any registry entry fall through to the
     * normal lookup - gating does NOT intercept user code. */
    init_lua();

    HlResolvedModuleSet set;
    hl_module_set_clear(&set);
    lua_rt.base.module_set = &set;

    int rc = luaL_dostring(lua_rt.L, "require('myapp.helpers')");
    /* Without an app_dir, expect "module not found" (original behavior),
     * NOT the gating error. */
    ASSERT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "module not found"), NULL);
    /* And NOT the gating message. */
    ASSERT_EQ(strstr(err, "app.manifest"), NULL);
    lua_pop(lua_rt.L, 1);

    lua_rt.base.module_set = NULL;
    cleanup_lua();
}

UTEST(lua_runtime, require_null_module_set_is_permissive)
{
    /* NULL module_set = legacy entry points: gating disabled. */
    init_lua();
    ASSERT_EQ(lua_rt.base.module_set, NULL);

    /* require('hull.validate') would normally fire the gate if a set
     * were wired; with NULL set, behavior matches pre-phase-2. */
    int rc = luaL_dostring(lua_rt.L,
        "local v = require('hull.validate'); "
        "if type(v) ~= 'table' then error('not a table') end");
    ASSERT_EQ(rc, LUA_OK);

    cleanup_lua();
}

UTEST(lua_runtime, require_resolves_native_modules)
{
    /* Native C modules (luaL_requiref-registered) like
     * hull.crypto must resolve via the custom require even though they
     * live in _LOADED, not __hull_modules. */
    init_lua();

    int rc = luaL_dostring(lua_rt.L,
        "local c = require('hull.crypto'); "
        "if type(c) ~= 'table' or type(c.sha256) ~= 'function' "
        "then error('crypto not loaded correctly: ' .. type(c)) end");
    if (rc != LUA_OK) {
        const char *e = lua_tostring(lua_rt.L, -1);
        fprintf(stderr, "require native err: %s\n", e ? e : "?");
    }
    ASSERT_EQ(rc, LUA_OK);

    cleanup_lua();
}

UTEST(lua_runtime, json_module_requireable)
{
    init_lua();

    /* json is a DECLARED module as of v0.1.0 release - no longer
     * a global. require("hull.json") returns the table. Wrapped in
     * IIFE since eval_int prefixes "return". */
    int result = eval_int(
        "(function() local json = require('hull.json') "
        "return type(json) == 'table' and type(json.encode) == 'function' "
        "and type(json.decode) == 'function' and 1 or 0 end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

UTEST(lua_runtime, json_encode_decode)
{
    init_lua();

    char *s = eval_str(
        "(function() local json = require('hull.json') "
        "return json.encode({name='hull'}) end)()");
    ASSERT_NE(s, NULL);
    ASSERT_NE(strstr(s, "\"name\""), NULL);
    ASSERT_NE(strstr(s, "\"hull\""), NULL);
    free(s);

    int result = eval_int(
        "(function() local json = require('hull.json') "
        "return json.decode('{\"x\":42}').x end)()");
    ASSERT_EQ(result, 42);

    cleanup_lua();
}

UTEST(lua_runtime, json_roundtrip)
{
    init_lua();

    int result = eval_int(
        "(function() local json = require('hull.json') "
        "local t = {a=1, b='two'} "
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

    /* Trigger an error - should not crash */
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

/* ── Instruction limit tests ─────────────────────────────────────────── */

UTEST(lua_runtime, instruction_limit_catches_infinite_loop)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.max_instructions = 1000; /* very low limit */
    HlLua limited_lua;
    memset(&limited_lua, 0, sizeof(limited_lua));

    int rc = hl_lua_init(&limited_lua, &cfg);
    ASSERT_EQ(rc, 0);

    /* Infinite loop should be interrupted */
    rc = luaL_dostring(limited_lua.L, "while true do end");
    ASSERT_NE(rc, LUA_OK);

    /* Error message should mention instruction limit */
    const char *err = lua_tostring(limited_lua.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "instruction limit"), NULL);
    lua_pop(limited_lua.L, 1);

    /* VM should still be functional after the error */
    rc = luaL_dostring(limited_lua.L, "return 1 + 1");
    ASSERT_EQ(rc, LUA_OK);
    int result = (int)lua_tointeger(limited_lua.L, -1);
    ASSERT_EQ(result, 2);
    lua_pop(limited_lua.L, 1);

    hl_lua_free(&limited_lua);
}

UTEST(lua_runtime, instruction_limit_unlimited_allows_long_code)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.max_instructions = 0; /* unlimited */
    HlLua unlimited_lua;
    memset(&unlimited_lua, 0, sizeof(unlimited_lua));

    int rc = hl_lua_init(&unlimited_lua, &cfg);
    ASSERT_EQ(rc, 0);

    /* 10K-iteration loop should complete without error */
    rc = luaL_dostring(unlimited_lua.L,
        "local sum = 0; for i = 1, 10000 do sum = sum + i end");
    ASSERT_EQ(rc, LUA_OK);

    hl_lua_free(&unlimited_lua);
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
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    if (lua_initialized)
        hl_lua_free(&lua_rt);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    memset(&lua_rt, 0, sizeof(lua_rt));
    lua_rt.base.platform_vfs = &platform_vfs;
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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);

    /* require('../../etc/passwd') should error - escapes above app_dir */
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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

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
    /* The loader passes "@path", which marks a FILE chunkname: Lua renders
     * errors as "path:line:" and luaO_chunkid truncates from the FRONT, so
     * the file name survives however long the host's temp path is. (With a
     * bare chunkname Lua renders [string "..."] and truncates from the BACK,
     * dropping the name - which is what this used to have to tolerate.) */
    ASSERT_NE(strstr(err, "bad.lua"), NULL);
    lua_pop(lua_rt.L, 1);

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_require_fs, returns_nil)
{
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

    /* Create a file that exceeds HL_MODULE_MAX_SIZE */
    char path[1024];
    snprintf(path, sizeof(path), "%s/big.lua", tmpdir);
    FILE *f = fopen(path, "w");
    ASSERT_TRUE(f != NULL);
    /* Write just past the limit - use fseek to create a sparse file */
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
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_test"), NULL);

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

    /* SHA-256 of "hello" - known hash */
    char *hash = eval_str("require('hull.encoding').hex.encode(crypto.sha256('hello'))");
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

    /* log is a DECLARED module as of v0.1.0 release - no longer a
     * global. Apps must require("hull.log"). The test environment
     * has no manifest, so the require gate is permissive. */
    int result = eval_int(
        "(function() local log = require('hull.log') "
        "return type(log.info) == 'function' and "
        "type(log.warn) == 'function' and "
        "type(log.error) == 'function' and "
        "type(log.debug) == 'function' and 1 or 0 end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, log_does_not_error)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Calling all four log functions should not raise a Lua error */
    int rc = luaL_dostring(lua_rt.L,
        "local log = require('hull.log')\n"
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

    /* PATH is not in the allowlist - should return nil */
    int result = eval_int("env.get('PATH') == nil and 1 or 0");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, env_get_nonexistent)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* HULL_TEST_VAR is allowed but not set - should return nil */
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

/* ── DB namespace protection tests ──────────────────────────────────── */

UTEST(lua_chunkname, stdlib_namespace_survives_the_file_marker)
{
    /* Modules are loaded with an "@" marker so errors render as
     * "name:line:" rather than [string "name"]:line:. ar.source keeps that
     * marker verbatim (only short_src strips it), and two gates match
     * ar.source against "hull.": mod_db.c decides whether _hull_* internal
     * tables may be touched, and mod_fs.c decides whether a require came
     * from user code. A raw strncmp would stop matching the moment the
     * marker appeared, silently revoking every stdlib module's access to
     * its own tables - so the namespace test skips the marker first. */
    ASSERT_TRUE(hl_lua_source_is_stdlib("hull.template"));   /* unmarked */
    ASSERT_TRUE(hl_lua_source_is_stdlib("@hull.template"));  /* file      */
    ASSERT_TRUE(hl_lua_source_is_stdlib("=hull.template"));  /* literal   */

    /* App code is not in the namespace, marked or not. */
    ASSERT_FALSE(hl_lua_source_is_stdlib("./routes/users"));
    ASSERT_FALSE(hl_lua_source_is_stdlib("@./routes/users"));
    ASSERT_FALSE(hl_lua_source_is_stdlib("@/tmp/app/hull_evil.lua"));

    /* The dot matters: "hull" alone, or a lookalike, is not the namespace. */
    ASSERT_FALSE(hl_lua_source_is_stdlib("@hullx.template"));
    ASSERT_FALSE(hl_lua_source_is_stdlib("@hull"));
    ASSERT_FALSE(hl_lua_source_is_stdlib(NULL));
}

UTEST(lua_cap, db_namespace_blocks_hull_tables)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int result = eval_int(
        "(function() "
        "  local ok, err = pcall(db.exec, 'CREATE TABLE _hull_test (id INT)') "
        "  if not ok and string.find(tostring(err), 'reserved') then return 1 end "
        "  return 0 "
        "end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, db_namespace_blocks_hull_query)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int result = eval_int(
        "(function() "
        "  local ok, err = pcall(db.query, 'SELECT * FROM _hull_outbox') "
        "  if not ok and string.find(tostring(err), 'reserved') then return 1 end "
        "  return 0 "
        "end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, db_namespace_no_internal_bypass)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* db._exec and db._query must not exist - no bypass possible */
    int result = eval_int(
        "(function() "
        "  return (db._exec == nil and db._query == nil) and 1 or 0 "
        "end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

UTEST(lua_cap, db_namespace_allows_normal_tables)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int result = eval_int(
        "(function() "
        "  db.exec('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)') "
        "  db.exec('INSERT INTO users (name) VALUES (?)', {'alice'}) "
        "  local rows = db.query('SELECT name FROM users') "
        "  return rows[1].name == 'alice' and 1 or 0 "
        "end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua_caps();
}

/* ── Manifest tests ────────────────────────────────────────────────── */

#include "hull/manifest.h"

UTEST(lua_runtime, manifest_not_declared)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    HlManifest m;
    int rc = hl_manifest_extract_lua(lua_rt.L, &m, NULL);
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
    rc = hl_manifest_extract_lua(lua_rt.L, &m, NULL);
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

    hl_manifest_free(&m);
    cleanup_lua();
}

/* ── Middleware tests ────────────────────────────────────────────────── */

UTEST(lua_middleware, registration_stores_handler_id)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\napp.use('*', '/*', function(req, res) return 0 end)\n");
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
        "app.manifest({modules = {'hull/http-server@1'}})\napp.get('/test', function(req, res) end)\n"
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
        "app.manifest({modules = {'hull/http-server@1'}})\napp.use('*', '/*', function(req, res) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    /* Get the handler_id */
    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "handler_id");
    int handler_id = (int)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 3);

    /* Dispatch with stub request/response */
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    int result = hl_lua_dispatch_middleware(&lua_rt, handler_id, &req, &res);
    ASSERT_EQ(result, 0);

    free_lua_req_ctx(&req);
    cleanup_lua();
}

UTEST(lua_middleware, dispatch_return_nonzero_short_circuits)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\napp.use('*', '/*', function(req, res) return 1 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "handler_id");
    int handler_id = (int)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 3);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    int result = hl_lua_dispatch_middleware(&lua_rt, handler_id, &req, &res);
    ASSERT_EQ(result, 1);

    free_lua_req_ctx(&req);
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
        "app.manifest({modules = {'hull/http-server@1'}})\napp.get('/test', function(req, res) end)\n"
        "app.use('*', '/*', function(req, res) return 0 end)\n"
        "app.use('GET', '/api/*', function(req, res) return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    /* Create a minimal KlHttpServer to wire into */
    KlHttpServer server;
    KlHttpServerConfig cfg = {
        .port = 0,
        .max_connections = 1,
        .alloc = NULL,
    };
    kl_http_server_init(&server, &cfg);

    wiring_alloc_count_lua = 0;
    rc = hl_lua_wire_routes_server(&lua_rt, &server, tracking_alloc_lua);
    ASSERT_EQ(rc, 0);

    /* Verify middleware was registered */
    ASSERT_EQ(server.router.mw_count, 2);

    /* Free tracked allocations (route + middleware contexts) */
    for (int i = 0; i < wiring_alloc_count_lua; i++)
        free(wiring_allocs_lua[i]);

    kl_http_server_free(&server);
    cleanup_lua();
}

UTEST(lua_middleware, order_preserved)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* Register two middlewares - order should be preserved */
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\napp.use('*', '/*', function(req, res) return 0 end)\n"
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

/* ── HMAC-SHA256 tests ───────────────────────────────────────────── */

UTEST(lua_cap, crypto_hmac_sha256)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* RFC 4231 Test Case 2: key="Jefe", data="what do ya want for nothing?" */
    char *hmac = eval_str(
        "require('hull.encoding').hex.encode(crypto.hmac_sha256('what do ya want for nothing?', 'Jefe'))");
    ASSERT_NE(hmac, NULL);
    ASSERT_STREQ(hmac,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    free(hmac);

    cleanup_lua_caps();
}

/* The byte API: every key, nonce, signature, tag and digest is raw bytes of
 * its exact size, round trips work, and a wrong-length argument raises.
 * Returns 0, or the number of the first check that failed. */
UTEST(lua_cap, crypto_bytes_contract)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  if #crypto.sha256('x') ~= 32 or #crypto.sha512('x') ~= 64 then return 1 end "
        "  if #crypto.hmac_sha256('x', 'k') ~= 32 or #crypto.hmac_sha1('x', 'k') ~= 20 then return 2 end "
        "  local h = crypto.create_sha256(); h:update('he'); h:update('llo') "
        "  if h:digest() ~= crypto.sha256('hello') then return 3 end "
        "  local pk, sk = crypto.ed25519_keypair() "
        "  if #pk ~= 32 or #sk ~= 64 then return 4 end "
        "  local sig = crypto.ed25519_sign('msg', sk) "
        "  if #sig ~= 64 or not crypto.ed25519_verify('msg', sig, pk) then return 5 end "
        "  if crypto.ed25519_verify('msh', sig, pk) then return 6 end "
        "  local key, nonce = crypto.random(32), crypto.random(24) "
        "  local ct = crypto.secretbox('secret', nonce, key) "
        "  if #ct ~= 6 + 16 or crypto.secretbox_open(ct, nonce, key) ~= 'secret' then return 7 end "
        "  if crypto.secretbox_open(ct:sub(1, -2) .. string.char(ct:byte(-1) ~ 1), nonce, key) ~= nil then return 8 end "
        "  local apk, ask = crypto.box_keypair() "
        "  local bpk, bsk = crypto.box_keypair() "
        "  local bct = crypto.box('hi', nonce, bpk, ask) "
        "  if crypto.box_open(bct, nonce, apk, bsk) ~= 'hi' then return 9 end "
        "  local xa_pk, xa_sk = crypto.x25519_keypair() "
        "  local xb_pk, xb_sk = crypto.x25519_keypair() "
        "  local s1, s2 = crypto.x25519(xa_sk, xb_pk), crypto.x25519(xb_sk, xa_pk) "
        "  if #s1 ~= 32 or s1 ~= s2 then return 10 end "
        "  local tag = crypto.auth('m', key) "
        "  if #tag ~= 32 or not crypto.auth_verify(tag, 'm', key) then return 11 end "
        "  if pcall(crypto.ed25519_sign, 'm', sk:sub(2)) then return 12 end "
        "  if pcall(crypto.secretbox, 'm', nonce, key .. 'x') then return 13 end "
        "  if pcall(crypto.hmac_sha256, 'm', '') then return 14 end "
        "  if crypto.hmac_sha256_verify('m', 'k', 'short') then return 15 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(step, 0);
    cleanup_lua_caps();
}

UTEST(lua_cap, crypto_constant_time_eq)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    char *r;
    r = eval_str("crypto.constant_time_eq('abc','abc') and '1' or '0'");
    ASSERT_NE(r, NULL); ASSERT_STREQ(r, "1"); free(r);          /* equal */
    r = eval_str("crypto.constant_time_eq('abc','abd') and '1' or '0'");
    ASSERT_NE(r, NULL); ASSERT_STREQ(r, "0"); free(r);          /* differ */
    r = eval_str("crypto.constant_time_eq('abc','ab') and '1' or '0'");
    ASSERT_NE(r, NULL); ASSERT_STREQ(r, "0"); free(r);          /* length */
    r = eval_str("crypto.constant_time_eq('','') and '1' or '0'");
    ASSERT_NE(r, NULL); ASSERT_STREQ(r, "1"); free(r);          /* empty */

    cleanup_lua_caps();
}

UTEST(lua_cap, crypto_hmac_sha1)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* RFC 2202 Test Case 2: key="Jefe", data="what do ya want for nothing?".
     * Provides binding-level proof that the vtable dispatches correctly
     * to mbedTLS for SHA-1. */
    char *hmac = eval_str(
        "require('hull.encoding').hex.encode(crypto.hmac_sha1('what do ya want for nothing?', 'Jefe'))");
    ASSERT_NE(hmac, NULL);
    ASSERT_STREQ(hmac, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    free(hmac);

    /* RFC 6238 TOTP HMAC-SHA1 reference vector for T = 59 (counter = 1).
     *
     * The spec uses key "12345678901234567890" (ASCII). Counter 1 encodes
     * to the big-endian 8-byte value 0x0000000000000001.
     *
     * Build the 8-byte BE counter in Lua via string.pack - proves
     * the full TOTP-style call sequence works through the binding. */
    char *vec = eval_str(
        "require('hull.encoding').hex.encode(crypto.hmac_sha1(string.pack('>I8', 1), "
        "'12345678901234567890'))");
    ASSERT_NE(vec, NULL);
    ASSERT_STREQ(vec, "75a48a19d4cbe100644e8ac1397eea747a2d33ab");
    free(vec);

    cleanup_lua_caps();
}

/* ── hull.qrcode tests ─────────────────────────────────────────────────── */

UTEST(lua_stdlib, qrcode_hello_v1)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* "Hello" at EC M fits in v1 (21x21). Pin a few known structural
     * properties (size, version, mask) and one data-cell that varies
     * by encoding correctness. The full matrix was cross-verified
     * against Python's `qrcode` library (RFC-style reference impl) on
     * 48 input/EC/mask combinations during development. */
    int size = eval_int(
        "(function() "
        "  local qr = require('hull.qrcode') "
        "  local q = qr.encode('Hello', { ec_level = 'M', mask = 0 }) "
        "  return q.size "
        "end)()");
    ASSERT_EQ(size, 21);

    int version = eval_int(
        "(function() "
        "  local qr = require('hull.qrcode') "
        "  return qr.encode('Hello', { ec_level = 'M', mask = 0 }).version "
        "end)()");
    ASSERT_EQ(version, 1);

    /* Mask 0 was forced; encoder should honor it. */
    int mask = eval_int(
        "(function() "
        "  local qr = require('hull.qrcode') "
        "  return qr.encode('Hello', { ec_level = 'M', mask = 0 }).mask "
        "end)()");
    ASSERT_EQ(mask, 0);

    /* Data cell at (9, 17) per the Python reference for ('Hello', M, 0).
     * If the encoding or placement regresses, this cell flips. */
    int cell = eval_int(
        "(function() "
        "  local qr = require('hull.qrcode') "
        "  local q = qr.encode('Hello', { ec_level = 'M', mask = 0 }) "
        "  return q.matrix[9][17] "
        "end)()");
    ASSERT_EQ(cell, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, qrcode_auto_mask_and_version)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* No mask arg → encoder scores all 8 and picks the best one.
     * For "Hello" at EC M, the Python reference picks mask 2; pinning
     * this catches regressions in the score-based selector. */
    int mask = eval_int(
        "(function() "
        "  local qr = require('hull.qrcode') "
        "  return qr.encode('Hello', { ec_level = 'M' }).mask "
        "end)()");
    ASSERT_EQ(mask, 2);

    /* Auto version selection - 73-byte URL fits in v5 at EC M. */
    int version = eval_int(
        "(function() "
        "  local qr = require('hull.qrcode') "
        "  local url = 'otpauth://totp/Hull:alice@example.com?secret=JBSWY3DPEHPK3PXP&issuer=Hull' "
        "  return qr.encode(url, { ec_level = 'M' }).version "
        "end)()");
    ASSERT_EQ(version, 5);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, qrcode_svg)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* SVG output has the expected wrapper + at least one path element. */
    char *prefix = eval_str(
        "(function() "
        "  local qr = require('hull.qrcode') "
        "  local s = qr.svg('Hi', { scale = 2 }) "
        "  return s:sub(1, 4) "
        "end)()");
    ASSERT_NE(prefix, NULL);
    ASSERT_STREQ(prefix, "<svg");
    free(prefix);

    int has_path = eval_int(
        "(function() "
        "  local qr = require('hull.qrcode') "
        "  local s = qr.svg('Hi') "
        "  return s:find('<path', 1, true) and 1 or 0 "
        "end)()");
    ASSERT_EQ(has_path, 1);

    cleanup_lua_caps();
}

/* ── hull.web.middleware.totp tests ────────────────────────────────────── */

/* Pure-function RFC vectors: Base32 (RFC 4648) + TOTP step digest
 * (RFC 6238 Appendix B). These exercise the math without the DB
 * round-trip - if these fail, the whole module is broken at the
 * foundation. */
UTEST(lua_stdlib, totp_rfc_vectors)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Base32 RFC 4648 vector: "foobar" -> "MZXW6YTBOI". */
    char *b32 = eval_str(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  return totp._test.base32_encode('foobar') "
        "end)()");
    ASSERT_NE(b32, NULL);
    ASSERT_STREQ(b32, "MZXW6YTBOI");
    free(b32);

    /* Base32 round-trip on the 20-byte RFC 6238 key. */
    int rt = eval_int(
        "(function() "
        "  local t = require('hull.web.middleware.totp')._test "
        "  local s = '12345678901234567890' "
        "  return t.base32_decode(t.base32_encode(s)) == s and 1 or 0 "
        "end)()");
    ASSERT_EQ(rt, 1);

    /* RFC 6238 Appendix B, SHA-1, 8-digit, key='12345678901234567890':
     *   T=59         step=1        -> 94287082
     *   T=1111111109 step=37037036 -> 07081804
     *   T=1234567890 step=41152263 -> 89005924
     *   T=2000000000 step=66666666 -> 69279037
     * The HMAC-SHA1 path comes through the hl_crypto_hmac_backend vtable,
     * so this also pins the HMAC integration. */
    char *v1 = eval_str(
        "require('hull.web.middleware.totp')._test"
        ".totp_at_step('12345678901234567890', 1, 8)");
    ASSERT_NE(v1, NULL); ASSERT_STREQ(v1, "94287082"); free(v1);

    char *v2 = eval_str(
        "require('hull.web.middleware.totp')._test"
        ".totp_at_step('12345678901234567890', 37037036, 8)");
    ASSERT_NE(v2, NULL); ASSERT_STREQ(v2, "07081804"); free(v2);

    char *v3 = eval_str(
        "require('hull.web.middleware.totp')._test"
        ".totp_at_step('12345678901234567890', 41152263, 8)");
    ASSERT_NE(v3, NULL); ASSERT_STREQ(v3, "89005924"); free(v3);

    char *v4 = eval_str(
        "require('hull.web.middleware.totp')._test"
        ".totp_at_step('12345678901234567890', 66666666, 8)");
    ASSERT_NE(v4, NULL); ASSERT_STREQ(v4, "69279037"); free(v4);

    /* 6-digit truncation of the same step uses the same dynamic offset;
     * value should match the last 6 digits of the 8-digit form (mod 10^6). */
    char *v1_6 = eval_str(
        "require('hull.web.middleware.totp')._test"
        ".totp_at_step('12345678901234567890', 1, 6)");
    ASSERT_NE(v1_6, NULL); ASSERT_STREQ(v1_6, "287082"); free(v1_6);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_enroll_confirm_verify)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* The end-to-end happy path: init -> enroll -> confirm with a code
     * we generate the same way the verify-side generates -> verify
     * with the next step's code.
     *
     * Code generation uses totp._test.totp_at_step against the secret
     * the enroll returned (decoded from base32), so the test mirrors
     * what a real authenticator app would do without needing one
     * present. */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp._test.reset() "
        "  totp.init({ issuer = 'TestApp' }) "
        "  local r = totp.enroll('user-1') "
        "  if type(r.secret_base32) ~= 'string' then return 0 end "
        "  if not r.qr_svg:find('<svg', 1, true) then return 0 end "
        "  if #r.recovery_codes ~= 10 then return 0 end "
        "  if not r.otpauth_url:find('otpauth://totp/TestApp:user%-1') then return 0 end "
        "  local secret = totp._test.base32_decode(r.secret_base32) "
        "  local step = totp._test.current_step() "
        "  local code = totp._test.totp_at_step(secret, step, 6) "
        "  if not totp.confirm('user-1', code) then return 0 end "
        "  if not totp.enrolled('user-1') then return 0 end "
        "  if totp.verify('user-1', code) then return 0 end "
        "  local next_code = totp._test.totp_at_step(secret, step + 1, 6) "
        "  local v_ok, kind = totp.verify_with_kind('user-1', next_code) "
        "  if not v_ok or kind ~= 'totp' then return 0 end "
        "  if totp.verify('user-1', next_code) then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_pending_cleanup)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Round-8 LOW-13: cleanup() prunes orphaned pending rows older
     * than pending_ttl. Confirmed _hull_totp rows are never touched. */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp._test.reset() "
        "  totp.init({ pending_ttl = 60, cleanup = false, window = 10, "
        "               recovery_codes = 0 }) "
        "  local r = totp.enroll('u-fresh') "
        "  totp.enroll('u-stale') "
        "  totp._test.force_pending_stale('u-stale') "
        "  if totp.cleanup() ~= 1 then return 0 end "
        "  if totp.cleanup() ~= 0 then return 0 end "
        "  local secret = totp._test.base32_decode(r.secret_base32) "
        "  local step = totp._test.current_step() "
        "  local good = totp._test.totp_at_step(secret, step, 6) "
        "  if not totp.confirm('u-fresh', good) then return 0 end "
        "  if totp.confirm('u-stale', good) then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_brute_force_lockout)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Round-8 HIGH-3: brute-force lockout baked into the module.
     * After max_failed_attempts consecutive wrong codes the user is
     * locked for lockout_duration seconds; verify returns false
     * silently during the window. Successful TOTP verify clears the
     * counter. Apps that want UX can read totp.lockout_remaining. */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp._test.reset() "
        /* recovery_codes = 0: each failed verify would otherwise
         * walk all stored recovery codes with PBKDF2 (10×~1s under
         * MSan). Drop them; the lockout path doesn't care. */
        "  totp.init({ max_failed_attempts = 3, lockout_duration = 60, "
        "               window = 10, recovery_codes = 0 }) "
        "  local r = totp.enroll('u1') "
        "  local secret = totp._test.base32_decode(r.secret_base32) "
        "  local step = totp._test.current_step() "
        "  if not totp.confirm('u1', totp._test.totp_at_step(secret, step, 6)) "
        "    then return 0 end "
        "  if totp.verify('u1', '000000') then return 0 end "
        "  if totp.verify('u1', '000001') then return 0 end "
        "  if totp.lockout_remaining('u1') ~= 0 then return 0 end "
        "  if totp.verify('u1', '000002') then return 0 end "
        "  local remain = totp.lockout_remaining('u1') "
        "  if remain <= 0 or remain > 60 then return 0 end "
        "  local good = totp._test.totp_at_step(secret, step + 2, 6) "
        "  if totp.verify('u1', good) then return 0 end "
        "  totp._test.clear_failed_attempts('u1') "
        "  if totp.lockout_remaining('u1') ~= 0 then return 0 end "
        "  if totp.verify('u1', '000000') then return 0 end "
        "  if totp.verify('u1', '000001') then return 0 end "
        "  local good2 = totp._test.totp_at_step(secret, step + 3, 6) "
        "  if not totp.verify('u1', good2) then return 0 end "
        "  if totp.verify('u1', '000000') then return 0 end "
        "  if totp.verify('u1', '000001') then return 0 end "
        "  if totp.lockout_remaining('u1') ~= 0 then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_recovery_code_single_use)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Recovery code: first use accepts + returns kind='recovery';
     * second use of the same code rejects (used_at flag). */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp._test.reset() "
        "  totp.init({ issuer = 'TestApp', recovery_codes = 3 }) "
        "  local r = totp.enroll('user-2') "
        "  local secret = totp._test.base32_decode(r.secret_base32) "
        "  local code = totp._test.totp_at_step(secret, "
        "    totp._test.current_step(), 6) "
        "  if not totp.confirm('user-2', code) then return 0 end "
        "  local rc = r.recovery_codes[1] "
        "  local v_ok, kind = totp.verify_with_kind('user-2', rc) "
        "  if not v_ok or kind ~= 'recovery' then return 0 end "
        "  if totp.verify('user-2', rc) then return 0 end "
        "  local v2_ok, k2 = totp.verify_with_kind('user-2', r.recovery_codes[2]) "
        "  if not v2_ok or k2 ~= 'recovery' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_disable_clears_secret_and_recovery)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp._test.reset() "
        "  totp.init({ issuer = 'TestApp' }) "
        "  totp.enroll('user-3') "
        "  if not totp.disable('user-3') then return 0 end "
        "  if totp.enrolled('user-3') then return 0 end "
        "  if totp.disable('user-3') then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_ct_eq_and_normalize)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* ct_eq: constant-time string compare. Pinning equal / unequal /
     * different-length / non-string inputs. The "constant-time" part
     * isn't directly testable from Lua, but the functional contract
     * (returns boolean, no early exit) is. */
    int ok = eval_int(
        "(function() "
        "  local t = require('hull.web.middleware.totp')._test "
        "  if not t.ct_eq('287082', '287082') then return 0 end "
        "  if t.ct_eq('287082', '287083') then return 0 end "
        "  if t.ct_eq('287082', '2870820') then return 0 end "
        "  if t.ct_eq('', '') then return 1 end "
        "  return 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    int safe = eval_int(
        "(function() "
        "  local t = require('hull.web.middleware.totp')._test "
        "  if t.ct_eq(nil, 'x') then return 0 end "
        "  if t.ct_eq('x', nil) then return 0 end "
        "  if t.ct_eq(42, 42) then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(safe, 1);

    /* normalize_recovery_code: strip non-alphanumerics, uppercase. */
    char *n1 = eval_str(
        "require('hull.web.middleware.totp')._test"
        ".normalize_recovery_code('ABCD-EFGH-IJKL')");
    ASSERT_NE(n1, NULL); ASSERT_STREQ(n1, "ABCDEFGHIJKL"); free(n1);

    char *n2 = eval_str(
        "require('hull.web.middleware.totp')._test"
        ".normalize_recovery_code('abcdefghijkl')");
    ASSERT_NE(n2, NULL); ASSERT_STREQ(n2, "ABCDEFGHIJKL"); free(n2);

    char *n3 = eval_str(
        "require('hull.web.middleware.totp')._test"
        ".normalize_recovery_code('  abcd efgh ijkl  ')");
    ASSERT_NE(n3, NULL); ASSERT_STREQ(n3, "ABCDEFGHIJKL"); free(n3);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_recovery_accepts_user_typed_forms)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* The DB-backed end-to-end: a recovery code displayed as
     * "ABCD-EFGH-IJKL" should also verify when the user types it
     * without the hyphens, in lowercase, or with stray whitespace.
     * Reuses the enroll → confirm path to bring a user to the state
     * where verify is allowed. */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp._test.reset() "
        "  totp.init({ issuer = 'TestApp', recovery_codes = 4 }) "
        "  local r = totp.enroll('rec-user') "
        "  local secret = totp._test.base32_decode(r.secret_base32) "
        "  local code = totp._test.totp_at_step(secret, "
        "    totp._test.current_step(), 6) "
        "  if not totp.confirm('rec-user', code) then return 0 end "
        "  local rc = r.recovery_codes[1] "
        "  local plain = rc:gsub('-', '') "
        "  local lower = plain:lower() "
        "  local spaced = '  ' .. plain .. '  ' "
        "  local v_plain = totp.verify('rec-user', plain) "
        "  if not v_plain then return 0 end "
        "  local v_lower = totp.verify('rec-user', "
        "    r.recovery_codes[2]:lower()) "
        "  if not v_lower then return 0 end "
        "  local v_spaced = totp.verify('rec-user', "
        "    '  ' .. r.recovery_codes[3] .. '  ') "
        "  if not v_spaced then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_encryption_at_rest)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* With encryption_key set, the on-disk secret is a secretbox
     * blob - round-trip via load_secret should still yield the
     * plaintext, but raw row inspection should NOT show the original
     * 20 bytes. The blob also has to be longer than 20 bytes
     * (nonce=24 + MAC=16 = 40 extra bytes added). */
    int ok = eval_int(
        /* End-to-end round-trip with encryption on: enroll stores
         * the secret encrypted, confirm + verify go through the
         * decrypt path. Direct row inspection (to assert the blob
         * is NOT the plaintext) is blocked by Hull's _hull_* table
         * access guard from user code, so we exercise the
         * encrypt+decrypt invariant via the public API: if enroll
         * silently dropped the key OR decrypt failed, neither
         * confirm nor verify could succeed. Pairs with the unit
         * test of the encrypt/decrypt helpers below. */
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp._test.reset() "
        "  totp.init({ issuer = 'TestApp', "
        "              encryption_key = ('k'):rep(32) }) "
        "  local r = totp.enroll('user-4') "
        "  local secret = totp._test.base32_decode(r.secret_base32) "
        "  local step = totp._test.current_step() "
        "  local code = totp._test.totp_at_step(secret, step, 6) "
        "  if not totp.confirm('user-4', code) then return 0 end "
        "  local next_code = totp._test.totp_at_step(secret, step + 1, 6) "
        "  if not totp.verify('user-4', next_code) then return 0 end "
        "  local blob, enc_flag, version = totp._test.encrypt_secret(secret) "
        "  if enc_flag ~= 1 or version ~= 1 then return 0 end "
        "  if #blob <= #secret then return 0 end "
        "  local pt, v = totp._test.decrypt_secret(blob, 1) "
        "  if pt ~= secret or v ~= 1 then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_key_rotation_lazy_on_verify)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Two-key rotation: enroll under key v1, then init() with the
     * same key plus a new v2 + current=2. The on-disk blob is
     * still v1-encrypted; the next successful verify should re-
     * encrypt it under v2 (lazy rekey-on-verify). Proven by calling
     * rekey() after - if the lazy path worked, rekey reports
     * rekeyed=0 (everything already on current). */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp._test.reset() "
        "  local k1 = ('a'):rep(32) "
        "  local k2 = ('b'):rep(32) "
        "  totp.init({ encryption_keys = {[1]=k1}, current = 1 }) "
        "  local r = totp.enroll('u') "
        "  local secret = totp._test.base32_decode(r.secret_base32) "
        "  local step = totp._test.current_step() "
        "  if not totp.confirm('u', totp._test.totp_at_step(secret, step, 6)) "
        "    then return 2 end "
        "  local b1 = totp._test.get_blob('u') "
        "  if not b1 then return 50 end "
        "  local v1 = (string.byte(b1,1) << 24) | (string.byte(b1,2) << 16) "
        "          | (string.byte(b1,3) << 8) | string.byte(b1,4) "
        "  if v1 ~= 1 then return 60 + v1 end "
        "  totp.init({ encryption_keys = {[1]=k1, [2]=k2}, current = 2 }) "
        "  if not totp.verify('u', "
        "       totp._test.totp_at_step(secret, step + 1, 6)) then return 3 end "
        "  local b2 = totp._test.get_blob('u') "
        "  if not b2 then return 70 end "
        "  local v2 = (string.byte(b2,1) << 24) | (string.byte(b2,2) << 16) "
        "          | (string.byte(b2,3) << 8) | string.byte(b2,4) "
        "  if v2 ~= 2 then return 80 + v2 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_rekey_batch_helper)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Enroll three users under v1, rotate to v2, call totp.rekey().
     * Expect scanned=3, rekeyed=3, failed=0. Second rekey() reports
     * scanned=3, rekeyed=0, failed=0 (all already on current). */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp._test.reset() "
        "  local k1 = ('a'):rep(32) "
        "  local k2 = ('b'):rep(32) "
        "  totp.init({ encryption_keys = {[1]=k1}, current = 1 }) "
        "  for i = 1, 3 do "
        "    local r = totp.enroll('u' .. i) "
        "    local secret = totp._test.base32_decode(r.secret_base32) "
        "    local code = totp._test.totp_at_step(secret, "
        "      totp._test.current_step(), 6) "
        "    if not totp.confirm('u' .. i, code) then return 0 end "
        "  end "
        "  totp.init({ encryption_keys = {[1]=k1, [2]=k2}, current = 2 }) "
        "  local r1 = totp.rekey() "
        "  if r1.scanned ~= 3 or r1.rekeyed ~= 3 or r1.failed ~= 0 then return 0 end "
        "  local r2 = totp.rekey() "
        "  if r2.scanned ~= 3 or r2.rekeyed ~= 0 or r2.failed ~= 0 then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* hull.encoding's C fast path (hull.encoding._native) against its pure-Lua
 * codecs. A vanilla state loads the module twice - once with no native module
 * (pure), once with it preloaded (fast) - and both must give the same answer,
 * reasons included, on every input: the fast path is only sound if the C
 * decoders accept exactly what the Lua ones accept. The script returns 0, or
 * the number of the first check that failed. */
int luaopen_hull_encoding_native(lua_State *L);

UTEST(lua_stdlib, encoding_native_matches_pure)
{
    lua_State *L = luaL_newstate();
    ASSERT_TRUE(L != NULL);
    luaL_openlibs(L);
    int rc = luaL_dostring(L,
        "package.path = 'stdlib/lua/?.lua;stdlib/lua/?/init.lua;' .. package.path");
    ASSERT_EQ(rc, LUA_OK);

    rc = luaL_dostring(L,
        "local pure = require('hull.encoding') "
        "package.loaded['hull.encoding'] = nil "
        "_G.PURE = pure");
    ASSERT_EQ(rc, LUA_OK);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_hull_encoding_native);
    lua_setfield(L, -2, "hull.encoding._native");
    lua_pop(L, 2);

    rc = luaL_dostring(L,
        "return (function() "
        "  local pure, fast = PURE, require('hull.encoding') "
        "  if pure == fast then return 1 end "
        "  if type(package.loaded['hull.encoding._native']) ~= 'table' then return 1 end "
        "  math.randomseed(7) "
        "  local A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/-_=aF09 \\n' "
        "  local function text(n, alpha) "
        "    local t = {} "
        "    for i = 1, n do "
        "      if alpha then local k = math.random(#A); t[i] = A:sub(k, k) "
        "      else t[i] = string.char(math.random(0, 255)) end "
        "    end "
        "    return table.concat(t) "
        "  end "
        "  local accepted = 0 "
        "  for i = 1, 20000 do "
        "    local s = text(math.random(0, 24), i % 3 ~= 0) "
        "    if math.random(4) == 1 then s = s .. string.rep('=', math.random(0, 2)) end "
        "    local a1, a2 = pure.hex.decode(s) "
        "    local b1, b2 = fast.hex.decode(s) "
        "    if a1 ~= b1 or a2 ~= b2 then return 2 end "
        "    if pure.hex.encode(s) ~= fast.hex.encode(s) then return 3 end "
        "    for _, url in ipairs({ false, true }) do "
        "      a1, a2 = pure.base64.decode(s, { url = url }) "
        "      b1, b2 = fast.base64.decode(s, { url = url }) "
        "      if a1 ~= b1 or a2 ~= b2 then return 4 end "
        "      if b1 then accepted = accepted + 1 end "
        "      for _, pad in ipairs({ false, true }) do "
        "        local o = { url = url, pad = pad } "
        "        if pure.base64.encode(s, o) ~= fast.base64.encode(s, o) then return 5 end "
        "      end "
        "      local o = { url = url } "
        "      if pure.base64.encode(s, o) ~= fast.base64.encode(s, o) then return 6 end "
        "    end "
        "  end "
        /* the comparison is only worth something if decoders did accept */
        "  if accepted < 1000 then return 7 end "
        /* lenient still goes the pure way and still works */
        "  if fast.base64.decode('Zm9v\\nYmFy', { lenient = true }) ~= 'foobar' then return 8 end "
        "  return 0 "
        "end)()");
    if (rc != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    ASSERT_EQ(rc, LUA_OK);
    EXPECT_EQ(lua_tointeger(L, -1), 0);
    lua_close(L);
}

/* crypto.random_token: the shape of each format, the bounds, and that two
 * calls differ. Returns 0, or the number of the first check that failed. */
UTEST(lua_stdlib, crypto_random_token)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  local crypto = require('hull.crypto') "
        "  local t = crypto.random_token(16) "
        "  if #t ~= 22 or t:find('[^%w_%-]') then return 1 end "
        "  if crypto.random_token(16) == t then return 2 end "
        "  local h = crypto.random_token(32, 'hex') "
        "  if #h ~= 64 or h:find('[^0-9a-f]') then return 3 end "
        "  if #crypto.random_token(1) ~= 2 or #crypto.random_token(1024) ~= 1366 then return 4 end "
        "  if pcall(crypto.random_token, 0) then return 5 end "
        "  if pcall(crypto.random_token, 1025) then return 6 end "
        "  if pcall(crypto.random_token, 16, 'base32') then return 7 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(step, 0);
    cleanup_lua_caps();
}

/* A value of a few MB through hull.encoding inside the 64 MB Lua heap. The
 * pure codecs build one table slot per byte (hex) or group (base64) and ran
 * out of heap around 3 MB; the C fast path writes one buffer. */
UTEST(lua_stdlib, encoding_large_value_fits_the_heap)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  local enc = require('hull.encoding') "
        "  local v = string.rep('\\1\\2\\3\\250', 1024 * 1024) "
        "  local b = enc.base64.encode(v) "
        "  if #b ~= (#v + 2) // 3 * 4 then return 1 end "
        "  if enc.base64.decode(b) ~= v then return 2 end "
        "  local h = enc.hex.encode(v) "
        "  if #h ~= 2 * #v then return 3 end "
        "  if enc.hex.decode(h) ~= v then return 4 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(step, 0);
    cleanup_lua_caps();
}

/* hull.crypto.sealbox + encrypted hull.kv. Each chunk returns 0 when every
 * check passes, else the number of the first check that failed. */
/* Regressions from docs/crypto_encoding_ssh_audit.md (PR 1). Returns 0, or
 * the number of the first check that failed. */
UTEST(lua_stdlib, crypto_encoding_audit_fixes)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  local jwt = require('hull.jwt') "
        "  local kv = require('hull.kv') "
        "  local cache = require('hull.cache') "
        "  local sb = require('hull.crypto.sealbox') "
        "  local otp = require('hull.crypto.otp') "
        "  local function code(f) local ok, e = pcall(f) "
        "    if ok then return nil end return type(e) == 'table' and e.code or tostring(e) end "
        /* 1-3: a malformed token is a clean reject, never a raise */
        "  local ok, p, err = pcall(jwt.verify, 'ew.e30.AA', 'secret') "
        "  if not ok or p ~= nil or err ~= 'invalid header JSON' then return 1 end "
        "  if not pcall(jwt.verify, 'NQ.NQ.AA', 'secret') then return 2 end "
        "  if jwt.decode('eyJhbGciOiJIUzI1NiJ9.NQ.AA') ~= nil then return 3 end "
        /* 4-6: rekey keeps each value's expiry */
        "  local K1, K2 = ('a'):rep(32), ('b'):rep(32) "
        "  local h1 = kv.open{ namespace = 'ttl', encrypt = { keys = {[1] = K1}, current = 1 } } "
        "  h1:set('t', 'v', { ttl = 3600 }) "
        "  h1:set('p', 'v') "
        "  local st = h1._s "
        "  local before = st.data['t'].exp "
        "  if not before then return 4 end "
        "  local h2 = kv.open{ namespace = 'ttl', encrypt = { keys = {[1] = K1, [2] = K2}, current = 2 } } "
        "  if h2:rekey() ~= 2 then return 5 end "
        "  if st.data['t'].exp ~= before or st.data['p'].exp ~= nil then return 6 end "
        /* 7-9: keyring ids in canonical decimal, and nothing else */
        "  local r = sb.keyring{ keys = { ['1'] = K1 }, current = '1' } "
        "  if r.current ~= 1 or not r.keys[1] then return 7 end "
        "  if pcall(sb.keyring, { keys = { [' 1'] = K1 }, current = 1 }) then return 8 end "
        "  if pcall(sb.keyring, { keys = { ['01'] = K1 }, current = 1 }) then return 9 end "
        /* 10: cache refuses encrypt instead of storing plaintext */
        "  if code(function() cache.open{ encrypt = { keys = {[1] = K1}, current = 1 } } end) "
        "     ~= 'invalid_argument' then return 10 end "
        /* 11-12: sealbox checks its arguments */
        "  if pcall(sb.seal, r, 42) then return 11 end "
        "  if pcall(sb.seal, r, 'v', 'not-an-array') then return 12 end "
        /* 13: otp.step refuses a zero period */
        "  if pcall(otp.step, 60, 0) then return 13 end "
        "  return 0 "
        "end)()");
    ASSERT_EQ(step, 0);
    cleanup_lua_caps();
}

/* HKDF-SHA256 against RFC 5869 appendix A, test cases 1-3 (3 is the empty
 * salt and info case), through the real crypto.hmac_sha256. Returns 0, or the
 * number of the first check that failed. */
UTEST(lua_stdlib, hkdf_rfc5869_vectors)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  local hkdf = require('hull.crypto.hkdf') "
        "  local enc = require('hull.encoding') "
        "  local hex = enc.hex.encode "
        "  local function range(a, b) local t = {} for i = a, b do t[#t + 1] = string.char(i) end "
        "    return table.concat(t) end "
        "  local ikm = string.rep('\\11', 22) "
        "  if hex(hkdf.extract(range(0, 12), ikm)) ~= '077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5' then return 1 end "
        "  if hex(hkdf.derive(ikm, 42, { salt = range(0, 12), info = range(0xf0, 0xf9) })) "
        "     ~= '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865' then return 2 end "
        "  if hex(hkdf.derive(range(0, 0x4f), 82, { salt = range(0x60, 0xaf), "
        "        info = range(0xb0, 0xff) })) "
        "     ~= 'b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87' then return 3 end "
        "  if hex(hkdf.derive(ikm, 42)) ~= '8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8' then return 4 end "
        "  if pcall(hkdf.derive, ikm, 0) or pcall(hkdf.derive, ikm, 8161) then return 5 end "
        "  if #hkdf.derive(ikm, 8160) ~= 8160 then return 6 end "
        /* two labels, two unrelated keys */
        "  if hkdf.derive(ikm, 32, { info = 'enc' }) == hkdf.derive(ikm, 32, { info = 'mac' }) "
        "     then return 7 end "
        "  if pcall(hkdf.expand, 'short', '', 32) then return 8 end "
        "  return 0 "
        "end)()");
    ASSERT_EQ(step, 0);
    cleanup_lua_caps();
}

/* A key held in C (crypto.key_from_env), end to end through sealbox, kv and
 * the env allowlist. HULL_TEST_VAR is the harness's allowlisted variable; it
 * holds 32 bytes 0x00..0x1f as hex. Returns 0, or the first failed check. */
UTEST(lua_stdlib, crypto_key_from_env)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    setenv("HULL_TEST_VAR", "not a key", 1);
    int bad = eval_int(
        "(function() "
        "  local ok = pcall(require('hull.crypto').key_from_env, 'HULL_TEST_VAR') "
        "  return ok and 1 or 0 "
        "end)()");
    ASSERT_EQ(bad, 0);

    setenv("HULL_TEST_VAR", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n", 1);
    int step = eval_int(
        "(function() "
        "  local crypto = require('hull.crypto') "
        "  local sb = require('hull.crypto.sealbox') "
        "  local kv = require('hull.kv') "
        "  local raw = '' for i = 0, 31 do raw = raw .. string.char(i) end "
        "  local k = crypto.key_from_env('HULL_TEST_VAR') "
        /* 1-2: the handle names its variable and hides everything else */
        "  if tostring(k) ~= 'crypto.key(HULL_TEST_VAR)' then return 1 end "
        "  if getmetatable(k) ~= 'crypto.key' then return 2 end "
        /* 3-4: the held key IS those 32 bytes, both ways round */
        "  local rh = sb.keyring{ keys = { [1] = k }, current = 1 } "
        "  local rr = sb.keyring{ keys = { [1] = raw }, current = 1 } "
        "  if sb.open(rr, sb.seal(rh, 'v', { 'ctx' }), { 'ctx' }) ~= 'v' then return 3 end "
        "  if sb.open(rh, sb.seal(rr, 'w', { 'ctx' }), { 'ctx' }) ~= 'w' then return 4 end "
        /* 5: keyring_from_env */
        "  local re = sb.keyring_from_env{ keys = { [1] = 'HULL_TEST_VAR' }, current = 1 } "
        "  if sb.open(re, sb.seal(rr, 'x')) ~= 'x' then return 5 end "
        /* 6: an encrypted kv handle with a held key */
        "  local h = kv.open{ namespace = 'held', encrypt = { keys = { [1] = k }, current = 1 } } "
        "  h:set('a', 'secret') "
        "  if h:get('a') ~= 'secret' then return 6 end "
        /* 7: a variable outside manifest.env is refused */
        "  if pcall(crypto.key_from_env, 'PATH') then return 7 end "
        /* 8-9: destroy zeroes it now, and the handle stops working */
        "  local k2 = crypto.key_from_env('HULL_TEST_VAR') "
        "  k2:destroy() "
        "  if pcall(k2.secretbox, k2, 'x', ('n'):rep(24)) then return 8 end "
        "  if not tostring(k2):find('destroyed', 1, true) then return 9 end "
        /* 10: a wrong nonce length is refused, not truncated */
        "  if pcall(k.secretbox, k, 'x', 'short') then return 10 end "
        "  return 0 "
        "end)()");
    unsetenv("HULL_TEST_VAR");
    ASSERT_EQ(step, 0);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, otp_rfc4226_vectors)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    /* RFC 4226 Appendix D: the 20-byte ASCII key, counters 0..9. */
    int step = eval_int(
        "(function() "
        "  local otp = require('hull.crypto.otp') "
        "  local want = { '755224', '287082', '359152', '969429', '338314', "
        "                 '254676', '287922', '162583', '399871', '520489' } "
        "  for i, w in ipairs(want) do "
        "    if otp.hotp('12345678901234567890', i - 1) ~= w then return i end "
        "  end "
        "  if otp.hotp('12345678901234567890', 1, 8) ~= '94287082' then return 11 end "
        "  if pcall(otp.hotp, 'k', -1) or pcall(otp.hotp, 'k', 1.5) then return 12 end "
        "  if pcall(otp.hotp, 'k', 1, 9) then return 13 end "
        "  if otp.step(59, 30) ~= 1 or otp.step(60.0, 30) ~= 2 then return 14 end "
        "  if otp.hotp('12345678901234567890', otp.step(20000000000, 30), 8) ~= '65353130' then return 15 end "
        "  return 0 "
        "end)()");
    ASSERT_EQ(step, 0);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, sealbox_seal_open)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  local sb = require('hull.crypto.sealbox') "
        "  if pcall(sb.keyring, { keys = {[1] = 'short'}, current = 1 }) then return 1 end "
        "  if pcall(sb.keyring, { keys = {[1] = ('a'):rep(32)}, current = 2 }) then return 2 end "
        "  local r1 = sb.keyring{ keys = {[1] = ('a'):rep(32)}, current = 1 } "
        "  local v = '\\0\\1\\254\\255 secret' "
        "  local b = sb.seal(r1, v, { 'ns', 'key' }) "
        "  local o, ver = sb.open(r1, b, { 'ns', 'key' }) "
        "  if o ~= v or ver ~= 1 then return 3 end "
        "  if #b ~= sb.MIN_LEN + 4 + 2 + 4 + 3 + #v then return 4 end "
        "  local t = b:sub(1, 30) .. string.char(b:byte(31) ~ 1) .. b:sub(32) "
        "  if select(2, sb.open(r1, t, { 'ns', 'key' })) ~= 'open_failed' then return 5 end "
        "  if select(2, sb.open(r1, b, { 'ns', 'other' })) ~= 'open_failed' then return 6 end "
        "  if select(2, sb.open(r1, b, { 'n', 'skey' })) ~= 'open_failed' then return 7 end "
        "  if sb.open(r1, b) == v then return 8 end "
        "  if select(2, sb.open(r1, 'short')) ~= 'open_failed' then return 9 end "
        "  local r3 = sb.keyring{ keys = {[3] = ('c'):rep(32)}, current = 3 } "
        "  if select(2, sb.open(r3, b, { 'ns', 'key' })) ~= 'unknown_version' then return 10 end "
        "  local r12 = sb.keyring{ keys = {[1] = ('a'):rep(32), [2] = ('b'):rep(32)}, current = 2 } "
        "  o, ver = sb.open(r12, b, { 'ns', 'key' }) "
        "  if o ~= v or ver ~= 1 then return 11 end "
        "  o, ver = sb.open(r12, sb.seal(r12, v)) "
        "  if o ~= v or ver ~= 2 then return 12 end "
        "  return 0 "
        "end)()");
    ASSERT_EQ(step, 0);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, kv_encrypted_handle)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  local kv = require('hull.kv') "
        "  local function code(f) local ok, e = pcall(f) "
        "    if ok then return nil end return type(e) == 'table' and e.code or tostring(e) end "
        "  local K1, K2 = ('a'):rep(32), ('b'):rep(32) "
        "  local enc = kv.open{ namespace = 'sec', encrypt = { keys = {[1] = K1}, current = 1 } } "
        "  local raw = kv.open{ namespace = 'sec' } "
        "  local v = '\\0\\255 pass' "
        "  enc:set('k', v) "
        "  if enc:get('k') ~= v then return 1 end "
        "  if enc:get('miss') ~= nil then return 2 end "
        "  local stored = raw:get('k') "
        "  if stored == v or stored:find('pass', 1, true) then return 3 end "
        "  raw:set('moved', stored) "
        "  if code(function() enc:get('moved') end) ~= 'decrypt_failed' then return 4 end "
        "  kv.open{ namespace = 'other' }:set('k', stored) "
        "  local enc_other = kv.open{ namespace = 'other', encrypt = { keys = {[1] = K1}, current = 1 } } "
        "  if code(function() enc_other:get('k') end) ~= 'decrypt_failed' then return 5 end "
        "  raw:set('planted', 'plain') "
        "  if code(function() enc:get('planted') end) ~= 'decrypt_failed' then return 6 end "
        "  if code(function() enc:incr('n', 1) end) ~= 'unsupported' then return 7 end "
        "  if not enc:cas('c', nil, 'a') or enc:cas('c', nil, 'b') then return 8 end "
        "  if enc:cas('c', 'x', 'b') or not enc:cas('c', 'a', 'b') or enc:get('c') ~= 'b' then return 9 end "
        "  raw:delete('moved'); raw:delete('planted') "
        "  local both = kv.open{ namespace = 'sec', encrypt = { keys = {[1] = K1, [2] = K2}, current = 2 } } "
        "  if both:get('k') ~= v then return 10 end "
        "  if both:rekey() ~= 2 or both:rekey() ~= 0 then return 11 end "
        "  local only2 = kv.open{ namespace = 'sec', encrypt = { keys = {[2] = K2}, current = 2 } } "
        "  if only2:get('k') ~= v or only2:get('c') ~= 'b' then return 12 end "
        "  if code(function() enc:get('k') end) ~= 'decrypt_failed' then return 13 end "
        "  raw:set('old', 'legacy') "
        "  local mig = kv.open{ namespace = 'sec', encrypt = { keys = {[2] = K2}, current = 2, allow_plaintext = true } } "
        "  if mig:get('old') ~= 'legacy' then return 14 end "
        "  if mig:rekey() ~= 1 or only2:get('old') ~= 'legacy' then return 15 end "
        "  if code(function() raw:rekey() end) ~= 'invalid_argument' then return 16 end "
        "  if code(function() kv.open{ encrypt = 'x' } end) ~= 'invalid_argument' then return 17 end "
        "  if code(function() kv.open{ encrypt = { keys = {[1] = 'short'}, current = 1 } } end) ~= 'invalid_argument' then return 18 end "
        "  return 0 "
        "end)()");
    ASSERT_EQ(step, 0);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_legacy_v1_format_decrypts_via_legacy_key_version)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Simulate a pre-versioning row: encrypt_secret without a
     * version prefix is what the OLD code would have written.
     * After init with legacy_key_version pointing at the same key,
     * decrypt_secret recovers it. */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  local crypto = require('hull.crypto') "
        "  totp._test.reset() "
        "  local k1 = ('a'):rep(32) "
        "  totp.init({ encryption_keys = {[1]=k1}, current = 1, "
        "              legacy_key_version = 1 }) "
        "  local secret = string.rep('S', 20) "
        "  local nonce = crypto.random(24) "
        "  local blob = nonce .. crypto.secretbox(secret, nonce, k1) "
        "  local pt, version = totp._test.decrypt_secret(blob, 1) "
        "  if pt ~= secret then return 0 end "
        "  if version ~= 0 then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, totp_unknown_key_version_fails_clean)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* A blob with version=99 (not in encryption_keys map) and no
     * legacy_key_version configured should return nil cleanly. */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  local crypto = require('hull.crypto') "
        "  totp._test.reset() "
        "  local k1 = ('a'):rep(32) "
        "  totp.init({ encryption_keys = {[1]=k1}, current = 1 }) "
        "  local blob = string.char(0,0,0,99) .. string.rep('x', 24+36) "
        "  local pt = totp._test.decrypt_secret(blob, 1) "
        "  if pt ~= nil then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* ── hull.web.auth-flows tests ─────────────────────────────────────────── */

/* Lua tests for auth-flows share an init-helper string because each
 * test rebuilds the in-memory user store + init() call from scratch.
 * Kept under the 16 KiB eval buffer limit by skipping doc comments
 * and inlining only what each test needs. */
#define AF_INIT_LUA \
"local af = require('hull.web.auth-flows') " \
"af._test.reset() " \
"_G._users = {} _G._by_id = {} _G._sent = {} " \
"af.init({ " \
"  state_secret = ('k'):rep(32), " \
"  trust_request_host = true, " \
"  email_send = function(to, sub, html, text) " \
"    _G._sent[#_G._sent+1] = {to=to, sub=sub, html=html, text=text} " \
"  end, " \
"  templates = { " \
"    welcome = function(c) return {subject='w',text='link:'..c.verify_url} end, " \
"    verify = function(c) return {subject='v',text='x'} end, " \
"    magic_link = function(c) return {subject='m',text='link:'..c.link} end, " \
"    password_reset = function(c) return {subject='p',text='link:'..c.link} end, " \
"    email_change = function(c) return {subject='e',text='link:'..c.link} end, " \
"  }, " \
"  user_find_by_email = function(e) return _G._users[e] end, " \
"  user_get = function(id) return _G._by_id[id] end, " \
"  user_create = function(e, ph) " \
"    local id = 'u'..tostring(1 + select(2, next(_G._by_id) and #_G._by_id or 0)) " \
"    local i = 0; for _ in pairs(_G._by_id) do i = i + 1 end; id = 'u'..(i+1) " \
"    local u = {id=id, email=e, password_hash=ph, email_verified=false} " \
"    _G._users[e] = u; _G._by_id[id] = u; return id " \
"  end, " \
"  user_set_password = function(id, ph) _G._by_id[id].password_hash = ph end, " \
"  user_set_email = function(id, ne) " \
"    local u = _G._by_id[id] _G._users[u.email] = nil " \
"    u.email = ne _G._users[ne] = u " \
"  end, " \
"  user_set_email_verified = function(id, v) _G._by_id[id].email_verified = v end, " \
"  on_login = function(req, res, user) " \
"    _G._last_login = user.id; res:json({ok=true,id=user.id}) " \
"  end, " \
"}) "

UTEST(lua_stdlib, crypto_envelope_round_trip)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* sign + verify on a known payload, with a 64-hex secret. */
    int ok = eval_int(
        "(function() "
        "  local env = require('hull.crypto.envelope') "
        "  local secret = ('aa'):rep(32) "  /* 32 bytes hex = 64 chars */
        "  local tok = env.sign({sub='u1',action='verify',exp=99}, secret) "
        "  if type(tok) ~= 'string' then return 0 end "
        "  if not tok:find('.', 1, true) then return 0 end "
        "  local p, err = env.verify(tok, secret) "
        "  if not p or err then return 0 end "
        "  if p.sub ~= 'u1' or p.action ~= 'verify' or p.exp ~= 99 then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);
}

UTEST(lua_stdlib, crypto_envelope_failure_modes)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Each failure mode reports its vague reason. The TOTP-pending
     * flow in auth-flows depends on these being distinct strings. */
    int ok = eval_int(
        "(function() "
        "  local env = require('hull.crypto.envelope') "
        "  local secret = ('bb'):rep(32) "
        "  local _, e1 = env.verify('', secret) "
        "  if e1 ~= 'missing' then return 0 end "
        "  local _, e2 = env.verify('no-dot-here', secret) "
        "  if e2 ~= 'malformed' then return 0 end "
        "  local tok = env.sign({x=1}, secret) "
        "  local tampered = tok:sub(1, -3) .. 'zz' "
        "  local _, e3 = env.verify(tampered, secret) "
        "  if e3 ~= 'bad tag' then return 0 end "
        "  local _, e4 = env.verify('body.junkhex', secret) "
        "  if e4 ~= 'bad tag' then return 0 end "
        "  local wrong = ('cc'):rep(32) "
        "  local _, e5 = env.verify(tok, wrong) "
        "  if e5 ~= 'bad tag' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);
}

/* A token's tag is lowercase hex, the form sign() writes. hex.decode takes
 * either case, so an upper-cased tag verified too - and a single-use token,
 * keyed on its exact text, was replayable once per case variant. */
UTEST(lua_stdlib, crypto_envelope_tag_is_lowercase_only)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int ok = eval_int(
        "(function() "
        "  local env = require('hull.crypto.envelope') "
        "  local secret = ('bb'):rep(32) "
        "  local tok = env.sign({x=1}, secret) "
        "  local dot = tok:find('.', 1, true) "
        "  local up = tok:sub(1, dot) .. tok:sub(dot + 1):upper() "
        "  if up == tok then return 2 end "
        "  local p, e = env.verify(up, secret) "
        "  if p or e ~= 'bad tag' then return 0 end "
        "  return env.verify(tok, secret) and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);
    cleanup_lua_caps();
}

/* Link origins: the Host header is parsed strictly and the URL is built from
 * the allowlist entry, never from the header. "app.example.com:@evil.com"
 * used to pass the allowlist and become the emailed link (token to evil.com).
 * X-Forwarded-Host / -Proto count only behind a trusted proxy. */
UTEST(lua_stdlib, auth_flows_origin_is_built_from_the_allowlist)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  local o = af._test.origin_for "
        "  local function req(h) return { headers = h } end "
        "  if o(req{host='app.example.com:8443'}) ~= 'http://app.example.com:8443' then return 1 end "
        "  if o(req{host='app.example.com:@evil.com'}) ~= nil then return 2 end "
        "  if o(req{host='evil.com/x'}) ~= nil then return 3 end "
        "  if o(req{host='app.example.com', ['x-forwarded-host']='evil.com'}) ~= 'http://app.example.com' then return 4 end "
        "  local st = af._test.state "
        "  st.trust_request_host = false; st.trusted_hosts = { 'app.example.com' } "
        "  if o(req{host='app.example.com:@evil.com'}) ~= nil then return 5 end "
        "  if o(req{host='app.example.com:8443'}) ~= 'https://app.example.com:8443' then return 6 end "
        "  if o(req{host='evil.com'}) ~= nil then return 7 end "
        "  if o(req{host='app.example.com:99999'}) ~= nil then return 8 end "
        "  st.trust_proxy = true "
        "  if o(req{host='x', ['x-forwarded-host']='app.example.com', ['x-forwarded-proto']='javascript'}) ~= 'https://app.example.com' then return 9 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(step, 0);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, auth_flows_token_round_trip)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  local A = af._test.ACTIONS "
        "  local tok = af._test.issue_token('u1', A.verify_email, 60) "
        "  local env, err = af._test.consume_token(tok, A.verify_email) "
        "  if not env or env.sub ~= 'u1' then return 0 end "
        "  local env2, err2 = af._test.consume_token(tok, A.verify_email) "
        "  if env2 or err2 ~= 'replayed' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    int rejections = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  local A = af._test.ACTIONS "
        "  local tok = af._test.issue_token('u1', A.verify_email, 60) "
        "  local _, err = af._test.consume_token(tok, A.password_reset) "
        "  if err ~= 'wrong action' then return 0 end "
        "  local tampered = tok:sub(1, -3) .. 'zz' "
        "  local _, err2 = af._test.consume_token(tampered, A.verify_email) "
        "  if err2 ~= 'bad tag' then return 0 end "
        "  local _, err3 = af._test.consume_token('garbage', A.verify_email) "
        "  if err3 ~= 'malformed' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(rejections, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, auth_flows_register_verify_login)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* End-to-end through the route handlers via the test runner is
     * possible but heavier than needed here; the smoke-test ran the
     * full HTTP path. This test exercises the token + storage
     * invariants directly. */
    int ok = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  local A = af._test.ACTIONS "
        "  af.send_verify_email("
        "    { id='u1', email='a@x.com' }, 'http://t.io') "
        "  if #_G._sent ~= 1 then return 0 end "
        "  local link = _G._sent[1].text "
        "  local tok = link:match('token=(.+)') "
        "  if not tok then return 0 end "
        "  local env, err = af._test.consume_token(tok, A.verify_email) "
        "  if not env or env.sub ~= 'u1' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, auth_flows_input_validation)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local af = require('hull.web.auth-flows') "
        "  af._test.reset() "
        "  local t = af._test "
        "  if not t.is_email_ish('a@b.co') then return 0 end "
        "  if t.is_email_ish('') then return 0 end "
        "  if t.is_email_ish('no-at-sign') then return 0 end "
        "  if t.is_email_ish('@leading') then return 0 end "
        "  if t.is_email_ish('trailing@') then return 0 end "
        "  if t.is_email_ish('a@b') then return 0 end "
        "  if t.is_email_ish('a@b.') then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, auth_flows_password_reset_helper)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  _G._users['a@x.com'] = {id='u1', email='a@x.com'} "
        "  _G._by_id['u1'] = _G._users['a@x.com'] "
        "  af.send_password_reset('a@x.com', 'http://t.io') "
        "  if #_G._sent ~= 1 then return 0 end "
        "  af.send_password_reset('missing@x.com', 'http://t.io') "
        "  if #_G._sent ~= 1 then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, auth_flows_magic_link_auto_signup_opt_in)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Default: unknown email → silent no-op (no email sent). */
    int silent = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  af.send_magic_link('unknown@x.com', 'http://t.io') "
        "  return #_G._sent == 0 and 1 or 0 "
        "end)()");
    ASSERT_EQ(silent, 1);

    /* Opt-in: re-init with auto_signup → creates user + sends. */
    int auto_signup = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  af._test.reset() "
        "  af.init({ "
        "    state_secret = ('k'):rep(32), "
        "    trust_request_host = true, "
        "    email_send = function(to, s, h, t) "
        "      _G._sent[#_G._sent+1] = {to=to} "
        "    end, "
        "    templates = { "
        "      welcome = function() return {subject='w',text='x'} end, "
        "      verify = function() return {subject='v',text='x'} end, "
        "      magic_link = function() return {subject='m',text='x'} end, "
        "      password_reset = function() return {subject='p',text='x'} end, "
        "      email_change = function() return {subject='e',text='x'} end, "
        "    }, "
        "    user_find_by_email = function(e) return _G._users[e] end, "
        "    user_get = function(id) return _G._by_id[id] end, "
        "    user_create = function(e, ph) "
        "      local u = {id='auto', email=e, password_hash=ph} "
        "      _G._users[e] = u; _G._by_id['auto'] = u; return 'auto' "
        "    end, "
        "    user_set_password = function() end, "
        "    user_set_email = function() end, "
        "    user_set_email_verified = function() end, "
        "    on_login = function() end, "
        "    magic_link_auto_signup = true, "
        "  }) "
        "  af.send_magic_link('new@x.com', 'http://t.io') "
        "  return (#_G._sent == 1 and _G._users['new@x.com'] ~= nil) "
        "    and 1 or 0 "
        "end)()");
    ASSERT_EQ(auto_signup, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, auth_flows_state_secret_non_ascii_round_trip)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Round-8 HIGH-2: state_secret may contain bytes >= 0x80 (e.g.
     * a passphrase / random binary key). Both runtimes must derive the
     * same HMAC key from it, which is only true if each hexes the raw
     * bytes (hull.encoding) - handing a JS string to C UTF-8-inflates
     * it. This test pins the byte-for-byte hex encoding (32 bytes of
     * 0x80 -> "80" repeated 32x) AND verifies a token signed under the
     * high-byte secret round-trips via issue_token / parse_token. */
    int ok = eval_int(
        "(function() "
        "  local af = require('hull.web.auth-flows') "
        "  af._test.reset() "
        "  local secret = string.rep(string.char(0x80), 32) "
        "  af.init({ "
        "    state_secret = secret, "
        "    trust_request_host = true, "
        "    email_send = function() end, "
        "    templates = { "
        "      welcome = function() return {subject='w',text='x'} end, "
        "      verify = function() return {subject='v',text='x'} end, "
        "      magic_link = function() return {subject='m',text='x'} end, "
        "      password_reset = function() return {subject='p',text='x'} end, "
        "      email_change = function() return {subject='e',text='x'} end, "
        "    }, "
        "    user_find_by_email = function() end, "
        "    user_get = function() end, "
        "    user_create = function() end, "
        "    user_set_password = function() end, "
        "    user_set_email = function() end, "
        "    user_set_email_verified = function() end, "
        "    on_login = function() end, "
        "  }) "
        "  local A = af._test.ACTIONS "
        "  local tok = af._test.issue_token('u1', A.verify_email, 60) "
        "  local env, err = af._test.parse_token(tok, A.verify_email) "
        "  if not env or err then return 0 end "
        "  if env.sub ~= 'u1' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, auth_flows_email_rate_limit_per_recipient)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Round-8 HIGH-1: per-recipient email rate limit closes the
     * attacker-chosen-recipient email-storm class. Gate sits inside
     * send_email; blocked sends are silently dropped so the response
     * shape stays enumeration-safe. Buckets are per (lower-cased)
     * recipient with a sliding window. */
    int ok = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  af._test.reset() "
        "  af.init({ "
        "    state_secret = ('k'):rep(32), "
        "    trust_request_host = true, "
        "    email_rate_limit = { limit = 2, window = 60 }, "
        "    email_send = function() end, "
        "    templates = { "
        "      welcome = function() return {subject='w',text='x'} end, "
        "      verify = function() return {subject='v',text='x'} end, "
        "      magic_link = function() return {subject='m',text='x'} end, "
        "      password_reset = function() return {subject='p',text='x'} end, "
        "      email_change = function() return {subject='e',text='x'} end, "
        "    }, "
        "    user_find_by_email = function() end, "
        "    user_get = function() end, "
        "    user_create = function() end, "
        "    user_set_password = function() end, "
        "    user_set_email = function() end, "
        "    user_set_email_verified = function() end, "
        "    on_login = function() end, "
        "  }) "
        "  local a1 = af._test.email_rate_allow('victim@x.com') "
        "  local a2 = af._test.email_rate_allow('victim@x.com') "
        "  local a3 = af._test.email_rate_allow('victim@x.com') "
        "  local b1 = af._test.email_rate_allow('other@x.com') "
        "  local c1 = af._test.email_rate_allow('VICTIM@x.com') "
        "  af._test.email_rate_reset() "
        "  local d1 = af._test.email_rate_allow('victim@x.com') "
        "  return (a1 and a2 and not a3 and b1 and not c1 and d1) "
        "    and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, auth_flows_email_rate_limit_drops_send)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Integration check via send_magic_link + auto_signup. */
    int ok = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  af._test.reset() "
        "  af.init({ "
        "    state_secret = ('k'):rep(32), "
        "    trust_request_host = true, "
        "    email_rate_limit = { limit = 2, window = 60 }, "
        "    magic_link_auto_signup = true, "
        "    email_send = function(to) "
        "      _G._sent[#_G._sent+1] = {to=to} "
        "    end, "
        "    templates = { "
        "      welcome = function() return {subject='w',text='x'} end, "
        "      verify = function() return {subject='v',text='x'} end, "
        "      magic_link = function() return {subject='m',text='x'} end, "
        "      password_reset = function() return {subject='p',text='x'} end, "
        "      email_change = function() return {subject='e',text='x'} end, "
        "    }, "
        "    user_find_by_email = function(e) return _G._users[e] end, "
        "    user_get = function(id) return _G._by_id[id] end, "
        "    user_create = function(e, ph) "
        "      local u = {id='u'..e, email=e, password_hash=ph} "
        "      _G._users[e] = u; _G._by_id[u.id] = u; return u.id "
        "    end, "
        "    user_set_password = function() end, "
        "    user_set_email = function() end, "
        "    user_set_email_verified = function() end, "
        "    on_login = function() end, "
        "  }) "
        "  af.send_magic_link('flood@x.com', 'http://t.io') "
        "  af.send_magic_link('flood@x.com', 'http://t.io') "
        "  af.send_magic_link('flood@x.com', 'http://t.io') "
        "  af.send_magic_link('flood@x.com', 'http://t.io') "
        "  af.send_magic_link('clean@x.com', 'http://t.io') "
        "  return #_G._sent == 3 and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* ── hull.web.cookie tests ─────────────────────────────────────────────── */

UTEST(lua_stdlib, cookie_parse)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local c = require('hull.web.cookie') "
        "  local r = c.parse('session=abc; theme=dark') "
        "  return r.session == 'abc' and r.theme == 'dark' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    /* Empty string returns empty table */
    int empty = eval_int(
        "(function() "
        "  local c = require('hull.web.cookie') "
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

    /* Default options: HttpOnly, Secure, SameSite=Lax, Path=/ */
    char *cookie = eval_str(
        "require('hull.web.cookie').serialize('sid', 'abc123')");
    ASSERT_NE(cookie, NULL);
    ASSERT_NE(strstr(cookie, "sid=abc123"), NULL);
    ASSERT_NE(strstr(cookie, "HttpOnly"), NULL);
    ASSERT_NE(strstr(cookie, "Secure"), NULL);
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
        "require('hull.web.cookie').clear('sid')");
    ASSERT_NE(cookie, NULL);
    ASSERT_NE(strstr(cookie, "sid="), NULL);
    ASSERT_NE(strstr(cookie, "Max-Age=0"), NULL);
    free(cookie);

    cleanup_lua_caps();
}

/* ── hull.web.middleware.session tests ─────────────────────────────────── */

UTEST(lua_stdlib, session_create_and_load)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local s = require('hull.web.middleware.session') "
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
        "  local s = require('hull.web.middleware.session') "
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

/* An absolute exp stays absolute. The relative cutoff was 2e9, above the
 * current time, so the standard exp = now + 3600 was taken as a duration and
 * the token lived ~57 years. */
UTEST(lua_stdlib, jwt_absolute_exp_is_kept)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int ok = eval_int(
        "(function() "
        "  local jwt = require('hull.jwt') "
        "  local abs = 1900000000 "
        "  local p = jwt.verify(jwt.sign({ exp = abs }, 'mysecret'), 'mysecret') "
        "  if not p or p.exp ~= abs then return 0 end "
        "  local r = jwt.verify(jwt.sign({ exp = 3600 }, 'mysecret'), 'mysecret') "
        "  if not r or r.exp < 1000000000 or r.exp > abs then return 0 end "
        "  return 1 "
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

/* ── hull.web.middleware.csrf tests ────────────────────────────────────── */

UTEST(lua_stdlib, csrf_generate_and_verify)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local csrf = require('hull.web.middleware.csrf') "
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
        "  local csrf = require('hull.web.middleware.csrf') "
        "  local token = csrf.generate('session123', 'secret') "
        "  return csrf.verify(token, 'other_session', 'secret') and 0 or 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* Cross-runtime wire-format fixture. The reference token below was
 * precomputed for session_id="s1", secret="k", tsHex="1" - i.e. the
 * HMAC of "s1:1" keyed by hex("k")="6b". The same fixture lives in
 * tests/hull/runtime/js/test_js.c; both must accept it byte-for-byte
 * or the Lua and JS sibling middlewares have drifted out of parity. */
UTEST(lua_stdlib, csrf_cross_runtime_reference_token)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* max_age of 4294967295 (year ~2106 in unix-seconds) keeps the
     * fixture valid forever for the purposes of this test. */
    int ok = eval_int(
        "(function() "
        "  local csrf = require('hull.web.middleware.csrf') "
        "  local ref = '1.6ae78d056ed813a207a55074947fdbeef0ae8c7850acab486cb52bae058956da' "
        "  return csrf.verify(ref, 's1', 'k', 4294967295) and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    /* Flip one bit of the MAC - must reject. */
    int rej = eval_int(
        "(function() "
        "  local csrf = require('hull.web.middleware.csrf') "
        "  local bad = '1.7ae78d056ed813a207a55074947fdbeef0ae8c7850acab486cb52bae058956da' "
        "  return csrf.verify(bad, 's1', 'k', 4294967295) and 0 or 1 "
        "end)()");
    ASSERT_EQ(rej, 1);

    cleanup_lua_caps();
}

/* ── hull.web.middleware.auth tests (smoke - modules load and expose API) */

UTEST(lua_cap, crypto_hmac_sha256_verify)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Correct MAC → true */
    int ok = eval_int(
        "(function() "
        "  local mac = crypto.hmac_sha256('what do ya want for nothing?', 'Jefe') "
        "  return crypto.hmac_sha256_verify('what do ya want for nothing?', 'Jefe', mac) and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    /* Wrong MAC → false */
    int bad_mac = eval_int(
        "crypto.hmac_sha256_verify('what do ya want for nothing?', 'Jefe', "
        "  string.rep('\\0', 32)) and 1 or 0");
    ASSERT_EQ(bad_mac, 0);

    /* Wrong key → false */
    int bad_key = eval_int(
        "(function() "
        "  local mac = crypto.hmac_sha256('hello', 'Jefe') "
        "  return crypto.hmac_sha256_verify('hello', 'Jeff', mac) and 1 or 0 "
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
        "  local auth = require('hull.web.middleware.auth') "
        "  return type(auth.session_middleware) == 'function' "
        "     and type(auth.jwt_middleware) == 'function' "
        "     and type(auth.login) == 'function' "
        "     and type(auth.logout) == 'function' "
        "     and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* ── hull.web.form tests ─────────────────────────────────────────────────── */

UTEST(lua_stdlib, form_parse)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local form = require('hull.web.form') "
        "  local r = form.parse('email=a%40b.com&pass=hello+world') "
        "  return r.email == 'a@b.com' and r.pass == 'hello world' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    /* Empty/nil returns empty table */
    int empty = eval_int(
        "(function() "
        "  local form = require('hull.web.form') "
        "  local r = form.parse('') "
        "  return next(r) == nil and 1 or 0 "
        "end)()");
    ASSERT_EQ(empty, 1);

    cleanup_lua();
}

/* ── hull.validate tests ─────────────────────────────────────────────── */

UTEST(lua_stdlib, validate_check_required)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local v = require('hull.validate') "
        "  local ok, errors = v.check({}, { name = { required = true } }) "
        "  return ok == false and errors.name == 'is required' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    int pass = eval_int(
        "(function() "
        "  local v = require('hull.validate') "
        "  local ok = v.check({ name = 'alice' }, { name = { required = true } }) "
        "  return ok and 1 or 0 "
        "end)()");
    ASSERT_EQ(pass, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, validate_check_min_max)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local v = require('hull.validate') "
        "  local ok, errors = v.check({ pw = 'abc' }, { pw = { min = 8 } }) "
        "  return ok == false and errors.pw == 'must be at least 8 characters' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    int max_ok = eval_int(
        "(function() "
        "  local v = require('hull.validate') "
        "  local ok, errors = v.check({ n = 'toolong' }, { n = { max = 3 } }) "
        "  return ok == false and errors.n == 'must be at most 3 characters' and 1 or 0 "
        "end)()");
    ASSERT_EQ(max_ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, validate_check_email)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local v = require('hull.validate') "
        "  local ok = v.check({ e = 'a@b.com' }, { e = { email = true } }) "
        "  return ok and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    int bad = eval_int(
        "(function() "
        "  local v = require('hull.validate') "
        "  local ok, errors = v.check({ e = 'notanemail' }, { e = { email = true } }) "
        "  return ok == false and errors.e == 'is not a valid email' and 1 or 0 "
        "end)()");
    ASSERT_EQ(bad, 1);

    cleanup_lua();
}

/* ── hull.i18n tests ─────────────────────────────────────────────────── */

UTEST(lua_stdlib, i18n_load_and_translate)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local i18n = require('hull.i18n') "
        "  i18n.reset() "
        "  i18n.load('en', { greeting = 'Hello', nav = { home = 'Home' } }) "
        "  i18n.locale('en') "
        "  return i18n.t('greeting') == 'Hello' "
        "     and i18n.t('nav.home') == 'Home' "
        "     and i18n.t('missing') == 'missing' "
        "     and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, i18n_interpolation)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local i18n = require('hull.i18n') "
        "  i18n.reset() "
        "  i18n.load('en', { total = 'Total: ${amount}' }) "
        "  i18n.locale('en') "
        "  return i18n.t('total', {amount = '42'}) == 'Total: 42' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, i18n_number_and_date)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local i18n = require('hull.i18n') "
        "  i18n.reset() "
        "  i18n.load('en', { format = { decimal_sep = '.', thousands_sep = ',', date_pattern = 'YYYY-MM-DD' } }) "
        "  i18n.locale('en') "
        "  return i18n.number(1500) == '1,500' "
        "     and i18n.date(0) == '1970-01-01' "
        "     and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, i18n_detect)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local i18n = require('hull.i18n') "
        "  i18n.reset() "
        "  i18n.load('en', {}) "
        "  i18n.load('hu', {}) "
        "  return i18n.detect('hu,en;q=0.9') == 'hu' "
        "     and i18n.detect('en-US') == 'en' "
        "     and i18n.detect('ja') == nil "
        "     and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

/* ── hull.csv tests ──────────────────────────────────────────────────── */

UTEST(lua_stdlib, csv_parse_basic)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local csv = require('hull.csv') "
        "  local rows = csv.parse('a,b,c\\n1,2,3\\n') "
        "  return #rows == 2 and rows[1][1] == 'a' and rows[2][3] == '3' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, csv_parse_headers)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local csv = require('hull.csv') "
        "  local rows = csv.parse('name,age\\nalice,30\\n', { headers = true }) "
        "  return #rows == 1 and rows[1].name == 'alice' and rows[1].age == '30' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, csv_parse_quoted)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local csv = require('hull.csv') "
        "  local rows = csv.parse('\"a,b\",c\\n') "
        "  return rows[1][1] == 'a,b' and rows[1][2] == 'c' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, csv_encode_basic)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    char *s = eval_str(
        "require('hull.csv').encode({{'a','b','c'},{'1','2','3'}})");
    ASSERT_NE(s, NULL);
    ASSERT_STREQ(s, "a,b,c\n1,2,3\n");
    free(s);

    cleanup_lua();
}

UTEST(lua_stdlib, csv_encode_headers)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local csv = require('hull.csv') "
        "  local result = csv.encode({{name='alice', age='30'}}, { headers = true }) "
        "  local rows = csv.parse(result, { headers = true }) "
        "  return #rows == 1 and rows[1].name == 'alice' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, csv_encode_sanitize_formulas)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* Off by default: a formula cell is emitted verbatim. */
    char *plain = eval_str("require('hull.csv').encode({{'=cmd|calc'}})");
    ASSERT_NE(plain, NULL);
    ASSERT_STREQ(plain, "=cmd|calc\n");
    free(plain);

    /* Opt-in: a leading = / @ (etc.) is prefixed with a ' so a spreadsheet
     * treats it as text; a non-formula cell is untouched. */
    char *safe = eval_str(
        "require('hull.csv').encode({{'=cmd|calc'},{'@x'},{'ok'}}, "
        "{ sanitize_formulas = true })");
    ASSERT_NE(safe, NULL);
    ASSERT_STREQ(safe, "'=cmd|calc\n'@x\nok\n");
    free(safe);

    cleanup_lua();
}

/* ── hull.archive.tar tests (parse/create marshalling) ──────────────── */

UTEST(lua_stdlib, tar_create_parse_roundtrip)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* create -> parse must recover names, data, dir flag, and mode. */
    int ok = eval_int(
        "(function() "
        "  local tar = require('hull.archive.tar') "
        "  local b = tar.create({ "
        "     { name='greet.txt', data='hello', mode=420 }, "     /* 0644 */
        "     { name='sub', is_dir=true, mode=493 }, "            /* 0755 */
        "     { name='sub/x.bin', data='\\0\\1\\2' } }) "
        "  local e = tar.parse(b) "
        "  return (#e==3 and e[1].name=='greet.txt' and e[1].data=='hello' "
        "          and e[1].mode==420 and e[2].is_dir==true "
        "          and e[3].name=='sub/x.bin' and #e[3].data==3) and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, tar_parse_rejects_truncated)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* A valid archive truncated mid-data: the 512-byte header declares a
     * 1000-byte member but the buffer is cut short, so parse returns
     * (nil, err) rather than reading past the end. */
    int ok = eval_int(
        "(function() "
        "  local tar = require('hull.archive.tar') "
        "  local b = tar.create({ { name='f', data=string.rep('x', 1000) } }) "
        "  local r, err = tar.parse(b:sub(1, 600)) "
        "  return (r == nil and type(err) == 'string') and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

UTEST(lua_stdlib, tar_create_rejects_unsafe_name)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    /* An absolute / ".." member name is refused by the writer. */
    int ok = eval_int(
        "(function() "
        "  local tar = require('hull.archive.tar') "
        "  local r = tar.create({ { name='../escape', data='x' } }) "
        "  return (r == nil) and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua();
}

/* ── hull.search tests ───────────────────────────────────────────────── */

/* The user-facing Lua stdlib ships 16 co-located test scripts under
 * stdlib/lua/hull/tests/. Until now NOTHING ran them: no Makefile glob
 * collects that directory -- every glob that names it does so only to
 * EXCLUDE any path under a tests directory -- and no harness loaded them, so they
 * were 16 files of assurance that did not exist. test_csv.lua even documents
 * a runner -- "test_lua_runtime.c" -- that no longer exists under that name.
 *
 * This is the first one wired, deliberately chosen because it already meets
 * the `return { pass, fail }` contract: it proves the seam without also
 * testing a conversion. The remaining scripts follow, and 8 of them must
 * first lose an `os.exit(1)` tail that would kill this binary outright.
 *
 * hull.csv is pure Lua (no capability use), so the vanilla state is the right
 * harness; a script needing db/crypto belongs in a caps-bearing leg below. */
/* The two Lua suites the vanilla harness cannot host: their modules are
 * C-backed and unreachable without the capability layer. Same scripts, same
 * { pass, fail } contract -- only the state they run in differs. */
UTEST(lua_stdlib, search_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_search.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, email_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_email.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, ws_stream_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ws_stream.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_wire_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_wire.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_packet_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_packet.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_kexinit_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_kexinit.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, encoding_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_encoding.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_kex_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_kex.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_hostkey_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_hostkey.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

/* ECDSA and RSA host keys against keys made by ssh-keygen and signatures made
 * by OpenSSL. Vanilla state: the encoding half (DER/PEM byte-equal to
 * OpenSSL's, signature conversion, the refusals) - 13 tests. */
UTEST(lua_stdlib, ssh_hostkey_vectors_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_hostkey_vectors.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_EQ(pass, 13LL);
}

/* The same script with the capability layer, so hull.crypto is real and the
 * OpenSSL signatures are verified by mbedTLS through crypto.verify: 3 more
 * tests. The exact count is the point - a require that quietly failed would
 * skip the crypto half and still report no failures. */
UTEST(lua_stdlib, ssh_hostkey_vectors_real_crypto)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_ssh_hostkey_vectors.lua",
                                     &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_EQ(pass, 16LL);

    cleanup_lua_caps();
}

/* RSA user keys written by ssh-keygen. Vanilla: parsing, the size floor, the
 * damaged-key error (3). With the capability layer: the loaded key signs
 * exactly as OpenSSL does with the same key, the passphrase-protected copy
 * too, and verifies (3 more). Exact counts, as for the host-key vectors. */
UTEST(lua_stdlib, ssh_rsa_key_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_rsa_key.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_EQ(pass, 3LL);
}

UTEST(lua_stdlib, ssh_rsa_key_real_crypto)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_ssh_rsa_key.lua",
                                     &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_EQ(pass, 6LL);

    cleanup_lua_caps();
}

/* ECDSA user keys written by ssh-keygen. Vanilla: P-256 / P-384 load with a
 * SEC1 PEM byte-equal to OpenSSL's, P-521 refused, a key failing its
 * self-check reported as damaged, the r||s -> mpint encoding (5). With the
 * capability layer the keys really sign and verify against ssh-keygen's
 * export, including a passphrase-protected copy (3 more). */
UTEST(lua_stdlib, ssh_ecdsa_key_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_ecdsa_key.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_EQ(pass, 5LL);
}

UTEST(lua_stdlib, ssh_ecdsa_key_real_crypto)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_ssh_ecdsa_key.lua",
                                     &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_EQ(pass, 8LL);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, ssh_userauth_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_userauth.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_channel_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_channel.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_sftp_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_sftp.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

/* chacha20-poly1305@openssh.com. Vanilla: the framing under stand-in
 * primitives (10). With the capability layer the same file runs again over
 * hull.crypto's real ChaCha20 / Poly1305 (7 more). */
UTEST(lua_stdlib, ssh_chacha_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_chacha.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_EQ(pass, 10LL);
}

UTEST(lua_stdlib, ssh_chacha_real_crypto)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_ssh_chacha.lua",
                                     &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_EQ(pass, 17LL);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, ssh_cipher_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_cipher.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_privatekey_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_privatekey.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_known_hosts_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_known_hosts.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, ssh_transport_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_transport.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

/* The tunnel COMPOSITION: ssh.connect over hull.ssh.ws_stream. Driven
 * against a fake relay, so it asserts the bytes that relay is sent rather
 * than re-testing either layer. */
UTEST(lua_stdlib, ssh_tunnel_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_ssh_tunnel.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, csv_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_csv.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);        /* ran to a { pass, fail } return */
    EXPECT_EQ(fail, 0LL);    /* every assertion in the script held */
    EXPECT_GT(pass, 0LL);    /* and it actually executed cases */
}

/* The rest of the co-located Lua stdlib suites. Every one of these ran
 * nowhere until now; see the csv_suite note above for how that happened.
 *
 * Nine of them had to lose a trailing `if fail > 0 then os.exit(1) end`
 * first. In the vanilla state this harness uses, `os` EXISTS -- unlike inside
 * Hull's sandbox -- so that line did not fail a suite, it terminated the test
 * binary and took every later suite with it.
 *
 * Two scripts are deliberately absent. test_search.lua needs a db (hull.search
 * calls require("hull.db").default() at module load) and test_email.lua needs
 * hull.http-client + hull.smtp; all three are C-backed with no .lua file, so
 * require cannot resolve them in a vanilla state at all. Those need an
 * in-runtime harness, which is a separate mechanism and a separate change. */
UTEST(lua_stdlib, confirm_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_confirm.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, form_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_form.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, htmx_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_htmx.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, htmx_form_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_htmx_form.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, htmx_inline_edit_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_htmx_inline_edit.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, htmx_pagination_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_htmx_pagination.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, htmx_search_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_htmx_search.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, htmx_sort_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_htmx_sort.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, htmx_table_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_htmx_table.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, i18n_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_i18n.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, json_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_json.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, toast_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_toast.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}

UTEST(lua_stdlib, validate_suite)
{
    long long pass = 0, fail = -1;
    int rc = run_lua_test("stdlib/lua/hull/tests/test_validate.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
}


UTEST(lua_stdlib, search_create_and_query)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local s = require('hull.search') "
        "  s.create_index('test_articles', {'title', 'body'}) "
        "  s.index('test_articles', '1', {title='Hello World', body='Test article about searching'}) "
        "  s.index('test_articles', '2', {title='Lua Guide', body='Learn Lua programming'}) "
        "  local results = s.query('test_articles', 'lua') "
        "  local ok = #results == 1 and results[1].id == '2' "
        "  s.drop_index('test_articles') "
        "  return ok and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, search_remove)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local s = require('hull.search') "
        "  s.create_index('test_rm', {'title'}) "
        "  s.index('test_rm', '1', {title='hello'}) "
        "  s.index('test_rm', '2', {title='world'}) "
        "  s.remove('test_rm', '1') "
        "  local results = s.query('test_rm', 'hello') "
        "  local ok = #results == 0 "
        "  s.drop_index('test_rm') "
        "  return ok and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* Tokenize grammar parity with JS. Must match
 * /^[A-Za-z][A-Za-z0-9_]*( [A-Za-z][A-Za-z0-9_]*)*$/ - leading/trailing/
 * double spaces, leading digits, leading underscores all rejected. */
UTEST(lua_stdlib, search_tokenize_grammar_parity)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Valid: single identifier, multi-word with single spaces. */
    int ok = eval_int(
        "(function() "
        "  local s = require('hull.search') "
        "  s.create_index('tk_a', {'t'}, { tokenize = 'unicode61' }) "
        "  s.drop_index('tk_a') "
        "  s.create_index('tk_b', {'t'}, { tokenize = 'porter ascii' }) "
        "  s.drop_index('tk_b') "
        "  s.create_index('tk_c', {'t'}, { tokenize = "
        "      'porter unicode61 remove_diacritics 1' }) "
        "  s.drop_index('tk_c') "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    /* Invalid: each of these used to slip through ^[a-zA-Z0-9_ ]+$. */
    const char *bad_inputs[] = {
        "' '",                /* single space */
        "'  '",               /* double space */
        "' unicode61'",       /* leading space */
        "'unicode61 '",       /* trailing space */
        "'unicode61  porter'",/* double space between */
        "'123abc'",           /* leading digit */
        "'_foo'",             /* leading underscore */
        "''",                 /* empty */
    };
    for (size_t i = 0; i < sizeof(bad_inputs)/sizeof(bad_inputs[0]); i++) {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "(function() "
            "  local s = require('hull.search') "
            "  local ok, err = pcall(s.create_index, 'tk_x', {'t'}, "
            "    { tokenize = %s }) "
            "  return (not ok) and 1 or 0 "
            "end)()", bad_inputs[i]);
        ASSERT_EQ(eval_int(buf), 1);
    }

    cleanup_lua_caps();
}

UTEST(lua_stdlib, search_snippet)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local s = require('hull.search') "
        "  s.create_index('test_snip', {'title', 'content'}) "
        "  s.index('test_snip', '1', {title='Guide', content='A comprehensive guide to searching'}) "
        "  local results = s.query('test_snip', 'guide', { "
        "    snippet = { column = 2, tokens = 10, before = '<b>', after = '</b>' } "
        "  }) "
        "  local ok = #results >= 1 "
        "  s.drop_index('test_snip') "
        "  return ok and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* ── hull.web.middleware.rbac tests ───────────────────────────────────────── */

UTEST(lua_stdlib, rbac_init_and_assign)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local rbac = require('hull.web.middleware.rbac') "
        "  rbac.init() "
        "  rbac.define_role('admin') "
        "  rbac.define_permission('users.read') "
        "  rbac.grant('admin', 'users.read') "
        "  rbac.assign('user1', 'admin') "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, rbac_has_role)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local rbac = require('hull.web.middleware.rbac') "
        "  rbac.init() "
        "  rbac.define_role('admin') "
        "  rbac.assign('user1', 'admin') "
        "  return rbac.has_role('user1', 'admin') and "
        "         not rbac.has_role('user1', 'editor') and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, rbac_has_permission)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local rbac = require('hull.web.middleware.rbac') "
        "  rbac.init() "
        "  rbac.define_role('admin') "
        "  rbac.define_permission('users.read') "
        "  rbac.define_permission('users.write') "
        "  rbac.grant('admin', 'users.read') "
        "  rbac.assign('user1', 'admin') "
        "  return rbac.has_permission('user1', 'users.read') and "
        "         not rbac.has_permission('user1', 'users.write') and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

UTEST(lua_stdlib, rbac_middleware_deny)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local rbac = require('hull.web.middleware.rbac') "
        "  rbac.init() "
        "  local mw = rbac.require_role('admin') "
        "  return type(mw) == 'function' and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}

/* ── Bytecode cache ─────────────────────────────────────────────────
 *
 * The cache lives at $HOME/.hull/cache/lua-bytecode/. Tests redirect
 * $HOME into a tmpdir so they can stat the produced .luac files
 * without polluting the developer's real cache. */

#include <dirent.h>
#include <ftw.h>

static int bc_rm_entry(const char *path, const struct stat *st,
                       int type, struct FTW *ftw)
{
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

static void bc_with_tmp_home(char *tmpdir, size_t n)
{
    if (hl_test_path(tmpdir, n, "hull_bc_cache_XXXXXX") != 0 || !mkdtemp(tmpdir)) {
        fprintf(stderr, "bc_with_tmp_home: no usable temp dir\n");
        tmpdir[0] = 0;
        return;
    }
    setenv("HOME", tmpdir, 1);
    /* Make sure no stale opt-out from a previous test leaks in. */
    unsetenv("HULL_NO_CACHE");
    unsetenv("HULL_NO_LUA_BYTECODE_CACHE");
    /* Tear down the process-wide store singleton so the next call
     * resolves to the freshly-redirected $HOME. */
    hl_lua_bytecode_cache_reset();
}

static void bc_cleanup_tmp_home(const char *tmpdir)
{
    nftw(tmpdir, bc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* Source has to clear the 256-byte minimum cache threshold. */
static const char *BC_PROBE_SRC =
    "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "local function probe(n) return n * 2 + 3 end\n"
    "return probe(7)\n";

/* Count cached bytecode files. The store is sharded under
 * blobs/runtime/lua-bytecode/blobs/<XX>/<sha256-hex>, so we walk
 * the two-level shard tree and tally files whose name looks like a
 * 64-char hex id. */
static int bc_count_luac(const char *dir)
{
    char root[512];
    snprintf(root, sizeof(root),
             "%s/.hull/blobs/runtime/lua-bytecode/blobs", dir);
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

UTEST(lua_bytecode_cache, chunkname_in_key)
{
    /* The key folds in the chunkname because lua_dump bakes it into the
     * bytecode as the proto's `source`, and the name handed to
     * luaL_loadbuffer for a BINARY chunk does not override it. Keyed on
     * source bytes alone, the same source under two names collapsed to one
     * entry and the second load reported the first one's name - which
     * mod_db.c::lua_is_stdlib_caller reads to gate _hull_* access. Mirrors
     * js_bytecode_cache.module_name_in_key. */
    char tmp[256];
    bc_with_tmp_home(tmp, sizeof tmp);

    lua_State *L = luaL_newstate();
    ASSERT_NE_MSG((void *)L, NULL, "newstate");

    ASSERT_EQ(0, bc_count_luac(tmp));
    ASSERT_EQ(LUA_OK, hl_lua_load_cached(L, BC_PROBE_SRC,
                                         strlen(BC_PROBE_SRC), "=name_a"));
    lua_pop(L, 1);
    ASSERT_EQ(1, bc_count_luac(tmp));

    ASSERT_EQ(LUA_OK, hl_lua_load_cached(L, BC_PROBE_SRC,
                                         strlen(BC_PROBE_SRC), "=name_b"));
    ASSERT_EQ_MSG(2, bc_count_luac(tmp),
                  "distinct chunknames produce distinct entries");

    /* And the second load reports its OWN name, not the first's. */
    lua_Debug ar;
    lua_getinfo(L, ">S", &ar);   /* ">S" consumes the function on the stack */
    /* ar.source keeps the "=" prefix (only short_src strips it) - and that
     * raw field is what the _hull_* gate matches "hull." against. */
    ASSERT_STREQ("=name_b", ar.source);

    lua_close(L);
    nftw(tmp, bc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(lua_bytecode_cache, miss_then_hit_populates_disk)
{
    char tmp[256];
    bc_with_tmp_home(tmp, sizeof tmp);

    lua_State *L = luaL_newstate();
    ASSERT_NE_MSG((void *)L, NULL, "newstate");

    /* First call: cache miss, should compile + persist. */
    ASSERT_EQ(0, bc_count_luac(tmp));
    int rc = hl_lua_load_cached(L, BC_PROBE_SRC, strlen(BC_PROBE_SRC), "=probe");
    ASSERT_EQ_MSG(rc, LUA_OK, "first load");
    ASSERT_EQ(1, bc_count_luac(tmp));

    /* Run it to make sure the loaded chunk is functional. */
    ASSERT_EQ(LUA_OK, lua_pcall(L, 0, 1, 0));
    ASSERT_EQ(17, (int)lua_tointeger(L, -1));
    lua_pop(L, 1);

    /* Second call: cache hit - function loads, no extra file. */
    rc = hl_lua_load_cached(L, BC_PROBE_SRC, strlen(BC_PROBE_SRC), "=probe");
    ASSERT_EQ(LUA_OK, rc);
    ASSERT_EQ(1, bc_count_luac(tmp));
    ASSERT_EQ(LUA_OK, lua_pcall(L, 0, 1, 0));
    ASSERT_EQ(17, (int)lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_close(L);
    bc_cleanup_tmp_home(tmp);
}

UTEST(lua_bytecode_cache, opt_out_via_env_skips_disk)
{
    char tmp[256];
    bc_with_tmp_home(tmp, sizeof tmp);
    setenv("HULL_NO_LUA_BYTECODE_CACHE", "1", 1);

    lua_State *L = luaL_newstate();
    int rc = hl_lua_load_cached(L, BC_PROBE_SRC, strlen(BC_PROBE_SRC), "=probe");
    ASSERT_EQ(LUA_OK, rc);
    ASSERT_EQ_MSG(0, bc_count_luac(tmp), "no luac written when opted out");
    ASSERT_EQ(LUA_OK, lua_pcall(L, 0, 1, 0));
    ASSERT_EQ(17, (int)lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_close(L);

    unsetenv("HULL_NO_LUA_BYTECODE_CACHE");
    bc_cleanup_tmp_home(tmp);
}

UTEST(lua_bytecode_cache, tiny_source_skips_cache)
{
    /* Under 256 bytes - cache shouldn't bother to memoize. */
    char tmp[256];
    bc_with_tmp_home(tmp, sizeof tmp);

    const char *src = "return 1 + 2\n";
    lua_State *L = luaL_newstate();
    int rc = hl_lua_load_cached(L, src, strlen(src), "=tiny");
    ASSERT_EQ(LUA_OK, rc);
    ASSERT_EQ_MSG(0, bc_count_luac(tmp), "tiny chunks bypass cache");
    ASSERT_EQ(LUA_OK, lua_pcall(L, 0, 1, 0));
    ASSERT_EQ(3, (int)lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_close(L);

    bc_cleanup_tmp_home(tmp);
}

UTEST(lua_bytecode_cache, parse_error_returns_no_cache_write)
{
    char tmp[256];
    bc_with_tmp_home(tmp, sizeof tmp);

    /* Padded but syntactically invalid. */
    const char *bad =
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "this is = not = valid Lua )(\n";

    lua_State *L = luaL_newstate();
    int rc = hl_lua_load_cached(L, bad, strlen(bad), "=bad");
    ASSERT_NE_MSG(rc, LUA_OK, "parse error reported");
    /* Error string on the stack - matches luaL_loadbuffer contract. */
    ASSERT_TRUE(lua_isstring(L, -1));
    ASSERT_EQ(0, bc_count_luac(tmp));
    lua_pop(L, 1);
    lua_close(L);

    bc_cleanup_tmp_home(tmp);
}

UTEST(lua_bytecode_cache, corrupt_cache_falls_back_to_source)
{
    char tmp[256];
    bc_with_tmp_home(tmp, sizeof tmp);

    /* Prime the cache. */
    lua_State *L = luaL_newstate();
    ASSERT_EQ(LUA_OK,
        hl_lua_load_cached(L, BC_PROBE_SRC, strlen(BC_PROBE_SRC), "=probe"));
    ASSERT_EQ(LUA_OK, lua_pcall(L, 0, 1, 0));
    lua_pop(L, 1);
    ASSERT_EQ(1, bc_count_luac(tmp));

    /* Corrupt every cached entry. Walk the sharded layout
     * (blobs/runtime/lua-bytecode/blobs/<XX>/<hex>) and overwrite
     * each 64-char-hex-named file with garbage. */
    char root[512];
    snprintf(root, sizeof(root),
             "%s/.hull/blobs/runtime/lua-bytecode/blobs", tmp);
    DIR *r = opendir(root);
    ASSERT_NE(r, NULL);
    struct dirent *sh;
    while ((sh = readdir(r))) {
        if (sh->d_name[0] == '.') continue;
        char shard[512];
        snprintf(shard, sizeof(shard), "%s/%s", root, sh->d_name);
        DIR *d = opendir(shard);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strlen(e->d_name) != 64) continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", shard, e->d_name);
            FILE *f = fopen(full, "wb");
            if (!f) continue;
            fwrite("\x00\x00\x00\x00garbage", 1, 11, f);
            fclose(f);
        }
        closedir(d);
    }
    closedir(r);

    /* Reload - must recover, re-compile from source, repopulate cache. */
    int rc = hl_lua_load_cached(L, BC_PROBE_SRC, strlen(BC_PROBE_SRC), "=probe");
    ASSERT_EQ(LUA_OK, rc);
    ASSERT_EQ(LUA_OK, lua_pcall(L, 0, 1, 0));
    ASSERT_EQ(17, (int)lua_tointeger(L, -1));
    lua_pop(L, 1);
    /* Still one file - corrupt one evicted, fresh one persisted. */
    ASSERT_EQ(1, bc_count_luac(tmp));

    lua_close(L);
    bc_cleanup_tmp_home(tmp);
}

/* ── Template cache ─────────────────────────────────────────────────
 *
 * Mirrors the bytecode-cache test layout but exercises
 * hl_lua_template_compile_cached. The cache stores the inner
 * render function (post-pcall), so a hit returns a callable
 * function directly. */

static void tc_with_tmp_home(char *tmpdir, size_t n)
{
    if (hl_test_path(tmpdir, n, "hull_tc_cache_XXXXXX") != 0 || !mkdtemp(tmpdir)) {
        fprintf(stderr, "tc_with_tmp_home: no usable temp dir\n");
        tmpdir[0] = 0;
        return;
    }
    setenv("HOME", tmpdir, 1);
    unsetenv("HULL_NO_CACHE");
    unsetenv("HULL_NO_TEMPLATE_CACHE");
    hl_lua_template_cache_reset();
}

static int tc_count(const char *dir)
{
    char root[512];
    snprintf(root, sizeof(root),
             "%s/.hull/blobs/runtime/templates/blobs", dir);
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

/* Stand-in for what hull.template's compile_source would produce:
 * a Lua chunk that returns an inner render function. Padded to
 * clear the 256-byte minimum. */
static const char *TC_PROBE_CODE =
    "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "return function(data)\n"
    "    local x = (data and data.x) or 0\n"
    "    return tostring(x * 2 + 3)\n"
    "end\n";

UTEST(lua_template_cache, miss_then_hit_populates_disk)
{
    char tmp[256];
    tc_with_tmp_home(tmp, sizeof tmp);

    lua_State *L = luaL_newstate();
    ASSERT_NE_MSG((void *)L, NULL, "newstate");
    /* The probe calls tostring(), so load stdlibs in the test state. */
    luaL_openlibs(L);

    ASSERT_EQ(0, tc_count(tmp));
    int rc = hl_lua_template_compile_cached(L, TC_PROBE_CODE,
                                            strlen(TC_PROBE_CODE),
                                            "=tpl_probe");
    ASSERT_EQ_MSG(rc, LUA_OK, "first compile");
    ASSERT_EQ(1, tc_count(tmp));

    /* The cache stores the render function directly - call it with
     * data and check the result is the right type. */
    lua_newtable(L);
    lua_pushinteger(L, 7);
    lua_setfield(L, -2, "x");
    ASSERT_EQ(LUA_OK, lua_pcall(L, 1, 1, 0));
    ASSERT_STREQ("17", lua_tostring(L, -1));
    lua_pop(L, 1);

    /* Second call: cache hit, no extra file. */
    rc = hl_lua_template_compile_cached(L, TC_PROBE_CODE,
                                        strlen(TC_PROBE_CODE),
                                        "=tpl_probe");
    ASSERT_EQ(LUA_OK, rc);
    ASSERT_EQ(1, tc_count(tmp));
    lua_newtable(L);
    lua_pushinteger(L, 7);
    lua_setfield(L, -2, "x");
    ASSERT_EQ(LUA_OK, lua_pcall(L, 1, 1, 0));
    ASSERT_STREQ("17", lua_tostring(L, -1));
    lua_pop(L, 1);

    lua_close(L);
    nftw(tmp, bc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(lua_template_cache, opt_out_via_env_skips_disk)
{
    char tmp[256];
    tc_with_tmp_home(tmp, sizeof tmp);
    setenv("HULL_NO_TEMPLATE_CACHE", "1", 1);
    hl_lua_template_cache_reset();

    lua_State *L = luaL_newstate();
    int rc = hl_lua_template_compile_cached(L, TC_PROBE_CODE,
                                            strlen(TC_PROBE_CODE),
                                            "=tpl_probe");
    ASSERT_EQ(LUA_OK, rc);
    ASSERT_EQ_MSG(0, tc_count(tmp),
                  "no entry written when opted out");
    lua_close(L);

    unsetenv("HULL_NO_TEMPLATE_CACHE");
    nftw(tmp, bc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(lua_template_cache, generated_code_change_invalidates)
{
    /* The cache is keyed by the generated code, NOT the template
     * name. Two different code strings produce two different
     * entries - the natural invalidation that the design relies on
     * when extends/include targets change. */
    char tmp[256];
    tc_with_tmp_home(tmp, sizeof tmp);

    lua_State *L = luaL_newstate();
    ASSERT_EQ(LUA_OK,
        hl_lua_template_compile_cached(L, TC_PROBE_CODE,
                                       strlen(TC_PROBE_CODE), "=t1"));
    lua_pop(L, 1);

    const char *alt =
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "return function(data)\n"
        "    return 'different output line one\\n'\n"
        "end\n";
    ASSERT_EQ(LUA_OK,
        hl_lua_template_compile_cached(L, alt, strlen(alt), "=t2"));
    lua_pop(L, 1);

    ASSERT_EQ_MSG(2, tc_count(tmp),
                  "distinct generated code produces distinct entries");

    lua_close(L);
    nftw(tmp, bc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(lua_template_cache, parse_error_returns_no_cache_write)
{
    char tmp[256];
    tc_with_tmp_home(tmp, sizeof tmp);

    const char *bad =
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
        "this is = not = valid Lua )(\n";

    lua_State *L = luaL_newstate();
    int rc = hl_lua_template_compile_cached(L, bad, strlen(bad), "=bad");
    ASSERT_NE_MSG(rc, LUA_OK, "parse error reported");
    ASSERT_TRUE(lua_isstring(L, -1));
    ASSERT_EQ(0, tc_count(tmp));
    lua_pop(L, 1);
    lua_close(L);

    nftw(tmp, bc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* ── app.X phase-gate (registration_closed) tests ─────────────────────
 *
 * After serve.c finishes wire_routes and sets
 * runtime.registration_closed = 1, the app.{get,post,...} / use /
 * use_post / ws / sse / every / daily bindings MUST throw a structured
 * Lua error.  Pre-flag-flip the same calls succeed (covered by the
 * many existing tests above - re-asserted here once for clarity).
 *
 * Implementation gate lives in lua_app_reject_if_serving(). */

/* ── the ssh byte-stream bridge ─────────────────────────────────────── */

#ifdef HL_ENABLE_HTTP

UTEST(lua_ssh_bridge, app_code_cannot_reach_the_byte_stream)
{
    /* The caller gate is what makes "the application never holds a socket"
     * true. The policy alone would not: it bounds WHERE a connection may go,
     * so an app could otherwise reach a permitted host and speak something
     * other than SSH over it. */
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "local s = require('hull.ssh._stream')\n"
        "s.connect({ host = 'spark.local', port = 22, user = 'operator' })\n");
    ASSERT_NE_MSG(rc, LUA_OK, "app code must be refused");
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_TRUE(strstr(err, "internal to the SSH module") != NULL);
    /* The message points somewhere useful rather than just saying no. */
    ASSERT_TRUE(strstr(err, "hull.ssh") != NULL);
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

/* Run a chunk under a stdlib name, which is how the bridge recognises trusted
 * callers. Safe to do here because this is C driving the runtime directly; the
 * chunk name cannot be forged from Lua (see lua_template_bridge above). */
static int run_as_stdlib(lua_State *L, const char *src)
{
    if (luaL_loadbuffer(L, src, strlen(src), "@hull.ssh.probe") != LUA_OK)
        return -1;
    return lua_pcall(L, 0, LUA_MULTRET, 0);
}

UTEST(lua_ssh_bridge, an_undeclared_manifest_denies_every_connect)
{
    /* Fails closed: no `ssh` key means no grant, even for the stdlib. */
    init_lua();
    int rc = run_as_stdlib(lua_rt.L,
        "local s = require('hull.ssh._stream')\n"
        "local h, err = s.connect({ host = 'spark.local', port = 22,\n"
        "                           user = 'operator' })\n"
        "assert(h == nil, 'a connection must not be handed out')\n"
        "return err\n");
    ASSERT_EQ_MSG(rc, LUA_OK, "the call itself should return, not raise");
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    /* The reason names the manifest key to add, not just "denied". */
    ASSERT_TRUE_MSG(strstr(err, "ssh") != NULL,
                    "the denial should name the capability");
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

/* Declare a manifest and apply its ssh grant to the runtime, the way
 * serve_cli.c does after load. app.manifest alone does not: the policy is
 * wired by the entry point, so without this every connect is refused as
 * "grants nothing" - which contains the words "hosts" and "users", and once
 * let host and user tests pass without ever reaching those rules. */
static HlManifest ssh_test_manifest;
static int        ssh_test_manifest_live;

static int ssh_declare(const char *src)
{
    if (luaL_dostring(lua_rt.L, src) != LUA_OK) return -1;
    if (ssh_test_manifest_live) hl_manifest_free(&ssh_test_manifest);
    ssh_test_manifest_live = 0;
    if (hl_manifest_extract_lua(lua_rt.L, &ssh_test_manifest,
                                lua_rt.base.alloc) != 0) return -1;
    ssh_test_manifest_live = 1;
    lua_rt.base.ssh_policy = &ssh_test_manifest.ssh;
    return 0;
}

static void ssh_undeclare(void)
{
    lua_rt.base.ssh_policy = NULL;
    if (ssh_test_manifest_live) hl_manifest_free(&ssh_test_manifest);
    ssh_test_manifest_live = 0;
}

UTEST(lua_ssh_bridge, a_host_outside_the_grant_is_denied)
{
    init_lua();
    ASSERT_EQ(ssh_declare(
        "app.manifest({ modules = { 'hull/ssh@1' },\n"
        "  ssh = { connect = { hosts = { 'spark.local' }, ports = { 22 },\n"
        "                      users = { 'operator' } } } })\n"), 0);

    int rc = run_as_stdlib(lua_rt.L,
        "local s = require('hull.ssh._stream')\n"
        "local h, err = s.connect({ host = 'evil.example.com', port = 22,\n"
        "                           user = 'operator' })\n"
        "assert(h == nil)\n"
        "return err\n");
    ASSERT_EQ(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "host is not in ssh.connect.hosts");
    lua_pop(lua_rt.L, 1);
    ssh_undeclare();
    cleanup_lua();
}

UTEST(lua_ssh_bridge, a_user_outside_the_grant_is_denied)
{
    /* The login is the part a reach grant cannot express, so it gets its own
     * case: the host and port here are both permitted. */
    init_lua();
    ASSERT_EQ(ssh_declare(
        "app.manifest({ modules = { 'hull/ssh@1' },\n"
        "  ssh = { connect = { hosts = { 'spark.local' }, ports = { 22 },\n"
        "                      users = { 'operator' } } } })\n"), 0);

    int rc = run_as_stdlib(lua_rt.L,
        "local s = require('hull.ssh._stream')\n"
        "local h, err = s.connect({ host = 'spark.local', port = 22,\n"
        "                           user = 'root' })\n"
        "assert(h == nil)\n"
        "return err\n");
    ASSERT_EQ(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "user is not in ssh.connect.users");
    lua_pop(lua_rt.L, 1);
    ssh_undeclare();
    cleanup_lua();
}

UTEST(lua_ssh_bridge, an_out_of_range_port_is_refused_not_wrapped)
{
    /* A cast to int used to wrap 2^32 + 22 to 22 - checked and dialled as
     * 22, consistently, but not the port that was asked for. */
    init_lua();
    ASSERT_EQ(ssh_declare(
        "app.manifest({ modules = { 'hull/ssh@1' },\n"
        "  ssh = { connect = { hosts = { 'spark.local' }, ports = { 22 },\n"
        "                      users = { 'operator' } } } })\n"), 0);
    const char *cases[] = {
        "return select(2, require('hull.ssh._stream').connect({ host = 'spark.local',"
        " port = 4294967318, user = 'operator' }))",
        "return select(2, require('hull.ssh._stream').connect({ host = 'spark.local',"
        " port = 22, user = 'operator', timeout_ms = -5 }))",
        "return select(2, require('hull.ssh._stream').connect({ host = 'spark.local',"
        " port = 22, user = 'operator', via = { host = 'r', port = 70000 } }))",
    };
    const char *want[] = { "port must be", "timeout_ms must be", "via.port must be" };
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(run_as_stdlib(lua_rt.L, cases[i]), LUA_OK);
        const char *err = lua_tostring(lua_rt.L, -1);
        EXPECT_TRUE_MSG(err && strstr(err, want[i]), cases[i]);
        lua_settop(lua_rt.L, 0);
    }
    ssh_undeclare();
    cleanup_lua();
}

/* Start `src` in a coroutine of its own, compiled under a stdlib chunk name,
 * the way the runtime starts a handler: active_co is what a park captures to
 * resume later. Returns lua_resume's status. A coroutine that finishes here is
 * unpinned here; one that yields is unpinned by hl_lua_async_resume when it
 * finishes. */
static int ssh_co_start(HlLua *lua, const char *src, lua_State **co_out)
{
    lua_State *L  = lua->L;
    lua_State *co = lua_newthread(L);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    *co_out = co;
    if (luaL_loadbuffer(co, src, strlen(src), "@hull.ssh.probe") != LUA_OK)
        return -1;
    lua->active_co         = co;
    lua->active_conn       = NULL;          /* detached, as under app.main */
    lua->active_thread_ref = ref;
    int nres = 0;
    int st = lua_resume(co, L, 0, &nres);
    if (st != LUA_YIELD) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua->active_co = NULL;
        lua->active_thread_ref = LUA_NOREF;
    }
    return st;
}

static void ssh_tick_until_done(const HlAsyncBackend *be, HlAsyncBackendCtx *ctx,
                                lua_State *co)
{
    for (int i = 0; i < 250 && lua_status(co) == LUA_YIELD; i++)
        be->tick(ctx, 20);
}

/* A stream the SSH stdlib has connected to a loopback listener, on a real
 * loop, with the manifest applied: what every bridge test below starts from.
 * The handle is the global H, shared by the coroutines a test starts. */
typedef struct {
    const HlAsyncBackend *be;
    int lfd, peer, port;
} SshBridgeFix;

/* Returns 0, or the step that failed. */
static int ssh_bridge_open(SshBridgeFix *bf)
{
    memset(bf, 0, sizeof *bf);
    bf->lfd = bf->peer = -1;
    bf->be = hl_async_backend();
    if (!bf->be) return 1;

    bf->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (bf->lfd < 0) return 2;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof a;
    if (bind(bf->lfd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(bf->lfd, 4) != 0 ||
        getsockname(bf->lfd, (struct sockaddr *)&a, &alen) != 0) return 3;
    bf->port = ntohs(a.sin_port);

    init_lua();
    if (bf->be->init(&lua_rt.base.async_ctx, NULL) != 0) return 4;
    if (bf->be->pool_create(&lua_rt.base.thread_pool, lua_rt.base.async_ctx,
                            2, 16) != 0) return 5;

    char src[320];
    snprintf(src, sizeof src,
        "app.manifest({ modules = { 'hull/ssh@1' },\n"
        "  ssh = { connect = { hosts = { '127.0.0.1' }, ports = { %d },\n"
        "                      users = { 'operator' } } } })\n", bf->port);
    if (ssh_declare(src) != 0) return 6;

    snprintf(src, sizeof src,
        "H = assert(require('hull.ssh._stream').connect{\n"
        "  host = '127.0.0.1', port = %d, user = 'operator' })\n", bf->port);
    lua_State *co;
    int st = ssh_co_start(&lua_rt, src, &co);
    if (st != LUA_OK && st != LUA_YIELD) {
        fprintf(stderr, "connect: %s\n", lua_tostring(co, -1));
        return 7;
    }
    ssh_tick_until_done(bf->be, lua_rt.base.async_ctx, co);
    if (lua_status(co) != LUA_OK) return 8;
    bf->peer = accept(bf->lfd, NULL, NULL);
    return bf->peer >= 0 ? 0 : 9;
}

/* Start `src` in its own coroutine and run the loop until it finishes.
 * Returns the coroutine, finished or not. */
static lua_State *ssh_bridge_run(SshBridgeFix *bf, const char *src)
{
    lua_State *co;
    int st = ssh_co_start(&lua_rt, src, &co);
    if (st == LUA_YIELD) ssh_tick_until_done(bf->be, lua_rt.base.async_ctx, co);
    return co;
}

static void ssh_bridge_close(SshBridgeFix *bf)
{
    /* Stream teardown runs through the backend, so the loop outlives it. */
    if (lua_initialized) run_as_stdlib(lua_rt.L, "if H then H:close() end; H = nil");
    HlAsyncBackendCtx  *actx = lua_rt.base.async_ctx;
    HlAsyncBackendPool *pool = lua_rt.base.thread_pool;
    ssh_undeclare();
    cleanup_lua();
    if (bf->be && actx) {
        bf->be->tick(actx, 0);
        if (pool) bf->be->pool_free(pool);
        bf->be->free(actx);
    }
    if (bf->peer >= 0) close(bf->peer);
    if (bf->lfd >= 0) close(bf->lfd);
}

UTEST(lua_ssh_bridge, a_read_size_is_not_an_allocation_size)
{
    /* The SSH layer once passed a peer-declared packet length straight
     * through, and the binding sized its buffer from it: 1 GiB here would
     * exceed the VM's memory limit. */
    SshBridgeFix bf;
    ASSERT_EQ(ssh_bridge_open(&bf), 0);
    ASSERT_EQ(send(bf.peer, "hi", 2, 0), (ssize_t)2);
    lua_State *co = ssh_bridge_run(&bf, "return H:read(1 << 30)\n");
    EXPECT_EQ_MSG(lua_status(co), LUA_OK, "a huge read size must not fail");
    EXPECT_STREQ(lua_tostring(co, -1), "hi");
    ssh_bridge_close(&bf);
}

UTEST(lua_ssh_bridge, a_second_waiter_on_one_stream_is_refused_not_parked)
{
    /* Two coroutines sharing one connection - two requests using a
     * module-level SSH connection, say. The stream has a single park slot. A
     * second read used to overwrite it: the first coroutine was never resumed
     * again, and a second WRITE also replaced the first writer's anchored
     * bytes, which the first writer's retry would then have sent. */
    SshBridgeFix bf;
    ASSERT_EQ(ssh_bridge_open(&bf), 0);

    /* B: nothing has arrived, so the read parks. */
    lua_State *co_b, *co_c;
    ASSERT_EQ_MSG(ssh_co_start(&lua_rt, "return H:read(16)\n", &co_b), LUA_YIELD,
                  "the first reader parks");

    /* C: the stream already has a waiter. Refused at once, not parked. */
    int st = ssh_co_start(&lua_rt,
        "local d, e = H:read(16)\n"
        "assert(d == nil, 'a second reader must not get data')\n"
        "return e\n", &co_c);
    EXPECT_EQ_MSG(st, LUA_OK, "the second reader must be refused, not parked");
    if (st == LUA_OK) {
        const char *err = lua_tostring(co_c, -1);
        EXPECT_TRUE_MSG(err && strstr(err, "busy"), "the refusal says why");
    }

    /* B is still the one that gets the data: its park survived C. */
    ASSERT_EQ(send(bf.peer, "ping", 4, 0), (ssize_t)4);
    ssh_tick_until_done(bf.be, lua_rt.base.async_ctx, co_b);
    EXPECT_EQ_MSG(lua_status(co_b), LUA_OK, "the first reader resumes");
    if (lua_status(co_b) == LUA_OK)
        EXPECT_STREQ(lua_tostring(co_b, -1), "ping");
    ssh_bridge_close(&bf);
}

UTEST(lua_ssh_bridge, app_code_holding_a_stream_is_refused)
{
    /* Only connect used to check its caller, so a handle that leaked (it did,
     * as conn.t.stream) could read and write raw bytes to a granted host. */
    SshBridgeFix bf;
    ASSERT_EQ(ssh_bridge_open(&bf), 0);
    const char *app_calls[] = { "return H:read(1)", "return H:write('x')",
                                "return H:close()", "return H:wait(1)",
                                "return H:deadline(1)" };
    for (size_t i = 0; i < sizeof app_calls / sizeof app_calls[0]; i++) {
        int rc = luaL_dostring(lua_rt.L, app_calls[i]);
        EXPECT_NE_MSG(rc, LUA_OK, app_calls[i]);
        if (rc != LUA_OK) {
            const char *err = lua_tostring(lua_rt.L, -1);
            EXPECT_TRUE_MSG(err && strstr(err, "internal to the SSH module"),
                            app_calls[i]);
        }
        lua_settop(lua_rt.L, 0);
    }
    ssh_bridge_close(&bf);
}

UTEST(lua_ssh_bridge, a_bounded_wait_comes_back_coded_and_the_stream_lives)
{
    /* A quiet server used to hold a read forever. The wait bound returns it
     * as (nil, message, "timeout"); a deadline as "deadline"; and the stream
     * still reads afterwards, because what an expiry means is the protocol
     * layer's decision. */
    SshBridgeFix bf;
    ASSERT_EQ(ssh_bridge_open(&bf), 0);

    lua_State *co = ssh_bridge_run(&bf,
        "H:wait(50)\n"
        "local d, m, code = H:read(8)\n"
        "return code\n");
    ASSERT_EQ(lua_status(co), LUA_OK);
    EXPECT_STREQ(lua_tostring(co, -1), "timeout");

    co = ssh_bridge_run(&bf,
        "H:wait(0); H:deadline(50)\n"
        "local d, m, code = H:read(8)\n"
        "return code\n");
    ASSERT_EQ(lua_status(co), LUA_OK);
    EXPECT_STREQ(lua_tostring(co, -1), "deadline");

    ASSERT_EQ(send(bf.peer, "ok", 2, 0), (ssize_t)2);
    co = ssh_bridge_run(&bf, "H:deadline(0)\nreturn H:read(8)\n");
    ASSERT_EQ(lua_status(co), LUA_OK);
    EXPECT_STREQ(lua_tostring(co, -1), "ok");

    /* A bound outside 0..24 h is a caller error, not a silent clamp. */
    EXPECT_NE(run_as_stdlib(lua_rt.L, "H:wait(-1)"), LUA_OK);
    lua_settop(lua_rt.L, 0);
    ssh_bridge_close(&bf);
}

UTEST(lua_ssh_bridge, a_known_hosts_line_hashed_by_openssh_is_matched)
{
    /* The line below is ssh-keygen's own: a fresh ed25519 key recorded for
     * web1.internal, then `ssh-keygen -H`. The unit suite checks the format
     * with a stand-in HMAC; this checks it against OpenSSH, through the real
     * hull.crypto, which is the only way to know the two agree. */
    init_lua();
    ASSERT_EQ(ssh_declare("app.manifest({ modules = { 'hull/ssh@1', 'hull/crypto@1' } })"), 0);
    int rc = run_as_stdlib(lua_rt.L,
        "local ssh = require('hull.ssh')\n"
        "local key = 'AAAAC3NzaC1lZDI1NTE5AAAAIIVv0Mu44/XjxMDB01Pok3Zpil+AvjX4noCS12D4xykm'\n"
        "local line = '|1|Ni3Ir1hENwtFHCWUUHN1PzRJcIw=|L939wIPq3PCppqUZb9UObH57zdQ= "
        "ssh-ed25519 ' .. key\n"
        "local fs = { read = function() return line .. '\\n' end,\n"
        "             write = function() return true end }\n"
        "local st = ssh.file_store('known_hosts', { fs = fs })\n"
        "local want = require('hull.encoding').base64.decode(key)\n"
        "assert(st.get('web1.internal') == want, 'the hashed entry must match its host')\n"
        "assert(st.get('web2.internal') == nil, 'and nothing else')\n"
        "return 'ok'\n");
    if (rc != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(lua_rt.L, -1));
    ASSERT_EQ(rc, LUA_OK);
    EXPECT_STREQ(lua_tostring(lua_rt.L, -1), "ok");
    lua_settop(lua_rt.L, 0);
    ssh_undeclare();
    cleanup_lua();
}

UTEST(lua_ssh_bridge, a_trust_store_over_the_real_hull_kv)
{
    /* The unit suite uses a stand-in with hull.kv's semantics; this is the
     * real memory backend, so a change to cas or scan cannot drift past it. */
    init_lua();
    ASSERT_EQ(ssh_declare("app.manifest({ modules = { 'hull/ssh@1', 'hull/kv@1' } })"), 0);
    int rc = run_as_stdlib(lua_rt.L,
        "local ssh = require('hull.ssh')\n"
        "local kv = require('hull.kv').open{ backend = 'memory', namespace = 'ssh-trust-test' }\n"
        "local st = ssh.kv_store(kv)\n"
        "-- ssh-ed25519 public key blobs: accept_host refuses anything else\n"
        "local function ed(b) return string.pack('>s4s4', 'ssh-ed25519', string.rep(b, 32)) end\n"
        "local blob = ed('1')\n"
        "assert(ssh.accept_host(st, 'web1', blob) == true)\n"
        "assert(st.get('web1') == blob)\n"
        "local ok, err = ssh.accept_host(st, 'web1', ed('2'))\n"
        "assert(ok == nil and err.code == 'already_trusted', 'no overwrite')\n"
        "assert(st.entries().web1 == blob)\n"
        "assert(ssh.forget_host(st, 'web1') == true and st.get('web1') == nil)\n"
        "return 'ok'\n");
    if (rc != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(lua_rt.L, -1));
    ASSERT_EQ(rc, LUA_OK);
    EXPECT_STREQ(lua_tostring(lua_rt.L, -1), "ok");
    lua_settop(lua_rt.L, 0);
    ssh_undeclare();
    cleanup_lua();
}

UTEST(lua_ssh_bridge, a_manifest_denial_carries_its_code)
{
    /* So the SSH layer reports `denied` for the manifest and only for it. */
    init_lua();
    ASSERT_EQ(ssh_declare(
        "app.manifest({ modules = { 'hull/ssh@1' },\n"
        "  ssh = { connect = { hosts = { 'spark.local' }, ports = { 22 },\n"
        "                      users = { 'operator' } } } })\n"), 0);
    ASSERT_EQ(run_as_stdlib(lua_rt.L,
        "return select(3, require('hull.ssh._stream').connect({\n"
        "  host = 'elsewhere', port = 22, user = 'operator' }))"), LUA_OK);
    EXPECT_STREQ(lua_tostring(lua_rt.L, -1), "denied");
    lua_settop(lua_rt.L, 0);
    ssh_undeclare();
    cleanup_lua();
}

#endif /* HL_ENABLE_HTTP */

/* ── chunk names decide trust, so they are not caller-supplied ────── */

UTEST(lua_template_bridge, compile_cannot_forge_a_stdlib_chunk_name)
{
    /* hl_lua_source_is_stdlib grants _hull_* table access to any chunk whose
     * name starts with "hull.". _template._compile is reachable from app code
     * (require's capability gate only fires for names in the module registry,
     * and "hull._template" is not one), so if it took a chunk name verbatim,
     * app code could mint a chunk that claims to be stdlib.
     *
     * The name is built instead. Whatever the caller asks for lands after a
     * "=template:" prefix, so the result cannot begin with "hull.". */
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "local t = require('hull._template')\n"
        "local f = t._compile('return function() error(\"boom\") end',\n"
        "                     'hull.forged')\n"
        "local ok, err = pcall(f)\n"
        "assert(not ok, 'the probe chunk must raise')\n"
        "return err\n");
    ASSERT_EQ(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);

    /* Lua prefixes a runtime error with the chunk name, so the error text
     * reports what the chunk ended up being called. */
    ASSERT_TRUE_MSG(strstr(err, "template:") != NULL,
                    "chunk name should carry the forced template prefix");
    ASSERT_TRUE_MSG(strncmp(err, "hull.", 5) != 0,
                    "chunk must not be named as stdlib");

    /* The requested name is kept, just demoted - error messages stay useful. */
    ASSERT_TRUE_MSG(strstr(err, "hull.forged") != NULL,
                    "the caller's name should still appear");
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

UTEST(lua_template_bridge, compile_strips_a_caller_supplied_marker)
{
    /* A leading "@" or "=" is a chunkname marker. Left in place it would land
     * in the middle of the built name, so it is stripped before prefixing. */
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "local t = require('hull._template')\n"
        "local f = t._compile('return function() error(\"boom\") end',\n"
        "                     '@hull.forged')\n"
        "local ok, err = pcall(f)\n"
        "assert(not ok)\n"
        "return err\n");
    ASSERT_EQ(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_TRUE_MSG(strstr(err, "template:hull.forged") != NULL,
                    "marker stripped, name kept after the prefix");
    ASSERT_TRUE(strstr(err, "template:@") == NULL);
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

UTEST(lua_template_bridge, compile_without_a_name_still_works)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "local t = require('hull._template')\n"
        "local f = t._compile('return function() return 1 end')\n"
        "assert(type(f) == 'function')\n"
        "assert(f() == 1)\n");
    ASSERT_EQ_MSG(rc, LUA_OK, "a nameless compile keeps working");
    cleanup_lua();
}

UTEST(lua_runtime, app_get_rejected_after_registration_closed)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\n");
    ASSERT_EQ(rc, LUA_OK);

    /* Simulate the post-wire_routes / post-serve-loop entry state.
     * In production this is set by hl_serve_wire_routes via the
     * runtime pointer. */
    lua_rt.base.registration_closed = 1;

    rc = luaL_dostring(lua_rt.L,
        "app.get('/late', function(req, res) res:json({late=true}) end)\n");
    ASSERT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    /* Error names the call and explains the rule.  Substring asserts
     * cover both pieces without locking the wording too tightly. */
    ASSERT_TRUE(strstr(err, "app.get") != NULL);
    ASSERT_TRUE(strstr(err, "app startup") != NULL);
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

UTEST(lua_runtime, app_use_rejected_after_registration_closed)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\n");
    ASSERT_EQ(rc, LUA_OK);
    lua_rt.base.registration_closed = 1;
    rc = luaL_dostring(lua_rt.L,
        "app.use('*', '/api/*', function(req, res) return 0 end)\n");
    ASSERT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_TRUE(err != NULL && strstr(err, "app.use") != NULL);
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

UTEST(lua_runtime, app_every_rejected_after_registration_closed)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1', 'hull/timers@1'}})\n");
    ASSERT_EQ(rc, LUA_OK);
    lua_rt.base.registration_closed = 1;
    /* Defense against a recursive registration: a timer callback that
     * tries to install another timer must fail just like a fresh
     * registration call would. */
    rc = luaL_dostring(lua_rt.L,
        "app.every(5000, function() end)\n");
    ASSERT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_TRUE(err != NULL && strstr(err, "app.every") != NULL);
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

UTEST(lua_runtime, app_get_allowed_before_registration_closed)
{
    /* Sanity-check the boot-phase path: same registration succeeds
     * pre-flag-flip.  Guards against an over-eager gate that fires
     * during top-level execution. */
    init_lua();
    /* Flag defaults to 0 from memset in init_lua. */
    ASSERT_EQ(lua_rt.base.registration_closed, 0);
    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\n"
        "app.get('/ok', function(req, res) res:json({ok=true}) end)\n");
    ASSERT_EQ(rc, LUA_OK);
    cleanup_lua();
}


/* ── hull.async tasks, hull.gather, hull.map (docs/task_join_design.md) ──
 *
 * Driven on a real loop, as under app.main: each script runs on its own
 * coroutine (ssh_co_start), the loop ticks until it finishes, and the script
 * leaves its verdict in the global OUT ("ok", or what went wrong).
 * Concurrency is measured, not timed: a counter of items in flight and its
 * peak, so a slow CI runner cannot make these flaky. */

static const char TASK_PRELUDE[] =
    "local inflight, peak = 0, 0\n"
    "local function busy(ms)\n"
    "  inflight = inflight + 1\n"
    "  if inflight > peak then peak = inflight end\n"
    "  hull.sleep(ms)\n"
    "  inflight = inflight - 1\n"
    "end\n"
    "local function check(c, what) if not c then error(what, 2) end end\n";

/* Returns 0, or the step that failed; OUT is left in `out`. */
static int run_task_script(const char *body, char *out, size_t outsz)
{
    const HlAsyncBackend *be = hl_async_backend();
    if (!be) return 1;
    init_lua();
    if (be->init(&lua_rt.base.async_ctx, NULL) != 0) return 2;

    size_t n = strlen(TASK_PRELUDE) + strlen(body) + 64;
    char *src = malloc(n);
    if (!src) return 3;
    snprintf(src, n, "%s%s\nOUT = 'ok'\n", TASK_PRELUDE, body);
    lua_State *co;
    int st = ssh_co_start(&lua_rt, src, &co);
    free(src);
    if (st == LUA_YIELD) ssh_tick_until_done(be, lua_rt.base.async_ctx, co);
    st = lua_status(co);

    lua_getglobal(lua_rt.L, "OUT");
    const char *o = lua_tostring(lua_rt.L, -1);
    snprintf(out, outsz, "%s", o ? o
             : (st != LUA_OK && lua_tostring(co, -1)) ? lua_tostring(co, -1)
             : "(no verdict)");
    lua_pop(lua_rt.L, 1);

    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();
    be->tick(actx, 0);
    be->free(actx);
    return 0;
}

#define TASK_CASE(name, body)                                   \
    UTEST(lua_async, name)                                      \
    {                                                           \
        char out[512];                                          \
        ASSERT_EQ(run_task_script(body, out, sizeof out), 0);   \
        ASSERT_STREQ(out, "ok");                                \
    }

TASK_CASE(gather_runs_concurrently_and_keeps_order,
    "local a, b, c = hull.gather(\n"
    "  function() busy(60); return 'a' end,\n"
    "  function() busy(20); return 'b' end,\n"
    "  function() busy(40); return 'c' end)\n"
    "check(a == 'a' and b == 'b' and c == 'c', 'order: '..tostring(a)..tostring(b)..tostring(c))\n"
    "check(peak == 3, 'peak '..peak)\n")

TASK_CASE(map_respects_the_limit_and_keeps_order,
    "local items = {}\n"
    "for i = 1, 10 do items[i] = i end\n"
    "local r = hull.map(items, function(x, i) busy(10 + (11 - x)); return x * x, 'extra' end,\n"
    "                   { limit = 3 })\n"
    "check(peak == 3, 'peak '..peak)\n"
    "for i = 1, 10 do check(r[i] == i * i, 'r['..i..']='..tostring(r[i])) end\n")

TASK_CASE(map_default_limit_is_16,
    "local items = {}\n"
    "for i = 1, 40 do items[i] = i end\n"
    "hull.map(items, function() busy(5) end)\n"
    "check(peak == 16, 'peak '..peak)\n")

TASK_CASE(map_of_nothing_is_empty,
    "local r = hull.map({}, function() error('never') end)\n"
    "check(r.n == 0 and next(r, 'n') == nil and r[1] == nil, 'not empty')\n")

TASK_CASE(a_failure_waits_for_the_rest_and_reports_all,
    "local finished = 0\n"
    "local items = {}\n"
    "for i = 1, 8 do items[i] = i end\n"
    "local ok, err = pcall(hull.map, items, function(x)\n"
    "  busy(10)\n"
    "  if x == 2 or x == 5 then error('bad '..x, 0) end\n"
    "  finished = finished + 1\n"
    "end, { limit = 4 })\n"
    "check(not ok, 'did not raise')\n"
    "check(finished == 6, 'finished '..finished)\n"
    "check(err.errors[2] == 'bad 2' and err.errors[5] == 'bad 5', 'errors')\n"
    "check(tostring(err) == 'bad 2', 'message '..tostring(err))\n"
    "check(inflight == 0, 'still in flight '..inflight)\n")

TASK_CASE(gather_reports_failures_by_position,
    "local ok, err = pcall(hull.gather,\n"
    "  function() busy(10); return 1 end,\n"
    "  function() busy(5); error({ code = 'x' }) end)\n"
    "check(not ok and err.errors[2].code == 'x', 'errors')\n"
    "check(err.errors[1] == nil, 'first did not fail')\n")

TASK_CASE(wait_returns_every_value_including_nils,
    "local t = hull.async(function(a, b) busy(10); return a, nil, b, nil end, 'x', 'y')\n"
    "local r = table.pack(t:wait())\n"
    "check(r.n == 4 and r[1] == 'x' and r[2] == nil and r[3] == 'y', 'values')\n"
    "check(t:done(), 'not done')\n"
    "local again = table.pack(t:wait())\n"
    "check(again.n == 4 and again[3] == 'y', 'second wait')\n")

TASK_CASE(wait_reraises_the_task_error,
    "local t = hull.async(function() busy(5); error('boom', 0) end)\n"
    "local ok, e = pcall(t.wait, t)\n"
    "check(not ok and e == 'boom', 'got '..tostring(e))\n")

TASK_CASE(several_waiters_on_one_task,
    "local t = hull.async(function() busy(30); return 42 end)\n"
    "local a, b = hull.gather(function() return t:wait() end,\n"
    "                         function() return t:wait() end)\n"
    "check(a == 42 and b == 42, 'waiters')\n")

TASK_CASE(tasks_nest,
    "local outer = hull.async(function()\n"
    "  local x, y = hull.gather(function() busy(10); return 1 end,\n"
    "                           function() busy(10); return 2 end)\n"
    "  return x + y\n"
    "end)\n"
    "check(outer:wait() == 3, 'nested')\n")

TASK_CASE(a_task_that_never_yields_is_already_done,
    "local t = hull.async(function() return 'now' end)\n"
    "check(t:done() and t:wait() == 'now', 'sync task')\n")

TASK_CASE(the_running_count_returns_to_zero,
    "hull.map({1, 2, 3}, function() busy(5) end)\n"
    "local t = hull.async(function() busy(5) end)\n"
    "check(hull._running == 1, 'running '..hull._running)\n"
    "t:wait()\n"
    "check(hull._running == 0, 'running after '..hull._running)\n")

TASK_CASE(bad_arguments_are_refused,
    "check(not pcall(hull.async, 42), 'async')\n"
    "check(not pcall(hull.gather, function() end, 'x'), 'gather')\n"
    "check(not pcall(hull.map, {}, function() end, { limit = 0 }), 'limit 0')\n"
    "check(not pcall(hull.map, 'x', function() end), 'items')\n"
    "check(pcall(hull.map, {1}, function() end, { limit = math.huge }), 'huge')\n")

TASK_CASE(map_honours_n_and_reports_it,
    "local r = hull.map(table.pack(1, nil, 3), function(x) return x end)\n"
    "check(r.n == 3 and r[1] == 1 and r[2] == nil and r[3] == 3, 'n '..tostring(r.n))\n"
    "check(#hull.map({1, 2}, function(x) return x end, { limit = 2.0 }) == 2, 'integral float limit')\n"
    "check(not pcall(hull.map, {1}, function() end, { limit = 1.5 }), 'fractional limit')\n"
    "check(not pcall(hull.map, {1}, function() end, 5), 'opts not a table')\n"
    "check(not pcall(hull.map, {1}, function() end, { limit = false }), 'limit false (as in JS)')\n")

TASK_CASE(an_error_with_no_value_still_counts,
    "local ok, err = pcall(hull.gather, function() busy(5) end, function() error() end)\n"
    "check(not ok and err.errors[2] == '(error with no value)', 'errors[2] '..tostring(err and err.errors and err.errors[2]))\n"
    "check(tostring(err) == '(error with no value)', 'message '..tostring(err))\n")

/* A wait can only suspend the coroutine the runtime drives. Where it cannot
 * yield at all (a C callback) or would suspend some other coroutine (one the
 * app made), it raises before anything is armed - it used to arm the op
 * first, then fail in lua_yieldk, and later resume the wrong coroutine. */
TASK_CASE(a_wait_where_it_cannot_yield_raises_at_once,
    "local function refused(f, what)\n"
    "  local ok, e = pcall(f)\n"
    "  check(not ok and tostring(e):find('can only wait', 1, true), what..': '..tostring(e))\n"
    "end\n"
    "refused(function() string.gsub('a', 'a', function() hull.sleep(5) end) end, 'gsub sleep')\n"
    "refused(coroutine.wrap(function() hull.sleep(5) end), 'own coroutine')\n"
    "local t = hull.async(function() busy(5); return 1 end)\n"
    "refused(function() string.gsub('a', 'a', function() t:wait() end) end, 'gsub wait')\n"
    "check(t:wait() == 1, 'the task is still joinable')\n")

static int oc_calls;
static void count_oc(struct HlLua *lua, void *ctx)
{
    (void)lua;
    (void)ctx;
    oc_calls++;
}

/* A task starts with none of its spawner's per-dispatch context - here the
 * deferred-teardown hook a ws on_close handler carries. The handler's own
 * completion runs it once; the task's completion used to run it again (in a
 * real on_close, a second teardown of a freed connection). */
UTEST(lua_async, a_task_does_not_inherit_its_spawners_teardown_hook)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);

    oc_calls = 0;
    lua_rt.active_on_complete     = count_oc;
    lua_rt.active_on_complete_ctx = NULL;
    lua_State *co;
    int st = ssh_co_start(&lua_rt,
        "local t = hull.async(function() hull.sleep(5); return 7 end)\n"
        "OUT = (t:wait() == 7) and 'ok' or 'bad'\n", &co);
    lua_rt.active_on_complete = NULL;
    ASSERT_EQ(st, LUA_YIELD);
    ssh_tick_until_done(be, lua_rt.base.async_ctx, co);

    lua_getglobal(lua_rt.L, "OUT");
    const char *out = lua_tostring(lua_rt.L, -1);
    EXPECT_STREQ(out ? out : "(no verdict)", "ok");
    lua_pop(lua_rt.L, 1);
    EXPECT_EQ(oc_calls, 1);

    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();
    be->tick(actx, 0);
    be->free(actx);
}

/* A timer handler that waits more than once keeps its timer: the last wait's
 * continuation still clears in_flight and reschedules, so the handler runs
 * again. From the second wait on it used to be lost, and the timer never
 * fired again - every gather / map in a timer handler waits more than once.
 * The handler cancels itself (returns false) on its third run, so nothing is
 * left pending at the end. */
UTEST(lua_async, a_timer_handler_keeps_its_timer_across_waits)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);

    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "RUNS = 0\n"
        "return function()\n"
        "  RUNS = RUNS + 1\n"
        "  hull.sleep(5)\n"
        "  hull.sleep(5)\n"
        "  if RUNS >= 3 then return false end\n"
        "end\n"), LUA_OK);
    lua_newtable(L);
    lua_insert(L, -2);
    lua_rawseti(L, -2, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_timers");

    static HlLuaTimer t;
    memset(&t, 0, sizeof t);
    t.lua         = &lua_rt;
    t.handler_id  = 1;
    t.interval_ms = 20;
    hl_lua_timer_trampoline(&t);

    int runs = 0;
    for (int i = 0; i < 150 && (runs < 3 || t.in_flight); i++) {
        be->tick(lua_rt.base.async_ctx, 20);
        lua_getglobal(L, "RUNS");
        runs = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    EXPECT_EQ(runs, 3);
    EXPECT_EQ(t.in_flight, 0);

    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();
    be->tick(actx, 0);
    be->free(actx);
}

/* A timer may fire while a task is suspended. The trampoline used to
 * assert(dispatch_depth == 0), and every suspended handler or task holds that
 * above zero - so the first tick with a task in flight aborted the process. */
UTEST(lua_async, a_timer_fires_while_a_task_waits)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);

    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "TICKS = 0\n"
        "return function() TICKS = TICKS + 1; return false end\n"), LUA_OK);
    lua_newtable(L);
    lua_insert(L, -2);
    lua_rawseti(L, -2, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_timers");

    lua_State *co;
    int st = ssh_co_start(&lua_rt,
        "local t = hull.async(function() hull.sleep(60) end)\n"
        "t:wait()\n"
        "OUT = 'ok'\n", &co);
    ASSERT_EQ(st, LUA_YIELD);

    static HlLuaTimer t;
    memset(&t, 0, sizeof t);
    t.lua         = &lua_rt;
    t.handler_id  = 1;
    t.interval_ms = 20;
    hl_lua_timer_trampoline(&t);    /* main and its task are both suspended */

    ssh_tick_until_done(be, lua_rt.base.async_ctx, co);
    lua_getglobal(L, "TICKS");
    EXPECT_EQ(lua_tointeger(L, -1), 1);
    lua_pop(L, 1);
    lua_getglobal(L, "OUT");
    const char *out = lua_tostring(L, -1);
    EXPECT_STREQ(out ? out : "(no verdict)", "ok");
    lua_pop(L, 1);

    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();
    be->tick(actx, 0);
    be->free(actx);
}

/* Error values reach the logs as text: a table with __tostring (hull.gather's
 * aggregate) as its message, where lua_tostring gave NULL - "(unknown)", or a
 * NULL for "%s". Also from a coroutine that died with it, and without letting
 * a __tostring that raises escape. */
UTEST(lua_runtime, error_text_reads_error_objects)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    char buf[64];
    int top = lua_gettop(L);

    lua_pushliteral(L, "plain");
    EXPECT_STREQ(hl_lua_error_text(&lua_rt, L, -1, buf, sizeof buf), "plain");
    lua_pop(L, 1);

    lua_pushnil(L);
    EXPECT_STREQ(hl_lua_error_text(&lua_rt, L, -1, buf, sizeof buf), "nil");
    lua_pop(L, 1);

    ASSERT_EQ(luaL_dostring(L, "return setmetatable({}, {__tostring = "
                               "function() return 'from tostring' end})"), LUA_OK);
    EXPECT_STREQ(hl_lua_error_text(&lua_rt, L, -1, buf, sizeof buf), "from tostring");
    lua_pop(L, 1);

    ASSERT_EQ(luaL_dostring(L, "return setmetatable({}, {__tostring = "
                               "function() error('boom') end})"), LUA_OK);
    EXPECT_STREQ(hl_lua_error_text(&lua_rt, L, -1, buf, sizeof buf),
                 "(table error value)");
    lua_pop(L, 1);
    EXPECT_EQ(lua_gettop(L), top);

    lua_State *co = lua_newthread(L);
    ASSERT_EQ(luaL_loadstring(co, "error(setmetatable({}, {__tostring = "
                                  "function() return 'died' end}))"), LUA_OK);
    int nres = 0;
    EXPECT_EQ(lua_resume(co, L, 0, &nres), LUA_ERRRUN);
    EXPECT_STREQ(hl_lua_error_text(&lua_rt, co, -1, buf, sizeof buf), "died");
    lua_pop(L, 1);                  /* the thread */
    EXPECT_EQ(lua_gettop(L), top);

    cleanup_lua();
}

UTEST_MAIN();
