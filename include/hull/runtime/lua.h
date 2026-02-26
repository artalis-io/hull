/*
 * runtime/lua.h — Lua 5.4 runtime for Hull
 *
 * Manages Lua VM lifecycle: init, sandbox, module loader, and request dispatch.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_LUA_H
#define HL_RUNTIME_LUA_H

#include <stddef.h>
#include <stdint.h>
#include "hull/limits.h"

/* Forward declarations */
typedef struct lua_State lua_State;
typedef struct KlRequest KlRequest;
typedef struct KlResponse KlResponse;
typedef struct SHArena SHArena;
typedef struct HlAllocator HlAllocator;
typedef struct sqlite3 sqlite3;
typedef struct HlFsConfig HlFsConfig;
typedef struct HlEnvConfig HlEnvConfig;
typedef struct HlHttpConfig HlHttpConfig;

/* ── Configuration ──────────────────────────────────────────────────── */

typedef struct {
    size_t  max_heap_bytes;       /* Lua heap limit (default: 64 MB) */
    int     sandbox;              /* 1 = sandbox (default), 0 = full access */
} HlLuaConfig;

/* Sensible defaults */
#define HL_LUA_CONFIG_DEFAULT {             \
    .max_heap_bytes = HL_LUA_DEFAULT_HEAP,  \
    .sandbox        = 1,                    \
}

/* ── Runtime context ────────────────────────────────────────────────── */

typedef struct HlLua {
    lua_State      *L;

    /* Capabilities (shared with JS runtime) */
    sqlite3        *db;
    HlFsConfig   *fs_cfg;
    HlEnvConfig  *env_cfg;
    HlHttpConfig *http_cfg;

    /* Process-level tracking allocator (NULL = raw malloc) */
    HlAllocator    *alloc;

    /* Lua sub-limit tracking */
    size_t          mem_used;
    size_t          mem_limit;

    /* Module search paths */
    const char     *app_dir;         /* application root directory */
    size_t          app_dir_size;    /* allocation size for tracked free */

    /* Per-request scratch arena (reset between dispatches) */
    SHArena        *scratch;

    /* Per-request response body (allocated via alloc, freed after dispatch) */
    char           *response_body;
    size_t          response_body_size;
} HlLua;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/*
 * Initialize the Lua runtime with sandboxing: no io, no os, no loadfile,
 * no dofile, no load. Registers hull.* modules.
 *
 * Returns 0 on success, -1 on error.
 */
int hl_lua_init(HlLua *lua, const HlLuaConfig *cfg);

/*
 * Load and execute the application entry point (app.lua).
 * This registers routes, middleware, config, etc.
 * Returns 0 on success, -1 on error.
 */
int hl_lua_load_app(HlLua *lua, const char *filename);

/*
 * Dispatch an HTTP request to the Lua handler that matched the route.
 * Called from Hull's Keel middleware/handler bridge.
 *
 * `handler_id` is the 1-based route index registered during app loading.
 * Creates Lua request/response objects, calls the handler, and
 * marshals the response back to KlResponse.
 *
 * Returns 0 on success, -1 on error.
 */
int hl_lua_dispatch(HlLua *lua, int handler_id,
                       KlRequest *req, KlResponse *res);

/*
 * Destroy the Lua runtime and free all resources.
 */
void hl_lua_free(HlLua *lua);

/* ── Module registration ────────────────────────────────────────────── */

/*
 * Register all hull.* built-in modules (app, db, time, env, crypto, log).
 * Called internally by hl_lua_init().
 */
int hl_lua_register_modules(HlLua *lua);

/*
 * Register Lua stdlib: custom require(), embedded module table,
 * loaded-module cache, and pre-loaded globals (json).
 * Called internally by hl_lua_init() after hl_lua_register_modules().
 */
int hl_lua_register_stdlib(HlLua *lua);

/* ── Error reporting ────────────────────────────────────────────────── */

/*
 * Print the current Lua error to stderr with stack trace.
 */
void hl_lua_dump_error(HlLua *lua);

/* ── Bindings (defined in lua_bindings.c) ───────────────────────────── */

/*
 * Push a Lua table representing the HTTP request onto the stack.
 */
void hl_lua_make_request(lua_State *L, KlRequest *req);

/*
 * Push a Lua userdata representing the HTTP response onto the stack.
 */
void hl_lua_make_response(lua_State *L, KlResponse *res);

#endif /* HL_RUNTIME_LUA_H */
