/* mod_buffer.c - Unified buffer protocol implementation (Lua)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/fs.h"
#include "hull/cap/image.h"

#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm_buffer.h"
#endif

/* -- Image pixel allocator -- */

static void *lua_image_px_malloc(void *ctx, size_t size)
{
    void *ud;
    lua_Alloc f = lua_getallocf((lua_State *)ctx, &ud);
    return f(ud, NULL, 0, size);
}

static void lua_image_px_free(void *ctx, void *ptr, size_t size)
{
    void *ud;
    lua_Alloc f = lua_getallocf((lua_State *)ctx, &ud);
    f(ud, ptr, size, 0);
}

void hl_lua_image_alloc(lua_State *L, HlImageAlloc *out)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    out->ctx = lua_tothread(L, -1);
    lua_pop(L, 1);
    out->malloc = lua_image_px_malloc;
    out->free   = lua_image_px_free;
}

/* ── Unified buffer protocol ──────────────────────────────────────── */

int lua_get_buffer(lua_State *L, int idx, HlBufferView *out)
{
    /* Try Lua string first (most common) */
    if (lua_isstring(L, idx)) {
        out->data = lua_tolstring(L, idx, &out->len);
        return 1;
    }
    /* Try MappedBuffer */
    HlMappedBuffer **mp = luaL_testudata(L, idx, HL_MMAP_MT);
    if (mp && *mp && !(*mp)->closed) {
        out->data = (*mp)->addr;
        out->len = (*mp)->len;
        return 1;
    }
#ifdef HL_ENABLE_WASM
    /* Try WasmBuffer */
    HlWasmBuffer **pp = luaL_testudata(L, idx, HL_WASM_BUF_MT);
    if (pp && *pp && !(*pp)->closed) {
        out->data = hl_wasm_buffer_data(*pp);
        out->len = hl_wasm_buffer_len(*pp);
        return 1;
    }
#endif
    /* Try HlImage (pixel data) */
    HlImage **imgp = luaL_testudata(L, idx, HL_IMAGE_MT);
    if (imgp && *imgp) {
        out->data = (*imgp)->pixels;
        out->len = (*imgp)->pixel_len;
        return 1;
    }
    return 0; /* not a buffer type */
}
