/*
 * lua_runtime.h — Lua 5.4 runtime for Hull
 *
 * Manages Lua VM lifecycle: init, sandbox, module loader, and request dispatch.
 * Mirrors the QuickJS runtime interface (js_runtime.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HULL_LUA_RUNTIME_H
#define HULL_LUA_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include "hull/hull_cap.h"

/* Forward declarations */
typedef struct lua_State lua_State;
typedef struct KlRequest KlRequest;
typedef struct KlResponse KlResponse;

/* ── Configuration ──────────────────────────────────────────────────── */

typedef struct {
    size_t  max_heap_bytes;       /* Lua heap limit (default: 64 MB) */
} HullLuaConfig;

/* Sensible defaults */
#define HULL_LUA_CONFIG_DEFAULT {           \
    .max_heap_bytes = 64 * 1024 * 1024,     \
}

/* ── Runtime context ────────────────────────────────────────────────── */

typedef struct HullLua {
    lua_State      *L;

    /* Capabilities (shared with JS runtime) */
    sqlite3        *db;
    HullFsConfig   *fs_cfg;
    HullEnvConfig  *env_cfg;
    HullHttpConfig *http_cfg;

    /* Memory tracking */
    size_t          mem_used;
    size_t          mem_limit;

    /* Module search paths */
    const char     *app_dir;         /* application root directory */

    /* Route handlers stored in Lua registry:
     *   registry["__hull_routes"]     = { [1]=fn, [2]=fn, ... }
     *   registry["__hull_route_defs"] = { [1]={method,pattern,handler_id}, ... }
     */
} HullLua;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/*
 * Initialize the Lua runtime with sandboxing: no io, no os, no loadfile,
 * no dofile, no load. Registers hull.* modules.
 *
 * Returns 0 on success, -1 on error.
 */
int hull_lua_init(HullLua *lua, const HullLuaConfig *cfg);

/*
 * Load and execute the application entry point (app.lua).
 * This registers routes, middleware, config, etc.
 * Returns 0 on success, -1 on error.
 */
int hull_lua_load_app(HullLua *lua, const char *filename);

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
int hull_lua_dispatch(HullLua *lua, int handler_id,
                       KlRequest *req, KlResponse *res);

/*
 * Destroy the Lua runtime and free all resources.
 */
void hull_lua_free(HullLua *lua);

/* ── Module registration ────────────────────────────────────────────── */

/*
 * Register all hull.* built-in modules (app, db, time, env, crypto, log).
 * Called internally by hull_lua_init().
 */
int hull_lua_register_modules(HullLua *lua);

/* ── Error reporting ────────────────────────────────────────────────── */

/*
 * Print the current Lua error to stderr with stack trace.
 */
void hull_lua_dump_error(HullLua *lua);

/* ── Bindings (defined in lua_bindings.c) ───────────────────────────── */

/*
 * Push a Lua table representing the HTTP request onto the stack.
 */
void hull_lua_make_request(lua_State *L, KlRequest *req);

/*
 * Push a Lua userdata representing the HTTP response onto the stack.
 */
void hull_lua_make_response(lua_State *L, KlResponse *res);

#endif /* HULL_LUA_RUNTIME_H */
