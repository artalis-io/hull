/* mod_gpu.c - hull.gpu module: GPU compute via wgpu-native
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */


#include "hull/cap/fs.h"          /* HlMappedBuffer / HL_MMAP_MT (pinned views); without
                                   * WASM nothing else brought it in */
#include "hull/cap/fs_resolve.h"  /* hl_fs_fopen_read */
#include "mod_buffer.h"
#include "hull/cap/gpu.h"
#include "hull/cap/image.h"
#include "hull/worker_gpu.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#include "hull/vfs.h"

/* Forward declaration */
static int lua_parse_texture_descs(lua_State *L, int tbl_idx,
                                    HlGpuTextureDesc *out, int max, int anchor);

#include "hull/utils/alloc.h"
#include <pthread.h>
#include "hull/shared/work_charge.h"   /* hl_work_bulk_units */
#include "internal.h"   /* hl_lua_anchor, hl_lua_check_can_wait */
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm_buffer.h"
#endif
#include "protected.h"   /* pushes that cannot leak the C buffer */

#include <keel/http_server.h>

/* ── Pinned views ─────────────────────────────────────────────────────
 *
 * dispatch / pipeline (and their async forms) read long descriptor tables,
 * taking a raw view of each buffer as they go - and reading a table field can
 * run app code (__index), which can close() a buffer whose view was already
 * taken. The GPU then read unmapped or freed memory, and could hand it back.
 * So each view is pinned the moment it is taken, until the call returns: a
 * MappedBuffer / WasmBuffer takes a borrow (its close waits for the release),
 * an image's pixels are copied (images have no borrow). The pins live in a
 * to-be-closed userdata, released on return and on an error alike, found
 * through the call's anchor table. */
#define GPU_PINS_MAX 64
enum { GPU_PIN_MMAP = 1, GPU_PIN_WBUF, GPU_PIN_COPY };
typedef struct {
    int n;
    struct { int kind; void *p; size_t len; } e[GPU_PINS_MAX];
    HlOffheapHold hold;     /* the image copies (gpu_offheap_copy) */
} GpuPins;

static int gpu_pins_close(lua_State *L)
{
    GpuPins *ps = (GpuPins *)lua_touserdata(L, 1);
    if (!ps) return 0;
    for (int i = 0; i < ps->n; i++) {
        switch (ps->e[i].kind) {
        case GPU_PIN_MMAP: hl_cap_fs_mmap_release(ps->e[i].p); break;
#ifdef HL_ENABLE_WASM
        case GPU_PIN_WBUF: hl_wasm_buffer_release((HlWasmBuffer *)ps->e[i].p); break;
#endif
        case GPU_PIN_COPY: free(ps->e[i].p); break;
        default: break;
        }
    }
    ps->n = 0;
    hl_offheap_drop(&ps->hold);
    return 0;
}

/* One pin set per call, as a to-be-closed slot, recorded on @p anchor. */
static void gpu_pins_open(lua_State *L, int anchor)
{
    GpuPins *ps = (GpuPins *)lua_newuserdatauv(L, sizeof *ps, 0);
    ps->n = 0;
    ps->hold = (HlOffheapHold){0};
    if (luaL_newmetatable(L, "hull.gpu.pins")) {
        lua_pushcfunction(L, gpu_pins_close);
        lua_setfield(L, -2, "__close");
        lua_pushcfunction(L, gpu_pins_close);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, anchor, "pins");
    lua_toclose(L, -1);
}

static void gpu_pin_add(lua_State *L, GpuPins *ps, int kind, void *p, size_t len)
{
    if (ps->n >= GPU_PINS_MAX) {
        if (kind == GPU_PIN_MMAP) hl_cap_fs_mmap_release(p);
#ifdef HL_ENABLE_WASM
        else if (kind == GPU_PIN_WBUF) hl_wasm_buffer_release((HlWasmBuffer *)p);
#endif
        else if (kind == GPU_PIN_COPY) free(p);
        luaL_error(L, "gpu: too many buffers in one call (max %d)", GPU_PINS_MAX);
        return;
    }
    ps->e[ps->n].kind = kind;
    ps->e[ps->n].p = p;
    ps->e[ps->n].len = len;
    ps->n++;
}

/* A copy of @p len bytes made off the VM heap for a call (an image pin) or an
 * async op (its uniforms and buffer data, which a worker reads after the
 * binding returns). A MappedBuffer, WasmBuffer or borrowed image is not on
 * the heap, and nothing bounded these copies: one gpu.async.dispatch of 16
 * buffers over a 1 GiB mapping copied 16 GiB, and a copy whose malloc failed
 * ran as an empty buffer (audit 20). The copy holds its bytes on the VM's
 * off-heap total (`wasm_buf_bytes`, shared with OWNED WasmBuffers) under the
 * heap limit, through @p hold, and is charged 1 unit per 8 bytes. Returns the
 * copy, or NULL with @p *err set (nothing held for it). Only the charge can
 * raise (lua_hlwork, at once when it puts the run over), and it runs before
 * anything is held: call it only where every copy made so far has an owner a
 * raise closes (a to-be-closed slot). */
static void *gpu_offheap_copy(lua_State *L, HlLua *lua, HlOffheapHold *hold,
                              const void *src, size_t len, const char **err)
{
    lua_hlwork(L, (size_t)hl_work_bytes_units((uint64_t)len), 0);
    if (hl_offheap_take(hold, lua ? &lua->wasm_buf_bytes : NULL,
                        lua ? lua->mem_limit : 0, len) != 0) {
        *err = "its buffers would take the VM past its heap limit "
               "(close unused buffers)";
        return NULL;
    }
    void *p = malloc(len);
    if (!p) {
        hl_offheap_give(hold, len);
        *err = "out of memory";
        return NULL;
    }
    memcpy(p, src, len);
    return p;
}

/* Pin the buffer at @p idx, whose view @p bv was just taken. */
static void gpu_pin_view(lua_State *L, int anchor, int idx, HlBufferView *bv)
{
    idx = lua_absindex(L, idx);
    lua_getfield(L, anchor, "pins");
    GpuPins *ps = (GpuPins *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!ps || !bv->data || bv->len == 0) return;
    HlMappedBuffer **mp = (HlMappedBuffer **)luaL_testudata(L, idx, HL_MMAP_MT);
    if (mp && *mp) {
        hl_cap_fs_mmap_borrow(*mp);
        gpu_pin_add(L, ps, GPU_PIN_MMAP, *mp, 0);
        return;
    }
#ifdef HL_ENABLE_WASM
    HlWasmBuffer **wp = (HlWasmBuffer **)luaL_testudata(L, idx, HL_WASM_BUF_MT);
    if (wp && *wp) {
        hl_wasm_buffer_borrow(*wp);
        gpu_pin_add(L, ps, GPU_PIN_WBUF, *wp, 0);
        return;
    }
#endif
    if (luaL_testudata(L, idx, HL_IMAGE_MT)) {
        const char *err = NULL;
        void *copy = gpu_offheap_copy(L, get_hl_lua(L), &ps->hold, bv->data,
                                      bv->len, &err);
        if (!copy) { luaL_error(L, "gpu: %s", err); return; }
        gpu_pin_add(L, ps, GPU_PIN_COPY, copy, bv->len);
        bv->data = copy;
    }
    /* a string: anchored by the caller, immutable */
}

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.gpu module - GPU compute (wgpu-native)
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

/* opts.timeout (ms) as a dispatch's timeout_ms: 0 (the backend default) for
 * a non-positive or non-integer value, at most HL_GPU_TIMEOUT_MAX_MS. Cast
 * straight to uint32_t, -1 waited ~49.7 days on the event loop. */
static uint32_t gpu_timeout_arg(lua_State *L, int idx)
{
    lua_Integer t = lua_tointeger(L, idx);
    if (t <= 0) return 0;
    return t > HL_GPU_TIMEOUT_MAX_MS ? HL_GPU_TIMEOUT_MAX_MS : (uint32_t)t;
}

static HlGpuCtx *lua_get_gpu_ctx(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return lua ? lua->base.gpu_ctx : NULL;
}

/* The VM heap limit (0 = none): a read-back is refused past it before the
 * backend copies it off the heap (audit 17). */
static size_t gpu_heap_limit(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    return lua ? lua->mem_limit : 0;
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
    int vn = snprintf(vfs_name, sizeof(vfs_name), "shaders/%s.wgsl", name);
    /* A cut name would open a different file: no ".wgsl", a prefix of it. */
    if (vn < 0 || (size_t)vn >= sizeof(vfs_name))
        return luaL_error(L, "shader name too long: %s", name);
    const HlEntry *entry = NULL;
    if (lua && lua->base.app_vfs)
        entry = hl_vfs_find(lua->base.app_vfs, vfs_name);

    const char *wgsl = NULL;
    size_t wgsl_len = 0;
    char *file_buf = NULL;

    if (entry && entry->data) {
        wgsl = (const char *)entry->data;
        wgsl_len = entry->len;
    } else if (lua && lua->base.app_vfs && lua->base.app_vfs->root_dir &&
               !hl_vfs_embedded(lua->base.app_vfs)) {
        /* Try filesystem: <app_dir>/shaders/<name>.wgsl. Development only:
         * a built binary (app files embedded) runs the shaders it was built
         * with, not whatever lies under its working directory. */
        char path[4096];
        int pn = snprintf(path, sizeof(path), "%s/shaders/%s.wgsl",
                 lua->base.app_vfs->root_dir, name);
        if (pn < 0 || (size_t)pn >= sizeof(path)) {
            lua_pushnil(L);
            lua_pushstring(L, "shader path too long");
            return 2;
        }
        /* Opened under the app root: the name check above is lexical, and
         * a symlink below shaders/ used to be followed wherever it led. */
        FILE *f = hl_fs_fopen_read(lua->base.app_vfs->root_dir, vfs_name);
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
            /* --verify-sig: only a signed shader, as signed. */
            if (wgsl && hl_vfs_disk_gate_check(vfs_name, wgsl, wgsl_len) != 0) {
                free(file_buf);
                lua_pushnil(L);
                lua_pushfstring(L, "shader '%s' is not a signed file (--verify-sig)",
                                name);
                return 2;
            }
        }
    }

    if (!wgsl) {
        free(file_buf);
        lua_pushnil(L);
        lua_pushfstring(L, "shader '%s' not found in shaders/", name);
        return 2;
    }

    /* Charged before the compile, which runs on the event loop in this one
     * call (audit 24). Non-raising: file_buf is freed before the trip. */
    if (hl_lua_budget_charge(L, (int64_t)hl_work_bytes_units(
            (uint64_t)wgsl_len))) {
        free(file_buf);
        return hl_lua_budget_raise(L);
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

    /* The compile runs on the event loop in this one call: charged before
     * it, 1 unit per 8 WGSL bytes (audit 24). */
    lua_hlwork(L, (size_t)hl_work_bytes_units((uint64_t)wgsl_len), 0);

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
    lua_settop(L, 2);
    lua_newtable(L);                 /* anchor: see hl_lua_anchor */
    const int anchor = lua_gettop(L);
    gpu_pins_open(L, anchor);

    HlGpuDispatchOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.device = -1;
    opts.max_output = gpu_heap_limit(L);   /* audit 17 */

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
        opts.timeout_ms = gpu_timeout_arg(L, -1);
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
        hl_lua_anchor(L, anchor, -1);
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
                hl_lua_anchor(L, anchor, -1);
                lua_pop(L, 1);

                lua_getfield(L, -1, "data");
                {
                    HlBufferView bv;
                    if (lua_get_buffer(L, -1, &bv)) {
                        hl_lua_anchor(L, anchor, -1);
                        gpu_pin_view(L, anchor, -1, &bv);
                        bufs[i].data = bv.data;
                        bufs[i].data_len = bv.len;
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
    int tex_count = lua_parse_texture_descs(L, lua_gettop(L), tex_descs, 8,
                                            anchor);
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

    /* The call's own buffers, uniforms and inline textures are charged
     * before they are made, one unit per 64 bytes, as gpu.buffer's create
     * is (audit 17). lua_hlwork runs no app code; the pins close on a
     * raise. */
    lua_hlwork(L, (size_t)hl_work_bulk_units(
                      (uint64_t)hl_gpu_dispatch_charge_bytes(&opts)), 0);

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
                if (hl_lua_pushlstring_safe(L, (const char *)output, output_len) != 0) {
                    free(output);
                    return luaL_error(L, "not enough memory for the result");
                }
                free(output);
            }
        } else
#endif
        {
            (void)want_buffer;
            if (hl_lua_pushlstring_safe(L, (const char *)output, output_len) != 0) {
                free(output);
                return luaL_error(L, "not enough memory for the result");
            }
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

    /* The upload (and a create, sized by it) is charged before it runs, one
     * unit per 64 bytes (audit 17): a MappedBuffer source is off-heap, and
     * the device memory it fills is outside the heap limit. lua_hlwork
     * raises at once when the run is over; it runs no app code. */
    lua_hlwork(L, (size_t)hl_work_bulk_units((uint64_t)data_len), 0);

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
    /* Refused before the backend copies it, when the heap could never hold
     * it (audit 17). */
    int rc = hl_cap_gpu_buffer_read(ctx, device, name, gpu_heap_limit(L),
                                    &data, &len);
    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "buffer_read_error");
        return 2;
    }

    /* The read-back and its copy into the string: 1 unit per 8 bytes, as
     * texture_read (audit 26). */
    if (hl_lua_budget_charge(L, (int64_t)hl_work_bytes_units((uint64_t)len))) {
        free(data);
        return hl_lua_budget_raise(L);
    }

    if (hl_lua_pushlstring_safe(L, (const char *)data, len) != 0) {
        free(data);
        return luaL_error(L, "not enough memory for the result");
    }
    free(data);
    return 1;
}

/* The bytes a gpu.buffer_copy would move: @p size, or the rest of the
 * source from @p src_offset when it is 0, never more than the source holds
 * (a bogus size is refused by the cap, and must not trip the budget first).
 * 0 when the device or the buffer cannot be found (the cap refuses those).
 * Read under the device's lock, as the cap reads it; the cap has no size
 * accessor (audit 27). */
static size_t lua_gpu_copy_bytes(HlGpuCtx *ctx, int device, const char *src,
                                 size_t src_offset, size_t size)
{
    if (!ctx || !src) return 0;
    int idx = device < 0 ? ctx->default_device : device;
    if (idx < 0 || idx >= ctx->device_count || idx >= HL_GPU_MAX_DEVICES)
        return 0;
    if (ctx->device_restriction && !ctx->allowed_devices[idx])
        return 0;
    HlGpuDevice *dev = &ctx->devices[idx];
    size_t src_size = 0;
    int found = 0;
    pthread_mutex_lock(&dev->mutex);
    for (int i = 0; i < dev->buffer_count; i++) {
        if (strcmp(dev->buffers[i].name, src) == 0) {
            src_size = dev->buffers[i].size;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&dev->mutex);
    if (!found || src_offset >= src_size) return 0;
    size_t rest = src_size - src_offset;
    return (size == 0 || size > rest) ? rest : size;
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

    /* The device-side copy, one unit per 64 bytes before it runs, as a
     * gpu.buffer upload (audit 27: uncharged, and size 0 copies the whole
     * source). lua_hlwork runs no app code. */
    lua_hlwork(L, (size_t)hl_work_bulk_units(
        (uint64_t)lua_gpu_copy_bytes(ctx, device, src, src_offset, size)), 0);

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

#ifdef HL_ENABLE_IMAGE
static HlGpuTexFormat image_format_to_gpu(HlImageFormat fmt)
{
    return (HlGpuTexFormat)fmt;  /* safe: enum values match */
}
#endif

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

    /* The opts first: reading them can run app code (__index), which could
     * close the image / buffer in arg 2 - after its pixel pointer was taken,
     * the upload read freed or unmapped memory into a texture the app can read
     * back. The source is taken last, with nothing between it and the upload;
     * the opts' width / height / format still win over the source's. */
    int have_w = 0, have_h = 0, have_fmt = 0;
    uint32_t opt_w = 0, opt_h = 0;
    HlGpuTexFormat opt_fmt = HL_GPU_TEX_RGBA8;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "device");
        if (!lua_isnil(L, -1)) device = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "width");
        if (!lua_isnil(L, -1)) { opt_w = (uint32_t)lua_tointeger(L, -1); have_w = 1; }
        lua_pop(L, 1);
        lua_getfield(L, 3, "height");
        if (!lua_isnil(L, -1)) { opt_h = (uint32_t)lua_tointeger(L, -1); have_h = 1; }
        lua_pop(L, 1);
        lua_getfield(L, 3, "format");
        if (lua_isstring(L, -1)) { opt_fmt = lua_parse_tex_format(lua_tostring(L, -1)); have_fmt = 1; }
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

#ifdef HL_ENABLE_IMAGE
    /* Check if arg 2 is an HlImage userdata */
    HlImage **imgp = (HlImage **)luaL_testudata(L, 2, HL_IMAGE_MT);
    if (imgp && *imgp) {
        HlImage *img = *imgp;
        width = img->width;
        height = img->height;
        format = image_format_to_gpu(img->format);
        pixels = img->pixels;
        pixel_len = img->pixel_len;
    } else
#endif
    {
        /* Buffer data: require opts with width, height, format */
        HlBufferView bv;
        if (!lua_get_buffer(L, 2, &bv))
            return luaL_error(L, "gpu.texture: data must be "
#ifdef HL_ENABLE_IMAGE
                              "HlImage, "
#endif
                              "string, or buffer");
        pixels = bv.data;
        pixel_len = bv.len;
    }
    if (have_w) width = opt_w;
    if (have_h) height = opt_h;
    if (have_fmt) format = opt_fmt;

    if (width == 0 || height == 0)
        return luaL_error(L, "gpu.texture: width and height required");

    /* The texture's device memory and the upload are charged before they
     * are made, one unit per 64 bytes: a create with no data allocated up
     * to 16384^2 x 8 bytes for nothing (audit 17). Runs no app code. */
    lua_hlwork(L, (size_t)(hl_work_bulk_units(
                       (uint64_t)hl_gpu_tex_bytes(width, height, format)) +
                   hl_work_bulk_units((uint64_t)pixel_len)), 0);

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

#ifdef HL_ENABLE_IMAGE
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

    int rc = hl_cap_gpu_texture_read(ctx, device, name, gpu_heap_limit(L),
                                     &data, &len, &w, &h, &fmt);
    if (rc != HL_GPU_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "texture_read_failed");
        return 2;
    }

    /* The read-back and its copy into the image: 1 unit per 8 bytes, before
     * the copy (audit 26). The backend's buffer goes back before a raise. */
    if (hl_lua_budget_charge(L, (int64_t)hl_work_bytes_units((uint64_t)len))) {
        free(data);
        return hl_lua_budget_raise(L);
    }

    /* Wrap as HlImage (takes ownership of data) */
    HlImageAlloc ia;
    hl_lua_image_alloc(L, &ia);
    HlImage *img = hl_image_new(w, h, (HlImageFormat)fmt, data, len, &ia);
    free(data);
    if (!img) {
        lua_pushnil(L);
        lua_pushstring(L, "image_create_failed");
        return 2;
    }

    /* As mod_image.c's push_image: the collector sees only the small
     * userdata, so the pixels are added as GC debt - a loop dropping read-back
     * textures otherwise hit the heap limit before a cycle collected them
     * (audit 10). (push_image is in the image feature archive, which a GPU
     * app need not compose; hence the copy.) */
    size_t kb = img->pixel_len >> 10;
    if (hl_lua_push_slot_safe(L, HL_IMAGE_MT, img) != 0) {
        hl_image_free(img);   /* made before its userdata */
        return luaL_error(L, "not enough memory for the image");
    }
    if (kb > 0)
        lua_gc(L, LUA_GCSTEP, kb > INT_MAX ? INT_MAX : (int)kb);
    return 1;
}
#endif /* HL_ENABLE_IMAGE */

/* ── Texture desc parsing helper (used by dispatch + pipeline) ────── */

static int lua_parse_texture_descs(lua_State *L, int tbl_idx,
                                    HlGpuTextureDesc *descs, int max_descs,
                                    int anchor)
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
            hl_lua_anchor(L, anchor, -1);
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
#ifdef HL_ENABLE_IMAGE
            lua_getfield(L, -1, "image");
            HlImage **imgp2 = (HlImage **)luaL_testudata(L, -1, HL_IMAGE_MT);
            if (imgp2 && *imgp2) {
                HlImage *img = *imgp2;
                hl_lua_anchor(L, anchor, -1);   /* the image owns pixels */
                HlBufferView iv = { img->pixels, img->pixel_len };
                gpu_pin_view(L, anchor, -1, &iv);   /* copied: close() frees pixels */
                descs[i].data = iv.data;
                descs[i].data_len = iv.len;
                if (descs[i].width == 0) descs[i].width = img->width;
                if (descs[i].height == 0) descs[i].height = img->height;
                descs[i].format = image_format_to_gpu(img->format);
            }
            lua_pop(L, 1);
#endif

            if (!descs[i].data) {
                lua_getfield(L, -1, "data");
                HlBufferView bv;
                if (lua_get_buffer(L, -1, &bv)) {
                    hl_lua_anchor(L, anchor, -1);
                    gpu_pin_view(L, anchor, -1, &bv);
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

/* The deep copies gpu.async.dispatch makes before the op owns them, as a
 * to-be-closed slot: a later lua_getfield (an __index error, a number
 * conversion out of memory) raised past plain mallocs, and a pcall loop
 * leaked up to 16 buffer copies per call outside the VM's memory limit. */
typedef struct {
    void            *uni;
    HlGpuBufferDesc *descs;
    void           **datas;
    int              count;
    HlOffheapHold    hold;      /* the copies (gpu_offheap_copy) */
} GpuAsyncTmp;

static int gpu_async_tmp_close(lua_State *L)
{
    GpuAsyncTmp *t = (GpuAsyncTmp *)lua_touserdata(L, 1);
    if (!t) return 0;
    for (int i = 0; i < t->count; i++) {
        if (t->descs) free((void *)(uintptr_t)t->descs[i].name);
        if (t->datas) free(t->datas[i]);
    }
    free(t->descs);
    free(t->datas);
    free(t->uni);
    hl_offheap_drop(&t->hold);
    memset(t, 0, sizeof *t);
    return 0;
}

static GpuAsyncTmp *gpu_async_tmp_open(lua_State *L)
{
    GpuAsyncTmp *t = (GpuAsyncTmp *)lua_newuserdatauv(L, sizeof *t, 0);
    memset(t, 0, sizeof *t);
    if (luaL_newmetatable(L, "hull.gpu.async_tmp")) {
        lua_pushcfunction(L, gpu_async_tmp_close);
        lua_setfield(L, -2, "__close");
        lua_pushcfunction(L, gpu_async_tmp_close);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    lua_toclose(L, -1);
    return t;
}

/* Async dispatch - submits GPU work to thread pool */
static int l_gpu_async_dispatch(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.thread_pool)
        return luaL_error(L, "gpu.async not available (no thread pool)");
    if (!lua->base.async_ctx)
        return luaL_error(L, "gpu.async requires an active event loop");
    if (!lua->base.gpu_ctx)
        return luaL_error(L, "gpu.async.dispatch: GPU not initialized");

    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    /* Parse opts (same as sync dispatch) */
    HlGpuDispatchOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.device = -1;
    opts.max_output = lua->mem_limit;   /* audit 17 */

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
        opts.timeout_ms = gpu_timeout_arg(L, -1);
    lua_pop(L, 1);

    /* Every copy below is owned by `tmp` until the op takes it. */
    int tmp_idx = lua_gettop(L) + 1;
    GpuAsyncTmp *tmp = gpu_async_tmp_open(L);

    /* Parse and deep-copy uniforms */
    void *uni_copy = NULL;
    size_t uni_len = 0;
    lua_getfield(L, 2, "uniforms");
    if (lua_isstring(L, -1)) {
        const char *uni = lua_tolstring(L, -1, &uni_len);
        if (uni && uni_len > 0) {
            const char *err = NULL;
            uni_copy = gpu_offheap_copy(L, lua, &tmp->hold, uni, uni_len, &err);
            if (!uni_copy)
                return luaL_error(L, "gpu.async.dispatch: %s", err);
            tmp->uni = uni_copy;
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
            tmp->descs = buf_descs;
            tmp->datas = buf_data_ptrs;
            tmp->count = (buf_descs && buf_data_ptrs) ? buf_count : 0;
            if (!buf_descs || !buf_data_ptrs)
                return luaL_error(L, "gpu.async.dispatch: out of memory");
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
                            /* A failed copy fails the call: it ran as an
                             * empty buffer (audit 20). `tmp` frees. */
                            const char *err = NULL;
                            buf_data_ptrs[i] = gpu_offheap_copy(L, lua,
                                &tmp->hold, d, dlen, &err);
                            if (!buf_data_ptrs[i])
                                return luaL_error(L, "gpu.async.dispatch: %s",
                                                  err);
                            buf_descs[i].data = buf_data_ptrs[i];
                            buf_descs[i].data_len = dlen;
                            buf_descs[i].size = dlen;
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

    /* After the opts (app code that could BEGIN), before anything is armed;
     * on a refusal `tmp` frees the copies (audit 15). */
    hl_lua_check_can_wait(L, "gpu.async.dispatch()");

    /* Charged here, while `tmp` still owns the copies, as the sync call's
     * (audit 17). */
    {
        HlGpuDispatchOpts c = opts;
        c.buffers = buf_descs;
        c.buffer_count = buf_count;
        c.uniforms = uni_copy;
        c.uniforms_len = uni_len;
        lua_hlwork(L, (size_t)hl_work_bulk_units(
                          (uint64_t)hl_gpu_dispatch_charge_bytes(&c)), 0);
    }

    /* Allocate worker op */
    HlWorkerGpuOp *op = calloc(1, sizeof(HlWorkerGpuOp));
    if (!op)
        return luaL_error(L, "gpu.async.dispatch: out of memory");   /* tmp frees */

    op->server = lua->server;
    op->gpu_ctx = lua->base.gpu_ctx;
    /* Read-backs on the VM's in-flight result total (audit 29). */
    hl_offheap_shared_init(&op->result_hold, &lua->async_result_bytes,
                           lua->mem_limit);
    snprintf(op->shader_name, sizeof(op->shader_name), "%s", name);
    op->opts = opts;
    op->buffers = buf_descs;
    op->buffer_data = buf_data_ptrs;
    op->buffer_count = buf_count;
    op->uniforms = uni_copy;
    op->uniforms_len = uni_len;
    op->copy_hold = tmp->hold;
    /* The op owns the copies now (hl_worker_gpu_op_free releases them on
     * every path below); the slot is closed here, before the yield. */
    memset(tmp, 0, sizeof *tmp);
    lua_closeslot(L, tmp_idx);

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(lua->server, lua->base.net_ctx, lua->base.alloc);
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
    actx->detached = (lua->active_conn == NULL);

    op->async_ctx = actx;
    atomic_store(&op->cancelled, 0);

    if (hl_worker_gpu_submit(lua->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return luaL_error(L, "gpu.async.dispatch: thread pool full");
    }

    if (!actx->detached &&
        hl_net_op_suspend(lua->base.net_ctx, (HlReqHandle *)lua->active_conn, (HlSuspendOp *)&actx->op) < 0) {
        atomic_store(&op->cancelled, 1);
        /* No cancel(): it unrefs the handler coroutine's registry slot,
         * which dispatch unrefs again when this error reaches it - a
         * double luaL_unref corrupts the registry free list. */
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
                                  int *total_bufs, int anchor)
{
    int stage_count = (int)lua_rawlen(L, tbl_idx);
    if (stage_count > max_stages) stage_count = max_stages;
    int buf_offset = 0;

    for (int s = 0; s < stage_count; s++) {
        lua_rawgeti(L, tbl_idx, s + 1);

        /* Zero-init every counted stage BEFORE the type check. A non-table
         * entry takes the `continue` below, but the caller still iterates all
         * stage_count entries; leaving this one uninitialized reads garbage
         * (stages[s].shader) downstream. */
        memset(&stages[s], 0, sizeof(stages[s]));

        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

        /* shader (required) */
        lua_getfield(L, -1, "shader");
        stages[s].shader = lua_tostring(L, -1);
        hl_lua_anchor(L, anchor, -1);
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
        hl_lua_anchor(L, anchor, -1);
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
                    hl_lua_anchor(L, anchor, -1);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "data");
                    {
                        HlBufferView bv;
                        if (lua_get_buffer(L, -1, &bv)) {
                            hl_lua_anchor(L, anchor, -1);
                            gpu_pin_view(L, anchor, -1, &bv);
                            all_bufs[buf_offset + b].data = bv.data;
                            all_bufs[buf_offset + b].data_len = bv.len;
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
    lua_settop(L, 2);                /* opts may be absent: keep slot 2 */
    lua_newtable(L);                 /* anchor: see hl_lua_anchor */
    const int anchor = lua_gettop(L);
    gpu_pins_open(L, anchor);
    HlGpuCtx *ctx = lua_get_gpu_ctx(L);
    luaL_checktype(L, 1, LUA_TTABLE); /* stages array */

    /* Parse stages */
    HlGpuPipelineStage stages[HL_GPU_MAX_PIPELINE_STAGES];
    HlGpuBufferDesc all_bufs[HL_GPU_MAX_PIPELINE_BUFFERS];
    int total_bufs = 0;
    int stage_count = parse_pipeline_stages(L, 1, stages,
        HL_GPU_MAX_PIPELINE_STAGES, all_bufs, HL_GPU_MAX_PIPELINE_BUFFERS,
        &total_bufs, anchor);

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
                } else {
                    /* Not a table: the defaults a table entry gets (it left
                     * the slot uninitialized - stack garbage). */
                    outputs[i].stage = stage_count - 1;
                    outputs[i].buffer = 0;
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
            pipe_timeout_ms = gpu_timeout_arg(L, -1);
        lua_pop(L, 1);
    }

    HlGpuPipelineOpts popts = {
        .stages = stages,
        .stage_count = stage_count,
        .outputs = output_count > 0 ? outputs : NULL,
        .output_count = output_count,
        .device = device,
        .timeout_ms = pipe_timeout_ms,
        .max_output = gpu_heap_limit(L),   /* audit 17 */
    };

    /* The stages' own buffers are charged before they are made (audit 17). */
    lua_hlwork(L, (size_t)hl_work_bulk_units(
                      (uint64_t)hl_gpu_pipeline_charge_bytes(&popts)), 0);

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
        if (hl_lua_pushlstring_safe(L, (const char *)result.data[0], result.len[0]) != 0) {
            hl_cap_gpu_pipeline_result_free(&result);
            return luaL_error(L, "not enough memory for the result");
        }
        hl_cap_gpu_pipeline_result_free(&result);
        return 1;
    }

    /* Protected pushes: a plain one that ran out of memory raised past the
     * free below and leaked every stage output. */
    if (!lua_checkstack(L, 4)) {
        hl_cap_gpu_pipeline_result_free(&result);
        return luaL_error(L, "not enough memory for the result");
    }
    lua_createtable(L, result.count, 0);
    for (int i = 0; i < result.count; i++) {
        if (hl_lua_pushlstring_safe(L, (const char *)result.data[i], result.len[i]) != 0) {
            hl_cap_gpu_pipeline_result_free(&result);
            return luaL_error(L, "not enough memory for the result");
        }
        lua_rawseti(L, -2, i + 1);
    }
    hl_cap_gpu_pipeline_result_free(&result);
    return 1;
}

/* Async pipeline - submits to thread pool */
static void lua_push_worker_gpu_pipeline_result(lua_State *L, void *driver)
{
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)driver;
    lua_newtable(L);
    if (op->error) {
        lua_pushstring(L, op->error_msg);
        lua_setfield(L, -2, "error");
    } else if (op->pipe_result.count > 1) {
        /* More than one output: a table of strings, as gpu.pipeline
         * returns (audit 29: it resolved to "" - the worker moves only a
         * single output to op->output). */
        lua_createtable(L, op->pipe_result.count, 0);
        for (int i = 0; i < op->pipe_result.count; i++) {
            lua_pushlstring(L, (const char *)op->pipe_result.data[i],
                            op->pipe_result.len[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "result");
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
    lua_settop(L, 2);                /* opts may be absent: keep slot 2 */
    lua_newtable(L);                 /* anchor: see hl_lua_anchor */
    const int anchor = lua_gettop(L);
    gpu_pins_open(L, anchor);
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.thread_pool)
        return luaL_error(L, "gpu.async not available (no thread pool)");
    if (!lua->base.async_ctx)
        return luaL_error(L, "gpu.async requires an active event loop");
    if (!lua->base.gpu_ctx)
        return luaL_error(L, "gpu.async.pipeline: GPU not initialized");

    luaL_checktype(L, 1, LUA_TTABLE);

    /* Parse stages (same as sync) */
    HlGpuPipelineStage stages[HL_GPU_MAX_PIPELINE_STAGES];
    HlGpuBufferDesc all_bufs[HL_GPU_MAX_PIPELINE_BUFFERS];
    int total_bufs = 0;
    int stage_count = parse_pipeline_stages(L, 1, stages,
        HL_GPU_MAX_PIPELINE_STAGES, all_bufs, HL_GPU_MAX_PIPELINE_BUFFERS,
        &total_bufs, anchor);
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
                } else {
                    /* Not a table: the defaults a table entry gets (it left
                     * the slot uninitialized - stack garbage). */
                    outputs[i].stage = stage_count - 1;
                    outputs[i].buffer = 0;
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "timeout");
        if (!lua_isnil(L, -1))
            pipe_timeout_ms = gpu_timeout_arg(L, -1);
        lua_pop(L, 1);
    }

    /* After the stages and opts (app code that could BEGIN), before
     * anything is armed (audit 15). */
    hl_lua_check_can_wait(L, "gpu.async.pipeline()");

    /* Charged before the deep copy (nothing is owned yet), as the sync
     * call's (audit 17). */
    {
        HlGpuPipelineOpts c = { .stages = stages, .stage_count = stage_count };
        lua_hlwork(L, (size_t)hl_work_bulk_units(
                          (uint64_t)hl_gpu_pipeline_charge_bytes(&c)), 0);
    }

    /* The copies the op makes below (uniforms and buffer data), counted on
     * the VM's off-heap total under the heap limit and charged 1 unit per 8
     * bytes, before the op exists: nothing bounded them, up to 64 buffers of
     * a 1 GiB mapping per call (audit 20). Charged first - lua_hlwork raises
     * at once when it puts the run over - and taken while nothing is owned. */
    HlOffheapHold copy_hold = {0};
    {
        size_t copy_bytes = 0;
        for (int s = 0; s < stage_count; s++) {
            size_t n = stages[s].uniforms ? stages[s].uniforms_len : 0;
            copy_bytes = copy_bytes <= SIZE_MAX - n ? copy_bytes + n : SIZE_MAX;
        }
        for (int b = 0; b < total_bufs; b++) {
            size_t n = all_bufs[b].data ? all_bufs[b].data_len : 0;
            copy_bytes = copy_bytes <= SIZE_MAX - n ? copy_bytes + n : SIZE_MAX;
        }
        lua_hlwork(L, (size_t)hl_work_bytes_units((uint64_t)copy_bytes), 0);
        if (hl_offheap_take(&copy_hold, &lua->wasm_buf_bytes, lua->mem_limit,
                            copy_bytes) != 0)
            return luaL_error(L, "gpu.async.pipeline: its buffers would take "
                                 "the VM past its heap limit (close unused "
                                 "buffers)");
    }

    /* Deep-copy into worker op */
    HlWorkerGpuOp *op = calloc(1, sizeof(HlWorkerGpuOp));
    if (!op) {
        hl_offheap_drop(&copy_hold);
        return luaL_error(L, "gpu.async.pipeline: out of memory");
    }
    op->copy_hold = copy_hold;   /* hl_worker_gpu_op_free gives it back */

    op->server = lua->server;
    op->gpu_ctx = lua->base.gpu_ctx;
    /* Read-backs on the VM's in-flight result total (audit 29). */
    hl_offheap_shared_init(&op->result_hold, &lua->async_result_bytes,
                           lua->mem_limit);
    op->is_pipeline = 1;
    op->stage_count = stage_count;
    op->pipe_opts.device = device;
    op->pipe_opts.timeout_ms = pipe_timeout_ms;
    op->pipe_opts.max_output = lua->mem_limit;   /* audit 17 */
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
            /* Copy what the data holds (data_len), not `size`: a size
             * larger than the data read past its end. */
            if (src_desc->data && src_desc->data_len > 0) {
                op->buffer_data[buf_off] = malloc(src_desc->data_len);
                if (!op->buffer_data[buf_off]) {
                    hl_worker_gpu_op_free(op); free(op);
                    return luaL_error(L, "gpu.async.pipeline: out of memory");
                }
                memcpy(op->buffer_data[buf_off], src_desc->data,
                       src_desc->data_len);
                dst_desc->data = op->buffer_data[buf_off];
                dst_desc->data_len = src_desc->data_len;
            }
            buf_off++;
        }
    }

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(lua->server, lua->base.net_ctx, lua->base.alloc);
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
    actx->detached = (lua->active_conn == NULL);

    op->async_ctx = actx;
    atomic_store(&op->cancelled, 0);

    if (hl_worker_gpu_submit(lua->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return luaL_error(L, "gpu.async.pipeline: thread pool full");
    }

    if (!actx->detached &&
        hl_net_op_suspend(lua->base.net_ctx, (HlReqHandle *)lua->active_conn, (HlSuspendOp *)&actx->op) < 0) {
        atomic_store(&op->cancelled, 1);
        /* No cancel(): it unrefs the handler coroutine's registry slot,
         * which dispatch unrefs again when this error reaches it - a
         * double luaL_unref corrupts the registry free list. */
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
#ifdef HL_ENABLE_IMAGE
    {"texture_read",  l_gpu_texture_read},
#endif
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

