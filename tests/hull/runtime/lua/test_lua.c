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
#include "hull/cap/blob.h"   /* hl_cap_blob_live_handles (a34) */
#ifdef HL_ENABLE_HTTP
#include "hull/cap/ssh.h"    /* hl_cap_ssh_live_streams (a35) */
#endif
#include "hull/reqctx.h"
#include "hull/vfs.h"
#include "hull/stdlib_feature.h"
#include "hull/cap/db.h"

#include "../../lua_script_test.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"
#include "hull/cap/db_registry.h"
#include "hull/cap/env.h"
#include "hull/cap/crypto.h"  /* hl_cap_crypto_random_fail_next_for_test (a36) */
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm.h"   /* HlWasmCache (lua_audit15 compute.stream) */
#endif
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
/* __test_hooks(name): the test hooks stdlib module `name` registered with
 * hull._test_hooks (audit 16: no longer exported on the module, where app
 * code could call them). That module is stdlib-internal, so the harness loads
 * both through the runtime's trusted require - C code, which no script can
 * reach. */
static int test_hooks_get(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    lua_pushcfunction(L, hl_lua_require_trusted);
    lua_pushstring(L, name);
    lua_call(L, 1, 0);
    lua_pushcfunction(L, hl_lua_require_trusted);
    lua_pushliteral(L, "hull._test_hooks");
    lua_call(L, 1, 1);
    lua_getfield(L, -1, "get");
    lua_pushstring(L, name);
    lua_call(L, 1, 1);
    return 1;
}

static void install_test_globals(lua_State *L)
{
    lua_pushcfunction(L, test_hooks_get);
    lua_setglobal(L, "__test_hooks");

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

#ifdef HL_ENABLE_HTTP_SERVER
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
#endif

/* Init lua with database and env capabilities for testing */
static sqlite3 *test_db = NULL;

/* The harness's own SQL on _hull_* tables (setting up an old schema, aging a
 * claim) runs as Hull's: the SQLite authorizer refuses `_hull_*` objects to
 * untrusted SQL (audit 39). */
static int trusted_exec(sqlite3 *db, const char *sql, int (*cb)(void *, int, char **, char **),
                        void *ud, char **err)
{
    int prev = hl_cap_db_ns_trust(1);
    int rc = sqlite3_exec(db, sql, cb, ud, err);
    hl_cap_db_ns_restore(prev);
    return rc;
}

static int trusted_prepare(sqlite3 *db, const char *sql, int n, sqlite3_stmt **st,
                           const char **tail)
{
    int prev = hl_cap_db_ns_trust(1);
    int rc = sqlite3_prepare_v2(db, sql, n, st, tail);
    hl_cap_db_ns_restore(prev);
    return rc;
}
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
    /* A script is a whole suite in one uninterrupted run, not one entry, so
     * the run watchdog's per-run deadline (60 s) does not apply to it: under
     * MSan test_stdlib_audit16.lua alone runs past it. Off for the script
     * (and the runs nested in it), any deadline already armed cleared. */
    hl_run_watchdog_configure(0);
    hl_run_watch_disarm(&lua_rt.run_watch);
    rc = lua_pcall(lua_rt.L, 0, 1, 0);
    hl_run_watchdog_configure(-1);
    if (rc != LUA_OK) {
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
        "planted.lua", "templates/planted.html", "templates",
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

/* ── Audit 17: a built binary loads modules / templates only from its VFS ─
 *
 * A non-empty app VFS is a built binary: its relative requires and its
 * templates come from what it embedded. Unfixed, a module or template not
 * embedded was read from <app_dir> on disk - code planted beside the binary
 * ran, without --verify-sig. With an empty VFS (development) the disk
 * fallback stays. */

static int expose_template_bridge(lua_State *L);   /* below */

static const unsigned char a17_tpl[] = "embedded template";
static const HlEntry a17_built_entries[] = {
    { "templates/embedded.html", a17_tpl, sizeof a17_tpl - 1 },
    { 0, 0, 0 }
};

static void a17_plant(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/planted.lua", dir);
    write_file(path, "return { val = 17 }\n");
    snprintf(path, sizeof(path), "%s/templates", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/templates/planted.html", dir);
    write_file(path, "planted template");
}

UTEST(lua_audit17, relative_require_reads_disk_only_in_development)
{
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_a17"), NULL);
    a17_plant(tmpdir);

    /* Development (no app VFS): the module on disk loads. */
    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);
    EXPECT_EQ(eval_int("(function() return require('./planted').val end)()"),
              17);

    /* Built (non-empty app VFS): it does not. */
    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);
    HlVfs vfs;
    hl_vfs_init(&vfs, a17_built_entries, tmpdir);
    lua_rt.base.app_vfs = &vfs;
    EXPECT_EQ(eval_int("(function() local ok = pcall(require, './planted') "
                       "return ok and 1 or 0 end)()"), 0);
    lua_rt.base.app_vfs = NULL;

    cleanup_lua();
    rm_rf(tmpdir);
}

UTEST(lua_audit17, template_reads_disk_only_in_development)
{
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_a17"), NULL);
    a17_plant(tmpdir);

    /* Development: the template on disk is read. */
    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(expose_template_bridge(lua_rt.L), LUA_OK);
    char *got = eval_str("__tb._load_raw('planted.html')");
    EXPECT_TRUE(got && strcmp(got, "planted template") == 0);
    free(got);

    /* Built: only what was embedded. */
    init_lua_with_appdir(tmpdir);
    ASSERT_TRUE(lua_initialized);
    HlVfs vfs;
    hl_vfs_init(&vfs, a17_built_entries, tmpdir);
    lua_rt.base.app_vfs = &vfs;
    ASSERT_EQ(expose_template_bridge(lua_rt.L), LUA_OK);
    EXPECT_EQ(eval_int("__tb._load_raw('planted.html') == nil and 1 or 0"), 1);
    got = eval_str("__tb._load_raw('embedded.html')");
    EXPECT_TRUE(got && strcmp(got, "embedded template") == 0);
    free(got);
    lua_rt.base.app_vfs = NULL;

    cleanup_lua();
    rm_rf(tmpdir);
}

/* Audit 40: a built binary runs only its embedded entry. An entry it did not
 * embed (a symlinked app.lua planted beside it) is never read from disk. */
UTEST(lua_audit40, entry_reads_disk_only_in_development)
{
    char tmpdir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(tmpdir, sizeof tmpdir, "hull_a40"), NULL);
    char path[HL_TEST_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/planted.lua", tmpdir);
    write_file(path, "__a40 = 40\n");

    /* Development (no app VFS): the entry on disk loads. */
    init_lua_with_appdir(NULL); /* hl_lua_load_app sets app_dir */
    ASSERT_TRUE(lua_initialized);
    EXPECT_EQ(hl_lua_load_app(&lua_rt, path), 0);
    EXPECT_EQ(eval_int("__a40 or 0"), 40);

    /* Built (non-empty app VFS): it does not. */
    init_lua_with_appdir(NULL); /* hl_lua_load_app sets app_dir */
    ASSERT_TRUE(lua_initialized);
    HlVfs vfs;
    hl_vfs_init(&vfs, a17_built_entries, tmpdir);
    lua_rt.base.app_vfs = &vfs;
    EXPECT_NE(hl_lua_load_app(&lua_rt, path), 0);
    EXPECT_EQ(eval_int("__a40 or 0"), 0);
    lua_rt.base.app_vfs = NULL;

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

/* Audit 29: print wrote app text straight to stderr - a CR / LF / ESC forged
 * log or terminal lines - and charged nothing. It is one log record now,
 * escaped, tagged [app], and charged 1 unit per 8 bytes before the write. */
static char g_lua_print_log[1024];
static void lua_print_log_collect(log_Event *ev)
{
    char buf[1024];
    va_list ap;
    va_copy(ap, ev->ap);
    vsnprintf(buf, sizeof buf, ev->fmt, ap);
    va_end(ap);
    if (strstr(buf, "PRINTPROBE"))
        snprintf(g_lua_print_log, sizeof g_lua_print_log, "%s", buf);
}

UTEST(lua_audit29, print_is_an_escaped_log_record)
{
    static int registered;
    if (!registered) {
        log_add_callback(lua_print_log_collect, NULL, LOG_TRACE);
        registered = 1;
    }
    g_lua_print_log[0] = '\0';
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(luaL_dostring(lua_rt.L,
        "print('PRINTPROBE a\\nforged\\r\\27[2J', 7, nil)"), LUA_OK);
    EXPECT_NE_MSG(strstr(g_lua_print_log, "[app] PRINTPROBE a\\nforged\\r"
                                          "\\x1b[2J\t7\tnil"), NULL,
                  g_lua_print_log);
    EXPECT_EQ(strchr(g_lua_print_log, '\n'), NULL);
    EXPECT_EQ(strchr(g_lua_print_log, '\r'), NULL);
    EXPECT_EQ(strchr(g_lua_print_log, '\x1b'), NULL);
    cleanup_lua_caps();
}

UTEST(lua_audit29, print_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    lua_rt.max_instructions = HL_DEFAULT_INSTRUCTIONS;
    HL_LUA_ARM(&lua_rt, lua_rt.L);
    ASSERT_EQ(luaL_dostring(lua_rt.L,
        "S = string.rep('x', 8 * 1024 * 1024)"), LUA_OK);
    /* 8 MB is 1M units: over this run's limit before a byte is written. */
    lua_rt.max_instructions = 200000;
    HL_LUA_ARM(&lua_rt, lua_rt.L);
    int rc = luaL_dostring(lua_rt.L, "pcall(print, S) return 1");
    EXPECT_NE(rc, LUA_OK);
    const char *e = lua_tostring(lua_rt.L, -1);
    EXPECT_NE_MSG(strstr(e ? e : "", "instruction limit"), NULL, e ? e : "");
    lua_settop(lua_rt.L, 0);
    lua_rt.max_instructions = HL_DEFAULT_INSTRUCTIONS;
    HL_LUA_ARM(&lua_rt, lua_rt.L);
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

/* Audit 24: gpu.devices entries of the wrong type were skipped and not
 * counted as listed, so a list of nothing but strings / floats was the
 * empty list - every device (/dev/dri unveiled whole). Each entry counts
 * now, so a list with no valid index disables gpu; a devices value that is
 * not a list does too. */
UTEST(lua_runtime, manifest_gpu_devices_fail_closed)
{
    static const struct { const char *decl; int gpu; int count; } cases[] = {
        { "app.manifest({ gpu = { devices = { 0, 2 } } })",        1, 2 },
        { "app.manifest({ gpu = { devices = {} } })",              1, 0 },
        { "app.manifest({ gpu = {} })",                            1, 0 },
        { "app.manifest({ gpu = { devices = { '0' } } })",         0, 0 },
        { "app.manifest({ gpu = { devices = { 0.5 } } })",         0, 0 },
        { "app.manifest({ gpu = { devices = { 1, '0' } } })",      1, 1 },
        { "app.manifest({ gpu = { devices = { x = 0 } } })",       0, 0 },
        { "app.manifest({ gpu = { devices = '0' } })",             0, 0 },
        { "app.manifest({ gpu = { devices = 0 } })",               0, 0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        init_lua();
        ASSERT_TRUE(lua_initialized);
        ASSERT_EQ(luaL_dostring(lua_rt.L, cases[i].decl), LUA_OK);
        HlManifest m;
        ASSERT_EQ(hl_manifest_extract_lua(lua_rt.L, &m, NULL), 0);
        EXPECT_EQ_MSG(m.gpu, cases[i].gpu, cases[i].decl);
        EXPECT_EQ_MSG(m.gpu_device_count, cases[i].count, cases[i].decl);
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

#ifdef HL_ENABLE_HTTP_SERVER
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
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
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

/* Audit 17: a middleware's number is truncated toward zero, sign kept, as
 * the JS runtime reads it. 1.5 read as 0 ("continue": JS stops) and 2^32
 * through the (int) cast as 0. */
UTEST(lua_middleware, dispatch_number_return_matches_js)
{
    static const struct { const char *ret; int want; } cases[] = {
        { "1.5", 1 }, { "0.5", 0 }, { "-0.5", 0 }, { "-1.5", -1 },
        { "2^32", 1 }, { "4294967296", 1 }, { "-4294967296", -1 },
        { "0/0", 0 }, { "1/0", 1 }, { "'1.5'", 1 }, { "0.0", 0 },
        { "nil", 0 }, { "true", 1 }, { "2", 1 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        init_lua();
        ASSERT_TRUE(lua_initialized);
        char code[256];
        snprintf(code, sizeof code,
            "app.manifest({modules = {'hull/http-server@1'}})\n"
            "app.use('*', '/*', function(req, res) return %s end)\n",
            cases[i].ret);
        ASSERT_EQ(luaL_dostring(lua_rt.L, code), LUA_OK);
        lua_getfield(lua_rt.L, LUA_REGISTRYINDEX, "__hull_middleware");
        lua_rawgeti(lua_rt.L, -1, 1);
        lua_getfield(lua_rt.L, -1, "handler_id");
        int handler_id = (int)lua_tointeger(lua_rt.L, -1);
        lua_pop(lua_rt.L, 3);
        KlHttpRequest req = {0};
        KlHttpResponse res = {0};
        EXPECT_EQ_MSG(hl_lua_dispatch_middleware(&lua_rt, handler_id,
                                                 &req, &res),
                      cases[i].want, cases[i].ret);
        free_lua_req_ctx(&req);
        cleanup_lua();
    }
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
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
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
#endif /* HL_ENABLE_HTTP_SERVER */

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

/* A failed random source makes crypto.ed25519_keypair() raise a catchable
 * error; it used to abort() the process inside TweetNaCl's randombytes()
 * (audit 36). The next call works. */
UTEST(lua_cap, crypto_ed25519_keypair_entropy_failure_raises)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    hl_cap_crypto_random_fail_next_for_test(1);
    int step = eval_int(
        "(function() "
        "  local ok, err = pcall(crypto.ed25519_keypair) "
        "  if ok then return 1 end "
        "  if not tostring(err):find('keypair generation failed', 1, true) then return 2 end "
        "  local pk, sk = crypto.ed25519_keypair() "
        "  if #pk ~= 32 or #sk ~= 64 then return 3 end "
        "  return 0 "
        "end)()");
    hl_cap_crypto_random_fail_next_for_test(0);
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
        "  return __test_hooks('hull.web.middleware.totp').base32_encode('foobar') "
        "end)()");
    ASSERT_NE(b32, NULL);
    ASSERT_STREQ(b32, "MZXW6YTBOI");
    free(b32);

    /* Base32 round-trip on the 20-byte RFC 6238 key. */
    int rt = eval_int(
        "(function() "
        "  local t = __test_hooks('hull.web.middleware.totp') "
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
        "__test_hooks('hull.web.middleware.totp')"
        ".totp_at_step('12345678901234567890', 1, 8)");
    ASSERT_NE(v1, NULL); ASSERT_STREQ(v1, "94287082"); free(v1);

    char *v2 = eval_str(
        "__test_hooks('hull.web.middleware.totp')"
        ".totp_at_step('12345678901234567890', 37037036, 8)");
    ASSERT_NE(v2, NULL); ASSERT_STREQ(v2, "07081804"); free(v2);

    char *v3 = eval_str(
        "__test_hooks('hull.web.middleware.totp')"
        ".totp_at_step('12345678901234567890', 41152263, 8)");
    ASSERT_NE(v3, NULL); ASSERT_STREQ(v3, "89005924"); free(v3);

    char *v4 = eval_str(
        "__test_hooks('hull.web.middleware.totp')"
        ".totp_at_step('12345678901234567890', 66666666, 8)");
    ASSERT_NE(v4, NULL); ASSERT_STREQ(v4, "69279037"); free(v4);

    /* 6-digit truncation of the same step uses the same dynamic offset;
     * value should match the last 6 digits of the 8-digit form (mod 10^6). */
    char *v1_6 = eval_str(
        "__test_hooks('hull.web.middleware.totp')"
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
     * Code generation uses __test_hooks('hull.web.middleware.totp').totp_at_step against the secret
     * the enroll returned (decoded from base32), so the test mirrors
     * what a real authenticator app would do without needing one
     * present. */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  __test_hooks('hull.web.middleware.totp').reset() "
        "  totp.init({ issuer = 'TestApp' }) "
        "  local r = totp.enroll('user-1') "
        "  if type(r.secret_base32) ~= 'string' then return 0 end "
        "  if not r.qr_svg:find('<svg', 1, true) then return 0 end "
        "  if #r.recovery_codes ~= 10 then return 0 end "
        "  if not r.otpauth_url:find('otpauth://totp/TestApp:user%-1') then return 0 end "
        "  local secret = __test_hooks('hull.web.middleware.totp').base32_decode(r.secret_base32) "
        "  local step = __test_hooks('hull.web.middleware.totp').current_step() "
        "  local code = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step, 6) "
        "  if not totp.confirm('user-1', code) then return 0 end "
        "  if not totp.enrolled('user-1') then return 0 end "
        "  if totp.verify('user-1', code) then return 0 end "
        "  local next_code = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step + 1, 6) "
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
        "  __test_hooks('hull.web.middleware.totp').reset() "
        "  totp.init({ pending_ttl = 60, cleanup = false, window = 10, "
        "               recovery_codes = 0 }) "
        "  local r = totp.enroll('u-fresh') "
        "  totp.enroll('u-stale') "
        "  __test_hooks('hull.web.middleware.totp').force_pending_stale('u-stale') "
        "  if totp.cleanup() ~= 1 then return 0 end "
        "  if totp.cleanup() ~= 0 then return 0 end "
        "  local secret = __test_hooks('hull.web.middleware.totp').base32_decode(r.secret_base32) "
        "  local step = __test_hooks('hull.web.middleware.totp').current_step() "
        "  local good = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step, 6) "
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
     * counter. Apps that want UX can read totp.lockout_remaining.
     *
     * The wrong codes are chosen to differ from the real code at every
     * step from step - 5 to step + 6 (the window is 3, plus drift): fixed
     * 000000 / 000001 / 000002 matched a real code at an eligible step
     * about once in 16000 runs (audit 29). The lockout bound allows a
     * second of clock movement either way. */
    int ok = eval_int(
        "(function() "
        "  local totp = require('hull.web.middleware.totp') "
        "  __test_hooks('hull.web.middleware.totp').reset() "
        /* recovery_codes = 0: each failed verify would otherwise
         * walk all stored recovery codes with PBKDF2 (10×~1s under
         * MSan). Drop them; the lockout path doesn't care. */
        "  totp.init({ max_failed_attempts = 3, lockout_duration = 60, "
        "               window = 3, recovery_codes = 0 }) "
        "  local r = totp.enroll('u1') "
        "  local secret = __test_hooks('hull.web.middleware.totp').base32_decode(r.secret_base32) "
        "  local step = __test_hooks('hull.web.middleware.totp').current_step() "
        "  local at = __test_hooks('hull.web.middleware.totp').totp_at_step "
        "  local function wrong(seed) "
        "    for c = seed, 999999 do "
        "      local code, hit = string.format('%06d', c), false "
        "      for s = step - 5, step + 6 do "
        "        if at(secret, s, 6) == code then hit = true break end "
        "      end "
        "      if not hit then return code end "
        "    end "
        "  end "
        "  local w0, w1, w2 = wrong(0), wrong(100), wrong(200) "
        "  if not totp.confirm('u1', __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step, 6)) "
        "    then return 0 end "
        "  if totp.verify('u1', w0) then return 0 end "
        "  if totp.verify('u1', w1) then return 0 end "
        "  if totp.lockout_remaining('u1') ~= 0 then return 0 end "
        "  if totp.verify('u1', w2) then return 0 end "
        "  local remain = totp.lockout_remaining('u1') "
        "  if remain < 1 or remain > 61 then return 0 end "
        "  local good = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step + 2, 6) "
        "  if totp.verify('u1', good) then return 0 end "
        "  __test_hooks('hull.web.middleware.totp').clear_failed_attempts('u1') "
        "  if totp.lockout_remaining('u1') ~= 0 then return 0 end "
        "  if totp.verify('u1', w0) then return 0 end "
        "  if totp.verify('u1', w1) then return 0 end "
        "  local good2 = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step + 3, 6) "
        "  if not totp.verify('u1', good2) then return 0 end "
        "  if totp.verify('u1', w0) then return 0 end "
        "  if totp.verify('u1', w1) then return 0 end "
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
        "  __test_hooks('hull.web.middleware.totp').reset() "
        "  totp.init({ issuer = 'TestApp', recovery_codes = 3 }) "
        "  local r = totp.enroll('user-2') "
        "  local secret = __test_hooks('hull.web.middleware.totp').base32_decode(r.secret_base32) "
        "  local code = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, "
        "    __test_hooks('hull.web.middleware.totp').current_step(), 6) "
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
        "  __test_hooks('hull.web.middleware.totp').reset() "
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
        "  local t = __test_hooks('hull.web.middleware.totp') "
        "  if not t.ct_eq('287082', '287082') then return 0 end "
        "  if t.ct_eq('287082', '287083') then return 0 end "
        "  if t.ct_eq('287082', '2870820') then return 0 end "
        "  if t.ct_eq('', '') then return 1 end "
        "  return 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    int safe = eval_int(
        "(function() "
        "  local t = __test_hooks('hull.web.middleware.totp') "
        "  if t.ct_eq(nil, 'x') then return 0 end "
        "  if t.ct_eq('x', nil) then return 0 end "
        "  if t.ct_eq(42, 42) then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(safe, 1);

    /* normalize_recovery_code: strip non-alphanumerics, uppercase. */
    char *n1 = eval_str(
        "__test_hooks('hull.web.middleware.totp')"
        ".normalize_recovery_code('ABCD-EFGH-IJKL')");
    ASSERT_NE(n1, NULL); ASSERT_STREQ(n1, "ABCDEFGHIJKL"); free(n1);

    char *n2 = eval_str(
        "__test_hooks('hull.web.middleware.totp')"
        ".normalize_recovery_code('abcdefghijkl')");
    ASSERT_NE(n2, NULL); ASSERT_STREQ(n2, "ABCDEFGHIJKL"); free(n2);

    char *n3 = eval_str(
        "__test_hooks('hull.web.middleware.totp')"
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
        "  __test_hooks('hull.web.middleware.totp').reset() "
        "  totp.init({ issuer = 'TestApp', recovery_codes = 4 }) "
        "  local r = totp.enroll('rec-user') "
        "  local secret = __test_hooks('hull.web.middleware.totp').base32_decode(r.secret_base32) "
        "  local code = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, "
        "    __test_hooks('hull.web.middleware.totp').current_step(), 6) "
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
        "  __test_hooks('hull.web.middleware.totp').reset() "
        "  totp.init({ issuer = 'TestApp', "
        "              encryption_key = ('k'):rep(32) }) "
        "  local r = totp.enroll('user-4') "
        "  local secret = __test_hooks('hull.web.middleware.totp').base32_decode(r.secret_base32) "
        "  local step = __test_hooks('hull.web.middleware.totp').current_step() "
        "  local code = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step, 6) "
        "  if not totp.confirm('user-4', code) then return 0 end "
        "  local next_code = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step + 1, 6) "
        "  if not totp.verify('user-4', next_code) then return 0 end "
        "  local blob, enc_flag, version = __test_hooks('hull.web.middleware.totp').encrypt_secret(secret) "
        "  if enc_flag ~= 1 or version ~= 1 then return 0 end "
        "  if #blob <= #secret then return 0 end "
        "  local pt, v = __test_hooks('hull.web.middleware.totp').decrypt_secret(blob, 1) "
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
        "  __test_hooks('hull.web.middleware.totp').reset() "
        "  local k1 = ('a'):rep(32) "
        "  local k2 = ('b'):rep(32) "
        "  totp.init({ encryption_keys = {[1]=k1}, current = 1 }) "
        "  local r = totp.enroll('u') "
        "  local secret = __test_hooks('hull.web.middleware.totp').base32_decode(r.secret_base32) "
        "  local step = __test_hooks('hull.web.middleware.totp').current_step() "
        "  if not totp.confirm('u', __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step, 6)) "
        "    then return 2 end "
        "  local b1 = __test_hooks('hull.web.middleware.totp').get_blob('u') "
        "  if not b1 then return 50 end "
        "  local v1 = (string.byte(b1,1) << 24) | (string.byte(b1,2) << 16) "
        "          | (string.byte(b1,3) << 8) | string.byte(b1,4) "
        "  if v1 ~= 1 then return 60 + v1 end "
        "  totp.init({ encryption_keys = {[1]=k1, [2]=k2}, current = 2 }) "
        "  if not totp.verify('u', "
        "       __test_hooks('hull.web.middleware.totp').totp_at_step(secret, step + 1, 6)) then return 3 end "
        "  local b2 = __test_hooks('hull.web.middleware.totp').get_blob('u') "
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
void hl_lua_budget_arm(lua_State *thread, HlLuaBudget *b, int64_t limit,
                       uint32_t run_ms);

UTEST(db_audit9, lua_a_runaway_query_hits_the_instruction_limit)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    hl_lua_budget_arm(lua_rt.L, &lua_rt.budget, 1000000, 0);
    int rc = luaL_dostring(lua_rt.L,
        "local ok, e = pcall(db.query, 'WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) AS n FROM c') "
        "return 'caught: ' .. tostring(e)");
    EXPECT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    EXPECT_NE_MSG(strstr(err ? err : "", "instruction limit"), NULL, err ? err : "");
    lua_settop(lua_rt.L, 0);
    /* The next run is armed afresh, and its queries run. */
    hl_lua_budget_arm(lua_rt.L, &lua_rt.budget, 1000000, 0);
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
    hl_lua_budget_arm(lua_rt.L, &lua_rt.budget, 2000000, 0);
    int rc = luaL_dostring(lua_rt.L,
        "local ok, e = pcall(function() for i = 1, 5000 do "
        "  db.query('SELECT length(randomblob(200000)) AS n') end end) "
        "return 'done: ' .. tostring(e)");
    EXPECT_NE(rc, LUA_OK);
    const char *err = lua_tostring(lua_rt.L, -1);
    EXPECT_NE_MSG(strstr(err ? err : "", "instruction limit"), NULL, err ? err : "");
    lua_settop(lua_rt.L, 0);
    hl_lua_budget_arm(lua_rt.L, &lua_rt.budget, 2000000, 0);
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
        "  __test_hooks('hull.web.middleware.totp').reset() "
        "  local k1 = ('a'):rep(32) "
        "  local k2 = ('b'):rep(32) "
        "  totp.init({ encryption_keys = {[1]=k1}, current = 1 }) "
        "  for i = 1, 3 do "
        "    local r = totp.enroll('u' .. i) "
        "    local secret = __test_hooks('hull.web.middleware.totp').base32_decode(r.secret_base32) "
        "    local code = __test_hooks('hull.web.middleware.totp').totp_at_step(secret, "
        "      __test_hooks('hull.web.middleware.totp').current_step(), 6) "
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
        "  __test_hooks('hull.web.middleware.totp').reset() "
        "  local k1 = ('a'):rep(32) "
        "  totp.init({ encryption_keys = {[1]=k1}, current = 1, "
        "              legacy_key_version = 1 }) "
        "  local secret = string.rep('S', 20) "
        "  local nonce = crypto.random(24) "
        "  local blob = nonce .. crypto.secretbox(secret, nonce, k1) "
        "  local pt, version = __test_hooks('hull.web.middleware.totp').decrypt_secret(blob, 1) "
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
        "  __test_hooks('hull.web.middleware.totp').reset() "
        "  local k1 = ('a'):rep(32) "
        "  totp.init({ encryption_keys = {[1]=k1}, current = 1 }) "
        "  local blob = string.char(0,0,0,99) .. string.rep('x', 24+36) "
        "  local pt = __test_hooks('hull.web.middleware.totp').decrypt_secret(blob, 1) "
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
"__test_hooks('hull.web.auth-flows').reset() " \
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

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
        "  local o = __test_hooks('hull.web.auth-flows').origin_for "
        "  local function req(h) return { headers = h } end "
        "  if o(req{host='app.example.com:8443'}) ~= 'http://app.example.com:8443' then return 1 end "
        "  if o(req{host='app.example.com:@evil.com'}) ~= nil then return 2 end "
        "  if o(req{host='evil.com/x'}) ~= nil then return 3 end "
        "  if o(req{host='app.example.com', ['x-forwarded-host']='evil.com'}) ~= 'http://app.example.com' then return 4 end "
        "  local st = __test_hooks('hull.web.auth-flows').state "
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
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
        "  local g = __test_hooks('hull.web.auth-flows').same_origin_request "
        "  local st = __test_hooks('hull.web.auth-flows').state "
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
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(lua_stdlib, auth_flows_token_round_trip)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  local A = __test_hooks('hull.web.auth-flows').ACTIONS "
        "  local tok = __test_hooks('hull.web.auth-flows').issue_token('u1', A.verify_email, 60) "
        "  local env, err = __test_hooks('hull.web.auth-flows').consume_token(tok, A.verify_email) "
        "  if not env or env.sub ~= 'u1' then return 0 end "
        "  local env2, err2 = __test_hooks('hull.web.auth-flows').consume_token(tok, A.verify_email) "
        "  if env2 or err2 ~= 'replayed' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    int rejections = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  local A = __test_hooks('hull.web.auth-flows').ACTIONS "
        "  local tok = __test_hooks('hull.web.auth-flows').issue_token('u1', A.verify_email, 60) "
        "  local _, err = __test_hooks('hull.web.auth-flows').consume_token(tok, A.password_reset) "
        "  if err ~= 'wrong action' then return 0 end "
        "  local tampered = tok:sub(1, -3) .. 'zz' "
        "  local _, err2 = __test_hooks('hull.web.auth-flows').consume_token(tampered, A.verify_email) "
        "  if err2 ~= 'bad tag' then return 0 end "
        "  local _, err3 = __test_hooks('hull.web.auth-flows').consume_token('garbage', A.verify_email) "
        "  if err3 ~= 'malformed' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(rejections, 1);

    cleanup_lua_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
        "  local A = __test_hooks('hull.web.auth-flows').ACTIONS "
        "  af.send_verify_email("
        "    { id='u1', email='a@x.com' }, 'http://t.io') "
        "  if #_G._sent ~= 1 then return 0 end "
        "  local link = _G._sent[1].text "
        "  local tok = link:match('token=(.+)') "
        "  if not tok then return 0 end "
        "  local env, err = __test_hooks('hull.web.auth-flows').consume_token(tok, A.verify_email) "
        "  if not env or env.sub ~= 'u1' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(lua_stdlib, auth_flows_input_validation)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    int ok = eval_int(
        "(function() "
        "  local af = require('hull.web.auth-flows') "
        "  __test_hooks('hull.web.auth-flows').reset() "
        "  local t = __test_hooks('hull.web.auth-flows') "
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
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
        "  __test_hooks('hull.web.auth-flows').reset() "
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
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
        "  __test_hooks('hull.web.auth-flows').reset() "
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
        "  local A = __test_hooks('hull.web.auth-flows').ACTIONS "
        "  local tok = __test_hooks('hull.web.auth-flows').issue_token('u1', A.verify_email, 60) "
        "  local env, err = __test_hooks('hull.web.auth-flows').parse_token(tok, A.verify_email) "
        "  if not env or err then return 0 end "
        "  if env.sub ~= 'u1' then return 0 end "
        "  return 1 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
        "  __test_hooks('hull.web.auth-flows').reset() "
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
        "  local a1 = __test_hooks('hull.web.auth-flows').email_rate_allow('victim@x.com') "
        "  local a2 = __test_hooks('hull.web.auth-flows').email_rate_allow('victim@x.com') "
        "  local a3 = __test_hooks('hull.web.auth-flows').email_rate_allow('victim@x.com') "
        "  local b1 = __test_hooks('hull.web.auth-flows').email_rate_allow('other@x.com') "
        "  local c1 = __test_hooks('hull.web.auth-flows').email_rate_allow('VICTIM@x.com') "
        "  __test_hooks('hull.web.auth-flows').email_rate_reset() "
        "  local d1 = __test_hooks('hull.web.auth-flows').email_rate_allow('victim@x.com') "
        "  return (a1 and a2 and not a3 and b1 and not c1 and d1) "
        "    and 1 or 0 "
        "end)()");
    ASSERT_EQ(ok, 1);

    cleanup_lua_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(lua_stdlib, auth_flows_email_rate_limit_drops_send)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    /* Integration check via send_magic_link + auto_signup. */
    int ok = eval_int(
        "(function() " AF_INIT_LUA
        "  local af = require('hull.web.auth-flows') "
        "  __test_hooks('hull.web.auth-flows').reset() "
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
#endif

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

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
#endif

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

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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

/* Stdlib regressions from audit 15: jobs.init upgrades an old schema (its
 * ALTERs went through pcall(db.exec, ...), refused by the _hull_* check),
 * ttl options are positive numbers, TOTP secrets are bound to their account,
 * and the stdlib JSON encoder's numbers round-trip (test_json.lua). */
UTEST(lua_stdlib, stdlib_audit15_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(trusted_exec(test_db, AUDIT15_OLD_JOBS_SCHEMA, NULL, NULL, NULL), SQLITE_OK);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit15.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);

    for (size_t i = 0; i < sizeof AUDIT15_NEW_COLUMNS / sizeof AUDIT15_NEW_COLUMNS[0]; i++) {
        sqlite3_stmt *st = NULL;
        EXPECT_EQ(trusted_prepare(test_db, AUDIT15_NEW_COLUMNS[i], -1, &st, NULL), SQLITE_OK);
        sqlite3_finalize(st);
    }

    cleanup_lua_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 16: no module exports its test hooks to app
 * code (hull._test_hooks is stdlib-internal), totp's allow_unbound_secrets
 * and rekey_status().unbound, and an integral float user id. */
UTEST(lua_stdlib, stdlib_audit16_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit16.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}
#endif

/* Stdlib regressions from audit 22: totp's allow_plaintext_secrets and
 * rekey_status().plaintext. */
UTEST(lua_stdlib, stdlib_audit22_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit22.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 23: template.compile's (data) -> string
 * wrapper, config's finite numbers, the idempotency principal, the jobs.dead
 * and attachment.sweep limits. */
UTEST(lua_stdlib, stdlib_audit23_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit23.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 24: csv nil cells, clamped LIMITs, strict
 * status lists, session device refs, the outbox delivery timeout, jwt / totp
 * / validate number checks, trusted-host ports, the email-change user id. */
UTEST(lua_stdlib, stdlib_audit24_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit24.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 25: jobs / outbox schedule values are
 * finite whole seconds, outbox max_attempts a whole number. */
UTEST(lua_stdlib, stdlib_audit25_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit25.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}
#endif

/* Stdlib regressions from audit 26: safe_url raw output, Lua template dot
 * paths (rawget, no functions), search snippet / highlight escaping. */
UTEST(lua_stdlib, stdlib_audit26_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit26.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib option validation from audit 26: audit_log retain_days,
 * attachment max_size, search limit / offset, jobs max_attempts / cleanup
 * before, idempotency / inbox ttl bounds, email C1 controls, htmx form. */
UTEST(lua_stdlib, stdlib_options_audit26_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_options_audit26.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}
#endif

/* TOTP regressions from audit 26: a stripped (v1-shaped) bound blob is not
 * another account's secret, unbound / legacy secrets are 20 bytes, a success
 * keeps the per-IP count, max_lockout_duration / pending_ttl are numbers. */
UTEST(lua_stdlib, totp_audit26_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_totp_audit26.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 27: search paging clamps, jobs time options
 * as whole seconds, count options read with tonumber, attachment
 * max_size = false, addresses refuse invalid UTF-8. */
UTEST(lua_stdlib, stdlib_audit27_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit27.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}
#endif

/* Stdlib regressions from audit 28: safe_url encodes a backslash. */
UTEST(lua_stdlib, stdlib_audit28_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit28.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

/* Stdlib regressions from audit 29: jobs keys stored as strings and capped
 * at the column, csrf 413 / empty header, one Bearer rule. */
UTEST(lua_stdlib, stdlib_audit29_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit29.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* auth-flows needs both HTTP halves. */
/* Stdlib regressions from audit 30: the login lockout and TOTP attempt
 * counters incremented by the database (semantics unchanged), and
 * rbac.has_any_role / has_any_permission taking a string as one name. */
UTEST(lua_stdlib, stdlib_audit30_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit30.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

/* Stdlib regressions from audit 31: lockout counters decide their INSERT
 * with a non-locking read (MySQL gap-lock deadlock), bump the account row
 * before the per-IP one, a TOTP per-IP failure keeps a running lock, and
 * /login counts an unknown address under its own key. */
UTEST(lua_stdlib, stdlib_audit31_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit31.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

/* Stdlib regressions from audit 39: retry.run re-raises at level 0 (a
 * refusal keeps its "[hull:<code>] " prefix) and never retries a refusal. */
UTEST(lua_stdlib, stdlib_audit39_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit39.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}
#endif

/* TOTP regressions from audit 28: a success takes only the user's own
 * failures off the per-IP count; other accounts' failures still lock. */
UTEST(lua_stdlib, totp_audit28_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_totp_audit28.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

/* The csrf middleware's own suite (generate / verify, and audit 22's
 * mixed-case header_name). It sat under web/middleware/tests unrun. */
UTEST(lua_stdlib, csrf_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_csrf.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

/* The other middleware suites under web/middleware/tests, which no
 * harness ran either. */
UTEST(lua_stdlib, middleware_auth_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_auth.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, middleware_cors_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_cors.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, middleware_csp_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_csp.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, middleware_idempotency_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_idempotency.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, middleware_inbox_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_inbox.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, middleware_logger_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_logger.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
UTEST(lua_stdlib, middleware_outbox_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_outbox.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}
#endif

UTEST(lua_stdlib, middleware_ratelimit_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_ratelimit.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, middleware_rbac_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_rbac.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, middleware_session_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_session.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

UTEST(lua_stdlib, middleware_transaction_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/web/middleware/tests/test_transaction.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Stdlib regressions from audit 17: etag helpers with an empty body, and
 * jobs' live config moved to hull._test_hooks. */
UTEST(lua_stdlib, stdlib_audit17_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);

    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_stdlib_audit17.lua", &pass, &fail);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_GT(pass, 0LL);
    cleanup_lua_caps();
}
#endif

/* hull.jobs through its clock-injectable test hooks (audit 17: jobs._tick /
 * _cron_next / _events_drain left the module for hull._test_hooks, so the
 * deterministic cron and subscription-drain cases moved here from
 * tests/e2e_jobs.sh). Each case runs the script in a fresh state and a fresh
 * :memory: database, named in the global __jobs_case. */
static int run_jobs_hooks_case(const char *name, long long *pass, long long *fail)
{
    init_lua_with_caps();
    if (!lua_initialized) return -1;
    lua_pushstring(lua_rt.L, name);
    lua_setglobal(lua_rt.L, "__jobs_case");
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_jobs_hooks.lua", pass, fail);
    cleanup_lua_caps();
    return rc;
}

#define JOBS_HOOKS_CASE(c)                                         UTEST(lua_stdlib, jobs_hooks_##c)                              {                                                                  long long pass = 0, fail = -1;                                 ASSERT_EQ(run_jobs_hooks_case(#c, &pass, &fail), 0);           EXPECT_EQ(fail, 0LL);                                          EXPECT_GT(pass, 1LL);                                      }
JOBS_HOOKS_CASE(cron)
JOBS_HOOKS_CASE(events)
JOBS_HOOKS_CASE(poison)
JOBS_HOOKS_CASE(horizon)
JOBS_HOOKS_CASE(audit20)
JOBS_HOOKS_CASE(audit22)
JOBS_HOOKS_CASE(audit23)
JOBS_HOOKS_CASE(audit30)
JOBS_HOOKS_CASE(audit38)
JOBS_HOOKS_CASE(audit39)
#undef JOBS_HOOKS_CASE

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

#ifdef HL_LINK_TLS
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
#endif

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

#ifdef HL_LINK_TLS
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
#endif

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

#ifdef HL_LINK_TLS
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
#endif

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

#ifdef HL_LINK_TLS
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
#endif

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
    ASSERT_EQ(trusted_exec(test_db,
        "UPDATE _hull_jobs SET claimed_at = CAST(strftime('%s','now') AS INTEGER) - 1 "
        "WHERE queue = 'a10vt'", NULL, NULL, NULL), SQLITE_OK);
    EXPECT_EQ(eval_int(
        "(function() local jobs = require('hull.jobs') "
        "  jobs.reap({ visibility_timeout = 1 }) "
        "  return jobs.get(A10_RID).status == 'running' and 0 or 1 end)()"), 0);

    /* Two seconds behind: held at least one full second - reaped. */
    ASSERT_EQ(trusted_exec(test_db,
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

/* ── coroutine helpers (the ssh bridge's, and the task tests') ──────── */

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

UTEST(lua_ssh_bridge, ports_must_be_lua_integers_audit39)
{
    /* lua_tointegerx converted "22e2" to 2200 - the port the grant was
     * checked against - while the stdlib wrote "host:22e2" into a relay's
     * destination header, which a relay reading it with atoi dialled as 22.
     * Strings and floats are refused now, for port, via.port and
     * timeout_ms; and a host that is not one name (a glob grant admitted
     * "evil,a.example.com") is refused before any grant or dial. */
    init_lua();
    ASSERT_EQ(ssh_declare(
        "app.manifest({ modules = { 'hull/ssh@1' },\n"
        "  ssh = { connect = { hosts = { 'spark.local', '*.example.com' },\n"
        "                      ports = { 22, 2200 }, users = { 'operator' } },\n"
        "          tunnel = { hosts = { 'r.example.com' }, ports = { 443 } } } })\n"), 0);
    const char *cases[] = {
        "return select(2, require('hull.ssh._stream').connect({ host = 'spark.local',"
        " port = '22e2', user = 'operator' }))",
        "return select(2, require('hull.ssh._stream').connect({ host = 'spark.local',"
        " port = 22.0, user = 'operator' }))",
        "return select(2, require('hull.ssh._stream').connect({ host = 'spark.local',"
        " port = 22, user = 'operator', timeout_ms = '1000' }))",
        "return select(2, require('hull.ssh._stream').connect({ host = 'spark.local',"
        " port = 22, user = 'operator', via = { host = 'r.example.com', port = '443' } }))",
        "return (select(2, require('hull.ssh._stream').connect({ host = 'evil,a.example.com',"
        " port = 22, user = 'operator', via = { host = 'r.example.com', port = 443 } })))",
    };
    const char *want[] = { "port must be", "port must be", "timeout_ms must be",
                           "via.port must be", "host must be a host name" };
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(run_as_stdlib(lua_rt.L, cases[i]), LUA_OK);
        const char *err = lua_tostring(lua_rt.L, -1);
        EXPECT_TRUE_MSG(err && strstr(err, want[i]), cases[i]);
        lua_settop(lua_rt.L, 0);
    }
    ssh_undeclare();
    cleanup_lua();
}

/* A stream the SSH stdlib has connected to a loopback listener, on a real
 * loop, with the manifest applied: what every bridge test below starts from.
 * The handle is the global H, shared by the coroutines a test starts. */
typedef struct {
    const HlAsyncBackend *be;
    int lfd, peer, port;
    int with_db;            /* the runtime has a default database (caps) */
} SshBridgeFix;

/* The listener's backlog: a test that opens many streams raises it, since a
 * connect past a full backlog is not answered. */
static int ssh_bridge_backlog = 4;

/* Returns 0, or the step that failed. */
static int ssh_bridge_open_ex(SshBridgeFix *bf, int with_db)
{
    memset(bf, 0, sizeof *bf);
    bf->with_db = with_db;
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
        listen(bf->lfd, ssh_bridge_backlog) != 0 ||
        getsockname(bf->lfd, (struct sockaddr *)&a, &alen) != 0) return 3;
    bf->port = ntohs(a.sin_port);

    if (with_db) init_lua_with_caps();
    else         init_lua();
    if (!lua_initialized) return 4;
    if (bf->be->init(&lua_rt.base.async_ctx, NULL) != 0) return 4;
    if (bf->be->pool_create(&lua_rt.base.thread_pool, lua_rt.base.async_ctx,
                            2, 16) != 0) return 5;

    char src[320];
    snprintf(src, sizeof src,
        "app.manifest({ modules = { 'hull/ssh@1'%s },\n"
        "  ssh = { connect = { hosts = { '127.0.0.1' }, ports = { %d },\n"
        "                      users = { 'operator' } } } })\n",
        with_db ? ", 'hull/db@1'" : "", bf->port);
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

static int ssh_bridge_open(SshBridgeFix *bf) { return ssh_bridge_open_ex(bf, 0); }

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
    if (bf->with_db) cleanup_lua_caps();
    else             cleanup_lua();
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

UTEST(lua_ssh_bridge, a_close_wakes_its_waiter_from_the_loop)
{
    /* Audit 18 M3. Closing a stream another coroutine is parked on used to
     * resume that coroutine right there, inside the closer's run: the
     * resume's stale-transaction guard rolled back the CLOSER's open
     * transaction, and clearing active_co left the closer unable to wait.
     * The waiter is woken from the loop now, after the closer's run. */
    SshBridgeFix bf;
    ASSERT_EQ(ssh_bridge_open_ex(&bf, 1), 0);

    lua_State *co_b, *co_c;
    ASSERT_EQ_MSG(ssh_co_start(&lua_rt,
        "local d, m, code = H:read(16)\n"
        "return code\n", &co_b), LUA_YIELD, "the reader parks");

    int st = ssh_co_start(&lua_rt,
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE sc (x INTEGER)')\n"
        "db.exec('BEGIN')\n"
        "db.exec('INSERT INTO sc VALUES (1)')\n"
        "H:close()\n"
        "assert(db.in_transaction(), 'the close ended the closer\\'s transaction')\n"
        "db.exec('COMMIT')\n"
        "hull.sleep(5)\n"
        "return db.query('SELECT COUNT(*) AS n FROM sc')[1].n\n", &co_c);
    if (st != LUA_YIELD && st != LUA_OK)
        fprintf(stderr, "closer: %s\n", lua_tostring(co_c, -1));
    EXPECT_EQ_MSG(st, LUA_YIELD, "the closer keeps its transaction and can wait");
    EXPECT_EQ_MSG(lua_status(co_b), LUA_YIELD,
                  "the reader is not resumed inside the closer's run");

    ssh_tick_until_done(bf.be, lua_rt.base.async_ctx, co_c);
    ssh_tick_until_done(bf.be, lua_rt.base.async_ctx, co_b);
    EXPECT_EQ(lua_status(co_c), LUA_OK);
    if (lua_status(co_c) == LUA_OK) EXPECT_EQ(lua_tointeger(co_c, -1), 1);
    EXPECT_EQ_MSG(lua_status(co_b), LUA_OK, "the reader is woken");
    if (lua_status(co_b) == LUA_OK) EXPECT_STREQ(lua_tostring(co_b, -1), "closed");
    ssh_bridge_close(&bf);
}

/* Audit 35: live streams are capped per process (HL_SSH_MAX_LIVE_STREAMS),
 * and a stream dropped without close() holds its slot until collected, so a
 * refused connect collects once and retries. */
UTEST(lua_ssh_bridge, live_streams_are_capped_and_dropped_ones_are_collected)
{
    SshBridgeFix bf;
    ssh_bridge_backlog = 128;
    int orc = ssh_bridge_open(&bf);
    ssh_bridge_backlog = 4;
    ASSERT_EQ(orc, 0);
    ASSERT_EQ(hl_cap_ssh_live_streams(), 1);      /* H */

    char src[512];
    snprintf(src, sizeof src,
        "local s = require('hull.ssh._stream')\n"
        "T = {}\n"
        "for i = 1, %d do\n"
        "  local h, e, code = s.connect{ host = '127.0.0.1', port = %d,\n"
        "                                user = 'operator' }\n"
        "  if not h then return i, e, code end\n"
        "  T[i] = h\n"
        "end\n"
        "return 0\n", HL_SSH_MAX_LIVE_STREAMS + 4, bf.port);
    lua_State *co = ssh_bridge_run(&bf, src);
    for (int i = 0; i < 4 && lua_status(co) == LUA_YIELD; i++)
        ssh_tick_until_done(bf.be, lua_rt.base.async_ctx, co);
    ASSERT_EQ(lua_status(co), LUA_OK);
    /* H holds one slot, so the HL_SSH_MAX_LIVE_STREAMS-th connect here is
     * the first one past the cap - refused even after a collection, since
     * every live handle is still referenced from T. */
    EXPECT_EQ(lua_tointeger(co, -3), HL_SSH_MAX_LIVE_STREAMS);
    EXPECT_TRUE(lua_tostring(co, -2) &&
                strstr(lua_tostring(co, -2), "too many open SSH connections"));
    EXPECT_STREQ(lua_tostring(co, -1), "too_many_streams");
    EXPECT_EQ(hl_cap_ssh_live_streams(), HL_SSH_MAX_LIVE_STREAMS);

    /* Drop them all without closing: the next connect's refusal collects
     * them, and the retry succeeds. */
    snprintf(src, sizeof src,
        "T = nil\n"
        "local h, e, code = require('hull.ssh._stream').connect{\n"
        "  host = '127.0.0.1', port = %d, user = 'operator' }\n"
        "assert(h, tostring(e) .. ' ' .. tostring(code))\n"
        "h:close()\n"
        "return true\n", bf.port);
    co = ssh_bridge_run(&bf, src);
    for (int i = 0; i < 4 && lua_status(co) == LUA_YIELD; i++)
        ssh_tick_until_done(bf.be, lua_rt.base.async_ctx, co);
    if (lua_status(co) != LUA_OK)
        fprintf(stderr, "retry: %s\n", lua_tostring(co, -1));
    EXPECT_EQ(lua_status(co), LUA_OK);
    ssh_bridge_close(&bf);
    EXPECT_EQ(hl_cap_ssh_live_streams(), 0);    /* every slot given back */
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

/* The http config the next run_task_script_db installs (NULL: none). */
static HlHttpConfig *task_http_cfg;

/* run_task_script with the database capability (a registry whose "default"
 * is an in-memory SQLite connection). */
static int run_task_script_db(const char *body, char *out, size_t outsz)
{
    const HlAsyncBackend *be = hl_async_backend();
    if (!be) return 1;
    init_lua_with_caps();
    if (!lua_initialized) return 4;
    if (be->init(&lua_rt.base.async_ctx, NULL) != 0) return 2;
    lua_rt.base.http_cfg = task_http_cfg;

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
    lua_rt.base.http_cfg = NULL;
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
        /* The refusal carries its code (audit 35). */
        "check(tostring(e):find('^%[hull:wait_refused%] '), 'code: '..tostring(e))\n"
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

#ifdef HL_ENABLE_HTTP_SERVER
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
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_HTTP_SERVER
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
#endif /* HL_ENABLE_HTTP_SERVER */

/* Audit 29: at teardown (a __gc finalizer lua_close runs) hull.async ran
 * its task inline and a wait in it armed a timer and tracked its ctx after
 * the teardown sweep. Both refuse once the runtime is shutting down. */
UTEST(lua_audit29, no_task_or_wait_while_shutting_down)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);
    lua_rt.base.shutting_down = 1;

    lua_State *co;
    int st = ssh_co_start(&lua_rt,
        "local ok1, e1 = pcall(hull.async, function() hull.sleep(10) end)\n"
        "local ok2, e2 = pcall(hull.sleep, 10)\n"
        "OUT = tostring(ok1)..'|'..tostring(e1)..'|'..tostring(ok2)..'|'..tostring(e2)\n",
        &co);
    EXPECT_EQ(st, LUA_OK);
    EXPECT_TRUE(lua_rt.base.async_live == NULL);
    lua_getglobal(lua_rt.L, "OUT");
    const char *out = lua_tostring(lua_rt.L, -1);
    out = out ? out : "(none)";
    EXPECT_NE_MSG(strstr(out, "false|"), NULL, out);
    EXPECT_NE_MSG(strstr(out, "hull.async(): the runtime is shutting down"),
                  NULL, out);
    EXPECT_NE_MSG(strstr(out, "hull.sleep() cannot wait: the runtime is "
                              "shutting down"), NULL, out);
    lua_pop(lua_rt.L, 1);

    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    EXPECT_EQ(be->tick(actx, 0), 0);
    lua_rt.base.shutting_down = 0;
    cleanup_lua();
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

/* Audit 35: a coroutine the app made that yielded inside a transaction was
 * resumed by a later entry - after the stale-transaction guard had rolled
 * that transaction back - so its remaining INSERT autocommitted and its
 * COMMIT "succeeded". Only the run that yielded it may resume, wrap-call or
 * close it now. */
UTEST(lua_audit35, a_coroutine_yielded_in_a_txn_is_refused_in_a_later_run)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE ct (x INTEGER)')\n"
        "local co, w, cl\n"
        "require('hull._task').spawn(function()\n"
        "  co = coroutine.create(function()\n"
        "    db.exec('BEGIN') db.exec('INSERT INTO ct VALUES (1)')\n"
        "    coroutine.yield()\n"
        "    db.exec('INSERT INTO ct VALUES (2)') db.exec('COMMIT') end)\n"
        "  assert(coroutine.resume(co))\n"
        "  db.exec('ROLLBACK')\n"
        "  w = coroutine.wrap(function()\n"
        "    db.exec('BEGIN') coroutine.yield()\n"
        "    db.exec('INSERT INTO ct VALUES (3)') db.exec('COMMIT') end)\n"
        "  w()\n"
        "  db.exec('ROLLBACK')\n"
        "  cl = coroutine.create(function()\n"
        "    local _ <close> = setmetatable({}, { __close = function()\n"
        "      db.exec('INSERT INTO ct VALUES (4)') end })\n"
        "    db.exec('BEGIN') coroutine.yield() end)\n"
        "  assert(coroutine.resume(cl))\n"
        "end)\n"      /* the task ends inside cl's transaction: rolled back */
        "hull.sleep(20)\n"
        "check(co ~= nil and w ~= nil and cl ~= nil, 'the task did not run')\n"
        "local ok, e = pcall(coroutine.resume, co)\n"
        "check(not ok and tostring(e):find('inside a database transaction', 1, true),\n"
        "      'resume: '..tostring(e))\n"
        "ok, e = pcall(w)\n"
        "check(not ok and tostring(e):find('inside a database transaction', 1, true),\n"
        "      'wrap: '..tostring(e))\n"
        "ok, e = pcall(coroutine.close, cl)\n"
        "check(not ok and tostring(e):find('inside a database transaction', 1, true),\n"
        "      'close: '..tostring(e))\n"
        "check(coroutine.status(co) == 'suspended', 'co ran')\n"
        "check(db.query('SELECT COUNT(*) AS n FROM ct')[1].n == 0, 'a row was committed')\n"
        "check(not db.in_transaction(), 'left inside a transaction')\n"
        "db.exec('BEGIN') db.exec('COMMIT')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* Within the run that yielded it, a coroutine inside a transaction resumes as
 * before (a generator under db.batch); a coroutine that yields with no
 * transaction open resumes from any later run, including one whose earlier
 * yield was inside a transaction. */
UTEST(lua_audit35, same_run_and_txn_free_coroutines_still_resume)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE cs (x INTEGER)')\n"
        "local co = coroutine.create(function()\n"
        "  db.exec('BEGIN') db.exec('INSERT INTO cs VALUES (1)')\n"
        "  coroutine.yield('mid')\n"
        "  db.exec('INSERT INTO cs VALUES (2)') db.exec('COMMIT')\n"
        "  coroutine.yield('after')\n"
        "  return 'done' end)\n"
        "local ok, v = coroutine.resume(co)\n"
        "check(ok and v == 'mid', 'first: '..tostring(v))\n"
        "ok, v = coroutine.resume(co)\n"
        "check(ok and v == 'after', 'same run: '..tostring(v))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM cs')[1].n == 2, 'rows')\n"
        "local g = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i) end end)\n"
        "check(g() == 1, 'gen 1')\n"
        "local n = 0\n"
        "db.batch(function()\n"
        "  for x in coroutine.wrap(function() for i = 1, 3 do\n"
        "      db.exec('INSERT INTO cs VALUES (?)', {10 + i}) coroutine.yield(i) end end) do\n"
        "    n = n + x end end)\n"
        "check(n == 6, 'batch generator '..n)\n"
        "hull.sleep(5)\n"   /* a new run */
        "check(g() == 2, 'a txn-free generator resumes in a later run')\n"
        "ok, v = coroutine.resume(co)\n"
        "check(ok and v == 'done', 'later run after a txn-free yield: '..tostring(v))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM cs')[1].n == 5, 'rows after')\n"
        "local dead = coroutine.create(function() db.exec('BEGIN') coroutine.yield()\n"
        "  db.exec('ROLLBACK') end)\n"
        "coroutine.resume(dead) coroutine.resume(dead)\n"
        "hull.sleep(5)\n"
        "ok, v = coroutine.resume(dead)\n"
        "check(not ok and tostring(v):find('dead', 1, true), 'dead: '..tostring(v))\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* Audit 36: the audit-35 check compared the yielding run's id, which only
 * changes at the next arm - so app code that runs after an entry's closing
 * stale-transaction guard and before the next entry (an error object's
 * __tostring read by hl_lua_error_text, a __gc finalizer) still matched it,
 * resumed the coroutine into the rolled-back transaction, and its next INSERT
 * autocommitted. The check is now against the transaction itself. */
UTEST(lua_audit36, a_coroutine_is_refused_once_its_txn_was_rolled_back)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE tg (x INTEGER)')\n"
        "CO = coroutine.create(function()\n"
        "  db.exec('BEGIN') db.exec('INSERT INTO tg VALUES (1)')\n"
        "  coroutine.yield()\n"
        "  db.exec('INSERT INTO tg VALUES (2)') end)\n"
        "assert(coroutine.resume(CO))\n"
        "ERR = setmetatable({}, { __tostring = function()\n"
        "  local ok, e = pcall(coroutine.resume, CO)\n"
        "  RESUMED, RESUME_ERR = ok, tostring(e)\n"
        "  return 'boom' end })\n"), LUA_OK);
    /* The entry's closing guard, then the error path's message - with no
     * new run armed in between. */
    hl_db_registry_guard_stale_txns(lua_rt.base.db_registry);
    char buf[256];
    lua_getglobal(L, "ERR");
    EXPECT_STREQ(hl_lua_error_text(&lua_rt, L, -1, buf, sizeof buf), "boom");
    lua_settop(L, 0);
    EXPECT_EQ(eval_int("RESUMED == false and 1 or 0"), 1);
    EXPECT_EQ(eval_int("RESUME_ERR:find('since ended', 1, true) and 1 or 0"), 1);
    /* A finalizer is refused the same way. */
    ASSERT_EQ(luaL_dostring(L,
        "setmetatable({}, { __gc = function()\n"
        "  local ok, e = pcall(coroutine.resume, CO)\n"
        "  GC_RESUMED, GC_ERR = ok, tostring(e) end })\n"), LUA_OK);
    lua_gc(L, LUA_GCCOLLECT);
    EXPECT_EQ(eval_int("GC_RESUMED == false and 1 or 0"), 1);
    EXPECT_EQ(eval_int("GC_ERR:find('since ended', 1, true) and 1 or 0"), 1);
    EXPECT_EQ(eval_int("coroutine.status(CO) == 'suspended' and 1 or 0"), 1);
    EXPECT_EQ(eval_int("require('hull.db').default()"
                       ".query('SELECT COUNT(*) AS n FROM tg')[1].n"), 0);
    EXPECT_EQ(eval_int("require('hull.db').default().in_transaction()"
                       " and 1 or 0"), 0);
    cleanup_lua_caps();
}

/* Audit 36: #927 tagged a coroutine on every yield inside a transaction,
 * including the RESUMER's (a db.batch around `v = gen()`), so a generator or
 * work queue kept across requests was refused for good from the second
 * request on, though every batch had committed. Only a transaction the
 * coroutine opened itself tags it now. */
UTEST(lua_audit36, a_generator_consumed_inside_each_runs_batch_keeps_working)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE gb (x INTEGER)')\n"
        "local gen = coroutine.wrap(function() local i = 0\n"
        "  while true do i = i + 1\n"
        "    db.exec('INSERT INTO gb VALUES (?)', {i}) coroutine.yield(i) end end)\n"
        "local co = coroutine.create(function() local i = 0\n"
        "  while true do i = i + 1 coroutine.yield(i) end end)\n"
        "for r = 1, 3 do\n"
        "  local v, ok, w\n"
        "  db.batch(function() v = gen() ok, w = coroutine.resume(co) end)\n"
        "  check(v == r and ok and w == r, 'run '..r..': '..tostring(v)..' '..tostring(w))\n"
        "  hull.sleep(5)\n"   /* a new run */
        "end\n"
        "db.exec('BEGIN') local v = gen() db.exec('COMMIT')\n"
        "check(v == 4, 'raw txn: '..tostring(v))\n"
        "hull.sleep(5)\n"
        "check(gen() == 5, 'after the raw txn, in a later run')\n"
        "check(db.query('SELECT COUNT(*) AS n FROM gb')[1].n == 5, 'rows')\n"
        /* A coroutine resumed inside the resumer's transaction that ends it
         * and opens one of its own is tagged by that one. */
        "local own = coroutine.create(function()\n"
        "  db.exec('COMMIT') db.exec('BEGIN') db.exec('INSERT INTO gb VALUES (99)')\n"
        "  coroutine.yield() db.exec('COMMIT') end)\n"
        "require('hull._task').spawn(function()\n"
        "  db.exec('BEGIN') assert(coroutine.resume(own)) end)\n"
        "hull.sleep(20)\n"   /* the task ended: its guard rolled 99 back */
        "local ok, e = pcall(coroutine.resume, own)\n"
        "check(not ok and tostring(e):find('since ended', 1, true), 'own: '..tostring(e))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM gb WHERE x = 99')[1].n == 0, '99 committed')\n"
        "check(not db.in_transaction(), 'left inside a transaction')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* Audit 37: round 36 rewrote a still-matching "yielded in its own
 * transaction" tag to "resumed inside a transaction" on every resume, and the
 * next yield then took the transaction for the resumer's and cleared the tag.
 * A coroutine that did BEGIN, yielded, was resumed and yielded again lost its
 * tag; once the guard rolled the transaction back, a later run resumed it and
 * its later statements autocommitted. */
UTEST(lua_audit37, a_coroutine_yielding_twice_in_its_txn_is_refused_later)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE dy (x INTEGER)')\n"
        "local own = coroutine.create(function()\n"
        "  db.exec('BEGIN') db.exec('INSERT INTO dy VALUES (1)')\n"
        "  coroutine.yield() coroutine.yield()\n"
        "  db.exec('INSERT INTO dy VALUES (2)') end)\n"
        "require('hull._task').spawn(function()\n"
        "  assert(coroutine.resume(own)) assert(coroutine.resume(own)) end)\n"
        "hull.sleep(20)\n"
        "check(not db.in_transaction(), 'left inside a transaction')\n"
        "local ok, e = pcall(coroutine.resume, own)\n"
        "check(not ok and tostring(e):find('since ended', 1, true), 'resume: '..tostring(e))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM dy')[1].n == 0, 'rows')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* Any number of yields inside the coroutine's own transaction keeps the tag,
 * a coroutine nested inside it does not disturb it, and inside the run the
 * coroutine still resumes and commits normally. */
UTEST(lua_audit37, a_coroutine_keeps_its_tag_across_many_yields)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE my (x INTEGER)')\n"
        "local function worker(n, commit)\n"
        "  return coroutine.create(function()\n"
        "    db.exec('BEGIN')\n"
        "    for i = 1, n do\n"
        "      db.exec('INSERT INTO my VALUES (?)', {i})\n"
        "      local inner = coroutine.create(function() coroutine.yield() end)\n"
        "      assert(coroutine.resume(inner))\n"
        "      coroutine.yield(i)\n"
        "    end\n"
        "    if commit then db.exec('COMMIT') end end)\n"
        "end\n"
        "local done = worker(4, true)\n"
        "local left = worker(4, false)\n"
        "require('hull._task').spawn(function()\n"
        "  for _ = 1, 5 do assert(coroutine.resume(done)) end\n"
        "  for _ = 1, 4 do assert(coroutine.resume(left)) end end)\n"
        "hull.sleep(20)\n"
        "check(coroutine.status(done) == 'dead', 'the committing worker finished')\n"
        "check(db.query('SELECT COUNT(*) AS n FROM my')[1].n == 4, 'its 4 rows')\n"
        "check(not db.in_transaction(), 'left inside a transaction')\n"
        "local ok, e = pcall(coroutine.resume, left)\n"
        "check(not ok and tostring(e):find('since ended', 1, true), 'resume: '..tostring(e))\n"
        "ok, e = pcall(coroutine.close, left)\n"
        "check(not ok and tostring(e):find('since ended', 1, true), 'close: '..tostring(e))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM my')[1].n == 4, 'no autocommitted row')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* coroutine.wrap goes through the same check. */
UTEST(lua_audit37, a_wrapped_coroutine_yielding_twice_is_refused_later)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE wy (x INTEGER)')\n"
        "local f = coroutine.wrap(function()\n"
        "  db.exec('BEGIN') db.exec('INSERT INTO wy VALUES (1)')\n"
        "  coroutine.yield() coroutine.yield() coroutine.yield()\n"
        "  db.exec('INSERT INTO wy VALUES (2)') end)\n"
        "require('hull._task').spawn(function() f() f() f() end)\n"
        "hull.sleep(20)\n"
        "local ok, e = pcall(f)\n"
        "check(not ok and tostring(e):find('since ended', 1, true), 'wrap: '..tostring(e))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM wy')[1].n == 0, 'rows')\n"
        "check(not db.in_transaction(), 'left inside a transaction')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* A generator consumed inside each run's db.batch that runs a multi-statement
 * text or a refused transaction statement keeps working: SQLite reports
 * transaction ends (txn_seq), so the generation is not changed by the text
 * alone (audit 37; it was bumped for any text that could end one). */
UTEST(lua_audit37, a_generator_running_multi_statement_sql_in_a_batch_keeps_working)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE ms (x INTEGER)')\n"
        "local gen = coroutine.wrap(function() local i = 0\n"
        "  while true do i = i + 1\n"
        "    db.exec('INSERT INTO ms VALUES (1); INSERT INTO ms VALUES (0)')\n"
        "    pcall(db.exec, 'BEGIN')\n"   /* refused inside the batch */
        "    coroutine.yield(i) end end)\n"
        "for r = 1, 3 do\n"
        "  local v\n"
        "  db.batch(function() v = gen() end)\n"
        "  check(v == r, 'run '..r..': '..tostring(v))\n"
        "  hull.sleep(5)\n"
        "end\n"
        "check(db.query('SELECT COUNT(*) AS n FROM ms')[1].n == 3, 'rows '..db.query('SELECT COUNT(*) AS n FROM ms')[1].n)\n"
        "check(not db.in_transaction(), 'left inside a transaction')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* Audit 38: coroutine.resume, like lcorolib, left a coroutine that raised
 * with its to-be-closed variables pending until coroutine.close. A later
 * run's close() then ran the coroutine's own __close handlers - written to
 * run inside its transaction - after the stale-transaction guard had rolled
 * that transaction back: the credit committed alone in autocommit. Now the
 * coroutine is closed in the run that raised: its handler runs inside the
 * transaction, the guard rolls both writes back, and close() runs nothing. */
UTEST(lua_audit38, a_coroutine_that_raised_runs_its_close_handlers_in_its_run)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE lg (x TEXT)')\n"
        "local held, ok1, e1, ran\n"
        "require('hull._task').spawn(function()\n"
        "  held = coroutine.create(function()\n"
        "    db.exec('BEGIN') db.exec(\"INSERT INTO lg VALUES ('debit')\")\n"
        "    local g <close> = setmetatable({}, {__close = function()\n"
        "      ran = db.in_transaction()\n"
        "      db.exec(\"INSERT INTO lg VALUES ('credit')\") end})\n"
        "    error('boom') end)\n"
        "  ok1, e1 = coroutine.resume(held) end)\n"
        "hull.sleep(20)\n"
        "check(ok1 == false and tostring(e1):find('boom', 1, true), 'resume: '..tostring(e1))\n"
        "check(ran == true, 'the handler ran inside the transaction: '..tostring(ran))\n"
        "check(coroutine.status(held) == 'dead', 'status '..coroutine.status(held))\n"
        "check(not db.in_transaction(), 'left inside a transaction')\n"
        "check(db.query('SELECT COUNT(*) AS n FROM lg')[1].n == 0, 'rows after the run')\n"
        "local ok, r = pcall(coroutine.close, held)\n"
        "check(ok and r == true, 'close: '..tostring(ok)..' '..tostring(r))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM lg')[1].n == 0, 'close committed a row')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* The same when the coroutine first yielded tagged inside its own
 * transaction and a second resume in the same run made it raise. */
UTEST(lua_audit38, a_tagged_coroutine_that_raised_is_closed_in_its_run)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE lt (x TEXT)')\n"
        "local held, ok1, e1\n"
        "require('hull._task').spawn(function()\n"
        "  held = coroutine.create(function()\n"
        "    db.exec('BEGIN') db.exec(\"INSERT INTO lt VALUES ('debit')\")\n"
        "    local g <close> = setmetatable({}, {__close = function()\n"
        "      db.exec(\"INSERT INTO lt VALUES ('credit')\") end})\n"
        "    if coroutine.yield() then error('boom') end end)\n"
        "  assert(coroutine.resume(held))\n"
        "  ok1, e1 = coroutine.resume(held, true) end)\n"
        "hull.sleep(20)\n"
        "check(ok1 == false and tostring(e1):find('boom', 1, true), 'resume: '..tostring(e1))\n"
        "check(not db.in_transaction(), 'left inside a transaction')\n"
        "check(db.query('SELECT COUNT(*) AS n FROM lt')[1].n == 0, 'rows after the run')\n"
        "local ok, r = pcall(coroutine.close, held)\n"
        "check(ok and r == true, 'close: '..tostring(ok)..' '..tostring(r))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM lt')[1].n == 0, 'close committed a row')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* A runtime-run coroutine (here a hull._task task) that raised is closed
 * too, before the guard: its handler runs inside its transaction, and a
 * reference the app kept (coroutine.running()) can neither close nor resume
 * it afterwards; it reads dead. */
UTEST(lua_audit38, a_task_that_raised_is_closed_before_the_guard)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local db = require('hull.db').default()\n"
        "db.exec('CREATE TABLE lk (x TEXT)')\n"
        "local held, ran\n"
        "require('hull._task').spawn(function()\n"
        "  held = coroutine.running()\n"
        "  db.exec('BEGIN') db.exec(\"INSERT INTO lk VALUES ('debit')\")\n"
        "  local g <close> = setmetatable({}, {__close = function()\n"
        "    ran = db.in_transaction()\n"
        "    db.exec(\"INSERT INTO lk VALUES ('credit')\") end})\n"
        "  error('boom') end)\n"
        "hull.sleep(20)\n"
        "check(ran == true, 'the handler ran inside the transaction: '..tostring(ran))\n"
        "check(coroutine.status(held) == 'dead', 'status '..coroutine.status(held))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM lk')[1].n == 0, 'rows after the run')\n"
        "local ok, e = pcall(coroutine.close, held)\n"
        "check(not ok and tostring(e):find('ended by an error', 1, true), 'close: '..tostring(e))\n"
        "ok, e = pcall(coroutine.resume, held)\n"
        "check(not ok and tostring(e):find('ended by an error', 1, true), 'resume: '..tostring(e))\n"
        "check(db.query('SELECT COUNT(*) AS n FROM lk')[1].n == 0, 'a row committed')\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}

/* Ordinary errors keep coroutine.resume / close / wrap's results: (false,
 * err) with the original error object, the __close handler already run, a
 * handler's own error replacing the original (as coroutine.close reports
 * it), close() of the dead coroutine true, and a dead coroutine refusing a
 * resume as before. */
UTEST(lua_audit38, resume_and_close_results_are_unchanged)
{
    char out[512];
    ASSERT_EQ(run_task_script_db(
        "local closed = 0\n"
        "local co = coroutine.create(function()\n"
        "  local g <close> = setmetatable({}, {__close = function(_, err)\n"
        "    closed = closed + 1; assert(tostring(err):find('boom', 1, true)) end})\n"
        "  error('boom') end)\n"
        "local ok, e = coroutine.resume(co)\n"
        "check(ok == false and tostring(e):find(':%d+: boom$'), 'resume: '..tostring(e))\n"
        "check(closed == 1, 'the handler ran: '..closed)\n"
        "check(coroutine.status(co) == 'dead', 'status '..coroutine.status(co))\n"
        "check(coroutine.close(co) == true, 'close of the dead coroutine')\n"
        "check(closed == 1, 'ran twice: '..closed)\n"
        "ok, e = coroutine.resume(co)\n"
        "check(ok == false and e == 'cannot resume dead coroutine', 'dead: '..tostring(e))\n"
        "local obj = {}\n"
        "co = coroutine.create(function() error(obj) end)\n"
        "ok, e = coroutine.resume(co)\n"
        "check(ok == false and e == obj, 'the error object is kept')\n"
        "co = coroutine.create(function()\n"
        "  local g <close> = setmetatable({}, {__close = function() error('inclose', 0) end})\n"
        "  error('boom') end)\n"
        "ok, e = coroutine.resume(co)\n"
        "check(ok == false and e == 'inclose', 'handler error: '..tostring(e))\n"
        "check(coroutine.status(co) == 'dead', 'status after a handler error')\n"
        "local w = coroutine.wrap(function()\n"
        "  local g <close> = setmetatable({}, {__close = function() closed = closed + 1 end})\n"
        "  error('wboom') end)\n"
        "ok, e = pcall(w)\n"
        "check(not ok and tostring(e):find('wboom', 1, true) and closed == 2, 'wrap: '..tostring(e))\n"
        "co = coroutine.create(function() coroutine.yield(1) return 2 end)\n"
        "local a, b = coroutine.resume(co)\n"
        "check(a == true and b == 1, 'yield results')\n"
        "a, b = coroutine.resume(co)\n"
        "check(a == true and b == 2, 'return results')\n",
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

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
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
        "__test_hooks('hull.web.auth-flows').reset()\n"
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
        "local ok, err = pcall(db.batch, function() __test_hooks('hull.web.auth-flows').handlers.register(req, res) end)\n"
        "check(ok, 'register under db.batch raised: '..tostring(err))\n"
        "check(res.code == 200, 'status '..tostring(res.code))\n"
        "check(#S.sent == 0 and S.by_email['txn@x.test'] == nil, 'ran inside the request')\n"
        "hull.sleep(30)\n"
        "check(S.by_email['txn@x.test'] ~= nil, 'no account after the response')\n"
        "check(#S.sent == 1 and S.sent[1] == 'txn@x.test', 'mail sent '..#S.sent)\n",
        out, sizeof out), 0);
    ASSERT_STREQ(out, "ok");
}
#endif

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

/* Audit 19, Lua / JS stdlib parity. retry.run with max_attempts 0 or NaN
 * never called fn and raised a nil error (JS: `maxAttempts || 3`). tui.run
 * left the terminal in raw mode when tui.poll / tui.flush raised: only draw
 * and on_event were under pcall (JS wraps the whole loop). run() returns 0,
 * or the number of the first check that failed. */
UTEST(lua_stdlib, audit19_stdlib_parity)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local retry = require('hull.retry') "
        "  local calls = 0 "
        "  local r = retry.run(function() calls = calls + 1; return 7 end, "
        "                      { max_attempts = 0 }) "
        "  if r ~= 7 or calls ~= 1 then return 1 end "
        "  r = retry.run(function() return 8 end, { max_attempts = 0/0 }) "
        "  if r ~= 8 then return 2 end "
        /* tui is a composed feature: absent from this build, skip its part. */
        "  local ok, tui = pcall(require, 'hull.tui') "
        "  if not ok or type(tui) ~= 'table' or type(tui.run) ~= 'function' then "
        "    return 0 "
        "  end "
        "  local left = 0 "
        "  tui.enter = function() end "
        "  tui.leave = function() left = left + 1 end "
        "  tui.size = function() return 80, 24 end "
        "  tui.flush = function() end "
        "  tui.poll = function() error('poll failed') end "
        "  local rok, err = pcall(tui.run, { draw = function() end }) "
        "  if rok or not tostring(err):find('poll failed', 1, true) then return 3 end "
        "  if left ~= 1 then return 4 end "
        "  return 0 "
        "end)()");
    EXPECT_EQ(v, 0);
    cleanup_lua_caps();
}

/* Audit 20: retry.run with max_attempts below 1 (-1, 0.5) never called fn
 * and raised nil; any non-number or value under 1 takes the default now. */
UTEST(lua_stdlib, audit20_retry_below_one)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    int v = eval_int(
        "(function() "
        "  local retry = require('hull.retry') "
        "  for _, m in ipairs({ -1, 0.5, '3' }) do "
        "    local calls = 0 "
        "    local r = retry.run(function() calls = calls + 1; return 9 end, "
        "                        { max_attempts = m }) "
        "    if r ~= 9 or calls ~= 1 then return 1 end "
        "  end "
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
        "  if pcall(inbox.check_and_mark, 'm2', 'src', { ttl = -1 }) then return 8 end "
        "  if inbox.check_and_mark('m2', 'src') ~= false then return 8 end "
        "  __test_hooks('hull.web.middleware.inbox').expire('m2', 'src') "
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

/* Audit 14 G1: a detached hull.sleep (app.main, a timer) still pending when
 * the runtime shuts down is on the teardown list; stop_async disarms its
 * timer and releases the coroutine. It used to outlive the runtime: the
 * ctx and continuation leaked, and the timer still pointed at them. */
UTEST(lua_worker, a_pending_detached_sleep_is_released_at_teardown)
{
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    lua_State *co;
    EXPECT_EQ(ssh_co_start(&lua_rt, "hull.sleep(60000) return 'woke'", &co),
              LUA_YIELD);
    lua_rt.active_co = NULL;
    lua_rt.active_thread_ref = LUA_NOREF;   /* the continuation holds it */
    EXPECT_TRUE(lua_rt.base.async_live != NULL);
    hl_lua_vtable.stop_async(&lua_rt.base);
    EXPECT_TRUE(lua_rt.base.async_live == NULL);
    EXPECT_EQ(lua_rt.base.shutting_down, 1);
    /* The timer is gone: ticking the loop resumes nothing. */
    EXPECT_EQ(f.be->tick(f.actx, 0), 0);
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

/* Audit 33: when the runtime cannot build an op's result (out of memory) it
 * fails the handler without resuming it, and frees the op - but the coroutine
 * stays suspended in the binding's lua_yieldk. Only unparked, a later
 * coroutine.resume from app code ran the binding's continuation on the freed
 * op (lua_db_async_k / lua_compute_async_k: a use-after-free) and the dead
 * handler on outside its request. It is now refused for good. */
UTEST(lua_audit33, an_abandoned_wait_can_never_be_resumed)
{
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    lua_State *L = lua_rt.L;
    lua_State *co = NULL;
    int st = ssh_co_start(&lua_rt,
        "SAVED = coroutine.running()\n"
        "local r = require('hull.worker').dispatch(function()\n"
        "  return string.rep('x', 3 * 1024 * 1024) end)\n"
        "RAN_ON = true\n"
        "return 'resumed ' .. #r\n", &co);
    ASSERT_EQ(st, LUA_YIELD);
    /* Leave about 1 MB of heap: the 3 MB result cannot be pushed. */
    lua_gc(L, LUA_GCCOLLECT);
    size_t room = lua_rt.mem_limit - lua_rt.mem_used;
    ASSERT_GT(room, (size_t)(4u << 20));
    (void)lua_newuserdatauv(L, room - (size_t)(1u << 20), 0);
    lua_setglobal(L, "BALLAST");
    /* Until the dispatch completes the coroutine is merely parked; once its
     * result push has failed it is abandoned. */
    int abandoned = 0;
    for (int i = 0; i < 250 && !abandoned; i++) {
        f.be->tick(f.actx, 20);
        ASSERT_EQ(luaL_dostring(L,
            "return select(2, pcall(coroutine.resume, SAVED))"), LUA_OK);
        const char *m = lua_tostring(L, -1);
        abandoned = m && strstr(m, "abandoned") != NULL;
        lua_pop(L, 1);
    }
    EXPECT_TRUE(abandoned);
    ASSERT_EQ(luaL_dostring(L, "BALLAST = nil collectgarbage()"), LUA_OK);
    /* Still suspended in lua_yieldk, and still refused, now and later. */
    EXPECT_EQ(lua_status(co), LUA_YIELD);
    for (int i = 0; i < 2; i++) {
        ASSERT_EQ(luaL_dostring(L,
            "local ok, e = pcall(coroutine.resume, SAVED, 'forged') "
            "local ok2, e2 = pcall(coroutine.close, SAVED) "
            "return tostring(ok) .. ':' .. tostring(e) .. '|' .. tostring(ok2)"),
            LUA_OK);
        const char *msg = lua_tostring(L, -1);
        EXPECT_TRUE_MSG(msg && strncmp(msg, "false:", 6) == 0 &&
                        strstr(msg, "abandoned") != NULL &&
                        strstr(msg, "|false") != NULL, msg ? msg : "(nil)");
        lua_pop(L, 1);
    }
    lua_getglobal(L, "RAN_ON");
    EXPECT_TRUE(lua_isnil(L, -1));   /* the dead handler never ran on */
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

/* A '.' / '..' in HULL_CACHE_DIR is refused before anything is made: through
 * a part that does not exist yet it could not be resolved, so the ~/.hull
 * checks passed and =~/.hull/nosuch/../keys/new was made inside ~/.hull/keys
 * (audit 14). */
UTEST(lua_audit14, a_dotted_cache_dir_override_is_refused_before_mkdir)
{
    char tmpdir[512];
    bc_with_tmp_home(tmpdir, sizeof tmpdir);
    ASSERT_NE(tmpdir[0], 0);
    char out[1024], ovr[1024], dir[1024], made[1024];
    snprintf(dir, sizeof dir, "%s/.hull", tmpdir);
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.hull/keys", tmpdir);
    mkdir(dir, 0700);
    snprintf(made, sizeof made, "%s/.hull/keys/new", tmpdir);
    struct stat st;

    snprintf(ovr, sizeof ovr, "%s/.hull/nosuch/../keys/new", tmpdir);
    setenv("HULL_CACHE_DIR", ovr, 1);
    EXPECT_EQ(hl_hull_cache_dir(out, sizeof out), -1);
    EXPECT_NE(stat(made, &st), 0);
    snprintf(ovr, sizeof ovr, "%s/app-cache/./x", tmpdir);
    setenv("HULL_CACHE_DIR", ovr, 1);
    EXPECT_EQ(hl_hull_cache_dir(out, sizeof out), -1);
    snprintf(ovr, sizeof ovr, "%s/app-cache/..", tmpdir);
    setenv("HULL_CACHE_DIR", ovr, 1);
    EXPECT_EQ(hl_hull_cache_dir(out, sizeof out), -1);
    /* A name that merely starts with a dot is fine. */
    snprintf(ovr, sizeof ovr, "%s/.app-cache", tmpdir);
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
    st = hl_lua_resume_status(co, L, st);
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

/* Audit 22: the tool process (hull build / sign) loaded stdlib and CLI
 * plugin bytecode sealed under the RUNTIME key, which every app process
 * holds. hl_runtime_cache_code_off turns those caches off there; the tool
 * key's caches are untouched. */
UTEST(cache_seal, a_tool_process_reads_no_runtime_key_code)
{
    char tmp[256];
    bc_with_tmp_home(tmp, sizeof tmp);
    ASSERT_NE(tmp[0], ' ');
    char root[512];
    snprintf(root, sizeof root, "%s/store", tmp);
    HlBlobStore *st = NULL;
    ASSERT_EQ(0, hl_blob_store_open(&st, NULL, root, 1, 0));

    static const char k1[] =
        "3333333333333333333333333333333333333333333333333333333333333333";
    static const char k2[] =
        "4444444444444444444444444444444444444444444444444444444444444444";
    static const uint8_t payload[] = "bytecode";
    uint8_t *out = NULL;
    size_t n = 0;

    hl_runtime_cache_put_sealed(st, "lua-bytecode", k1, payload, sizeof payload);
    hl_runtime_cache_code_off(1);
    EXPECT_EQ(-1, hl_runtime_cache_get_sealed(st, "lua-bytecode", k1, &out, &n));
    EXPECT_TRUE(out == NULL);
    hl_runtime_cache_put_sealed(st, "lua-bytecode", k2, payload, sizeof payload);
    /* the tool key's cache still works */
    hl_tool_cache_put_sealed(st, "compute-aot", k2, payload, sizeof payload);
    ASSERT_EQ(0, hl_tool_cache_get_sealed(st, "compute-aot", k2, &out, &n));
    free(out);
    out = NULL;
    hl_runtime_cache_code_off(0);
    /* k1 was left in the store (not dropped) */
    ASSERT_EQ(0, hl_runtime_cache_get_sealed(st, "lua-bytecode", k1, &out, &n));
    free(out);
    out = NULL;

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
 * so the __close handlers a later coroutine.close ran were unmetered. Since
 * audit 38 the coroutine is closed in the run that tripped: its handler runs
 * there, metered, and is stopped by the trip; a later close() finds it dead
 * and runs nothing. */
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
        "assert(coroutine.status(CO) == 'dead') "
        "assert(coroutine.close(CO) == true)", "CLOSED", &done);
    EXPECT_EQ(rc, LUA_OK);
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

#ifdef HL_ENABLE_HTTP_SERVER
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
#endif /* HL_ENABLE_HTTP_SERVER */

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

/* A program named by path runs only from an execute grant - except the very
 * file Hull's own lookup resolves for that name ($PATH here): tool.find_tool
 * hands compute_build.lua an absolute clang path, and on Windows no grant
 * covers C:/msys64/ucrt64/bin, so every wasm32 compute build was refused. A
 * same-named copy anywhere else is still refused. */
UTEST(lua_tool_vm, spawn_by_path_admits_only_the_resolved_program)
{
    char on_path[HL_TEST_PATH_MAX], elsewhere[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(on_path, sizeof on_path, "hull_tv_path") != NULL);
    ASSERT_TRUE(hl_test_mkdtemp(elsewhere, sizeof elsewhere, "hull_tv_evil") != NULL);
    char good[HL_TEST_PATH_MAX + 32], evil[HL_TEST_PATH_MAX + 32];
    snprintf(good, sizeof good, "%s/clang%s", on_path, hl_host_exe_suffix());
    snprintf(evil, sizeof evil, "%s/clang%s", elsewhere, hl_host_exe_suffix());
    ASSERT_EQ(tool_vm_write(good, "#!/bin/sh\nexit 0\n"), 0);
    ASSERT_EQ(tool_vm_write(evil, "#!/bin/sh\nexit 0\n"), 0);
    ASSERT_EQ(chmod(good, 0755), 0);
    ASSERT_EQ(chmod(evil, 0755), 0);

    const char *old = getenv("PATH");
    char *saved = old ? strdup(old) : NULL;
    setenv("PATH", on_path, 1);

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

    lua_pushstring(L, good);
    lua_setglobal(L, "GOOD");
    lua_pushstring(L, evil);
    lua_setglobal(L, "EVIL");
    const char *chunk =
        "local ok, err = pcall(tool.spawn_read, { EVIL, '--version' })\n"
        "assert(not ok and err:find('outside the tool sandbox', 1, true), tostring(err))\n"
        /* It may fail to run here; it must not be refused by the sandbox. */
        "ok, err = pcall(tool.spawn_read, { GOOD, '--version' })\n"
        "assert(ok or not tostring(err):find('outside the tool sandbox', 1, true), tostring(err))\n"
        "return true\n";
    ASSERT_EQ(luaL_loadstring(L, chunk), LUA_OK);
    int rc = lua_pcall(L, 0, 1, 0);
    if (rc != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    EXPECT_EQ(rc, LUA_OK);
    lua_pop(L, 1);

    hl_lua_free(&tv);
    hl_tool_unveil_free(&uctx);
    hl_platform_vfs_dispose(pvfs_owned);
    if (saved) { setenv("PATH", saved, 1); free(saved); } else unsetenv("PATH");
    remove(good); remove(evil);
    rmdir(on_path); rmdir(elsewhere);
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

#ifdef HL_ENABLE_HTTP_SERVER
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
#endif /* HL_ENABLE_HTTP_SERVER */

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
#include "hull/cap/smtp_op.h"   /* hl_smtp_message_bytes */
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

/* Audit 26 L-M: smtp.send copied the message (into the owned op the worker
 * sends) uncharged, so a pcall'd loop of sends refused at admission copied
 * a big body on every call for one instruction each. The copy is charged
 * 1 unit per 8 bytes before it now, on the sync path too, and the size it
 * charges counts every field the op copies. */
UTEST(lua_audit26, smtp_send_copy_is_charged)
{
    HlSmtpMessage m = { .host = "h", .from = "f", .to = "t", .subject = "s",
                        .body = "0123456789" };
    const char *cc[] = { "c1", "c22" };
    m.cc = cc;
    m.cc_count = 2;
    size_t want = sizeof(HlSmtpOp) + 2 + 2 + 2 + 2 + 11
                + 2 * sizeof(char *) + 3 + 4;
    EXPECT_EQ(hl_smtp_message_bytes(&m), want);
    EXPECT_EQ(hl_smtp_message_bytes(NULL), (size_t)0);

    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlSmtpConfig smtp = {0};   /* no hosts: every send is refused */
    lua_rt.base.smtp_cfg = &smtp;
    char err[512];
    ASSERT_EQ(audit10_run("BIG = string.rep('x', 8 * 1000000) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* 10 sends of an 8 MB body: 10M units, over a 200k limit. */
    EXPECT_NE(audit10_run(
        "local o = { host = 'mail.invalid', from = 'a@x.test', to = 'b@x.test', "
        "            subject = 's', body = BIG } "
        "for i = 1, 10 do pcall(smtp.send, o) end return 1",
        200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* A small message is cheap (and refused: the host is not allowed). */
    EXPECT_EQ(audit10_run(
        "local r = smtp.send({ host = 'mail.invalid', from = 'a@x.test', "
        "  to = 'b@x.test', subject = 's', body = 'b' }) "
        "assert(type(r) == 'table' and not r.ok) return 1",
        200000, err, sizeof err), LUA_OK);
    audit10_run("BIG = nil return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err);
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

#ifdef HL_ENABLE_HTTP_SERVER

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
    hl_lua_http_error_response(&lua_rt, &res);
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
#endif /* HL_ENABLE_HTTP_SERVER */

/* ── Audit 11: Content-Type and error headers across middleware ────────── */

#ifdef HL_ENABLE_HTTP_SERVER
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
    hl_lua_http_error_response(&lua_rt, &res);
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
#endif /* HL_ENABLE_HTTP_SERVER */

/* ── The run watchdog (cap/run_watchdog.h, audit 12) ──────────────────
 *
 * A wall-clock deadline per uninterrupted run, the backstop for work one
 * instruction does that the instruction budget does not charge. Each case
 * runs with NO instruction limit, so only the watchdog can stop it, and a
 * short deadline (200 ms); assertions are on the trip, not on timing (the
 * sanitizer jobs are slow). */
#define WD_TEST_MS 200

static void wd_sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) {}
}

/* A VM with no instruction limit whose top level runs `code` under a fresh
 * WD_TEST_MS deadline; returns its status, the error in errbuf. */
static int wd_lua_run(const char *code, char *errbuf, size_t n)
{
    hl_run_watchdog_configure(WD_TEST_MS);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.max_instructions = 0;
    HlLua lim;
    memset(&lim, 0, sizeof lim);
    if (hl_lua_init(&lim, &cfg) != 0) {
        hl_run_watchdog_configure(-1);
        return -100;
    }
    HL_LUA_ARM(&lim, lim.L);
    int rc = luaL_dostring(lim.L, code);
    errbuf[0] = '\0';
    if (rc != LUA_OK) {
        const char *e = lua_tostring(lim.L, -1);
        snprintf(errbuf, n, "%s", e ? e : "");
    }
    hl_lua_free(&lim);
    hl_run_watchdog_configure(-1);
    return rc;
}

UTEST(lua_run_watchdog, loops_over_uncharged_work_are_stopped)
{
    static const char *const cases[] = {
        "while true do end",
        /* round-12 triggers: rehash churn, a finalizer check per object */
        "N = 0 while true do local t = {} for i = 1, 64 do t[i] = i end "
        "  t.x = 1 N = N + 1 end",
        "while true do setmetatable({}, { __gc = function() end }) end",
        /* the trip is uncatchable: pcall / xpcall / a coroutine */
        "while true do pcall(function() while true do end end) end",
        "xpcall(function() while true do end end, function() return 1 end) "
        "while true do end",
        "local co = coroutine.wrap(function() while true do end end) co() "
        "return 1",
        NULL
    };
    for (int i = 0; cases[i]; i++) {
        char err[512];
        int rc = wd_lua_run(cases[i], err, sizeof err);
        EXPECT_NE_MSG(rc, LUA_OK, cases[i]);
        EXPECT_NE_MSG(strstr(err, HL_RUN_TIME_LIMIT_MSG), NULL, cases[i]);
    }
}

UTEST(lua_run_watchdog, a_run_under_its_deadline_is_not_stopped)
{
    char err[512];
    EXPECT_EQ(wd_lua_run("local n = 0 for i = 1, 100000 do n = n + i end "
                         "assert(n > 0) return 1", err, sizeof err), LUA_OK);
    EXPECT_STREQ(err, "");
}

/* The finalizers lua_close runs at teardown are under a deadline of their
 * own (audit 22): the watch was disarmed before the close, so a finalizer
 * looping under no instruction limit hung hl_lua_free for good. The loop
 * here is the only thing that could stop it; a hang is the failure. */
UTEST(lua_run_watchdog, a_looping_finalizer_does_not_hang_teardown)
{
    char err[512];
    EXPECT_EQ(wd_lua_run("KEEP = setmetatable({}, { __gc = function() "
                         "  while true do end end }) return 1",
                         err, sizeof err), LUA_OK);
    EXPECT_STREQ(err, "");
}

/* A run that waits (a parked handler) is not over its deadline when it is
 * resumed: each entry re-arms it. Simulated: the deadline passes while the
 * VM is idle, and the next arm clears it. */
UTEST(lua_run_watchdog, an_idle_vm_is_rearmed_by_the_next_entry)
{
    hl_run_watchdog_configure(WD_TEST_MS);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.max_instructions = 0;
    HlLua lim;
    memset(&lim, 0, sizeof lim);
    ASSERT_EQ(hl_lua_init(&lim, &cfg), 0);
    HL_LUA_ARM(&lim, lim.L);
    wd_sleep_ms(2 * WD_TEST_MS + 100);
    EXPECT_TRUE(hl_run_watch_stopped(&lim.run_watch));
    HL_LUA_ARM(&lim, lim.L);
    EXPECT_FALSE(hl_run_watch_stopped(&lim.run_watch));
    EXPECT_EQ(luaL_dostring(lim.L, "return 1 + 1"), LUA_OK);
    hl_lua_free(&lim);
    hl_run_watchdog_configure(-1);
}

/* 0 turns the bound off; the instruction budget still applies. */
UTEST(lua_run_watchdog, zero_disables_it)
{
    hl_run_watchdog_configure(0);
    EXPECT_EQ(hl_run_watchdog_limit_ms(HL_RUN_ENTRY), 0u);
    EXPECT_EQ(hl_run_watchdog_limit_ms(HL_RUN_MAIN), 0u);
    hl_run_watchdog_configure(-1);
    EXPECT_EQ(hl_run_watchdog_limit_ms(HL_RUN_ENTRY), HL_RUN_DEFAULT_MS);
    EXPECT_EQ(hl_run_watchdog_limit_ms(HL_RUN_MAIN), HL_RUN_MAIN_DEFAULT_MS);
    int64_t v = 0;
    EXPECT_EQ(hl_run_watchdog_parse_ms("1500", &v), 0);
    EXPECT_EQ(v, 1500);
    EXPECT_EQ(hl_run_watchdog_parse_ms("-1", &v), -1);
    EXPECT_EQ(hl_run_watchdog_parse_ms("12x", &v), -1);
    EXPECT_EQ(hl_run_watchdog_parse_ms("", &v), -1);
}

#ifdef HL_ENABLE_WASM
/* ── Audit 15: compute.stream chunks are charged to the run ───────────
 *
 * compute.stream ran every chunk inside one binding call, each with its own
 * gas and timeout, and charged nothing to the run: a stream of many chunks
 * held the event loop for as long as UINT32_MAX chunk timeouts. Each chunk
 * is charged now (1 unit + 1 per 64 input bytes) and the binding raises the
 * trip once the stream is down. */

/* echo.wasm (tests/hull/cap/test_wasm.c): copies its input to its output. */
static const unsigned char a15_echo_wasm[] = {
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
static const HlEntry a15_wasm_entries[] = {
    { "compute/echo.wasm", a15_echo_wasm, sizeof a15_echo_wasm },
    { 0, 0, 0 }
};

/* 1 MB in 256-byte chunks is 4096 chunks: 20480 units charged per chunk,
 * against 16384 for the 1 MB result string. Under an 18500 limit the stream
 * is stopped part-way (unfixed, it ran to the end, the result fit the limit,
 * and `after` was set). Under the default limit it completes. */
UTEST(lua_audit15, compute_stream_chunks_are_charged_to_the_run)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a15_wasm_entries, NULL);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua v;
    memset(&v, 0, sizeof v);
    v.base.wasm_cache = &cache;
    v.base.app_vfs = &vfs;
    ASSERT_EQ(hl_lua_init(&v, &cfg), 0);
    HL_LUA_ARM(&v, v.L);
    ASSERT_EQ(luaL_dostring(v.L, "compute = require('hull.compute') "
                                 "ab = string.rep('a', 1 << 20)"), LUA_OK);

    int64_t saved = v.max_instructions;
    v.max_instructions = 18500;
    HL_LUA_ARM(&v, v.L);
    int rc = luaL_dostring(v.L,
        "compute.stream('echo', ab, { chunk_size = 256 }) after = 1");
    EXPECT_NE(rc, LUA_OK);
    const char *e = lua_tostring(v.L, -1);
    EXPECT_TRUE(e && strstr(e, "instruction limit exceeded") != NULL);
    if (e && !strstr(e, "instruction limit exceeded")) printf("  got: %s\n", e);
    lua_settop(v.L, 0);
    EXPECT_EQ(lua_getglobal(v.L, "after"), LUA_TNIL);
    lua_settop(v.L, 0);

    v.max_instructions = saved;
    HL_LUA_ARM(&v, v.L);
    EXPECT_EQ(luaL_dostring(v.L,
        "local s = compute.stream('echo', ab, { chunk_size = 256 }) "
        "assert(#s == 1 << 20)"), LUA_OK);

    hl_lua_free(&v);
    hl_cap_wasm_destroy(&cache);
}

/* Buffer-mode output is capped at what is left of the VM heap: it was grown
 * off-heap without bound, then copied into the heap in one piece (unfixed,
 * the 4 MB result was built off-heap and only the push failed). The input
 * is made under the default heap - string.rep builds its result in a
 * buffer and then copies it, so it needs twice its size - and the limit is
 * then narrowed to 1 MB above what is in use (the allocator reads mem_limit
 * live), leaving far less than the 4 MB echo. */
UTEST(lua_audit15, compute_stream_output_is_capped_at_the_heap_limit)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a15_wasm_entries, NULL);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua v;
    memset(&v, 0, sizeof v);
    v.base.wasm_cache = &cache;
    v.base.app_vfs = &vfs;
    ASSERT_EQ(hl_lua_init(&v, &cfg), 0);
    HL_LUA_ARM(&v, v.L);
    ASSERT_EQ(luaL_dostring(v.L, "compute = require('hull.compute') "
                                 "ab = string.rep('a', 4 * 1024 * 1024)"),
              LUA_OK);
    lua_gc(v.L, LUA_GCCOLLECT);   /* drop string.rep's buffer */
    size_t saved = v.mem_limit;
    v.mem_limit = v.mem_used + 1024 * 1024;
    HL_LUA_ARM(&v, v.L);
    int rc = luaL_dostring(v.L,
        "local s, err = compute.stream('echo', ab, { chunk_size = 65536 }) "
        "assert(s == nil and err == 'output_too_large', tostring(err))");
    EXPECT_EQ(rc, LUA_OK);
    if (rc != LUA_OK) {
        const char *e = lua_tostring(v.L, -1);
        printf("  got: %s\n", e ? e : "?");
    }
    v.mem_limit = saved;

    hl_lua_free(&v);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 18: compute.stream took a registry ref of its output callback
 * before reading the opts table, and an opts __index that raised leaked it
 * (one registry slot per call). The callback stays in its argument slot now.
 * Probed through the registry free list: a slot freed just before the call
 * is the next one handed out, unless the call took it and never gave it
 * back. */
UTEST(lua_audit18, compute_stream_raising_opts_leak_no_registry_ref)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a15_wasm_entries, NULL);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua v;
    memset(&v, 0, sizeof v);
    v.base.wasm_cache = &cache;
    v.base.app_vfs = &vfs;
    ASSERT_EQ(hl_lua_init(&v, &cfg), 0);
    HL_LUA_ARM(&v, v.L);
    ASSERT_EQ(luaL_dostring(v.L, "compute = require('hull.compute')"), LUA_OK);

    lua_pushboolean(v.L, 1);
    int r1 = luaL_ref(v.L, LUA_REGISTRYINDEX);
    luaL_unref(v.L, LUA_REGISTRYINDEX, r1);
    int rc = luaL_dostring(v.L,
        "local o = setmetatable({}, { __index = function() error('opts boom') end }) "
        "local ok, e = pcall(compute.stream, 'echo', 'abc', function() end, o) "
        "assert(not ok and tostring(e):find('opts boom', 1, true), tostring(e))");
    EXPECT_EQ(rc, LUA_OK);
    if (rc != LUA_OK) printf("  got: %s\n", lua_tostring(v.L, -1));
    lua_settop(v.L, 0);
    lua_pushboolean(v.L, 1);
    int r2 = luaL_ref(v.L, LUA_REGISTRYINDEX);
    EXPECT_EQ_MSG(r2, r1, "the raising call kept a registry slot");
    luaL_unref(v.L, LUA_REGISTRYINDEX, r2);

    /* The callback still receives the output. */
    EXPECT_EQ(luaL_dostring(v.L,
        "local got = {} "
        "compute.stream('echo', 'hello', function(d) got[#got + 1] = d end) "
        "assert(table.concat(got) == 'hello', table.concat(got))"), LUA_OK);

    hl_lua_free(&v);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 18 M4: an OWNED WasmBuffer (compute.buffer, the compute.call{buffer}
 * copy fallback, a GPU readback) holds its bytes on the server allocator,
 * which no VM limit covers: a loop keeping them alive grew without bound.
 * They are counted against the heap limit now (refused past it, given back
 * on close), and compute.buffer's copy is charged 1 unit per 8 bytes. */
UTEST(lua_audit18, owned_buffers_are_bounded_by_the_heap_limit)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a15_wasm_entries, NULL);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua v;
    memset(&v, 0, sizeof v);
    v.base.wasm_cache = &cache;
    v.base.app_vfs = &vfs;
    ASSERT_EQ(hl_lua_init(&v, &cfg), 0);
    HL_LUA_ARM(&v, v.L);
    ASSERT_EQ(luaL_dostring(v.L, "compute = require('hull.compute') "
                                 "s = string.rep('a', 1 << 20)"), LUA_OK);
    size_t saved = v.mem_limit;
    v.mem_limit = 8u << 20;   /* 8 MB of buffers, the strings fit too */
    HL_LUA_ARM(&v, v.L);
    int rc = luaL_dostring(v.L,
        "held = {}\n"
        "local ok, err = pcall(function()\n"
        "  for i = 1, 64 do held[i] = compute.buffer(s) end\n"
        "end)\n"
        "assert(not ok and tostring(err):find('heap limit'), tostring(err))\n"
        "assert(#held >= 7 and #held < 9, #held)\n"
        "for i = 1, #held do held[i]:close() end\n"
        "held = nil\n"
        "local b = compute.buffer(s); assert(#b == 1 << 20); b:close()");
    EXPECT_EQ(rc, LUA_OK);
    if (rc != LUA_OK) printf("  got: %s\n", lua_tostring(v.L, -1));
    lua_settop(v.L, 0);
    EXPECT_EQ(v.wasm_buf_bytes, (size_t)0);
    v.mem_limit = saved;

    /* The copy is charged: 1 MB is 131072 units, over a 100000 budget. */
    int64_t saved_ins = v.max_instructions;
    v.max_instructions = 100000;
    HL_LUA_ARM(&v, v.L);
    rc = luaL_dostring(v.L, "local b = compute.buffer(s) after = 1");
    EXPECT_NE(rc, LUA_OK);
    const char *e = lua_tostring(v.L, -1);
    EXPECT_TRUE(e && strstr(e, "instruction limit exceeded") != NULL);
    lua_settop(v.L, 0);
    EXPECT_EQ(lua_getglobal(v.L, "after"), LUA_TNIL);
    lua_settop(v.L, 0);
    v.max_instructions = saved_ins;

    hl_lua_free(&v);
    hl_cap_wasm_destroy(&cache);
}

#ifdef HL_ENABLE_IMAGE
/* Audit 18 L7: stb_image_write works off the VM heap, a few times the image
 * at its peak, and nothing refused an encode before the work. A zero-copy
 * image (here a view of a 16 MB WasmBuffer) can be larger than the heap
 * limit; its encode is refused with image_too_large. */
UTEST(lua_audit18, image_encode_refused_past_the_heap_limit)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a15_wasm_entries, NULL);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua v;
    memset(&v, 0, sizeof v);
    v.base.wasm_cache = &cache;
    v.base.app_vfs = &vfs;
    ASSERT_EQ(hl_lua_init(&v, &cfg), 0);
    HL_LUA_ARM(&v, v.L);
    if (luaL_dostring(v.L, "image = require('hull.image') "
                           "compute = require('hull.compute')") != LUA_OK) {
        lua_settop(v.L, 0);   /* image compiled out */
        hl_lua_free(&v);
        hl_cap_wasm_destroy(&cache);
        return;
    }
    ASSERT_EQ(luaL_dostring(v.L,
        "local s = string.rep('a', 16 << 20) "
        "buf = compute.buffer(s) s = nil "
        "img = image.from_buffer(buf, 4096, 4096, 'r8')"), LUA_OK);
    lua_gc(v.L, LUA_GCCOLLECT, 0);
    size_t saved = v.mem_limit;
    v.mem_limit = v.mem_used + (4u << 20);
    HL_LUA_ARM(&v, v.L);
    int rc = luaL_dostring(v.L,
        "local out, err = image.encode(img, 'png') "
        "assert(out == nil and err == 'image_too_large', tostring(err))");
    EXPECT_EQ(rc, LUA_OK);
    if (rc != LUA_OK) printf("  got: %s (mem_used %zu)\n",
                             lua_tostring(v.L, -1), v.mem_used);
    lua_settop(v.L, 0);
    v.mem_limit = saved;
    HL_LUA_ARM(&v, v.L);
    EXPECT_EQ(luaL_dostring(v.L, "img:close() buf:close()"), LUA_OK);
    lua_settop(v.L, 0);

    hl_lua_free(&v);
    hl_cap_wasm_destroy(&cache);
}

/* Audit 35: an encode was charged 1 unit per 8 RGBA bytes whatever the
 * format, ~30-40x under a PNG encode's real cost (~4x under JPEG's). A
 * 512 x 512 RGBA image is 131072 units at the base rate; as PNG it is 32x
 * that, past a 2M budget the old charge fit under, and as JPEG 4x. */
UTEST(lua_audit35, image_encode_is_charged_per_format)
{
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua v;
    memset(&v, 0, sizeof v);
    ASSERT_EQ(hl_lua_init(&v, &cfg), 0);
    HL_LUA_ARM(&v, v.L);
    if (luaL_dostring(v.L, "image = require('hull.image')") != LUA_OK) {
        lua_settop(v.L, 0);   /* image compiled out */
        hl_lua_free(&v);
        return;
    }
    ASSERT_EQ(luaL_dostring(v.L,
        "img = image.new(512, 512, 'rgba8', string.rep('\\0', 512 * 512 * 4))"),
        LUA_OK);
    lua_gc(v.L, LUA_GCCOLLECT, 0);

    int64_t saved = v.max_instructions;
    v.max_instructions = 2000000;
    HL_LUA_ARM(&v, v.L);
    int rc = luaL_dostring(v.L,
        "local out, err = image.encode(img, 'jpeg') assert(out, err)");
    EXPECT_EQ(rc, LUA_OK);
    if (rc != LUA_OK) printf("  got: %s\n", lua_tostring(v.L, -1));
    lua_settop(v.L, 0);

    HL_LUA_ARM(&v, v.L);
    rc = luaL_dostring(v.L, "pcall(image.encode, img, 'png') return 1");
    EXPECT_NE(rc, LUA_OK);
    const char *e = lua_tostring(v.L, -1);
    EXPECT_NE_MSG(strstr(e ? e : "", "instruction limit"), NULL, e ? e : "");
    lua_settop(v.L, 0);

    v.max_instructions = saved;
    HL_LUA_ARM(&v, v.L);
    EXPECT_EQ(luaL_dostring(v.L, "img:close()"), LUA_OK);
    lua_settop(v.L, 0);
    hl_lua_free(&v);
}
#endif /* HL_ENABLE_IMAGE */
#endif /* HL_ENABLE_WASM */

/* ── Audit 15: off-heap whole-object reads, tar.create / tar.pack ──────
 *
 * fs.read, blob.get, a blob reader's read(n) and tar.pack read whole files
 * into buffers from the server's allocator (no VM limit) and charged nothing
 * for the read; tar.create / tar.pack built the archive off-heap, uncapped.
 * Each is held to the heap limit before it allocates and charged before it
 * reads now. The budgets below sit between the old cost (the copy into the
 * heap, 1 unit per 64 bytes) and the new one (1 per 8 bytes read). */

#include "hull/cap/fs_policy.h"   /* HlFsPolicy (a15 fixture) */

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
    lua_rt.base.fs_cfg = &f->fs;
    return 0;
}

static void a15_fs_close(A15Fs *f)
{
    lua_rt.base.fs_cfg = NULL;
    hl_fs_policy_free(&f->policy);
    nftw(f->dir, bc_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

UTEST(lua_audit15, fs_read_is_charged_and_bounded)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    char err[512];
    /* One 8 MB read: 1M units now, ~131k for the copy before. */
    EXPECT_NE(audit10_run("local s = fs.read('big.bin') return 1",
                          500000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run("assert(#fs.read('big.bin') == 8 * 1024 * 1024) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* A file larger than the heap limit is refused before any allocation. */
    size_t saved = lua_rt.mem_limit;
    lua_rt.mem_limit = 4 * 1024 * 1024;
    EXPECT_EQ(audit10_run("local s, e = fs.read('big.bin') "
                          "assert(s == nil and e == 'file_too_large', tostring(e)) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    lua_rt.mem_limit = saved;
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit15, blob_reads_are_charged_and_clamped)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    char err[512];
    ASSERT_EQ(audit10_run(
        "blob = require('hull.blob') blob.init({ dir = 'blobs' }) "
        "ID = blob.put(string.rep('b', 4 * 1024 * 1024)) return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* blob.get of 4 MB: 512k units for the read + hash now. */
    EXPECT_NE(audit10_run("local s = blob.get(ID) return 1",
                          300000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* reader:read(n): n was allocated as asked (a terabyte failed), off-heap;
     * it is clamped now, and charged. */
    EXPECT_EQ(audit10_run(
        "local r = blob.reader(ID) local s = r:read(1 << 40) r:close() "
        "assert(#s == 4 * 1024 * 1024) return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE(audit10_run(
        "local r = blob.reader(ID) local s = r:read(8 * 1024 * 1024) return 1",
        500000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* A blob larger than the heap limit is refused before it is read. */
    size_t saved = lua_rt.mem_limit;
    lua_rt.mem_limit = 2 * 1024 * 1024;
    EXPECT_NE(audit10_run("local s = blob.get(ID) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "heap limit"), NULL);
    lua_rt.mem_limit = saved;
    cleanup_lua_caps();
    a15_fs_close(&f);
}

/* Audit 34: blob writers / readers hold descriptors; live handles are
 * capped, a dropped one is collected to make room, and the open is charged. */
UTEST(lua_audit34, blob_handles_are_capped_collected_and_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    ASSERT_EQ(hl_cap_blob_live_handles(), 0);
    char err[512];
    ASSERT_EQ(audit10_run(
        "blob = require('hull.blob') blob.init({ dir = 'blobs' }) "
        "ID = blob.put('abc') return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* Held handles: the cap, then a clear refusal. */
    EXPECT_EQ(audit10_run(
        "H = {} local ok, e = pcall(function() for i = 1, 100000 do "
        "  H[#H + 1] = (i % 2 == 0) and blob.writer() or blob.reader(ID) end end) "
        "assert(not ok and tostring(e):find('too many open blob'), tostring(e)) "
        /* The refusal carries the busy code (audit 35). */
        "assert(tostring(e):find('^%[hull:busy%] '), tostring(e)) "
        "assert(#H == 256, #H) return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_EQ(hl_cap_blob_live_handles(), HL_BLOB_MAX_LIVE_HANDLES);
    /* Closing gives the slot back exactly once. */
    EXPECT_EQ(audit10_run("H[2]:abort() H[2]:abort() H[1]:close() H[1]:close() return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_EQ(hl_cap_blob_live_handles(), HL_BLOB_MAX_LIVE_HANDLES - 2);
    /* Dropped without close: the open collects them and goes ahead. */
    EXPECT_EQ(audit10_run(
        "H = nil for i = 1, 600 do local w = blob.writer() end "
        "local w = blob.writer() w:write('x') "
        "assert(type(w:finalize()) == 'string') return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_TRUE(hl_cap_blob_live_handles() <= HL_BLOB_MAX_LIVE_HANDLES);
    /* The open is charged: 200 opens are over a 100k budget. */
    EXPECT_NE(audit10_run("for i = 1, 200 do blob.writer():abort() end return 1",
                          100000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    cleanup_lua_caps();
    EXPECT_EQ(hl_cap_blob_live_handles(), 0);
    a15_fs_close(&f);
}

UTEST(lua_audit15, tar_is_capped_at_the_heap_and_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    char err[512];
    ASSERT_EQ(audit10_run("tar = require('hull.archive.tar') "
                          "MB = string.rep('m', 1024 * 1024) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* 100 entries naming one 1 MB string: a 100 MB archive, built off-heap
     * before (the push then failed). Refused before it is built now. */
    EXPECT_EQ(audit10_run(
        "local e = {} for i = 1, 100 do e[i] = { name = 'f' .. i, data = MB } end "
        "local b, msg = tar.create(e) "
        "assert(b == nil and tostring(msg):find('heap limit', 1, true), tostring(msg)) "
        "return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* More entries than any archive takes. */
    EXPECT_EQ(audit10_run(
        "local e = {} for i = 1, 65537 do e[i] = { name = 'f', is_dir = true } end "
        "local b, msg = tar.create(e) "
        "assert(b == nil and tostring(msg):find('too many', 1, true), tostring(msg)) "
        "return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* The build is charged (1 unit per 64 bytes). */
    EXPECT_NE(audit10_run(
        "local e = {} for i = 1, 16 do e[i] = { name = 'f' .. i, data = MB } end "
        "local b = tar.create(e) return 1", 200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* tar.pack: the same 8 MB file ten times is 80 MB - over the heap -
     * refused by the running total before the eighth read. */
    EXPECT_EQ(audit10_run(
        "MB = nil collectgarbage() "   /* seven files are read: room for 56 MB */
        "local l = {} for i = 1, 10 do l[i] = { path = 'big.bin', name = 'b' .. i } end "
        "local b, msg = tar.pack(l) "
        "assert(b == nil and tostring(msg):find('heap limit', 1, true), tostring(msg)) "
        "return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* ...and each read is charged: one 8 MB file is 1M units now. */
    EXPECT_NE(audit10_run("local b = tar.pack({ 'big.bin' }) return 1",
                          500000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run("assert(#tar.pack({ 'big.bin' }) > 8 * 1024 * 1024) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

/* tar.extract wrote every entry to disk with no charge: an archive of any
 * size (a MappedBuffer of up to 1 GiB) cost a handful of instructions. Each
 * entry is charged before it is written now, as fs.write is (audit 16). */
UTEST(lua_audit16, tar_extract_is_charged_per_entry)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    char err[512];
    ASSERT_EQ(audit10_run("tar = require('hull.archive.tar') "
                          "local mb = string.rep('m', 1024 * 1024) "
                          "local e = {} for i = 1, 8 do e[i] = { name = 'f' .. i, data = mb } end "
                          "TB = tar.create(e) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* 8 MB to write: ~1M units; each 1 MB entry is 131073, so the budget
     * runs out at the fourth entry, before it is written. */
    EXPECT_NE(audit10_run("tar.extract(TB, 'out') return 1",
                          500000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    char p[HL_TEST_PATH_MAX + 32];
    struct stat st;
    snprintf(p, sizeof p, "%s/out/f1", f.dir);
    EXPECT_EQ(0, stat(p, &st));
    snprintf(p, sizeof p, "%s/out/f8", f.dir);
    EXPECT_NE(0, stat(p, &st));
    EXPECT_EQ(audit10_run("assert(tar.extract(TB, 'out2') == true) "
                          "TB = nil return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    snprintf(p, sizeof p, "%s/out2/f8", f.dir);
    EXPECT_EQ(0, stat(p, &st));
    cleanup_lua_caps();
    a15_fs_close(&f);
}

/* Headers of a typeflag the parser skips (here '1', a hard link) were
 * checksummed and never surfaced, so the per-entry charges missed them: a
 * 10 MB archive of them cost nothing. The input is charged up front now,
 * one unit per 64 bytes (audit 17). */
UTEST(lua_audit17, tar_skipped_headers_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    char err[512];
    ASSERT_EQ(audit10_run("tar = require('hull.archive.tar') "
                          "local h = tar.create({ { name = 'l', data = '' } }):sub(1, 512) "
                          "local sum = tonumber(h:sub(149, 156):match('[0-7]+'), 8) + 1 "
                          "h = h:sub(1, 148) .. string.format('%06o', sum) .. '\\0 1' "
                          "    .. h:sub(158) "
                          "assert(#h == 512) "
                          "LB = string.rep(h, 20000) "
                          "assert(#tar.parse(LB) == 0) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* 10 MB walked: 160000 units. */
    EXPECT_NE(audit10_run("tar.parse(LB) return 1", 100000, err, sizeof err),
              LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_NE(audit10_run("tar.extract(LB, 'out') return 1", 100000,
                          err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run("assert(tar.extract(LB, 'out') == true) LB = nil "
                          "return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err),
              LUA_OK);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

/* The hull.encoding C codecs were charged only through their result
 * buffer, at the allocator's rate (1 unit per 64 bytes), and their input
 * scan not at all. They charge input + output 1 unit per 8 bytes before the
 * work now, the hull:encoding rate in JS (audit 18). The medium decode
 * passing under the limit shows the native path ran (the pure-Lua codec
 * costs several instructions per byte). */
UTEST(lua_audit18, encoding_codecs_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    ASSERT_EQ(audit10_run("enc = require('hull.encoding') "
                          "MID = string.rep('00', 200000) "
                          "HEX = string.rep('00', 2000000) "
                          "RAW = string.rep('x', 3000000) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* 400 KB in, 200 KB out: 75000 units. */
    EXPECT_EQ(audit10_run("assert(#enc.hex.decode(MID) == 200000) return 1",
                          400000, err, sizeof err), LUA_OK);
    /* 4 MB in, 2 MB out: 750000 units. */
    EXPECT_NE(audit10_run("enc.hex.decode(HEX) return 1", 400000,
                          err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* 3 MB in, 4 MB out: 875000 units. */
    EXPECT_NE(audit10_run("enc.base64.encode(RAW) return 1", 400000,
                          err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run("MID, HEX, RAW = nil, nil, nil return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
}

#ifdef HL_ENABLE_HTTP_SERVER
/* ── Audit 14: SSE and WebSocket writes are charged ────────────────────
 *
 * stream:event / stream:comment wrote every line of their text as three
 * stream writes, conn:send / ws.broadcast framed and queued their message
 * (to every connection), all inside one uncharged call. Each is charged
 * before the write now: 1 unit per 8 bytes, plus 1 per SSE line / per frame,
 * per destination. A tripped charge stops the call before Keel is reached. */

#include "../../../../src/hull/runtime/lua/mod_buffer.h"   /* struct HlSseStreamUD */
#include "hull/cap/ws.h"                                   /* HlWsRegistry */

static int a14_sse_writes;
static int a14_sse_write(void *ctx, const char *data, size_t len)
{
    (void)ctx; (void)data; (void)len;
    a14_sse_writes++;
    return 0;
}

UTEST(lua_audit14, sse_and_ws_writes_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    char err[512];

    /* A stream whose writes land in a counter (no request: a NULL life is
     * live). */
    struct HlSseStreamUD *ud = (struct HlSseStreamUD *)lua_newuserdata(L, sizeof *ud);
    memset(ud, 0, sizeof *ud);
    ud->sse.write_fn = a14_sse_write;
    luaL_setmetatable(L, "HlSseStream");
    lua_setglobal(L, "S");
    ASSERT_EQ(audit10_run("NL = string.rep('\\n', 1000000) "
                          "BIG = string.rep('w', 8 * 1000000) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    a14_sse_writes = 0;
    EXPECT_NE(audit10_run("S:event('e', NL) return 1", 200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_NE(audit10_run("S:comment(NL) return 1", 200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(a14_sse_writes, 0);   /* stopped before Keel wrote a byte */
    EXPECT_EQ(audit10_run("S:event('e', 'a\\nb', '7') S:comment('hi') return 1",
                          200000, err, sizeof err), LUA_OK);
    EXPECT_GT(a14_sse_writes, 0);

    /* ws.broadcast is charged per connection on the path (these have no Keel
     * connection, so nothing is sent): 100 x 1 MB. */
    HlAllocator wa;
    hl_alloc_init(&wa, 0);
    HlWsRegistry reg;
    hl_ws_registry_init(&reg, &wa);
    for (int i = 0; i < 100; i++)
        ASSERT_NE(hl_ws_registry_add(&reg, "/ws", NULL), NULL);
    lua_rt.base.ws_registry = &reg;
    ASSERT_EQ(audit10_run("ws = require('hull.web.ws-server') "
                          "MB = string.rep('m', 1024 * 1024) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE(audit10_run("ws.broadcast('/ws', MB) return 1",
                          1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run("assert(ws.broadcast('/ws', 'hi') == 0) return 1",
                          1000000, err, sizeof err), LUA_OK);

    /* conn:send: the charge trips before Keel is reached, so a connection
     * whose Keel handle is a placeholder is never written to. */
    static char placeholder;
    HlWsConn *c = hl_ws_registry_add(&reg, "/one", (KlWsServerConn *)(void *)&placeholder);
    ASSERT_NE(c, NULL);
    hl_lua_ws_push_conn(L, c);
    lua_setglobal(L, "C");
    EXPECT_NE(audit10_run("C:send(BIG) return 1", 200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_NE(audit10_run("C:send_binary(BIG) return 1", 200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    hl_lua_ws_invalidate_conn(L, c);

    lua_rt.base.ws_registry = NULL;
    hl_ws_registry_free(&reg);
    cleanup_lua_caps();
}

/* The outbound twin (hull.web.ws-client): conn:send / conn:send_binary are
 * charged the same way before Keel is reached, so a client whose Keel handle
 * is a placeholder is never written to (audit 15 follow-up: the charge #748
 * added had no test). */
UTEST(lua_audit14, ws_client_sends_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    char err[512];

    /* Registers the client connection metatable. */
    luaL_requiref(L, "hull.web.ws-client", luaopen_hull_ws_client, 0);
    lua_pop(L, 1);
    ASSERT_EQ(audit10_run("BIG = string.rep('w', 8 * 1000000) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);

    static char placeholder;
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)lua_newuserdata(L, sizeof *ud);
    memset(ud, 0, sizeof *ud);
    ud->client = (struct KlWsClientConn *)(void *)&placeholder;
    ud->on_open_ref = ud->on_message_ref = LUA_NOREF;
    ud->on_close_ref = ud->on_error_ref = ud->self_ref = LUA_NOREF;
    ud->L = L;
    ud->lua = &lua_rt;
    luaL_setmetatable(L, HL_WS_CLIENT_CONN_MT);
    lua_setglobal(L, "WC");

    EXPECT_NE(audit10_run("WC:send(BIG) return 1", 200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_NE(audit10_run("WC:send_binary(BIG) return 1", 200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);

    /* Nothing reached Keel: the finalizer must not free the placeholder. */
    ud->client = NULL;
    ud->closed = 1;
    lua_pushnil(L);
    lua_setglobal(L, "WC");
    cleanup_lua_caps();
}

/* Audit 17: conn.data read or set after the connection closed made a
 * registry ref nothing freed (the userdata had no __gc), so every remote
 * reconnect grew the registry. The table is the userdata's user value now:
 * it is collected with the userdata, a cycle through it included. The
 * metatable is locked like the others. */
UTEST(lua_audit17, ws_conn_data_after_close_is_collected)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    char err[512];

    HlAllocator wa;
    hl_alloc_init(&wa, 0);
    HlWsRegistry reg;
    hl_ws_registry_init(&reg, &wa);
    lua_rt.base.ws_registry = &reg;
    ASSERT_EQ(audit10_run("ws = require('hull.web.ws-server') return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    HlWsConn *c = hl_ws_registry_add(&reg, "/d", NULL);
    ASSERT_NE(c, NULL);
    hl_lua_ws_push_conn(L, c);
    lua_setglobal(L, "C");
    EXPECT_EQ_MSG(audit10_run(
        "C.data.x = 1 assert(C.data.x == 1) "
        "C.data = { y = 2 } assert(C.data.y == 2 and C.data.x == nil) "
        "assert(getmetatable(C) == 'locked') return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK, err);
    hl_lua_ws_invalidate_conn(L, c);

    EXPECT_EQ_MSG(audit10_run(
        "assert(C.data.y == nil) "   /* the close dropped the table */
        "W = setmetatable({}, { __mode = 'v' }) "
        "for i = 1, 100 do local t = {} C.data = t W[i] = t end "
        "W.lazy = C.data C.data.self = C "
        "C = nil collectgarbage() collectgarbage() "
        "local n = 0 for _ in pairs(W) do n = n + 1 end "
        "assert(n == 0, n .. ' data tables kept') return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK, err);

    lua_rt.base.ws_registry = NULL;
    hl_ws_registry_free(&reg);
    cleanup_lua_caps();
}
#endif /* HL_ENABLE_HTTP_SERVER */

#ifdef HL_ENABLE_IMAGE
/* Audit 17: image.encode checked the image before reading its options, so
 * an opts __index that closed it left the binding a NULL image. The check
 * runs after the arguments now; the metatable is locked. */
UTEST(lua_audit17, image_encode_closed_by_its_opts)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(luaL_dostring(lua_rt.L, "return image ~= nil"), LUA_OK);
    if (!lua_toboolean(lua_rt.L, -1)) {   /* image compiled out */
        cleanup_lua();
        return;
    }
    lua_settop(lua_rt.L, 0);
    int rc = luaL_dostring(lua_rt.L,
        "local img = image.new(2, 2, 'rgba8', string.rep('\\0', 16)) "
        "assert(getmetatable(img) == 'locked') "
        "local opts = setmetatable({}, { __index = function() img:close() end }) "
        "local ok, e = pcall(image.encode, img, 'png', opts) "
        "if ok then return 'encoded a closed image' end "
        "return tostring(e):find('already closed', 1, true) and 'ok' or tostring(e)");
    ASSERT_EQ(rc, LUA_OK);
    EXPECT_STREQ(lua_tostring(lua_rt.L, -1), "ok");
    lua_settop(lua_rt.L, 0);
    cleanup_lua();
}
#endif /* HL_ENABLE_IMAGE */

/* A VM freed while its deadline is pending: the watchdog must never write to
 * the freed watch (ASan reports it if it does). */
UTEST(lua_run_watchdog, a_vm_freed_while_armed_is_never_touched)
{
    hl_run_watchdog_configure(WD_TEST_MS);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    for (int i = 0; i < 4; i++) {
        HlLua *l = calloc(1, sizeof *l);
        ASSERT_TRUE(l != NULL);
        ASSERT_EQ(hl_lua_init(l, &cfg), 0);
        HL_LUA_ARM(l, l->L);
        hl_lua_free(l);
        free(l);
    }
    wd_sleep_ms(2 * WD_TEST_MS + 100);
    hl_run_watchdog_configure(-1);
}

/* A worker.dispatch job has its own deadline; the dispatching run, parked
 * meanwhile, is past ITS deadline when resumed and still completes. */
UTEST(lua_run_watchdog, a_runaway_worker_dispatch_is_stopped)
{
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    lua_rt.max_instructions = 0;
    hl_run_watchdog_configure(WD_TEST_MS);
    char out[256];
    lua_worker_run(&f,
        "local r = require('hull.worker').dispatch(\n"
        "                      function() while true do end end)\n"
        "return type(r) == 'table' and r.error or tostring(r)\n", out, sizeof out);
    EXPECT_NE_MSG(strstr(out, HL_RUN_TIME_LIMIT_MSG), NULL, out);
    hl_run_watchdog_configure(-1);
    lua_worker_close(&f);
}

/* ── Audit 12: runtime lows ──────────────────────────────────────────── */

#ifdef HL_ENABLE_HTTP_SERVER
/* res:header handed Keel C strings it measures with strlen, after Hull's
 * Content-Type check had looked at the whole Lua string: a name with a NUL
 * ("Content-Type\0x") went out as a second, unchecked Content-Type. A name
 * must be a token now and a value may not hold a NUL; both raise. */
UTEST(lua_audit12, header_name_must_be_a_token_and_nul_is_refused)
{
    static const char *const bad[] = {
        "res:header('Content-Type\\0x', 'text/html')",
        "res:header('X-A\\0', 'v')",
        "res:header('Bad Name', 'v')",
        "res:header('', 'v')",
        "res:header('X-A:', 'v')",
        "res:header('X-\\xc3\\xa9', 'v')",
        "res:header('X-A', 'v\\0w')",
    };
    for (size_t i = 0; i <= sizeof bad / sizeof bad[0]; i++) {
        const char *call = i < sizeof bad / sizeof bad[0] ? bad[i]
            : "res:header(\"X-Ok_1!#$%&'*+.^`|~\", 'a b')";
        init_lua();
        ASSERT_TRUE(lua_initialized);
        lua_State *L = lua_rt.L;
        char code[512];
        snprintf(code, sizeof code,
            "app.manifest({modules = {'hull/http-server@1'}})\n"
            "app.use('*', '/*', function(req, res) res:json(1) %s return 1 end)\n",
            call);
        ASSERT_EQ(luaL_dostring(L, code), LUA_OK);
        KlAllocator alloc = kl_allocator_default();
        KlHttpResponse res;
        ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
        KlHttpRequest req = {0};
        int ok = i == sizeof bad / sizeof bad[0];
        EXPECT_EQ_MSG(hl_lua_dispatch_middleware(&lua_rt,
                          first_mw_handler_id(L, 1), &req, &res),
                      ok ? 1 : -1, call);
        EXPECT_EQ_MSG(a10_header_count(&res, "Content-Type"), 1, call);
        if (ok)
            EXPECT_TRUE(a10_headers_contain(&res, "X-Ok_1!#$%&'*+.^`|~: a b\r\n"));
        else
            EXPECT_FALSE_MSG(a10_headers_contain(&res, "X-A"), call);
        free_lua_req_ctx(&req);
        kl_http_response_free(&res);
        lua_settop(L, 0);
        cleanup_lua();
    }
}

/* The kept-headers snapshots: a middleware that raises keeps the headers the
 * middleware before it set (its 500 dropped them all: the snapshot was only
 * taken at handler start, and a middleware start forgot it); the list is the
 * runtime's own and hl_lua_free empties it; and a request whose dispatch
 * fails before its handler runs never gets an earlier request's snapshot. */
UTEST(lua_audit12, error_reset_snapshots_are_per_request_and_per_runtime)
{
    init_lua();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    ASSERT_EQ(luaL_dostring(L,
        "app.manifest({modules = {'hull/http-server@1'}})\n"
        "app.use('*', '/*', function(req, res)\n"
        "  res:header('Strict-Transport-Security', 'max-age=1') return 0 end)\n"
        "app.use('*', '/*', function(req, res)\n"
        "  res:header('Set-Cookie', 'sid=1') error('mw boom') end)\n"
        "app.use('*', '/*', function(req, res) return 0 end)\n"),
        LUA_OK);
    int mw1 = first_mw_handler_id(L, 1);
    int mw2 = first_mw_handler_id(L, 2);
    int mw3 = first_mw_handler_id(L, 3);
    KlAllocator alloc = kl_allocator_default();

    /* A raising middleware keeps the earlier middleware's header. */
    KlHttpResponse res;
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest req = {0};
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt, mw1, &req, &res), 0);
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt, mw2, &req, &res), -1);
    hl_lua_http_error_response(&lua_rt, &res);
    EXPECT_EQ(res.status, 500);
    EXPECT_EQ(a10_header_count(&res, "Strict-Transport-Security"), 1);
    EXPECT_EQ(a10_header_count(&res, "Set-Cookie"), 0);
    EXPECT_EQ(lua_rt.res_bases.count, (size_t)0);   /* taken by the 500 */
    free_lua_req_ctx(&req);
    kl_http_response_free(&res);

    /* Request A leaves a snapshot; request B on the same response fails
     * before any handler runs: its 500 must not restore A's header. */
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest ra = {0};
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt, mw1, &ra, &res), 0);
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt, mw3, &ra, &res), 0);
    EXPECT_EQ(lua_rt.res_bases.count, (size_t)1);
    free_lua_req_ctx(&ra);
    kl_http_response_free(&res);
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest rb = {0};
    EXPECT_EQ(hl_lua_dispatch(&lua_rt, 99999, &rb, &res), -1);   /* no handler */
    hl_lua_http_error_response(&lua_rt, &res);
    EXPECT_EQ(res.status, 500);
    EXPECT_EQ(a10_header_count(&res, "Strict-Transport-Security"), 0);
    free_lua_req_ctx(&rb);
    kl_http_response_free(&res);

    /* A snapshot left behind is the runtime's, and goes with it. */
    ASSERT_EQ(kl_http_response_init(&res, &alloc), 0);
    KlHttpRequest rc = {0};
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt, mw1, &rc, &res), 0);
    EXPECT_EQ(hl_lua_dispatch_middleware(&lua_rt, mw3, &rc, &res), 0);
    EXPECT_EQ(lua_rt.res_bases.count, (size_t)1);
    free_lua_req_ctx(&rc);
    kl_http_response_free(&res);
    lua_settop(L, 0);
    cleanup_lua();
    EXPECT_EQ(lua_rt.res_bases.count, (size_t)0);
    EXPECT_EQ(lua_rt.res_bases.bytes, (size_t)0);
    EXPECT_TRUE(lua_rt.res_bases.head == NULL);
}

/* Set field @p key of timer def @p idx in the registry's __hull_timer_defs:
 * an integer (kind 'i'), a float ('f') or a string ('s'). */
static void a12_set_timer_def(lua_State *L, int idx, const char *key,
                              char kind, lua_Integer i, double f,
                              const char *s)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_timer_defs");
    lua_rawgeti(L, -1, idx);
    if (kind == 'i') lua_pushinteger(L, i);
    else if (kind == 'f') lua_pushnumber(L, f);
    else lua_pushstring(L, s);
    lua_setfield(L, -2, key);
    lua_pop(L, 2);
}

/* Timer wiring checks each def as app.every / app.daily do, and a timer that
 * cannot be armed or tracked refuses the wiring (it was skipped, and one
 * armed but not tracked was left running). */
UTEST(lua_audit12, invalid_timer_defs_fail_the_wiring)
{
    static const struct {
        int idx; const char *key; char kind; lua_Integer i; double f;
        const char *s;
    } tamper[] = {
        { 1, "interval_ms", 'i', 0,  0,     NULL },
        { 1, "interval_ms", 'i', 99, 0,     NULL },
        { 1, "interval_ms", 'f', 0,  150.5, NULL },
        { 1, "interval_ms", 's', 0,  0,     "1000" },
        { 1, "handler_id",  'i', -1, 0,     NULL },
        { 1, "type",        's', 0,  0,     "hourly" },
        { 2, "hour",        'i', 99, 0,     NULL },
        { 2, "minute",      'i', -1, 0,     NULL },
        { 2, "minute",      'i', 60, 0,     NULL },
    };
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    size_t n = sizeof tamper / sizeof tamper[0];
    for (size_t c = 0; c <= n; c++) {
        init_lua();
        ASSERT_TRUE(lua_initialized);
        ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);
        lua_State *L = lua_rt.L;
        ASSERT_EQ(luaL_dostring(L,
            "app.manifest({modules = {'hull/http-server@1', 'hull/timers@1'}})\n"
            "app.get('/x', function(req, res) end)\n"
            "app.every(100, function() end)\n"
            "app.daily('03:30', function() end)\n"), LUA_OK);
        if (c < n)
            a12_set_timer_def(L, tamper[c].idx, tamper[c].key, tamper[c].kind,
                              tamper[c].i, tamper[c].f, tamper[c].s);
        KlHttpServer server;
        KlHttpServerConfig cfg = { .port = 0, .max_connections = 1, .alloc = NULL };
        kl_http_server_init(&server, &cfg);
        EXPECT_EQ_MSG(hl_lua_wire_routes_server(&lua_rt, &server, NULL),
                      c < n ? -1 : 0, c < n ? tamper[c].key : "valid");
        if (c == n)
            EXPECT_EQ(lua_rt.timer_count, (size_t)2);
        kl_http_server_free(&server);
        HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
        lua_settop(L, 0);
        cleanup_lua();
        be->tick(actx, 0);
        be->free(actx);
    }
}
#endif /* HL_ENABLE_HTTP_SERVER */

/* A failed task's hook (hull._spawn's on_fail) runs as an entry of its own:
 * it re-arms the budget and clears the active run's state. Run from a loop
 * pumped inside another run, it left that run with the hook's budget and
 * SQL budget binding, and its completion hook cleared; they are put back
 * now, as lua_task_fire does. */
UTEST(lua_audit12, a_task_failure_hook_restores_the_runs_state)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);
    lua_State *L = lua_rt.L;
    /* The run's wall-clock deadline too (audit 14): the hook arms its own,
     * which the run used to keep. */
    HL_LUA_ARM(&lua_rt, L);
    uint64_t run_deadline = hl_run_watch_save(&lua_rt.run_watch);
    EXPECT_NE(run_deadline, (uint64_t)0);
    ASSERT_EQ(luaL_dostring(L,
        "HOOKED = 0\n"
        "hull._spawn(function() error('task boom') end,\n"
        "            function(msg) HOOKED = HOOKED + 1 end)\n"), LUA_OK);
    wd_sleep_ms(20);   /* the hook's deadline is a later one */

    /* The state of the run the loop is pumped from. */
    static int marker;
    oc_calls = 0;
    lua_rt.active_on_complete     = count_oc;
    lua_rt.active_on_complete_ctx = &marker;
    HlLuaBudget run_budget = { .limit = 12345, .used = 678, .tripped = 0 };
    lua_rt.budget = run_budget;

    int hooked = 0;
    for (int i = 0; i < 20 && !hooked; i++) {
        be->tick(lua_rt.base.async_ctx, 10);
        lua_getglobal(L, "HOOKED");
        hooked = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    EXPECT_EQ(hooked, 1);
    EXPECT_TRUE(lua_rt.active_on_complete == count_oc);
    EXPECT_TRUE(lua_rt.active_on_complete_ctx == &marker);
    EXPECT_EQ(lua_rt.budget.limit, (int64_t)12345);
    EXPECT_EQ(lua_rt.budget.used, (int64_t)678);
    EXPECT_EQ(lua_rt.budget.tripped, 0);
    EXPECT_EQ(oc_calls, 0);
    EXPECT_EQ(hl_run_watch_save(&lua_rt.run_watch), run_deadline);

    lua_rt.active_on_complete     = NULL;
    lua_rt.active_on_complete_ctx = NULL;
    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();
    be->tick(actx, 0);
    be->free(actx);
}

/* Audit 17: hull._spawn runs the task's first stretch inline, on the
 * spawner's budget. A task that tripped it ended the spawner's run too, but
 * spawn returned normally and the spawner ran on until its own hook's stride
 * came round. It raises the trip now - through pcall as well. */
UTEST(lua_audit17, spawn_reraises_a_task_budget_trip)
{
    static const char *const srcs[] = {
        "AFTER = 0\n"
        "hull._spawn(function() while true do end end)\n"
        "AFTER = 1\n",
        "AFTER = 0\n"
        "local ok = pcall(hull._spawn, function() while true do end end)\n"
        "AFTER = 1\n",
        "AFTER = 0\n"
        "hull._spawn(function() while true do end end, function() end)\n"
        "AFTER = 1\n",
    };
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        init_lua();
        ASSERT_TRUE(lua_initialized);
        lua_State *L = lua_rt.L;
        lua_rt.max_instructions = 100000;
        HL_LUA_ARM(&lua_rt, L);
        int rc = luaL_dostring(L, srcs[i]);
        EXPECT_NE_MSG(rc, LUA_OK, srcs[i]);
        const char *e = rc != LUA_OK ? lua_tostring(L, -1) : NULL;
        EXPECT_NE_MSG(strstr(e ? e : "", "instruction limit"), NULL, srcs[i]);
        lua_settop(L, 0);
        lua_rt.max_instructions = 0;
        HL_LUA_ARM(&lua_rt, L);
        lua_getglobal(L, "AFTER");
        EXPECT_EQ_MSG((int)lua_tointeger(L, -1), 0, srcs[i]);
        lua_pop(L, 1);
        lua_rt.max_instructions = HL_DEFAULT_INSTRUCTIONS;
        cleanup_lua();
    }
}

UTEST(lua_stdlib, etag_audit13_suite)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    long long pass = 0, fail = -1;
    int rc = run_lua_test_in_runtime("stdlib/lua/hull/tests/test_etag_audit13.lua", &pass, &fail);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(fail, 0LL);
    EXPECT_EQ(pass, 36LL);
    cleanup_lua_caps();
}

/* ── Audit 15: runtime lows ───────────────────────────────────────────── */

#ifdef HL_ENABLE_HTTP_CLIENT
/* A wait refuses to start inside a transaction, but http.fetch checked that
 * before it read its options: an opts __index (app code) that BEGINs ran
 * after the check, and the fetch then waited inside the transaction. The
 * check runs after the options now, with no app code before the op is
 * armed. Unfixed, the fetch got as far as the host check. */
UTEST(lua_audit15, the_wait_check_runs_after_the_opts_are_read)
{
    HlHttpConfig cfg = {0};          /* no hosts: a started fetch is refused */
    task_http_cfg = &cfg;
    char out[512];
    int rc = run_task_script_db(
        "local db = require('hull.db').default()\n"
        "local http = require('hull.http-client')\n"
        "local opts = setmetatable({}, { __index = function(_, k)\n"
        "  if k == 'body' then db.exec('BEGIN') end end })\n"
        "local ok, e = pcall(http.async.request, 'GET', 'http://x.invalid/', opts)\n"
        "check(not ok and tostring(e):find('transaction is open', 1, true),\n"
        "      'fetch: '..tostring(e))\n"
        "db.exec('ROLLBACK')\n",
        out, sizeof out);
    task_http_cfg = NULL;
    ASSERT_EQ(rc, 0);
    ASSERT_STREQ(out, "ok");
}
#endif

/* A worker.dispatch VM is closed after the job, and lua_close runs its __gc
 * finalizers - app code. The job's deadline was disarmed before the close,
 * so a looping finalizer held the pool thread for good; it is disarmed after
 * now. No instruction limit: only the deadline can stop it. */
UTEST(lua_audit15, a_dispatch_finalizer_is_bounded_by_the_deadline)
{
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    hl_run_watchdog_configure(WD_TEST_MS);
    lua_rt.max_instructions = 0;
    char out[256];
    lua_worker_run(&f,
        "return require('hull.worker').dispatch(function()\n"
        "  KEEP = setmetatable({}, { __gc = function() while true do end end })\n"
        "  return 'done' end)\n", out, sizeof out);
    hl_run_watchdog_configure(-1);
    EXPECT_STREQ(out, "done");
    lua_worker_close(&f);
}

#ifdef HL_ENABLE_HTTP_SERVER
/* A timer handler's wait that is cancelled never completes, so nothing
 * cleared the timer's in_flight: every later fire saw it in flight and only
 * retried. The cancel clears it and reschedules the timer (not at shutdown). */
UTEST(lua_audit15, a_cancelled_timer_wait_clears_in_flight)
{
    LuaWorkerFix f;
    ASSERT_EQ(lua_worker_open(&f), 0);
    HlLuaTimer t;
    memset(&t, 0, sizeof t);
    t.lua = &lua_rt;
    t.interval_ms = 60000;
    t.in_flight = 1;                 /* as the trampoline leaves it */
    lua_rt.active_timer = &t;
    lua_State *co;
    EXPECT_EQ(ssh_co_start(&lua_rt, "hull.sleep(60000) return 'woke'", &co),
              LUA_YIELD);
    lua_rt.active_timer = NULL;
    lua_rt.active_co = NULL;
    lua_rt.active_thread_ref = LUA_NOREF;   /* the continuation holds it */
    /* Cancel the wait, the runtime not shutting down. */
    hl_async_teardown_run(&lua_rt.base.async_live, lua_rt.base.async_ctx);
    EXPECT_EQ(t.in_flight, 0);
    EXPECT_NE(t.timer_id, (int64_t)0);
    if (t.timer_id > 0)
        f.be->timer_cancel(f.actx, (uint64_t)t.timer_id);
    lua_worker_close(&f);
}
#endif

/* ── Audit 21 G6: store walks, mmap retry, segment copies ── */

UTEST(lua_audit21, blob_store_walks_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    char err[512];
    ASSERT_EQ(audit10_run(
        "blob = require('hull.blob') blob.init({ dir = 'blobs' }) "
        "return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* 200 walks x 256 shard directories > 40k units. */
    EXPECT_NE(audit10_run("for i = 1, 200 do blob.count() end return 1",
                          40000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_NE(audit10_run("for i = 1, 200 do blob.total_size() end return 1",
                          40000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

/* A MappedBuffer dropped without close() is a small userdata: 256 of them
 * between collections filled the SIGBUS guard table for the process. */
UTEST(lua_audit21, mmap_collects_and_retries_when_the_guard_table_is_full)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a15_fs_open(&f), 0);
    char err[512];
    EXPECT_EQ(audit10_run(
        "collectgarbage('stop') "
        "for i = 1, 300 do local m, e = fs.mmap('big.bin') assert(m, e) end "
        "collectgarbage('restart') collectgarbage() return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    if (err[0]) printf("  err: %s\n", err);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

#ifdef HL_ENABLE_WASM
UTEST(lua_audit21, compute_segment_copies_are_charged)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a15_wasm_entries, NULL);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua v;
    memset(&v, 0, sizeof v);
    v.base.wasm_cache = &cache;
    v.base.app_vfs = &vfs;
    ASSERT_EQ(hl_lua_init(&v, &cfg), 0);
    HL_LUA_ARM(&v, v.L);
    ASSERT_EQ(luaL_dostring(v.L, "compute = require('hull.compute') "
                                 "big = string.rep('x', 4 << 20)"), LUA_OK);
    v.max_instructions = 300000;
    HL_LUA_ARM(&v, v.L);
    /* 4 MB copied into the segment: 512k units. */
    int rc = luaL_dostring(v.L, "compute.segment('echo', 's', big) after = 1");
    EXPECT_NE(rc, LUA_OK);
    const char *e = lua_tostring(v.L, -1);
    EXPECT_TRUE(e && strstr(e, "instruction limit exceeded") != NULL);
    if (e && !strstr(e, "instruction limit exceeded")) printf("  got: %s\n", e);
    hl_lua_free(&v);
    hl_cap_wasm_destroy(&cache);
}

#endif /* HL_ENABLE_WASM */

/* ── Audit 23 G6: the sync http client gives its scratch back ───────── */

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* #812 missed it: a sync call's header copies (and the response's
 * lowercased names) stayed in the scratch arena for the rest of the run,
 * so a run making requests in a loop filled it - then every scratch user
 * (headers, db params, smtp) failed. */
UTEST(lua_audit23, sync_http_gives_its_scratch_back)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlHttpConfig cfg = {0};          /* no hosts: every request is refused */
    lua_rt.base.http_cfg = &cfg;
    char err[512];
    EXPECT_EQ_MSG(audit10_run(
        "local http = require('hull.http-client') "
        "local h = { ['X-Big'] = string.rep('a', 65536) } "
        "for i = 1, 400 do "
        "  local ok, e = pcall(http.get, 'http://x.invalid/', { headers = h }) "
        "  assert(not ok and not tostring(e):find('scratch'), i .. ': ' .. tostring(e)) "
        "end "
        "assert(db.query('SELECT ? AS v', { 1 })[1].v == 1) "
        "return 1",
        0, err, sizeof err), LUA_OK, err);
    lua_rt.base.http_cfg = NULL;
    cleanup_lua_caps();
}
#endif

/* ── Audit 22 G1: async input copies are held to the heap limit ──────── */

UTEST(lua_audit22, async_param_and_ctx_copies_are_held_to_the_heap_limit)
{
    LuaWorkerFix f;
    memset(&f, 0, sizeof f);
    f.be = hl_async_backend();
    ASSERT_EQ(f.be->init(&f.actx, NULL), 0);
    ASSERT_EQ(f.be->pool_create(&f.pool, f.actx, 1, 16), 0);
    pending_async_ctx   = f.actx;
    pending_thread_pool = f.pool;
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    size_t saved = lua_rt.mem_limit;
    lua_rt.mem_limit = 16u << 20;
    char out[512];
    lua_worker_run(&f,
        "local s = string.rep('x', 1 << 20)\n"
        "local t = {} for i = 1, 64 do t[i] = s end\n"
        "local ok1, e1 = pcall(db.async.exec, 'SELECT ?', t)\n"
        "local c = {} for i = 1, 32 do c['k' .. i] = s end\n"
        "local ok2, e2 = pcall(require('hull.worker').dispatch,\n"
        "                      function() return 1 end, c)\n"
        "return tostring(ok1) .. '|' .. tostring(e1) .. '|'\n"
        "    .. tostring(ok2) .. '|' .. tostring(e2)\n", out, sizeof out);
    lua_rt.mem_limit = saved;
    EXPECT_NE_MSG(strstr(out, "false|"), NULL, out);
    EXPECT_EQ_MSG(strstr(out, "true"), NULL, out);
    const char *first = strstr(out, "heap limit");
    EXPECT_NE_MSG(first, NULL, out);
    EXPECT_NE_MSG(first ? strstr(first + 1, "heap limit") : NULL, NULL, out);
    EXPECT_EQ(lua_rt.wasm_buf_bytes, (size_t)0);
    pending_async_ctx = NULL;
    cleanup_lua_caps();
    f.be->tick(f.actx, 0);
    f.be->pool_free(f.pool);
    f.be->free(f.actx);
}

/* ── Audit 23 G1: async RESULT copies are held to the heap limit ─────── */

/* A worker.dispatch return value was copied off the heap with no bound (a
 * table of keys naming one big string, once per key), and a db.async result
 * was capped per op, so the ops in flight held that cap many times over.
 * Both now take their bytes from the VM's async_result_bytes. */
static char lua_a23_worker_dsn[HL_TEST_PATH_MAX + 16];

UTEST(lua_audit23, async_result_copies_are_held_to_the_heap_limit)
{
    char dir[HL_TEST_PATH_MAX];
    ASSERT_NE(hl_test_mkdtemp(dir, sizeof dir, "hull_a23wdb"), NULL);
    snprintf(lua_a23_worker_dsn, sizeof lua_a23_worker_dsn, "%s/w.db", dir);
    hl_worker_db_init(lua_a23_worker_dsn);
    LuaWorkerFix f;
    memset(&f, 0, sizeof f);
    f.be = hl_async_backend();
    ASSERT_EQ(f.be->init(&f.actx, NULL), 0);
    ASSERT_EQ(f.be->pool_create(&f.pool, f.actx, 1, 16), 0);
    pending_async_ctx   = f.actx;
    pending_thread_pool = f.pool;
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    size_t saved = lua_rt.mem_limit;
    lua_rt.mem_limit = 16u << 20;
    char out[512];
    lua_worker_run(&f,
        "local w = require('hull.worker')\n"
        "local r = w.dispatch(function()\n"
        "  local s = string.rep('x', 1 << 20) local t = {}\n"
        "  for i = 1, 64 do t['k' .. i] = s end return t end)\n"
        "local small = w.dispatch(function() return { a = 'b' } end)\n"
        "return (type(r) == 'table' and tostring(r.error) or '?') .. '|'\n"
        "    .. tostring(small.a)\n", out, sizeof out);
    EXPECT_NE_MSG(strstr(out, "result too large"), NULL, out);
    EXPECT_NE_MSG(strstr(out, "|b"), NULL, out);
    EXPECT_EQ(lua_rt.async_result_bytes, (size_t)0);

    /* Other results in flight already hold all but 64 KiB of the limit. */
    lua_rt.async_result_bytes = (16u << 20) - (64u << 10);
    lua_worker_run(&f,
        "local ok, a, b = pcall(db.async.query,\n"
        "                       'SELECT zeroblob(262144) AS z')\n"
        "return tostring(ok) .. '|' .. tostring(a) .. '|' .. tostring(b)\n",
        out, sizeof out);
    EXPECT_NE_MSG(strstr(out, "in-flight"), NULL, out);
    EXPECT_EQ(lua_rt.async_result_bytes, (size_t)((16u << 20) - (64u << 10)));
    lua_rt.async_result_bytes = 0;
    lua_worker_run(&f,
        "local rows = db.async.query('SELECT zeroblob(262144) AS z')\n"
        "return tostring(#rows[1].z)\n", out, sizeof out);
    EXPECT_STREQ(out, "262144");
    EXPECT_EQ(lua_rt.async_result_bytes, (size_t)0);

    lua_rt.mem_limit = saved;
    pending_async_ctx = NULL;
    cleanup_lua_caps();
    f.be->tick(f.actx, 0);
    f.be->pool_free(f.pool);
    f.be->free(f.actx);
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Audit 22: http.* sent a caller Host / Transfer-Encoding / Connection
 * as given - a second Host line, or a smuggled request. */
UTEST(lua_audit22, http_refuses_headers_hulls_client_writes)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlHttpConfig cfg = {0};          /* no hosts: every request is refused */
    lua_rt.base.http_cfg = &cfg;
    char err[512];
    static const char *const cases[] = {
        "http.get('http://x.invalid/', { headers = { Host = 'other.example' } })",
        "http.post('http://x.invalid/', 'b', { headers = { ['Transfer-Encoding'] = 'chunked' } })",
        "http.request('GET', 'http://x.invalid/', { headers = { ['bad name'] = 'v' } })",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char code[512];
        snprintf(code, sizeof code,
                 "local http = require('hull.http-client') %s", cases[i]);
        EXPECT_NE_MSG(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err),
                      LUA_OK, cases[i]);
        EXPECT_NE_MSG(strstr(err, "may not be set"), NULL, cases[i]);
    }
    lua_rt.base.http_cfg = NULL;
    cleanup_lua_caps();
}
#endif

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Audit 28: the synchronous http.request / post / put / patch never charged
 * a body (http.fetch's is), so a pcall loop handed Keel a big body to copy
 * on every call, uncharged. */
UTEST(lua_audit28, sync_http_bodies_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlHttpConfig cfg = {0};          /* no hosts: every request is refused */
    lua_rt.base.http_cfg = &cfg;
    char err[512];
    static const char *const calls[] = {
        "http.post('http://x.invalid/', B)",
        "http.put('http://x.invalid/', B)",
        "http.patch('http://x.invalid/', B)",
        "http.request('POST', 'http://x.invalid/', { body = B })",
    };
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        char code[512];
        snprintf(code, sizeof code,
                 "local http = require('hull.http-client') "
                 "local B = string.rep('x', 4 * 1024 * 1024) "
                 "for _ = 1, N do pcall(function() %s end) end", calls[i]);
        char once[640], loop[640];
        snprintf(once, sizeof once, "local N = 1 %s", code);
        snprintf(loop, sizeof loop, "local N = 100 %s", code);
        EXPECT_EQ_MSG(audit10_run(once, 10000000, err, sizeof err), LUA_OK, err);
        EXPECT_NE_MSG(audit10_run(loop, 10000000, err, sizeof err), LUA_OK,
                      calls[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, err);
    }
    lua_rt.base.http_cfg = NULL;
    cleanup_lua_caps();
}
#endif

/* Audit 22: the scratch arena is reset only when a run starts, and
 * crypto.random / db params took from it for good, so one run filled it and
 * every later scratch user in that run failed. */
UTEST(lua_audit22, one_run_does_not_exhaust_the_scratch_arena)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    EXPECT_EQ_MSG(audit10_run(
        "for i = 1, 400 do assert(#crypto.random(65536) == 65536) end "
        "local sql = 'SELECT ' .. string.rep('?, ', 99) .. '? AS last' "
        "local p = {} for i = 1, 100 do p[i] = i end "
        "for i = 1, 5000 do assert(db.query(sql, p)[1].last == 100) end "
        "return 1",
        0, err, sizeof err), LUA_OK, err);
    cleanup_lua_caps();
}

/* Audit 23 L4: a db.async call refused for its off-heap copies raised with
 * its params still on the scratch arena, so a pcall loop over it filled the
 * arena for the rest of the run and every later scratch user failed. */
UTEST(lua_audit23, refused_db_async_gives_its_params_scratch_back)
{
    LuaWorkerFix f;
    memset(&f, 0, sizeof f);
    f.be = hl_async_backend();
    ASSERT_EQ(f.be->init(&f.actx, NULL), 0);
    ASSERT_EQ(f.be->pool_create(&f.pool, f.actx, 1, 16), 0);
    pending_async_ctx   = f.actx;
    pending_thread_pool = f.pool;
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    size_t saved = lua_rt.mem_limit;
    lua_rt.mem_limit = 16u << 20;
    char out[512];
    /* 32766 params a call (about 1 MB of scratch each), naming one 600-byte
     * string: ~19 MB of copies, refused. 30 calls are several times the
     * arena, and their copy charges stay under the instruction budget. */
    lua_worker_run(&f,
        "local s = string.rep('x', 600)\n"
        "local t = {} for i = 1, 32766 do t[i] = s end\n"
        "for i = 1, 30 do\n"
        "  local ok, e = pcall(db.async.exec, 'SELECT ?', t)\n"
        "  if ok or not tostring(e):find('heap limit', 1, true) then\n"
        "    return 'call ' .. i .. ': ' .. tostring(e)\n"
        "  end\n"
        "end\n"
        "assert(#crypto.random(65536) == 65536)\n"
        "local sql = 'SELECT ' .. string.rep('?, ', 99) .. '? AS last'\n"
        "local p = {} for i = 1, 100 do p[i] = i end\n"
        "return tostring(db.query(sql, p)[1].last)\n", out, sizeof out);
    lua_rt.mem_limit = saved;
    EXPECT_STREQ(out, "100");
    EXPECT_EQ(lua_rt.wasm_buf_bytes, (size_t)0);
    pending_async_ctx = NULL;
    cleanup_lua_caps();
    f.be->tick(f.actx, 0);
    f.be->pool_free(f.pool);
    f.be->free(f.actx);
}

/* Audit 24 R2: db.upsert / insert_if_absent refused (bad arrays, a length
 * mismatch, a reserved identifier) raised with their scratch arrays held,
 * so a pcall loop over a refused call filled the arena for the rest of the
 * run and every later scratch user failed. */
UTEST(lua_audit24, refused_db_helpers_give_their_scratch_back)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    /* 32766 columns a call: ~256 KB of names, ~768 KB of values. 200 calls
     * of each kind are many times the arena. */
    EXPECT_EQ_MSG(audit10_run(
        "db.exec('CREATE TABLE a24 (id INTEGER PRIMARY KEY, v TEXT)') "
        "local cols, vals = {}, {} "
        "for i = 1, 32766 do cols[i] = 'c' .. i; vals[i] = i end "
        "local bad = {} for i = 1, 32765 do bad[i] = 'c' .. i end bad[32766] = 1 "
        "local res = {} for i = 1, 32766 do res[i] = '_hull_x' end "
        "for i = 1, 200 do "
        "  assert(not pcall(db.upsert, 'a24', {'id'}, cols, {1}))\n"
        "  assert(not pcall(db.insert_if_absent, 'a24', nil, cols, {1}))\n"
        "  assert(not pcall(db.upsert, 'a24', cols, bad, vals))\n"
        "  assert(not pcall(db.upsert, 'a24', {'id'}, res, vals))\n"
        "  assert(not pcall(db.insert_if_absent, 'a24', cols, res, vals))\n"
        "  assert(not pcall(db.upsert, 'a24', {'id'}, cols, {{}}))\n"
        "end "
        "assert(#crypto.random(65536) == 65536) "
        "local sql = 'SELECT ' .. string.rep('?, ', 99) .. '? AS last' "
        "local p = {} for i = 1, 100 do p[i] = i end "
        "assert(db.query(sql, p)[1].last == 100) "
        "assert(db.upsert('a24', {'id'}, {'id', 'v'}, {1, 'x'}) == 1) "
        "assert(db.query('SELECT v FROM a24 WHERE id = 1')[1].v == 'x') "
        "local ok, e = pcall(db.upsert, 'a24', {'id'}, {'id'}, {1, 2}) "
        "assert(not ok and tostring(e):find('length mismatch', 1, true), e) "
        "ok, e = pcall(db.upsert, '_hull_x', {'id'}, {'id'}, {1}) "
        "assert(not ok and tostring(e):find('_hull_', 1, true), e) "
        "ok, e = pcall(db.upsert, 'a24', {}, {'id'}, {1}) "
        "assert(not ok and tostring(e):find('non-empty', 1, true), e) "
        "return 1",
        0, err, sizeof err), LUA_OK, err);
    cleanup_lua_caps();
}

/* Audit 24 H1: the header copy counted the table, then converted numeric
 * values in place - an allocation that could run a __gc finalizer adding
 * keys to the same table, so the walk wrote past the array it had sized.
 * Numbers are formatted without touching the Lua heap now, and the walk is
 * bounded by its count. */
UTEST(lua_audit24, http_header_numbers_do_not_run_app_code)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlHttpConfig cfg = {0};          /* no hosts: every request is refused */
    lua_rt.base.http_cfg = &cfg;
    char err[512];
    EXPECT_NE(audit10_run(
        "local http = require('hull.http-client') "
        "local h = {} "
        "for i = 1, 64 do h['X-N' .. i] = i + 0.5 end "
        "for i = 1, 64 do h['X-I' .. i] = i end "
        "local n = 0 "
        "for i = 1, 2000 do "
        "  setmetatable({}, { __gc = function() "
        "    n = n + 1 h['X-G' .. n] = n end }) "
        "end "
        "collectgarbage('incremental', 1, 1000) "
        "http.get('http://x.invalid/', { headers = h })",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* Refused for the host, not the headers. */
    EXPECT_EQ_MSG(strstr(err, "invalid headers"), NULL, err);
    (void)luaL_dostring(lua_rt.L, "collectgarbage('incremental', 200, 100)");
    lua_settop(lua_rt.L, 0);
    lua_rt.base.http_cfg = NULL;
    cleanup_lua_caps();
}

/* Audit 24 R4: log.* passed CR / LF through, so a message could write a log
 * line of its own (a forged record). They go out as \r / \n now. */
static char g_a24_log[2048];
static void a24_log_collect(log_Event *ev)
{
    if (!ev->fmt)
        return;
    char line[1024];
    vsnprintf(line, sizeof line, ev->fmt, ev->ap);
    if (!strstr(line, "a24mark"))
        return;
    size_t n = strlen(g_a24_log);
    snprintf(g_a24_log + n, sizeof g_a24_log - n, "%s|", line);
}

UTEST(lua_audit24, log_escapes_cr_lf)
{
    static int registered;
    if (!registered) {
        log_add_callback(a24_log_collect, NULL, LOG_TRACE);
        registered = 1;
    }
    g_a24_log[0] = '\0';
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    EXPECT_EQ_MSG(audit10_run(
        "local log = require('hull.log') "
        "log.info('a24mark one\\n[hull:lua] forged\\r\\nend') "
        "log.warn('a24mark', 42) "
        "return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK, err);
    EXPECT_EQ_MSG(strchr(g_a24_log, '\n'), NULL, g_a24_log);
    EXPECT_EQ_MSG(strchr(g_a24_log, '\r'), NULL, g_a24_log);
    EXPECT_NE_MSG(strstr(g_a24_log,
                         "a24mark one\\n[hull:lua] forged\\r\\nend"), NULL,
                  g_a24_log);
    EXPECT_NE_MSG(strstr(g_a24_log, "a24mark|"), NULL, g_a24_log);
    /* A long message is charged: 64 KiB logged 64 times is 2^19 units, past
     * a 100000-instruction limit. */
    log_set_quiet(true);
    EXPECT_NE(audit10_run(
        "local log = require('hull.log') "
        "local s = string.rep('x', 1 << 16) "
        "for i = 1, 64 do log.debug(s) end return 1",
        100000, err, sizeof err), LUA_OK);
    log_set_quiet(false);
    cleanup_lua_caps();
}

#if defined(HL_ENABLE_HTTP_CLIENT) && defined(HL_ENABLE_HTTP_SERVER)
/* Audit 24: ws.connect passed Keel no config, so every wss:// failed with a
 * generic error. It takes the client TLS config http.fetch uses now, and with
 * none says so up front (the allowlist still runs first). */
UTEST(lua_audit24, wss_connect_needs_the_client_tls_config)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    static const char *hosts[] = { "ws.example" };
    HlHttpConfig cfg = {0};
    cfg.allowed_hosts = hosts;
    cfg.count = 1;
    lua_rt.base.http_cfg = &cfg;
    char err[512];
    const char *pre = "local ws = require('hull.web.ws-client') ";
    char code[512];
    snprintf(code, sizeof code, "%sws.connect('wss://ws.example/x', {}) return 1", pre);
    EXPECT_NE(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE_MSG(strstr(err, "wss:// needs TLS"), NULL, err);
    snprintf(code, sizeof code, "%sws.connect('wss://other.example/x', {}) return 1", pre);
    EXPECT_NE(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE_MSG(strstr(err, "not in allowlist"), NULL, err);
    /* ws:// needs no TLS: it gets past both checks to the server one. */
    snprintf(code, sizeof code, "%sws.connect('ws://ws.example/x', {}) return 1", pre);
    EXPECT_NE(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE_MSG(strstr(err, "requires running server"), NULL, err);
    lua_rt.base.http_cfg = NULL;
    cleanup_lua_caps();
}
#endif

#ifdef HL_ENABLE_IMAGE
/* Audit 25 L10: image.new / from_wasm / from_buffer's copy branch copied
 * w * h * bpp bytes through the raw VM allocator uncharged; decode, encode
 * and to_wasm were charged before the work. */
UTEST(lua_audit25, image_copies_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    ASSERT_EQ(luaL_dostring(lua_rt.L,
        "px = string.rep('a', 4 << 20) "
        "wasm = string.pack('<I4I4B', 1024, 1024, 1) .. px "
        "return image ~= nil"), LUA_OK);
    int have = lua_toboolean(lua_rt.L, -1);
    lua_settop(lua_rt.L, 0);
    if (!have) {   /* image compiled out */
        cleanup_lua_caps();
        return;
    }
    /* 4 MB copied: 512k units each, over a 300k limit. */
    static const char *const calls[] = {
        "image.new(1024, 1024, 'rgba8', px) return 1",
        "image.from_buffer(px, 1024, 1024, 'rgba8') return 1",
        "image.from_wasm(wasm) return 1",
        NULL
    };
    for (int i = 0; calls[i]; i++) {
        EXPECT_NE_MSG(audit10_run(calls[i], 300000, err, sizeof err), LUA_OK,
                      calls[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit exceeded"), NULL, err);
    }
    /* Small copies still work under that limit. */
    EXPECT_EQ_MSG(audit10_run(
        "local im = image.new(16, 16, 'rgba8', string.rep('b', 1024)) "
        "assert(im:width() == 16) return 1", 300000, err, sizeof err),
        LUA_OK, err);
    cleanup_lua_caps();
}
#endif /* HL_ENABLE_IMAGE */

#ifdef HL_ENABLE_HTTP_CLIENT
/* Audit 25 L11: a refused http.fetch (here: inside a transaction) and a
 * failed smtp.send kept their header / cc copies in the scratch arena, which
 * is reset only when a run starts, so a pcall'd loop of failures filled it
 * and every later scratch user of the run failed. */
UTEST(lua_audit25, refused_fetch_gives_its_scratch_back)
{
    LuaWorkerFix f;
    memset(&f, 0, sizeof f);
    f.be = hl_async_backend();
    ASSERT_EQ(f.be->init(&f.actx, NULL), 0);
    ASSERT_EQ(f.be->pool_create(&f.pool, f.actx, 1, 16), 0);
    pending_async_ctx   = f.actx;
    pending_thread_pool = f.pool;
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlHttpConfig cfg = {0};
    lua_rt.base.http_cfg = &cfg;
    char out[512];
    /* 400 x 64 KB of header copies: many times the arena. */
    lua_worker_run(&f,
        "local http = require('hull.http-client')\n"
        "local h = { ['X-Big'] = string.rep('a', 65536) }\n"
        "db.exec('BEGIN')\n"
        "for i = 1, 400 do\n"
        "  local ok, e = pcall(http.async.request, 'GET', 'http://x.invalid/',\n"
        "                      { headers = h })\n"
        "  if ok or not tostring(e):find('transaction') then\n"
        "    db.exec('ROLLBACK') return 'call ' .. i .. ': ' .. tostring(e)\n"
        "  end\n"
        "end\n"
        "db.exec('ROLLBACK')\n"
        "local sql = 'SELECT ' .. string.rep('?, ', 99) .. '? AS last'\n"
        "local p = {} for i = 1, 100 do p[i] = i end\n"
        "return tostring(db.query(sql, p)[1].last)\n", out, sizeof out);
    EXPECT_STREQ(out, "100");
    lua_rt.base.http_cfg = NULL;
    pending_async_ctx = NULL;
    cleanup_lua_caps();
    f.be->tick(f.actx, 0);
    f.be->pool_free(f.pool);
    f.be->free(f.actx);
}

UTEST(lua_audit25, failed_smtp_gives_its_scratch_back)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlSmtpConfig smtp = {0};          /* no hosts: every send is refused */
    lua_rt.base.smtp_cfg = &smtp;
    char err[512];
    /* 1000 x 1 KB cc entries a call (~1 MB of scratch), 60 calls of each
     * failure: a missing field, a refused host, a bad cc entry. */
    EXPECT_EQ_MSG(audit10_run(
        "local cc = {} for i = 1, 1000 do cc[i] = string.rep('c', 1024) end "
        "local bad = {} for i = 1, 999 do bad[i] = cc[i] end bad[1000] = 1 "
        "for i = 1, 60 do "
        "  local r = smtp.send({ from = 'a@x.test', to = 'b@x.test', "
        "                        subject = 's', body = 'b', cc = cc }) "
        "  assert(r.ok == false and r.error == 'host required', tostring(r.error)) "
        "  r = smtp.send({ host = 'mail.invalid', from = 'a@x.test', "
        "                  to = 'b@x.test', subject = 's', body = 'b', cc = cc }) "
        "  assert(r.ok == false and not tostring(r.error):find('scratch'), "
        "         tostring(r.error)) "
        "  local ok, e = pcall(smtp.send, { host = 'mail.invalid', "
        "      from = 'a@x.test', to = 'b@x.test', subject = 's', body = 'b', "
        "      cc = bad }) "
        "  assert(not ok and tostring(e):find('must be a string'), tostring(e)) "
        "end "
        "assert(db.query('SELECT ? AS v', { 1 })[1].v == 1) "
        "return 1",
        0, err, sizeof err), LUA_OK, err);
    lua_rt.base.smtp_cfg = NULL;
    cleanup_lua_caps();
}
#endif /* HL_ENABLE_HTTP_CLIENT */

/* ── Audit 27: the Lua runtime ────────────────────────────────────────── */

/* M1: HMAC charged only its data; a key over the 64-byte block is hashed in
 * full first, so `hmac("", big_key)` in a loop was free. */
UTEST(lua_audit27, hmac_key_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    ASSERT_EQ(audit10_run("BIGK = string.rep('k', 8 * 1000000) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    static const char *const loops[] = {
        "for i = 1, 10 do crypto.hmac_sha256('', BIGK) end return 1",
        "for i = 1, 10 do crypto.hmac_sha1('', BIGK) end return 1",
        "for i = 1, 10 do crypto.hmac_sha256_verify('', BIGK, string.rep('x', 32)) end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 200000, err, sizeof err), LUA_OK, loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    /* A short key stays cheap. */
    EXPECT_EQ(audit10_run("for i = 1, 100 do crypto.hmac_sha256('d', 'key') end return 1",
                          200000, err, sizeof err), LUA_OK);
    audit10_run("BIGK = nil return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err);
    cleanup_lua_caps();
}

/* L: a table / function / userdata param bound NULL, silently. Refused now,
 * with the params' scratch given back first (a pcall loop over a refused
 * call must not fill the arena), and a query with no params still works
 * (the result table and guard are made before the params now). */
UTEST(lua_audit27, db_params_refuse_unsupported_types)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    EXPECT_EQ_MSG(audit10_run(
        "local ok, e = pcall(db.query, 'SELECT ? AS v', { {} }) "
        "assert(not ok and tostring(e):find(\"unsupported value type 'table'\", 1, true), tostring(e)) "
        "ok, e = pcall(db.exec, 'SELECT ?', { print }) "
        "assert(not ok and tostring(e):find(\"unsupported value type 'function'\", 1, true), tostring(e)) "
        "local p = {} for i = 1, 32765 do p[i] = i end p[32766] = {} "
        "local sql = 'SELECT ' .. string.rep('?, ', 32765) .. '?' "
        "for i = 1, 200 do assert(not pcall(db.query, sql, p)) "
        "                  assert(not pcall(db.exec, sql, p)) end "
        "assert(db.query('SELECT 7 AS v')[1].v == 7) "
        "assert(db.query('SELECT ? AS v', nil) ~= nil) "
        "assert(db.query('SELECT ? AS v', { 'x' })[1].v == 'x') "
        "db.exec('CREATE TABLE a27 (x)') "
        "assert(db.exec('INSERT INTO a27 VALUES (?)', { 3 }) == 1) "
        "assert(db.exec('DELETE FROM a27') == 1) "
        "local q = {} for i = 1, 100 do q[i] = i end "
        "local s2 = 'SELECT ' .. string.rep('?, ', 99) .. '? AS last' "
        "assert(db.query(s2, q)[1].last == 100) "
        "return 1", 0, err, sizeof err), LUA_OK, err);
    cleanup_lua_caps();
}

/* L: a Lua UDF that returned a table wrote NULL. A SQL error now. */
UTEST(lua_audit27, udf_unsupported_result_is_an_error)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    EXPECT_EQ_MSG(audit10_run(
        "if not db or not db.udf then return 1 end "
        "db.udf.register('hull_a27_tbl', function() return {} end) "
        "db.udf.register('hull_a27_nil', function() return nil end) "
        "local ok, e = pcall(db.query, 'SELECT hull_a27_tbl() AS v') "
        "assert(not ok and tostring(e):find('unsupported return type', 1, true), tostring(e)) "
        "assert(db.query('SELECT hull_a27_nil() AS v')[1].v == nil) "
        "return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK, err);
    cleanup_lua_caps();
}

/* L: tar.create wrote an empty member for non-string data. A data value
 * that is neither a string nor a buffer is refused now. */
UTEST(lua_audit27, tar_create_refuses_unsupported_data)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    EXPECT_EQ_MSG(audit10_run(
        "local tar = require('hull.archive.tar') "
        "local b, msg = tar.create({ { name = 'a', data = {} } }) "
        "assert(b == nil and tostring(msg):find('string or a buffer', 1, true), tostring(msg)) "
        "b = tar.create({ { name = 'a', data = 42 }, { name = 'e' } }) "
        "local e = tar.parse(b) "
        "assert(e[1].data == '42' and e[2].data == '', tostring(e[1].data)) "
        "return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK, err);
    cleanup_lua_caps();
}

#ifdef HL_ENABLE_IMAGE
/* L: img:pixels() copied the pixels onto the heap uncharged, with no
 * heap-limit refusal; JS has both. */
UTEST(lua_audit27, image_pixels_copy_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    ASSERT_EQ(audit10_run("if not image then return 1 end "
                          "IMG = image.new(1024, 1024, 'r8', string.rep('a', 1 << 20)) "
                          "return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* 64 copies of 1 MB: 8M units, over a 200k limit (unfixed, each cost a
     * unit per 64 bytes through the allocator: 1M in all). */
    EXPECT_NE(audit10_run("if not IMG then error('instruction limit (no image)') end "
                          "for i = 1, 64 do local p = IMG:pixels() end return 1",
                          200000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* One copy fits a default budget. (The heap-limit refusal is for an
     * image borrowed off-heap - from_buffer over a mapping - larger than
     * the whole heap; a heap-backed image can never trip it.) */
    EXPECT_EQ(audit10_run("if IMG then assert(#IMG:pixels() == 1 << 20) end return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    audit10_run("IMG = nil return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err);
    cleanup_lua_caps();
}
#endif /* HL_ENABLE_IMAGE */

#ifdef HL_ENABLE_WASM
/* L: sync compute.call / inst:call charged neither input nor output. 1 MB
 * echoed 20 times is 5M units now (input + output at 1 per 8 bytes), over a
 * 1M limit; unfixed only the result string's allocation was charged (~330k).
 * A callback error from compute.stream is the app's own error now, not
 * "callback_failed". */
UTEST(lua_audit27, sync_compute_call_is_charged)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, a15_wasm_entries, NULL);
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    HlLua v;
    memset(&v, 0, sizeof v);
    v.base.wasm_cache = &cache;
    v.base.app_vfs = &vfs;
    ASSERT_EQ(hl_lua_init(&v, &cfg), 0);
    HL_LUA_ARM(&v, v.L);
    ASSERT_EQ(luaL_dostring(v.L, "compute = require('hull.compute') "
                                 "ab = string.rep('a', 1000000)"), LUA_OK);
    static const char *const loops[] = {
        "for i = 1, 20 do local s = assert(compute.call('echo', ab)) end",
        "local inst = compute.instance('echo') "
        "for i = 1, 20 do local s = assert(inst:call(ab)) end",
    };
    int64_t saved = v.max_instructions;
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        v.max_instructions = 1000000;
        HL_LUA_ARM(&v, v.L);
        int rc = luaL_dostring(v.L, loops[i]);
        EXPECT_NE_MSG(rc, LUA_OK, loops[i]);
        const char *e = lua_tostring(v.L, -1);
        EXPECT_TRUE_MSG(e && strstr(e, "instruction limit") != NULL, e ? e : loops[i]);
        lua_settop(v.L, 0);
    }
    v.max_instructions = saved;
    HL_LUA_ARM(&v, v.L);
    int rc = luaL_dostring(v.L,
        "assert(compute.call('echo', ab) == ab) "
        "local ok, e = pcall(compute.stream, 'echo', 'hello', "
        "                    function() error({ code = 42 }) end) "
        "assert(not ok and type(e) == 'table' and e.code == 42, tostring(e)) "
        "ok, e = pcall(compute.stream, 'echo', 'hello', function() error('boom27') end) "
        "assert(not ok and tostring(e):find('boom27', 1, true), tostring(e))");
    EXPECT_EQ_MSG(rc, LUA_OK, lua_tostring(v.L, -1));
    lua_settop(v.L, 0);

    hl_lua_free(&v);
    hl_cap_wasm_destroy(&cache);
}
#endif /* HL_ENABLE_WASM */

/* ── gpu.* over an in-memory backend ─────────────────────────────────────
 *
 * The bindings are in the base build; only the wgpu backend is a feature.
 * tests/hull/gpu_mock.h stands in for it, so these run on every runner: a
 * two-device context, buffers and textures that keep their bytes, and an
 * identity "shader" whose output is the buffer it names. */

#include "../../gpu_mock.h"

static HlGpuCtx *lgpu;

static int lgpu_init(int devices)
{
    cleanup_lua();
    hl_platform_vfs_dispose(platform_vfs_owned);
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    lgpu = gpu_mock_ctx(devices);
    if (!lgpu) return -1;
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    memset(&lua_rt, 0, sizeof(lua_rt));
    lua_rt.base.platform_vfs = &platform_vfs;
    lua_rt.base.gpu_ctx = lgpu;
    lua_initialized = (hl_lua_init(&lua_rt, &cfg) == 0);
    if (!lua_initialized) return -1;
    install_test_globals(lua_rt.L);
    return 0;
}

static void lgpu_cleanup(void)
{
    cleanup_lua();
    gpu_mock_free(lgpu);
    lgpu = NULL;
}

/* Runs `src`, which sets the global R; returns a copy of R as a string
 * ("ok" by convention), or the error. Caller frees. */
static char *lgpu_run(const char *src)
{
    HL_LUA_ARM(&lua_rt, lua_rt.L);
    char *out;
    if (luaL_dostring(lua_rt.L, src) != LUA_OK) {
        const char *e = lua_tostring(lua_rt.L, -1);
        out = strdup(e ? e : "(error)");
    } else {
        lua_getglobal(lua_rt.L, "R");
        const char *r = lua_tostring(lua_rt.L, -1);
        out = strdup(r ? r : "(R unset)");
    }
    lua_settop(lua_rt.L, 0);
    return out;
}

#define LGPU_EXPECT(src, want) do {                                        \
        char *got_ = lgpu_run(src);                                        \
        EXPECT_STREQ(got_, want);                                          \
        free(got_);                                                        \
    } while (0)

static int lgpu_has(int device, const char *kind, const char *name)
{
    HlGpuDevice *d = &lgpu->devices[device];
    if (strcmp(kind, "buffer") == 0) {
        for (int i = 0; i < d->buffer_count; i++)
            if (strcmp(d->buffers[i].name, name) == 0) return 1;
    } else {
        for (int i = 0; i < d->texture_count; i++)
            if (strcmp(d->textures[i].name, name) == 0) return 1;
    }
    return 0;
}

/* The test shader declares the storage bindings the dispatches below make:
 * every binding is checked against the shader's declarations (audit 35). */
#define HL_TEST_GPU_WGSL \
    "@group(0) @binding(0) var<storage, read_write> a: array<u32>; " \
    "@group(0) @binding(1) var<storage, read_write> b: array<u32>; " \
    "@group(0) @binding(2) var<storage, read_write> c: array<u32>; " \
    "@group(0) @binding(3) var<storage, read_write> d: array<u32>; " \
    "fn main() {}"

UTEST(lua_gpu, devices_are_listed)
{
    ASSERT_EQ(lgpu_init(2), 0);
    LGPU_EXPECT("local d = gpu.devices() "
                "R = tostring(gpu.available()) .. ' ' .. #d .. ' ' .. "
                "d[1].id .. d[1].name .. ' ' .. d[2].id .. d[2].name",
                "true 2 0mock0 1mock1");
    lgpu_cleanup();
}

UTEST(lua_gpu, buffers_are_per_device)
{
    ASSERT_EQ(lgpu_init(2), 0);
    LGPU_EXPECT("gpu.buffer('b', 'AAAAAAAA') "
                "gpu.buffer('b', 'BBBBBBBB', { device = 1 }) "
                "R = gpu.buffer_read('b') .. gpu.buffer_read('b', { device = 1 })",
                "AAAAAAAABBBBBBBB");
    /* A write to an existing buffer lands at its offset - whole 4-byte words
     * at a 4-aligned offset, as WebGPU writes; anything else is refused
     * rather than logged and dropped. */
    LGPU_EXPECT("gpu.buffer('b', 'wxyz', { device = 1, offset = 4 }) "
                "local ok, e = gpu.buffer('b', 'q', { device = 1, offset = 1 }) "
                "R = gpu.buffer_read('b', { device = 1 }) .. ' ' .. tostring(ok) "
                "  .. ' ' .. tostring(e)",
                "BBBBwxyz nil buffer_error");
    EXPECT_EQ(lgpu->devices[0].live_bytes, (size_t)8);
    EXPECT_EQ(lgpu->devices[1].live_bytes, (size_t)8);
    lgpu_cleanup();
}

/* gpu.texture(name, nil, { device = N }) destroyed the texture of that name
 * on the DEFAULT device: the destroy branch ran before opts.device was read
 * (#870). gpu.buffer's destroy reads it first. */
UTEST(lua_gpu, a_destroy_targets_the_device_it_names)
{
    ASSERT_EQ(lgpu_init(2), 0);
    LGPU_EXPECT("local px = string.rep('\\1', 16) "
                "for d = 0, 1 do "
                "  gpu.buffer('b', 'data', { device = d }) "
                "  gpu.texture('t', px, { width = 2, height = 2, device = d }) "
                "end "
                "gpu.buffer('b', nil, { device = 1 }) "
                "gpu.texture('t', nil, { device = 1 }) "
                "gpu.texture('t', nil, { device = 7 }) "   /* no such device */
                "R = 'ok'",
                "ok");
    EXPECT_TRUE(lgpu_has(0, "buffer", "b"));
    EXPECT_FALSE(lgpu_has(1, "buffer", "b"));
    EXPECT_TRUE(lgpu_has(0, "texture", "t"));
    EXPECT_FALSE(lgpu_has(1, "texture", "t"));
    EXPECT_EQ(lgpu->devices[1].live_bytes, (size_t)0);
    EXPECT_EQ(lgpu->devices[0].live_bytes, (size_t)(4 + 16));
    LGPU_EXPECT("gpu.buffer('b', nil) gpu.texture('t', nil) R = 'ok'", "ok");
    EXPECT_EQ(lgpu->devices[0].live_bytes, (size_t)0);
    EXPECT_EQ(lgpu->devices[0].buffer_count + lgpu->devices[0].texture_count, 0);
    lgpu_cleanup();
}

UTEST(lua_gpu, an_unknown_device_is_refused)
{
    ASSERT_EQ(lgpu_init(1), 0);
    LGPU_EXPECT("local ok = gpu.buffer('b', 'x', { device = 3 }) "
                "local r, e = gpu.buffer_read('b', { device = 3 }) "
                "R = tostring(ok) .. ' ' .. tostring(r) .. ' ' .. tostring(e)",
                "nil nil buffer_read_error");
    EXPECT_EQ(gpu_mock_buffer_creates, 0);
    lgpu_cleanup();
}

UTEST(lua_gpu, dispatch_reads_back_the_buffer_it_names)
{
    ASSERT_EQ(lgpu_init(1), 0);
    LGPU_EXPECT("assert(gpu.compile('id', '" HL_TEST_GPU_WGSL "')) "
                "local a = gpu.dispatch('id', { buffers = {"
                "  { data = 'hello' }, { size = 3 } } }) "        /* default: 1 */
                "local b = gpu.dispatch('id', { buffers = {"
                "  { data = 'hello' }, { size = 3 } }, output = 2 }) "
                "local c = gpu.dispatch('id', { buffers = {"
                "  { data = 'hello' } }, output = false }) "
                "R = a .. ' ' .. #b .. ' ' .. (b == '\\0\\0\\0' and 'z' or '?') "
                "  .. ' ' .. tostring(c)",
                "hello 3 z true");
    /* An uncompiled shader never reaches the backend. */
    int before = gpu_mock_dispatches;
    LGPU_EXPECT("local r, e = gpu.dispatch('nope', { buffers = {"
                "  { data = 'x' } } }) R = tostring(r) .. ' ' .. tostring(e)",
                "nil shader_not_found");
    EXPECT_EQ(gpu_mock_dispatches, before);
    lgpu_cleanup();
}

/* The twin of js_gpu.compile_and_reads_take_the_device: compile, buffer_read
 * and texture_read act on the device the opts name. */
UTEST(lua_gpu, compile_and_reads_take_the_device)
{
    ASSERT_EQ(lgpu_init(2), 0);
    LGPU_EXPECT("assert(gpu.compile('id1', '" HL_TEST_GPU_WGSL "', { device = 1 })) "
                "local r = gpu.dispatch('id1', { device = 1, buffers = { { data = 'one' } } }) "
                "local _, e = gpu.dispatch('id1', { buffers = { { data = 'x' } } }) "
                "gpu.buffer('b', 'zero') gpu.buffer('b', 'one!', { device = 1 }) "
                "R = r .. ' ' .. tostring(e) .. ' ' .. gpu.buffer_read('b', { device = 1 })",
                "one shader_not_found one!");
#ifdef HL_ENABLE_IMAGE
    LGPU_EXPECT("gpu.texture('t', string.rep('\\0', 16), { width = 2, height = 2 }) "
                "gpu.texture('t', string.rep('\\0', 16), { width = 4, height = 1, device = 1 }) "
                "R = gpu.texture_read('t'):width() .. ' ' .. "
                "  gpu.texture_read('t', { device = 1 }):width()",
                "2 4");
#endif
    lgpu_cleanup();
}

/* gpu.load compiled on the default device whatever the opts said: a shader
 * from shaders/ could not be dispatched on another device. */
static const char lgpu_wgsl[] = HL_TEST_GPU_WGSL;
static const HlEntry lgpu_shader_entries[] = {
    { "shaders/id.wgsl", (const unsigned char *)lgpu_wgsl, sizeof lgpu_wgsl - 1 },
    { 0, 0, 0 }
};

UTEST(lua_gpu, load_takes_the_device)
{
    ASSERT_EQ(lgpu_init(2), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, lgpu_shader_entries, NULL);
    lua_rt.base.app_vfs = &vfs;
    LGPU_EXPECT("assert(gpu.load('id', { device = 1 })) "
                "local r = gpu.dispatch('id', { device = 1, buffers = { { data = 'one' } } }) "
                "local _, e = gpu.dispatch('id', { buffers = { { data = 'x' } } }) "
                "assert(gpu.load('id')) "
                "R = r .. ' ' .. tostring(e) .. ' ' .. "
                "  gpu.dispatch('id', { buffers = { { data = 'zero' } } })",
                "one shader_not_found zero");
    lua_rt.base.app_vfs = NULL;
    lgpu_cleanup();
}

UTEST(lua_gpu, a_pipeline_with_several_outputs_returns_a_list)
{
    ASSERT_EQ(lgpu_init(1), 0);
    LGPU_EXPECT("assert(gpu.compile('id', '" HL_TEST_GPU_WGSL "')) "
                "local stages = {"
                "  { shader = 'id', buffers = { { name = 'a', data = 'xy' } } },"
                "  { shader = 'id', buffers = { { name = 'a' }, { data = 'zz' } } } } "
                "local one = gpu.pipeline(stages) "
                "local two = gpu.pipeline(stages, { outputs = {"
                "  { stage = 1, buffer = 1 }, { stage = 2, buffer = 2 } } }) "
                "R = type(one) .. ':' .. one .. ' ' .. type(two) .. ':' .. "
                "  #two .. ':' .. two[1] .. two[2]",
                "string:xy table:2:xyzz");
    lgpu_cleanup();
}

/* A read-back larger than the VM heap limit - one the heap could never
 * hold - is refused before the backend copies it off the heap (audit 17),
 * in buffer_read and in dispatch. The limit is narrowed below the 8 MB
 * buffer once the string that filled it is collected. */
UTEST(lua_gpu, read_backs_past_the_heap_limit_are_refused)
{
    ASSERT_EQ(lgpu_init(1), 0);
    LGPU_EXPECT("gpu.buffer('big', string.rep('x', 8 << 20)) "
                "assert(gpu.compile('id', '" HL_TEST_GPU_WGSL "')) "
                "collectgarbage() R = 'ok'", "ok");
    size_t saved = lua_rt.mem_limit;
    lua_rt.mem_limit = lua_rt.mem_used + (1u << 20);
    ASSERT_LT(lua_rt.mem_limit, (size_t)8 << 20);
    LGPU_EXPECT("local r, e = gpu.buffer_read('big') "
                "local d, de = gpu.dispatch('id', { buffers = { { name = 'big' } } }) "
                "R = tostring(r) .. ' ' .. tostring(e) .. ' ' .. tostring(d) .. ' ' "
                "  .. tostring(de)",
                "nil buffer_read_error nil output_too_large");
    lua_rt.mem_limit = saved;
    LGPU_EXPECT("R = #gpu.buffer_read('big')", "8388608");
    lgpu_cleanup();
}

/* ── Audit 37: fs.list, fs.stat and small fs / blob writes are charged ─
 *
 * fs.list read a whole directory (every entry, matching the grant's pattern
 * or not) and fstatat()ed each one it kept, uncharged; an empty fs.write /
 * blob.put (a temp file, an fsync, a rename) cost 0 units; fs.stat,
 * blob.exists / size / delete and blob.init (three mkdir_p and a tmp/ sweep)
 * nothing. Each is charged now (hull/shared/work_charge.h): a loop of them
 * trips the budget, a single call of each still fits easily. */

#define A37_FILES 3000

/* A temp app root whose policy reads @p read and writes "." , with
 * A37_FILES empty files in many/, wired into the caps runtime. */
static int a37_fs_open(A15Fs *f, const char *read)
{
    static const char *const all[] = { "." };
    const char *reads[1] = { read };
    memset(f, 0, sizeof *f);
    if (!hl_test_mkdtemp(f->dir, sizeof f->dir, "hull_a37")) return -1;
    hl_alloc_init(&f->alloc, 0);
    f->policy = HL_FS_POLICY_INIT;
    char p[HL_TEST_PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/many", f->dir);
    if (mkdir(p, 0755) != 0) return -1;
    for (int i = 0; i < A37_FILES; i++) {
        snprintf(p, sizeof p, "%s/many/f%05d.txt", f->dir, i);
        FILE *fp = fopen(p, "wb");
        if (!fp) return -1;
        fclose(fp);
    }
    const char *perr = NULL;
    if (hl_fs_policy_compile_manifest(f->dir, &f->alloc, reads, 1, all, 1,
                                      &f->policy, &perr) != 0)
        return -1;
    f->fs.base_dir = f->dir;
    f->fs.base_len = strlen(f->dir);
    f->fs.policy = &f->policy;
    lua_rt.base.fs_cfg = &f->fs;
    return 0;
}

UTEST(lua_audit37, fs_list_is_charged_per_entry)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a37_fs_open(&f, "."), 0);
    char err[512];
    /* 3000 entries kept: ~1M units a listing. */
    EXPECT_NE(audit10_run("for i = 1, 100 do fs.list('many') end return 1",
                          5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* The trip is not an error pcall can catch and loop past. */
    EXPECT_NE(audit10_run("for i = 1, 100 do pcall(fs.list, 'many') end return 1",
                          5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run("assert(#fs.list('many') == 3000) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit37, fs_list_charges_entries_the_pattern_drops)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a37_fs_open(&f, "many/*.log"), 0);
    char err[512];
    /* Nothing matches, yet every listing reads 3000 entries (~190k units). */
    EXPECT_EQ(audit10_run("assert(#fs.list('many') == 0) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE(audit10_run("for i = 1, 100 do fs.list('many') end return 1",
                          5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit37, small_fs_calls_have_a_floor)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a37_fs_open(&f, "."), 0);
    char err[512];
    static const char *const loops[] = {
        "for i = 1, 1000 do fs.write('e.txt', '') end return 1",
        "for i = 1, 100000 do fs.stat('many/f00001.txt') end return 1",
        "fs.write('m.bin', 'abcd') for i = 1, 10000 do fs.mmap('m.bin'):close() end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 5000000, err, sizeof err), LUA_OK,
                      loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    /* A few of each fit in a small budget. */
    EXPECT_EQ(audit10_run(
        "for i = 1, 10 do assert(fs.write('e.txt', '')) end "
        "assert(fs.stat('e.txt').size == 0) return 1",
        1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit37, small_blob_calls_have_a_floor)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a37_fs_open(&f, "."), 0);
    char err[512];
    ASSERT_EQ(audit10_run(
        "blob = require('hull.blob') blob.init({ dir = 'blobs' }) "
        "ID = blob.put('x') return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    static const char *const loops[] = {
        "for i = 1, 1000 do blob.put('') end return 1",
        "for i = 1, 10000 do blob.exists(ID) end return 1",
        "for i = 1, 10000 do blob.size(ID) end return 1",
        "for i = 1, 10000 do blob.delete(string.rep('0', 64)) end return 1",
        "for i = 1, 1000 do blob.init({ dir = 'blobs' }) end return 1",
        "for i = 1, 1000 do local w = blob.writer() w:finalize() end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 5000000, err, sizeof err), LUA_OK,
                      loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    EXPECT_EQ(audit10_run(
        "blob.init({ dir = 'blobs' }) local id = blob.put('') "
        "assert(blob.exists(id)) assert(blob.size(id) == 0) "
        "assert(blob.delete(id)) return 1",
        1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

/* ── Audit 38: path resolution, directory creation and tar entries ────
 *
 * The resolver did its work - a lookup and a directory open per component,
 * a readlink per symlink, a mkdirat per missing directory - inside one
 * binding call, uncharged: fs.read of an empty or missing file cost 0 units,
 * a 26-deep path 26 directory opens a call, a 100-deep fs.write mkdir-p'd
 * 100 directories under a flat floor, and tar.extract wrote every entry for
 * 1 + size/8. The resolver charges each step to the binding's charge scope
 * now (cap/fs_resolve.h, hull/shared/work_charge.h): a loop of any of them
 * trips a small budget, uncatchable, and ordinary amounts of work still fit. */

#define A38_DEEP "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z"

/* A temp app root, fs.read + fs.write over all of it, with an empty file,
 * a 26-deep directory holding `empty` and `one`, and a chain of 40
 * symlinks s40 -> s39 -> ... -> s1 -> dir. */
static int a38_fs_open(A15Fs *f)
{
    static const char *const all[] = { "." };
    memset(f, 0, sizeof *f);
    if (!hl_test_mkdtemp(f->dir, sizeof f->dir, "hull_a38")) return -1;
    hl_alloc_init(&f->alloc, 0);
    f->policy = HL_FS_POLICY_INIT;
    char p[HL_TEST_PATH_MAX + 128], q[64];
    snprintf(p, sizeof p, "%s/d", f->dir);
    if (mkdir(p, 0755) != 0) return -1;
    for (const char *c = A38_DEEP; *c; c += c[1] ? 2 : 1) {
        size_t n = strlen(p);
        snprintf(p + n, sizeof p - n, "/%c", *c);
        if (mkdir(p, 0755) != 0) return -1;
    }
    static const char *const leaves[] = { "d/" A38_DEEP "/empty", "empty",
                                          "dir/empty" };
    snprintf(p, sizeof p, "%s/dir", f->dir);
    if (mkdir(p, 0755) != 0) return -1;
    for (size_t i = 0; i < sizeof leaves / sizeof leaves[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", f->dir, leaves[i]);
        FILE *fp = fopen(p, "wb");
        if (!fp) return -1;
        fclose(fp);
    }
    snprintf(p, sizeof p, "%s/d/" A38_DEEP "/one", f->dir);
    FILE *fp = fopen(p, "wb");
    if (!fp || fputs("1", fp) < 0) { if (fp) fclose(fp); return -1; }
    fclose(fp);
    for (int i = 1; i <= 40; i++) {
        snprintf(p, sizeof p, "%s/s%d", f->dir, i);
        if (i == 1) snprintf(q, sizeof q, "dir");
        else snprintf(q, sizeof q, "s%d", i - 1);
        if (symlink(q, p) != 0) return -1;
    }
    const char *perr = NULL;
    if (hl_fs_policy_compile_manifest(f->dir, &f->alloc, all, 1, all, 1,
                                      &f->policy, &perr) != 0)
        return -1;
    f->fs.base_dir = f->dir;
    f->fs.base_len = strlen(f->dir);
    f->fs.policy = &f->policy;
    lua_rt.base.fs_cfg = &f->fs;
    return 0;
}

UTEST(lua_audit38, path_resolution_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a38_fs_open(&f), 0);
    char err[512];
    /* Each loop did 0 units of fs work before (only the loop's own few
     * instructions per iteration, far under the budget); each trips now,
     * and pcall does not catch it. */
    static const char *const loops[] = {
        "for i = 1, 100000 do fs.read('empty') end return 1",
        "for i = 1, 100000 do pcall(fs.read, 'missing') end return 1",
        "for i = 1, 2000 do fs.read('d/" A38_DEEP "/empty') end return 1",
        "for i = 1, 2000 do fs.stat('d/" A38_DEEP "/empty') end return 1",
        "for i = 1, 2000 do fs.mmap('d/" A38_DEEP "/one'):close() end return 1",
        "for i = 1, 2000 do fs.list('d/" A38_DEEP "') end return 1",
        "for i = 1, 20000 do pcall(fs.read, 's40/empty') end return 1",
        "for i = 1, 20000 do pcall(fs.stat, 's40/empty') end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 5000000, err, sizeof err), LUA_OK,
                      loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    /* One of each still fits a small budget, and works. */
    EXPECT_EQ(audit10_run(
        "assert(fs.read('empty') == '') "
        "assert(fs.read('d/" A38_DEEP "/one') == '1') "
        "assert(fs.stat('d/" A38_DEEP "/empty').size == 0) "
        "fs.mmap('d/" A38_DEEP "/one'):close() "
        "assert(#fs.list('d/" A38_DEEP "') == 2) "
        "assert(fs.read('s40/empty') == '') "
        "assert(fs.read('missing') == nil) return 1",
        1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit38, directory_creation_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a38_fs_open(&f), 0);
    char err[512];
    /* 100 directories made per call: before, a flat 32768 per call - 100
     * calls (10,000 directories) fitted in 5M. */
    EXPECT_NE(audit10_run(
        "local deep = string.rep('a/', 100) "
        "for i = 1, 100 do pcall(fs.write, 'w/' .. i .. '/' .. deep .. 'f', '') end "
        "return 1", 5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* The trip stopped the walk part-way: far fewer than the 100 trees. */
    EXPECT_EQ(audit10_run(
        "local n = #fs.list('w') assert(n >= 1 and n < 20, n) return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit38, tar_entries_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a38_fs_open(&f), 0);
    char err[512];
    ASSERT_EQ(audit10_run(
        "tar = require('hull.archive.tar') "
        "local es = {} for i = 1, 2000 do es[i] = { name = 'e' .. i, data = '' } end "
        "TAR = tar.create(es) "
        "PACK = {} for i = 1, 20000 do PACK[i] = { name = 'e' .. i, path = 'empty' } end "
        "return 1", HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* 2000 empty files in ONE call: charged 1 unit each before (~2000). */
    EXPECT_NE(audit10_run("pcall(tar.extract, TAR, 'x') return 1",
                          5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* 20000 reads of one empty path in ONE call: charged its bytes (0). */
    EXPECT_NE(audit10_run("pcall(tar.pack, PACK) return 1",
                          5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* A small archive still extracts and packs in a small budget. */
    EXPECT_EQ(audit10_run(
        "local t = tar.create({ { name = 'a.txt', data = 'A' }, "
        "  { name = 'sub/b.txt', data = 'B' } }) "
        "assert(tar.extract(t, 'y')) "
        "assert(fs.read('y/sub/b.txt') == 'B') "
        "assert(#tar.pack({ 'y/a.txt', 'y/sub/b.txt' }) > 0) return 1",
        1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

/* The re-tuned floors leave ordinary workloads their headroom: 500 small
 * files written into a subdirectory and read back fit in 15M units (~16.7k
 * a write there and ~4.5k a read; a 100M budget holds ~6000 such writes,
 * ~6900 flat ones - it held ~3000 under audit 37's flat 32768). */
UTEST(lua_audit38, ordinary_fs_work_keeps_its_headroom)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a38_fs_open(&f), 0);
    char err[512];
    EXPECT_EQ(audit10_run(
        "for i = 1, 500 do assert(fs.write('site/p' .. i .. '.html', 'page ' .. i)) end "
        "for i = 1, 500 do assert(fs.read('site/p' .. i .. '.html') == 'page ' .. i) end "
        "assert(#fs.list('site') == 500) return 1",
        15000000, err, sizeof err), LUA_OK);
    EXPECT_STREQ(err, "");
    cleanup_lua_caps();
    a15_fs_close(&f);
}

/* ── Audit 39: closing a dying task passes its resumer ───────────────────
 *
 * hl_lua_resume_status closed a coroutine that raised or made a bare yield
 * with lua_closethread(co, NULL): the closed thread's C-call depth restarted
 * at 0 and its __close handlers' unreported work (Lua patch 0004) went
 * nowhere. hull._spawn (and tui.async) run the task inline inside the
 * spawner's run, so a dying task whose __close spawned another dying task
 * nested C frames with no limit until the native stack overflowed (one
 * request killed the server), and each closed task's handlers ran up to a
 * hook stride the budget never saw. The resumer is passed as `from` now. */

/* A bare-yield (and a raising) task whose __close spawns another: the chain
 * stops at Lua's C-stack limit, inside the spawner's run, instead of
 * overflowing the native stack. */
static const char A39_BOMB_YIELD[] =
    "local n = 0\n"
    "local function bomb() return function() n = n + 1\n"
    "  local x <close> = setmetatable({}, {__close = function()\n"
    "    hull._spawn(bomb()) end})\n"
    "  coroutine.yield() end end\n";
static const char A39_BOMB_RAISE[] =
    "local n = 0\n"
    "local function bomb() return function() n = n + 1\n"
    "  local x <close> = setmetatable({}, {__close = function()\n"
    "    hull._spawn(bomb()) end})\n"
    "  error('dying') end end\n";

UTEST(lua_audit39, nested_dying_spawn_stops_at_the_c_stack_limit_in_main)
{
    lua_task_log_reset();
    static const char *const bombs[] = { A39_BOMB_YIELD, A39_BOMB_RAISE };
    for (size_t i = 0; i < 2; i++) {
        char body[1024];
        snprintf(body, sizeof body, "%s"
                 "hull._spawn(bomb())\n"
                 "check(n > 1 and n < 1000, 'tasks: '..n)\n", bombs[i]);
        char out[512];
        ASSERT_EQ(run_task_script(body, out, sizeof out), 0);
        EXPECT_STREQ(out, "ok");
    }
}

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(lua_audit39, nested_dying_spawn_stops_at_the_c_stack_limit_in_a_handler)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);
    lua_State *L = lua_rt.L;
    char src[2048];
    snprintf(src, sizeof src,
             "app.manifest({modules = {'hull/http-server@1'}})\n"
             "%s"
             "app.get('/boom', function(req, res) hull._spawn(bomb()) "
             "  res:json({ n = n }) end)\n", A39_BOMB_YIELD);
    ASSERT_EQ(luaL_dostring(L, src), LUA_OK);
    KlHttpRouter router;
    KlAllocator kalloc = kl_allocator_default();
    kl_http_router_init(&router, &kalloc);
    ASSERT_EQ(hl_lua_wire_routes(&lua_rt, &router), 0);
    hl_lua_test_register(L, &router, &lua_rt);
    ASSERT_EQ(luaL_dostring(L,
        "test('boom', function() "
        "  local r = test.get('/boom') assert(r.status == 200, r.status) "
        "  local n = r.json.n "
        "  assert(n > 1 and n < 1000, 'tasks: ' .. tostring(n)) "
        "  r = test.get('/boom') assert(r.status == 200, r.status) end)\n"),
        LUA_OK);
    int total = 0, passed = 0, failed = 0;
    HlTestCaseResult results[1];
    memset(results, 0, sizeof results);
    hl_lua_test_run(L, &total, &passed, &failed, NULL, results, 1);
    EXPECT_EQ(total, 1);
    EXPECT_TRUE_MSG(results[0].passed, results[0].error);

    kl_http_router_free(&router);
    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua_caps();
    be->tick(actx, 0);
    be->free(actx);
}
#endif

/* Each closed task's __close work is charged to the spawner: a loop of
 * spawns whose handler runs just under a hook stride and then raises trips
 * the limit (unfixed, none of it was ever charged and the loop ran on). */
UTEST(lua_audit39, closed_task_close_work_is_charged_to_the_spawner)
{
    const HlAsyncBackend *be = hl_async_backend();
    ASSERT_TRUE(be != NULL);
    lua_task_log_reset();
    init_lua();
    ASSERT_TRUE(lua_initialized);
    ASSERT_EQ(be->init(&lua_rt.base.async_ctx, NULL), 0);
    lua_rt.max_instructions = 20000000;
    HL_LUA_ARM(&lua_rt, lua_rt.L);

    lua_State *co;
    (void)ssh_co_start(&lua_rt,
        "local mt = {__close = function()\n"
        "  local s = 0 for i = 1, 4000 do s = s + i end error('c') end}\n"
        "local function body() local x <close> = setmetatable({}, mt) error('b') end\n"
        "for i = 1, 20000 do hull._spawn(body) end\n"
        "OUT = 'ran on'\n", &co);
    if (lua_status(co) == LUA_YIELD)
        ssh_tick_until_done(be, lua_rt.base.async_ctx, co);
    lua_getglobal(lua_rt.L, "OUT");
    EXPECT_TRUE(lua_isnil(lua_rt.L, -1));
    lua_pop(lua_rt.L, 1);
    const char *e = lua_tostring(co, -1);
    EXPECT_TRUE_MSG(e && strstr(e, "instruction limit") != NULL, e ? e : "(none)");

    lua_rt.max_instructions = HL_DEFAULT_INSTRUCTIONS;
    HlAsyncBackendCtx *actx = lua_rt.base.async_ctx;
    cleanup_lua();
    be->tick(actx, 0);
    be->free(actx);
}

/* ── Audit 39: blob walks and init, db.open, template compiles ────────
 *
 * Calls that did syscalls or per-entry work for a flat or no charge: a
 * blob walk opened 256 shard directories (65,536 at shard_depth 2) for 256
 * units, blob.init walked and mkdir-p'd a deep path under a 16384 floor,
 * db.open + close of a file database cost ~1 ms for SQLite's allocations
 * (~2400 units), template.render_string compiled (and persisted to the
 * shared cache) every distinct source for its Lua instructions only, and
 * reader:read(1) charged 0 units. Each is charged before the work now: a
 * loop trips a small budget, pcall does not catch it, ordinary use fits,
 * and render_string writes no cache file. */

UTEST(lua_audit39, blob_walks_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a38_fs_open(&f), 0);
    char err[512];
    ASSERT_EQ(audit10_run(
        "blob = require('hull.blob') blob.init({ dir = 'blobs' }) "
        "for i = 1, 300 do blob.put('b' .. i) end return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* 2000 walks of 256 shard directories: ~0.5M units before. */
    static const char *const loops[] = {
        "for i = 1, 2000 do pcall(blob.count) end return 1",
        "for i = 1, 2000 do pcall(blob.total_size) end return 1",
        "for i = 1, 2000 do pcall(blob.iter) end return 1",
        "for i = 1, 2000 do pcall(blob.cleanup, { max_age = 1, dry_run = true }) end return 1",
        "blob.init({ dir = 'blobs2', shard_depth = 2 }) "
        "for i = 1, 2000 do pcall(blob.count) end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        if (i < 4)
            ASSERT_EQ(audit10_run("blob.init({ dir = 'blobs' }) return 1",
                                  HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
        EXPECT_NE_MSG(audit10_run(loops[i], 5000000, err, sizeof err), LUA_OK,
                      loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    /* Ordinary use of a filled store still fits a small budget. */
    EXPECT_EQ(audit10_run(
        "blob.init({ dir = 'blobs' }) "
        "for i = 1, 3 do assert(blob.count() == 300) end "
        "local n = 0 for id in blob.iter() do n = n + 1 end assert(n == 300) "
        "assert(blob.total_size() > 0) "
        "assert(blob.cleanup({ max_age = 1e9 }).removed == 0) return 1",
        5000000, err, sizeof err), LUA_OK);
    EXPECT_STREQ(err, "");
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit39, blob_init_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a38_fs_open(&f), 0);
    char err[512];
    ASSERT_EQ(audit10_run("blob = require('hull.blob') return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* A fresh 150-deep store a call: 150 directories made under a flat
     * 16384 before (100 calls = 1.6M units, 15,000 directories). */
    EXPECT_NE(audit10_run(
        "local deep = string.rep('a/', 150) "
        "for i = 1, 100 do pcall(blob.init, { dir = 'bi/' .. i .. '/' .. deep .. 's' }) end "
        "return 1", 5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run(
        "local n = #fs.list('bi') assert(n >= 1 and n < 20, n) return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* The same deep store again and again: O(depth^2) lookups a call. */
    EXPECT_NE(audit10_run(
        "local deep = string.rep('a/', 150) "
        "for i = 1, 200 do pcall(blob.init, { dir = 'bs/' .. deep .. 's' }) end "
        "return 1", 5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* An ordinary store inits a few times in a small budget. */
    EXPECT_EQ(audit10_run(
        "for i = 1, 10 do blob.init({ dir = 'data/blobs' }) end "
        "assert(blob.count() == 0) return 1",
        1000000, err, sizeof err), LUA_OK);
    EXPECT_STREQ(err, "");
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit39, blob_reader_reads_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a38_fs_open(&f), 0);
    char err[512];
    ASSERT_EQ(audit10_run(
        "blob = require('hull.blob') blob.init({ dir = 'blobs' }) "
        "R = blob.reader(blob.put('xyz')) return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* A read syscall a call - past EOF after the third - for 0 units. */
    EXPECT_NE(audit10_run("for i = 1, 200000 do pcall(R.read, R, 1) end return 1",
                          5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run(
        "local r = blob.reader(blob.put('hello')) local t = {} "
        "while true do local c = r:read(2) if not c then break end t[#t + 1] = c end "
        "r:close() assert(table.concat(t) == 'hello') return 1",
        1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit39, db_open_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    A15Fs f;
    ASSERT_EQ(a38_fs_open(&f), 0);
    static HlManifest m;
    memset(&m, 0, sizeof m);
    m.databases.dynamic.declared = 1;
    m.databases.dynamic.schemes[0] = "sqlite";
    m.databases.dynamic.scheme_count = 1;
    hl_db_registry_set_manifest(test_db_registry, &m);
    char err[512];
    ASSERT_EQ(audit10_run("dbm = require('hull.db') return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* ~2400 units an open + close before: 1000 of them fitted in 5M. */
    EXPECT_NE(audit10_run(
        "for i = 1, 1000 do pcall(function() dbm.open('x.db').close() end) end "
        "return 1", 5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* A trip inside the open - the floor, or SQLite's allocator - is the
     * limit, never "connection failed". */
    for (int64_t lim = 2000; lim <= 20000; lim += 1500) {
        int rc = audit10_run("dbm.open(':memory:').close() return 1",
                             lim, err, sizeof err);
        if (rc != LUA_OK)
            EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, err);
    }
    /* Opening a database a few times fits a small budget. */
    EXPECT_EQ(audit10_run(
        "for i = 1, 5 do local c = dbm.open('x.db') "
        "c.exec('CREATE TABLE IF NOT EXISTS t (x INTEGER)') "
        "c.exec('INSERT INTO t VALUES (?)', { i }) c.close() end "
        "local c = dbm.open('x.db') local r = c.query('SELECT count(*) AS n FROM t') "
        "c.close() assert(r[1].n == 5) return 1",
        2000000, err, sizeof err), LUA_OK);
    EXPECT_STREQ(err, "");
    hl_db_registry_set_manifest(test_db_registry, NULL);
    cleanup_lua_caps();
    a15_fs_close(&f);
}

UTEST(lua_audit39, render_string_is_charged_and_not_persisted)
{
    const char *old_home = getenv("HOME");
    char saved_home[512];
    snprintf(saved_home, sizeof saved_home, "%s", old_home ? old_home : "");
    char tmp[256];
    tc_with_tmp_home(tmp, sizeof tmp);
    ASSERT_NE(tmp[0], 0);
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    ASSERT_EQ(audit10_run(
        "tpl = require('hull.template') PAD = string.rep('x', 300) return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* Distinct sources render, and none is written to the shared cache
     * (each was a sealed file in ~/.hull before). */
    EXPECT_EQ(audit10_run(
        "for i = 1, 20 do assert(tpl.render_string(PAD .. '{{ n }}' .. i, { n = i }) "
        "  == PAD .. i .. i) end return 1",
        HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_EQ(tc_count(tmp), 0);
    /* The compile is charged by size: ~1.6k units a render before. */
    EXPECT_NE(audit10_run(
        "for i = 1, 1000 do pcall(tpl.render_string, PAD .. i, {}) end return 1",
        4000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run(
        "for i = 1, 50 do tpl.render_string('<p>{{ x }}</p>', { x = i }) end return 1",
        1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
    if (saved_home[0]) setenv("HOME", saved_home, 1);
    hl_lua_template_cache_reset();
}

/* ── Audit 39: certificate parses and sync HTTP requests ─────────────
 *
 * crypto.x509_pubkey_pem parsed up to 64 KiB of DER for ~10 units (a crafted
 * certificate of ~5,400 names took ~0.55 ms), and a sync http.get to a
 * refused port did DNS + socket + connect for ~7 units: both loops ran for
 * minutes per budget. Each is charged before the work now
 * (hull/shared/work_charge.h): a loop trips a small budget, pcall does not
 * catch it, and ordinary use still fits. */

UTEST(lua_audit39, x509_parse_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    /* The charge is by length, before the parse: 60 KB of junk costs the
     * same as a crafted certificate of that size (~31k units a call). */
    EXPECT_NE(audit10_run(
        "local der = string.rep('\48', 60000) "
        "for i = 1, 1000 do crypto.x509_pubkey_pem(der) end return 1",
        5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_NE(audit10_run(
        "local der = string.rep('\48', 60000) "
        "for i = 1, 1000 do pcall(crypto.x509_pubkey_pem, der) end return 1",
        5000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* A certificate-sized input (~2 KB) is ~2k units: 100 fit in 1M. */
    EXPECT_EQ(audit10_run(
        "local der = string.rep('\48', 2048) "
        "for i = 1, 100 do assert(crypto.x509_pubkey_pem(der) == nil) end return 1",
        1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
}

#ifndef __has_feature
#  define __has_feature(x) 0
#endif
/* Not under MSan: the sync connect resolves through libc getaddrinfo,
 * which MSan does not instrument (it flags the result as uninitialised). */
#if defined(HL_ENABLE_HTTP_CLIENT) && !__has_feature(memory_sanitizer)
UTEST(lua_audit39, sync_http_requests_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    static const char *hosts[] = { "127.0.0.1" };
    HlHttpConfig cfg = { .allowed_hosts = hosts, .count = 1 };
    lua_rt.base.http_cfg = &cfg;
    char err[512];
    /* Port 1 refuses the connect: each call did real work for ~7 units. */
    static const char *const loops[] = {
        "for i = 1, 2000 do pcall(http.get, 'http://127.0.0.1:1/x') end return 1",
        "for i = 1, 2000 do pcall(http.post, 'http://127.0.0.1:1/x', '') end return 1",
        "for i = 1, 2000 do pcall(http.delete, 'http://127.0.0.1:1/x') end return 1",
        "for i = 1, 2000 do pcall(http.request, 'GET', 'http://127.0.0.1:1/x') end return 1",
        /* refused by the allowlist: the floor is charged before the check */
        "for i = 1, 2000 do pcall(http.get, 'http://x.invalid/') end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        char code[512];
        snprintf(code, sizeof code, "local http = require('hull.http-client') %s",
                 loops[i]);
        EXPECT_NE_MSG(audit10_run(code, 5000000, err, sizeof err), LUA_OK, loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    /* A handful of requests still fits a small budget. */
    EXPECT_EQ(audit10_run(
        "local http = require('hull.http-client') "
        "for i = 1, 20 do assert(not pcall(http.get, 'http://127.0.0.1:1/x')) end "
        "return 1", 1000000, err, sizeof err), LUA_OK);
    lua_rt.base.http_cfg = NULL;
    cleanup_lua_caps();
}
#endif /* HL_ENABLE_HTTP_CLIENT */

/* ── Audit 40: the Lua string-hash seed ────────────────────────────────────────
 *
 * Lua 5.4 seeds its string hash per state (lstate.c luai_makeseed: the
 * state's address, a stack address, a function address and the time), not
 * a compile-time constant, and Hull does not override it. The seed shows in
 * the order pairs() walks string keys: two live states order the same keys
 * differently. */
static char *a40_lua_key_order(lua_State *L)
{
    static const char code[] =
        "local t = {} for i = 1, 64 do t['key' .. i] = true end "
        "local o = {} for k in pairs(t) do o[#o + 1] = k end "
        "return table.concat(o, ',')";
    if (luaL_dostring(L, code) != LUA_OK)
        return NULL;
    const char *s = lua_tostring(L, -1);
    char *out = s ? strdup(s) : NULL;
    lua_pop(L, 1);
    return out;
}

UTEST(lua_audit40, string_hash_seed_differs_between_states)
{
    lua_State *a = luaL_newstate();
    lua_State *b = luaL_newstate();
    ASSERT_TRUE(a != NULL && b != NULL);
    luaL_openlibs(a);
    luaL_openlibs(b);
    char *oa = a40_lua_key_order(a);
    char *ob = a40_lua_key_order(b);
    ASSERT_TRUE(oa != NULL && ob != NULL);
    EXPECT_STRNE(oa, ob);
    free(oa);
    free(ob);
    lua_close(a);
    lua_close(b);
}

/* ── Audit 40: mime.sniff, ws server ping, per-frame ws / SSE floors ──
 *
 * mime.sniff charged nothing, and after an XML declaration its "<svg" scan
 * compared at every byte of the 4 KiB window (~16 us a call): a loop of
 * `mime.sniff("<?xml" .. rep)` ran ~300 s per budget. The server conn:ping()
 * was not charged at all, and a ws frame / SSE event cost 1-3 units for
 * ~5 us of framing and syscalls. Each is charged at its measured cost now
 * (hull/shared/work_charge.h): a loop trips a small budget, pcall does not
 * catch it, and ordinary use still fits. */

#include "hull/shared/work_charge.h"   /* hl_work_send_units */

UTEST(lua_audit40, mime_sniff_is_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    ASSERT_EQ(audit10_run("mime = require('hull.mime') "
                          "XML = '<?xml' .. string.rep('<', 8000) return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    /* ~1088 units a call over a full window. */
    EXPECT_NE(audit10_run("for i = 1, 1000 do mime.sniff(XML) end return 1",
                          500000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_NE(audit10_run("for i = 1, 1000 do pcall(mime.sniff, XML) end return 1",
                          500000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    /* Small inputs cost the floor: 100 fit easily. The scan still finds a
     * root of any case, and gives up at the window. */
    EXPECT_EQ_MSG(audit10_run(
        "for i = 1, 100 do assert(mime.sniff('hello') == 'text/plain') end "
        "assert(mime.sniff('<?xml version=\"1.0\"?><SVG/>') == 'image/svg+xml') "
        "assert(mime.sniff('<!-- x --><svg></svg>') == 'image/svg+xml') "
        "assert(mime.sniff('<?xml?><!DOCTYPE html><HTML>') == 'text/html') "
        "assert(mime.sniff(XML) == 'application/xml') "
        "assert(mime.sniff('<?xml' .. string.rep(' ', 5000) .. '<svg>') "
        "       == 'application/xml') "
        "assert(mime.sniff('<?xml<<svg') == 'image/svg+xml') return 1",
        100000, err, sizeof err), LUA_OK, err);
    cleanup_lua_caps();
}

#ifdef HL_ENABLE_HTTP_SERVER
UTEST(lua_audit40, ws_and_sse_frames_are_charged)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    lua_State *L = lua_rt.L;
    char err[512];

    /* The floors are the measured per-frame cost, not 1 unit. */
    EXPECT_GE(hl_work_send_units(0, 1), 300u);
    EXPECT_EQ(hl_work_send_units(0, 10), 10 * hl_work_send_units(0, 1));
    EXPECT_GE(hl_work_sse_units("", 0, 0), 300u);
    EXPECT_GE(hl_work_sse_units("\n\n\n\n", 4, 0) - hl_work_sse_units("", 0, 0), 4 * 30u);

    /* An empty comment in a loop: ~352 units each (4 before). */
    struct HlSseStreamUD *ud = (struct HlSseStreamUD *)lua_newuserdata(L, sizeof *ud);
    memset(ud, 0, sizeof *ud);
    ud->sse.write_fn = a14_sse_write;
    luaL_setmetatable(L, "HlSseStream");
    lua_setglobal(L, "S");
    EXPECT_NE(audit10_run("for i = 1, 10000 do S:comment('') end return 1",
                          1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_NE(audit10_run("for i = 1, 10000 do pcall(S.event, S, 'e', '') end return 1",
                          1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run("for i = 1, 100 do S:comment('hi') end return 1",
                          1000000, err, sizeof err), LUA_OK);

    /* An empty broadcast is a frame per connection (no Keel connection
     * here, so nothing is sent). */
    HlAllocator wa;
    hl_alloc_init(&wa, 0);
    HlWsRegistry reg;
    hl_ws_registry_init(&reg, &wa);
    for (int i = 0; i < 100; i++)
        ASSERT_NE(hl_ws_registry_add(&reg, "/ws", NULL), NULL);
    lua_rt.base.ws_registry = &reg;
    ASSERT_EQ(audit10_run("ws = require('hull.web.ws-server') return 1",
                          HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
    EXPECT_NE(audit10_run("for i = 1, 100 do ws.broadcast('/ws', '') end return 1",
                          1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);

    /* conn:ping() / conn:send(''): charged before Keel is reached, so one
     * call over a budget smaller than a frame stops before the placeholder
     * handle is used - ping was never charged and reached it. */
    static char placeholder;
    HlWsConn *c = hl_ws_registry_add(&reg, "/one", (KlWsServerConn *)(void *)&placeholder);
    ASSERT_NE(c, NULL);
    hl_lua_ws_push_conn(L, c);
    lua_setglobal(L, "C");
    static const char *const calls[] = {
        "B = 1 C:ping() A = 1 return 1",
        "B = 1 C:ping('payload') A = 1 return 1",
        "B = 1 C:send('') A = 1 return 1",
        "B = 1 C:send_binary('') A = 1 return 1",
    };
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        ASSERT_EQ(audit10_run("B, A = nil, nil return 1",
                              HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK);
        EXPECT_NE_MSG(audit10_run(calls[i], 250, err, sizeof err), LUA_OK, calls[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, calls[i]);
        lua_getglobal(L, "B");
        EXPECT_TRUE_MSG(lua_toboolean(L, -1), calls[i]);   /* reached the call */
        lua_getglobal(L, "A");
        EXPECT_FALSE_MSG(lua_toboolean(L, -1), calls[i]);  /* stopped in it */
        lua_pop(L, 2);
    }
    hl_lua_ws_invalidate_conn(L, c);

    lua_rt.base.ws_registry = NULL;
    hl_ws_registry_free(&reg);
    cleanup_lua_caps();
}
#endif /* HL_ENABLE_HTTP_SERVER */

UTEST(lua_audit40, small_calls_have_a_floor)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    static const char *const loops[] = {
        "local t = require('hull.time') for i = 1, 100000 do t.date() end return 1",
        "local t = require('hull.time') for i = 1, 100000 do t.datetime() end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 1000000, err, sizeof err), LUA_OK, loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    EXPECT_EQ(audit10_run("local t = require('hull.time') "
                          "for i = 1, 1000 do t.date() t.datetime() end return 1",
                          1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
}

/* ── Audit 41: round trips, log records, ctx output, small crypto ─────
 *
 * A kv command paid only its reply chunks (~1 unit for a ~100 us round
 * trip), kv.open nothing, a log record or print only its bytes, ctx.stdout
 * / ctx.stderr writes nothing at all, and a small crypto call or sync
 * smtp.send nothing past its bytes. Each has a per-call floor now
 * (hull/shared/work_charge.h): a loop trips a small budget, pcall does not
 * catch it, and ordinary use still fits. */

#include "../../fake_resp.h"

/* Point stdout at /dev/null while a test writes megabytes to it. */
#include <fcntl.h>
static int a41_stdout_mute(void)
{
    fflush(stdout);
    int saved = dup(1);
    int nul = open("/dev/null", O_WRONLY);
    if (nul >= 0) { dup2(nul, 1); close(nul); }
    return saved;
}
static void a41_stdout_restore(int saved)
{
    fflush(stdout);
    if (saved >= 0) { dup2(saved, 1); close(saved); }
}

UTEST(lua_audit41, floors_are_the_measured_cost)
{
    EXPECT_GE(hl_work_kv_command_units(0), 6000u);
    EXPECT_GE((uint64_t)HL_WORK_KV_OPEN_UNITS, 16000u);
    EXPECT_GE((uint64_t)HL_WORK_NET_SQL_UNITS, 6000u);
    EXPECT_GE(hl_work_log_record_units(0), 100u);
    EXPECT_GE(hl_work_crypto_units(0), 64u);
    EXPECT_GE((uint64_t)HL_WORK_SMTP_SEND_UNITS, 16000u);
}

UTEST(lua_audit41, log_and_print_have_a_floor)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    log_set_quiet(true);
    static const char *const loops[] = {
        "local log = require('hull.log') for i = 1, 10000 do log.info('') end return 1",
        "local log = require('hull.log') for i = 1, 10000 do log.debug('') end return 1",
        "local log = require('hull.log') for i = 1, 100000 do log.info() end return 1",
        "local log = require('hull.log') "
        "for i = 1, 10000 do pcall(log.info, '', '', '', '') end return 1",
        "for i = 1, 10000 do print('') end return 1",
        "for i = 1, 10000 do pcall(print) end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 1000000, err, sizeof err), LUA_OK, loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    EXPECT_EQ(audit10_run("local log = require('hull.log') "
                          "for i = 1, 100 do log.info('a', 'b') print('c', 1) end return 1",
                          1000000, err, sizeof err), LUA_OK);
    log_set_quiet(false);
    cleanup_lua_caps();
}

UTEST(lua_audit41, small_crypto_calls_have_a_floor)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    static const char *const loops[] = {
        "for i = 1, 100000 do crypto.sha256('') end return 1",
        "for i = 1, 100000 do crypto.sha1('') end return 1",
        "for i = 1, 100000 do crypto.sha512('') end return 1",
        "for i = 1, 100000 do crypto.random(1) end return 1",
        "for i = 1, 100000 do crypto.random_token(1) end return 1",
        "for i = 1, 100000 do crypto.hmac_sha256('', 'k') end return 1",
        "for i = 1, 100000 do crypto.create_sha256() end return 1",
        "for i = 1, 100000 do pcall(crypto.create_sha256().digest) end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 1000000, err, sizeof err), LUA_OK, loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    EXPECT_EQ(audit10_run("for i = 1, 1000 do crypto.sha256('x') crypto.random_token(16) "
                          "crypto.hmac_sha256('m', 'k') "
                          "local h = crypto.create_sha256() h:update('x') h:digest() "
                          "end return 1",
                          1000000, err, sizeof err), LUA_OK);
    cleanup_lua_caps();
}

/* ctx.stdout / ctx.stderr writes and flushes were not charged at all, and
 * ctx is a table app.main can keep for request handlers; ctx.stdin:read()
 * blocked the loop with no deadline from there. Writes are charged, and a
 * read outside app.main's own code is refused. */
UTEST(lua_audit41, ctx_output_is_charged_and_stdin_is_mains)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    char err[512];
    ASSERT_EQ(luaL_dostring(lua_rt.L, "app.main(function(ctx) SAVED = ctx return 0 end)"),
              LUA_OK);
    int exit_code = 99;
    ASSERT_EQ(hl_lua_vtable.run_main(&lua_rt.base, NULL, 0, NULL, NULL, &exit_code), 0);
    ASSERT_EQ(exit_code, 0);
    static const char *const loops[] = {
        "for i = 1, 100000 do SAVED.stderr:write('') end return 1",
        "for i = 1, 100000 do SAVED.stdout:write('', '') end return 1",
        "local big = string.rep('x', 1048576) "
        "for i = 1, 100 do SAVED.stdout:write(big, big) end return 1",
        "for i = 1, 100000 do SAVED.stderr:flush() end return 1",
    };
    int muted = a41_stdout_mute();
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 1000000, err, sizeof err), LUA_OK, loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    a41_stdout_restore(muted);
    EXPECT_EQ(audit10_run("for i = 1, 100 do SAVED.stderr:write('') SAVED.stderr:flush() end "
                          "return 1", 1000000, err, sizeof err), LUA_OK);
    /* Outside app.main: refused before it reads. */
    EXPECT_NE(audit10_run("return SAVED.stdin:read()", 1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "only available in app.main"), NULL);
    cleanup_lua_caps();
}

#ifdef HL_ENABLE_HTTP_CLIENT
UTEST(lua_audit41, sync_smtp_send_has_a_floor)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    HlSmtpConfig smtp = {0};
    lua_rt.base.smtp_cfg = &smtp;
    char err[512];
    log_set_quiet(true);
    /* host_not_allowed returns at once: the floor is paid before it. */
    static const char *const o =
        "local o = { host = 'mail.invalid', from = 'a@x.test', to = 'b@x.test', "
        "            subject = 's', body = 'b' } ";
    char code[512];
    snprintf(code, sizeof code, "%s for i = 1, 1000 do smtp.send(o) end return 1", o);
    EXPECT_NE(audit10_run(code, 1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    snprintf(code, sizeof code, "%s for i = 1, 10 do smtp.send(o) end return 1", o);
    EXPECT_EQ(audit10_run(code, 1000000, err, sizeof err), LUA_OK);
    log_set_quiet(false);
    lua_rt.base.smtp_cfg = NULL;
    cleanup_lua_caps();
}
#endif

/* kv.open is charged before the policy-checked DSN is dialed - on a build
 * with no KV backend too, where the open then fails at once. */
UTEST(lua_audit41, kv_open_and_commands_have_a_floor)
{
    init_lua_with_caps();
    ASSERT_TRUE(lua_initialized);
    static HlManifestKvDynamic pol = {
        .hosts = { "127.0.0.1" }, .host_count = 1,
        .schemes = { "redis" }, .scheme_count = 1, .declared = 1,
    };
    lua_rt.base.kv_policy = &pol;
    lua_State *L = lua_rt.L;
    lua_pushcfunction(L, hl_lua_require_trusted);
    lua_pushstring(L, "hull.kv._native");
    ASSERT_EQ(lua_pcall(L, 1, 1, 0), LUA_OK);
    lua_setglobal(L, "KVN");
    char err[512];
    EXPECT_NE(audit10_run("for i = 1, 100 do pcall(KVN.open, 'redis://127.0.0.1:1') end "
                          "return 1", 1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    EXPECT_EQ(audit10_run("for i = 1, 10 do pcall(KVN.open, 'redis://127.0.0.1:1') end "
                          "return 1", 1000000, err, sizeof err), LUA_OK);
#ifdef HL_ENABLE_VALKEY
    /* Commands against a server: each pays a round trip's floor. */
    HlFakeResp srv;
    ASSERT_EQ(fake_resp_start(&srv), 0);
    char code[512];
    snprintf(code, sizeof code, "K = KVN.open('redis://127.0.0.1:%d') return 1", srv.port);
    ASSERT_EQ_MSG(audit10_run(code, HL_DEFAULT_INSTRUCTIONS, err, sizeof err), LUA_OK, err);
    static const char *const loops[] = {
        "for i = 1, 1000 do K:get('a') end return 1",
        "for i = 1, 1000 do K:set('a', 'x') end return 1",
        "for i = 1, 1000 do K:has('a') end return 1",
        "for i = 1, 1000 do K:del('a') end return 1",
        "for i = 1, 1000 do pcall(K.get, K, 'a') end return 1",
    };
    for (size_t i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        EXPECT_NE_MSG(audit10_run(loops[i], 1000000, err, sizeof err), LUA_OK, loops[i]);
        EXPECT_NE_MSG(strstr(err, "instruction limit"), NULL, loops[i]);
    }
    EXPECT_EQ_MSG(audit10_run("for i = 1, 10 do assert(K:get('a') == 'x') K:set('a', 'x') end "
                              "K:close() return 1", 1000000, err, sizeof err), LUA_OK, err);
    snprintf(code, sizeof code, "for i = 1, 100 do KVN.open('redis://127.0.0.1:%d'):close() "
             "end return 1", srv.port);
    EXPECT_NE(audit10_run(code, 1000000, err, sizeof err), LUA_OK);
    EXPECT_NE(strstr(err, "instruction limit"), NULL);
    lua_gc(L, LUA_GCCOLLECT);   /* anything the trip left open */
    fake_resp_stop(&srv);
#endif
    lua_rt.base.kv_policy = NULL;
    cleanup_lua_caps();
}

UTEST_MAIN();
