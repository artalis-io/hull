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

#include "../../client_ip_matrix.h"
#include "utest.h"
#include "hull/runtime/lua.h"
#include "hull/runtime/lua_bytecode_cache.h"
#include "hull/runtime/test.h"           /* hl_lua_test_clear / _run */
#include "hull/shared/cache_dir.h"   /* hl_hull_cache_dir / _subdir */
#include "hull/shared/host.h"        /* hl_host_is_windows */
#include "hull/runtime/lua_template_cache.h"
#include "hull/runtime/cache_common.h"
#include "hull/shared/blob_store.h"
#include "hull/reqctx.h"
#include "hull/vfs.h"
#include "hull/stdlib_feature.h"
#include "hull/cap/db.h"

#include "../../lua_script_test.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"
#include "hull/cap/db_registry.h"
#include "hull/cap/env.h"
#include "hull/cap/tool.h"          /* HlToolUnveilCtx (tool-VM sandbox test) */

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
#include <time.h>
#include "hull/shared/async_backend.h"
#include "log.h"                /* log_add_callback (lua_task tests) */
#include "hull/shared/async.h"      /* HlAsyncCont */
#include "hull/worker_db.h"   /* hl_deep_copy_params */
#include "hull/utils/alloc.h"   /* hl_free_const */
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

/* A loop + pool for the NEXT init_lua: hull.worker registers only when a
 * thread pool exists at init. Consumed (cleared) by init_lua. */
static HlAsyncBackendCtx  *pending_async_ctx;
static HlAsyncBackendPool *pending_thread_pool;

static void init_lua(void)
{
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    if (lua_initialized)
        hl_lua_free(&lua_rt);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    memset(&lua_rt, 0, sizeof(lua_rt));
    lua_rt.base.platform_vfs = &platform_vfs;
    lua_rt.base.async_ctx   = pending_async_ctx;
    lua_rt.base.thread_pool = pending_thread_pool;
    pending_async_ctx   = NULL;
    pending_thread_pool = NULL;
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
    if (rctx == &hl_lua_req_ctx_marker) {   /* a middleware's table */
        if (lua_initialized) hl_lua_req_ctx_drop(lua_rt.L, req);
        req->ctx = NULL;
        return;
    }
    if (rctx->kind == HL_REQCTX_JSON)
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
    lua_rt.base.async_ctx   = pending_async_ctx;
    lua_rt.base.thread_pool = pending_thread_pool;
    pending_async_ctx   = NULL;
    pending_thread_pool = NULL;
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

    /* vendor.* is internal to the stdlib (audit 5 M6): app code cannot
     * require it; the stdlib's own json still works. */
    int result = eval_int(
        "(function() local ok = pcall(require, 'vendor.json') "
        "return ok and 0 or 1 end)()");
    ASSERT_EQ(result, 1);

    cleanup_lua();
}

/* Round-7 H3: the CLI plugins (stdlib/cli/lua) are not loaded into an app
 * VM, and a hull.* name outside the registry is refused - whoever asks.
 * hull.project.registry.load used to require a module name its caller
 * passed, with the stdlib's identity, which reached hull._template and
 * hull.db._internal_conn from app code. */
static int expose_stdlib_require(lua_State *L)
{
    const char *src =
        "__sr = function(n) return pcall(function() return require(n) end) end";
    if (luaL_loadbuffer(L, src, strlen(src), "@hull.tests.stdlib_require") != LUA_OK)
        return -1;
    return lua_pcall(L, 0, 0, 0);
}

UTEST(lua_runtime, require_cli_plugin_refused)
{
    init_lua();
    ASSERT_EQ(expose_stdlib_require(lua_rt.L), LUA_OK);
    int result = eval_int(
        "(function() "
        "  local a = pcall(function() return require('hull.project.registry') end) "
        "  local b = pcall(function() return require('hull.build') end) "
        "  local c = pcall(function() return require('hull.no_such_module') end) "
        /* not even from a stdlib chunk: the plugins are not in this VM */
        "  local d = __sr('hull.project.registry') "
        "  local e = __sr('hull.source.lua') "
        "  return (not a and not b and not c and not d and not e) and 1 or 0 "
        "end)()");
    ASSERT_EQ(result, 1);

    /* the refusal names the rule, not a lookup miss */
    int rc = luaL_dostring(lua_rt.L, "require('hull.project.registry')");
    ASSERT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    ASSERT_NE(strstr(err, "not a Hull module"), NULL);
    lua_pop(lua_rt.L, 1);

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

    /* The trip is sticky until the next entry arms the budget again (each
     * run - a request, a resume, a timer - gets the whole limit). */
    rc = luaL_dostring(limited_lua.L, "return 1 + 1");
    ASSERT_NE(rc, LUA_OK);
    lua_pop(limited_lua.L, 1);
    HL_LUA_ARM(&limited_lua, limited_lua.L);

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

/* http = { timeout_ms = N }: the app-wide outbound HTTP timeout. As for the
 * wasm limits, an integer of at least 1 counts (capped at INT32_MAX, so it
 * fits HlHttpConfig.timeout_ms); anything else is absent (0 = 30 s). */
UTEST(lua_runtime, manifest_http_timeout)
{
    static const struct { const char *decl; uint32_t want; } cases[] = {
        { "app.manifest({ http = { timeout_ms = 1500 } })",       1500 },
        { "app.manifest({ http = { timeout_ms = 0 } })",          0 },
        { "app.manifest({ http = { timeout_ms = -1 } })",         0 },
        { "app.manifest({ http = { timeout_ms = '9' } })",        0 },
        { "app.manifest({ http = { timeout_ms = 1.5 } })",        0 },
        { "app.manifest({ http = { timeout_ms = 1 << 40 } })",    (uint32_t)INT32_MAX },
        { "app.manifest({ http = 5000 })",                        0 },
        { "app.manifest({ hosts = { 'a.test' } })",               0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        init_lua();
        ASSERT_TRUE(lua_initialized);
        ASSERT_EQ(luaL_dostring(lua_rt.L, cases[i].decl), LUA_OK);
        HlManifest m;
        ASSERT_EQ(hl_manifest_extract_lua(lua_rt.L, &m, NULL), 0);
        EXPECT_EQ_MSG(m.http_timeout_ms, cases[i].want, cases[i].decl);
        hl_manifest_free(&m);
        cleanup_lua();
    }
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

/* A `res` kept past its request fails closed: middleware stashes it, the
 * request finishes, and using it later raises instead of writing into a
 * response that was sent (on a connection that may be gone). */
UTEST(lua_middleware, res_kept_past_its_request_fails_closed)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);

    int rc = luaL_dostring(lua_rt.L,
        "app.manifest({modules = {'hull/http-server@1'}})\n"
        "app.use('*', '/*', function(req, res) KEPT = res; res:status(201); return 0 end)\n");
    ASSERT_EQ(rc, LUA_OK);

    lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
    lua_rawgeti(lua_rt.L, -1, 1);
    lua_getfield(lua_rt.L, -1, "handler_id");
    int handler_id = (int)lua_tointeger(lua_rt.L, -1);
    lua_pop(lua_rt.L, 3);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    ASSERT_EQ(hl_lua_dispatch_middleware(&lua_rt, handler_id, &req, &res), 0);
    EXPECT_EQ(res.status, 201);   /* usable while the request runs */

    rc = luaL_dostring(lua_rt.L, "KEPT:status(500)");
    EXPECT_NE(rc, LUA_OK);
    if (rc != LUA_OK) {
        const char *err = lua_tostring(lua_rt.L, -1);
        EXPECT_TRUE(err && strstr(err, "has finished") != NULL);
        lua_pop(lua_rt.L, 1);
    }
    EXPECT_EQ(res.status, 201);   /* and nothing was written after */

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

/* A db.batch inside another is a savepoint of the outer transaction (audit 4
 * C-M2): an inner error rolls back only the inner writes, and the outer
 * batch's error rolls back everything - nothing commits early. */
UTEST(lua_stdlib, nested_batch_is_a_savepoint)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int ok = eval_int(
        "(function() "
        "  local db = require('hull.db').default() "
        "  db.exec('CREATE TABLE nb (x INTEGER)') "
        "  db.batch(function() "
        "    db.exec('INSERT INTO nb VALUES (1)') "
        "    local inner_ok = pcall(db.batch, function() "
        "      db.exec('INSERT INTO nb VALUES (2)') error('inner') end) "
        "    if inner_ok then error('the inner batch should have failed') end "
        "    db.batch(function() db.exec('INSERT INTO nb VALUES (3)') end) "
        "  end) "
        "  local r = db.query('SELECT x FROM nb ORDER BY x') "
        "  if #r ~= 2 or r[1].x ~= 1 or r[2].x ~= 3 then return 0 end "
        "  local outer_ok = pcall(db.batch, function() "
        "    db.exec('INSERT INTO nb VALUES (4)') "
        "    db.batch(function() db.exec('INSERT INTO nb VALUES (5)') end) "
        "    error('outer') end) "
        "  if outer_ok then return 0 end "
        "  r = db.query('SELECT COUNT(*) AS n FROM nb') "
        "  if r[1].n ~= 2 then return 0 end "
        "  db.batch(function() db.exec('INSERT INTO nb VALUES (6)') end) "
        "  r = db.query('SELECT COUNT(*) AS n FROM nb') "
        "  return r[1].n == 3 and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);
    cleanup_lua_caps();
}

/* One statement is charged to the run's instruction budget (audit 9 H4): a
 * recursive CTE in one db.query held the event loop for good, and a pcall
 * around it does not catch the limit. */
void hl_lua_budget_arm(lua_State *thread, HlLuaBudget *b, int64_t limit);

UTEST(db_audit9, lua_a_runaway_query_hits_the_instruction_limit)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    hl_lua_budget_arm(lua_rt.L, &lua_rt.budget, 1000000);
    int rc = luaL_dostring(lua_rt.L,
        "local ok, e = pcall(db.query, 'WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) AS n FROM c') "
        "return 'caught: ' .. tostring(e)");
    EXPECT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    EXPECT_NE_MSG(strstr(err ? err : "", "instruction limit"), NULL, err ? err : "");
    lua_settop(lua_rt.L, 0);
    /* The next run is armed afresh, and its queries run. */
    hl_lua_budget_arm(lua_rt.L, &lua_rt.budget, 1000000);
    EXPECT_EQ(eval_int("db.query('SELECT 7 AS n')[1].n"), 7);
    cleanup_lua_caps();
}

/* Audit 10 H3: one SQLite opcode can allocate a lot (randomblob, replace,
 * a sort) and the progress handler counted it as one instruction - a loop
 * of such queries ran ~free. Allocations are charged by size. */
UTEST(db_audit10, lua_sql_allocations_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    hl_lua_budget_arm(lua_rt.L, &lua_rt.budget, 2000000);
    int rc = luaL_dostring(lua_rt.L,
        "local ok, e = pcall(function() for i = 1, 5000 do "
        "  db.query('SELECT length(randomblob(200000)) AS n') end end) "
        "return 'done: ' .. tostring(e)");
    EXPECT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    EXPECT_NE_MSG(strstr(err ? err : "", "instruction limit"), NULL, err ? err : "");
    lua_settop(lua_rt.L, 0);
    hl_lua_budget_arm(lua_rt.L, &lua_rt.budget, 2000000);
    EXPECT_EQ(eval_int("db.query('SELECT length(randomblob(1000)) AS n')[1].n"), 1000);
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
 * The request's port is not copied either (audit 7): a "host:port" entry pins
 * one. X-Forwarded-Host / -Proto count only behind a trusted proxy. */
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
        "  if o(req{host='app.example.com:8443'}) ~= 'https://app.example.com' then return 6 end "
        "  if o(req{host='evil.com'}) ~= nil then return 7 end "
        "  if o(req{host='app.example.com:99999'}) ~= nil then return 8 end "
        "  st.trusted_hosts = { 'app.example.com:8443' } "
        "  if o(req{host='app.example.com:8443'}) ~= 'https://app.example.com:8443' then return 10 end "
        "  if o(req{host='app.example.com:8444'}) ~= nil then return 11 end "
        "  if o(req{host='app.example.com'}) ~= nil then return 12 end "
        "  st.trusted_hosts = { 'app.example.com' } "
        "  st.trust_proxy = true "
        "  if o(req{host='x', ['x-forwarded-host']='app.example.com', ['x-forwarded-proto']='javascript'}) ~= 'https://app.example.com' then return 9 end "
        "  local ok, e = pcall(af.init, { state_secret = ('a'):rep(32), email_send = function() end, "
        "    templates = {}, trusted_hosts = { 'app.example.com:x' } }) "
        "  if ok or not tostring(e):find('host:port', 1, true) then return 13 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(step, 0);
    cleanup_lua_caps();
}

/* Login CSRF (audit 8): the guard on every session-setting POST. Sec-Fetch-
 * Site same-origin / none passes, cross-site and same-site do not; without it
 * Origin, then Referer, must name the app; with neither, only a body that is
 * JSON by its Content-Type essence passes. */
UTEST(lua_stdlib, auth_flows_cross_site_guard)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  local g = af._test.same_origin_request "
        "  local st = af._test.state "
        "  st.trust_request_host = false; st.trusted_hosts = { 'app.example.com' } "
        "  local form = 'application/x-www-form-urlencoded' "
        "  local function req(h, body) h['content-type'] = h['content-type'] or form "
        "    return { headers = h, body = body or 'email=a%40b.co&password=x' } end "
        "  if not g(req{ ['sec-fetch-site']='same-origin' }) then return 1 end "
        "  if g(req{ ['sec-fetch-site']='cross-site' }) then return 2 end "
        "  if g(req{ ['sec-fetch-site']='same-site' }) then return 3 end "
        "  if g(req{ host='app.example.com' }) then return 4 end "
        "  if not g(req{ host='app.example.com', origin='https://app.example.com' }) then return 5 end "
        "  if g(req{ host='app.example.com', origin='https://evil.example' }) then return 6 end "
        "  if g(req{ host='app.example.com', origin='null' }) then return 7 end "
        "  if not g(req{ host='x.test:81', referer='http://x.test:81/login' }) then return 8 end "
        "  if g(req({ ['content-type']='text/plain; x=application/json' }, '{\"email\":\"a\"}')) then return 9 end "
        "  if not g(req({ ['content-type']='application/json; charset=utf-8' }, '{\"email\":\"a\"}')) then return 10 end "
        "  if g(req({ ['content-type']='application/json' }, 'email=a')) then return 11 end "
        "  if g(req{ ['sec-fetch-site']='cross-site', origin='https://app.example.com' }) then return 12 end "
        "  if not g({ headers = {}, body = '' }, true) then return 13 end "
        "  if g(req{ origin='https://app.example.com.evil.test' }) then return 14 end "
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

    /* On by default: a leading = / @ / - (etc.) is prefixed with a ' so a
     * spreadsheet treats it as text; a plain number and a non-formula cell
     * are untouched. */
    char *safe = eval_str(
        "require('hull.csv').encode({{'=cmd|calc'},{'@x'},{'-2+3+cmd|x'},"
        "{'-5'},{'+3.2'},{'1e-3'},{'-1+1'},{'ok'}})");
    ASSERT_NE(safe, NULL);
    ASSERT_STREQ(safe,
        "'=cmd|calc\n'@x\n'-2+3+cmd|x\n-5\n+3.2\n1e-3\n'-1+1\nok\n");
    free(safe);

    /* Opt out: emitted verbatim. */
    char *plain = eval_str(
        "require('hull.csv').encode({{'=cmd|calc'}}, { sanitize_formulas = false })");
    ASSERT_NE(plain, NULL);
    ASSERT_STREQ(plain, "=cmd|calc\n");
    free(plain);

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

/* Stdlib regressions from audit 10 (logx / _logfmt escaping, cache.fetch
 * misses, i18n, qrcode, csv): logx needs hull.log, cache hull.time, so the
 * caps-bearing state. */
UTEST(lua_stdlib, stdlib_audit10_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit10.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);

    cleanup_lua_caps();
}

/* hull.cache / hull.kv memory + SQL stores and rbac names (audit 5 DA-L3..L6):
 * needs time + db, so the caps-bearing state. */
UTEST(lua_stdlib, kv_cache_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_kv_cache.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);

    cleanup_lua_caps();
}

/* Web stdlib regressions from audit 9 (auth-flows email-change undo, lockout
 * id keys, ratelimit, cookie, session logout, idempotency, oauth): auth-flows
 * and session need the db, so the caps-bearing state. */
UTEST(lua_stdlib, web_audit9_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_web_audit9.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);

    cleanup_lua_caps();
}

/* Auth stdlib regressions from audit 10 (the vacated address of an undoable
 * email change is reserved, undo resets / totp_disable, deferred magic-link
 * signup, logout origins, idempotency
 * principal, inbox source): auth-flows needs the db, so the caps state. */
UTEST(lua_stdlib, auth_audit10_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_auth_audit10.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);

    cleanup_lua_caps();
}

/* Auth-flows regressions from audit 12 (token mails go to the stored address,
 * exact standard_users lookup, unsuppressible email-change notice, persistent
 * recovery lock, keyed revoke / confirm writes, init requirements). */
UTEST(lua_stdlib, auth_audit12_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_auth_audit12.lua", &pass, &fail);
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

/* Audit 4 (stdlib, jobs / kv / cache / rbac). Returns 0, or the number of the
 * first check that failed. */
UTEST(lua_stdlib, audit4_jobs_kv_cache_rbac)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  local db = require('hull.db').default() "
        /* 1: rbac.grant / assign create the rows their foreign keys need
         *    (foreign keys on, as on Postgres / MySQL) */
        "  db.exec('PRAGMA foreign_keys = ON') "
        "  local rbac = require('hull.web.middleware.rbac') "
        "  rbac.init() "
        "  if not pcall(rbac.grant, 'ghost', 'ghost.read') then return 1 end "
        "  if not pcall(rbac.assign, 'u1', 'ghost') then return 2 end "
        "  if not rbac.has_permission('u1', 'ghost.read') then return 3 end "
        "  db.exec('PRAGMA foreign_keys = OFF') "
        /* 4: memstore scan limit 0 = unlimited */
        "  local kv = require('hull.kv') "
        "  local h = kv.open{ namespace = 'a4scan' } "
        "  h:set('a', '1'); h:set('b', '2'); h:set('c', '3') "
        "  if #h:scan('', { limit = 0 }) ~= 3 then return 4 end "
        "  if #h:scan('', { limit = 2 }) ~= 2 then return 5 end "
        /* 6-8: cache.open is bounded by default; an explicit 0 is not */
        "  local cache = require('hull.cache') "
        "  local c = cache.open{ namespace = 'a4bound' } "
        "  if c._s.max_items ~= cache.DEFAULT_MAX_ITEMS then return 6 end "
        "  if c._s.max_bytes ~= cache.DEFAULT_MAX_BYTES then return 7 end "
        "  local u = cache.open{ namespace = 'a4unbound', max_items = 0 } "
        "  if u._s.max_items ~= 0 or u._s.max_bytes ~= 0 then return 8 end "
        /* 9-11: a job whose worker vanished on its last attempt is
         *       dead-lettered by the reaper, not re-pended */
        "  local jobs = require('hull.jobs') "
        "  jobs.init() "
        "  local id = jobs.enqueue('a4', {}, { max_attempts = 1, dedup_key = 'k1' }) "
        "  if not id then return 9 end "
        "  if #jobs.claim({ batch = 1 }) ~= 1 then return 10 end "
        "  jobs.reap({ visibility_timeout = 0 }) "
        "  local j = jobs.get(id) "
        "  if not j or j.status ~= 'dead' then return 11 end "
        /* 12-13: a finished job's dedup_key no longer blocks a re-enqueue,
         *        and an unfinished one's still does */
        "  local id2 = jobs.enqueue('a4', {}, { dedup_key = 'k1' }) "
        "  if not id2 or id2 == id then return 12 end "
        "  if jobs.enqueue('a4', {}, { dedup_key = 'k1' }) ~= nil then return 13 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(step, 0);
    cleanup_lua_caps();
}

/* Audit 5 (jobs reaper). Returns 0, or the number of the first check that
 * failed. */
UTEST(lua_stdlib, audit5_jobs_reaper)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  local jobs = require('hull.jobs') "
        "  jobs.init({ backoff = function() return 0 end }) "
        /* 1-2: more than one reaper pass of exhausted rows (500) are all
         *      dead-lettered; none is re-pended by the reclaim */
        "  for i = 1, 505 do "
        "    jobs.enqueue('a5bulk', {}, { queue = 'a5bulk', max_attempts = 1 }) "
        "  end "
        "  while #jobs.claim({ queue = 'a5bulk', batch = 200 }) > 0 do end "
        "  jobs.reap({ visibility_timeout = 0 }) "
        "  local st = jobs.stats({ queue = 'a5bulk' }) "
        "  if st.dead ~= 505 then return 1 end "
        "  if st.pending ~= 0 or st.running ~= 0 then return 2 end "
        /* 3-8: a workflow whose worker is lost on its last attempt still
         *      runs its saga compensations, once, and dead-letters */
        "  local log = {} "
        "  jobs.workflow('a5wf', function(ctx) "
        "    ctx.step('charge', function() log[#log + 1] = 'charge'; return 1 end, "
        "      { compensate = function() log[#log + 1] = 'refund' end }) "
        "    ctx.step('ship', function() log[#log + 1] = 'ship'; error('boom') end) "
        "  end) "
        "  local id = jobs.start('a5wf', {}, { queue = 'a5wf', max_attempts = 2 }) "
        "  jobs.work({ queue = 'a5wf' }) "             /* attempt 1: ship fails */
        "  if table.concat(log, ',') ~= 'charge,ship' then return 3 end "
        "  if #jobs.claim({ queue = 'a5wf', batch = 1 }) ~= 1 then return 4 end "
        "  jobs.reap({ visibility_timeout = 0 }) "      /* attempt 2 lost */
        "  local j = jobs.get(id) "
        "  if not j or j.status ~= 'pending' then return 5 end "
        "  jobs.work({ queue = 'a5wf' }) "             /* compensation run */
        "  if table.concat(log, ',') ~= 'charge,ship,refund' then return 6 end "
        "  j = jobs.get(id) "
        "  if not j or j.status ~= 'dead' then return 7 end "
        /* 8-9: a compensation run that is itself lost dead-letters */
        "  local id2 = jobs.start('a5wf', {}, { queue = 'a5wf2', max_attempts = 1 }) "
        "  if #jobs.claim({ queue = 'a5wf2', batch = 1 }) ~= 1 then return 8 end "
        "  jobs.reap({ visibility_timeout = 0 }) "
        "  if #jobs.claim({ queue = 'a5wf2', batch = 1 }) ~= 1 then return 9 end "
        "  jobs.reap({ visibility_timeout = 0 }) "
        "  local j2 = jobs.get(id2) "
        "  if not j2 or j2.status ~= 'dead' then return 10 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(step, 0);
    cleanup_lua_caps();
}

/* Audit 10 follow-ups (jobs). Each step returns 0, or the number of the first
 * check that failed.
 *
 * A durable-workflow wait inside the app's db.batch is refused: it suspends
 * by raising its yield sentinel, which unwound the batch and rolled back the
 * wait's own record, so every resume recorded a new wake time and the
 * workflow slept forever. Outside the batch the same wait suspends as ever.
 *
 * The reaper's cutoff: timestamps are whole seconds, so a claim one second
 * behind may be only moments old (a heartbeat just before a second
 * boundary) and is kept; one two seconds behind is at least a second old and
 * is reaped (vt = 1). */
UTEST(lua_stdlib, audit10_followup_jobs)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int step = eval_int(
        "(function() "
        "  local jobs = require('hull.jobs') "
        "  local db = require('hull.db').default() "
        "  jobs.init() "
        "  if db.in_transaction() then return 1 end "
        "  local inside "
        "  db.batch(function() inside = db.in_transaction() end) "
        "  if inside ~= true then return 2 end "
        "  local e1, e2 "
        "  jobs.workflow('a10wf', function(ctx) "
        "    local ok1, err1 = pcall(db.batch, function() ctx.sleep(60) end) "
        "    if ok1 then return 'slept in a batch' end "
        "    e1 = tostring(err1) "
        "    local ok2, err2 = pcall(db.batch, function() ctx.wait_signal('go') end) "
        "    if ok2 then return 'waited in a batch' end "
        "    e2 = tostring(err2) "
        "    ctx.sleep(60) "
        "    return 'woke' "
        "  end) "
        "  local id = jobs.start('a10wf', {}, { queue = 'a10wf' }) "
        "  jobs.work({ queue = 'a10wf' }) "
        "  if not (e1 and e1:find('ctx.sleep cannot wait inside db.batch', 1, true)) then return 3 end "
        "  if not (e2 and e2:find('ctx.wait_signal cannot wait inside db.batch', 1, true)) then return 4 end "
        "  local j = jobs.get(id) "
        "  if not j or j.status ~= 'pending' then return 5 end "
        "  local ws = jobs.workflow_status(id) "
        "  if not ws or not tostring(ws.waiting_for):find('sleep:', 1, true) then return 6 end "
        /* the reaper part: claim one job (vt = 1, last attempt) */
        "  local rid = jobs.enqueue('a10vt', {}, { queue = 'a10vt', max_attempts = 1 }) "
        "  if #jobs.claim({ queue = 'a10vt', batch = 1 }) ~= 1 then return 7 end "
        "  A10_RID = rid "
        "  return 0 "
        "end)()");
    EXPECT_EQ(step, 0);

    /* One second behind: may be a heartbeat made a moment ago - kept. */
    ASSERT_EQ(sqlite3_exec(test_db,
        "UPDATE _hull_jobs SET claimed_at = CAST(strftime('%s','now') AS INTEGER) - 1 "
        "WHERE queue = 'a10vt'", NULL, NULL, NULL), SQLITE_OK);
    EXPECT_EQ(eval_int(
        "(function() local jobs = require('hull.jobs') "
        "  jobs.reap({ visibility_timeout = 1 }) "
        "  return jobs.get(A10_RID).status == 'running' and 0 or 1 end)()"), 0);

    /* Two seconds behind: held at least one full second - reaped. */
    ASSERT_EQ(sqlite3_exec(test_db,
        "UPDATE _hull_jobs SET claimed_at = CAST(strftime('%s','now') AS INTEGER) - 2 "
        "WHERE queue = 'a10vt'", NULL, NULL, NULL), SQLITE_OK);
    EXPECT_EQ(eval_int(
        "(function() local jobs = require('hull.jobs') "
        "  jobs.reap({ visibility_timeout = 1 }) "
        "  return jobs.get(A10_RID).status == 'dead' and 0 or 1 end)()"), 0);
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

/* A render function with an upvalue besides _ENV is not cached (audit 4):
 * lua_dump drops upvalue values, so on a hit the outer chunk's local came
 * back as the globals table ("attempt to call a table value"). */
static const char *TC_UPVALUE_CODE =
    "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "-- pad pad pad pad pad pad pad pad pad pad pad pad pad pad pad\n"
    "local cat = table.concat\n"
    "return function(data)\n"
    "    return cat({ 'a', tostring(data and data.x or 0) })\n"
    "end\n";

UTEST(lua_template_cache, render_fn_with_upvalues_not_cached)
{
    char tmp[256];
    tc_with_tmp_home(tmp, sizeof tmp);
    lua_State *L = luaL_newstate();
    ASSERT_NE_MSG((void *)L, NULL, "newstate");
    luaL_openlibs(L);
    for (int round = 0; round < 2; round++) {
        int rc = hl_lua_template_compile_cached(L, TC_UPVALUE_CODE,
                                                strlen(TC_UPVALUE_CODE),
                                                "=tpl_upv");
        ASSERT_EQ(LUA_OK, rc);
        lua_newtable(L);
        lua_pushinteger(L, 5);
        lua_setfield(L, -2, "x");
        ASSERT_EQ(LUA_OK, lua_pcall(L, 1, 1, 0));
        ASSERT_STREQ("a5", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    ASSERT_EQ(0, tc_count(tmp));
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
    ASSERT_TRUE(strstr(err, "internal to the") != NULL);
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

/* App code cannot require hull._template; a stdlib module hands its compile
 * function on. Load it the way the stdlib does (a hull.* chunk that names
 * require) and leave it in the global __tb for the app-level chunk under test. */
static int expose_template_bridge(lua_State *L)
{
    const char *src = "__tb = require('hull._template')";
    if (luaL_loadbuffer(L, src, strlen(src), "@hull.tests.template_bridge") != LUA_OK)
        return -1;
    return lua_pcall(L, 0, 0, 0);
}

UTEST(lua_template_bridge, compile_cannot_forge_a_stdlib_chunk_name)
{
    /* hl_lua_source_is_stdlib grants _hull_* table access to any chunk whose
     * name starts with "hull.". App code cannot require hull._template, but
     * the template engine hands it names, and a stdlib module that passed one
     * through from the app would let it mint a chunk that claims to be
     * stdlib if the name were taken verbatim.
     *
     * The name is built instead. Whatever the caller asks for lands after a
     * "=template:" prefix, so the result cannot begin with "hull.". */
    init_lua();
    ASSERT_EQ(expose_template_bridge(lua_rt.L), LUA_OK);
    int rc = luaL_dostring(lua_rt.L,
        "local t = __tb\n"
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
    ASSERT_EQ(expose_template_bridge(lua_rt.L), LUA_OK);
    int rc = luaL_dostring(lua_rt.L,
        "local t = __tb\n"
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
    ASSERT_EQ(expose_template_bridge(lua_rt.L), LUA_OK);
    int rc = luaL_dostring(lua_rt.L,
        "local t = __tb\n"
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

/* run_task_script with the database capability (a registry whose "default"
 * is an in-memory SQLite connection). */
static int run_task_script_db(const char *body, char *out, size_t outsz)
{
    const HlAsyncBackend *be = hl_async_backend();
    if (!be) return 1;
    init_lua_with_caps();
    if (!lua_initialized) return 4;
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
    cleanup_lua_caps();
    be->tick(actx, 0);
    be->free(actx);
    return 0;
}

/* A wait while a registry connection is inside a transaction is refused
 * (audit 5 M1): the connection is shared, so while this coroutine was parked
 * another request ran on it and the stale-transaction guard rolled this
 * transaction back; the rest of the handler then autocommitted. Once the
 * transaction ends the wait works again, and the writes are intact. */
UTEST(lua_async, a_wait_inside_a_transaction_is_refused)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE wt (x INTEGER)')\n"
        "local t = hull.async(function() hull.sleep(20); return 1 end)\n"
        "db.exec('BEGIN')\n"
        "db.exec('INSERT INTO wt VALUES (1)')\n"
        "local ok, e = pcall(hull.sleep, 5)\n"
        "check(not ok and tostring(e):find('transaction is open', 1, true)\n"
        "      and tostring(e):find(\"'default'\", 1, true), 'sleep in txn: '..tostring(e))\n"
        "ok, e = pcall(function() return t:wait() end)\n"
        "check(not ok and tostring(e):find('transaction is open', 1, true), 'wait in txn: '..tostring(e))\n"
        "db.exec('COMMIT')\n"
        "check(t:wait() == 1, 'the task is joinable after COMMIT')\n"
        "hull.sleep(5)\n"
        "check(db.query('SELECT COUNT(*) AS n FROM wt')[1].n == 1, 'the write survived')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
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
    "check(next(r) == nil, 'not empty')\n")

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
    "check(r[1] == 1 and r[2] == nil and r[3] == 3, 'items.n')\n"
    "check(r.n == nil, 'results carry no n: they encode as a JSON list')\n"
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
    /* A reschedule left pending (a failed run above) would fire the
     * trampoline on the destroyed runtime at the final tick, hiding the
     * real assertion behind a crash. */
    if (t.timer_id > 0) be->timer_cancel(actx, (uint64_t)t.timer_id);
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

/* ── hull._task - detached tasks (the twin of JS hull:_task) ───────── */

/* The task errors the runtime logged (log.c has no callback removal, so the
 * collector is registered once and stays). */
static char g_lua_task_log[4096];
static void lua_task_log_collect(log_Event *ev)
{
    if (!ev->fmt || !strstr(ev->fmt, "spawned task error"))
        return;
    size_t n = strlen(g_lua_task_log);
    if (n + 2 >= sizeof g_lua_task_log) return;
    vsnprintf(g_lua_task_log + n, sizeof g_lua_task_log - n - 1, ev->fmt, ev->ap);
    n = strlen(g_lua_task_log);
    g_lua_task_log[n] = '\n';
    g_lua_task_log[n + 1] = '\0';
}

static void lua_task_log_reset(void)
{
    static int registered;
    if (!registered) {
        log_add_callback(lua_task_log_collect, NULL, LOG_ERROR);
        registered = 1;
    }
    g_lua_task_log[0] = '\0';
}

/* The task runs after the run that spawned it, on a coroutine of its own (it
 * can wait), and in order with other tasks. */
TASK_CASE(task_runs_after_its_spawner,
    "local T = require('hull._task')\n"
    "local order = {}\n"
    "T.spawn(function() order[#order + 1] = 'a'; hull.sleep(5); order[#order + 1] = 'a2' end)\n"
    "T.spawn(function() order[#order + 1] = 'b' end)\n"
    "order[#order + 1] = 'spawner'\n"
    "check(#order == 1, 'a task ran inline: '..table.concat(order, ','))\n"
    "hull.sleep(40)\n"
    "check(table.concat(order, ',') == 'spawner,a,b,a2', 'order '..table.concat(order, ','))\n"
    "check(not pcall(T.spawn, 42), 'a non-function is refused')\n")

/* The task belongs to nothing: it runs with no connection, timer or teardown
 * hook active even when the loop fires it while those are set (a loop pumped
 * from inside another run), and puts back what it found. With the spawner's
 * connection its hull.sleep would have suspended that connection. */
UTEST(lua_task, runs_detached_and_restores_what_it_clobbers)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);
    lua_State *L = lua_rt.L;

    lua_State *co;
    int st = ssh_co_start(&lua_rt,
        "require('hull._task').spawn(function()\n"
        "  hull.sleep(5)\n"
        "  DONE = 1\n"
        "end)\n", &co);
    ASSERT_EQ(st, LUA_OK);

    static int dummy_conn, dummy_timer;
    oc_calls = 0;
    lua_rt.active_conn            = (KlHttpConn *)(void *)&dummy_conn;
    lua_rt.active_timer           = &dummy_timer;
    lua_rt.active_on_complete     = count_oc;
    lua_rt.active_on_complete_ctx = NULL;
    lua_rt.budget.used            = 12345;
    be->tick(lua_rt.base.async_ctx, 0);            /* the task's first turn */
    EXPECT_TRUE(lua_rt.active_conn == (KlHttpConn *)(void *)&dummy_conn);
    EXPECT_TRUE(lua_rt.active_timer == &dummy_timer);
    EXPECT_TRUE(lua_rt.active_on_complete == count_oc);
    EXPECT_EQ(lua_rt.budget.used, 12345);
    lua_rt.active_conn        = NULL;
    lua_rt.active_timer       = NULL;
    lua_rt.active_on_complete = NULL;

    for (int i = 0; i < 100; i++) {
        lua_getglobal(L, "DONE");
        int done = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (done) break;
        be->tick(lua_rt.base.async_ctx, 5);
    }
    lua_getglobal(L, "DONE");
    EXPECT_EQ(lua_tointeger(L, -1), 1);
    lua_pop(L, 1);
    EXPECT_EQ(oc_calls, 0);

    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();
    be->tick(actx, 0);
    be->free(actx);
}

/* A task's error - thrown at once, or after a wait - is logged, never raised
 * into the spawner, which has moved on. */
UTEST(lua_task, errors_are_logged_not_raised)
{
    char out[512];
    lua_task_log_reset();
    ASSERT_EQ(run_task_script(
        "local T = require('hull._task')\n"
        "T.spawn(function() error('first boom', 0) end)\n"
        "T.spawn(function() hull.sleep(5); error('second boom', 0) end)\n"
        "T.spawn(function() coroutine.yield() end)\n"
        "hull.sleep(40)\n",
        out, sizeof out), 0);
    EXPECT_STREQ(out, "ok");
    EXPECT_NE(strstr(g_lua_task_log, "first boom"), NULL);
    EXPECT_NE(strstr(g_lua_task_log, "coroutine.yield()"), NULL);
}

/* The task runs under an instruction budget of its own: one that loops
 * forever is stopped (and logged), and the spawner's run carries on. */
UTEST(lua_task, has_a_budget_of_its_own)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    lua_task_log_reset();
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);
    lua_rt.max_instructions = 2000000;

    lua_State *co;
    int st = ssh_co_start(&lua_rt,
        "require('hull._task').spawn(function() while true do end end)\n"
        "hull.sleep(10)\n"
        "local n = 0\n"
        "for i = 1, 1000 do n = n + i end\n"
        "OUT = (n == 500500) and 'ok' or 'bad'\n", &co);
    ASSERT_EQ(st, LUA_YIELD);
    ssh_tick_until_done(be, lua_rt.base.async_ctx, co);

    lua_getglobal(lua_rt.L, "OUT");
    const char *out = lua_tostring(lua_rt.L, -1);
    EXPECT_STREQ(out ? out : "(no verdict)", "ok");
    lua_pop(lua_rt.L, 1);
    EXPECT_NE(strstr(g_lua_task_log, "spawned task error"), NULL);

    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();
    be->tick(actx, 0);
    be->free(actx);
}

/* A transaction the task leaves open is rolled back when it ends, so the next
 * entry does not join it; a wait inside the task's own transaction is refused
 * as anywhere else. */
UTEST(lua_task, an_open_transaction_is_rolled_back)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE tt (x INTEGER)')\n"
        "local refused\n"
        "require('hull._task').spawn(function()\n"
        "  db.exec('BEGIN')\n"
        "  db.exec('INSERT INTO tt VALUES (1)')\n"
        "  local ok, e = pcall(hull.sleep, 5)\n"
        "  refused = not ok and tostring(e):find('transaction is open', 1, true) ~= nil\n"
        "end)\n"
        "hull.sleep(20)\n"
        "check(refused == true, 'the wait in the task transaction was not refused')\n"
        "check(db.query('SELECT COUNT(*) AS n FROM tt')[1].n == 0, 'the write survived')\n"
        "db.exec('BEGIN')\n"     /* not inside a leftover transaction */
        "db.exec('COMMIT')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* Spawned inside the spawner's transaction, the task runs after it ended:
 * neither inside it nor rolled back with it. */
UTEST(lua_task, runs_outside_its_spawners_transaction)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE ts (x INTEGER)')\n"
        "local T = require('hull._task')\n"
        "local ok = pcall(db.batch, function()\n"
        "  T.spawn(function() db.exec('INSERT INTO ts VALUES (2)') end)\n"
        "  db.exec('INSERT INTO ts VALUES (1)')\n"
        "  error('roll back', 0)\n"
        "end)\n"
        "check(not ok, 'the batch did not fail')\n"
        "hull.sleep(20)\n"
        "local r = db.query('SELECT x FROM ts')\n"
        "check(#r == 1 and r[1].x == 2, 'rows '..#r)\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* Stdlib-only, and it needs a loop; a task that never got its turn is freed
 * with the VM. */
UTEST(lua_task, stdlib_only_needs_a_loop_and_is_freed_unrun)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    int rc = luaL_dostring(L, "return require('hull._task')");
    EXPECT_NE(rc, LUA_OK);
    if (rc != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        EXPECT_NE(e ? strstr(e, "internal to the Hull stdlib") : NULL, NULL);
    }
    lua_settop(L, 0);
    /* App code cannot reach it through the hull global either. */
    EXPECT_EQ(eval_int("hull._task == nil and 1 or 0"), 1);

    lua_State *co;
    ASSERT_NE(ssh_co_start(&lua_rt,
        "require('hull._task').spawn(function() end)\n", &co), LUA_OK);
    EXPECT_NE(strstr(lua_tostring(co, -1), "requires an active event loop"), NULL);
    cleanup_lua();

    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);
    ASSERT_EQ(ssh_co_start(&lua_rt,
        "require('hull._task').spawn(function() RAN = 1 end)\n", &co), LUA_OK);
    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();          /* never ticked: hl_lua_free releases the task */
    be->tick(actx, 0);      /* its timer was cancelled: nothing fires */
    be->free(actx);
}

/* auth-flows' deferred work leaves the request's transaction (audit 10
 * follow-up): /register run inside db.batch answers, and its account and
 * welcome mail come after the response, once the batch has committed. It
 * used to raise (500) - a Lua task could not leave the transaction. */
UTEST(lua_task, auth_flows_register_under_a_batch_defers_its_mail)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local json = require('hull.json')\n"
        "local af = require('hull.web.auth-flows')\n"
        "local db = require('hull.db').default()\n"
        "local S = { by_id = {}, by_email = {}, sent = {} }\n"
        "af._test.reset()\n"
        "af.init({\n"
        "  secret = ('s'):rep(32), trust_request_host = true, email_rate_limit = false,\n"
        "  email_send = function(to) S.sent[#S.sent + 1] = to end,\n"
        "  templates = {\n"
        "    welcome = function(c) return { subject = 'w', text = c.verify_url } end,\n"
        "    magic_link = function(c) return { subject = 'm', text = c.link } end,\n"
        "    password_reset = function(c) return { subject = 'p', text = c.link } end,\n"
        "    email_change = function(c) return { subject = 'e', text = c.link } end,\n"
        "    email_change_notify = function(c) return { subject = 'n', text = c.revoke_url } end,\n"
        "  },\n"
        "  user_find_by_email = function(e) return S.by_email[e] end,\n"
        "  user_get = function(id) return S.by_id[tostring(id)] end,\n"
        "  user_create = function(e, h)\n"
        "    local u = { id = 100, email = e, password_hash = h, email_verified = false }\n"
        "    S.by_id['100'] = u; S.by_email[e] = u\n"
        "    return 100\n"
        "  end,\n"
        "  user_set_password = function(id, h) S.by_id[tostring(id)].password_hash = h end,\n"
        "  user_set_email = function() end,\n"
        "  user_set_email_verified = function(id, v) S.by_id[tostring(id)].email_verified = v end,\n"
        "  on_login = function(_req, res) res:json({ ok = true }) end,\n"
        "  on_password_reset = function() end,\n"
        "  email_change_reauth = function() return true end,\n"
        "})\n"
        "local res = { code = 200, headers = {} }\n"
        "function res.status(self, c) self.code = c; return self end\n"
        "function res.json(self, v) self.body = v; return self end\n"
        "function res.header(self, k, v) self.headers[k] = v; return self end\n"
        "local req = { method = 'POST', path = '/auth/register', ctx = {},\n"
        "  remote_addr = '192.0.2.7',\n"
        "  headers = { host = 'app.test', ['content-type'] = 'application/json',\n"
        "              ['sec-fetch-site'] = 'same-origin' },\n"
        "  body = json.encode({ email = 'txn@x.test', password = 'some-password-1' }) }\n"
        "local ok, err = pcall(db.batch, function() af._test.handlers.register(req, res) end)\n"
        "check(ok, 'register under db.batch raised: '..tostring(err))\n"
        "check(res.code == 200, 'status '..tostring(res.code))\n"
        "check(#S.sent == 0 and S.by_email['txn@x.test'] == nil, 'ran inside the request')\n"
        "hull.sleep(30)\n"
        "check(S.by_email['txn@x.test'] ~= nil, 'no account after the response')\n"
        "check(#S.sent == 1 and S.sent[1] == 'txn@x.test', 'mail sent '..#S.sent)\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* Bytecode is never loaded from app reach: the template bridge, which app
 * code can require, compiles text only. A binary chunk is unverified Lua
 * bytecode - crafted, it corrupts the VM, out of the sandbox. */
UTEST(lua_runtime, template_bridge_refuses_bytecode)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(expose_template_bridge(lua_rt.L), LUA_OK);
    int ok = eval_int(
        "(function() "
        "  local tb = __tb "
        "  local good = pcall(tb._compile, 'return function() return 1 end') "
        "  local bad, err = pcall(tb._compile, string.dump(function() return 7 end)) "
        "  if not good then return 1 end "
        "  if bad then return 2 end "
        "  if not tostring(err):find('binary chunk', 1, true) then return 3 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(ok, 0);
    cleanup_lua();
}

/* db.async copies every TEXT/BLOB parameter, empty ones too: the op frees
 * whatever it holds, and an uncopied "" was the caller's string. */
UTEST(lua_runtime, db_async_copies_empty_params)
{
    static const char empty[] = "";
    HlValue v[2];
    memset(v, 0, sizeof v);
    v[0].type = HL_TYPE_TEXT; v[0].s = empty; v[0].len = 0;
    v[1].type = HL_TYPE_BLOB; v[1].s = empty; v[1].len = 0;
    HlValue *c = hl_deep_copy_params(v, 2);
    ASSERT_TRUE(c != NULL);
    for (int i = 0; i < 2; i++) {
        EXPECT_TRUE(c[i].s != NULL);
        EXPECT_TRUE(c[i].s != empty);
        EXPECT_EQ(c[i].len, (size_t)0);
        hl_free_const(c[i].s);
    }
    free(c);
}

/* A Lua UDF registered from a coroutine (a handler, app.main) runs on every
 * later query - after that coroutine has finished and been collected. It
 * kept the coroutine's lua_State and ran on it (a use after free under ASan);
 * it runs on the main state now. */
UTEST(lua_cap, udf_outlives_the_coroutine_that_registered_it)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  if not db or not db.udf then return -1 end "
        "  local co = coroutine.create(function() "
        "    db.udf.register('hull_twice', function(x) return x * 2 end) "
        "  end) "
        "  assert(coroutine.resume(co)) "
        "  co = nil "
        "  collectgarbage() collectgarbage() "
        "  local rows = db.query('SELECT hull_twice(21) AS v') "
        "  return rows[1].v "
        "end)()");
    EXPECT_EQ(v, 42);
    cleanup_lua_caps();
}

/* Query parameters stay on the Lua stack while they are bound (they keep their
 * strings alive). The marshaller pushed one per parameter with no room check,
 * so past the free slots it wrote out of bounds. */
UTEST(lua_cap, many_query_params_fit_the_stack)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local p = {} for i = 1, 300 do p[i] = i end "
        "  local rows = db.query('SELECT ' .. ('?+'):rep(299) .. '? AS s', p) "
        "  return rows[1].s "
        "end)()");
    EXPECT_EQ(v, 45150);
    cleanup_lua_caps();
}

/* Internal modules ("_" segments) are the stdlib's plumbing: once the app's
 * module set is wired, app code may not require one, even a cached one, nor
 * an undeclared module the runtime itself already loaded (hull.json). */
UTEST(lua_runtime, internal_and_cached_modules_stay_gated)
{
    init_lua();
    HlManifest m;
    memset(&m, 0, sizeof(m));
    m.modules[0].name = "validate";
    m.modules[0].api_major = 1;
    m.modules_count = 1;
    m.modules_declared = 1;
    HlResolvedModuleSet set;
    char err[256] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &set, err, sizeof(err)), 0);
    lua_rt.base.module_set = &set;

    const char *names[] = { "hull._template", "hull.kv._native", "hull.json" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char code[128];
        snprintf(code, sizeof code, "require('%s')", names[i]);
        int rc = luaL_dostring(lua_rt.L, code);
        EXPECT_NE(rc, LUA_OK);
        if (rc != LUA_OK) lua_pop(lua_rt.L, 1);
    }
    /* A declared module still loads, and so does the runtime's own use of an
     * internal one: hull.map loads hull._async from C. */
    EXPECT_EQ(luaL_dostring(lua_rt.L, "assert(type(require('hull.validate')) == 'table')"),
              LUA_OK);
    EXPECT_EQ(luaL_dostring(lua_rt.L, "assert(type(hull.map) == 'function')"), LUA_OK);

    lua_rt.base.module_set = NULL;
    cleanup_lua();
}

/* ...and before it is wired, too: an app's top-level require runs before the
 * manifest is read, and an internal name is not in the registry, so the
 * import tracker never sees one. Stdlib code may still require one. */
UTEST(lua_runtime, internal_modules_are_stdlib_only_before_wiring)
{
    init_lua();
    ASSERT_TRUE(lua_rt.base.module_set == NULL);
    const char *names[] = { "hull._template", "hull.db._internal_conn",
                            "hull.web._request" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char code[128];
        snprintf(code, sizeof code, "require('%s')", names[i]);
        int rc = luaL_dostring(lua_rt.L, code);
        EXPECT_NE_MSG(rc, LUA_OK, names[i]);
        if (rc != LUA_OK) lua_pop(lua_rt.L, 1);
    }
    const char *lib = "return type(require('hull.web._request').client_ip)";
    ASSERT_EQ(luaL_loadbuffer(lua_rt.L, lib, strlen(lib), "@hull.tests.internal"),
              LUA_OK);
    ASSERT_EQ(lua_pcall(lua_rt.L, 0, 1, 0), LUA_OK);
    EXPECT_STREQ(lua_tostring(lua_rt.L, -1), "function");
    lua_pop(lua_rt.L, 1);

    /* A stdlib helper that calls a function the app gave it does not lend it
     * its identity: handed `require` and an internal name, it is refused, as
     * pcall(require, ...) from stdlib code is - neither names the call. */
    const char *helper = "__call_with = function(f, x) return f(x) end";
    ASSERT_EQ(luaL_loadbuffer(lua_rt.L, helper, strlen(helper), "@hull.tests.helper"),
              LUA_OK);
    ASSERT_EQ(lua_pcall(lua_rt.L, 0, 0, 0), LUA_OK);
    EXPECT_NE(luaL_dostring(lua_rt.L, "__call_with(require, 'hull._template')"),
              LUA_OK);
    lua_settop(lua_rt.L, 0);
    const char *viapcall = "return (pcall(require, 'hull._template'))";
    ASSERT_EQ(luaL_loadbuffer(lua_rt.L, viapcall, strlen(viapcall), "@hull.tests.viapcall"),
              LUA_OK);
    ASSERT_EQ(lua_pcall(lua_rt.L, 0, 1, 0), LUA_OK);
    EXPECT_FALSE(lua_toboolean(lua_rt.L, -1));
    lua_settop(lua_rt.L, 0);
    cleanup_lua();
}

/* hull.web._request.client_ip is the one place a request's source IP is
 * derived under trust_proxy; four middleware delegate to it. The JS twin
 * asserts the same string (js_stdlib.client_ip_matrix), so the two cannot
 * drift. */
UTEST(lua_stdlib, client_ip_matrix)
{
    init_lua();
    const char *code =
        "local _request = require('hull.web._request')\n"
        "local function s(v) return v == nil and '(nil)' or v end\n"
        "local cases = {\n"
        "  { { headers = {}, remote_addr = '10.0.0.1' }, false },\n"
        "  { { headers = { ['x-forwarded-for'] = '1.1.1.1' }, remote_addr = '10.0.0.1' }, false },\n"
        "  { { headers = { ['x-forwarded-for'] = 'a, b, c' }, remote_addr = '10.0.0.1' }, true },\n"
        "  { { headers = { ['x-forwarded-for'] = ' 1.2.3.4 , x' }, remote_addr = '10.0.0.1' }, true },\n"
        "  { { headers = {}, remote_addr = '10.0.0.1' }, true },\n"
        "  { { headers = { ['x-forwarded-for'] = '' }, remote_addr = '10.0.0.1' }, true },\n"
        "  { { headers = {} }, false },\n"
        "  { { headers = {}, remote_addr = string.rep('a', 100) }, false },\n"
        "}\n"
        "local out = {}\n"
        "for _, c in ipairs(cases) do out[#out + 1] = s(_request.client_ip(c[1], c[2])) end\n"
        "out[#out + 1] = s(_request.client_ip(nil, true))\n"
        "return table.concat(out, '|')\n";
    ASSERT_EQ(luaL_loadbuffer(lua_rt.L, code, strlen(code), "@hull.tests.client_ip"),
              LUA_OK);
    int rc = lua_pcall(lua_rt.L, 0, 1, 0);
    if (rc != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(lua_rt.L, -1));
    ASSERT_EQ(rc, LUA_OK);
    EXPECT_STREQ(lua_tostring(lua_rt.L, -1), HL_TEST_CLIENT_IP_MATRIX);
    lua_pop(lua_rt.L, 1);
    cleanup_lua();
}

#ifdef HL_ENABLE_IMAGE
/* Audit 9 M2: an image's pixels were plain malloc (decoded ones stb's),
 * outside the VM's heap limit - an app could hold any number of 256 MB
 * images. They now come from the VM allocator: counted in mem_used, refused
 * past mem_limit, and handed back when the image is closed. A loop that
 * drops images keeps running: the collector is told about the pixels. */
UTEST(lua_cap, image_pixels_count_against_the_heap_limit)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(luaL_dostring(lua_rt.L,
        "px = string.rep('a', 1 << 20) return image ~= nil"), LUA_OK);
    if (!lua_toboolean(lua_rt.L, -1)) {   /* image compiled out */
        cleanup_lua();
        return;
    }
    lua_settop(lua_rt.L, 0);

    /* The new image's GC step may collect other garbage: start from none,
     * and allow the chunk's own bits some slack. */
    lua_gc(lua_rt.L, LUA_GCCOLLECT, 0);
    size_t base = lua_rt.mem_used;
    ASSERT_EQ(luaL_dostring(lua_rt.L,
        "img = image.new(1024, 1024, 'r8', px)"), LUA_OK);
    EXPECT_GE(lua_rt.mem_used, base + (1u << 20) - (64u << 10));
    ASSERT_EQ(luaL_dostring(lua_rt.L, "img:close() img = nil"), LUA_OK);
    EXPECT_LT(lua_rt.mem_used, base + (512u << 10));

    size_t saved = lua_rt.mem_limit;
    lua_rt.mem_limit = lua_rt.mem_used + (512u << 10);
    int rc = luaL_dostring(lua_rt.L,
        "local ok, err = pcall(image.new, 1024, 1024, 'r8', px) "
        "return ok and 'made' or tostring(err)");
    ASSERT_EQ(rc, LUA_OK);
    const char *msg = lua_tostring(lua_rt.L, -1);
    EXPECT_TRUE_MSG(msg && strstr(msg, "out of memory") != NULL, msg);
    lua_settop(lua_rt.L, 0);

    /* Room for a few at a time, not 64: garbage ones must be collected. */
    lua_rt.mem_limit = lua_rt.mem_used + (8u << 20);
    rc = luaL_dostring(lua_rt.L,
        "for i = 1, 64 do local im = image.new(1024, 1024, 'r8', px) end "
        "return 'done'");
    EXPECT_EQ_MSG(rc, LUA_OK, lua_tostring(lua_rt.L, -1));
    lua_settop(lua_rt.L, 0);
    lua_rt.mem_limit = saved;
    cleanup_lua();
}
#endif /* HL_ENABLE_IMAGE */

/* Running out of Lua heap while a query's rows are being built raised from
 * inside the backend's read loop - on Postgres / MySQL that left the reply on
 * the wire, and the next query on the connection returned this one's rows.
 * The row is now built under lua_pcall: the query stops, the backend drains,
 * and the error is raised afterwards, so the connection stays in step. */
UTEST(lua_cap, a_result_too_big_for_the_heap_fails_cleanly)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(luaL_dostring(lua_rt.L,
        "db.exec('CREATE TABLE big (x BLOB)') "
        "db.exec(\"INSERT INTO big SELECT randomblob(65536) FROM "
        "  (WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c "
        "   WHERE i < 160) SELECT i FROM c)\")"), LUA_OK);

    size_t saved = lua_rt.mem_limit;
    lua_rt.mem_limit = 4u << 20;   /* the 10 MB result cannot fit */
    int rc = luaL_dostring(lua_rt.L,
        "local ok, err = pcall(db.query, 'SELECT x FROM big') "
        "assert(not ok) "
        "return tostring(err)");
    lua_rt.mem_limit = saved;
    ASSERT_EQ(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    ASSERT_NE(err, NULL);
    EXPECT_TRUE_MSG(strstr(err, "not enough memory for the result") != NULL, err);
    lua_settop(lua_rt.L, 0);

    /* The connection answers the next query with its own result. */
    EXPECT_EQ(eval_int("db.query('SELECT count(*) AS n FROM big')[1].n"), 160);
    EXPECT_EQ(eval_int("#db.query('SELECT x FROM big LIMIT 3')"), 3);
    cleanup_lua_caps();
}

/* The stdlib's trims were s:match("^%s*(.-)%s*$") / s:gsub("%s+$", ""),
 * quadratic in a run of whitespace with a non-space after it, inside one C
 * call where the instruction limit never fires: an 8 KB Cookie header held the
 * loop for 0.7 s. hull._text trims in one pass. A 1 MB run takes milliseconds
 * now; quadratically it would take hours. */
UTEST(lua_stdlib, trims_are_linear)
{
    init_lua();
    const char *helper =
        "local t = require('hull._text') "
        "assert(t.trim('  a b \\t\\n') == 'a b') "
        "assert(t.trim('') == '' and t.trim(' \\t ') == '') "
        "assert(t.trim('x') == 'x' and t.trim(' x') == 'x' and t.trim('x ') == 'x') "
        "assert(t.rtrim('  a  ') == '  a' and t.rtrim('   ') == '') "
        "assert(t.trim('\\v\\fa\\r') == 'a') "
        "return 0";
    ASSERT_EQ(luaL_loadbuffer(lua_rt.L, helper, strlen(helper), "@hull.tests.text"),
              LUA_OK);
    int rc = lua_pcall(lua_rt.L, 0, 1, 0);
    if (rc != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(lua_rt.L, -1));
    ASSERT_EQ(rc, LUA_OK);
    lua_settop(lua_rt.L, 0);

    int step = eval_int(
        "(function() "
        "  local cookie = require('hull.web.cookie') "
        "  local validate = require('hull.validate') "
        "  local gap = string.rep(' ', 1024 * 1024) "
        "  local t0 = time.clock() "
        "  local c = cookie.parse('a=b' .. gap .. 'c; d= e ') "
        "  if c.a ~= 'b' .. gap .. 'c' or c.d ~= 'e' then return 1 end "
        "  local data = { name = ' x' .. gap .. 'y ' } "
        "  validate.check(data, { name = { trim = true } }) "
        "  if data.name ~= 'x' .. gap .. 'y' then return 2 end "
        "  if time.clock() - t0 > 2000 then return 3 end "   /* ms */
        "  return 0 "
        "end)()");
    EXPECT_EQ(step, 0);
    cleanup_lua();
}

/* A stdlib helper that calls a function the app handed it does not lend that
 * function its stdlib identity: given `db.exec` itself as retry_on, retry.run
 * calls it with the value the app's fn returned, and the `_hull_*` guard still
 * applies. The dialect helpers check every identifier too. */
UTEST(lua_cap, stdlib_helpers_do_not_lend_their_identity)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local retry = require('hull.retry') "
        "  local ok = pcall(retry.run, "
        "      function() return 'CREATE TABLE _hull_probe (x)' end, "
        "      { max_attempts = 1, retry_on = db.exec }) "
        "  if ok then return 1 end "
        "  local n = db.query(\"SELECT count(*) AS n FROM sqlite_master \" .. "
        "      \"WHERE name = '_' || 'hull_probe'\")[1].n "
        "  if n ~= 0 then return 2 end "
        "  db.exec('CREATE TABLE life (id INTEGER PRIMARY KEY, v TEXT)') "
        "  if pcall(db.upsert, '_HULL_sessions', {'id'}, {'id'}, {1}) then return 3 end "
        "  if pcall(db.upsert, 'life', {'id'}, {'id', '_hull_x'}, {1, 2}) then return 4 end "
        "  if pcall(db.insert_if_absent, 'life', nil, {'id'}, {1}) == false then return 5 end "
        "  if pcall(db.table_columns, '_hull_sessions') then return 6 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(v, 0);
    cleanup_lua_caps();
}

/* Stdlib fixes from the second audit (D2): jwt verify options and strict
 * splitting; template quoting before a tag; the safe_url filter. run() returns
 * 0, or the number of the first check that failed. */
UTEST(lua_stdlib, audit2_stdlib_fixes)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local jwt = require('hull.jwt') "
        "  local tpl = require('hull.template') "
        "  local now = require('hull.time').now() "
        "  local k = string.rep('k', 32) "
        "  local t = jwt.sign({ sub = 'u', iss = 'me', aud = { 'api', 'x' }, exp = now + 60 }, k) "
        "  if not jwt.verify(t, k, { iss = 'me', aud = 'api' }) then return 1 end "
        "  if jwt.verify(t, k, { iss = 'other' }) then return 2 end "
        "  if jwt.verify(t, k, { aud = { 'nope', 'no' } }) then return 3 end "
        "  local h, p, s = t:match('^([^.]+)%.([^.]+)%.([^.]+)$') "
        "  if jwt.verify(h .. '..' .. p .. '.' .. s, k) then return 4 end "
        "  if jwt.decode(h .. '.' .. p .. '..' .. s) then return 5 end "
        "  local old = jwt.sign({ sub = 'u', exp = now - 5 }, k) "
        "  if jwt.verify(old, k) then return 6 end "
        "  if not jwt.verify(old, k, { leeway = 30 }) then return 7 end "
        "  if jwt.verify(jwt.sign({ sub = 'u', exp = now + 60 }, k), k, { aud = 'api' }) then return 8 end "
        "  if tpl.render_string('a[1]{{ x }}', { x = 2 }) ~= 'a[1]2' then return 9 end "
        "  if tpl.render_string('{{ u | safe_url }}', { u = 'java\\tscript:alert(1)' }) ~= '#' then return 10 end "
        "  if tpl.render_string('{{ u | safe_url }}', { u = 'DATA:text/html,x' }) ~= '#' then return 11 end "
        "  if tpl.render_string('{{ u | safe_url }}', { u = '/a?b=1' }) ~= '/a?b=1' then return 12 end "
        "  if tpl.render_string('{{ u | safe_url }}', { u = 'https://x.test/' }) ~= 'https://x.test/' then return 13 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(v, 0);
    cleanup_lua_caps();
}

/* The third audit's stdlib fixes. An integer user id is a user id (it made
 * session.destroy_all and audit_log.record do nothing); the inbox's insert is
 * its check; an IPv6 client counts as its /64; an exhausted rate-limit bucket
 * survives eviction; the idempotency fingerprint covers the query; and a PEM
 * public key is never an HS256 secret. */
UTEST(lua_stdlib, audit3_stdlib_fixes)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local s = require('hull.web.middleware.session') "
        "  s.init({ ttl = 3600 }) "
        "  local sid = s.create({ user_id = 42 }) "
        "  if #s.list_for_user(42) ~= 1 then return 1 end "
        "  if s.destroy_all(42) ~= 1 then return 2 end "
        "  if s.load(sid) ~= nil then return 3 end "
        "  if s.destroy_all(4.5) ~= 0 or s.destroy_all('') ~= 0 then return 4 end "
        "  local al = require('hull.web.middleware.audit-log') "
        "  al.init({ fingerprint_salt = 'test-salt-123' }) "
        "  local req = { headers = { ['user-agent'] = 'curl/8' }, remote_addr = '10.0.0.1' } "
        "  al.record(7, 'login', req) "
        "  if #al.list(7) ~= 1 or #al.list('7') ~= 1 then return 5 end "
        "  local inbox = require('hull.web.middleware.inbox') "
        "  inbox.init() "
        "  if inbox.check_and_mark('m1', 'src') ~= false then return 6 end "
        "  if inbox.check_and_mark('m1', 'src') ~= true then return 7 end "
        "  if inbox.check_and_mark('m2', 'src', { ttl = -1 }) ~= false then return 8 end "
        "  if inbox.check_and_mark('m2', 'src') ~= false then return 9 end "
        "  local rl = require('hull.web.middleware.ratelimit') "
        "  local function hdr() local r = {} "
        "    function r:header() return self end "
        "    function r:status() return self end "
        "    function r:json() return self end return r end "
        "  local function shared(a, b) "
        "    local m = rl.middleware({ limit = 1 }) "
        "    m({ headers = {}, remote_addr = a }, hdr()) "
        "    return m({ headers = {}, remote_addr = b }, hdr()) == 1 end "
        "  if not shared('2001:db8:0:1:aaaa::1', '2001:DB8:0000:0001:1:2:3:4') then return 10 end "
        "  if shared('2001:db8:0:1::1', '2001:db8:0:2::1') then return 11 end "
        "  if not shared('::ffff:192.0.2.7', '192.0.2.7') then return 12 end "
        "  if shared('192.0.2.7', '192.0.2.8') then return 13 end "
        "  if not shared('fe80::1%eth0', 'fe80::2') then return 14 end "
        "  local b = require('hull.cache').new({ max_entries = 2 }) "
        "  local sat = { map = {}, n = 0 } "
        "  rl.check(b, 'a', 1, 60, 100, sat) "
        "  if rl.check(b, 'a', 1, 60, 100, sat).allowed then return 17 end "
        "  for _, k in ipairs({ 'b', 'c', 'd' }) do rl.check(b, k, 1, 60, 100, sat) end "
        "  if rl.check(b, 'a', 1, 60, 101, sat).allowed then return 18 end "
        "  if not rl.check(b, 'a', 1, 60, 200, sat).allowed then return 19 end "
        "  local idem = require('hull.web.middleware.idempotency') "
        "  idem.init() "
        "  local mw = idem.middleware() "
        "  local function res() "
        "    local r = {} "
        "    function r:status(c) self.code = c; return self end "
        "    function r:json(d) self.err = d.error; return self end "
        "    function r:header() return self end "
        "    return r end "
        "  local function rqst(to) return { method = 'POST', path = '/transfer', "
        "    query = { to = to }, body = 'amount=5', ctx = {}, "
        "    headers = { ['idempotency-key'] = 'k1' } } end "
        "  if mw(rqst('alice'), res()) ~= 0 then return 20 end "
        "  local r2 = res() "
        "  if mw(rqst('bob'), r2) ~= 1 or r2.code ~= 409 then return 21 end "
        "  if not tostring(r2.err):find('different request', 1, true) then return 22 end "
        "  local jwt = require('hull.jwt') "
        "  local pem = '-----BEGIN PUBLIC KEY-----\\nMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE\\n-----END PUBLIC KEY-----\\n' "
        "  local now = require('hull.time').now() "
        "  local forged = jwt.sign({ sub = 'admin', exp = now + 60 }, pem) "
        "  if jwt.verify(forged, pem, { algs = { 'HS256', 'RS256' } }) then return 23 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(v, 0);
    cleanup_lua_caps();
}

/* The fourth audit's stdlib fixes (audit 3 found them). Each line would fail
 * on the code before: integer ids reach totp/rbac/session.update; search
 * refuses _HULL_*; an exhausted rate-limit bucket stays restored in later
 * windows; inbox takes an integer id; a comma in an SSH host name is refused;
 * the sort header normalises its direction; the audit-log /64 expands "::";
 * a client-chosen X-Forwarded-For is parsed in linear time. */
UTEST(lua_stdlib, audit4_stdlib_fixes)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  totp.init({ issuer = 'T', encryption_key = string.rep('k', 32) }) "
        "  if not pcall(totp.enroll, 42) then return 1 end "
        "  if totp.enrolled(42) ~= totp.enrolled('42') then return 2 end "
        "  if totp.disable(42) ~= true then return 3 end "
        "  local s = require('hull.web.middleware.session') "
        "  s.init({ ttl = 3600 }) "
        "  local sid = s.create({}) "
        "  s.update(sid, { user_id = 42 }) "
        "  if #s.list_for_user(42) ~= 1 then return 4 end "
        "  local rbac = require('hull.web.middleware.rbac') "
        "  rbac.init() rbac.define_role('admin') "
        "  rbac.assign(7, 'admin') "
        "  if not rbac.has_role('7', 'admin') then return 5 end "
        "  if pcall(rbac.assign, 7.5, 'admin') then return 6 end "
        "  local search = require('hull.search') "
        "  search.create_index('docs', { 'body' }) "
        "  if pcall(search.reindex, 'docs', '_HULL_SESSIONS', { columns = { body = 'data' } }) then return 7 end "
        "  local rows, why = search.query('docs', 'foo AND') "
        "  if #rows ~= 0 or why == nil then return 8 end "
        "  if pcall(search.query, 'docs', string.rep('w ', 70)) then return 9 end "
        "  local rl = require('hull.web.middleware.ratelimit') "
        "  local b = require('hull.cache').new({ max_entries = 2 }) "
        "  local sat = { map = {}, n = 0 } "
        "  rl.check(b, 'a', 1, 60, 100, sat) rl.check(b, 'a', 1, 60, 100, sat) "
        "  rl.check(b, 'a', 1, 60, 200, sat) rl.check(b, 'a', 1, 60, 200, sat) "
        "  for _, k in ipairs({ 'b', 'c', 'd' }) do rl.check(b, k, 1, 60, 200, sat) end "
        "  if rl.check(b, 'a', 1, 60, 201, sat).allowed then return 10 end "
        "  local inbox = require('hull.web.middleware.inbox') "
        "  inbox.init() "
        "  if inbox.check_and_mark(12345, 'w') ~= false then return 11 end "
        "  if inbox.check_and_mark(12345, 'w') ~= true then return 12 end "
        "  local hk = require('hull.ssh.hostkey') "
        "  if pcall(hk.store_name, 'victim.org,x.example.com', 22) then return 13 end "
        "  if hk.store_name('Web1', 2222) ~= '[web1]:2222' then return 14 end "
        "  local sort = require('hull.web.htmx.sort') "
        "  local h = sort.header_attrs('name', { column = 'name', direction = 'x\" onmouseover=\"y' }, { url = '/t' }) "
        "  if h:find('onmouseover', 1, true) then return 15 end "
        "  local al = require('hull.web.middleware.audit-log') "
        "  al.init({ fingerprint_salt = 'test-salt-123' }) "
        "  local function fp(ip) return al.fingerprint({ headers = { ['user-agent'] = 'curl/8' }, remote_addr = ip }) end "
        "  if fp('2001:db8::1') ~= fp('2001:db8::2') then return 16 end "
        "  if fp('2001:db8:1:2:3:4:5:6') ~= fp('2001:db8:1:2:9:9:9:9') then return 17 end "
        "  if fp('::ffff:10.1.2.3') ~= fp('10.1.2.99') then return 18 end "
        "  local m = rl.middleware({ limit = 1000, trust_proxy = true }) "
        "  local quiet = {} "
        "  function quiet:header() return self end "
        "  function quiet:status() return self end "
        "  function quiet:json() return self end "
        "  local t0 = require('hull.time').clock() "
        "  m({ headers = { ['x-forwarded-for'] = string.rep('1', 200000) .. ',' }, remote_addr = '1.1.1.1' }, quiet) "
        "  if require('hull.time').clock() - t0 > 1000 then return 19 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(v, 0);
    cleanup_lua_caps();
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

/* ── worker.dispatch VMs ─────────────────────────────────────────────
 *
 * A ONE-thread pool, so consecutive dispatches are guaranteed to share a
 * thread: that is where a reused VM leaked state. */
typedef struct {
    const HlAsyncBackend *be;
    HlAsyncBackendCtx    *actx;
    HlAsyncBackendPool   *pool;
} LuaWorkerFix;

static int lua_worker_open(LuaWorkerFix *f)
{
    memset(f, 0, sizeof *f);
    f->be = hl_async_backend();
    if (!f->be || f->be->init(&f->actx, NULL) != 0) return -1;
    if (f->be->pool_create(&f->pool, f->actx, 1, 16) != 0) return -1;
    pending_async_ctx   = f->actx;
    pending_thread_pool = f->pool;
    init_lua();
    return lua_initialized ? 0 : -1;
}

/* Run `body` in a coroutine (dispatch yields); its string result is the
 * verdict. */
static void lua_worker_run(LuaWorkerFix *f, const char *body,
                           char *out, size_t outsz)
{
    lua_State *co;
    int st = ssh_co_start(&lua_rt, body, &co);
    if (st == LUA_YIELD) ssh_tick_until_done(f->be, f->actx, co);
    if (lua_status(co) == LUA_OK && lua_type(co, -1) == LUA_TSTRING)
        snprintf(out, outsz, "%s", lua_tostring(co, -1));
    else
        snprintf(out, outsz, "(status %d: %s)", lua_status(co),
                 lua_tostring(co, -1) ? lua_tostring(co, -1) : "?");
}

static void lua_worker_close(LuaWorkerFix *f)
{
    cleanup_lua();
    if (f->actx) f->be->tick(f->actx, 0);
    if (f->pool) f->be->pool_free(f->pool);
    if (f->actx) f->be->free(f->actx);
}

UTEST(lua_worker, a_dispatch_does_not_see_what_the_last_one_left)
{
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    char out[256];
    lua_worker_run(&f,
        "local w = require('hull.worker')\n"
        "w.dispatch(function() LEAK = 42; string.leak = 1;\n"
        "  getmetatable('').__index.leak2 = 2; return 0 end)\n"
        "return w.dispatch(function()\n"
        "  return tostring(LEAK)..','..tostring(string.leak)..','..\n"
        "         tostring(string.leak2) end)\n", out, sizeof out);
    EXPECT_STREQ(out, "nil,nil,nil");
    lua_worker_close(&f);
}

UTEST(lua_worker, a_runaway_dispatch_is_stopped)
{
    /* It held a pool thread for good: no instruction hook in worker VMs. */
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    lua_rt.max_instructions = 100000;
    char out[256];
    lua_worker_run(&f,
        /* a failed dispatch resolves to { error = msg } */
        "local r = require('hull.worker').dispatch(\n"
        "                      function() while true do end end)\n"
        "return type(r) == 'table' and r.error or tostring(r)\n", out, sizeof out);
    EXPECT_NE_MSG(strstr(out, "instruction limit exceeded"), NULL, out);
    lua_worker_close(&f);
}

UTEST(lua_worker, a_dispatch_is_held_to_the_heap_limit)
{
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    lua_rt.mem_limit = 16u << 20;
    char out[256];
    lua_worker_run(&f,
        "local r = require('hull.worker').dispatch(function()\n"
        "  local t = {}\n"
        "  for i = 1, 64 do t[i] = string.rep('x', 1 << 20) .. i end\n"
        "  return #t end)\n"
        "return type(r) == 'table' and r.error or tostring(r)\n", out, sizeof out);
    EXPECT_NE_MSG(strstr(out, "memory"), NULL, out);
    lua_worker_close(&f);
}

UTEST(lua_worker, db_is_absent_unless_declared)
{
    /* No module set says hull/db, so the worker VM gets no `db` global. */
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    char out[256];
    lua_worker_run(&f,
        "return tostring(require('hull.worker').dispatch(\n"
        "  function() return db == nil end))\n", out, sizeof out);
    EXPECT_STREQ(out, "true");
    lua_worker_close(&f);
}

/* Audit 7 M4: a transaction a job left open stayed open on the worker
 * thread's pooled connection - holding the SQLite write lock, running later
 * jobs inside it, and rolled back with their writes by the next db.async op
 * on the thread. Now each job's transaction ends with the job. */
static char lua_a7_worker_dsn[HL_TEST_PATH_MAX + 16];

UTEST(lua_worker, a_transaction_does_not_outlive_its_job)
{
    char dir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(dir, sizeof dir, "hull_a7wdb"), NULL);
    /* A file: the worker keeps the DSN pointer, and cosmo's realpath turns
     * ":memory:" into a path it cannot open on Windows. */
    snprintf(lua_a7_worker_dsn, sizeof lua_a7_worker_dsn, "%s/w.db", dir);
    hl_worker_db_init(lua_a7_worker_dsn);

    LuaWorkerFix f;
    memset(&f, 0, sizeof f);
    f.be = hl_async_backend();
    ASSERT_EQ(f.be->init(&f.actx, NULL), 0);
    ASSERT_EQ(f.be->pool_create(&f.pool, f.actx, 1, 16), 0);
    pending_async_ctx   = f.actx;
    pending_thread_pool = f.pool;
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    /* The dispatch gives the worker VM `db` only for an app declaring it. */
    HlResolvedModuleSet set;
    hl_module_set_clear(&set);
    static const char *const mods[] = { "db", "worker" };
    for (size_t i = 0; i < 2; i++) {
        int idx = hl_module_registry_index(hl_module_registry_find_short(mods[i]));
        ASSERT_GE(idx, 0);
        set.bits[idx / 64] |= (uint64_t)1 << (idx % 64);
    }
    lua_rt.base.module_set = &set;

    char out[512];
    lua_worker_run(&f,
        "local w = require('hull.worker')\n"
        "local function msg(r) return type(r) == 'table' and r.error or tostring(r) end\n"
        "w.dispatch(function() db.exec('CREATE TABLE a7 (x INTEGER)') return 0 end)\n"
        "local r1 = w.dispatch(function()\n"
        "  db.exec('BEGIN') db.exec('INSERT INTO a7 VALUES (1)') return 1 end)\n"
        "local r2 = w.dispatch(function()\n"
        "  db.exec('BEGIN') db.exec('INSERT INTO a7 VALUES (2)') error('boom') end)\n"
        /* BEGIN fails inside an open transaction */
        "local r3 = w.dispatch(function() db.exec('BEGIN') db.exec('COMMIT')\n"
        "  return db.query('SELECT count(*) AS n FROM a7')[1].n end)\n"
        "return msg(r1) .. '|' .. msg(r2) .. '|' .. msg(r3)\n", out, sizeof out);
    EXPECT_NE_MSG(strstr(out, "cannot outlive a worker.dispatch job"), NULL, out);
    EXPECT_NE_MSG(strstr(out, "boom"), NULL, out);
    EXPECT_NE_MSG(strstr(out, "|0"), NULL, out);

    lua_rt.base.module_set = NULL;
    cleanup_lua_caps();
    f.be->tick(f.actx, 0);
    f.be->pool_free(f.pool);
    f.be->free(f.actx);
}

/* Audit 8 c_db M2: the worker db.batch ran raw BEGIN / COMMIT, so a nested
 * batch was no savepoint (SQLite refused it; Postgres / MySQL committed the
 * outer batch's writes early) and a raising outer batch stayed committed.
 * It now runs the event loop's batch machinery. */
static char lua_a8_worker_dsn[HL_TEST_PATH_MAX + 16];

UTEST(lua_worker, a_nested_batch_is_a_savepoint)
{
    char dir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(dir, sizeof dir, "hull_a8wdb"), NULL);
    snprintf(lua_a8_worker_dsn, sizeof lua_a8_worker_dsn, "%s/w.db", dir);
    hl_worker_db_init(lua_a8_worker_dsn);

    LuaWorkerFix f;
    memset(&f, 0, sizeof f);
    f.be = hl_async_backend();
    ASSERT_EQ(f.be->init(&f.actx, NULL), 0);
    ASSERT_EQ(f.be->pool_create(&f.pool, f.actx, 1, 16), 0);
    pending_async_ctx   = f.actx;
    pending_thread_pool = f.pool;
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlResolvedModuleSet set;
    hl_module_set_clear(&set);
    static const char *const mods[] = { "db", "worker" };
    for (size_t i = 0; i < 2; i++) {
        int idx = hl_module_registry_index(hl_module_registry_find_short(mods[i]));
        ASSERT_GE(idx, 0);
        set.bits[idx / 64] |= (uint64_t)1 << (idx % 64);
    }
    lua_rt.base.module_set = &set;

    char out[512];
    lua_worker_run(&f,
        "local w = require('hull.worker')\n"
        "local function msg(r) return type(r) == 'table' and r.error or tostring(r) end\n"
        "w.dispatch(function() db.exec('CREATE TABLE a8b (x INTEGER)') return 0 end)\n"
        "local r1 = w.dispatch(function() db.batch(function()\n"
        "  db.exec('INSERT INTO a8b VALUES (1)')\n"
        "  db.batch(function() db.exec('INSERT INTO a8b VALUES (2)') end)\n"
        "  db.exec('INSERT INTO a8b VALUES (3)')\n"
        "  error('outer') end) end)\n"
        "local r2 = w.dispatch(function()\n"
        "  db.batch(function()\n"
        "    db.exec('INSERT INTO a8b VALUES (10)')\n"
        "    db.batch(function() db.exec('INSERT INTO a8b VALUES (11)') end)\n"
        "    pcall(db.batch, function() db.exec('INSERT INTO a8b VALUES (12)')\n"
        "                               error('inner') end)\n"
        "    db.exec('INSERT INTO a8b VALUES (13)') end)\n"
        "  local row = db.query('SELECT count(*) AS n, sum(x) AS s FROM a8b')[1]\n"
        "  return row.n .. ',' .. row.s end)\n"
        "return msg(r1) .. '|' .. msg(r2)\n", out, sizeof out);
    EXPECT_NE_MSG(strstr(out, "outer"), NULL, out);
    EXPECT_NE_MSG(strstr(out, "|3,34"), NULL, out);

    lua_rt.base.module_set = NULL;
    cleanup_lua_caps();
    f.be->tick(f.actx, 0);
    f.be->pool_free(f.pool);
    f.be->free(f.actx);
}

/* ── audit 3: runtime fixes ──────────────────────────────────────────── */

/* A coroutine waiting on a Hull operation could be resumed (or closed) by
 * the app: the dispatch returned the app's values while the worker still
 * ran, and the real completion then resumed a dead coroutine. */
UTEST(lua_audit3, a_parked_coroutine_cannot_be_resumed_by_the_app)
{
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    lua_State *co = NULL;
    int st = ssh_co_start(&lua_rt,
        "PARKED = coroutine.running()\n"
        "local r = require('hull.worker').dispatch(function() return 7 end)\n"
        "return 'done ' .. tostring(r)\n", &co);
    ASSERT_EQ(st, LUA_YIELD);
    lua_State *L = lua_rt.L;
    static const char *const probes[] = {
        "local ok, e = pcall(coroutine.resume, PARKED, 'forged') "
        "return tostring(ok) .. ':' .. tostring(e)",
        "local ok, e = pcall(coroutine.close, PARKED) "
        "return tostring(ok) .. ':' .. tostring(e)",
    };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        ASSERT_EQ(luaL_dostring(L, probes[i]), LUA_OK);
        const char *msg = lua_tostring(L, -1);
        EXPECT_TRUE(msg && strncmp(msg, "false:", 6) == 0 &&
                    strstr(msg, "waiting on a Hull operation") != NULL);
        lua_pop(L, 1);
    }
    ssh_tick_until_done(f.be, f.actx, co);
    EXPECT_EQ(lua_status(co), LUA_OK);
    EXPECT_STREQ(lua_tostring(co, -1), "done 7");
    /* once it is no longer parked, the guard is out of the way */
    ASSERT_EQ(luaL_dostring(L,
        "local c = coroutine.create(function(a) return a + 1 end) "
        "return select(2, coroutine.resume(c, 41))"), LUA_OK);
    EXPECT_EQ(lua_tointeger(L, -1), 42);
    lua_pop(L, 1);
    lua_worker_close(&f);
}

/* A stored hash names its own iteration count: one past the cap is refused
 * at once instead of running for minutes. */
UTEST(lua_audit3, verify_password_refuses_an_absurd_iteration_count)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = eval_int(
        "(function() "
        "  local h = 'pbkdf2:2000000000:' .. string.rep('00', 16) .. ':' .. "
        "            string.rep('00', 32) "
        "  return crypto.verify_password('x', h) and 1 or 0 "
        "end)()");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    EXPECT_EQ(r, 0);
    EXPECT_LT((double)(t1.tv_sec - t0.tv_sec), 5.0);
    cleanup_lua_caps();
}

/* require("./hull.json") from an app run in its own directory resolved to
 * the cache key of the stdlib json module, and got that module back. */
UTEST(lua_audit3, a_local_json_file_is_not_the_stdlib_module)
{
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_a3json"), NULL);
    char p[HL_TEST_PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/hull.json", tmpdir);
    write_file(p, "{\"x\": 41}");
    char cwd[4096];
    ASSERT_NE(getcwd(cwd, sizeof cwd), NULL);
    ASSERT_EQ(chdir(tmpdir), 0);
    init_lua_with_appdir(".");
    ASSERT_TRUE(lua_initialized);
    /* load the stdlib json first so it is in the module cache */
    (void)luaL_dostring(lua_rt.L, "pcall(function() return require('hull.json') end)");
    lua_settop(lua_rt.L, 0);
    int st = luaL_dostring(lua_rt.L,
        "local t = require('./hull.json') "
        "return type(t) == 'table' and t.x == 41 and t.encode == nil "
        "       and 'file' or 'stdlib'");
    const char *got = st == LUA_OK ? lua_tostring(lua_rt.L, -1) : lua_tostring(lua_rt.L, -1);
    EXPECT_STREQ(got ? got : "(nil)", "file");
    lua_settop(lua_rt.L, 0);
    cleanup_lua();
    ASSERT_EQ(chdir(cwd), 0);
    rm_rf(tmpdir);
}

/* The bytecode cache is loaded without verification, so a cache directory
 * another account can write to is refused (the cache turns off). */
UTEST(lua_audit3, a_writable_cache_dir_is_not_used)
{
    if (hl_host_is_windows()) UTEST_SKIP("no owner / mode bits on Windows");
    char tmpdir[512];
    bc_with_tmp_home(tmpdir, sizeof tmpdir);
    ASSERT_NE(tmpdir[0], 0);
    char sub[1024];
    ASSERT_EQ(hl_hull_cache_subdir("lua-bytecode", sub, sizeof sub), 0);
    size_t n = strlen(sub);
    while (n > 1 && sub[n - 1] == '/') sub[--n] = '\0';
    ASSERT_EQ(chmod(sub, 0777), 0);
    EXPECT_EQ(hl_hull_cache_subdir("lua-bytecode", sub, sizeof sub), -1);
    char rt[1024];
    snprintf(rt, sizeof rt, "%s/.hull/blobs/runtime", tmpdir);
    ASSERT_EQ(chmod(rt, 0775), 0);
    EXPECT_EQ(hl_hull_cache_dir(sub, sizeof sub), -1);
    ASSERT_EQ(chmod(rt, 0700), 0);
    EXPECT_EQ(hl_hull_cache_dir(sub, sizeof sub), 0);
    bc_cleanup_tmp_home(tmpdir);
    hl_lua_bytecode_cache_reset();
}

/* HULL_CACHE_DIR is granted read-write-create to the app and tool sandboxes:
 * the root, $HOME and ~/.hull (the cache keys) are refused, a directory of
 * its own is used (audit 9). */
UTEST(lua_audit9, a_broad_cache_dir_override_is_refused)
{
    char tmpdir[512];
    bc_with_tmp_home(tmpdir, sizeof tmpdir);
    ASSERT_NE(tmpdir[0], 0);
    char out[1024], ovr[1024];

    setenv("HULL_CACHE_DIR", "/", 1);
    EXPECT_EQ(hl_hull_cache_dir(out, sizeof out), -1);
    setenv("HULL_CACHE_DIR", tmpdir, 1);
    EXPECT_EQ(hl_hull_cache_dir(out, sizeof out), -1);
    snprintf(ovr, sizeof ovr, "%s/.hull", tmpdir);
    setenv("HULL_CACHE_DIR", ovr, 1);
    EXPECT_EQ(hl_hull_cache_dir(out, sizeof out), -1);
    snprintf(ovr, sizeof ovr, "%s/app-cache", tmpdir);
    setenv("HULL_CACHE_DIR", ovr, 1);
    EXPECT_EQ(hl_hull_cache_dir(out, sizeof out), 0);

    unsetenv("HULL_CACHE_DIR");
    bc_cleanup_tmp_home(tmpdir);
    hl_lua_bytecode_cache_reset();
}

/* ── Audit 4: the Lua runtime ─────────────────────────────────────────── */

/* A limited VM whose top level runs `code`; returns its status, leaving the
 * error message (if any) in errbuf. */
static int limited_run(const char *code, char *errbuf, size_t n)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.max_instructions = 100000;
    HlLua lim;
    memset(&lim, 0, sizeof lim);
    if (hl_lua_init(&lim, &cfg) != 0) return -100;
    int rc = luaL_dostring(lim.L, code);
    errbuf[0] = '\0';
    if (rc != LUA_OK) {
        const char *e = lua_tostring(lim.L, -1);
        snprintf(errbuf, n, "%s", e ? e : "");
    }
    hl_lua_free(&lim);
    return rc;
}

/* pcall / xpcall / coroutine.resume used to catch the instruction-limit
 * error, after which the hook - already spent - never fired again: an
 * unbounded loop behind one pcall. */
UTEST(lua_audit4, instruction_limit_survives_pcall)
{
    char err[512];
    static const char *const cases[] = {
        "pcall(function() while true do end end) while true do end",
        "for i = 1, 3 do pcall(function() while true do end end) end return 1",
        "xpcall(function() while true do end end, function(e) return e end) return 1",
        "local co = coroutine.create(function() while true do end end) "
        "coroutine.resume(co) return 1",
        "pcall(coroutine.wrap(function() while true do end end)) return 1",
        NULL
    };
    for (int i = 0; cases[i]; i++) {
        int rc = limited_run(cases[i], err, sizeof err);
        EXPECT_NE(rc, LUA_OK);
        EXPECT_NE(strstr(err, "instruction limit"), NULL);
    }
    /* A pcall that catches an ordinary error still works. */
    EXPECT_EQ(limited_run("local ok = pcall(error, 'x') assert(not ok) return 1",
                          err, sizeof err), LUA_OK);
}

/* A coroutine that yields with nothing parked (a bare coroutine.yield() in a
 * handler) is an error, not a request left hanging forever. */
UTEST(lua_audit4, bare_yield_is_an_error)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    lua_State *co = lua_newthread(L);
    ASSERT_EQ(luaL_loadstring(co, "coroutine.yield() return 1"), LUA_OK);
    int nres = 0;
    int st = lua_resume(co, L, 0, &nres);
    ASSERT_EQ(st, LUA_YIELD);
    st = hl_lua_resume_status(co, st);
    EXPECT_EQ(st, LUA_ERRRUN);
    lua_pop(L, 1);
    cleanup_lua();
}

/* The stdlib runs in a private environment: an app replacing a library
 * function, or reaching the string metatable, does not change what the
 * stdlib calls. */
UTEST(lua_audit4, stdlib_env_is_private)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  if getmetatable('') ~= 'locked' then return 1 end "
        "  local json = require('hull.json') "
        "  local real_concat, real_format = table.concat, string.format "
        "  table.concat = function() return 'APP' end "
        "  string.format = function() return 'APP' end "
        "  local out = json.encode({ a = 1, b = 'x' }) "
        "  table.concat, string.format = real_concat, real_format "
        "  if out:find('APP', 1, true) then return 2 end "
        "  if not out:find('\"a\":1', 1, true) then return 3 end "
        "  if ('ab'):rep(2) ~= 'abab' then return 4 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(v, 0);
    cleanup_lua_caps();
}

/* Query params are read raw: a params table's __len (app code that could
 * close the connection under the call) is not run. */
UTEST(lua_audit4, db_params_ignore_len)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local called = false "
        "  local p = setmetatable({ 7 }, { __len = function() called = true return 1 end }) "
        "  local rows = db.query('SELECT ? AS v', p) "
        "  if called then return 1 end "
        "  if not rows[1] or rows[1].v ~= 7 then return 2 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(v, 0);
    cleanup_lua_caps();
}

/* tar.create / tar.pack: an entry whose __index raises is an ordinary error
 * (the entry array is Lua-owned now; it leaked before - visible under ASan). */
UTEST(lua_audit4, tar_entry_raise_is_clean)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local ok, tar = pcall(require, 'hull.archive.tar') "
        "  if not ok then return 9 end "   /* the wrong name used to pass */
        "  local bad = setmetatable({}, { __index = function() error('boom') end }) "
        "  local ok2, err = pcall(tar.create, { { name = 'a', data = 'x' }, bad }) "
        "  if ok2 or not tostring(err):find('boom', 1, true) then return 1 end "
        "  local bytes = tar.create({ { name = 'a', data = 'x' } }) "
        "  if type(bytes) ~= 'string' or #bytes < 512 then return 2 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(v, 0);
    cleanup_lua_caps();
}

/* Code-cache entries are sealed: an entry written without the MAC (a
 * planted chunk) is refused and dropped; a sealed one reads back. */
UTEST(lua_audit4, code_cache_entries_are_sealed)
{
    char tmp[512];
    ASSERT_NE(hl_test_mkdtemp(tmp, sizeof tmp, "hull-seal"), NULL);
    HlBlobStore *st = NULL;
    ASSERT_EQ(hl_blob_store_open(&st, NULL, tmp, 1, 0), 0);
    const char *key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    static const uint8_t data[] = "bytecode-ish payload";

    /* Planted: raw bytes under the key. */
    ASSERT_EQ(hl_blob_store_put_keyed(st, key, data, sizeof data), 0);
    uint8_t *out = NULL;
    size_t out_len = 0;
    EXPECT_EQ(hl_runtime_cache_get_sealed(st, "lua-bytecode", key, &out, &out_len), -1);
    EXPECT_EQ(out, NULL);
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    EXPECT_NE(hl_blob_store_get(st, key, 0, &raw, &raw_len), 0);   /* dropped */
    free(raw);

    /* Sealed: round trip (skipped where no key file can be made). */
    hl_runtime_cache_put_sealed(st, "lua-bytecode", key, data, sizeof data);
    if (hl_runtime_cache_get_sealed(st, "lua-bytecode", key, &out, &out_len) == 0) {
        EXPECT_EQ(out_len, sizeof data);
        EXPECT_EQ(memcmp(out, data, sizeof data), 0);
        free(out);
        /* One flipped byte of the stored entry and it no longer verifies. */
        ASSERT_EQ(hl_blob_store_get(st, key, 0, &raw, &raw_len), 0);
        raw[raw_len - 1] ^= 1;
        ASSERT_GE(hl_blob_store_delete(st, key), 0);   /* a keyed put keeps an existing entry */
        ASSERT_EQ(hl_blob_store_put_keyed(st, key, raw, raw_len), 0);
        free(raw);
        EXPECT_EQ(hl_runtime_cache_get_sealed(st, "lua-bytecode", key, &out, &out_len), -1);
    }
    hl_blob_store_close(st);
    bc_cleanup_tmp_home(tmp);
}

/* A sealed entry is bound to its kind and key (audit 5 M1): copied to
 * another key, or read as another cache's entry, it no longer verifies. */
UTEST(lua_audit5, sealed_entry_bound_to_kind_and_key)
{
    char tmp[512];
    ASSERT_NE(hl_test_mkdtemp(tmp, sizeof tmp, "hull-seal5"), NULL);
    HlBlobStore *st = NULL;
    ASSERT_EQ(hl_blob_store_open(&st, NULL, tmp, 1, 0), 0);
    const char *k1 = "1111111111111111111111111111111111111111111111111111111111111111";
    const char *k2 = "2222222222222222222222222222222222222222222222222222222222222222";
    static const uint8_t data[] = "template dump";
    hl_runtime_cache_put_sealed(st, "templates", k1, data, sizeof data);
    uint8_t *out = NULL, *raw = NULL;
    size_t out_len = 0, raw_len = 0;
    if (hl_blob_store_get(st, k1, 0, &raw, &raw_len) == 0) {   /* sealing on */
        /* Relocated to another key: refused. */
        ASSERT_EQ(hl_blob_store_put_keyed(st, k2, raw, raw_len), 0);
        EXPECT_EQ(hl_runtime_cache_get_sealed(st, "templates", k2, &out, &out_len), -1);
        /* Read as another cache's entry under its own key: refused. */
        EXPECT_EQ(hl_runtime_cache_get_sealed(st, "lua-bytecode", k1, &out, &out_len), -1);
        free(raw);
    }
    hl_blob_store_close(st);
    bc_cleanup_tmp_home(tmp);
}

/* A finalizer is metered (audit 5 H4, HULL PATCH 0001): an endless __gc no
 * longer pins the VM, and the trip stops the code that ran the collection. */
UTEST(lua_audit5, finalizer_loop_hits_the_limit)
{
    char err[512];
    int rc = limited_run(
        "setmetatable({}, {__gc = function() while true do end end}) "
        "collectgarbage() collectgarbage() while true do end", err, sizeof err);
    EXPECT_NE(rc, LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
}

/* Pattern matching is charged to the budget (audit 5 M2, HULL PATCH 0002). */
UTEST(lua_audit5, backtracking_pattern_hits_the_limit)
{
    char err[512];
    int rc = limited_run(
        "local s = string.rep('a', 4000) "
        "return string.find(s, string.rep('a-', 12) .. 'b')", err, sizeof err);
    EXPECT_NE(rc, LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* A plain match still works under the limit. */
    EXPECT_EQ(limited_run("assert(string.find('hello', 'l+') == 3) return 1",
                          err, sizeof err), LUA_OK);
}

/* app.get_manifest() is a copy, and the policy JSON is encoded in C: neither
 * a metatable on the returned table nor a replaced json.encode changes it
 * (audit 5 H1/H2). */
UTEST(lua_audit5, manifest_json_ignores_app_tampering)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "app.manifest({ modules = { 'hull/json@1' }, env = { 'PORT' } }) "
        "local m = app.get_manifest() "
        "setmetatable(m, { __index = { hosts = { '*' } } }) "
        "m.fs = { read = { '/' } } "
        "local ok, json = pcall(require, 'hull.json') "
        "if ok and json then json.encode = function() return '{}' end end"), LUA_OK);
    char *j = NULL;
    size_t jl = 0;
    ASSERT_EQ(hl_lua_manifest_json(L, &j, &jl), 0);
    ASSERT_NE(j, NULL);
    EXPECT_NE(strstr(j, "\"env\":[\"PORT\"]"), NULL);
    EXPECT_EQ(strstr(j, "hosts"), NULL);
    EXPECT_EQ(strstr(j, "\"fs\""), NULL);
    free(j);
    /* The stored copy has no metatable either. */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    EXPECT_FALSE(lua_getmetatable(L, -1));
    lua_settop(L, 0);
}

/* vendor.* is stdlib-only (audit 5 M6). */
UTEST(lua_audit5, vendor_modules_are_stdlib_only)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    int rc = luaL_dostring(L, "return require('vendor.json')");
    EXPECT_NE(rc, LUA_OK);
    if (rc != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        EXPECT_NE(e ? strstr(e, "internal to the Hull stdlib") : NULL, NULL);
    }
    lua_settop(L, 0);
}

/* tar.create reports an entry's shape error instead of returning an empty
 * archive (audit 5 L1). */
UTEST(lua_audit5, tar_create_reports_shape_errors)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local ok, tar = pcall(require, 'hull.archive.tar') "
        "  if not ok then return 9 end "
        "  local out, e = tar.create({ { mode = 1 } }) "
        "  if out ~= nil or type(e) ~= 'string' then return 1 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(v, 0);
    cleanup_lua_caps();
}

/* Audit 5 M4: the compute-AOT cache a build embeds is sealed under the
 * TOOL key, which no app runtime loads. With one shared key, an app
 * compromised at native level held the key (every app process loads it)
 * and could seal a forged AOT entry the next `hull build` embedded. */
UTEST(cache_seal, tool_key_is_not_the_runtime_key)
{
    char tmp[256];
    bc_with_tmp_home(tmp, sizeof tmp);
    ASSERT_NE(tmp[0], '\0');
    char root[512];
    snprintf(root, sizeof root, "%s/store", tmp);
    HlBlobStore *st = NULL;
    ASSERT_EQ(0, hl_blob_store_open(&st, NULL, root, 1, 0));

    static const char k1[] =
        "1111111111111111111111111111111111111111111111111111111111111111";
    static const char k2[] =
        "2222222222222222222222222222222222222222222222222222222222222222";
    static const uint8_t payload[] = "native-code";
    uint8_t *out = NULL;
    size_t n = 0;

    /* Sealed with the RUNTIME key: refused (and dropped) under the tool key. */
    hl_runtime_cache_put_sealed(st, "compute-aot", k1, payload, sizeof payload);
    EXPECT_EQ(-1, hl_tool_cache_get_sealed(st, "compute-aot", k1, &out, &n));
    EXPECT_TRUE(out == NULL);

    /* Sealed with the TOOL key: round-trips there, refused as runtime. */
    hl_tool_cache_put_sealed(st, "compute-aot", k2, payload, sizeof payload);
    ASSERT_EQ(0, hl_tool_cache_get_sealed(st, "compute-aot", k2, &out, &n));
    EXPECT_EQ(n, sizeof payload);
    EXPECT_EQ(0, memcmp(out, payload, sizeof payload));
    free(out);
    out = NULL;
    hl_tool_cache_put_sealed(st, "compute-aot", k2, payload, sizeof payload);
    EXPECT_EQ(-1, hl_runtime_cache_get_sealed(st, "compute-aot", k2, &out, &n));

    hl_blob_store_close(st);
    bc_cleanup_tmp_home(tmp);
}

/* ── Audit 6: the Lua runtime ─────────────────────────────────────────── */

/* A limited VM whose top level runs `code`, then (if @p second is set, after
 * re-arming the budget as a new entry would) `second`. Returns the status of
 * the last chunk run; *out gets integer global @p global (0 when unset). */
static int limited_run_global(const char *code, const char *second,
                              const char *global, lua_Integer *out)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.max_instructions = 100000;
    HlLua lim;
    memset(&lim, 0, sizeof lim);
    if (hl_lua_init(&lim, &cfg) != 0) return -100;
    int rc = luaL_dostring(lim.L, code);
    lua_settop(lim.L, 0);
    if (second) {
        HL_LUA_ARM(&lim, lim.L);
        rc = luaL_dostring(lim.L, second);
        lua_settop(lim.L, 0);
    }
    lua_getglobal(lim.L, global);
    *out = lua_isinteger(lim.L, -1) ? lua_tointeger(lim.L, -1) : 0;
    lua_pop(lim.L, 1);
    hl_lua_free(&lim);
    return rc;
}

/* `setup` with no limit (building large test values is not what is
 * measured), then `code` as one run under the 100000 limit. */
static int limited_run_after(const char *setup, const char *code,
                             char *errbuf, size_t n)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.max_instructions = 100000;
    HlLua lim;
    memset(&lim, 0, sizeof lim);
    if (hl_lua_init(&lim, &cfg) != 0) return -100;
    lim.max_instructions = 0;
    HL_LUA_ARM(&lim, lim.L);
    int rc = luaL_dostring(lim.L, setup);
    errbuf[0] = '\0';
    if (rc == LUA_OK) {
        lua_settop(lim.L, 0);
        lim.max_instructions = 100000;
        HL_LUA_ARM(&lim, lim.L);
        rc = luaL_dostring(lim.L, code);
    }
    if (rc != LUA_OK) {
        const char *e = lua_tostring(lim.L, -1);
        snprintf(errbuf, n, "%s", e ? e : "");
    }
    hl_lua_free(&lim);
    return rc;
}

/* H1: the trip is raised from inside the count hook, where every hook is
 * off. luaL_error allocated there, and a GC step it took ran pending
 * finalizers with no metering. A finalizer that finishes its 300000-step
 * loop (far over the 100000 limit) ran unmetered. */
UTEST(lua_audit6, finalizers_in_the_trip_raise_are_metered)
{
    lua_Integer done = -1;
    int rc = limited_run_global(
        /* Finalizable garbage, armed after it is made. The loop then grows
         * a table only (no instruction that runs a GC check), so the debt
         * is past the threshold when the trip raises: the raise's own
         * allocation took the young collection, which ran every pending
         * finalizer. */
        "FIN_DONE = 0 ARMED = false "
        "collectgarbage('generational') "
        "local mt = { __gc = function() if not ARMED then return end "
        "  for i = 1, 300000 do end FIN_DONE = FIN_DONE + 1 end } "
        "for i = 1, 20 do setmetatable({}, mt) end "
        "ARMED = true "
        "local t = {} local i = 0 "
        "while true do i = i + 1 t[i] = i end",
        NULL, "FIN_DONE", &done);
    EXPECT_NE(rc, LUA_OK);
    EXPECT_EQ(done, 0);
}

/* H1: xpcall's message handler ran inside the hook's raise, with hooks off:
 * `xpcall(spin, function() while true do end end)` was unbounded. It is not
 * run on a trip (the trip is re-raised whatever it returns). */
UTEST(lua_audit6, xpcall_handler_not_run_on_a_trip)
{
    lua_Integer done = -1;
    int rc = limited_run_global(
        "H_DONE = 0 "
        "xpcall(function() while true do end end, "
        "       function(e) for i = 1, 300000 do end H_DONE = H_DONE + 1 return e end)",
        NULL, "H_DONE", &done);
    EXPECT_NE(rc, LUA_OK);
    EXPECT_EQ(done, 0);
    /* An ordinary error still reaches the handler. */
    rc = limited_run_global(
        "H_DONE = 0 "
        "local ok, e = xpcall(error, function(e) H_DONE = 1 return 'h:' .. e end, 'x') "
        "assert(not ok and e == 'h:x')",
        NULL, "H_DONE", &done);
    EXPECT_EQ(rc, LUA_OK);
    EXPECT_EQ(done, 1);
}

/* H1 (HULL PATCH 0003): a coroutine that died by the trip kept hooks off,
 * so the __close handlers a later coroutine.close ran were unmetered. */
UTEST(lua_audit6, close_of_a_tripped_coroutine_is_metered)
{
    lua_Integer done = -1;
    int rc = limited_run_global(
        "CLOSED = 0 "
        "CO = coroutine.create(function() "
        "  local x <close> = setmetatable({}, { __close = function() "
        "    for i = 1, 300000 do end CLOSED = CLOSED + 1 end }) "
        "  while true do end "
        "end) "
        "coroutine.resume(CO)",
        "coroutine.close(CO)", "CLOSED", &done);
    EXPECT_NE(rc, LUA_OK);
    EXPECT_EQ(done, 0);
}

/* M1: Patch 0002 charged match() calls only. `%b` scans to the end of the
 * subject per call (O(n^2) for n match() calls), and a plain find made no
 * match() call at all. Both now trip. */
UTEST(lua_audit6, scanned_bytes_hit_the_limit)
{
    char err[512];
    int rc = limited_run(
        "local s = string.rep('(', 20000) return string.find(s, '%b()')",
        err, sizeof err);
    EXPECT_NE(rc, LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    rc = limited_run(
        "local s = string.rep('a', 100000) "
        "return string.find(s, string.rep('a', 2000) .. 'b', 1, true)",
        err, sizeof err);
    EXPECT_NE(rc, LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* Small ones still work under the limit, and a position capture's
     * back-reference is no huge charge. */
    EXPECT_EQ(limited_run(
        "assert(string.find('x(a(b)c)y', '%b()') == 2) "
        "assert(string.find('hello world', 'o w', 1, true) == 5) "
        "assert(string.find('abab', '(ab)%1') == 1) "
        "assert(not string.find('abc', '()%1')) return 1",
        err, sizeof err), LUA_OK);
}

/* ── Audit 7: work inside one instruction (HULL PATCH 0004) ────────────── */

/* Each case passes far under the 100000 limit in VM instructions alone, and
 * trips once the work its instructions do is charged. */
UTEST(lua_audit7, work_inside_an_instruction_hits_the_limit)
{
    static const char *const cases[] = {
        /* M1: a plain find's final scan that misses, and the work below one
         * hook period each call left uncharged */
        "local s = string.rep('a', 1000000) string.find(s, 'b', 1, true) return 1",
        "local s = string.rep('a', 5000) "
        "for i = 1, 1000 do string.find(s, 'b', 1, true) end return 1",
        "local s = string.rep('a', 5000) "
        "for i = 1, 1000 do string.find(s, '[b]') end return 1",
        /* M2: a long-string compare, a concatenation, a collection */
        "local a = string.rep('x', 1000000) local b = a:sub(1, -2) .. 'x' "
        "for i = 1, 2000 do local _ = (a == b) end return 1",
        "local a = string.rep('x', 1000000) local b = a:sub(1, -2) .. 'y' "
        "for i = 1, 2000 do local _ = (a < b) end return 1",
        "local a = string.rep('x', 100000) "
        "for i = 1, 500 do local _ = a .. 'y' end return 1",
        "local s = string.rep('x', 3000000) "
        "for i = 1, 50 do collectgarbage() end return 1",
        "local s = string.rep('\\xe4\\xb8\\x80', 200000) "
        "for i = 1, 100 do utf8.len(s) end return 1",
        /* loops in C whose length an argument or __len decides: these ran
         * for good inside one instruction */
        "table.move({}, 1, 1000000000000, 2) return 1",
        "local t = setmetatable({}, { __len = function() return 1e12 end }) "
        "table.insert(t, 1, 'x') return 1",
        "local t = setmetatable({}, { __len = function() return 1e12 end }) "
        "table.remove(t, 1) return 1",
        NULL
    };
    char err[512];
    for (int i = 0; cases[i]; i++) {
        int rc = limited_run(cases[i], err, sizeof err);
        EXPECT_NE_MSG(rc, LUA_OK, cases[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, cases[i]);
    }

    /* The same operations at ordinary sizes stay well under the limit. */
    EXPECT_EQ(limited_run(
        "local a = string.rep('x', 1000) local b = a:sub(1, -2) .. 'x' "
        "assert(a == b and not (a < b)) "
        "assert(not string.find(a, 'y', 1, true) and utf8.len(a) == 1000) "
        "local t = {} for i = 1, 100 do table.insert(t, 1, i) end "
        "table.sort(t) table.move(t, 1, 100, 2) assert(#t == 101) "
        "collectgarbage() return 1",
        err, sizeof err), LUA_OK);
}

/* The charge reaches the budget the hook keeps (lua_hltakeowed): one big
 * compare past the remaining count still counts in full. */
UTEST(lua_audit7, work_past_the_hook_count_is_collected)
{
    char err[512];
    /* 2 x ~15600 units of compare per iteration, 5 iterations: ~156000
     * (the strings are built set up first, unmetered) */
    int rc = limited_run_after(
        "A = string.rep('x', 1000000) B = A:sub(1, -2) .. 'x'",
        "for i = 1, 5 do local _ = (A == B) local _ = (A == B) end return 1",
        err, sizeof err);
    EXPECT_NE(rc, LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
}

/* ── Audit 8: the rest of the work inside one instruction ──────────────── */

/* Each case: `setup` runs unmetered (it builds large values), then `code`
 * runs under the 100000 limit. Every `code` below is a few hundred VM
 * instructions and allocates almost nothing, so before the audit-8 charges
 * each passed the limit with room to spare while doing millions of steps
 * of work in C. */
UTEST(lua_audit8, non_allocating_loops_hit_the_limit)
{
    static const char *const cases[][2] = {
        /* M10: table.unpack / string.byte / utf8.codepoint push a value
         * each into a stack that has already grown */
        { "E = {}",
          "for i = 1, 100 do local _ = select('#', table.unpack(E, 1, 100000)) end "
          "return 1" },
        { "S = string.rep('x', 100000)",
          "for i = 1, 100 do local _ = select('#', string.byte(S, 1, -1)) end "
          "return 1" },
        { "S = string.rep('x', 100000)",
          "for i = 1, 100 do local _ = select('#', utf8.codepoint(S, 1, -1)) end "
          "return 1" },
        /* string.pack / unpack / packsize: a format of no-op options */
        { "F = string.rep(' ', 100000)",
          "for i = 1, 100 do string.unpack(F, '') end return 1" },
        { "F = string.rep(' ', 100000)",
          "for i = 1, 100 do string.pack(F) string.packsize(F) end return 1" },
        /* vararg copies and results moved */
        { "T = {} for i = 1, 30000 do T[i] = i end "
          "function G(...) for i = 1, 100 do local _ = select('#', ...) end "
          "return 1 end",
          "return G(table.unpack(T))" },
        /* table.concat of empty strings copies nothing */
        { "T = {} for i = 1, 100000 do T[i] = '' end",
          "for i = 1, 100 do table.concat(T) end return 1" },
        /* utf8.offset / an utf8.codes step scan the string */
        { "S = string.rep('x', 1000000)",
          "for i = 1, 100 do utf8.offset(S, #S) end return 1" },
        { "C = 'a' .. string.rep('\\x80', 1000000) F = utf8.codes(C)",
          "for i = 1, 100 do F(C, 1) end return 1" },
        /* next() over a hash part whose entries were removed */
        { "T = {} for i = 1, 200000 do T['k' .. i] = i end "
          "for k in pairs(T) do T[k] = nil end",
          "for i = 1, 100 do local _ = next(T) end return 1" },
        /* a long-string key equal to the table's key, another object: a
         * memcmp per lookup (get, set, rawget, next) */
        { "K1 = string.rep('k', 1000000) T = { [K1] = 1 } "
          "K2 = string.rep('k', 1000000)",
          "for i = 1, 100 do local _ = T[K2] end return 1" },
        { "K1 = string.rep('k', 1000000) T = { [K1] = 1 } "
          "K2 = string.rep('k', 1000000)",
          "for i = 1, 100 do T[K2] = i end return 1" },
        { "K1 = string.rep('k', 1000000) T = { [K1] = 1 } "
          "K2 = string.rep('k', 1000000)",
          "for i = 1, 100 do local _ = rawget(T, K2) end return 1" },
        { "K1 = string.rep('k', 1000000) T = { [K1] = 1 } "
          "K2 = string.rep('k', 1000000)",
          "for i = 1, 100 do local _ = next(T, K2) end return 1" },
        /* string -> number coercion scans the whole string */
        { "S = string.rep(' ', 1000000)",
          "for i = 1, 100 do local _ = tonumber(S) end return 1" },
        { "S = string.rep(' ', 1000000)",
          "for i = 1, 100 do local _ = tonumber(S, 10) end return 1" },
        { "S = string.rep(' ', 1000000)",
          "for i = 1, 100 do pcall(function() return S + 0 end) end return 1" },
        /* string.rep: a copy per repetition */
        { "",
          "for i = 1, 100 do local _ = string.rep('a', 10000) end return 1" },
        /* a luaL_Buffer filled, then dropped by an error */
        { "BT = { string.rep('x', 1000000), true }",
          "for i = 1, 100 do pcall(table.concat, BT) end return 1" },
        { NULL, NULL }
    };
    char err[512];
    for (int i = 0; cases[i][0]; i++) {
        int rc = limited_run_after(cases[i][0], cases[i][1], err, sizeof err);
        EXPECT_NE_MSG(rc, LUA_OK, cases[i][1]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, cases[i][1]);
    }

    /* string.rep of an empty result ran its copy loop n times */
    EXPECT_EQ(limited_run(
        "assert(string.rep('', 1e15) == '') "
        "assert(string.rep('', 1e15, '') == '') return 1",
        err, sizeof err), LUA_OK);

    /* The same operations at ordinary sizes stay well under the limit. */
    EXPECT_EQ(limited_run(
        "local t = { 1, 2, 3 } local a, b, c = table.unpack(t) assert(c == 3) "
        "assert(select('#', string.byte('hello', 1, -1)) == 5) "
        "assert(select('#', utf8.codepoint('h\\xc3\\xa9llo', 1, -1)) == 5) "
        "assert(string.unpack('<i4', string.pack('<i4', 7)) == 7) "
        "assert(string.packsize('i4i4') == 8) "
        "local function n(...) return select('#', ...) end "
        "assert(n(1, 2, 3) == 3) "
        "assert(table.concat({ 'a', 'b' }, ',') == 'a,b') "
        "assert(utf8.offset('h\\xc3\\xa9llo', 3) == 4) "
        "for _, c in utf8.codes('h\\xc3\\xa9') do end "
        "local h = { a = 1, b = 2 } h.a = nil assert(next(h) == 'b') "
        "local k = string.rep('k', 50) local kt = { [k] = 1 } "
        "assert(kt[string.rep('k', 50)] == 1) "
        "assert(tonumber('  42  ') == 42 and tonumber('ff', 16) == 255) "
        "assert('10' + 1 == 11) "
        "assert(string.rep('ab', 3, ',') == 'ab,ab,ab') "
        "return 1",
        err, sizeof err), LUA_OK);
}

/* M11: the collector's work. An emergency full collection after a failed
 * allocation, a mode switch, or a collector tuned to run on every
 * allocation each did a full traversal of the live heap for a few
 * instructions. The setup keeps 20000 tables alive for it to traverse. */
UTEST(lua_audit8, collector_work_hits_the_limit)
{
    static const char *const cases[] = {
        /* the 48 MB concatenation cannot fit the 64 MB heap: each try runs
         * an emergency full collection, then a catchable memory error */
        "for i = 1, 50 do pcall(function() return A .. A end) end return 1",
        "for i = 1, 50 do collectgarbage('generational') "
        "collectgarbage('incremental') end return 1",
        /* pause 0: a whole cycle per allocation once one cycle ends */
        "collectgarbage('incremental', 1, 1000) collectgarbage('step') "
        "for i = 1, 10000 do local t = {} end return 1",
        "for i = 1, 50 do collectgarbage() end return 1",
        NULL
    };
    char err[512];
    for (int i = 0; cases[i]; i++) {
        int rc = limited_run_after(
            "KEEP = {} for i = 1, 20000 do KEEP[i] = {} end "
            "A = string.rep('x', 24000000)",
            cases[i], err, sizeof err);
        EXPECT_NE_MSG(rc, LUA_OK, cases[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, cases[i]);
    }
    /* A memory error is still an ordinary, catchable error. */
    EXPECT_EQ(limited_run_after(
        "A = string.rep('x', 24000000)",
        "local ok, e = pcall(function() return A .. A end) "
        "assert(not ok and tostring(e):find('memory')) return 1",
        err, sizeof err), LUA_OK);
    /* Garbage made and collected at an ordinary rate stays under it. */
    EXPECT_EQ(limited_run(
        "local keep = {} for i = 1, 1000 do keep[i] = { i } end "
        "for i = 1, 2000 do local t = { i } end "
        "collectgarbage() return 1",
        err, sizeof err), LUA_OK);
}

/* M12: a coroutine's run is charged when it returns or yields. The hook
 * reported whole periods (10000 instructions) only, so a coroutine body
 * shorter than one period was never charged: 100 x 9900 instructions
 * here ran under a 100000 limit. */
UTEST(lua_audit8, short_coroutines_are_charged)
{
    static const char *const cases[] = {
        "local f = function() for i = 1, 9900 do end end "
        "for j = 1, 100 do coroutine.wrap(f)() end return 1",
        "local f = function() for i = 1, 9900 do end end "
        "for j = 1, 100 do coroutine.resume(coroutine.create(f)) end return 1",
        "local f = function() for i = 1, 9900 do end coroutine.yield() end "
        "for j = 1, 100 do local co = coroutine.create(f) "
        "coroutine.resume(co) coroutine.close(co) end return 1",
        /* nested: the inner run reaches the outer thread, then the main */
        "local f = function() for i = 1, 4900 do end end "
        "local g = function() for i = 1, 4900 do end coroutine.wrap(f)() end "
        "for j = 1, 100 do coroutine.wrap(g)() end return 1",
        NULL
    };
    char err[512];
    for (int i = 0; cases[i]; i++) {
        int rc = limited_run(cases[i], err, sizeof err);
        EXPECT_NE_MSG(rc, LUA_OK, cases[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, cases[i]);
    }
    /* A few short coroutines stay under it. */
    EXPECT_EQ(limited_run(
        "local n = 0 "
        "for j = 1, 5 do coroutine.wrap(function() for i = 1, 1000 do end "
        "n = n + 1 end)() end assert(n == 5) return 1",
        err, sizeof err), LUA_OK);
}

/* L1: a string '<' is charged the bytes it compared, not the left
 * operand's length: `big < "b"` decides on the first byte. */
UTEST(lua_audit8, string_less_than_charges_the_compared_prefix)
{
    char err[512];
    EXPECT_EQ(limited_run_after(
        "A = string.rep('a', 1000000) T = {} "
        "for i = 1, 200 do T[i] = string.format('%05d', (i * 7919) % 200) .. "
        "A:sub(1, 100000) end",
        "for i = 1, 3000 do local _ = (A < 'b') local _ = (A <= 'b') end "
        "table.sort(T) return 1",
        err, sizeof err), LUA_OK);
    /* A compare that does run through a long common prefix still trips. */
    int rc = limited_run_after(
        "A = string.rep('x', 1000000) B = A:sub(1, -2) .. 'y'",
        "for i = 1, 100 do local _ = (A < B) end return 1",
        err, sizeof err);
    EXPECT_NE(rc, LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
}

/* M2 / L1: a NUL in a manifest string is refused by app.manifest: in a
 * modules entry it crashed extraction (strlen(NULL)), in csp it turned
 * CSP off while the signed JSON showed a policy. */
UTEST(lua_audit6, manifest_nul_strings_refused)
{
    static const char *const cases[] = {
        "app.manifest({ modules = { 'hull/db@1\\0' } })",
        "app.manifest({ csp = \"default-src 'self'\\0\" })",
        "app.manifest({ hosts = { 'a.example\\0.b' } })",
        "app.manifest({ ['env\\0x'] = { 'PORT' } })",
        NULL
    };
    for (int i = 0; cases[i]; i++) {
        init_lua();
        ASSERT_TRUE(lua_initialized);
        int rc = luaL_dostring(lua_rt.L, cases[i]);
        EXPECT_NE(rc, LUA_OK);
        if (rc != LUA_OK) {
            const char *e = lua_tostring(lua_rt.L, -1);
            EXPECT_TRUE(e && strstr(e, "NUL") != NULL);
        }
        cleanup_lua();
    }
}

/* M2 / L1, the extractor's own guard: a stored manifest that holds NUL
 * strings anyway (set without app.manifest) is read without crashing, the
 * bad module is skipped, and a bad csp means the default CSP, not none. */
UTEST(lua_audit6, manifest_extractor_skips_nul_strings)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    lua_newtable(L);
    lua_newtable(L);
    lua_pushlstring(L, "hull/db@1\0x", 11);
    lua_rawseti(L, -2, 1);
    lua_pushliteral(L, "hull/json@1");
    lua_rawseti(L, -2, 2);
    lua_setfield(L, -2, "modules");
    lua_pushlstring(L, "default-src 'self'\0", 19);
    lua_setfield(L, -2, "csp");
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_manifest");

    HlManifest m;
    ASSERT_EQ(hl_manifest_extract_lua(L, &m, NULL), 0);
    EXPECT_EQ(m.modules_count, 1);
    if (m.modules_count == 1) EXPECT_STREQ(m.modules[0].name, "hull/json");
    EXPECT_EQ(m.csp_set, 0);
    hl_manifest_free(&m);

    /* The legacy keyed form too. */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushlstring(L, "hull/db@1\0x", 11);
    lua_setfield(L, -2, "db");
    lua_setfield(L, -2, "modules");
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    ASSERT_EQ(hl_manifest_extract_lua(L, &m, NULL), 0);
    EXPECT_EQ(m.modules_count, 0);
    hl_manifest_free(&m);
    cleanup_lua();
}

/* L2: app.main's coroutine ref belongs to vt_lua_run_main. A continuation
 * of main's wait cancelled after run_main released it (pool teardown at
 * shutdown) unref'd the same slot again: the next two refs then shared one
 * slot. */
UTEST(lua_audit6, cancel_of_mains_wait_does_not_unref_twice)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    lua_State *co = lua_newthread(L);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                 HlLuaPushResultFn);
    lua_rt.cli_main_co = co;              /* as vt_lua_run_main sets it */
    lua_rt.active_co = co;
    lua_rt.active_thread_ref = co_ref;
    HlAsyncCont *cont = hl_lua_async_cont_create(&lua_rt, lua_rt.base.alloc, NULL);
    ASSERT_NE(cont, NULL);
    lua_rt.active_co = NULL;
    lua_rt.active_thread_ref = LUA_NOREF;

    /* run_main returns: it releases main's ref, then the loop is torn down. */
    lua_rt.cli_main_co = NULL;
    luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
    cont->cancel(cont);
    cont->destroy(cont);

    lua_pushboolean(L, 1);
    int a = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushboolean(L, 1);
    int b = luaL_ref(L, LUA_REGISTRYINDEX);
    EXPECT_NE(a, b);
    luaL_unref(L, LUA_REGISTRYINDEX, a);
    luaL_unref(L, LUA_REGISTRYINDEX, b);
    cleanup_lua();
}

/* L3: after main returns, hull._running is read raw and protected: a `hull`
 * global whose __index raises aborted the process (lua_atpanic). */
UTEST(lua_audit6, main_replacing_hull_global_keeps_exit_code)
{
    init_lua();
    int rc = luaL_dostring(lua_rt.L,
        "app.main(function() "
        "  hull = setmetatable({}, { __index = function() error('x') end }) "
        "  return 3 "
        "end)");
    ASSERT_EQ(rc, LUA_OK);
    int exit_code = 0;
    int run_rc = hl_lua_vtable.run_main(&lua_rt.base, NULL, 0, NULL, NULL, &exit_code);
    EXPECT_EQ(run_rc, 0);
    EXPECT_EQ(exit_code, 3);
    cleanup_lua();
}

/* Entries in the req.ctx table (bindings.c). */
static int req_ctx_entries(lua_State *L)
{
    int n = 0;
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &hl_lua_req_ctx_key) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2)) { n++; lua_pop(L, 1); }
    }
    lua_pop(L, 1);
    return n;
}

static int first_mw_handler_id(lua_State *L, int i)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_middleware");
    lua_rawgeti(L, -1, i);
    lua_getfield(L, -1, "handler_id");
    int id = (int)lua_tointeger(L, -1);
    lua_pop(L, 3);
    return id;
}

/* c_js H4, the Lua side: each middleware stage's req.ctx was a registry ref
 * freed only when a handler completed synchronously - a request a
 * middleware answered (or an SSE route, a waiting handler, a body that
 * never came) pinned it for good. A short-circuit now forgets it, the next
 * stage still sees it, and a request that ends any other way is overwritten
 * by the next one on the same KlHttpRequest. */
UTEST(lua_audit6, req_ctx_not_retained_past_its_request)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "app.manifest({modules = {'hull/http-server@1'}})\n"
        "app.use('*', '/*', function(req, res) req.ctx.user = 'u' return 0 end)\n"
        "app.use('*', '/*', function(req, res) SEEN = req.ctx.user return 1 end)\n"),
        LUA_OK);
    int first = first_mw_handler_id(L, 1);
    int second = first_mw_handler_id(L, 2);

    for (int i = 0; i < 100; i++) {
        KlHttpRequest req = {0};
        KlHttpResponse res = {0};
        ASSERT_EQ(hl_lua_dispatch_middleware(&lua_rt, first, &req, &res), 0);
        EXPECT_TRUE(req.ctx == &hl_lua_req_ctx_marker);
        ASSERT_EQ(hl_lua_dispatch_middleware(&lua_rt, second, &req, &res), 1);
        EXPECT_TRUE(req.ctx == NULL);              /* answered: forgotten */
    }
    EXPECT_EQ(req_ctx_entries(L), 0);
    lua_getglobal(L, "SEEN");
    EXPECT_STREQ(lua_tostring(L, -1), "u");        /* the handoff still works */
    lua_pop(L, 1);

    /* A request abandoned after the first stage (Keel then resets req->ctx):
     * the next request on the same KlHttpRequest replaces its entry. */
    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    for (int i = 0; i < 100; i++) {
        req.ctx = NULL;                            /* Keel's reset */
        ASSERT_EQ(hl_lua_dispatch_middleware(&lua_rt, first, &req, &res), 0);
    }
    EXPECT_EQ(req_ctx_entries(L), 1);
    free_lua_req_ctx(&req);
    EXPECT_EQ(req_ctx_entries(L), 0);
    cleanup_lua();
}

/* Round-7 H2 (Lua side): a request that passes the middleware but never
 * reaches a handler - a 404 / 405 Keel answers itself - kept its req.ctx
 * until its connection slot served another request through a middleware.
 * serve.c now reports every sent response (Keel's access-log hook) and the
 * runtime drops the entry there. */
UTEST(lua_audit7, req_ctx_dropped_when_the_response_is_sent)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "app.manifest({modules = {'hull/http-server@1'}})\n"
        "app.use('*', '/*', function(req, res) "
        "  req.ctx.big = string.rep('x', 4096) return 0 end)\n"), LUA_OK);
    int mw = first_mw_handler_id(L, 1);
    ASSERT_TRUE(hl_lua_vtable.request_done != NULL);

    enum { N = 64 };                 /* N connection slots, one 404 each */
    static KlHttpRequest reqs[N];
    for (int i = 0; i < N; i++) {
        KlHttpResponse res = {0};
        memset(&reqs[i], 0, sizeof reqs[i]);
        ASSERT_EQ(hl_lua_dispatch_middleware(&lua_rt, mw, &reqs[i], &res), 0);
    }
    EXPECT_EQ(req_ctx_entries(L), N);
    for (int i = 0; i < N; i++)      /* Keel sent each 404 */
        hl_lua_vtable.request_done(&lua_rt.base, &reqs[i]);
    EXPECT_EQ(req_ctx_entries(L), 0);
    /* a request with nothing kept, and a repeat, are no-ops */
    hl_lua_vtable.request_done(&lua_rt.base, &reqs[0]);
    EXPECT_EQ(req_ctx_entries(L), 0);
    for (int i = 0; i < N; i++) reqs[i].ctx = NULL;
    cleanup_lua();
}

#ifdef HL_ENABLE_HTTP_SERVER
/* Round-7 L10: `hull test` ran every case of a file on one budget, never
 * re-armed: a case that tripped the sticky limit failed every later case. */
UTEST(lua_audit7, test_cases_each_get_their_own_budget)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.max_instructions = 100000;
    HlLua lim;
    memset(&lim, 0, sizeof lim);
    ASSERT_EQ(hl_lua_init(&lim, &cfg), 0);
    hl_lua_test_clear(lim.L);
    ASSERT_EQ(luaL_dostring(lim.L,
        "local cases = {} "
        "cases[1] = { desc = 'spins', fn = function() while true do end end } "
        "cases[2] = { desc = 'fine',  fn = function() for i = 1, 50000 do end end } "
        "cases[3] = { desc = 'fine too', fn = function() for i = 1, 50000 do end end } "
        "return cases"), LUA_OK);
    lua_setfield(lim.L, LUA_REGISTRYINDEX, "__hull_test_cases");
    int total = 0, passed = 0, failed = 0;
    hl_lua_test_run(lim.L, &total, &passed, &failed, NULL, NULL, 0);
    EXPECT_EQ(total, 3);
    EXPECT_EQ(failed, 1);
    EXPECT_EQ(passed, 2);
    hl_lua_free(&lim);
}
#endif

/* The tool VM's userspace sandbox is the caller-set unveil context reaching
 * the tool bindings through hl_lua_init. hl_lua_init used to zero it with the
 * rest of the struct, so every binding saw NULL and skipped its check: tool
 * mode read and wrote anything on the hosts without a kernel sandbox (macOS,
 * Windows). Set up exactly as tool.c does; a file outside the granted
 * directory must not be readable. */
static int tool_vm_write(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fputs(text, f) >= 0;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

UTEST(lua_tool_vm, unveil_context_reaches_the_tool_bindings)
{
    char granted[HL_TEST_PATH_MAX], other[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(granted, sizeof granted, "hull_tv_in") != NULL);
    ASSERT_TRUE(hl_test_mkdtemp(other, sizeof other, "hull_tv_out") != NULL);
    char in_file[HL_TEST_PATH_MAX + 16], out_file[HL_TEST_PATH_MAX + 16];
    snprintf(in_file, sizeof in_file, "%s/in.txt", granted);
    snprintf(out_file, sizeof out_file, "%s/out.txt", other);
    ASSERT_EQ(tool_vm_write(in_file, "granted"), 0);
    ASSERT_EQ(tool_vm_write(out_file, "secret"), 0);

    HlToolUnveilCtx uctx;
    hl_tool_unveil_init(&uctx);
    ASSERT_EQ(hl_tool_unveil_add(&uctx, granted, "r"), 0);
    hl_tool_unveil_seal(&uctx);

    HlVfs pvfs;
    void *pvfs_owned = NULL;
    hl_platform_vfs_init(&pvfs, &pvfs_owned);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = 0;                       /* tool mode, as tool.c */
    HlLua tv;
    memset(&tv, 0, sizeof tv);
    tv.tool_unveil_ctx = &uctx;
    tv.base.platform_vfs = &pvfs;
    ASSERT_EQ(hl_lua_init(&tv, &cfg), 0);
    EXPECT_TRUE(tv.tool_unveil_ctx == &uctx);

    lua_State *L = tv.L;
    lua_getglobal(L, "tool");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "read_file");
    lua_pushstring(L, in_file);
    ASSERT_EQ(lua_pcall(L, 1, 1, 0), LUA_OK);
    EXPECT_TRUE(lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "granted") == 0);
    lua_pop(L, 1);
    lua_getfield(L, -1, "read_file");
    lua_pushstring(L, out_file);
    ASSERT_EQ(lua_pcall(L, 1, 1, 0), LUA_OK);
    EXPECT_TRUE(lua_isnil(L, -1));         /* outside the grant: refused */
    lua_pop(L, 2);

    hl_lua_free(&tv);
    hl_tool_unveil_free(&uctx);
    hl_platform_vfs_dispose(pvfs_owned);
    remove(in_file); remove(out_file);
    rmdir(granted); rmdir(other);
}

/* audit 8 c_caps L2: tool.rename removes the source's directory entry, so it
 * needs write + create on the source as well as the destination - 'r' let a
 * read-only grant (the app directory, ~/.hull/tools) lose files where only
 * the userspace list applies. */
UTEST(lua_tool_vm, rename_needs_write_on_the_source)
{
    char ro[HL_TEST_PATH_MAX], rw[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(ro, sizeof ro, "hull_tv_ro") != NULL);
    ASSERT_TRUE(hl_test_mkdtemp(rw, sizeof rw, "hull_tv_rw") != NULL);
    char src_ro[HL_TEST_PATH_MAX + 16], src_rw[HL_TEST_PATH_MAX + 16];
    char dst[HL_TEST_PATH_MAX + 16], dst2[HL_TEST_PATH_MAX + 16];
    snprintf(src_ro, sizeof src_ro, "%s/a.txt", ro);
    snprintf(src_rw, sizeof src_rw, "%s/b.txt", rw);
    snprintf(dst, sizeof dst, "%s/moved.txt", rw);
    snprintf(dst2, sizeof dst2, "%s/c.txt", ro);
    ASSERT_EQ(tool_vm_write(src_ro, "a"), 0);
    ASSERT_EQ(tool_vm_write(src_rw, "b"), 0);

    HlToolUnveilCtx uctx;
    hl_tool_unveil_init(&uctx);
    ASSERT_EQ(hl_tool_unveil_add(&uctx, ro, "r"), 0);
    ASSERT_EQ(hl_tool_unveil_add(&uctx, rw, "rwc"), 0);
    hl_tool_unveil_seal(&uctx);

    HlVfs pvfs;
    void *pvfs_owned = NULL;
    hl_platform_vfs_init(&pvfs, &pvfs_owned);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = 0;
    HlLua tv;
    memset(&tv, 0, sizeof tv);
    tv.tool_unveil_ctx = &uctx;
    tv.base.platform_vfs = &pvfs;
    ASSERT_EQ(hl_lua_init(&tv, &cfg), 0);

    lua_State *L = tv.L;
    const char *cases[3][2] = {
        { src_ro, dst },     /* out of a read-only grant: refused */
        { src_rw, dst2 },    /* into a read-only grant: refused */
        { src_rw, dst },     /* both writable: done */
    };
    int expect[3] = { 0, 0, 1 };
    for (int i = 0; i < 3; i++) {
        lua_getglobal(L, "tool");
        lua_getfield(L, -1, "rename");
        lua_pushstring(L, cases[i][0]);
        lua_pushstring(L, cases[i][1]);
        ASSERT_EQ(lua_pcall(L, 2, 1, 0), LUA_OK);
        EXPECT_EQ(lua_toboolean(L, -1), expect[i]);
        lua_pop(L, 2);
    }
    struct stat st;
    EXPECT_EQ(stat(src_ro, &st), 0);       /* still where it was */

    hl_lua_free(&tv);
    hl_tool_unveil_free(&uctx);
    hl_platform_vfs_dispose(pvfs_owned);
    remove(src_ro); remove(src_rw); remove(dst); remove(dst2);
    rmdir(ro); rmdir(rw);
}

/* audit 10 G1: the tool VM compiles no source from script (load / loadfile /
 * dofile read any file past the allowlist), tool.set_app_dir is gone, and the
 * bindings that read an app entry or create files check the allowlist too. */
UTEST(lua_tool_vm, no_load_and_bindings_check_the_allowlist)
{
    char other[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(other, sizeof other, "hull_tv_out") != NULL);
    char app[HL_TEST_PATH_MAX + 16];
    snprintf(app, sizeof app, "%s/app.lua", other);
    ASSERT_EQ(tool_vm_write(app, "return 1\n"), 0);

    HlToolUnveilCtx uctx;
    hl_tool_unveil_init(&uctx);
    hl_tool_unveil_seal(&uctx);                 /* nothing granted */

    HlVfs pvfs;
    void *pvfs_owned = NULL;
    hl_platform_vfs_init(&pvfs, &pvfs_owned);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = 0;
    HlLua tv;
    memset(&tv, 0, sizeof tv);
    tv.tool_unveil_ctx = &uctx;
    tv.base.platform_vfs = &pvfs;
    ASSERT_EQ(hl_lua_init(&tv, &cfg), 0);
    lua_State *L = tv.L;

    lua_pushstring(L, app);
    lua_setglobal(L, "APP");
    lua_pushstring(L, other);
    lua_setglobal(L, "OUT");
    const char *chunk =
        "assert(load == nil and loadfile == nil and dofile == nil)\n"
        "assert(tool.set_app_dir == nil)\n"
        "local m, err = tool.extract_manifest_lua(APP)\n"
        "assert(m == nil and err:find('not readable'), tostring(err))\n"
        "assert(tool.tmpdir() == nil)\n"
        "assert(tool.extract_platform(OUT) == false)\n"
        "return true\n";
    ASSERT_EQ(luaL_loadstring(L, chunk), LUA_OK);
    int rc = lua_pcall(L, 0, 1, 0);
    if (rc != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    EXPECT_EQ(rc, LUA_OK);
    lua_pop(L, 1);

    hl_lua_free(&tv);
    hl_tool_unveil_free(&uctx);
    hl_platform_vfs_dispose(pvfs_owned);
    remove(app);
    rmdir(other);
}

/* The app runtime never carries a tool context, even when the caller's struct
 * held one (the field is only kept in tool mode). */
UTEST(lua_tool_vm, app_runtime_drops_a_stray_unveil_context)
{
    HlToolUnveilCtx uctx;
    hl_tool_unveil_init(&uctx);
    hl_tool_unveil_seal(&uctx);
    HlVfs pvfs;
    void *pvfs_owned = NULL;
    hl_platform_vfs_init(&pvfs, &pvfs_owned);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;    /* sandboxed app runtime */
    HlLua app;
    memset(&app, 0, sizeof app);
    app.tool_unveil_ctx = &uctx;
    app.base.platform_vfs = &pvfs;
    ASSERT_EQ(hl_lua_init(&app, &cfg), 0);
    EXPECT_TRUE(app.tool_unveil_ctx == NULL);
    hl_lua_free(&app);
    hl_tool_unveil_free(&uctx);
    hl_platform_vfs_dispose(pvfs_owned);
}

/* ── Audit 9: the Lua runtime ─────────────────────────────────────────── */

#include "hull/limits/runtime.h"   /* HL_RES_HEADER_BYTES_MAX */

/* H2: integer keys hash to k % ((sizenode - 1) | 1) with no seed, so keys
 * k * 8191 in an 8192-node table all share one chain, and every lookup or
 * insert walked it inside a single instruction. The chain walk is charged
 * now (HULL PATCH 0004, docs/lua_patches.md). */
UTEST(lua_audit9, colliding_integer_keys_hit_the_limit)
{
    static const char *const setup =
        "M = 8191 T = {} for k = 1, 5000 do T[k * M] = true end";
    static const char *const cases[] = {
        "for i = 1, 1000 do local _ = T[(i % 5000 + 1) * M] end return 1",
        "for i = 1, 1000 do local _ = rawget(T, (i % 5000 + 1) * M) end return 1",
        "for i = 1, 1000 do local _ = T[(i % 5000 + 1) * M + 0.0] end return 1",
        "for i = 1, 1000 do T[(5000 + i) * M] = true end return 1",
        "for i = 1, 1000 do rawset(T, (5000 + i) * M, true) end return 1",
        NULL
    };
    char err[512];
    for (int i = 0; cases[i]; i++) {
        int rc = limited_run_after(setup, cases[i], err, sizeof err);
        EXPECT_NE_MSG(rc, LUA_OK, cases[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, cases[i]);
    }

    /* Ordinary integer and float keys stay well under the limit. */
    EXPECT_EQ(limited_run(
        "local t = {} for k = 1, 2000 do t[k * 7] = k end "
        "for k = 1, 2000 do assert(t[k * 7] == k) end "
        "local f = {} for k = 1, 500 do f[k + 0.5] = k end "
        "for k = 1, 500 do assert(f[k + 0.5] == k) end "
        "assert(#{ 1, 2, 3 } == 3) return 1",
        err, sizeof err), LUA_OK);
}

/* H3: verify_password runs as many PBKDF2 iterations as the STORED string
 * names (up to 10M): seconds of work counted as one instruction. It is
 * charged before the derivation now, so a run over its budget raises at
 * once, with nothing derived. */
UTEST(lua_audit9, verify_password_charges_its_iterations)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    lua_rt.max_instructions = 1000000;
    HL_LUA_ARM(&lua_rt, lua_rt.L);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = luaL_dostring(lua_rt.L,
        "local h = 'pbkdf2:10000000:' .. string.rep('00', 16) .. ':' .. "
        "          string.rep('00', 32) "
        "return crypto.verify_password('x', h)");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    EXPECT_NE(rc, LUA_OK);
    const char *e = lua_tostring(lua_rt.L, -1);
    EXPECT_NE(strstr(e ? e : "", "instruction limit"), NULL);
    EXPECT_LT((double)(t1.tv_sec - t0.tv_sec), 2.0);
    lua_settop(lua_rt.L, 0);

    /* The default hash still fits a run of the default limit. */
    lua_rt.max_instructions = HL_DEFAULT_INSTRUCTIONS;
    HL_LUA_ARM(&lua_rt, lua_rt.L);
    EXPECT_EQ(luaL_dostring(lua_rt.L,
        "local h = crypto.hash_password('pw') "
        "assert(crypto.verify_password('pw', h)) return 1"), LUA_OK);
    lua_settop(lua_rt.L, 0);
    cleanup_lua_caps();
}

/* M1 / M2: res:header appended to Keel's header buffer, outside the script
 * heap and with no cap. Past HL_RES_HEADER_BYTES_MAX it raises. res:text
 * added a Content-Type on every call; it sets one only when the response
 * has none now (an app's own wins), so a loop of it stacks nothing. */
UTEST(lua_audit9, response_headers_are_capped)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "app.manifest({modules = {'hull/http-server@1'}})\n"
        "app.use('*', '/*', function(req, res)\n"
        "  local v = string.rep('v', 100)\n"
        "  local ok, err = pcall(function()\n"
        "    for i = 1, 100000 do res:header('X-A', v) end end)\n"
        "  HDR_OK, HDR_ERR = ok, tostring(err)\n"
        "  return 1 end)\n"
        "app.use('*', '/*', function(req, res)\n"
        "  res:header('Content-Type', 'application/problem+json')\n"
        "  local ok, err = pcall(function()\n"
        "    res:json({ a = 1 }) res:html('<p>')\n"
        "    for i = 1, 100000 do res:text('x') end end)\n"
        "  TXT_OK, TXT_ERR = ok, tostring(err)\n"
        "  return 1 end)\n"), LUA_OK);
    for (int stage = 1; stage <= 2; stage++) {
        KlAllocator alloc = kl_allocator_default();
        KlHttpResponse res;
        ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
        KlHttpRequest req = {0};
        EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt,
                      first_mw_handler_id(L, stage), &req, &res), 1);
        EXPECT_LE(res.hdr_len, (size_t)HL_RES_HEADER_BYTES_MAX);
        if (stage == 2) {
            /* One Content-Type, the app's: nothing else was added. */
            EXPECT_EQ(res.hdr_len,
                      strlen("Content-Type: application/problem+json\r\n"));
        }
        free_lua_req_ctx(&req);
        kl_http_response_free(&res);
    }
    lua_getglobal(L, "HDR_OK");
    EXPECT_FALSE(lua_toboolean(L, -1));
    lua_getglobal(L, "HDR_ERR");
    EXPECT_NE(strstr(lua_tostring(L, -1) ? lua_tostring(L, -1) : "",
                     "would exceed"), NULL);
    lua_getglobal(L, "TXT_OK");
    EXPECT_TRUE(lua_toboolean(L, -1));
    lua_settop(L, 0);
    cleanup_lua();
}

/* M3: a hull.async task that tripped the budget after its first yield
 * never marked itself done (pcall re-raises the trip, so the code after it
 * never ran): its waiters stayed parked for good and hull._running stayed
 * raised. The runtime now runs the task's failure hook once that run is
 * over. */
UTEST(lua_audit9, a_task_that_trips_the_budget_still_finishes)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);
    lua_rt.max_instructions = 300000;

    lua_State *co;
    int st = ssh_co_start(&lua_rt,
        "local t = hull.async(function() hull.sleep(5) while true do end end)\n"
        "local ok, err = pcall(t.wait, t)\n"
        "if ok then OUT = 'wait returned'\n"
        "elseif not tostring(err):find('instruction limit') then\n"
        "  OUT = 'wrong error: ' .. tostring(err)\n"
        "elseif not t:done() then OUT = 'not done'\n"
        "elseif hull._running ~= 0 then OUT = 'running ' .. hull._running\n"
        "else OUT = 'ok' end\n", &co);
    ASSERT_EQ(st, LUA_YIELD);
    ssh_tick_until_done(be, lua_rt.base.async_ctx, co);
    lua_getglobal(lua_rt.L, "OUT");
    const char *out = lua_tostring(lua_rt.L, -1);
    EXPECT_STREQ(out ? out : "(no verdict: the waiter never woke)", "ok");
    lua_pop(lua_rt.L, 1);

    /* Tripped in its first, synchronous slice: that is the spawner's run,
     * which fails with it; the hook runs later, from the loop. */
    HL_LUA_ARM(&lua_rt, lua_rt.L);
    ASSERT_EQ(luaL_dostring(lua_rt.L, "T2 = nil"), LUA_OK);
    st = ssh_co_start(&lua_rt,
        "T2 = hull.async(function() while true do end end)\n", &co);
    (void)st;   /* the spawner may or may not get to its next hook */
    for (int i = 0; i < 5; i++) be->tick(lua_rt.base.async_ctx, 10);
    HL_LUA_ARM(&lua_rt, lua_rt.L);
    ASSERT_EQ(luaL_dostring(lua_rt.L,
        "return (T2 == nil or T2:done()) and hull._running == 0"), LUA_OK);
    EXPECT_TRUE(lua_toboolean(lua_rt.L, -1));
    lua_settop(lua_rt.L, 0);

    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();
    be->tick(actx, 0);
    be->free(actx);
}

/* ── Audit 10: the Lua runtime ────────────────────────────────────────── */

#include "hull/cap/smtp.h"   /* HlSmtpConfig */
#include "hull/cap/http.h"   /* HlHttpConfig */
#include "hull/cap/fs.h"     /* HlFsConfig */

/* Run @p code under a budget of @p limit on the caps runtime; the error
 * text, if any, lands in @p err. */
static int audit10_run(const char *code, int64_t limit, char *err, size_t n)
{
    lua_rt.max_instructions = limit;
    HL_LUA_ARM(&lua_rt, lua_rt.L);
    int rc = luaL_dostring(lua_rt.L, code);
    err[0] = '\0';
    if (rc != LUA_OK) {
        const char *e = lua_tostring(lua_rt.L, -1);
        snprintf(err, n, "%s", e ? e : "");
    }
    lua_settop(lua_rt.L, 0);
    lua_rt.max_instructions = HL_DEFAULT_INSTRUCTIONS;
    HL_LUA_ARM(&lua_rt, lua_rt.L);
    return rc;
}

/* M: every public-key operation counted as one instruction - a loop of
 * Ed25519 signs, X25519s or RSA-8192 signs ran far past the limit. Each is
 * charged before the work now: 2^14 units per scalar multiplication, RSA
 * ceil(bits / 1024)^3 * 2^14. */
UTEST(lua_audit10, asymmetric_crypto_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    static const char *const loops[] = {
        "local pk, sk = crypto.ed25519_keypair() "
        "for i = 1, 200 do crypto.ed25519_sign('m', sk) end return 1",
        "local pk, sk = crypto.ed25519_keypair() local s = crypto.ed25519_sign('m', sk) "
        "for i = 1, 200 do crypto.ed25519_verify('m', s, pk) end return 1",
        "for i = 1, 200 do crypto.ed25519_keypair() end return 1",
        "for i = 1, 200 do crypto.x25519_keypair() end return 1",
        "for i = 1, 200 do crypto.box_keypair() end return 1",
        "local pk, sk = crypto.x25519_keypair() local p2 = crypto.x25519_keypair() "
        "for i = 1, 200 do crypto.x25519(sk, p2) end return 1",
        "local pk, sk = crypto.box_keypair() local n = string.rep('n', 24) "
        "for i = 1, 200 do crypto.box('m', n, pk, sk) end return 1",
        NULL
    };
    /* 200 operations: 3.3M units, over a 1M limit; the loops themselves are
     * a few thousand instructions. */
    for (int i = 0; loops[i]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 1000000, err, sizeof err),
                      LUA_OK, loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    /* A few operations fit. */
    EXPECT_EQ(audit10_run(
        "local pk, sk = crypto.ed25519_keypair() "
        "local s = crypto.ed25519_sign('m', sk) "
        "assert(crypto.ed25519_verify('m', s, pk)) return 1",
        1000000, err, sizeof err), LUA_OK);

    /* RSA is charged by size before the key is parsed: a 5000-byte PEM is
     * ~6.7k bits (7^3 * 2^14 units), a 1024-byte signature against a key
     * that may be that long 8192 bits (8^3 * 2^14), both over the limit at
     * once (the junk key would otherwise be refused, or the signature be
     * false). */
    EXPECT_NE(audit10_run(
        "return crypto.sign('RS256', string.rep('A', 5000), 'm')",
        1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_NE(audit10_run(
        "return crypto.verify('RS256', string.rep('A', 2000), 'm', string.rep('s', 1024))",
        1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* An ES256 verify is one scalar multiplication. */
    EXPECT_EQ(audit10_run(
        "return crypto.verify('ES256', 'junk', 'm', string.rep('s', 64))",
        1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
}

/* Audit 11 L: an RSA verify was charged by the signature's length, which is
 * the attacker's: a 1024-byte signature cost an RSA-8192's 8^3 * 2^14 units
 * whatever the key. It is bounded by the public key's PEM now (at most
 * 6 bits per PEM byte), and a signature longer than any key mbedTLS takes is
 * false, uncharged. The same numbers as the JS runtime. */
UTEST(lua_audit11, rsa_verify_is_charged_by_the_key)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    /* 100 verifies of a 1024-byte signature against a 300-byte PEM (at most
     * 1800 bits: 2^3 * 2^14 units each, 13M in all) fit a 100M budget; at
     * the signature's 8192 bits they were 860M. */
    EXPECT_EQ(audit10_run(
        "local pem = string.rep('A', 300) local sig = string.rep('s', 1024) "
        "for i = 1, 100 do assert(not crypto.verify('RS256', pem, 'm', sig)) end "
        "return 1", 100000000, err, sizeof err), LUA_OK);
    /* An oversized signature is false and costs nothing beyond its data. */
    EXPECT_EQ(audit10_run(
        "local pem = string.rep('A', 20000) local sig = string.rep('s', 4096) "
        "for i = 1, 1000 do assert(not crypto.verify('PS256', pem, 'm', sig)) end "
        "return 1", 1000000, err, sizeof err), LUA_OK);
    /* A long key with a signature its length is still charged in full. */
    EXPECT_NE(audit10_run(
        "return crypto.verify('RS256', string.rep('A', 2000), 'm', string.rep('s', 1024))",
        1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    cleanup_lua_caps();
}

/* L: fs.write wrote up to the whole heap as one instruction. Charged before
 * the write now (one unit per 8 bytes), so the charge holds even for a
 * write the policy then refuses. */
UTEST(lua_audit10, fs_write_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_a10"), NULL);
    HlFsConfig fs = { tmpdir, strlen(tmpdir), NULL };   /* no grants */
    lua_rt.base.fs_cfg = &fs;
    char err[512];
    ASSERT_EQ(audit10_run("BIG = string.rep('x', 8 * 1000000) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE(audit10_run("for i = 1, 10 do fs.write('a.bin', BIG) end return 1",
                          200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* A small write is cheap (and refused: no grant). */
    EXPECT_EQ(audit10_run("local ok = fs.write('a.bin', 'x') assert(ok == nil) return 1",
                          200000, err, sizeof err), LUA_OK);
    lua_rt.base.fs_cfg = NULL;
    cleanup_lua_caps();
    rmdir(tmpdir);
}

#ifdef HL_ENABLE_HTTP_CLIENT
/* L: smtp.send read cc's length through __len and dropped, silently, any
 * entry it could not copy (a non-string, or one the scratch arena had no
 * room for): the message went out to fewer recipients than asked. Every
 * entry is copied now, or the call raises. */
UTEST(lua_audit10, smtp_cc_is_copied_whole_or_refused)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlSmtpConfig smtp = {0};
    lua_rt.base.smtp_cfg = &smtp;
    char err[512];
    static const char *const base =
        "local o = { host = 'mail.invalid', from = 'a@x.test', to = 'b@x.test', "
        "            subject = 's', body = 'b' } ";
    char code[1024];

    snprintf(code, sizeof code, "%s o.cc = { 'c@x.test', 42 } "
             "return smtp.send(o)", base);
    EXPECT_NE(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "cc[2] must be a string"), NULL);

    snprintf(code, sizeof code, "%s o.cc = 'c@x.test' return smtp.send(o)", base);
    EXPECT_NE(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "cc must be an array"), NULL);

    /* __len is not consulted: the raw length is. */
    snprintf(code, sizeof code, "%s o.cc = setmetatable({}, { __len = function() "
             "return 1e9 end }) local r = smtp.send(o) "
             "assert(type(r) == 'table') return 1", base);
    EXPECT_EQ(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);

    lua_rt.base.smtp_cfg = NULL;
    cleanup_lua_caps();
}

/* http.* opts.timeout_ms: a positive integer of milliseconds, or absent.
 * Anything else raises before the request starts (a mistyped timeout must
 * not silently become the 30 s default). The clamp itself is the cap
 * layer's (test_http.c timeout.*). */
UTEST(lua_http_timeout, per_call_option_is_validated)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlHttpConfig cfg = {0};          /* no hosts: every request is refused */
    lua_rt.base.http_cfg = &cfg;
    char err[512];
    static const char *const bad[] = {
        "http.get('http://x.invalid/', { timeout_ms = 0 })",
        "http.get('http://x.invalid/', { timeout_ms = -5 })",
        "http.get('http://x.invalid/', { timeout_ms = 1.5 })",
        "http.get('http://x.invalid/', { timeout_ms = '1000' })",
        "http.post('http://x.invalid/', 'b', { timeout_ms = {} })",
        "http.delete('http://x.invalid/', { timeout_ms = true })",
        "http.request('GET', 'http://x.invalid/', { timeout_ms = 0 })",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char code[512];
        snprintf(code, sizeof code,
                 "local http = require('hull.http-client') %s", bad[i]);
        EXPECT_NE_MSG(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err),
                      LUA_OK, bad[i]);
        EXPECT_NE_MSG(strstr(err, "opts.timeout_ms must be a positive integer"),
                      NULL, bad[i]);
    }
    /* A valid value (any size: the cap clamps it) gets as far as the host
     * check, which refuses the host. */
    static const char *const good[] = {
        "http.get('http://x.invalid/', { timeout_ms = 1000 })",
        "http.get('http://x.invalid/', { timeout_ms = 1000.0 })",
        "http.put('http://x.invalid/', 'b', { timeout_ms = 999999999 })",
        "http.get('http://x.invalid/', {})",
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        char code[512];
        snprintf(code, sizeof code,
                 "local http = require('hull.http-client') %s", good[i]);
        EXPECT_NE_MSG(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err),
                      LUA_OK, good[i]);
        EXPECT_EQ_MSG(strstr(err, "timeout_ms"), NULL, good[i]);
        EXPECT_NE_MSG(strstr(err, "failed"), NULL, good[i]);
    }
    lua_rt.base.http_cfg = NULL;
    cleanup_lua_caps();
}
#endif

#ifdef HL_ENABLE_HTTP_SERVER
/* L: test.get runs the app's dispatch, an entry: it re-armed the budget (a
 * case looping over test.get never hit the limit) and its stale-transaction
 * guard rolled back a transaction the CASE had open. The case's budget is
 * kept and charged with the request's work now, and the guard is held off
 * while the case has a transaction. */
UTEST(lua_audit10, nested_test_request_keeps_the_case_budget_and_txn)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "app.manifest({modules = {'hull/http-server@1'}})\n"
        "db.exec('CREATE TABLE a10 (x INTEGER)')\n"
        "app.get('/spin', function(req, res) "
        "  for i = 1, 30000 do end res:json({ ok = true }) end)\n"
        "app.get('/write', function(req, res) "
        "  db.exec('INSERT INTO a10 VALUES (2)') res:json({ ok = true }) end)\n"),
        LUA_OK);
    KlHttpRouter router;
    KlAllocator kalloc = kl_allocator_default();
    kl_http_router_init(&router, &kalloc);
    ASSERT_EQ(hl_lua_wire_routes(&lua_rt, &router), 0);
    hl_lua_test_register(L, &router, &lua_rt);
    ASSERT_EQ(luaL_dostring(L,
        "test('loops over test.get', function() "
        "  for i = 1, 10 do test.get('/spin') end end)\n"
        "test('one request fits', function() "
        "  local r = test.get('/spin') assert(r.status == 200) end)\n"
        "test('inside db.batch', function() "
        "  db.batch(function() "
        "    db.exec('INSERT INTO a10 VALUES (1)') "
        "    local r = test.get('/write') assert(r.status == 200) "
        "  end) "
        "  local rows = db.query('SELECT x FROM a10 ORDER BY x') "
        "  assert(#rows == 2, 'rows: ' .. #rows) end)\n"), LUA_OK);
    lua_rt.max_instructions = 100000;
    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[4];
    memset(results, 0, sizeof results);
    hl_lua_test_run(L, &total, &passed, &failed, NULL, results, 4);
    lua_rt.max_instructions = HL_DEFAULT_INSTRUCTIONS;
    EXPECT_EQ(total, 3);
    EXPECT_FALSE(results[0].passed);
    EXPECT_NE(strstr(results[0].error, "instruction limit"), NULL);
    EXPECT_TRUE_MSG(results[1].passed, results[1].error);
    EXPECT_TRUE_MSG(results[2].passed, results[2].error);

    kl_http_router_free(&router);   /* the route contexts: hl_lua_free */
    cleanup_lua_caps();
}
#endif
/* ── Audit 10: the shared response-header fixes, Lua side ─────────────── */

#include "hull/http_feature.h"   /* hl_lua_http_error_response */
#include <strings.h>             /* strncasecmp */

/* How many @p name headers the response carries (case-insensitive). */
static int a10_header_count(const KlHttpResponse *res, const char *name)
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

static int a10_headers_contain(const KlHttpResponse *res, const char *needle)
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

/* M (#712 regression): res:json / html / text kept the FIRST Content-Type,
 * whoever set it - res:html then res:json sent the JSON as text/html. Hull's
 * own default is replaced by the next body call; an app-set one stays; there
 * is always exactly one. res:bytes drops a default an earlier call left. */
UTEST(lua_audit10, a_body_call_replaces_hulls_own_content_type)
{
    static const struct { const char *body, *want; } cases[] = {
        { "res:html('<p>') res:json({ a = 1 })",
          "Content-Type: application/json\r\n" },
        { "res:json(1) res:text('x')",
          "Content-Type: text/plain; charset=utf-8\r\n" },
        { "res:header('Content-Type', 'text/csv') res:text('a') res:json(1)",
          "Content-Type: text/csv\r\n" },
        { "res:text('a') res:header('Content-Type', 'image/png') res:bytes('\\0\\1')",
          "Content-Type: image/png\r\n" },
        { "res:html('<p>') res:bytes('\\0\\1')", NULL },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        init_lua();
        ASSERT_TRUE(lua_initialized);
        lua_State *L = lua_rt.L;
        char code[512];
        snprintf(code, sizeof code,
            "app.manifest({modules = {'hull/http-server@1'}})\n"
            "app.use('*', '/*', function(req, res) %s return 1 end)\n",
            cases[i].body);
        ASSERT_EQ(luaL_dostring(L, code), LUA_OK);
        KlAllocator alloc = kl_allocator_default();
        KlHttpResponse res;
        ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
        KlHttpRequest req = {0};
        EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt,
                      first_mw_handler_id(L, 1), &req, &res), 1);
        EXPECT_EQ_MSG(a10_header_count(&res, "Content-Type"),
                      cases[i].want ? 1 : 0, cases[i].body);
        if (cases[i].want)
            EXPECT_TRUE_MSG(a10_headers_contain(&res, cases[i].want), cases[i].body);
        free_lua_req_ctx(&req);
        kl_http_response_free(&res);
        lua_settop(L, 0);
        cleanup_lua();
    }
}

/* L: the 500 a failed handler gets kept every header it had set (a
 * Set-Cookie, a Location, a second Content-Type), and res:bytes after a
 * gzipped body kept the gzip's Content-Encoding / Vary. */
UTEST(lua_audit10, error_response_and_bytes_drop_stale_headers)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "app.manifest({modules = {'hull/http-server@1'}})\n"
        "app.use('*', '/*', function(req, res)\n"
        "  res:header('Set-Cookie', 'sid=1') res:header('Location', '/x')\n"
        "  res:html('<p>') return 1 end)\n"
        "app.use('*', '/*', function(req, res) res:bytes('12345678') return 1 end)\n"),
        LUA_OK);
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt,
                  first_mw_handler_id(L, 1), &req, &res), 1);
    hl_lua_http_error_response(&res);
    EXPECT_EQ(res.status, 500);
    EXPECT_EQ(a10_header_count(&res, "Set-Cookie"), 0);
    EXPECT_EQ(a10_header_count(&res, "Location"), 0);
    EXPECT_EQ(a10_header_count(&res, "Content-Type"), 1);
    EXPECT_TRUE(a10_headers_contain(&res, "Content-Type: text/plain\r\n"));
    free_lua_req_ctx(&req);
    kl_http_response_free(&res);

    KlHttpResponse res2;
    ASSERT_EQ(kl_http_response_init(&res2, &alloc), 0);
    ASSERT_EQ(kl_http_response_header(&res2, "Content-Encoding", "gzip"), 0);
    ASSERT_EQ(kl_http_response_header(&res2, "Vary", "Accept-Encoding"), 0);
    KlHttpRequest req2 = {0};
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt,
                  first_mw_handler_id(L, 2), &req2, &res2), 1);
    EXPECT_EQ(a10_header_count(&res2, "Content-Encoding"), 0);
    EXPECT_EQ(a10_header_count(&res2, "Vary"), 0);
    EXPECT_EQ(res2.body_len, (size_t)8);
    free_lua_req_ctx(&req2);
    kl_http_response_free(&res2);
    lua_settop(L, 0);
    cleanup_lua();
}

/* ── Audit 11: Content-Type and error headers across middleware ────────── */

/* L: whether the Content-Type is Hull's default was a flag on the per-call
 * `res` object, so a middleware's res:html followed by the handler's res:json
 * (two objects, one response) kept text/html for the JSON. And the 500 a
 * failed handler gets dropped the headers earlier middleware set (CSP, HSTS,
 * CORS, a request id) along with the handler's own. */
UTEST(lua_audit11, content_type_and_error_headers_span_middleware)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "app.manifest({modules = {'hull/http-server@1'}})\n"
        "app.use('*', '/*', function(req, res)\n"
        "  res:header('Strict-Transport-Security', 'max-age=1') res:html('<p>')\n"
        "  return 0 end)\n"
        "app.use('*', '/*', function(req, res)\n"
        "  res:header('Content-Type', 'application/json') return 0 end)\n"
        "app.use('*', '/*', function(req, res) res:json({ a = 1 }) end)\n"
        "app.use('*', '/*', function(req, res)\n"
        "  res:header('Set-Cookie', 'sid=1') res:json(1) error('boom') end)\n"
        "app.use('*', '/*', function(req, res) res:html('<p>') end)\n"),
        LUA_OK);
    KlAllocator alloc = kl_allocator_default();

    /* Middleware res:html, handler res:json: one Content-Type, the JSON's. */
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt, first_mw_handler_id(L, 1),
                                         &req, &res), 0);
    EXPECT_EQ(hl_lua_dispatch(&lua_rt, first_mw_handler_id(L, 3), &req, &res), 0);
    EXPECT_EQ(a10_header_count(&res, "Content-Type"), 1);
    EXPECT_TRUE(a10_headers_contain(&res, "Content-Type: application/json\r\n"));
    free_lua_req_ctx(&req);
    kl_http_response_free(&res);

    /* The handler fails: the middleware's HSTS stays, the handler's
     * Set-Cookie and Content-Type go, one Content-Type (the error's). */
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req2 = {0};
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt, first_mw_handler_id(L, 1),
                                         &req2, &res), 0);
    EXPECT_EQ(hl_lua_dispatch(&lua_rt, first_mw_handler_id(L, 4), &req2, &res), -1);
    hl_lua_http_error_response(&res);
    EXPECT_EQ(res.status, 500);
    EXPECT_EQ(a10_header_count(&res, "Strict-Transport-Security"), 1);
    EXPECT_EQ(a10_header_count(&res, "Set-Cookie"), 0);
    EXPECT_EQ(a10_header_count(&res, "Content-Type"), 1);
    EXPECT_TRUE(a10_headers_contain(&res, "Content-Type: text/plain\r\n"));
    free_lua_req_ctx(&req2);
    kl_http_response_free(&res);

    /* An app Content-Type spelled like a default is still the app's: the
     * handler's res:html keeps it. */
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req3 = {0};
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt, first_mw_handler_id(L, 2),
                                         &req3, &res), 0);
    EXPECT_EQ(hl_lua_dispatch(&lua_rt, first_mw_handler_id(L, 5), &req3, &res), 0);
    EXPECT_EQ(a10_header_count(&res, "Content-Type"), 1);
    EXPECT_TRUE(a10_headers_contain(&res, "content-type: application/json\r\n"));
    free_lua_req_ctx(&req3);
    kl_http_response_free(&res);
    lua_settop(L, 0);
    cleanup_lua();
}

UTEST_MAIN();
