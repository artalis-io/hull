/*
 * wasm_stub.c - weak per-runtime compute-binding defaults (Lua).
 *
 * The compute.* binding (mod_compute) is a composable feature archive
 * (libhull_feature-wasm-lua.a, docs/wasm_feature.md). The pure runtime archive
 * still references two of its symbols: luaopen_hull_compute (modules.c's module
 * register) and lua_push_wasm_buffer (mod_gpu.c, a GPU result pushed as a
 * WasmBuffer). When the compute bridge is composed its strong defs win; when it
 * is NOT (a compute-free app, needs_wasm false) these weak no-ops satisfy the
 * link.
 *
 * These live in the PURE RUNTIME archive (where the Lua VM is linked), not the
 * base wasm_weakstub.c, so lua_push_wasm_buffer can push a balanced nil - the
 * base TU is also linked into a JS-only app, where lua_pushnil would be
 * undefined. Neither body is reached without the feature (modules.c gates the
 * compute register on wasm_cache, NULL when uncomposed; mod_gpu only pushes a
 * WasmBuffer when one exists, which requires the feature).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"   /* luaopen_hull_compute + lua_push_wasm_buffer decls, lua_State */

__attribute__((weak)) int luaopen_hull_compute(lua_State *L)
{
    (void)L;
    return 0;   /* not registered without wasm_cache; only satisfies the link */
}

__attribute__((weak)) void lua_push_wasm_buffer(lua_State *L, struct HlWasmBuffer *buf)
{
    (void)buf;
    lua_pushnil(L);   /* no WasmBuffer exists without the feature - balanced push */
}
