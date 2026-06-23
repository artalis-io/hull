/*
 * mod_image.c — hull.image module: image decode/encode/create
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/image.h"
#include "hull/cap/fs.h"
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm_buffer.h"
#endif

#include <stdlib.h>
#include <string.h>

#define HL_IMAGE_MT "HlImage"

/* ── Helpers ───────────────────────────────────────────────────────── */

static HlImage **check_image(lua_State *L, int idx)
{
    return (HlImage **)luaL_checkudata(L, idx, HL_IMAGE_MT);
}

/* ── Methods on HlImage ────────────────────────────────────────────── */

static int l_image_width(lua_State *L)
{
    HlImage **imgp = check_image(L, 1);
    if (!*imgp) return luaL_error(L, "image already closed");
    lua_pushinteger(L, (lua_Integer)(*imgp)->width);
    return 1;
}

static int l_image_height(lua_State *L)
{
    HlImage **imgp = check_image(L, 1);
    if (!*imgp) return luaL_error(L, "image already closed");
    lua_pushinteger(L, (lua_Integer)(*imgp)->height);
    return 1;
}

static int l_image_format(lua_State *L)
{
    HlImage **imgp = check_image(L, 1);
    if (!*imgp) return luaL_error(L, "image already closed");
    lua_pushstring(L, hl_image_format_name((*imgp)->format));
    return 1;
}

static int l_image_size(lua_State *L)
{
    HlImage **imgp = check_image(L, 1);
    if (!*imgp) return luaL_error(L, "image already closed");
    lua_pushinteger(L, (lua_Integer)(*imgp)->pixel_len);
    return 1;
}

static int l_image_pixels(lua_State *L)
{
    HlImage **imgp = check_image(L, 1);
    if (!*imgp) return luaL_error(L, "image already closed");
    lua_pushlstring(L, (const char *)(*imgp)->pixels, (*imgp)->pixel_len);
    return 1;
}

static int l_image_close(lua_State *L)
{
    HlImage **imgp = check_image(L, 1);
    if (*imgp) {
        hl_image_free(*imgp);
        *imgp = NULL;
    }
    return 0;
}

static int l_image_gc(lua_State *L)
{
    HlImage **imgp = (HlImage **)luaL_testudata(L, 1, HL_IMAGE_MT);
    if (imgp && *imgp) {
        hl_image_free(*imgp);
        *imgp = NULL;
    }
    return 0;
}

static int l_image_tostring(lua_State *L)
{
    HlImage **imgp = check_image(L, 1);
    if (!*imgp) {
        lua_pushstring(L, "Image(closed)");
    } else {
        lua_pushfstring(L, "Image(%dx%d %s)",
                        (*imgp)->width, (*imgp)->height,
                        hl_image_format_name((*imgp)->format));
    }
    return 1;
}

/* ── Module functions ──────────────────────────────────────────────── */

/* image.new(w, h, format_str, data) -> HlImage userdata */
static int l_image_new(lua_State *L)
{
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    const char *fmt_name = luaL_checkstring(L, 3);

    int fmt = hl_image_format_from_name(fmt_name);
    if (fmt < 0)
        return luaL_error(L, "unknown image format: %s", fmt_name);

    HlBufferView view;
    if (!lua_get_buffer(L, 4, &view))
        return luaL_error(L, "image.new: arg 4 must be a buffer (string, MappedBuffer, or WasmBuffer)");

    HlImage *img = hl_image_new((uint32_t)w, (uint32_t)h,
                                 (HlImageFormat)fmt,
                                 view.data, view.len, NULL);
    if (!img)
        return luaL_error(L, "image.new: invalid dimensions or data size");

    HlImage **udata = (HlImage **)lua_newuserdata(L, sizeof(HlImage *));
    *udata = img;
    luaL_getmetatable(L, HL_IMAGE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* image.from_buffer(buf, w, h, format_str) -> HlImage userdata (borrowed) */
static int l_image_from_buffer(lua_State *L)
{
    HlBufferView view;
    if (!lua_get_buffer(L, 1, &view))
        return luaL_error(L, "image.from_buffer: arg 1 must be a buffer");

    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    const char *fmt_name = luaL_checkstring(L, 4);

    int fmt = hl_image_format_from_name(fmt_name);
    if (fmt < 0)
        return luaL_error(L, "unknown image format: %s", fmt_name);

    /* Refcountable zero-copy sources (mmap / WASM buffer) are borrowed and
     * their teardown is deferred until this image is freed (so buf:close()
     * while an image borrows it can't dangle the pixels). Other sources
     * (Lua string, image) have no stable object to pin, so they are copied. */
    HlMappedBuffer **mp = (HlMappedBuffer **)luaL_testudata(L, 1, HL_MMAP_MT);
    HlMappedBuffer *mmap_src = (mp && *mp && !(*mp)->closed) ? *mp : NULL;
#ifdef HL_ENABLE_WASM
    HlWasmBuffer **wp = (HlWasmBuffer **)luaL_testudata(L, 1, HL_WASM_BUF_MT);
    HlWasmBuffer *wasm_src = (wp && *wp && !(*wp)->closed) ? *wp : NULL;
#endif

    HlImage *img;
    if (mmap_src) {
        img = hl_image_from_view((uint32_t)w, (uint32_t)h, (HlImageFormat)fmt,
                                 view.data, view.len, NULL);
        if (img) {
            hl_cap_fs_mmap_borrow(mmap_src);
            img->on_free = hl_cap_fs_mmap_release;
            img->on_free_ctx = mmap_src;
        }
    }
#ifdef HL_ENABLE_WASM
    else if (wasm_src) {
        img = hl_image_from_view((uint32_t)w, (uint32_t)h, (HlImageFormat)fmt,
                                 view.data, view.len, NULL);
        if (img) {
            hl_wasm_buffer_borrow(wasm_src);
            img->on_free = hl_wasm_buffer_release;
            img->on_free_ctx = wasm_src;
        }
    }
#endif
    else {
        img = hl_image_new((uint32_t)w, (uint32_t)h, (HlImageFormat)fmt,
                           view.data, view.len, NULL);
    }
    if (!img)
        return luaL_error(L, "image.from_buffer: invalid dimensions or data size");

    HlImage **udata = (HlImage **)lua_newuserdata(L, sizeof(HlImage *));
    *udata = img;
    luaL_getmetatable(L, HL_IMAGE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* image.decode(data, format?) -> HlImage userdata */
static int l_image_decode(lua_State *L)
{
    HlBufferView view;
    if (!lua_get_buffer(L, 1, &view))
        return luaL_error(L, "image.decode: arg 1 must be a buffer");

    const char *fmt_name = luaL_optstring(L, 2, NULL);
    const char *err_msg = NULL;

    HlImage *img = hl_image_decode(view.data, view.len,
                                    fmt_name, NULL, &err_msg);
    if (!img) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "decode failed");
        return 2;
    }

    HlImage **udata = (HlImage **)lua_newuserdata(L, sizeof(HlImage *));
    *udata = img;
    luaL_getmetatable(L, HL_IMAGE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* image.encode(img, format, opts?) -> string */
static int l_image_encode(lua_State *L)
{
    HlImage **imgp = check_image(L, 1);
    if (!*imgp) return luaL_error(L, "image already closed");

    const char *fmt_name = luaL_checkstring(L, 2);

    int quality = 90;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "quality");
        if (lua_isinteger(L, -1))
            quality = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    void *out = NULL;
    size_t out_len = 0;
    const char *err_msg = NULL;

    int rc = hl_image_encode(*imgp, fmt_name, quality,
                              &out, &out_len, NULL, &err_msg);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "encode failed");
        return 2;
    }

    lua_pushlstring(L, (const char *)out, out_len);
    free(out);
    return 1;
}

/* ── Module registration ───────────────────────────────────────────── */

static const luaL_Reg image_methods[] = {
    { "width",    l_image_width },
    { "height",   l_image_height },
    { "format",   l_image_format },
    { "size",     l_image_size },
    { "pixels",   l_image_pixels },
    { "close",    l_image_close },
    { NULL, NULL }
};

/* image.to_wasm(img) → string: [w:u32 LE][h:u32 LE][fmt:u8][pixels...] */
static int l_image_to_wasm(lua_State *L)
{
    HlImage **imgp = check_image(L, 1);
    if (!*imgp) return luaL_error(L, "image already closed");
    HlImage *img = *imgp;

    size_t hdr = 9; /* 4 + 4 + 1 */
    if (img->pixel_len > SIZE_MAX - hdr)
        return luaL_error(L, "image.to_wasm: image too large");
    size_t total = hdr + img->pixel_len;
    char *buf = malloc(total);
    if (!buf) return luaL_error(L, "out of memory");

    /* Little-endian width, height */
    buf[0] = (char)(img->width & 0xFF);
    buf[1] = (char)((img->width >> 8) & 0xFF);
    buf[2] = (char)((img->width >> 16) & 0xFF);
    buf[3] = (char)((img->width >> 24) & 0xFF);
    buf[4] = (char)(img->height & 0xFF);
    buf[5] = (char)((img->height >> 8) & 0xFF);
    buf[6] = (char)((img->height >> 16) & 0xFF);
    buf[7] = (char)((img->height >> 24) & 0xFF);
    buf[8] = (char)(img->format + 1); /* 1-based: RGBA8=1, R8=2, RGBA16F=3, R32F=4 */
    memcpy(buf + hdr, img->pixels, img->pixel_len);

    lua_pushlstring(L, buf, total);
    free(buf);
    return 1;
}

/* image.from_wasm(bytes) → HlImage: parse [w:u32 LE][h:u32 LE][fmt:u8][pixels...] */
static int l_image_from_wasm(lua_State *L)
{
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    if (len < 9) return luaL_error(L, "image.from_wasm: data too short (need 9+ bytes)");

    uint32_t w = (uint8_t)data[0] | ((uint8_t)data[1] << 8) |
                 ((uint8_t)data[2] << 16) | ((uint8_t)data[3] << 24);
    uint32_t h = (uint8_t)data[4] | ((uint8_t)data[5] << 8) |
                 ((uint8_t)data[6] << 16) | ((uint8_t)data[7] << 24);
    int fmt_byte = (uint8_t)data[8];
    if (fmt_byte < 1 || fmt_byte > 4)
        return luaL_error(L, "image.from_wasm: invalid format byte %d", fmt_byte);

    HlImageFormat fmt = (HlImageFormat)(fmt_byte - 1);
    int bpp = hl_image_bpp(fmt);
    /* Overflow-safe size check: w * h * bpp */
    if (w > HL_IMAGE_MAX_DIM || h > HL_IMAGE_MAX_DIM ||
        (w > 0 && h > SIZE_MAX / 2 / (size_t)w / (size_t)bpp))
        return luaL_error(L, "image.from_wasm: dimensions overflow");
    size_t expected = (size_t)w * (size_t)h * (size_t)bpp;
    size_t pixel_len = len - 9;
    if (pixel_len < expected)
        return luaL_error(L, "image.from_wasm: pixel data too short (%zu < %zu)", pixel_len, expected);

    HlImage *img = hl_image_new(w, h, fmt, data + 9, expected, NULL);
    if (!img) return luaL_error(L, "image.from_wasm: failed to create image");

    HlImage **ud = (HlImage **)lua_newuserdata(L, sizeof(HlImage *));
    *ud = img;
    luaL_setmetatable(L, HL_IMAGE_MT);
    return 1;
}

static const luaL_Reg image_funcs[] = {
    { "new",         l_image_new },
    { "from_buffer", l_image_from_buffer },
    { "decode",      l_image_decode },
    { "encode",      l_image_encode },
    { "to_wasm",     l_image_to_wasm },
    { "from_wasm",   l_image_from_wasm },
    { NULL, NULL }
};

int luaopen_hull_image(lua_State *L)
{
    /* Create HlImage metatable */
    luaL_newmetatable(L, HL_IMAGE_MT);

    /* __index = methods table */
    lua_newtable(L);
    luaL_setfuncs(L, image_methods, 0);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, l_image_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, l_image_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pop(L, 1); /* pop metatable */

    /* Create module table */
    luaL_newlib(L, image_funcs);
    return 1;
}
