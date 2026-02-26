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
#include "hull/cap/fs.h"
#include "hull/cap/env.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

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
        hl_alloc_free(lua->alloc, ptr, osize);
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
        new_ptr = hl_alloc_malloc(lua->alloc, nsize);
    else
        new_ptr = hl_alloc_realloc(lua->alloc, ptr, osize, nsize);

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

    /* Save caller-set fields before zeroing */
    sqlite3 *db = lua->db;
    HlFsConfig *fs_cfg = lua->fs_cfg;
    HlEnvConfig *env_cfg = lua->env_cfg;
    HlAllocator *alloc = lua->alloc;

    memset(lua, 0, sizeof(*lua));

    /* Restore caller-set fields */
    lua->db = db;
    lua->fs_cfg = fs_cfg;
    lua->env_cfg = env_cfg;
    lua->alloc = alloc;
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
        /* Tool mode: open all standard libraries (io, os, etc.) */
        luaL_openlibs(lua->L);
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
    lua->scratch = hl_arena_create(lua->alloc, HL_SCRATCH_SIZE);
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
    char *app_dir = hl_alloc_malloc(lua->alloc, fn_len + 1);
    if (!app_dir)
        return -1;
    memcpy(app_dir, filename, fn_len + 1);
    char *last_slash = strrchr(app_dir, '/');
    if (last_slash)
        *last_slash = '\0';
    else {
        hl_alloc_free(lua->alloc, app_dir, fn_len + 1);
        app_dir = hl_alloc_malloc(lua->alloc, 2);
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
        return -1;
    }

    lua_pop(lua->L, 1); /* pop routes table */
    return 0;
}

void hl_lua_free(HlLua *lua)
{
    if (!lua)
        return;

    if (lua->L) {
        lua_close(lua->L);
        lua->L = NULL;
    }
    if (lua->app_dir) {
        hl_alloc_free(lua->alloc, (void *)lua->app_dir, lua->app_dir_size);
        lua->app_dir = NULL;
        lua->app_dir_size = 0;
    }
    hl_arena_free(lua->alloc, lua->scratch);
    lua->scratch = NULL;
    if (lua->response_body) {
        hl_alloc_free(lua->alloc, lua->response_body,
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
