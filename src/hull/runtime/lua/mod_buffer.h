/* mod_buffer.h — Shared declarations for per-module Lua binding files
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_LUA_MOD_BUFFER_H
#define HULL_LUA_MOD_BUFFER_H

#include "hull/runtime/lua.h"
#include "hull/utils/buffer.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

/* ── Metatable name constants ─────────────────────────────────────── */

#define HL_MMAP_MT     "HlMappedBuffer"
#define HL_WASM_BUF_MT "HlWasmBuffer"
#define HL_IMAGE_MT    "HlImage"

/* ── Inline helpers used by most modules ──────────────────────────── */

/* Retrieve HlLua context from Lua registry */
static inline HlLua *get_hl_lua(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return lua;
}

/* Compiler-safe memory zeroing that won't be optimized away */
static inline void secure_zero(void *p, size_t n)
{
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

/* ── Unified buffer protocol ──────────────────────────────────────── */

/*
 * Extract a buffer view from a Lua value at stack index `idx`.
 * Accepts: string, MappedBuffer (HL_MMAP_MT), WasmBuffer (HL_WASM_BUF_MT).
 * Returns 1 on success (fills out->data and out->len), 0 if not a buffer type.
 * The data pointer is borrowed — valid only while the Lua value is alive.
 */
int lua_get_buffer(lua_State *L, int idx, HlBufferView *out);

/* ── Module openers — called from hl_lua_register_modules() ──────── */

int luaopen_hull_app(lua_State *L);
/* Installs the app.router(prefix, opts) method by evaluating an
 * embedded Lua source against the live `app` global. Must be called
 * AFTER `lua_setglobal(L, "app")`. Idempotent per Lua state. */
int hl_lua_install_app_router(lua_State *L);
int luaopen_hull_db(lua_State *L);
int luaopen_hull_kv_native(lua_State *L);   /* hull.kv._native (KV connection cap) */
int luaopen_hull_time(lua_State *L);
int luaopen_hull_env(lua_State *L);
int luaopen_hull_crypto(lua_State *L);
int luaopen_hull_http(lua_State *L);
int luaopen_hull_smtp(lua_State *L);
int luaopen_hull_log(lua_State *L);
int luaopen_hull_template_bridge(lua_State *L);
int luaopen_hull_worker(lua_State *L);
int luaopen_hull_server(lua_State *L);
int luaopen_hull_fs(lua_State *L);
int luaopen_hull_image(lua_State *L);
int luaopen_hull_blob(lua_State *L);
int luaopen_hull_mime(lua_State *L);
int luaopen_hull_tar(lua_State *L);

#ifdef HL_ENABLE_WASM
int luaopen_hull_compute(lua_State *L);

/* Push an HlWasmBuffer* as Lua userdata. Takes ownership of buf.
 * Used by mod_compute.c and mod_gpu.c. */
struct HlWasmBuffer;
void lua_push_wasm_buffer(lua_State *L, struct HlWasmBuffer *buf);
#endif

int luaopen_hull_gpu(lua_State *L);

#ifdef HL_ENABLE_TUI
int luaopen_hull_tui(lua_State *L);
#endif

struct HlWsConn;

/* SSE stream userdata (used by runtime.c for stream lifecycle) */
#include <keel/sse.h>
struct HlSseStreamUD {
    KlSse    sse;     /* Keel SSE context */
    int      closed;  /* 1 after close */
};

/* SSE stream helpers (defined in lua/mod_sse.c) */
void hl_lua_sse_register_mt(lua_State *L);
struct HlSseStreamUD *hl_lua_sse_push_stream(lua_State *L, KlResponse *res);

/* hull.ws module (defined in lua/mod_ws.c) */
int luaopen_hull_ws_server(lua_State *L);
int luaopen_hull_ws_client(lua_State *L);

/* conn userdata helpers (defined in lua/mod_ws.c) */
void hl_lua_ws_push_conn(lua_State *L, struct HlWsConn *conn);
void hl_lua_ws_invalidate_conn(lua_State *L, struct HlWsConn *conn);
void hl_lua_ws_register_conn_mt(lua_State *L);

/* hull async module (defined in lua/async.c) */
extern int luaopen_hull_hull(lua_State *L);

#endif /* HULL_LUA_MOD_BUFFER_H */
