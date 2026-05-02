/*
 * modules.c — hull.* module registry (Lua)
 *
 * Registers all hull.* built-in modules as Lua globals.
 * Module implementations are in per-capability mod_*.c files.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

/* ════════════════════════════════════════════════════════════════════
 * Module registry — called by hl_lua_init() to register all
 * hull.* built-in modules.
 * ════════════════════════════════════════════════════════════════════ */

int hl_lua_register_modules(HlLua *lua)
{
    if (!lua || !lua->L)
        return -1;

    lua_State *L = lua->L;

    /* Register hull.app as a global */
    luaL_requiref(L, "hull.app", luaopen_hull_app, 0);
    lua_setglobal(L, "app");

    /* Register hull.db (only if database is available) */
    if (lua->base.db_handle) {
        luaL_requiref(L, "hull.db", luaopen_hull_db, 0);
        lua_setglobal(L, "db");
    }

    /* Register hull.time */
    luaL_requiref(L, "hull.time", luaopen_hull_time, 0);
    lua_setglobal(L, "time");

    /* Register hull.env */
    luaL_requiref(L, "hull.env", luaopen_hull_env, 0);
    lua_setglobal(L, "env");

    /* Register hull.crypto */
    luaL_requiref(L, "hull.crypto", luaopen_hull_crypto, 0);
    lua_setglobal(L, "crypto");

    /* Register hull.log */
    luaL_requiref(L, "hull.log", luaopen_hull_log, 0);
    lua_setglobal(L, "log");

    /* Register hull.http — always available; per-function checks enforce
     * that http_cfg is set (wired from manifest after load_app). */
    luaL_requiref(L, "hull.http", luaopen_hull_http, 0);
    lua_setglobal(L, "http");

    /* Register hull.smtp — always available; per-function checks enforce
     * that smtp_cfg is set (wired from manifest after load_app). */
    luaL_requiref(L, "hull.smtp", luaopen_hull_smtp, 0);
    lua_setglobal(L, "smtp");

    /* Register hull._template — internal bridge for hull.template stdlib */
    luaL_requiref(L, "hull._template", luaopen_hull_template_bridge, 0);
    lua_setglobal(L, "_template");

    /* Register hull.worker (only if thread pool is available) */
    if (lua->base.thread_pool) {
        luaL_requiref(L, "hull.worker", luaopen_hull_worker, 0);
        lua_setglobal(L, "worker");
    }

    /* Register hull.server (always available) */
    luaL_requiref(L, "hull.server", luaopen_hull_server, 0);
    lua_setglobal(L, "server");

    /* Register hull.ws (always available — broadcast/connections are no-ops
     * when no WS endpoints are registered) */
    luaL_requiref(L, "hull.ws", luaopen_hull_ws, 0);
    lua_setglobal(L, "ws");

    /* Register SSE stream metatable (used by app.sse handler dispatch) */
    hl_lua_sse_register_mt(L);

    /* Register hull global (hull.sleep, hull.gather, etc.) */
    luaL_requiref(L, "hull.hull", luaopen_hull_hull, 0);
    lua_setglobal(L, "hull");

    /* Register hull.fs — always available; per-function checks enforce
     * that fs_cfg is set (wired from manifest after load_app). */
    luaL_requiref(L, "hull.fs", luaopen_hull_fs, 0);
    lua_setglobal(L, "fs");

    /* Register hull.image — always available */
    luaL_requiref(L, "hull.image", luaopen_hull_image, 0);
    lua_setglobal(L, "image");

#ifdef HL_ENABLE_WASM
    /* Register hull.compute (only if WASM runtime is available) */
    if (lua->base.wasm_cache) {
        luaL_requiref(L, "hull.compute", luaopen_hull_compute, 0);
        lua_setglobal(L, "compute");
    }
#endif

#ifdef HL_ENABLE_GPU
    /* Register hull.gpu (only if GPU context is available) */
    if (lua->base.gpu_ctx) {
        luaL_requiref(L, "hull.gpu", luaopen_hull_gpu, 0);
        lua_setglobal(L, "gpu");
    }
#endif

    return 0;
}
