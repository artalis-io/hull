/*
 * modules.c — hull.* module registry (Lua)
 *
 * Registers every first-party hull.* module into `_LOADED` so the
 * custom `require()` in mod_fs.c can resolve them. Only intrinsic
 * modules — `app`, `log`, and the `hull` namespace — get installed
 * as Lua globals; everything else is import-only. The custom
 * require's gate (mod_fs.c) then rejects undeclared names per the
 * resolved module set.
 *
 * Module implementations live in per-capability mod_*.c files.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

/* Register a native module so `require("hull.X")` finds it via the
 * _LOADED bridge in hl_lua_require, but do NOT set it as a global. */
static void register_native_module(lua_State *L, const char *name,
                                    lua_CFunction openf)
{
    luaL_requiref(L, name, openf, 0);
    lua_pop(L, 1); /* drop the value left on the stack by luaL_requiref */
}

/* ════════════════════════════════════════════════════════════════════
 * Module registry — called by hl_lua_init() to register all
 * hull.* built-in modules.
 * ════════════════════════════════════════════════════════════════════ */

int hl_lua_register_modules(HlLua *lua)
{
    if (!lua || !lua->L)
        return -1;

    lua_State *L = lua->L;

    /* ── Intrinsic globals (always present) ────────────────────────── */

    /* hull.app — route registration is infrastructure. */
    luaL_requiref(L, "hull.app", luaopen_hull_app, 0);
    lua_setglobal(L, "app");

    /* Add app.router AFTER app is global so the embedded Lua source
     * can reference `app.X` directly. */
    hl_lua_install_app_router(L);

    /* hull.log — basic stderr logger. */
    luaL_requiref(L, "hull.log", luaopen_hull_log, 0);
    lua_setglobal(L, "log");

    /* hull global (hull.sleep, hull.gather, etc.) — runtime primitives. */
    luaL_requiref(L, "hull.hull", luaopen_hull_hull, 0);
    lua_setglobal(L, "hull");

    /* ── Import-only side-effect modules ───────────────────────────── */
    /* Reachable via `require("hull.X")` if declared in the app manifest;
     * the gate in hl_lua_require enforces the resolved module set. */

#ifdef HL_ENABLE_DB
    if (lua->base.db_handle)
        register_native_module(L, "hull.db", luaopen_hull_db);
#endif
    register_native_module(L, "hull.time",   luaopen_hull_time);
    register_native_module(L, "hull.env",    luaopen_hull_env);
    register_native_module(L, "hull.crypto", luaopen_hull_crypto);
#ifdef HL_ENABLE_HTTP_CLIENT
    register_native_module(L, "hull.http",   luaopen_hull_http);
    register_native_module(L, "hull.smtp",   luaopen_hull_smtp);
#endif
#ifdef HL_ENABLE_HTTP_SERVER
    register_native_module(L, "hull.server", luaopen_hull_server);
    register_native_module(L, "hull.ws",     luaopen_hull_ws);
#endif
    register_native_module(L, "hull.fs",     luaopen_hull_fs);
    register_native_module(L, "hull.image",  luaopen_hull_image);

    /* Internal bridge used by the hull.template stdlib — name starts
     * with underscore so it's not exposed as a first-party module via
     * the registry. */
    register_native_module(L, "hull._template", luaopen_hull_template_bridge);

    if (lua->base.thread_pool)
        register_native_module(L, "hull.worker", luaopen_hull_worker);

#ifdef HL_ENABLE_WASM
    if (lua->base.wasm_cache)
        register_native_module(L, "hull.compute", luaopen_hull_compute);
#endif

#ifdef HL_ENABLE_GPU
    if (lua->base.gpu_ctx)
        register_native_module(L, "hull.gpu", luaopen_hull_gpu);
#endif

#ifdef HL_ENABLE_HTTP_SERVER
    /* SSE stream metatable (used by app.sse handler dispatch). */
    hl_lua_sse_register_mt(L);
#endif

    return 0;
}
