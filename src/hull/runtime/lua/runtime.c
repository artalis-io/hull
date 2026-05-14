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
#include "hull/manifest.h"
#include "hull/cap/tool.h"
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
    /* Remove dangerous globals */
    static const char *blocked[] = {
        "io", "os", "loadfile", "dofile", "load",
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
        hl_cap_tool_register(lua->L, lua->tool_unveil_ctx);
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
    if (lua->base.db_handle)
        hl_lua_worker_db_init();

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

    /* Cancel and free tracked timers */
    for (size_t i = 0; i < lua->timer_count; i++) {
        HlLuaTimer *t = (HlLuaTimer *)lua->timers[i];
        if (t->timer_id >= 0 && lua->server)
            kl_timer_cancel(&lua->server->ev, t->timer_id);
        hl_alloc_free(lua->base.alloc, t, sizeof(HlLuaTimer));
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

    /* Free WebSocket registry */
    if (lua->base.ws_registry) {
        hl_ws_registry_free(lua->base.ws_registry);
        hl_alloc_free(lua->base.alloc, lua->base.ws_registry,
                      sizeof(HlWsRegistry));
        lua->base.ws_registry = NULL;
    }

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

static int vt_lua_wire_routes_server(HlRuntime *rt, KlServer *server,
                                      void *(*alloc_fn)(size_t))
{
    return hl_lua_wire_routes_server((HlLua *)rt, server, alloc_fn);
}

static int vt_lua_extract_manifest(HlRuntime *rt, HlManifest *out)
{
    HlLua *lua = (HlLua *)rt;
    return hl_manifest_extract(lua->L, out, lua->base.alloc);
}

static void vt_lua_destroy(HlRuntime *rt)
{
    hl_lua_free((HlLua *)rt);
}

const HlRuntimeVtable hl_lua_vtable = {
    .init                = vt_lua_init,
    .load_app            = vt_lua_load_app,
    .wire_routes_server  = vt_lua_wire_routes_server,
    .extract_manifest    = vt_lua_extract_manifest,
    .destroy             = vt_lua_destroy,
    .name                = "Lua",
};
