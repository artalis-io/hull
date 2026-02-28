/*
 * lua_runtime.c — Lua 5.4 runtime for Hull
 *
 * Initializes Lua with sandboxing: no io, no os, no loadfile/dofile/load,
 * custom allocator with memory limits, and hull.* module registration.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "hull/alloc.h"
#include "hull/manifest.h"
#include "hull/cap/body.h"
#include "hull/cap/fs.h"
#include "hull/cap/env.h"
#include "hull/cap/tool.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>

#include <sh_arena.h>

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    /* Check Lua sub-limit first */
    if (nsize > osize) {
        size_t delta = nsize - osize;
        if (lua->mem_limit > 0 && lua->mem_used + delta > lua->mem_limit)
            return NULL; /* allocation refused */
    }

    /* Route through tracking allocator.
     * When ptr is NULL, osize is a Lua type hint (not a real size),
     * so use malloc to avoid confusing the tracker. */
    void *new_ptr;
    if (ptr == NULL)
        new_ptr = hl_alloc_malloc(lua->base.alloc, nsize);
    else
        new_ptr = hl_alloc_realloc(lua->base.alloc, ptr, osize, nsize);

    if (new_ptr) {
        if (nsize > osize)
            lua->mem_used += nsize - osize;
        else if (lua->mem_used >= osize - nsize)
            lua->mem_used -= osize - nsize;
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

    /* Create Lua state with custom allocator */
    lua->L = lua_newstate(hl_lua_alloc, lua);
    if (!lua->L)
        return -1;

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

    /* Load and execute the file */
    if (luaL_dofile(lua->L, filename) != LUA_OK) {
        hl_lua_dump_error(lua);
        return -1;
    }

    /* Reset scratch arena — startup module loads no longer needed */
    sh_arena_reset(lua->scratch);

    return 0;
}

int hl_lua_dispatch(HlLua *lua, int handler_id,
                       KlRequest *req, KlResponse *res)
{
    if (!lua || !lua->L || !req || !res)
        return -1;

    /* Reset scratch arena for this request */
    sh_arena_reset(lua->scratch);

    /* Get the handler function from the route registry */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_routes");
    if (!lua_istable(lua->L, -1)) {
        lua_pop(lua->L, 1);
        return -1;
    }

    lua_rawgeti(lua->L, -1, handler_id);
    if (!lua_isfunction(lua->L, -1)) {
        lua_pop(lua->L, 2); /* pop function + routes table */
        return -1;
    }

    /* Build request and response objects */
    hl_lua_make_request(lua->L, req);
    hl_lua_make_response(lua->L, res);

    /* Call handler(req, res) */
    if (lua_pcall(lua->L, 2, 0, 0) != LUA_OK) {
        log_error("[hull:c] lua handler error: %s",
                  lua_tostring(lua->L, -1));
        lua_pop(lua->L, 1); /* pop error message */
        lua_pop(lua->L, 1); /* pop routes table */
        /* Free ctx if middleware set it */
        if (req->ctx) {
            size_t ctx_sz = strlen((char *)req->ctx) + 1;
            hl_alloc_free(lua->base.alloc, req->ctx, ctx_sz);
            req->ctx = NULL;
        }
        return -1;
    }

    lua_pop(lua->L, 1); /* pop routes table */

    /* Free ctx if middleware set it */
    if (req->ctx) {
        size_t ctx_sz = strlen((char *)req->ctx) + 1;
        hl_alloc_free(lua->base.alloc, req->ctx, ctx_sz);
        req->ctx = NULL;
    }
    return 0;
}

void hl_lua_free(HlLua *lua)
{
    if (!lua)
        return;

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
    if (lua->response_body) {
        hl_alloc_free(lua->base.alloc, lua->response_body,
                      lua->response_body_size);
        lua->response_body = NULL;
        lua->response_body_size = 0;
    }
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
}

/* ── Route tracking ────────────────────────────────────────────────── */

static int hl_lua_track_route(HlLua *lua, void *route)
{
    if (lua->route_count >= lua->route_cap) {
        size_t new_cap = lua->route_cap ? lua->route_cap * 2 : 8;
        size_t old_sz = lua->route_cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(lua->base.alloc,
                                           lua->routes, old_sz, new_sz);
        if (!new_arr)
            return -1;
        lua->routes = new_arr;
        lua->route_cap = new_cap;
    }
    lua->routes[lua->route_count++] = route;
    return 0;
}

/* ── Route wiring ──────────────────────────────────────────────────── */

void hl_lua_keel_handler(KlRequest *req, KlResponse *res, void *user_data)
{
    HlLuaRoute *route = (HlLuaRoute *)user_data;
    if (hl_lua_dispatch(route->lua, route->handler_id, req, res) != 0) {
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body(res, "Internal Server Error", 21);
    }
}

int hl_lua_wire_routes(HlLua *lua, KlRouter *router)
{
    lua_State *L = lua->L;

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    int count = (int)luaL_len(L, -1);
    if (count <= 0) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "method");
        lua_getfield(L, -2, "pattern");
        lua_getfield(L, -3, "handler_id");

        const char *method_str = lua_tostring(L, -3);
        const char *pattern = lua_tostring(L, -2);
        int handler_id = (int)lua_tointeger(L, -1);

        if (method_str && pattern) {
            HlLuaRoute *route = hl_alloc_malloc(lua->base.alloc,
                                                  sizeof(HlLuaRoute));
            if (route) {
                route->lua = lua;
                route->handler_id = handler_id;
                hl_lua_track_route(lua, route);
                kl_router_add(router, method_str, pattern,
                              hl_lua_keel_handler, route, NULL);
            }
        }

        lua_pop(L, 3); /* method_str, pattern, handler_id */
        lua_pop(L, 1); /* route def table */
    }

    lua_pop(L, 1); /* __hull_route_defs table */
    return 0;
}

/* ── Server route wiring (with body reader factory) ────────────────── */

int hl_lua_wire_routes_server(HlLua *lua, KlServer *server,
                               void *(*alloc_fn)(size_t))
{
    (void)alloc_fn; /* routes always use Hull allocator */
    lua_State *L = lua->L;

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    int count = (int)luaL_len(L, -1);
    if (count <= 0) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "method");
        lua_getfield(L, -2, "pattern");
        lua_getfield(L, -3, "handler_id");

        const char *method_str = lua_tostring(L, -3);
        const char *pattern = lua_tostring(L, -2);
        int handler_id = (int)lua_tointeger(L, -1);

        if (method_str && pattern) {
            HlLuaRoute *route = hl_alloc_malloc(lua->base.alloc,
                                                  sizeof(HlLuaRoute));
            if (route) {
                route->lua = lua;
                route->handler_id = handler_id;
                hl_lua_track_route(lua, route);
                kl_server_route(server, method_str, pattern,
                                hl_lua_keel_handler, route,
                                hl_cap_body_factory);
            }
        }

        lua_pop(L, 3); /* method_str, pattern, handler_id */
        lua_pop(L, 1); /* route def table */
    }

    lua_pop(L, 1); /* __hull_route_defs table */

    /* Wire middleware from __hull_middleware */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_middleware");
    if (lua_istable(L, -1)) {
        int mw_count = (int)luaL_len(L, -1);
        for (int i = 1; i <= mw_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }

            lua_getfield(L, -1, "method");
            lua_getfield(L, -2, "pattern");
            lua_getfield(L, -3, "handler_id");

            const char *method_str = lua_tostring(L, -3);
            const char *pattern = lua_tostring(L, -2);
            int handler_id = (int)lua_tointeger(L, -1);

            if (method_str && pattern) {
                HlLuaRoute *ctx = hl_alloc_malloc(lua->base.alloc,
                                                    sizeof(HlLuaRoute));
                if (ctx) {
                    ctx->lua = lua;
                    ctx->handler_id = handler_id;
                    hl_lua_track_route(lua, ctx);
                    kl_server_use(server, method_str, pattern,
                                  hl_lua_keel_middleware, ctx);
                }
            }

            lua_pop(L, 3); /* method_str, pattern, handler_id */
            lua_pop(L, 1); /* middleware entry table */
        }
    }
    lua_pop(L, 1); /* __hull_middleware table */

    return 0;
}

/* ── Middleware dispatch ────────────────────────────────────────────── */

int hl_lua_dispatch_middleware(HlLua *lua, int handler_id,
                               KlRequest *req, KlResponse *res)
{
    if (!lua || !lua->L || !req || !res)
        return -1;

    /* Reset scratch arena for this middleware call */
    sh_arena_reset(lua->scratch);

    /* Get the handler function from the route registry */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_routes");
    if (!lua_istable(lua->L, -1)) {
        lua_pop(lua->L, 1);
        return -1;
    }

    lua_rawgeti(lua->L, -1, handler_id);
    if (!lua_isfunction(lua->L, -1)) {
        lua_pop(lua->L, 2); /* pop function + routes table */
        return -1;
    }

    /* Build request and response objects */
    hl_lua_make_request(lua->L, req);
    hl_lua_make_response(lua->L, res);

    /* Save a reference to the req table in the registry so we can
     * read ctx after pcall (which consumes the arguments). */
    lua_pushvalue(lua->L, -2); /* copy req table */
    lua_setfield(lua->L, LUA_REGISTRYINDEX, "__hull_mw_req");

    /* Call handler(req, res) — expect 1 return value */
    if (lua_pcall(lua->L, 2, 1, 0) != LUA_OK) {
        log_error("[hull:c] lua middleware error: %s",
                  lua_tostring(lua->L, -1));
        lua_pop(lua->L, 1); /* pop error message */
        lua_pop(lua->L, 1); /* pop routes table */
        /* Clean up registry ref */
        lua_pushnil(lua->L);
        lua_setfield(lua->L, LUA_REGISTRYINDEX, "__hull_mw_req");
        return -1;
    }

    /* Capture return value: 0 = continue, non-zero = short-circuit */
    int result = 0;
    if (lua_isnumber(lua->L, -1))
        result = (int)lua_tointeger(lua->L, -1);
    else if (lua_isboolean(lua->L, -1))
        result = lua_toboolean(lua->L, -1) ? 1 : 0;
    lua_pop(lua->L, 1); /* pop return value */

    /* Serialize req.ctx to JSON and store in req->ctx so the handler
     * dispatch (or next middleware) can reconstruct it. */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_mw_req");
    lua_getfield(lua->L, -1, "ctx");
    if (lua_istable(lua->L, -1)) {
        lua_getglobal(lua->L, "json");
        lua_getfield(lua->L, -1, "encode");
        lua_pushvalue(lua->L, -3); /* push ctx table */
        if (lua_pcall(lua->L, 1, 1, 0) == LUA_OK) {
            size_t json_len;
            const char *json_str = lua_tolstring(lua->L, -1, &json_len);
            if (json_str && json_len > 2 && json_len < 65536) { /* skip empty "{}" */
                if (req->ctx) {
                    size_t old_sz = strlen((char *)req->ctx) + 1;
                    hl_alloc_free(lua->base.alloc, req->ctx, old_sz);
                    req->ctx = NULL;
                }
                char *ctx_copy = hl_alloc_malloc(lua->base.alloc, json_len + 1);
                if (ctx_copy) {
                    memcpy(ctx_copy, json_str, json_len + 1);
                    req->ctx = ctx_copy;
                }
            }
            lua_pop(lua->L, 1); /* pop json string */
        } else {
            lua_pop(lua->L, 1); /* pop error */
        }
        lua_pop(lua->L, 1); /* pop json table */
    }
    lua_pop(lua->L, 1); /* pop ctx table */
    lua_pop(lua->L, 1); /* pop saved req table */

    /* Clean up registry ref */
    lua_pushnil(lua->L);
    lua_setfield(lua->L, LUA_REGISTRYINDEX, "__hull_mw_req");

    lua_pop(lua->L, 1); /* pop routes table */
    return result;
}

int hl_lua_keel_middleware(KlRequest *req, KlResponse *res, void *user_data)
{
    HlLuaRoute *ctx = (HlLuaRoute *)user_data;
    int rc = hl_lua_dispatch_middleware(ctx->lua, ctx->handler_id, req, res);
    if (rc < 0) {
        /* Middleware error — short-circuit with 500 */
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body(res, "Internal Server Error", 21);
        return 1; /* short-circuit */
    }
    return rc;
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
    return hl_manifest_extract(lua->L, out);
}

static void vt_lua_free_manifest_strings(HlRuntime *rt, HlManifest *m)
{
    /* Lua manifest strings are owned by the Lua state — no-op */
    (void)rt;
    (void)m;
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
    .free_manifest_strings = vt_lua_free_manifest_strings,
    .destroy             = vt_lua_destroy,
    .name                = "Lua",
};
