/* mod_gpu.c — hull.gpu module: GPU compute via wgpu-native
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_GPU

#include "mod_buffer.h"
#include "hull/cap/gpu.h"
#include "hull/cap/image.h"
#include "hull/worker_gpu.h"
#include "hull/async.h"
#include "hull/vfs.h"

/* Forward declaration */
static int lua_parse_texture_descs(lua_State *L, int tbl_idx,
                                    HlGpuTextureDesc *out, int max);

#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm_buffer.h"
#include "hull/alloc.h"
#endif

#include <keel/server.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.gpu module — GPU compute (wgpu-native)
 *
 * gpu.available() -> boolean
 * gpu.devices() -> { {id=0, name="..."}, ... }
 * gpu.compile(name, wgsl) -> true, err
 * gpu.dispatch(name, opts) -> output, err
 * gpu.async.dispatch(name, opts) -> {result=..., error=...}
 * gpu.buffer(name, data, opts?) -> true, err
 * gpu.buffer(name, nil) -> destroy
 * gpu.buffer_read(name) -> data, err
 * ════════════════════════════════════════════════════════════════════ */

static HlGpuCtx *lua_get_gpu_ctx(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return lua ? lua->base.gpu_ctx : NULL;
}

static int l_gpu_available(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    lua_pushboolean(L, hl_cap_gpu_available(ctx));
    return 1;
}

static int l_gpu_devices(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    int count = hl_cap_gpu_device_count(ctx);
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, i);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, hl_cap_gpu_device_name(ctx, i));
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* Load WGSL shader from shaders/<name>.wgsl (VFS or disk) */
static int l_gpu_load(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    const char *name = luaL_checkstring(L, 1);
    HlLua *lua = get_hl_lua(L);

    /* Validate shader name: reject path traversal */
    if (!name[0] || name[0] == '/' || strstr(name, "..") || strchr(name, '\\'))
        return luaL_error(L, "invalid shader name: %s", name);

    /* Try VFS first: shaders/<name>.wgsl */
    char vfs_name[512];
    snprintf(vfs_name, sizeof(vfs_name), "shaders/%s.wgsl", name);
    const HlEntry *entry = NULL;
    if (lua && lua->base.app_vfs)
        entry = hl_vfs_find(lua->base.app_vfs, vfs_name);

    const char *wgsl = NULL;
    size_t wgsl_len = 0;
    char *file_buf = NULL;

    if (entry && entry->data) {
        wgsl = (const char *)entry->data;
        wgsl_len = entry->len;
    } else if (lua && lua->base.app_vfs && lua->base.app_vfs->root_dir) {
        /* Try filesystem: <app_dir>/shaders/<name>.wgsl */
        char path[4096];
        int pn = snprintf(path, sizeof(path), "%s/shaders/%s.wgsl",
                 lua->base.app_vfs->root_dir, name);
        if (pn < 0 || (size_t)pn >= sizeof(path)) {
            lua_pushnil(L);
            lua_pushstring(L, "shader path too long");
            return 2;
        }
        FILE *f = fopen(path, "r");
        if (f) {
            long flen = -1;
            if (fseek(f, 0, SEEK_END) == 0) {
                flen = ftell(f);
                if (fseek(f, 0, SEEK_SET) != 0) flen = -1;
            }
            if (flen > 0 && flen < 10 * 1024 * 1024) {
                file_buf = malloc((size_t)flen + 1);
                if (file_buf && fread(file_buf, 1, (size_t)flen, f) == (size_t)flen) {
                    file_buf[flen] = '\0';
                    wgsl = file_buf;
                    wgsl_len = (size_t)flen;
                }
            }
            fclose(f);
        }
    }

    if (!wgsl) {
        free(file_buf);
        lua_pushnil(L);
        lua_pushfstring(L, "shader '%s' not found in shaders/", name);
        return 2;
    }

    int rc = hl_cap_gpu_compile(ctx, -1, name, wgsl, wgsl_len);
    free(file_buf);

    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "shader_error");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_gpu_compile(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    const char *name = luaL_checkstring(L, 1);
    size_t wgsl_len;
    const char *wgsl = luaL_checklstring(L, 2, &wgsl_len);

    int device = -1;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "device");
        if (!lua_isnil(L, -1))
            device = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    int rc = hl_cap_gpu_compile(ctx, device, name, wgsl, wgsl_len);
    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "shader_error");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_gpu_dispatch(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    HlGpuDispatchOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.device = -1;

    /* Parse device */
    lua_getfield(L, 2, "device");
    if (!lua_isnil(L, -1))
        opts.device = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    /* Parse workgroups */
    lua_getfield(L, 2, "workgroups");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "x");
        opts.workgroups.x = (uint32_t)luaL_optinteger(L, -1, 1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "y");
        opts.workgroups.y = (uint32_t)luaL_optinteger(L, -1, 1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "z");
        opts.workgroups.z = (uint32_t)luaL_optinteger(L, -1, 1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    /* Parse output buffer index (1-indexed in Lua).
     * output = N   -> read back buffer N (1-indexed)
     * output = false -> fire-and-forget (skip readback)
     * output absent -> default to buffer 1 (backward compat) */
    lua_getfield(L, 2, "output");
    if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
        opts.output_buffer = HL_GPU_OUTPUT_NONE;
    } else {
        opts.output_buffer = (int)luaL_optinteger(L, -1, 1) - 1;
    }
    lua_pop(L, 1);

    /* Parse timeout */
    lua_getfield(L, 2, "timeout");
    if (!lua_isnil(L, -1))
        opts.timeout_ms = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    /* buffer = true -> return WasmBuffer instead of string (zero-copy GPU->WASM) */
    int want_buffer = 0;
    lua_getfield(L, 2, "buffer");
    if (lua_toboolean(L, -1)) want_buffer = 1;
    lua_pop(L, 1);

    /* Parse uniforms */
    lua_getfield(L, 2, "uniforms");
    if (lua_isstring(L, -1)) {
        opts.uniforms = lua_tolstring(L, -1, &opts.uniforms_len);
    }
    lua_pop(L, 1);

    /* Parse buffers */
    lua_getfield(L, 2, "buffers");
    int buf_count = 0;
    HlGpuBufferDesc bufs[16];
    memset(bufs, 0, sizeof(bufs));
    if (lua_istable(L, -1)) {
        buf_count = (int)lua_rawlen(L, -1);
        if (buf_count > 16) buf_count = 16;
        for (int i = 0; i < buf_count; i++) {
            lua_rawgeti(L, -1, i + 1);
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "name");
                bufs[i].name = lua_tostring(L, -1);
                lua_pop(L, 1);

                lua_getfield(L, -1, "data");
                {
                    HlBufferView bv;
                    if (lua_get_buffer(L, -1, &bv)) {
                        bufs[i].data = bv.data;
                        bufs[i].size = bv.len;
                    }
                }
                lua_pop(L, 1);

                lua_getfield(L, -1, "size");
                if (!lua_isnil(L, -1))
                    bufs[i].size = (size_t)lua_tointeger(L, -1);
                lua_pop(L, 1);

                lua_getfield(L, -1, "usage");
                const char *usage = lua_tostring(L, -1);
                if (usage) {
                    if (strcmp(usage, "read") == 0)
                        bufs[i].usage = HL_GPU_USAGE_READ;
                    else if (strcmp(usage, "write") == 0)
                        bufs[i].usage = HL_GPU_USAGE_WRITE;
                    else if (strcmp(usage, "readwrite") == 0)
                        bufs[i].usage = HL_GPU_USAGE_READWRITE;
                }
                lua_pop(L, 1);

                bufs[i].binding = -1;
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    opts.buffers = bufs;
    opts.buffer_count = buf_count;
    opts.output_texture = -1;

    /* Parse textures */
    HlGpuTextureDesc tex_descs[8];
    memset(tex_descs, 0, sizeof(tex_descs));
    lua_getfield(L, 2, "textures");
    int tex_count = lua_parse_texture_descs(L, lua_gettop(L), tex_descs, 8);
    lua_pop(L, 1);
    opts.textures = tex_descs;
    opts.texture_count = tex_count;

    /* Parse output_texture (1-indexed in Lua, -1 = none) */
    lua_getfield(L, 2, "output_texture");
    if (!lua_isnil(L, -1))
        opts.output_texture = (int)lua_tointeger(L, -1) - 1;
    lua_pop(L, 1);

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    /* Fire-and-forget: pass NULL output pointers */
    int has_output = (opts.output_buffer >= 0 || opts.output_texture >= 0);
    void **out_ptr = has_output ? &output : NULL;
    size_t *out_len_ptr = has_output ? &output_len : NULL;

    int rc = hl_cap_gpu_dispatch(ctx, name, &opts,
                                 out_ptr, out_len_ptr, &err_msg);
    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "dispatch_failed");
        return 2;
    }

    if (output) {
#ifdef HL_ENABLE_WASM
        if (want_buffer) {
            /* Zero-copy: transfer malloc'd output to WasmBuffer */
            HlLua *lua_ctx = get_hl_lua(L);
            HlWasmBuffer *wbuf = hl_wasm_buffer_create_adopted(
                output, output_len, lua_ctx ? lua_ctx->base.alloc : NULL);
            if (wbuf) {
                lua_push_wasm_buffer(L, wbuf);
            } else {
                /* Fallback: copy to string if buffer creation fails */
                lua_pushlstring(L, (const char *)output, output_len);
                free(output);
            }
        } else
#endif
        {
            (void)want_buffer;
            lua_pushlstring(L, (const char *)output, output_len);
            free(output);
        }
    } else {
        lua_pushboolean(L, 1); /* fire-and-forget: return true */
    }
    return 1;
}

static int l_gpu_buffer(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    const char *name = luaL_checkstring(L, 1);

    int device = -1;
    size_t offset = 0;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "device");
        if (!lua_isnil(L, -1)) device = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "offset");
        if (!lua_isnil(L, -1)) offset = (size_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    /* nil = destroy */
    if (lua_isnil(L, 2)) {
        hl_cap_gpu_buffer_destroy(ctx, device, name);
        lua_pushboolean(L, 1);
        return 1;
    }

    /* Accept any buffer type (string, MappedBuffer, WasmBuffer) */
    HlBufferView bv;
    if (!lua_get_buffer(L, 2, &bv))
        return luaL_error(L, "gpu.buffer: data must be a string, MappedBuffer, or WasmBuffer");
    const void *data = bv.data;
    size_t data_len = bv.len;

    /* Try to write to existing buffer first */
    int rc = hl_cap_gpu_buffer_write(ctx, device, name, data, data_len, offset);
    if (rc == HL_GPU_ERR_NOT_FOUND) {
        /* Create new buffer */
        rc = hl_cap_gpu_buffer_create(ctx, device, name,
                                      data_len, HL_GPU_USAGE_READWRITE);
        if (rc == HL_GPU_OK)
            rc = hl_cap_gpu_buffer_write(ctx, device, name,
                                         data, data_len, 0);
    }

    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "buffer_error");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_gpu_buffer_read(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    const char *name = luaL_checkstring(L, 1);

    int device = -1;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "device");
        if (!lua_isnil(L, -1)) device = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    void *data = NULL;
    size_t len = 0;
    int rc = hl_cap_gpu_buffer_read(ctx, device, name, &data, &len);
    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "buffer_read_error");
        return 2;
    }

    lua_pushlstring(L, (const char *)data, len);
    free(data);
    return 1;
}

static int l_gpu_buffer_copy(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    const char *src = luaL_checkstring(L, 1);
    const char *dst = luaL_checkstring(L, 2);

    size_t src_offset = 0, dst_offset = 0, size = 0;
    int device = -1;

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "src_offset");
        src_offset = (size_t)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, 3, "dst_offset");
        dst_offset = (size_t)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, 3, "size");
        size = (size_t)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, 3, "device");
        if (!lua_isnil(L, -1)) device = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    int rc = hl_cap_gpu_buffer_copy(ctx, device, src, dst,
                                     src_offset, dst_offset, size);
    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, rc == HL_GPU_ERR_NOT_FOUND ? "buffer_not_found"
                       : rc == HL_GPU_ERR_BUFFER ? "out_of_bounds"
                       : "copy_failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ── Texture format helper ─────────────────────────────────────────── */

static HlGpuTexFormat lua_parse_tex_format(const char *s)
{
    if (!s) return HL_GPU_TEX_RGBA8;
    if (strcmp(s, "r8") == 0)      return HL_GPU_TEX_R8;
    if (strcmp(s, "rgba16f") == 0)  return HL_GPU_TEX_RGBA16F;
    if (strcmp(s, "r32f") == 0)     return HL_GPU_TEX_R32F;
    return HL_GPU_TEX_RGBA8;
}

static HlGpuTexFormat image_format_to_gpu(HlImageFormat fmt)
{
    return (HlGpuTexFormat)fmt;  /* safe: enum values match */
}

/* ── gpu.texture(name, data_or_nil, opts?) ────────────────────────── */

static int l_gpu_texture(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    const char *name = luaL_checkstring(L, 1);

    int device = -1;
    int storage = 0;
    int filter = HL_GPU_FILTER_NEAREST;
    int address_u = HL_GPU_ADDRESS_CLAMP;
    int address_v = HL_GPU_ADDRESS_CLAMP;

    /* nil = destroy */
    if (lua_isnil(L, 2)) {
        hl_cap_gpu_texture_destroy(ctx, device, name);
        lua_pushboolean(L, 1);
        return 1;
    }

    uint32_t width = 0, height = 0;
    HlGpuTexFormat format = HL_GPU_TEX_RGBA8;
    const void *pixels = NULL;
    size_t pixel_len = 0;

    /* Check if arg 2 is an HlImage userdata */
    HlImage **imgp = (HlImage **)luaL_testudata(L, 2, HL_IMAGE_MT);
    if (imgp && *imgp) {
        HlImage *img = *imgp;
        width = img->width;
        height = img->height;
        format = image_format_to_gpu(img->format);
        pixels = img->pixels;
        pixel_len = img->pixel_len;
    } else {
        /* Buffer data: require opts with width, height, format */
        HlBufferView bv;
        if (!lua_get_buffer(L, 2, &bv))
            return luaL_error(L, "gpu.texture: data must be HlImage, string, or buffer");
        pixels = bv.data;
        pixel_len = bv.len;
    }

    /* Parse opts */
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "device");
        if (!lua_isnil(L, -1)) device = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "width");
        if (!lua_isnil(L, -1)) width = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "height");
        if (!lua_isnil(L, -1)) height = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "format");
        if (lua_isstring(L, -1)) format = lua_parse_tex_format(lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 3, "storage");
        if (lua_toboolean(L, -1)) storage = 1;
        lua_pop(L, 1);
        lua_getfield(L, 3, "filter");
        if (lua_isstring(L, -1)) {
            const char *f = lua_tostring(L, -1);
            if (strcmp(f, "linear") == 0) filter = HL_GPU_FILTER_LINEAR;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "wrap");
        if (lua_isstring(L, -1)) {
            const char *w = lua_tostring(L, -1);
            if (strcmp(w, "repeat") == 0) {
                address_u = HL_GPU_ADDRESS_REPEAT;
                address_v = HL_GPU_ADDRESS_REPEAT;
            } else if (strcmp(w, "mirror") == 0) {
                address_u = HL_GPU_ADDRESS_MIRROR;
                address_v = HL_GPU_ADDRESS_MIRROR;
            }
        }
        lua_pop(L, 1);
    }

    if (width == 0 || height == 0)
        return luaL_error(L, "gpu.texture: width and height required");

    /* Create texture (idempotent) */
    int rc = hl_cap_gpu_texture_create(ctx, device, name, width, height,
                                        format, storage, filter,
                                        address_u, address_v);
    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "texture_create_failed");
        return 2;
    }

    /* Write pixel data */
    if (pixels && pixel_len > 0) {
        rc = hl_cap_gpu_texture_write(ctx, device, name, pixels, pixel_len);
        if (rc != HL_GPU_OK) {
            lua_pushnil(L);
            lua_pushstring(L, "texture_write_failed");
            return 2;
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* ── gpu.texture_read(name, opts?) → HlImage ─────────────────────── */

static int l_gpu_texture_read(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    const char *name = luaL_checkstring(L, 1);
    int device = -1;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "device");
        if (!lua_isnil(L, -1)) device = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    void *data = NULL;
    size_t len = 0;
    uint32_t w = 0, h = 0;
    HlGpuTexFormat fmt = HL_GPU_TEX_RGBA8;

    int rc = hl_cap_gpu_texture_read(ctx, device, name, &data, &len, &w, &h, &fmt);
    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "texture_read_failed");
        return 2;
    }

    /* Wrap as HlImage (takes ownership of data) */
    HlImage *img = hl_image_new(w, h, (HlImageFormat)fmt, data, len, NULL);
    free(data);
    if (!img) {
        lua_pushnil(L);
        lua_pushstring(L, "image_create_failed");
        return 2;
    }

    HlImage **udata = (HlImage **)lua_newuserdata(L, sizeof(HlImage *));
    *udata = img;
    luaL_getmetatable(L, HL_IMAGE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* ── Texture desc parsing helper (used by dispatch + pipeline) ────── */

static int lua_parse_texture_descs(lua_State *L, int tbl_idx,
                                    HlGpuTextureDesc *descs, int max_descs)
{
    if (!lua_istable(L, tbl_idx))
        return 0;
    int count = (int)lua_rawlen(L, tbl_idx);
    if (count > max_descs) count = max_descs;
    for (int i = 0; i < count; i++) {
        lua_rawgeti(L, tbl_idx, i + 1);
        if (lua_istable(L, -1)) {
            memset(&descs[i], 0, sizeof(descs[i]));
            descs[i].binding = -1;

            lua_getfield(L, -1, "name");
            descs[i].name = lua_tostring(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "storage");
            descs[i].storage = lua_toboolean(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "width");
            if (!lua_isnil(L, -1)) descs[i].width = (uint32_t)lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "height");
            if (!lua_isnil(L, -1)) descs[i].height = (uint32_t)lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "format");
            if (lua_isstring(L, -1))
                descs[i].format = lua_parse_tex_format(lua_tostring(L, -1));
            lua_pop(L, 1);

            /* data: pixel bytes or HlImage */
            lua_getfield(L, -1, "image");
            HlImage **imgp2 = (HlImage **)luaL_testudata(L, -1, HL_IMAGE_MT);
            if (imgp2 && *imgp2) {
                HlImage *img = *imgp2;
                descs[i].data = img->pixels;
                descs[i].data_len = img->pixel_len;
                if (descs[i].width == 0) descs[i].width = img->width;
                if (descs[i].height == 0) descs[i].height = img->height;
                descs[i].format = image_format_to_gpu(img->format);
            }
            lua_pop(L, 1);

            if (!descs[i].data) {
                lua_getfield(L, -1, "data");
                HlBufferView bv;
                if (lua_get_buffer(L, -1, &bv)) {
                    descs[i].data = bv.data;
                    descs[i].data_len = bv.len;
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    return count;
}

/* push_result callback: convert HlWorkerGpuOp result to Lua table */
static void lua_push_worker_gpu_result(lua_State *L, void *driver)
{
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)driver;
    lua_newtable(L);
    if (op->error) {
        lua_pushstring(L, op->error_msg);
        lua_setfield(L, -2, "error");
    } else {
        if (op->output && op->output_len > 0)
            lua_pushlstring(L, (const char *)op->output, op->output_len);
        else
            lua_pushlstring(L, "", 0);
        lua_setfield(L, -2, "result");
    }
}

/* Async dispatch — submits GPU work to thread pool */
static int l_gpu_async_dispatch(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.thread_pool)
        return luaL_error(L, "gpu.async not available (no thread pool)");
    if (!lua->server || !lua->active_conn)
        return luaL_error(L, "gpu.async can only be called from a request handler");
    if (!lua->base.gpu_ctx)
        return luaL_error(L, "gpu.async.dispatch: GPU not initialized");

    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    /* Parse opts (same as sync dispatch) */
    HlGpuDispatchOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.device = -1;

    lua_getfield(L, 2, "device");
    if (!lua_isnil(L, -1)) opts.device = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "workgroups");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "x");
        opts.workgroups.x = (uint32_t)luaL_optinteger(L, -1, 1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "y");
        opts.workgroups.y = (uint32_t)luaL_optinteger(L, -1, 1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "z");
        opts.workgroups.z = (uint32_t)luaL_optinteger(L, -1, 1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "output");
    opts.output_buffer = (int)luaL_optinteger(L, -1, 0) - 1;
    lua_pop(L, 1);

    lua_getfield(L, 2, "timeout");
    if (!lua_isnil(L, -1))
        opts.timeout_ms = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    /* Parse and deep-copy uniforms */
    void *uni_copy = NULL;
    size_t uni_len = 0;
    lua_getfield(L, 2, "uniforms");
    if (lua_isstring(L, -1)) {
        const char *uni = lua_tolstring(L, -1, &uni_len);
        if (uni && uni_len > 0) {
            uni_copy = malloc(uni_len);
            if (uni_copy)
                memcpy(uni_copy, uni, uni_len);
            else
                uni_len = 0; /* prevent NULL-with-nonzero-len */
        }
    }
    lua_pop(L, 1);

    /* Parse and deep-copy buffers */
    int buf_count = 0;
    HlGpuBufferDesc *buf_descs = NULL;
    void **buf_data_ptrs = NULL;

    lua_getfield(L, 2, "buffers");
    if (lua_istable(L, -1)) {
        buf_count = (int)lua_rawlen(L, -1);
        if (buf_count > 16) buf_count = 16;
        if (buf_count > 0) {
            buf_descs = calloc((size_t)buf_count, sizeof(HlGpuBufferDesc));
            buf_data_ptrs = calloc((size_t)buf_count, sizeof(void *));
            if (!buf_descs || !buf_data_ptrs) {
                free(buf_descs); free(buf_data_ptrs); free(uni_copy);
                lua_pop(L, 1);
                return luaL_error(L, "gpu.async.dispatch: out of memory");
            }
            for (int i = 0; i < buf_count; i++) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_istable(L, -1)) {
                    /* Deep-copy buffer name */
                    lua_getfield(L, -1, "name");
                    if (lua_isstring(L, -1)) {
                        size_t nlen;
                        const char *nstr = lua_tolstring(L, -1, &nlen);
                        if (nstr && nlen > 0 && nlen < HL_GPU_NAME_MAX) {
                            char *ncopy = malloc(nlen + 1);
                            if (ncopy) {
                                memcpy(ncopy, nstr, nlen);
                                ncopy[nlen] = '\0';
                                buf_descs[i].name = ncopy;
                            }
                        }
                    }
                    lua_pop(L, 1);

                    lua_getfield(L, -1, "data");
                    {
                        HlBufferView bv = {0};
                        lua_get_buffer(L, -1, &bv);
                        const void *d = bv.data;
                        size_t dlen = bv.len;
                        if (d && dlen > 0) {
                            buf_data_ptrs[i] = malloc(dlen);
                            if (buf_data_ptrs[i]) {
                                memcpy(buf_data_ptrs[i], d, dlen);
                                buf_descs[i].data = buf_data_ptrs[i];
                                buf_descs[i].size = dlen;
                            }
                        }
                    }
                    lua_pop(L, 1);

                    lua_getfield(L, -1, "size");
                    if (!lua_isnil(L, -1))
                        buf_descs[i].size = (size_t)lua_tointeger(L, -1);
                    lua_pop(L, 1);

                    lua_getfield(L, -1, "usage");
                    const char *us = lua_tostring(L, -1);
                    if (us) {
                        if (strcmp(us, "read") == 0) buf_descs[i].usage = HL_GPU_USAGE_READ;
                        else if (strcmp(us, "write") == 0) buf_descs[i].usage = HL_GPU_USAGE_WRITE;
                        else if (strcmp(us, "readwrite") == 0) buf_descs[i].usage = HL_GPU_USAGE_READWRITE;
                    }
                    lua_pop(L, 1);
                    buf_descs[i].binding = -1;
                }
                lua_pop(L, 1);
            }
        }
    }
    lua_pop(L, 1);

    /* Allocate worker op */
    HlWorkerGpuOp *op = calloc(1, sizeof(HlWorkerGpuOp));
    if (!op) {
        for (int i = 0; i < buf_count; i++) free(buf_data_ptrs[i]);
        free(buf_data_ptrs); free(buf_descs); free(uni_copy);
        return luaL_error(L, "gpu.async.dispatch: out of memory");
    }

    op->server = lua->server;
    op->gpu_ctx = lua->base.gpu_ctx;
    snprintf(op->shader_name, sizeof(op->shader_name), "%s", name);
    op->opts = opts;
    op->buffers = buf_descs;
    op->buffer_data = buf_data_ptrs;
    op->buffer_count = buf_count;
    op->uniforms = uni_copy;
    op->uniforms_len = uni_len;

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(lua->server, lua->base.alloc);
    if (!actx) {
        hl_worker_gpu_op_free(op); free(op);
        return luaL_error(L, "gpu.async.dispatch: out of memory");
    }

    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                   HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                   lua_push_worker_gpu_result);
    if (!cont) {
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return luaL_error(L, "gpu.async.dispatch: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_gpu_op_free_all;
    actx->op.on_cancel = hl_worker_gpu_async_cancel;

    op->async_ctx = actx;
    atomic_store(&op->cancelled, 0);

    if (hl_worker_gpu_submit(lua->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return luaL_error(L, "gpu.async.dispatch: thread pool full");
    }

    if (kl_async_suspend(lua->server, lua->active_conn, &actx->op) < 0) {
        atomic_store(&op->cancelled, 1);
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        return luaL_error(L, "gpu.async.dispatch: failed to suspend connection");
    }

    return lua_yieldk(L, 0, 0, NULL);
}

/* ── gpu.pipeline() ─────────────────────────────────────────────── */

/*
 * Parse a pipeline stages array from Lua table.
 * Returns stage_count on success, -1 on error.
 * Stages and buffer descs are written into caller-provided arrays.
 */
static int parse_pipeline_stages(lua_State *L, int tbl_idx,
                                  HlGpuPipelineStage *stages, int max_stages,
                                  HlGpuBufferDesc *all_bufs, int max_bufs,
                                  int *total_bufs)
{
    int stage_count = (int)lua_rawlen(L, tbl_idx);
    if (stage_count > max_stages) stage_count = max_stages;
    int buf_offset = 0;

    for (int s = 0; s < stage_count; s++) {
        lua_rawgeti(L, tbl_idx, s + 1);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

        memset(&stages[s], 0, sizeof(stages[s]));

        /* shader (required) */
        lua_getfield(L, -1, "shader");
        stages[s].shader = lua_tostring(L, -1);
        lua_pop(L, 1);

        /* workgroups */
        lua_getfield(L, -1, "workgroups");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "x");
            stages[s].workgroups.x = (uint32_t)luaL_optinteger(L, -1, 1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "y");
            stages[s].workgroups.y = (uint32_t)luaL_optinteger(L, -1, 1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "z");
            stages[s].workgroups.z = (uint32_t)luaL_optinteger(L, -1, 1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        /* uniforms */
        lua_getfield(L, -1, "uniforms");
        if (lua_isstring(L, -1))
            stages[s].uniforms = lua_tolstring(L, -1, &stages[s].uniforms_len);
        lua_pop(L, 1);

        /* buffers */
        lua_getfield(L, -1, "buffers");
        if (lua_istable(L, -1)) {
            int bc = (int)lua_rawlen(L, -1);
            if (bc > 16) bc = 16;
            if (buf_offset + bc > max_bufs) bc = max_bufs - buf_offset;
            stages[s].buffers = &all_bufs[buf_offset];
            stages[s].buffer_count = bc;
            for (int b = 0; b < bc; b++) {
                memset(&all_bufs[buf_offset + b], 0, sizeof(HlGpuBufferDesc));
                lua_rawgeti(L, -1, b + 1);
                if (lua_istable(L, -1)) {
                    lua_getfield(L, -1, "name");
                    all_bufs[buf_offset + b].name = lua_tostring(L, -1);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "data");
                    {
                        HlBufferView bv;
                        if (lua_get_buffer(L, -1, &bv)) {
                            all_bufs[buf_offset + b].data = bv.data;
                            all_bufs[buf_offset + b].size = bv.len;
                        }
                    }
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "size");
                    if (!lua_isnil(L, -1))
                        all_bufs[buf_offset + b].size = (size_t)lua_tointeger(L, -1);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "usage");
                    const char *us = lua_tostring(L, -1);
                    if (us) {
                        if (strcmp(us, "read") == 0)
                            all_bufs[buf_offset + b].usage = HL_GPU_USAGE_READ;
                        else if (strcmp(us, "write") == 0)
                            all_bufs[buf_offset + b].usage = HL_GPU_USAGE_WRITE;
                        else if (strcmp(us, "readwrite") == 0)
                            all_bufs[buf_offset + b].usage = HL_GPU_USAGE_READWRITE;
                    }
                    lua_pop(L, 1);
                    all_bufs[buf_offset + b].binding = -1;
                }
                lua_pop(L, 1);
            }
            buf_offset += bc;
        }
        lua_pop(L, 1); /* pop buffers */
        lua_pop(L, 1); /* pop stage table */
    }

    *total_bufs = buf_offset;
    return stage_count;
}

static int l_gpu_pipeline(lua_State *L)
{
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    luaL_checktype(L, 1, LUA_TTABLE); /* stages array */

    /* Parse stages */
    HlGpuPipelineStage stages[HL_GPU_MAX_PIPELINE_STAGES];
    HlGpuBufferDesc all_bufs[HL_GPU_MAX_PIPELINE_BUFFERS];
    int total_bufs = 0;
    int stage_count = parse_pipeline_stages(L, 1, stages,
        HL_GPU_MAX_PIPELINE_STAGES, all_bufs, HL_GPU_MAX_PIPELINE_BUFFERS,
        &total_bufs);

    if (stage_count <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, "empty_pipeline");
        return 2;
    }

    /* Parse opts (second argument, optional) */
    HlGpuPipelineOutput outputs[HL_GPU_MAX_PIPELINE_OUTPUTS];
    int output_count = 0;
    int device = -1;

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "device");
        if (!lua_isnil(L, -1)) device = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        /* output = false -> fire-and-forget pipeline (no readback) */
        lua_getfield(L, 2, "output");
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1))
            output_count = HL_GPU_OUTPUT_NONE;
        lua_pop(L, 1);

        /* outputs = { {stage=1, buffer=1}, {stage=2, buffer=1} } (1-indexed) */
        lua_getfield(L, 2, "outputs");
        if (output_count != HL_GPU_OUTPUT_NONE && lua_istable(L, -1)) {
            output_count = (int)lua_rawlen(L, -1);
            if (output_count > HL_GPU_MAX_PIPELINE_OUTPUTS)
                output_count = HL_GPU_MAX_PIPELINE_OUTPUTS;
            for (int i = 0; i < output_count; i++) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_istable(L, -1)) {
                    lua_getfield(L, -1, "stage");
                    outputs[i].stage = (int)luaL_optinteger(L, -1, stage_count) - 1;
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "buffer");
                    outputs[i].buffer = (int)luaL_optinteger(L, -1, 1) - 1;
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    /* buffer = true -> return WasmBuffer for GPU->WASM chaining */
    int pipe_want_buffer = 0;
    uint32_t pipe_timeout_ms = 0;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "buffer");
        if (lua_toboolean(L, -1)) pipe_want_buffer = 1;
        lua_pop(L, 1);
        lua_getfield(L, 2, "timeout");
        if (!lua_isnil(L, -1))
            pipe_timeout_ms = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    HlGpuPipelineOpts popts = {
        .stages = stages,
        .stage_count = stage_count,
        .outputs = output_count > 0 ? outputs : NULL,
        .output_count = output_count,
        .device = device,
        .timeout_ms = pipe_timeout_ms,
    };

    HlGpuPipelineResult result;
    const char *err_msg = NULL;
    int rc = hl_cap_gpu_pipeline(ctx, &popts, &result, &err_msg);

    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "pipeline_failed");
        return 2;
    }

    /* Fire-and-forget: return true */
    if (result.count == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }

    /* Return table of results */
    if (result.count == 1) {
#ifdef HL_ENABLE_WASM
        if (pipe_want_buffer) {
            HlLua *lua_ctx = get_hl_lua(L);
            HlWasmBuffer *wbuf = hl_wasm_buffer_create_adopted(
                result.data[0], result.len[0],
                lua_ctx ? lua_ctx->base.alloc : NULL);
            if (wbuf) {
                result.data[0] = NULL; /* ownership transferred */
                hl_cap_gpu_pipeline_result_free(&result);
                lua_push_wasm_buffer(L, wbuf);
                return 1;
            }
        }
#endif
        (void)pipe_want_buffer;
        lua_pushlstring(L, (const char *)result.data[0], result.len[0]);
        hl_cap_gpu_pipeline_result_free(&result);
        return 1;
    }

    lua_createtable(L, result.count, 0);
    for (int i = 0; i < result.count; i++) {
        lua_pushlstring(L, (const char *)result.data[i], result.len[i]);
        lua_rawseti(L, -2, i + 1);
    }
    hl_cap_gpu_pipeline_result_free(&result);
    return 1;
}

/* Async pipeline — submits to thread pool */
static void lua_push_worker_gpu_pipeline_result(lua_State *L, void *driver)
{
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)driver;
    lua_newtable(L);
    if (op->error) {
        lua_pushstring(L, op->error_msg);
        lua_setfield(L, -2, "error");
    } else if (op->output && op->output_len > 0) {
        lua_pushlstring(L, (const char *)op->output, op->output_len);
        lua_setfield(L, -2, "result");
    } else {
        lua_pushlstring(L, "", 0);
        lua_setfield(L, -2, "result");
    }
}

static int l_gpu_async_pipeline(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.thread_pool)
        return luaL_error(L, "gpu.async not available (no thread pool)");
    if (!lua->server || !lua->active_conn)
        return luaL_error(L, "gpu.async can only be called from a request handler");
    if (!lua->base.gpu_ctx)
        return luaL_error(L, "gpu.async.pipeline: GPU not initialized");

    luaL_checktype(L, 1, LUA_TTABLE);

    /* Parse stages (same as sync) */
    HlGpuPipelineStage stages[HL_GPU_MAX_PIPELINE_STAGES];
    HlGpuBufferDesc all_bufs[HL_GPU_MAX_PIPELINE_BUFFERS];
    int total_bufs = 0;
    int stage_count = parse_pipeline_stages(L, 1, stages,
        HL_GPU_MAX_PIPELINE_STAGES, all_bufs, HL_GPU_MAX_PIPELINE_BUFFERS,
        &total_bufs);
    if (stage_count <= 0)
        return luaL_error(L, "gpu.async.pipeline: empty pipeline");

    /* Parse opts */
    HlGpuPipelineOutput outputs[HL_GPU_MAX_PIPELINE_OUTPUTS];
    memset(outputs, 0, sizeof(outputs));
    int output_count = 0;
    int device = -1;
    uint32_t pipe_timeout_ms = 0;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "device");
        if (!lua_isnil(L, -1)) device = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "output");
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1))
            output_count = HL_GPU_OUTPUT_NONE;
        lua_pop(L, 1);
        lua_getfield(L, 2, "outputs");
        if (output_count != HL_GPU_OUTPUT_NONE && lua_istable(L, -1)) {
            output_count = (int)lua_rawlen(L, -1);
            if (output_count > HL_GPU_MAX_PIPELINE_OUTPUTS)
                output_count = HL_GPU_MAX_PIPELINE_OUTPUTS;
            for (int i = 0; i < output_count; i++) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_istable(L, -1)) {
                    lua_getfield(L, -1, "stage");
                    outputs[i].stage = (int)luaL_optinteger(L, -1, stage_count) - 1;
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "buffer");
                    outputs[i].buffer = (int)luaL_optinteger(L, -1, 1) - 1;
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "timeout");
        if (!lua_isnil(L, -1))
            pipe_timeout_ms = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    /* Deep-copy into worker op */
    HlWorkerGpuOp *op = calloc(1, sizeof(HlWorkerGpuOp));
    if (!op) return luaL_error(L, "gpu.async.pipeline: out of memory");

    op->server = lua->server;
    op->gpu_ctx = lua->base.gpu_ctx;
    op->is_pipeline = 1;
    op->stage_count = stage_count;
    op->pipe_opts.device = device;
    op->pipe_opts.timeout_ms = pipe_timeout_ms;
    op->pipe_opts.output_count = output_count;

    /* Deep-copy output specs */
    if (output_count > 0) {
        op->pipe_outputs = calloc((size_t)output_count, sizeof(HlGpuPipelineOutput));
        if (op->pipe_outputs)
            memcpy(op->pipe_outputs, outputs, (size_t)output_count * sizeof(HlGpuPipelineOutput));
        op->pipe_opts.outputs = op->pipe_outputs;
    }

    /* Deep-copy stages */
    op->stages = calloc((size_t)stage_count, sizeof(HlGpuPipelineStage));
    op->stage_uniforms = calloc((size_t)stage_count, sizeof(void *));
    if (!op->stages || !op->stage_uniforms) {
        hl_worker_gpu_op_free(op); free(op);
        return luaL_error(L, "gpu.async.pipeline: out of memory");
    }

    /* Deep-copy buffer descs and data */
    if (total_bufs > 0) {
        op->buffers = calloc((size_t)total_bufs, sizeof(HlGpuBufferDesc));
        op->buffer_data = calloc((size_t)total_bufs, sizeof(void *));
        if (!op->buffers || !op->buffer_data) {
            hl_worker_gpu_op_free(op); free(op);
            return luaL_error(L, "gpu.async.pipeline: out of memory");
        }
        op->buffer_count = total_bufs;
    }

    int buf_off = 0;
    for (int s = 0; s < stage_count; s++) {
        /* Deep-copy shader name (M-3: OOM must abort, not silently NULL). */
        if (stages[s].shader) {
            op->stages[s].shader = strdup(stages[s].shader);
            if (!op->stages[s].shader) {
                hl_worker_gpu_op_free(op); free(op);
                return luaL_error(L, "gpu.async.pipeline: out of memory");
            }
        }
        op->stages[s].workgroups = stages[s].workgroups;
        op->stages[s].buffer_count = stages[s].buffer_count;

        /* Deep-copy per-stage uniforms */
        if (stages[s].uniforms && stages[s].uniforms_len > 0) {
            op->stage_uniforms[s] = malloc(stages[s].uniforms_len);
            if (!op->stage_uniforms[s]) {
                hl_worker_gpu_op_free(op); free(op);
                return luaL_error(L, "gpu.async.pipeline: out of memory");
            }
            memcpy(op->stage_uniforms[s], stages[s].uniforms, stages[s].uniforms_len);
            op->stages[s].uniforms_len = stages[s].uniforms_len;
        }

        /* Deep-copy buffer descs + data for this stage */
        for (int b = 0; b < stages[s].buffer_count && buf_off < total_bufs; b++) {
            const HlGpuBufferDesc *src_desc = &all_bufs[buf_off];
            HlGpuBufferDesc *dst_desc = &op->buffers[buf_off];
            dst_desc->size = src_desc->size;
            dst_desc->usage = src_desc->usage;
            dst_desc->binding = src_desc->binding;
            if (src_desc->name) {
                dst_desc->name = strdup(src_desc->name);
                if (!dst_desc->name) {
                    hl_worker_gpu_op_free(op); free(op);
                    return luaL_error(L, "gpu.async.pipeline: out of memory");
                }
            }
            if (src_desc->data && src_desc->size > 0) {
                op->buffer_data[buf_off] = malloc(src_desc->size);
                if (!op->buffer_data[buf_off]) {
                    hl_worker_gpu_op_free(op); free(op);
                    return luaL_error(L, "gpu.async.pipeline: out of memory");
                }
                memcpy(op->buffer_data[buf_off], src_desc->data, src_desc->size);
                dst_desc->data = op->buffer_data[buf_off];
            }
            buf_off++;
        }
    }

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(lua->server, lua->base.alloc);
    if (!actx) {
        hl_worker_gpu_op_free(op); free(op);
        return luaL_error(L, "gpu.async.pipeline: out of memory");
    }

    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                   HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                   lua_push_worker_gpu_pipeline_result);
    if (!cont) {
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return luaL_error(L, "gpu.async.pipeline: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_gpu_op_free_all;
    actx->op.on_cancel = hl_worker_gpu_async_cancel;

    op->async_ctx = actx;
    atomic_store(&op->cancelled, 0);

    if (hl_worker_gpu_submit(lua->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return luaL_error(L, "gpu.async.pipeline: thread pool full");
    }

    if (kl_async_suspend(lua->server, lua->active_conn, &actx->op) < 0) {
        atomic_store(&op->cancelled, 1);
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        return luaL_error(L, "gpu.async.pipeline: failed to suspend");
    }

    return lua_yieldk(L, 0, 0, NULL);
}

static const luaL_Reg gpu_funcs[] = {
    {"available",     l_gpu_available},
    {"devices",       l_gpu_devices},
    {"compile",       l_gpu_compile},
    {"load",          l_gpu_load},
    {"dispatch",      l_gpu_dispatch},
    {"pipeline",      l_gpu_pipeline},
    {"buffer",        l_gpu_buffer},
    {"buffer_read",   l_gpu_buffer_read},
    {"buffer_copy",   l_gpu_buffer_copy},
    {"texture",       l_gpu_texture},
    {"texture_read",  l_gpu_texture_read},
    {NULL, NULL}
};

static const luaL_Reg gpu_async_funcs[] = {
    {"dispatch",  l_gpu_async_dispatch},
    {"pipeline",  l_gpu_async_pipeline},
    {NULL, NULL}
};

int luaopen_hull_gpu(lua_State *L)
{
    luaL_newlib(L, gpu_funcs);
    luaL_newlib(L, gpu_async_funcs);
    lua_setfield(L, -2, "async");  /* gpu.async = {...} */
    return 1;
}

#endif /* HL_ENABLE_GPU */
