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
#include "internal.h"  /* hl_lua_request_register, hl_lua_sse_register_mt */
#include "hull/cap/tui.h"  /* hl_tui_feature_register_lua (composed-feature seam) */
#include "hull/http_feature.h"  /* hl_lua_register_http_modules (HTTP-feature seam, #114) */

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

    /* hull.app — the bootstrap intrinsic. Only `app.manifest`,
     * `app.main`, and `app.get_manifest` are present by default; all
     * other methods (get/post/use/router/ws/sse/every/daily) are
     * decorated onto `app` by lua_app_manifest based on the modules
     * the app declares (hull/http-server, hull/web/ws-server, hull/web/sse,
     * hull/timers). The C# partial-class pattern. */
    luaL_requiref(L, "hull.app", luaopen_hull_app, 0);
    lua_setglobal(L, "app");

    /* hull.log — pre-register so `require("hull.log")` resolves
     * via LUA_LOADED_TABLE without a disk hit. As of v0.1.0
     * release, hull/log is a DECLARED module (no longer intrinsic):
     * apps that use it must put "hull/log@1" in manifest.modules.
     * No lua_setglobal — `log` is not a runtime global anymore. */
    luaL_requiref(L, "hull.log", luaopen_hull_log, 0);
    lua_pop(L, 1); /* drop the module table left on the stack */

    /* hull global (hull.sleep, hull.gather, etc.) — runtime primitives. */
    luaL_requiref(L, "hull.hull", luaopen_hull_hull, 0);
    lua_setglobal(L, "hull");

    /* ── Import-only side-effect modules ───────────────────────────── */
    /* Reachable via `require("hull.X")` if declared in the app manifest;
     * the gate in hl_lua_require enforces the resolved module set. */

#ifdef HL_ENABLE_DB
    if (lua->base.db_registry)
        register_native_module(L, "hull.db", luaopen_hull_db);
#endif
    register_native_module(L, "hull.time",   luaopen_hull_time);
    register_native_module(L, "hull.env",    luaopen_hull_env);
    register_native_module(L, "hull.crypto", luaopen_hull_crypto);
    /* HTTP-dependent modules (http-client/server, smtp, ws-server/client) +
     * the sse/multipart metatables register through the HTTP-feature seam
     * (hl_lua_register_http_modules, below) so this core registry no longer
     * references the web-module openers. See hull/http_feature.h (#114). */
    register_native_module(L, "hull.fs",     luaopen_hull_fs);
    register_native_module(L, "hull.blob",   luaopen_hull_blob);
    register_native_module(L, "hull.mime",   luaopen_hull_mime);
    register_native_module(L, "hull.archive.tar", luaopen_hull_tar);
#ifdef HL_ENABLE_IMAGE
    register_native_module(L, "hull.image",  luaopen_hull_image);
#endif

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

    /* hull.gpu registers only when a GPU backend is present (monolithic
     * HL_ENABLE_GPU build or a composed --with=gpu feature). */
    if (lua->base.gpu_ctx)
        register_native_module(L, "hull.gpu", luaopen_hull_gpu);

    /* TUI native bridge — registered by the tui feature (a monolithic
     * HL_ENABLE_TUI build or a composed --with=tui). The base ships a weak
     * no-op; the feature's strong override registers hull._tui (the underscore
     * bridge that stdlib/lua/hull/tui.lua layers tui.run/list/confirm on top
     * of). Unconditional so a composed binary picks it up without HL_ENABLE_TUI. */
    hl_tui_feature_register_lua(L);

    /* HTTP-feature seam (#114): registers http-client/server, smtp,
     * ws-server/client + the sse/multipart metatables. Weak no-op in a base
     * with no HTTP composed; the strong override (runtime/lua/http_register.c)
     * wins on a full base / composed http feature. Consolidated here from the
     * former inline #ifdef HL_ENABLE_HTTP_* blocks - module registration into
     * _LOADED is order-independent, so behavior is unchanged. */
    hl_lua_register_http_modules(L);

    return 0;
}
