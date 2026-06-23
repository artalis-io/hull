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
#include "hull/limits/runtime.h"  /* transitively pulls in core.h */
#include "hull/runtime.h"
#include "hull/cap/types.h"

/* Forward declarations */
typedef struct lua_State lua_State;
typedef struct KlRequest KlRequest;
typedef struct KlResponse KlResponse;
typedef struct KlRouter KlRouter;
typedef struct KlServer KlServer;
typedef struct KlConn KlConn;
typedef struct KlAsyncOp KlAsyncOp;
typedef struct SHArena SHArena;
typedef struct HlToolUnveilCtx HlToolUnveilCtx;
typedef struct HlAsyncCtx HlAsyncCtx;

/*
 * HlLuaTimer + HlLuaWorkerDispatchOp moved to
 * src/hull/runtime/lua/internal.h as part of architectural roadmap
 * item J — they're Hull-internal op structs that no external consumer
 * references.
 */

/* ── Configuration ──────────────────────────────────────────────────── */

typedef struct {
    size_t  max_heap_bytes;       /* Lua heap limit (default: 64 MB) */
    int     sandbox;              /* 1 = sandbox (default), 0 = full access */
    int64_t max_instructions;     /* 0 = unlimited (default: 100M) */
} HlLuaConfig;

/* Sensible defaults */
#define HL_LUA_CONFIG_DEFAULT {                      \
    .max_heap_bytes   = HL_LUA_DEFAULT_HEAP,         \
    .sandbox          = 1,                           \
    .max_instructions = HL_DEFAULT_INSTRUCTIONS,     \
}

/* ── Runtime context ────────────────────────────────────────────────── */

/*
 * Stability: Tier 4 (internal — see docs/stability.md). All fields below
 * `base` are Hull-internal implementation detail; the layout may change
 * in any release. External code must reach HlLua only via:
 *   - the lifecycle functions (hl_lua_init / _free / _load_app)
 *   - the runtime vtable (rt->vt->...)
 *   - the public `base` field (HlRuntime, Tier-2 stable)
 *
 * The struct stays in this public header for now so app_context.c and
 * tool.c can stack-allocate it; that storage strategy is itself Tier 4
 * and may move to a heap factory (hl_lua_create()) post-v0.1.0.
 */
typedef struct HlLua {
    HlRuntime       base;          /* vtable + shared capabilities — Tier 2 */

    lua_State      *L;

    /* Lua sub-limit tracking */
    size_t          mem_used;
    size_t          mem_limit;
    int64_t         max_instructions;  /* 0 = no limit */

    /* Module search paths */
    const char     *app_dir;         /* application root directory */
    size_t          app_dir_size;    /* allocation size for tracked free */

    /* Per-request scratch arena (reset between dispatches) */
    SHArena        *scratch;

    /* Tracked route allocations (freed in hl_lua_free) */
    void          **routes;
    size_t          route_count;
    size_t          route_cap;

    /* Tool-mode unveil context (NULL in sandbox mode) */
    HlToolUnveilCtx *tool_unveil_ctx;

    /* Tracked WS route allocations (freed in hl_lua_free) */
    void          **ws_routes;
    size_t          ws_route_count;
    size_t          ws_route_cap;

    /* Tracked WS config allocations (freed in hl_lua_free) */
    void          **ws_cfgs;
    size_t          ws_cfg_count;
    size_t          ws_cfg_cap;

    /* Tracked SSE route allocations (freed in hl_lua_free) */
    void          **sse_routes;
    size_t          sse_route_count;
    size_t          sse_route_cap;

    /* Tracked timer allocations (freed in hl_lua_free) */
    void          **timers;
    size_t          timer_count;
    size_t          timer_cap;

    /* Re-entrance guard: > 0 while dispatch or async resume is active */
    int         dispatch_depth;

    /* Per-request async state (set during dispatch, cleared after) */
    KlServer   *server;             /* set once during wire_routes_server */
    KlConn     *active_conn;        /* current connection (per dispatch) */
    KlRequest  *active_req;         /* current request (for compression check) */
    int         active_thread_ref;  /* registry ref to coroutine (LUA_NOREF = none) */
    lua_State  *active_co;          /* coroutine state (NULL = none) */

    /* Timer callback context: if non-NULL, hl_lua_async_cont_create
     * will wire timer_ctx on the new continuation. */
    void       *active_timer;       /* HlLuaTimer* during timer callback */

    /* Deferred-teardown hook: if set while a handler runs,
     * hl_lua_async_cont_create captures it so the action (e.g. ws on_close
     * conn teardown in ws.c) runs only once the async handler completes,
     * not while it is still suspended. Subsystem-agnostic. */
    void      (*active_on_complete)(struct HlLua *lua, void *ctx);
    void       *active_on_complete_ctx;

    /* UDF lifecycle: 1 while Lua state is valid, 0 before lua_close.
     * UDF destroy callbacks check this before calling luaL_unref. */
    int         udf_runtime_alive;

    /* CLI mode (app.main) — non-NULL while main's coroutine is alive.
     * When the existing async resume machinery sees the resumed coroutine
     * == cli_main_co and the status is no longer LUA_YIELD, it knows
     * main has terminated and calls kl_server_stop on `server` so the
     * event loop returns. */
    lua_State  *cli_main_co;
} HlLua;

/* ── Async push_result callback ─────────────────────────────────────── */

/*
 * Callback that pushes a driver result onto the Lua coroutine stack
 * on async resume. Each driver type provides its own implementation.
 * NULL = no result (e.g. hull.sleep).
 */
typedef void (*HlLuaPushResultFn)(lua_State *L, void *driver);

/* ── Worker dispatch ────────────────────────────────────────────────── */

/* Init hook: called when creating a per-worker Lua VM.
 * Use to register modules (e.g. db.*) into the worker environment. */
typedef int (*HlLuaWorkerInitFn)(lua_State *L);

/* Register an init hook for worker Lua VMs. Call before workers spawn. */
void hl_lua_worker_register_init(HlLuaWorkerInitFn fn);

/*
 * HlLuaWorkerDispatchOp + the hl_lua_worker_dispatch_* functions and
 * hl_lua_worker_db_init moved to src/hull/runtime/lua/internal.h
 * as part of architectural roadmap item J — they're Hull-internal
 * machinery, not consumed by any external code.
 */

/* ── Vtable ────────────────────────────────────────────────────────── */

extern const HlRuntimeVtable hl_lua_vtable;

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

/* ── Route wiring ──────────────────────────────────────────────────── */

/*
 * Per-route context: associates a Keel route with a Lua handler.
 *
 * multipart_config is set when the route was registered with
 *   app.post("/upload", handler, { multipart = {...} })
 * Non-NULL flags this as a streaming route: routes.c uses
 * kl_server_route_streaming_async + the multipart factory shim
 * instead of the regular kl_server_route + buffer factory. Stored as void* so
 * this public header doesn't pull in keel's body_reader_multipart.h
 * (the actual type is `KlMultipartConfig *`); freed alongside the
 * route in hl_lua_free.
 */
typedef struct {
    HlLua *lua;
    int    handler_id;
    void  *multipart_config;  /* (KlMultipartConfig *), NULL = not streaming */
} HlLuaRoute;

/*
 * Keel handler bridge: dispatches a request to the Lua handler.
 */
void hl_lua_keel_handler(KlRequest *req, KlResponse *res, void *user_data);

/*
 * Wire Lua routes from __hull_route_defs into a KlRouter.
 * Allocates HlLuaRoute structs using malloc (caller must track).
 *
 * `alloc_fn` is called to allocate per-route context. Pass NULL to use malloc.
 *
 * Returns 0 on success, -1 on error.
 */
int hl_lua_wire_routes(HlLua *lua, KlRouter *router);

/*
 * Wire Lua routes into a KlServer (with body reader factory).
 * `alloc_fn` allocates per-route context (pass NULL to use malloc).
 * Returns 0 on success, -1 on error.
 */
int hl_lua_wire_routes_server(HlLua *lua, KlServer *server,
                               void *(*alloc_fn)(size_t));

/*
 * Dispatch a middleware call to the Lua handler.
 * Returns 0 (continue), positive (short-circuit), or -1 (error).
 */
int hl_lua_dispatch_middleware(HlLua *lua, int handler_id,
                               KlRequest *req, KlResponse *res);

/*
 * Keel middleware bridge: dispatches a request to the Lua middleware.
 * Returns 0 (continue) or non-zero (short-circuit).
 */
int hl_lua_keel_middleware(KlRequest *req, KlResponse *res, void *user_data);

#endif /* HL_RUNTIME_LUA_H */
