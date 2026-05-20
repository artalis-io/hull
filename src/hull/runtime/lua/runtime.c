/*
 * lua_runtime.c — Lua 5.4 runtime for Hull
 *
 * Initializes Lua with sandboxing: no io, no os, no loadfile/dofile/load,
 * custom allocator with memory limits, and hull.* module registration.
 *
 * This file holds VM lifecycle + sandbox + custom allocator + the
 * runtime-vtable wrappers. Request dispatch, middleware, route wiring,
 * timers, WebSockets, and SSE live in the sibling .c files split out
 * from this TU (see internal.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/alloc.h"
#include "hull/async_backend.h"
#include "hull/manifest.h"
#include "hull/cap/tool.h"
#include "hull/runtime/tool.h"
#include "hull/runtime/test.h"
#include "hull/cap/ws.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>
#include <keel/websocket_server.h>

#include <sh_arena.h>

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Forward declaration for hull async cont (defined in lua/async.c) */
extern int luaopen_hull_hull(lua_State *L);

/* ── Instruction limit hook ─────────────────────────────────────────── */

void hl_lua_instruction_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    luaL_error(L, "instruction limit exceeded");
}

/* ── Custom allocator with memory limit ─────────────────────────────── */

static void *hl_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    HlLua *lua = (HlLua *)ud;

    if (nsize == 0) {
        /* Free — osize is the real block size here */
        if (lua->mem_used >= osize)
            lua->mem_used -= osize;
        else
            lua->mem_used = 0;
        hl_alloc_free(lua->base.alloc, ptr, osize);
        return NULL;
    }

    /* When ptr is NULL (new allocation), Lua passes a type-hint enum
     * (0–8) in osize, not a real size. Use 0 for tracking. */
    size_t effective_osize = (ptr == NULL) ? 0 : osize;

    /* Check Lua sub-limit first */
    if (nsize > effective_osize) {
        size_t delta = nsize - effective_osize;
        if (lua->mem_limit > 0 && lua->mem_used + delta > lua->mem_limit)
            return NULL; /* allocation refused */
    }

    /* Route through tracking allocator.
     * When ptr is NULL, use malloc to avoid confusing the tracker. */
    void *new_ptr;
    if (ptr == NULL)
        new_ptr = hl_alloc_malloc(lua->base.alloc, nsize);
    else
        new_ptr = hl_alloc_realloc(lua->base.alloc, ptr, osize, nsize);

    if (new_ptr) {
        if (nsize > effective_osize)
            lua->mem_used += nsize - effective_osize;
        else if (lua->mem_used >= effective_osize - nsize)
            lua->mem_used -= effective_osize - nsize;
        else
            lua->mem_used = 0;
    }
    return new_ptr;
}

/* ── Sandbox: remove dangerous globals ──────────────────────────────── */

static void hl_lua_sandbox(lua_State *L)
{
    /* Remove dangerous globals.
     *
     * `package` and `debug` are never opened via luaL_requiref above,
     * so these are normally absent. Nil-ing them defensively makes the
     * W^X intent explicit and survives any future change that opens
     * additional libraries (e.g. via luaL_openlibs). The `package`
     * library is the channel for `package.loadlib` / `package.cpath`
     * which would allow loading native code from disk; `debug` exposes
     * introspection APIs that can subvert sandboxing. */
    static const char *blocked[] = {
        "io", "os", "loadfile", "dofile", "load",
        "package", "debug",
    };

    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, blocked[i]);
    }
}

/* ── Print helper (mirrors console polyfill in JS) ──────────────────── */

static int hl_lua_print(lua_State *L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1)
            fputc('\t', stderr);
        const char *s = luaL_tolstring(L, i, NULL);
        if (s)
            fputs(s, stderr);
        lua_pop(L, 1); /* pop the string from luaL_tolstring */
    }
    fputc('\n', stderr);
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int hl_lua_init(HlLua *lua, const HlLuaConfig *cfg)
{
    if (!lua || !cfg)
        return -1;

    /* Save caller-set base fields before zeroing */
    HlRuntime saved_base = lua->base;

    memset(lua, 0, sizeof(*lua));

    /* Restore caller-set base fields */
    lua->base = saved_base;
    lua->mem_limit = cfg->max_heap_bytes;
    lua->max_instructions = cfg->max_instructions;

    /* Create Lua state with custom allocator */
    lua->L = lua_newstate(hl_lua_alloc, lua);
    if (!lua->L)
        return -1;

    /* Arm instruction limit hook */
    if (lua->max_instructions > 0) {
        lua_sethook(lua->L, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(lua->max_instructions));
    }

    if (cfg->sandbox) {
        /* Open safe standard libraries only */
        luaL_requiref(lua->L, "_G", luaopen_base, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_COLIBNAME, luaopen_coroutine, 1);
        lua_pop(lua->L, 1);

        /* Apply sandbox — remove io, os, loadfile, dofile, load */
        hl_lua_sandbox(lua->L);
    } else {
        /* Tool mode: safe libs + hull.tool (no raw os/io) */
        luaL_requiref(lua->L, "_G", luaopen_base, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(lua->L, 1);
        luaL_requiref(lua->L, LUA_COLIBNAME, luaopen_coroutine, 1);
        lua_pop(lua->L, 1);
        hl_lua_tool_register(lua->L, lua->tool_unveil_ctx);
    }

    /* Replace print with stderr version */
    lua_pushcfunction(lua->L, hl_lua_print);
    lua_setglobal(lua->L, "print");

    /* Store HlLua pointer in registry for C functions to access */
    lua_pushlightuserdata(lua->L, (void *)lua);
    lua_setfield(lua->L, LUA_REGISTRYINDEX, "__hull_lua");

    /* Register worker VM init hooks (e.g. db.* for worker.dispatch).
     * Must happen before modules are registered since module init may
     * trigger worker VM creation. */
#ifdef HL_ENABLE_DB
    if (lua->base.db_handle)
        hl_lua_worker_db_init();
#endif

    /* Register hull.* C modules */
    if (hl_lua_register_modules(lua) != 0) {
        hl_lua_free(lua);
        return -1;
    }

    /* Register Lua stdlib (embedded modules + custom require) */
    if (hl_lua_register_stdlib(lua) != 0) {
        hl_lua_free(lua);
        return -1;
    }

    /* Per-request scratch arena */
    lua->scratch = hl_arena_create(lua->base.alloc, HL_SCRATCH_SIZE);
    if (!lua->scratch) {
        hl_lua_free(lua);
        return -1;
    }

    lua->udf_runtime_alive = 1;

    return 0;
}

int hl_lua_load_app(HlLua *lua, const char *filename)
{
    if (!lua || !lua->L || !filename)
        return -1;

    /* Extract app directory from filename */
    size_t fn_len = strlen(filename);
    char *app_dir = hl_alloc_malloc(lua->base.alloc, fn_len + 1);
    if (!app_dir)
        return -1;
    memcpy(app_dir, filename, fn_len + 1);
    char *last_slash = strrchr(app_dir, '/');
    if (last_slash)
        *last_slash = '\0';
    else {
        hl_alloc_free(lua->base.alloc, app_dir, fn_len + 1);
        app_dir = hl_alloc_malloc(lua->base.alloc, 2);
        if (!app_dir)
            return -1;
        app_dir[0] = '.';
        app_dir[1] = '\0';
        fn_len = 1;
    }
    lua->app_dir = app_dir;
    lua->app_dir_size = fn_len + 1;

    /* Set module context so requires from app entry point resolve correctly */
    lua_pushstring(lua->L, filename);
    lua_setfield(lua->L, LUA_REGISTRYINDEX, "__hull_current_module");

    /* Try embedded VFS entry first (hull build binaries).
     * Convert filename to VFS name: basename without .lua, prepended with ./
     * e.g. "app.lua" → "./app", "/path/to/app.lua" → "./app" */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_modules");
    if (lua_istable(lua->L, -1)) {
        const char *base = strrchr(filename, '/');
        base = base ? base + 1 : filename;
        char vfs_name[256];
        size_t blen = strlen(base);
        if (blen >= 4 && strcmp(base + blen - 4, ".lua") == 0)
            blen -= 4;
        if (blen + 3 <= sizeof(vfs_name)) {
            vfs_name[0] = '.';
            vfs_name[1] = '/';
            memcpy(vfs_name + 2, base, blen);
            vfs_name[2 + blen] = '\0';

            lua_getfield(lua->L, -1, vfs_name);
            if (!lua_isnil(lua->L, -1)) {
                lua_remove(lua->L, -2); /* remove __hull_modules */
                if (lua_pcall(lua->L, 0, 0, 0) != LUA_OK) {
                    hl_lua_dump_error(lua);
                    return -1;
                }
                sh_arena_reset(lua->scratch);
                return 0;
            }
            lua_pop(lua->L, 1); /* pop nil */
        }
    }
    lua_pop(lua->L, 1); /* pop __hull_modules (or non-table) */

    /* Load and execute from filesystem (development mode) */
    if (luaL_dofile(lua->L, filename) != LUA_OK) {
        hl_lua_dump_error(lua);
        return -1;
    }

    /* Reset scratch arena — startup module loads no longer needed */
    sh_arena_reset(lua->scratch);

    return 0;
}

void hl_lua_free(HlLua *lua)
{
    if (!lua)
        return;

    /* Cancel and free tracked timers — via async backend vtable. */
    {
        const HlAsyncBackend *be = hl_async_backend();
        for (size_t i = 0; i < lua->timer_count; i++) {
            HlLuaTimer *t = (HlLuaTimer *)lua->timers[i];
            if (t->timer_id > 0 && lua->base.async_ctx)
                be->timer_cancel(lua->base.async_ctx, (uint64_t)t->timer_id);
            hl_alloc_free(lua->base.alloc, t, sizeof(HlLuaTimer));
        }
    }
    if (lua->timers) {
        hl_alloc_free(lua->base.alloc, lua->timers,
                      lua->timer_cap * sizeof(void *));
        lua->timers = NULL;
        lua->timer_count = 0;
        lua->timer_cap = 0;
    }

    /* Free tracked route allocations */
    for (size_t i = 0; i < lua->route_count; i++)
        hl_alloc_free(lua->base.alloc, lua->routes[i], sizeof(HlLuaRoute));
    if (lua->routes) {
        hl_alloc_free(lua->base.alloc, lua->routes,
                      lua->route_cap * sizeof(void *));
        lua->routes = NULL;
        lua->route_count = 0;
        lua->route_cap = 0;
    }

    /* Free tracked WS route allocations */
    for (size_t i = 0; i < lua->ws_route_count; i++)
        hl_alloc_free(lua->base.alloc, lua->ws_routes[i],
                      sizeof(HlLuaWsRoute));
    if (lua->ws_routes) {
        hl_alloc_free(lua->base.alloc, lua->ws_routes,
                      lua->ws_route_cap * sizeof(void *));
        lua->ws_routes = NULL;
        lua->ws_route_count = 0;
        lua->ws_route_cap = 0;
    }

    /* Free tracked WS config allocations */
    for (size_t i = 0; i < lua->ws_cfg_count; i++)
        hl_alloc_free(lua->base.alloc, lua->ws_cfgs[i],
                      sizeof(KlWsServerConfig));
    if (lua->ws_cfgs) {
        hl_alloc_free(lua->base.alloc, lua->ws_cfgs,
                      lua->ws_cfg_cap * sizeof(void *));
        lua->ws_cfgs = NULL;
        lua->ws_cfg_count = 0;
        lua->ws_cfg_cap = 0;
    }

    /* Free tracked SSE route allocations */
    for (size_t i = 0; i < lua->sse_route_count; i++)
        hl_alloc_free(lua->base.alloc, lua->sse_routes[i],
                      sizeof(HlLuaSseRoute));
    if (lua->sse_routes) {
        hl_alloc_free(lua->base.alloc, lua->sse_routes,
                      lua->sse_route_cap * sizeof(void *));
        lua->sse_routes = NULL;
        lua->sse_route_count = 0;
        lua->sse_route_cap = 0;
    }

    /* Free WebSocket registry — HTTP-only; CLI builds never create one. */
#ifdef HL_ENABLE_HTTP
    if (lua->base.ws_registry) {
        hl_ws_registry_free(lua->base.ws_registry);
        hl_alloc_free(lua->base.alloc, lua->base.ws_registry,
                      sizeof(HlWsRegistry));
        lua->base.ws_registry = NULL;
    }
#endif

    /* Mark runtime as dead before lua_close so UDF destroy callbacks
     * (fired by sqlite3_close) don't call luaL_unref on a dead state */
    lua->udf_runtime_alive = 0;

    if (lua->L) {
        lua_close(lua->L);
        lua->L = NULL;
    }
    if (lua->app_dir) {
        hl_alloc_free(lua->base.alloc, (void *)lua->app_dir, lua->app_dir_size);
        lua->app_dir = NULL;
        lua->app_dir_size = 0;
    }
    hl_arena_free(lua->base.alloc, lua->scratch);
    lua->scratch = NULL;
}

void hl_lua_dump_error(HlLua *lua)
{
    if (!lua || !lua->L)
        return;

    const char *msg = lua_tostring(lua->L, -1);
    if (msg)
        log_error("[hull:c] lua error: %s", msg);

    /* Try to get traceback */
    luaL_traceback(lua->L, lua->L, msg, 1);
    const char *tb = lua_tostring(lua->L, -1);
    if (tb && tb != msg)
        log_error("[hull:c] %s", tb);
    lua_pop(lua->L, 1); /* pop traceback */
    lua_pop(lua->L, 1); /* pop original error message */
}

/* ── Vtable adapters ───────────────────────────────────────────────── */

static int vt_lua_init(HlRuntime *rt, const void *config)
{
    return hl_lua_init((HlLua *)rt, (const HlLuaConfig *)config);
}

static int vt_lua_load_app(HlRuntime *rt, const char *filename)
{
    return hl_lua_load_app((HlLua *)rt, filename);
}

#ifdef HL_ENABLE_HTTP
static int vt_lua_wire_routes_server(HlRuntime *rt, KlServer *server,
                                      void *(*alloc_fn)(size_t))
{
    return hl_lua_wire_routes_server((HlLua *)rt, server, alloc_fn);
}
#else
/* CLI-only build placeholder (see vt_js equivalent). */
static int vt_lua_wire_routes_server(HlRuntime *rt, KlServer *server,
                                      void *(*alloc_fn)(size_t))
{
    (void)rt; (void)server; (void)alloc_fn;
    return -1;
}
#endif

static int vt_lua_extract_manifest(HlRuntime *rt, HlManifest *out)
{
    HlLua *lua = (HlLua *)rt;
    return hl_manifest_extract_lua(lua->L, out, lua->base.alloc);
}

/* Walk __hull_route_defs in the Lua registry, calling cb for each entry. */
static void vt_lua_enumerate_routes(HlRuntime *rt, HlRouteCb cb, void *user)
{
    if (!cb) return;
    HlLua *lua = (HlLua *)rt;
    lua_State *L = lua->L;
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (!lua_isnil(L, -1)) {
        int count = (int)luaL_len(L, -1);
        for (int i = 1; i <= count; i++) {
            lua_rawgeti(L, -1, i);
            lua_getfield(L, -1, "method");
            const char *method = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "pattern");
            const char *pattern = lua_tostring(L, -1);
            lua_pop(L, 1);
            cb(user, method, pattern);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

/* Walk __hull_middleware (pre) + __hull_post_middleware (post). */
static void vt_lua_enumerate_middleware(HlRuntime *rt, HlMiddlewareCb cb, void *user)
{
    if (!cb) return;
    HlLua *lua = (HlLua *)rt;
    lua_State *L = lua->L;

    static const struct { const char *key; const char *phase; } phases[] = {
        { "__hull_middleware",      "pre"  },
        { "__hull_post_middleware", "post" },
    };
    for (size_t p = 0; p < sizeof(phases)/sizeof(phases[0]); p++) {
        lua_getfield(L, LUA_REGISTRYINDEX, phases[p].key);
        if (!lua_isnil(L, -1)) {
            int count = (int)luaL_len(L, -1);
            for (int i = 1; i <= count; i++) {
                lua_rawgeti(L, -1, i);
                lua_getfield(L, -1, "method");
                const char *method = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "pattern");
                const char *pattern = lua_tostring(L, -1);
                lua_pop(L, 1);
                cb(user, method, pattern, phases[p].phase);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
}

#ifdef HL_ENABLE_HTTP
static int vt_lua_test_setup(HlRuntime *rt, KlRouter *router)
{
    HlLua *lua = (HlLua *)rt;
    if (hl_lua_wire_routes(lua, router) != 0)
        return -1;
    hl_lua_test_register(lua->L, router, lua);
    return 0;
}
#else
static int vt_lua_test_setup(HlRuntime *rt, KlRouter *router)
{
    (void)rt; (void)router;
    return -1;
}
#endif

static int vt_lua_run_test_file(HlRuntime *rt, const char *file_path,
                                HlTestCaseResult *results, int max_results,
                                int *file_total, int *file_passed, int *file_failed,
                                const char **load_err)
{
#ifdef HL_ENABLE_HTTP
    HlLua *lua = (HlLua *)rt;
    hl_lua_test_clear(lua->L);
    if (luaL_dofile(lua->L, file_path) != LUA_OK) {
        const char *err = lua_tostring(lua->L, -1);
        if (load_err) *load_err = err ? err : "unknown";
        lua_pop(lua->L, 1);
        return -1;
    }
    /* hl_lua_test_run unconditionally writes to file_total/passed/failed.
     * The shared runner always passes non-NULL pointers; cppcheck CTU
     * traces it via the vtable so the deref is provably safe. */
    hl_lua_test_run(lua->L, file_total, file_passed, file_failed,
                    NULL, results, max_results);
    return 0;
#else
    (void)rt; (void)file_path; (void)results; (void)max_results;
    (void)file_total; (void)file_passed; (void)file_failed;
    if (load_err) *load_err = "test runner unavailable on HTTP=0 builds";
    return -1;
#endif
}

static void vt_lua_destroy(HlRuntime *rt)
{
    hl_lua_free((HlLua *)rt);
}

/* ── CLI mode (app.main) ───────────────────────────────────────────────
 *
 * Methods that get pushed onto ctx.stdin / ctx.stdout / ctx.stderr.
 * They capture the stream identifier (0/1/2) as an upvalue and call
 * the corresponding libc FILE*. Methods accept `self` as the first
 * argument (so callers can use `ctx.stdout:write(s)`) but ignore it.
 */

static FILE *cli_stream(lua_State *L)
{
    int s = (int)lua_tointeger(L, lua_upvalueindex(1));
    switch (s) {
    case 0: return stdin;
    case 1: return stdout;
    case 2: return stderr;
    default: return NULL;
    }
}

static int cli_stdin_read(lua_State *L)
{
    FILE *f = cli_stream(L);
    if (!f) return luaL_error(L, "invalid stream");

    /* Accept "*l" (line, no newline), "*a" (all), or a positive integer
     * count of bytes. Default (no arg) → "*l". */
    const char *fmt = "*l";
    lua_Integer nbytes = 0;
    int want_bytes = 0;

    int top = lua_gettop(L);
    if (top >= 2 && !lua_isnil(L, 2)) {
        if (lua_isnumber(L, 2)) {
            nbytes = lua_tointeger(L, 2);
            if (nbytes < 0) return luaL_error(L, "read: negative byte count");
            want_bytes = 1;
        } else {
            fmt = luaL_checkstring(L, 2);
        }
    }

    if (want_bytes) {
        if (nbytes == 0) { lua_pushliteral(L, ""); return 1; }
        luaL_Buffer b;
        char *buf = luaL_buffinitsize(L, &b, (size_t)nbytes);
        size_t n = fread(buf, 1, (size_t)nbytes, f);
        luaL_pushresultsize(&b, n);
        if (n == 0 && feof(f)) {
            lua_pop(L, 1);
            lua_pushnil(L);
        }
        return 1;
    }

    if (strcmp(fmt, "*a") == 0 || strcmp(fmt, "a") == 0) {
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
            luaL_addlstring(&b, chunk, n);
        luaL_pushresult(&b);
        return 1;
    }

    if (strcmp(fmt, "*l") == 0 || strcmp(fmt, "l") == 0) {
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        int got = 0;
        for (;;) {
            int c = fgetc(f);
            if (c == EOF) break;
            got = 1;
            if (c == '\n') break;
            char ch = (char)c;
            luaL_addlstring(&b, &ch, 1);
        }
        luaL_pushresult(&b);
        if (!got) { lua_pop(L, 1); lua_pushnil(L); }
        return 1;
    }

    return luaL_error(L, "read: unsupported format '%s' (use '*l', '*a', or n bytes)", fmt);
}

static int cli_writer_write(lua_State *L)
{
    FILE *f = cli_stream(L);
    if (!f) return luaL_error(L, "invalid stream");
    int top = lua_gettop(L);
    for (int i = 2; i <= top; i++) {  /* arg 1 is self */
        size_t n = 0;
        const char *s = luaL_checklstring(L, i, &n);
        if (n && fwrite(s, 1, n, f) != n)
            return luaL_error(L, "write failed");
    }
    return 0;
}

static int cli_writer_flush(lua_State *L)
{
    FILE *f = cli_stream(L);
    if (f) fflush(f);
    return 0;
}

static int cli_stdin_close(lua_State *L)
{
    /* No-op: closing stdin should not propagate to other readers. */
    (void)L;
    return 0;
}

/* Build a stream table with the given methods, capturing `stream_id`
 * as an upvalue. Leaves the table on top of the Lua stack. */
static void cli_push_stream(lua_State *L, int stream_id,
                             const luaL_Reg *methods)
{
    lua_newtable(L);
    for (const luaL_Reg *m = methods; m->name; m++) {
        lua_pushinteger(L, stream_id);
        lua_pushcclosure(L, m->func, 1);
        lua_setfield(L, -2, m->name);
    }
}

static const luaL_Reg cli_stdin_methods[]  = {
    { "read",  cli_stdin_read  },
    { "close", cli_stdin_close },
    { NULL, NULL }
};
static const luaL_Reg cli_writer_methods[] = {
    { "write", cli_writer_write },
    { "flush", cli_writer_flush },
    { NULL, NULL }
};

static int vt_lua_has_main(HlRuntime *rt)
{
    if (!rt) return 0;
    HlLua *lua = (HlLua *)rt;
    if (!lua->L) return 0;
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_main");
    int has = !lua_isnil(lua->L, -1);
    lua_pop(lua->L, 1);
    return has;
}

/* Coerce the value on top of `co`'s stack to an exit code in [0..255]. */
static int lua_coerce_exit_code(lua_State *co)
{
    int t = lua_type(co, -1);
    if (t == LUA_TNONE || t == LUA_TNIL) return 0;
    if (t == LUA_TNUMBER) {
        if (lua_isinteger(co, -1)) {
            lua_Integer i = lua_tointeger(co, -1);
            if (i < 0) i = 0;
            if (i > 255) i = i & 0xff;
            return (int)i;
        }
        return (int)lua_tonumber(co, -1);
    }
    const char *s = lua_tostring(co, -1);
    fprintf(stderr, "[hull:main] non-numeric return: %s\n",
            s ? s : "(unprintable)");
    return 1;
}

static int vt_lua_run_main(HlRuntime *rt, KlServer *server,
                            int argc, char **argv,
                            const char *const *env_allowlist,
                            int *exit_code_out)
{
    if (exit_code_out) *exit_code_out = 1;
    if (!rt || !exit_code_out) return -1;
    HlLua *lua = (HlLua *)rt;
    lua_State *L = lua->L;

    /* CLI mode bypasses wire_routes_server, so the server pointer
     * normally set there isn't wired. Set it now so async ops
     * (hull.sleep, compute.async, gpu.async, http.fetch) can find it. */
    if (server) lua->server = server;

    /* Wrap main in a coroutine so async ops (compute.async, gpu.async,
     * http.fetch, hull.sleep) can yield via the existing infrastructure.
     * The same machinery the HTTP dispatch uses applies here — only the
     * "what to do when the coroutine finally terminates" differs.
     *
     * Stack: [_] → after setup [thread] (pinned via registry ref). */
    lua_State *co = lua_newthread(L);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (co_ref == LUA_REFNIL) {
        return -1;
    }

    /* Push main onto the coroutine stack. */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_main");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
        return -1;
    }
    lua_xmove(L, co, 1);  /* L: ... → co: [main_fn] */

    /* Build the ctx table directly on the coroutine. */
    lua_newtable(co);

    lua_newtable(co);
    for (int i = 0; i < argc; i++) {
        lua_pushstring(co, argv[i] ? argv[i] : "");
        lua_rawseti(co, -2, i + 1);
    }
    lua_setfield(co, -2, "args");

    lua_newtable(co);
    if (env_allowlist) {
        for (int i = 0; env_allowlist[i]; i++) {
            const char *name = env_allowlist[i];
            const char *val = getenv(name);
            if (val) {
                lua_pushstring(co, val);
                lua_setfield(co, -2, name);
            }
        }
    }
    lua_setfield(co, -2, "env");

    cli_push_stream(co, 0, cli_stdin_methods);   lua_setfield(co, -2, "stdin");
    cli_push_stream(co, 1, cli_writer_methods);  lua_setfield(co, -2, "stdout");
    cli_push_stream(co, 2, cli_writer_methods);  lua_setfield(co, -2, "stderr");

    /* Set active state so async ops see the right coroutine/conn. */
    lua_State *saved_co       = lua->active_co;
    KlConn   *saved_conn      = lua->active_conn;
    int       saved_thread_ref = lua->active_thread_ref;
    int       saved_depth      = lua->dispatch_depth;

    lua->active_co         = co;
    lua->active_conn       = NULL;       /* detached — no HTTP conn */
    lua->active_thread_ref = co_ref;
    lua->dispatch_depth    = saved_depth + 1;

    /* First resume: main runs until it returns or yields. */
    int nres = 0;
    int status = lua_resume(co, L, 1, &nres);

    if (status == LUA_YIELD) {
        /* Main yielded — async op in flight. Mark this coroutine so
         * hl_lua_async_resume knows to stop the server when it
         * eventually completes, then enter the event loop. */
        lua->cli_main_co = co;

        if (!lua->base.async_ctx) {
            fprintf(stderr, "[hull:main] internal: no event loop available\n");
            luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
            lua->active_co         = saved_co;
            lua->active_conn       = saved_conn;
            lua->active_thread_ref = saved_thread_ref;
            lua->dispatch_depth    = saved_depth;
            lua->cli_main_co       = NULL;
            return -1;
        }

        /* Drive the event loop via the async backend vtable. On HTTP=1
         * builds this is a wrap around KlServer's KlEventCtx (same loop
         * the HTTP server uses); on HTTP=0 it's the poll backend's
         * standalone loop. hl_lua_async_resume calls backend->stop when
         * main terminates, which returns us here. */
        int srv_rc = hl_async_backend()->run(lua->base.async_ctx);
        if (srv_rc < 0) {
            fprintf(stderr, "[hull:main] event loop error\n");
        }

        /* After run() returns, the coroutine status reflects main's
         * terminal state. */
        status = lua_status(co);
        nres   = lua_gettop(co);
    }

    /* `co`'s stack now holds main's return value (LUA_OK) or an error
     * message (LUA_ERRRUN etc.) — either way, the top of the stack is
     * what we read. */
    int rc;
    if (status == LUA_OK) {
        rc = lua_coerce_exit_code(co);
    } else {
        const char *err = lua_tostring(co, -1);
        fprintf(stderr, "[hull:main] error: %s\n", err ? err : "(unknown)");
        rc = 1;
    }

    /* Restore active state and release the coroutine. */
    lua->active_co         = saved_co;
    lua->active_conn       = saved_conn;
    lua->active_thread_ref = saved_thread_ref;
    lua->dispatch_depth    = saved_depth;
    lua->cli_main_co       = NULL;
    luaL_unref(L, LUA_REGISTRYINDEX, co_ref);

    *exit_code_out = rc;
    return (status == LUA_OK) ? 0 : -1;
}

const HlRuntimeVtable hl_lua_vtable = {
    .init                = vt_lua_init,
    .load_app            = vt_lua_load_app,
    .wire_routes_server  = vt_lua_wire_routes_server,
    .extract_manifest    = vt_lua_extract_manifest,
    .enumerate_routes    = vt_lua_enumerate_routes,
    .enumerate_middleware= vt_lua_enumerate_middleware,
    .test_setup          = vt_lua_test_setup,
    .run_test_file       = vt_lua_run_test_file,
    .destroy             = vt_lua_destroy,
    .name                = "Lua",
    .test_file_pattern   = "test_*.lua",
    .has_main            = vt_lua_has_main,
    .run_main            = vt_lua_run_main,
};
