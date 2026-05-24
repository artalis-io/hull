/*
 * mod_gpu.c — hull:gpu module (GPU compute via wgpu-native)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_GPU

#include "mod_buffer.h"
#include "hull/cap/gpu.h"
#include "hull/cap/image.h"
#include "hull/worker_gpu.h"
#include "hull/async.h"
#include "hull/net_backend.h"
#include "hull/alloc.h"
#include "hull/vfs.h"

/* Forward declaration */
static int js_parse_texture_descs(JSContext *ctx, JSValueConst arr,
                                   HlGpuTextureDesc *descs, int max_descs,
                                   const char **tracked_strs, int *str_count,
                                   int str_max);

#include <keel/server.h>
#include <stdio.h>
#include <stdatomic.h>

static HlGpuCtx *js_get_gpu_ctx(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue hull_opaque = JS_GetPropertyStr(ctx, global, "__hull_js");
    HlJS *js = (HlJS *)JS_GetOpaque(hull_opaque, 1);
    JS_FreeValue(ctx, hull_opaque);
    JS_FreeValue(ctx, global);
    return js ? js->base.gpu_ctx : NULL;
}

static JSValue js_gpu_available(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    return JS_NewBool(ctx, hl_cap_gpu_available(gpu));
}

static JSValue js_gpu_devices(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    int count = hl_cap_gpu_device_count(gpu);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < count; i++) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, obj, "name",
                          JS_NewString(ctx, hl_cap_gpu_device_name(gpu, i)));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
    }
    return arr;
}

/* Load WGSL shader from shaders/<name>.wgsl (VFS or disk) */
static JSValue js_gpu_load(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "gpu.load requires name");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu) return JS_ThrowInternalError(ctx, "gpu not available");

    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    /* Validate shader name: reject path traversal */
    if (!name[0] || name[0] == '/' || strstr(name, "..") || strchr(name, '\\')) {
        JS_FreeCString(ctx, name);
        return JS_ThrowRangeError(ctx, "invalid shader name");
    }

    char vfs_name[512];
    snprintf(vfs_name, sizeof(vfs_name), "shaders/%s.wgsl", name);
    const HlEntry *entry = NULL;
    if (js && js->base.app_vfs)
        entry = hl_vfs_find(js->base.app_vfs, vfs_name);

    const char *wgsl = NULL;
    size_t wgsl_len = 0;
    char *file_buf = NULL;

    if (entry && entry->data) {
        wgsl = (const char *)entry->data;
        wgsl_len = entry->len;
    } else if (js && js->base.app_vfs && js->base.app_vfs->root_dir) {
        char path[4096];
        int pn = snprintf(path, sizeof(path), "%s/shaders/%s.wgsl",
                 js->base.app_vfs->root_dir, name);
        if (pn < 0 || (size_t)pn >= sizeof(path)) {
            JS_FreeCString(ctx, name);
            return JS_ThrowInternalError(ctx, "gpu.load: shader path too long");
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
        JSValue err = JS_ThrowInternalError(ctx, "gpu.load: shader '%s' not found in shaders/", name);
        JS_FreeCString(ctx, name);
        return err;
    }

    int rc = hl_cap_gpu_compile(gpu, -1, name, wgsl, wgsl_len);
    free(file_buf);
    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.load: shader_error");

    return JS_TRUE;
}

static JSValue js_gpu_compile(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "gpu.compile requires name and wgsl");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    size_t wgsl_len;
    const char *wgsl = JS_ToCStringLen(ctx, &wgsl_len, argv[1]);
    if (!wgsl) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }

    int rc = hl_cap_gpu_compile(gpu, -1, name, wgsl, wgsl_len);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, wgsl);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.compile failed: shader_error");

    return JS_TRUE;
}

static JSValue js_gpu_dispatch(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "gpu.dispatch requires name and opts");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu)
        return JS_ThrowInternalError(ctx, "gpu not available");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    HlGpuDispatchOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.device = -1;

    JSValue opts_val = argv[1];

    /* Parse device */
    JSValue dev_val = JS_GetPropertyStr(ctx, opts_val, "device");
    if (!JS_IsUndefined(dev_val)) {
        int32_t d;
        JS_ToInt32(ctx, &d, dev_val);
        opts.device = d;
    }
    JS_FreeValue(ctx, dev_val);

    /* Parse workgroups */
    JSValue wg_val = JS_GetPropertyStr(ctx, opts_val, "workgroups");
    if (JS_IsObject(wg_val)) {
        JSValue xv = JS_GetPropertyStr(ctx, wg_val, "x");
        JSValue yv = JS_GetPropertyStr(ctx, wg_val, "y");
        JSValue zv = JS_GetPropertyStr(ctx, wg_val, "z");
        uint32_t x = 1, y = 1, z = 1;
        JS_ToUint32(ctx, &x, xv);
        JS_ToUint32(ctx, &y, yv);
        JS_ToUint32(ctx, &z, zv);
        opts.workgroups.x = x;
        opts.workgroups.y = y;
        opts.workgroups.z = z;
        JS_FreeValue(ctx, xv);
        JS_FreeValue(ctx, yv);
        JS_FreeValue(ctx, zv);
    }
    JS_FreeValue(ctx, wg_val);

    /* Parse output buffer index (0-indexed in JS).
     * output = N     → read back buffer N
     * output = false → fire-and-forget (skip readback)
     * output absent  → default to 0 (backward compat) */
    JSValue out_val = JS_GetPropertyStr(ctx, opts_val, "output");
    if (JS_IsBool(out_val) && !JS_ToBool(ctx, out_val)) {
        opts.output_buffer = HL_GPU_OUTPUT_NONE;
    } else if (!JS_IsUndefined(out_val)) {
        int32_t o;
        JS_ToInt32(ctx, &o, out_val);
        opts.output_buffer = o;
    }
    JS_FreeValue(ctx, out_val);

    /* Parse timeout */
    JSValue timeout_val = JS_GetPropertyStr(ctx, opts_val, "timeout");
    if (!JS_IsUndefined(timeout_val)) {
        uint32_t t = 0;
        JS_ToUint32(ctx, &t, timeout_val);
        opts.timeout_ms = t;
    }
    JS_FreeValue(ctx, timeout_val);

    /* Parse uniforms (string or ArrayBuffer) */
    const char *uni_str = NULL;  /* non-NULL if JS_ToCStringLen was used */
    JSValue uni_val = JS_GetPropertyStr(ctx, opts_val, "uniforms");
    if (!JS_IsUndefined(uni_val) && !JS_IsNull(uni_val)) {
        size_t uni_len;
        uint8_t *uni_ab = JS_GetArrayBuffer(ctx, &uni_len, uni_val);
        if (uni_ab) {
            opts.uniforms = uni_ab;
            opts.uniforms_len = uni_len;
        } else {
            uni_str = JS_ToCStringLen(ctx, &uni_len, uni_val);
            if (uni_str) {
                opts.uniforms = uni_str;
                opts.uniforms_len = uni_len;
            }
        }
    }
    JS_FreeValue(ctx, uni_val);

    /* Parse buffers array */
    HlGpuBufferDesc bufs[16];
    memset(bufs, 0, sizeof(bufs));
    int buf_count = 0;
    /* Keep JS string refs alive until after dispatch */
    const char *buf_name_strs[16] = {0};
    const char *buf_data_strs[16] = {0};

    JSValue bufs_val = JS_GetPropertyStr(ctx, opts_val, "buffers");
    if (JS_IsArray(ctx, bufs_val)) {
        JSValue len_val = JS_GetPropertyStr(ctx, bufs_val, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        if (len > 16) len = 16;

        for (int32_t i = 0; i < len; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, bufs_val, (uint32_t)i);
            if (JS_IsObject(elem)) {
                JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
                if (JS_IsString(nv)) {
                    buf_name_strs[buf_count] = JS_ToCString(ctx, nv);
                    bufs[buf_count].name = buf_name_strs[buf_count];
                }
                JS_FreeValue(ctx, nv);

                JSValue dv = JS_GetPropertyStr(ctx, elem, "data");
                if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                    size_t dlen;
                    uint8_t *dab = JS_GetArrayBuffer(ctx, &dlen, dv);
                    if (dab) {
                        bufs[buf_count].data = dab;
                        bufs[buf_count].size = dlen;
                    } else {
                        buf_data_strs[buf_count] = JS_ToCStringLen(ctx, &dlen, dv);
                        if (buf_data_strs[buf_count]) {
                            bufs[buf_count].data = buf_data_strs[buf_count];
                            bufs[buf_count].size = dlen;
                        }
                    }
                }
                JS_FreeValue(ctx, dv);

                JSValue sv = JS_GetPropertyStr(ctx, elem, "size");
                if (!JS_IsUndefined(sv)) {
                    int64_t sz;
                    JS_ToInt64(ctx, &sz, sv);
                    bufs[buf_count].size = (size_t)sz;
                }
                JS_FreeValue(ctx, sv);

                JSValue uv = JS_GetPropertyStr(ctx, elem, "usage");
                if (JS_IsString(uv)) {
                    const char *us = JS_ToCString(ctx, uv);
                    if (us) {
                        if (strcmp(us, "read") == 0)
                            bufs[buf_count].usage = HL_GPU_USAGE_READ;
                        else if (strcmp(us, "write") == 0)
                            bufs[buf_count].usage = HL_GPU_USAGE_WRITE;
                        else if (strcmp(us, "readwrite") == 0)
                            bufs[buf_count].usage = HL_GPU_USAGE_READWRITE;
                        JS_FreeCString(ctx, us);
                    }
                }
                JS_FreeValue(ctx, uv);

                bufs[buf_count].binding = -1;
                buf_count++;
            }
            JS_FreeValue(ctx, elem);
        }
    }
    JS_FreeValue(ctx, bufs_val);

    opts.buffers = bufs;
    opts.buffer_count = buf_count;
    opts.output_texture = -1;

    /* Parse textures */
    HlGpuTextureDesc tex_descs[8];
    const char *tex_strs[32] = {0};
    int tex_str_count = 0;
    JSValue tex_arr = JS_GetPropertyStr(ctx, opts_val, "textures");
    int tex_count = js_parse_texture_descs(ctx, tex_arr, tex_descs, 8,
                                            tex_strs, &tex_str_count, 32);
    JS_FreeValue(ctx, tex_arr);
    opts.textures = tex_descs;
    opts.texture_count = tex_count;

    /* Parse output_texture (0-indexed in JS) */
    JSValue ot_val = JS_GetPropertyStr(ctx, opts_val, "outputTexture");
    if (!JS_IsUndefined(ot_val)) {
        int32_t ot;
        JS_ToInt32(ctx, &ot, ot_val);
        opts.output_texture = ot;
    }
    JS_FreeValue(ctx, ot_val);

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    /* Fire-and-forget: pass NULL output pointers */
    void **out_ptr = (opts.output_buffer >= 0 || opts.output_texture >= 0)
                      ? &output : NULL;
    size_t *out_len_ptr = (opts.output_buffer >= 0 || opts.output_texture >= 0)
                           ? &output_len : NULL;

    int rc = hl_cap_gpu_dispatch(gpu, name, &opts,
                                 out_ptr, out_len_ptr, &err_msg);

    /* Free uniform string if we used JS_ToCStringLen */
    if (uni_str) JS_FreeCString(ctx, uni_str);

    /* Free buffer name/data strings */
    for (int i = 0; i < buf_count; i++) {
        if (buf_name_strs[i]) JS_FreeCString(ctx, buf_name_strs[i]);
        if (buf_data_strs[i]) JS_FreeCString(ctx, buf_data_strs[i]);
    }

    /* Free texture tracked strings */
    for (int i = 0; i < tex_str_count; i++)
        JS_FreeCString(ctx, tex_strs[i]);

    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.dispatch: %s",
                                     err_msg ? err_msg : "dispatch_failed");

    if (output) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)output, output_len);
        free(output);
        return ab;
    }
    return JS_TRUE; /* fire-and-forget */
}

static JSValue js_gpu_buffer(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "gpu.buffer requires name and data/null");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu)
        return JS_ThrowInternalError(ctx, "gpu not available");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    int device = -1;

    /* null = destroy */
    if (JS_IsNull(argv[1])) {
        hl_cap_gpu_buffer_destroy(gpu, device, name);
        JS_FreeCString(ctx, name);
        return JS_TRUE;
    }

    /* Get data from any buffer type (MappedBuffer, WasmBuffer, ArrayBuffer, string) */
    HlBufferView bv;
    const char *str_data = NULL;
    int str_needs_free = 0;
    if (!js_get_buffer(ctx, argv[1], &bv, &str_data, &str_needs_free)) {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "gpu.buffer: data must be a buffer type");
    }
    const uint8_t *data = (const uint8_t *)bv.data;
    size_t data_len = bv.len;

    size_t offset = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue off_val = JS_GetPropertyStr(ctx, argv[2], "offset");
        if (!JS_IsUndefined(off_val)) {
            int64_t o;
            JS_ToInt64(ctx, &o, off_val);
            offset = (size_t)o;
        }
        JS_FreeValue(ctx, off_val);
        JSValue dev_val = JS_GetPropertyStr(ctx, argv[2], "device");
        if (!JS_IsUndefined(dev_val)) {
            int32_t d;
            JS_ToInt32(ctx, &d, dev_val);
            device = d;
        }
        JS_FreeValue(ctx, dev_val);
    }

    int rc = hl_cap_gpu_buffer_write(gpu, device, name, data, data_len, offset);
    if (rc == HL_GPU_ERR_NOT_FOUND) {
        rc = hl_cap_gpu_buffer_create(gpu, device, name,
                                      data_len, HL_GPU_USAGE_READWRITE);
        if (rc == HL_GPU_OK)
            rc = hl_cap_gpu_buffer_write(gpu, device, name,
                                         data, data_len, 0);
    }

    if (str_data) JS_FreeCString(ctx, str_data);
    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.buffer failed");

    return JS_TRUE;
}

static JSValue js_gpu_buffer_read(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "gpu.bufferRead requires name");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu)
        return JS_ThrowInternalError(ctx, "gpu not available");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    int device = -1;
    void *data = NULL;
    size_t len = 0;

    int rc = hl_cap_gpu_buffer_read(gpu, device, name, &data, &len);
    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.bufferRead failed");

    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)data, len);
    free(data);
    return ab;
}

static JSValue js_gpu_buffer_copy(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "gpu.bufferCopy requires src and dst");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu)
        return JS_ThrowInternalError(ctx, "gpu not available");

    const char *src = JS_ToCString(ctx, argv[0]);
    if (!src) return JS_EXCEPTION;
    const char *dst = JS_ToCString(ctx, argv[1]);
    if (!dst) { JS_FreeCString(ctx, src); return JS_EXCEPTION; }

    size_t src_offset = 0, dst_offset = 0, size = 0;
    int device = -1;

    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[2], "srcOffset");
        if (!JS_IsUndefined(v)) { int64_t x; JS_ToInt64(ctx, &x, v); src_offset = (size_t)x; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "dstOffset");
        if (!JS_IsUndefined(v)) { int64_t x; JS_ToInt64(ctx, &x, v); dst_offset = (size_t)x; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "size");
        if (!JS_IsUndefined(v)) { int64_t x; JS_ToInt64(ctx, &x, v); size = (size_t)x; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "device");
        if (!JS_IsUndefined(v)) { int32_t d; JS_ToInt32(ctx, &d, v); device = d; }
        JS_FreeValue(ctx, v);
    }

    int rc = hl_cap_gpu_buffer_copy(gpu, device, src, dst,
                                     src_offset, dst_offset, size);
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, dst);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.bufferCopy: %s",
                   rc == HL_GPU_ERR_NOT_FOUND ? "buffer_not_found"
                 : rc == HL_GPU_ERR_BUFFER ? "out_of_bounds"
                 : "copy_failed");

    return JS_TRUE;
}

/* ── Texture format helper ─────────────────────────────────────────── */

static HlGpuTexFormat js_parse_tex_format(const char *s)
{
    if (!s) return HL_GPU_TEX_RGBA8;
    if (strcmp(s, "r8") == 0)      return HL_GPU_TEX_R8;
    if (strcmp(s, "rgba16f") == 0)  return HL_GPU_TEX_RGBA16F;
    if (strcmp(s, "r32f") == 0)     return HL_GPU_TEX_R32F;
    return HL_GPU_TEX_RGBA8;
}

/* ── gpu.texture(name, data|null, opts?) ──────────────────────────── */

static JSValue js_gpu_texture(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "gpu.texture requires name and data/null");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu) return JS_ThrowInternalError(ctx, "gpu not available");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    int device = -1;

    /* null = destroy */
    if (JS_IsNull(argv[1])) {
        hl_cap_gpu_texture_destroy(gpu, device, name);
        JS_FreeCString(ctx, name);
        return JS_TRUE;
    }

    uint32_t width = 0, height = 0;
    HlGpuTexFormat format = HL_GPU_TEX_RGBA8;
    int storage = 0;
    int filter = HL_GPU_FILTER_NEAREST;
    int address_u = HL_GPU_ADDRESS_CLAMP;
    int address_v = HL_GPU_ADDRESS_CLAMP;
    const void *pixels = NULL;
    size_t pixel_len = 0;
    const char *str_data = NULL;
    int str_needs_free = 0;

    /* Check if arg 1 is an HlImage object */
    HlImage *img = JS_GetOpaque(argv[1], js_image_class_id);
    if (img) {
        width = img->width;
        height = img->height;
        format = (HlGpuTexFormat)img->format;
        pixels = img->pixels;
        pixel_len = img->pixel_len;
    } else {
        /* Get data from buffer protocol */
        HlBufferView bv;
        if (js_get_buffer(ctx, argv[1], &bv, &str_data, &str_needs_free)) {
            pixels = bv.data;
            pixel_len = bv.len;
        } else {
            JS_FreeCString(ctx, name);
            return JS_ThrowTypeError(ctx, "gpu.texture: data must be HlImage, ArrayBuffer, or string");
        }
    }

    /* Parse opts */
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[2], "device");
        if (!JS_IsUndefined(v)) { int32_t d; JS_ToInt32(ctx, &d, v); device = d; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "width");
        if (!JS_IsUndefined(v)) { uint32_t w; JS_ToUint32(ctx, &w, v); width = w; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "height");
        if (!JS_IsUndefined(v)) { uint32_t h; JS_ToUint32(ctx, &h, v); height = h; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "format");
        if (JS_IsString(v)) {
            const char *f = JS_ToCString(ctx, v);
            if (f) { format = js_parse_tex_format(f); JS_FreeCString(ctx, f); }
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "storage");
        if (JS_ToBool(ctx, v)) storage = 1;
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "filter");
        if (JS_IsString(v)) {
            const char *f = JS_ToCString(ctx, v);
            if (f && strcmp(f, "linear") == 0) filter = HL_GPU_FILTER_LINEAR;
            if (f) JS_FreeCString(ctx, f);
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "wrap");
        if (JS_IsString(v)) {
            const char *w = JS_ToCString(ctx, v);
            if (w) {
                if (strcmp(w, "repeat") == 0) {
                    address_u = HL_GPU_ADDRESS_REPEAT;
                    address_v = HL_GPU_ADDRESS_REPEAT;
                } else if (strcmp(w, "mirror") == 0) {
                    address_u = HL_GPU_ADDRESS_MIRROR;
                    address_v = HL_GPU_ADDRESS_MIRROR;
                }
                JS_FreeCString(ctx, w);
            }
        }
        JS_FreeValue(ctx, v);
    }

    if (width == 0 || height == 0) {
        if (str_data) JS_FreeCString(ctx, str_data);
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "gpu.texture: width and height required");
    }

    int rc = hl_cap_gpu_texture_create(gpu, device, name, width, height,
                                        format, storage, filter,
                                        address_u, address_v);
    if (rc == HL_GPU_OK && pixels && pixel_len > 0)
        rc = hl_cap_gpu_texture_write(gpu, device, name, pixels, pixel_len);

    if (str_data) JS_FreeCString(ctx, str_data);
    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.texture failed");
    return JS_TRUE;
}

/* ── gpu.textureRead(name, opts?) → HlImage ──────────────────────── */

static JSValue js_gpu_texture_read(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "gpu.textureRead requires name");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu) return JS_ThrowInternalError(ctx, "gpu not available");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    int device = -1;
    void *data = NULL;
    size_t len = 0;
    uint32_t w = 0, h = 0;
    HlGpuTexFormat fmt = HL_GPU_TEX_RGBA8;

    int rc = hl_cap_gpu_texture_read(gpu, device, name, &data, &len,
                                      &w, &h, &fmt);
    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.textureRead failed");

    /* Create HlImage, then wrap as JS object */
    HlImage *img = hl_image_new(w, h, (HlImageFormat)fmt, data, len, NULL);
    free(data);
    if (!img)
        return JS_ThrowInternalError(ctx, "gpu.textureRead: image creation failed");

    /* Wrap using js_image_class_id (same pattern as mod_image.c) */
    JSValue obj = JS_NewObjectClass(ctx, (int)js_image_class_id);
    if (JS_IsException(obj)) {
        hl_image_free(img);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, img);
    return obj;
}

/* ── JS texture desc parsing helper ──────────────────────────────── */

static int js_parse_texture_descs(JSContext *ctx, JSValueConst arr,
                                   HlGpuTextureDesc *descs, int max_descs,
                                   const char **tracked_strs, int *str_count,
                                   int str_max)
{
    if (!JS_IsArray(ctx, arr))
        return 0;
    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t count = 0;
    JS_ToInt32(ctx, &count, len_val);
    JS_FreeValue(ctx, len_val);
    if (count > max_descs) count = max_descs;

    for (int32_t i = 0; i < count; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        memset(&descs[i], 0, sizeof(descs[i]));
        descs[i].binding = -1;

        if (JS_IsObject(elem)) {
            JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
            if (JS_IsString(nv)) {
                descs[i].name = JS_ToCString(ctx, nv);
                if (descs[i].name && *str_count < str_max)
                    tracked_strs[(*str_count)++] = descs[i].name;
            }
            JS_FreeValue(ctx, nv);

            JSValue sv2 = JS_GetPropertyStr(ctx, elem, "storage");
            descs[i].storage = JS_ToBool(ctx, sv2);
            JS_FreeValue(ctx, sv2);

            JSValue wv = JS_GetPropertyStr(ctx, elem, "width");
            if (!JS_IsUndefined(wv)) { uint32_t w; JS_ToUint32(ctx, &w, wv); descs[i].width = w; }
            JS_FreeValue(ctx, wv);

            JSValue hv = JS_GetPropertyStr(ctx, elem, "height");
            if (!JS_IsUndefined(hv)) { uint32_t h; JS_ToUint32(ctx, &h, hv); descs[i].height = h; }
            JS_FreeValue(ctx, hv);

            JSValue fv = JS_GetPropertyStr(ctx, elem, "format");
            if (JS_IsString(fv)) {
                const char *f = JS_ToCString(ctx, fv);
                if (f) { descs[i].format = js_parse_tex_format(f); JS_FreeCString(ctx, f); }
            }
            JS_FreeValue(ctx, fv);

            /* data: ArrayBuffer or string */
            JSValue dv = JS_GetPropertyStr(ctx, elem, "data");
            if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                size_t dlen;
                uint8_t *dab = JS_GetArrayBuffer(ctx, &dlen, dv);
                if (dab) {
                    descs[i].data = dab;
                    descs[i].data_len = dlen;
                } else {
                    const char *ds = JS_ToCStringLen(ctx, &dlen, dv);
                    if (ds) {
                        descs[i].data = ds;
                        descs[i].data_len = dlen;
                        if (*str_count < str_max)
                            tracked_strs[(*str_count)++] = ds;
                    }
                }
            }
            JS_FreeValue(ctx, dv);
        }
        JS_FreeValue(ctx, elem);
    }
    return count;
}

/* push_result callback: convert HlWorkerGpuOp result to JS value */
static JSValue js_push_worker_gpu_result(JSContext *ctx, void *driver)
{
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)driver;
    if (op->error)
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: %s", op->error_msg);
    if (op->output && op->output_len > 0)
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)op->output, op->output_len);
    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

/* Async dispatch — submits GPU work to thread pool, returns Promise */
static JSValue js_gpu_async_dispatch(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx, "gpu.async not available (no thread pool)");
    if (!js->base.async_ctx)
        return JS_ThrowInternalError(ctx, "gpu.async requires an active event loop");
    if (!js->base.gpu_ctx)
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: GPU not initialized");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "gpu.async.dispatch requires name and opts");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    /* Parse opts */
    HlGpuDispatchOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.device = -1;

    JSValue opts_val = argv[1];

    JSValue dev_val = JS_GetPropertyStr(ctx, opts_val, "device");
    if (!JS_IsUndefined(dev_val)) { int32_t d; JS_ToInt32(ctx, &d, dev_val); opts.device = d; }
    JS_FreeValue(ctx, dev_val);

    JSValue wg_val = JS_GetPropertyStr(ctx, opts_val, "workgroups");
    if (JS_IsObject(wg_val)) {
        JSValue xv = JS_GetPropertyStr(ctx, wg_val, "x");
        JSValue yv = JS_GetPropertyStr(ctx, wg_val, "y");
        JSValue zv = JS_GetPropertyStr(ctx, wg_val, "z");
        uint32_t x = 1, y = 1, z = 1;
        JS_ToUint32(ctx, &x, xv); JS_ToUint32(ctx, &y, yv); JS_ToUint32(ctx, &z, zv);
        opts.workgroups.x = x; opts.workgroups.y = y; opts.workgroups.z = z;
        JS_FreeValue(ctx, xv); JS_FreeValue(ctx, yv); JS_FreeValue(ctx, zv);
    }
    JS_FreeValue(ctx, wg_val);

    JSValue out_val = JS_GetPropertyStr(ctx, opts_val, "output");
    if (!JS_IsUndefined(out_val)) { int32_t o; JS_ToInt32(ctx, &o, out_val); opts.output_buffer = o; }
    JS_FreeValue(ctx, out_val);

    JSValue async_timeout_val = JS_GetPropertyStr(ctx, opts_val, "timeout");
    if (!JS_IsUndefined(async_timeout_val)) {
        uint32_t t = 0;
        JS_ToUint32(ctx, &t, async_timeout_val);
        opts.timeout_ms = t;
    }
    JS_FreeValue(ctx, async_timeout_val);

    /* Deep-copy uniforms */
    void *uni_copy = NULL;
    size_t uni_len = 0;
    JSValue uni_val = JS_GetPropertyStr(ctx, opts_val, "uniforms");
    if (!JS_IsUndefined(uni_val) && !JS_IsNull(uni_val)) {
        uint8_t *uni_ab = JS_GetArrayBuffer(ctx, &uni_len, uni_val);
        if (uni_ab && uni_len > 0) {
            uni_copy = malloc(uni_len);
            if (uni_copy) memcpy(uni_copy, uni_ab, uni_len);
        } else {
            const char *uni_str = JS_ToCStringLen(ctx, &uni_len, uni_val);
            if (uni_str) {
                if (uni_len > 0) {
                    uni_copy = malloc(uni_len);
                    if (uni_copy) memcpy(uni_copy, uni_str, uni_len);
                }
                JS_FreeCString(ctx, uni_str);
            }
        }
    }
    JS_FreeValue(ctx, uni_val);

    /* Deep-copy buffers */
    int buf_count = 0;
    HlGpuBufferDesc *buf_descs = NULL;
    void **buf_data_ptrs = NULL;

    JSValue bufs_val = JS_GetPropertyStr(ctx, opts_val, "buffers");
    if (JS_IsArray(ctx, bufs_val)) {
        JSValue len_val = JS_GetPropertyStr(ctx, bufs_val, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        if (len > 16) len = 16;
        if (len > 0) {
            buf_descs = calloc((size_t)len, sizeof(HlGpuBufferDesc));
            buf_data_ptrs = calloc((size_t)len, sizeof(void *));
            if (!buf_descs || !buf_data_ptrs) {
                free(buf_descs); free(buf_data_ptrs); free(uni_copy);
                JS_FreeValue(ctx, bufs_val);
                JS_FreeCString(ctx, name);
                return JS_ThrowInternalError(ctx, "gpu.async.dispatch: out of memory");
            }
            for (int32_t i = 0; i < len; i++) {
                JSValue elem = JS_GetPropertyUint32(ctx, bufs_val, (uint32_t)i);
                if (JS_IsObject(elem)) {
                    /* Deep-copy buffer name for thread safety */
                    JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
                    if (JS_IsString(nv)) {
                        size_t nlen;
                        const char *ns = JS_ToCStringLen(ctx, &nlen, nv);
                        if (ns && nlen > 0 && nlen < HL_GPU_NAME_MAX) {
                            char *ncopy = malloc(nlen + 1);
                            if (ncopy) {
                                memcpy(ncopy, ns, nlen);
                                ncopy[nlen] = '\0';
                                buf_descs[buf_count].name = ncopy;
                            }
                        }
                        if (ns) JS_FreeCString(ctx, ns);
                    }
                    JS_FreeValue(ctx, nv);

                    JSValue dv = JS_GetPropertyStr(ctx, elem, "data");
                    if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                        size_t dlen;
                        uint8_t *dab = JS_GetArrayBuffer(ctx, &dlen, dv);
                        if (dab && dlen > 0) {
                            buf_data_ptrs[buf_count] = malloc(dlen);
                            if (buf_data_ptrs[buf_count]) {
                                memcpy(buf_data_ptrs[buf_count], dab, dlen);
                                buf_descs[buf_count].data = buf_data_ptrs[buf_count];
                                buf_descs[buf_count].size = dlen;
                            }
                        } else {
                            const char *ds = JS_ToCStringLen(ctx, &dlen, dv);
                            if (ds) {
                                if (dlen > 0) {
                                    buf_data_ptrs[buf_count] = malloc(dlen);
                                    if (buf_data_ptrs[buf_count]) {
                                        memcpy(buf_data_ptrs[buf_count], ds, dlen);
                                        buf_descs[buf_count].data = buf_data_ptrs[buf_count];
                                        buf_descs[buf_count].size = dlen;
                                    }
                                }
                                JS_FreeCString(ctx, ds);
                            }
                        }
                    }
                    JS_FreeValue(ctx, dv);

                    JSValue sv = JS_GetPropertyStr(ctx, elem, "size");
                    if (!JS_IsUndefined(sv)) { int64_t sz; JS_ToInt64(ctx, &sz, sv); buf_descs[buf_count].size = (size_t)sz; }
                    JS_FreeValue(ctx, sv);

                    JSValue uv = JS_GetPropertyStr(ctx, elem, "usage");
                    if (JS_IsString(uv)) {
                        const char *us = JS_ToCString(ctx, uv);
                        if (us) {
                            if (strcmp(us, "read") == 0) buf_descs[buf_count].usage = HL_GPU_USAGE_READ;
                            else if (strcmp(us, "write") == 0) buf_descs[buf_count].usage = HL_GPU_USAGE_WRITE;
                            else if (strcmp(us, "readwrite") == 0) buf_descs[buf_count].usage = HL_GPU_USAGE_READWRITE;
                            JS_FreeCString(ctx, us);
                        }
                    }
                    JS_FreeValue(ctx, uv);

                    buf_descs[buf_count].binding = -1;
                    buf_count++;
                }
                JS_FreeValue(ctx, elem);
            }
        }
    }
    JS_FreeValue(ctx, bufs_val);

    /* Allocate worker op */
    HlWorkerGpuOp *op = calloc(1, sizeof(HlWorkerGpuOp));
    if (!op) {
        for (int i = 0; i < buf_count; i++) free(buf_data_ptrs[i]);
        free(buf_data_ptrs); free(buf_descs); free(uni_copy);
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: out of memory");
    }

    op->server = js->server;
    op->gpu_ctx = js->base.gpu_ctx;
    snprintf(op->shader_name, sizeof(op->shader_name), "%s", name);
    op->opts = opts;
    op->buffers = buf_descs;
    op->buffer_data = buf_data_ptrs;
    op->buffer_count = buf_count;
    op->uniforms = uni_copy;
    op->uniforms_len = uni_len;

    JS_FreeCString(ctx, name);

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx, js->base.alloc);
    if (!actx) {
        hl_worker_gpu_op_free(op); free(op);
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: out of memory");
    }

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_worker_gpu_result);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_gpu_op_free_all;
    actx->op.on_cancel = hl_worker_gpu_async_cancel;
    actx->detached = (js->active_conn == NULL);

    op->async_ctx = actx;
    atomic_store(&op->cancelled, 0);

    if (hl_worker_gpu_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: thread pool full");
    }

    if (!actx->detached &&
        hl_net_op_suspend(js->base.net_ctx, (HlReqHandle *)js->active_conn, (HlSuspendOp *)&actx->op) < 0) {
        atomic_store(&op->cancelled, 1);
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: failed to suspend");
    }

    return promise;
}

/* ── gpu.pipeline() ──────────────────────────────────────────────── */

static JSValue js_gpu_pipeline(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "gpu.pipeline requires stages array");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu) return JS_ThrowInternalError(ctx, "gpu not available");

    if (!JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "gpu.pipeline: first arg must be array");

    /* Parse stages */
    JSValue len_val = JS_GetPropertyStr(ctx, argv[0], "length");
    int32_t stage_count = 0;
    JS_ToInt32(ctx, &stage_count, len_val);
    JS_FreeValue(ctx, len_val);
    if (stage_count <= 0 || stage_count > HL_GPU_MAX_PIPELINE_STAGES)
        return JS_ThrowRangeError(ctx, "gpu.pipeline: invalid stage count");

    HlGpuPipelineStage stages[HL_GPU_MAX_PIPELINE_STAGES];
    HlGpuBufferDesc all_bufs[HL_GPU_MAX_PIPELINE_BUFFERS];
    memset(stages, 0, sizeof(stages));
    memset(all_bufs, 0, sizeof(all_bufs));
    int buf_offset = 0;

    /* Track JS strings for cleanup */
    const char *js_strings[256];
    int js_string_count = 0;

    #define JS_TRACK_STR(s) do { \
        if ((s) && js_string_count < 256) js_strings[js_string_count++] = (s); \
    } while (0)

    for (int32_t s = 0; s < stage_count; s++) {
        JSValue stage_val = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)s);
        if (!JS_IsObject(stage_val)) { JS_FreeValue(ctx, stage_val); continue; }

        /* shader */
        JSValue sv2 = JS_GetPropertyStr(ctx, stage_val, "shader");
        stages[s].shader = JS_ToCString(ctx, sv2);
        JS_TRACK_STR(stages[s].shader);
        JS_FreeValue(ctx, sv2);

        /* workgroups */
        JSValue wg = JS_GetPropertyStr(ctx, stage_val, "workgroups");
        if (JS_IsObject(wg)) {
            JSValue xv = JS_GetPropertyStr(ctx, wg, "x");
            JSValue yv = JS_GetPropertyStr(ctx, wg, "y");
            JSValue zv = JS_GetPropertyStr(ctx, wg, "z");
            uint32_t x = 1, y = 1, z = 1;
            JS_ToUint32(ctx, &x, xv); JS_ToUint32(ctx, &y, yv); JS_ToUint32(ctx, &z, zv);
            stages[s].workgroups = (HlGpuWorkgroups){ x, y, z };
            JS_FreeValue(ctx, xv); JS_FreeValue(ctx, yv); JS_FreeValue(ctx, zv);
        }
        JS_FreeValue(ctx, wg);

        /* uniforms */
        JSValue uni = JS_GetPropertyStr(ctx, stage_val, "uniforms");
        if (!JS_IsUndefined(uni) && !JS_IsNull(uni)) {
            size_t ulen;
            uint8_t *uab = JS_GetArrayBuffer(ctx, &ulen, uni);
            if (uab) {
                stages[s].uniforms = uab;
                stages[s].uniforms_len = ulen;
            } else {
                const char *us = JS_ToCStringLen(ctx, &ulen, uni);
                if (us) {
                    stages[s].uniforms = us;
                    stages[s].uniforms_len = ulen;
                    JS_TRACK_STR(us);
                }
            }
        }
        JS_FreeValue(ctx, uni);

        /* buffers */
        JSValue bufs_val = JS_GetPropertyStr(ctx, stage_val, "buffers");
        if (JS_IsArray(ctx, bufs_val)) {
            JSValue blen_val = JS_GetPropertyStr(ctx, bufs_val, "length");
            int32_t bc = 0;
            JS_ToInt32(ctx, &bc, blen_val);
            JS_FreeValue(ctx, blen_val);
            if (bc > 16) bc = 16;
            if (buf_offset + bc > HL_GPU_MAX_PIPELINE_BUFFERS)
                bc = HL_GPU_MAX_PIPELINE_BUFFERS - buf_offset;

            stages[s].buffers = &all_bufs[buf_offset];
            stages[s].buffer_count = bc;

            for (int32_t b = 0; b < bc; b++) {
                JSValue elem = JS_GetPropertyUint32(ctx, bufs_val, (uint32_t)b);
                if (JS_IsObject(elem)) {
                    JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
                    if (JS_IsString(nv)) {
                        all_bufs[buf_offset + b].name = JS_ToCString(ctx, nv);
                        JS_TRACK_STR(all_bufs[buf_offset + b].name);
                    }
                    JS_FreeValue(ctx, nv);

                    JSValue dv = JS_GetPropertyStr(ctx, elem, "data");
                    if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                        size_t dlen;
                        uint8_t *dab = JS_GetArrayBuffer(ctx, &dlen, dv);
                        if (dab) {
                            all_bufs[buf_offset + b].data = dab;
                            all_bufs[buf_offset + b].size = dlen;
                        } else {
                            const char *ds = JS_ToCStringLen(ctx, &dlen, dv);
                            if (ds) {
                                all_bufs[buf_offset + b].data = ds;
                                all_bufs[buf_offset + b].size = dlen;
                                JS_TRACK_STR(ds);
                            }
                        }
                    }
                    JS_FreeValue(ctx, dv);

                    JSValue szv = JS_GetPropertyStr(ctx, elem, "size");
                    if (!JS_IsUndefined(szv)) {
                        int64_t sz; JS_ToInt64(ctx, &sz, szv);
                        all_bufs[buf_offset + b].size = (size_t)sz;
                    }
                    JS_FreeValue(ctx, szv);

                    all_bufs[buf_offset + b].binding = -1;
                }
                JS_FreeValue(ctx, elem);
            }
            buf_offset += bc;
        }
        JS_FreeValue(ctx, bufs_val);
        JS_FreeValue(ctx, stage_val);
    }

    /* Parse opts (second argument) */
    HlGpuPipelineOutput outputs[HL_GPU_MAX_PIPELINE_OUTPUTS];
    int output_count = 0;
    int device = -1;
    uint32_t pipe_timeout_ms = 0;

    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue dv = JS_GetPropertyStr(ctx, argv[1], "device");
        if (!JS_IsUndefined(dv)) { int32_t d; JS_ToInt32(ctx, &d, dv); device = d; }
        JS_FreeValue(ctx, dv);

        /* output: false → fire-and-forget pipeline */
        JSValue out_flag = JS_GetPropertyStr(ctx, argv[1], "output");
        if (JS_IsBool(out_flag) && !JS_ToBool(ctx, out_flag))
            output_count = HL_GPU_OUTPUT_NONE;
        JS_FreeValue(ctx, out_flag);

        /* outputs: [{stage: 0, buffer: 0}, ...] (0-indexed) */
        JSValue ov = JS_GetPropertyStr(ctx, argv[1], "outputs");
        if (output_count != HL_GPU_OUTPUT_NONE && JS_IsArray(ctx, ov)) {
            JSValue olen = JS_GetPropertyStr(ctx, ov, "length");
            JS_ToInt32(ctx, &output_count, olen);
            JS_FreeValue(ctx, olen);
            if (output_count > HL_GPU_MAX_PIPELINE_OUTPUTS)
                output_count = HL_GPU_MAX_PIPELINE_OUTPUTS;
            for (int i = 0; i < output_count; i++) {
                JSValue oe = JS_GetPropertyUint32(ctx, ov, (uint32_t)i);
                outputs[i].stage = 0; outputs[i].buffer = 0;
                if (JS_IsObject(oe)) {
                    JSValue sv3 = JS_GetPropertyStr(ctx, oe, "stage");
                    JSValue bv = JS_GetPropertyStr(ctx, oe, "buffer");
                    int32_t si = 0, bi = 0;
                    JS_ToInt32(ctx, &si, sv3); JS_ToInt32(ctx, &bi, bv);
                    outputs[i].stage = si; outputs[i].buffer = bi;
                    JS_FreeValue(ctx, sv3); JS_FreeValue(ctx, bv);
                }
                JS_FreeValue(ctx, oe);
            }
        }
        JS_FreeValue(ctx, ov);

        JSValue tv = JS_GetPropertyStr(ctx, argv[1], "timeout");
        if (!JS_IsUndefined(tv)) {
            uint32_t t = 0;
            JS_ToUint32(ctx, &t, tv);
            pipe_timeout_ms = t;
        }
        JS_FreeValue(ctx, tv);
    }

    HlGpuPipelineOpts opts = {
        .stages = stages,
        .stage_count = stage_count,
        .outputs = output_count > 0 ? outputs : NULL,
        .output_count = output_count,
        .device = device,
        .timeout_ms = pipe_timeout_ms,
    };

    HlGpuPipelineResult result;
    const char *err_msg = NULL;
    int rc = hl_cap_gpu_pipeline(gpu, &opts, &result, &err_msg);

    /* Free tracked JS strings */
    for (int i = 0; i < js_string_count; i++)
        JS_FreeCString(ctx, js_strings[i]);
    #undef JS_TRACK_STR

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.pipeline: %s",
                                     err_msg ? err_msg : "failed");

    /* Fire-and-forget: return true */
    if (result.count == 0)
        return JS_TRUE;

    /* Return single ArrayBuffer or array of ArrayBuffers */
    if (result.count == 1) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)result.data[0],
                                            result.len[0]);
        hl_cap_gpu_pipeline_result_free(&result);
        return ab;
    }

    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < result.count; i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
            JS_NewArrayBufferCopy(ctx, (const uint8_t *)result.data[i],
                                   result.len[i]));
    }
    hl_cap_gpu_pipeline_result_free(&result);
    return arr;
}

/* Async pipeline — falls back to sync for now */
/* JS async pipeline — deep-copy all stage data, submit to thread pool */
static JSValue js_gpu_async_pipeline(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx, "gpu.async not available (no thread pool)");
    if (!js->base.async_ctx)
        return JS_ThrowInternalError(ctx, "gpu.async requires an active event loop");
    if (!js->base.gpu_ctx)
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: GPU not initialized");
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "gpu.async.pipeline requires stages array");

    /* Parse stage count */
    JSValue len_val = JS_GetPropertyStr(ctx, argv[0], "length");
    int32_t stage_count = 0;
    JS_ToInt32(ctx, &stage_count, len_val);
    JS_FreeValue(ctx, len_val);
    if (stage_count <= 0 || stage_count > HL_GPU_MAX_PIPELINE_STAGES)
        return JS_ThrowRangeError(ctx, "gpu.async.pipeline: invalid stage count");

    /* Allocate worker op */
    HlWorkerGpuOp *op = calloc(1, sizeof(HlWorkerGpuOp));
    if (!op) return JS_ThrowInternalError(ctx, "gpu.async.pipeline: out of memory");

    op->server = js->server;
    op->gpu_ctx = js->base.gpu_ctx;
    op->is_pipeline = 1;
    op->stage_count = stage_count;

    op->stages = calloc((size_t)stage_count, sizeof(HlGpuPipelineStage));
    op->stage_uniforms = calloc((size_t)stage_count, sizeof(void *));
    if (!op->stages || !op->stage_uniforms) {
        hl_worker_gpu_op_free(op); free(op);
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: out of memory");
    }

    /* Count total buffers across all stages for flat allocation */
    int total_bufs = 0;
    for (int32_t s = 0; s < stage_count; s++) {
        JSValue sv = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)s);
        if (JS_IsObject(sv)) {
            JSValue bv = JS_GetPropertyStr(ctx, sv, "buffers");
            if (JS_IsArray(ctx, bv)) {
                JSValue bl = JS_GetPropertyStr(ctx, bv, "length");
                int32_t bc = 0; JS_ToInt32(ctx, &bc, bl);
                JS_FreeValue(ctx, bl);
                if (bc > 16) bc = 16;
                total_bufs += bc;
            }
            JS_FreeValue(ctx, bv);
        }
        JS_FreeValue(ctx, sv);
    }

    if (total_bufs > 0) {
        op->buffers = calloc((size_t)total_bufs, sizeof(HlGpuBufferDesc));
        op->buffer_data = calloc((size_t)total_bufs, sizeof(void *));
        if (!op->buffers || !op->buffer_data) {
            hl_worker_gpu_op_free(op); free(op);
            return JS_ThrowInternalError(ctx, "gpu.async.pipeline: out of memory");
        }
        op->buffer_count = total_bufs;
    }

    /* Deep-copy each stage.
     *
     * M-3: any OOM during deep-copy must abort the dispatch — previously
     * NULL was silently stored in op->stages[s].shader and op->buffer_data[]
     * and the worker thread would crash. We set `oom` and continue parsing
     * (to keep the JS_FreeValue/JS_FreeCString pairs balanced), then bail
     * before submitting to the thread pool. */
    int oom = 0;
    int buf_off = 0;
    for (int32_t s = 0; s < stage_count; s++) {
        JSValue stage_val = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)s);
        if (!JS_IsObject(stage_val)) { JS_FreeValue(ctx, stage_val); continue; }

        /* Shader name (deep-copy) */
        JSValue sv2 = JS_GetPropertyStr(ctx, stage_val, "shader");
        const char *sname = JS_ToCString(ctx, sv2);
        if (sname) {
            op->stages[s].shader = strdup(sname);
            if (!op->stages[s].shader) oom = 1;
            JS_FreeCString(ctx, sname);
        }
        JS_FreeValue(ctx, sv2);

        /* Workgroups */
        JSValue wg = JS_GetPropertyStr(ctx, stage_val, "workgroups");
        if (JS_IsObject(wg)) {
            JSValue xv = JS_GetPropertyStr(ctx, wg, "x");
            JSValue yv = JS_GetPropertyStr(ctx, wg, "y");
            JSValue zv = JS_GetPropertyStr(ctx, wg, "z");
            uint32_t x = 1, y = 1, z = 1;
            JS_ToUint32(ctx, &x, xv); JS_ToUint32(ctx, &y, yv); JS_ToUint32(ctx, &z, zv);
            op->stages[s].workgroups = (HlGpuWorkgroups){ x, y, z };
            JS_FreeValue(ctx, xv); JS_FreeValue(ctx, yv); JS_FreeValue(ctx, zv);
        }
        JS_FreeValue(ctx, wg);

        /* Per-stage uniforms (deep-copy) */
        JSValue uni = JS_GetPropertyStr(ctx, stage_val, "uniforms");
        if (!JS_IsUndefined(uni) && !JS_IsNull(uni)) {
            size_t ulen;
            uint8_t *uab = JS_GetArrayBuffer(ctx, &ulen, uni);
            if (uab && ulen > 0) {
                op->stage_uniforms[s] = malloc(ulen);
                if (op->stage_uniforms[s]) {
                    memcpy(op->stage_uniforms[s], uab, ulen);
                    op->stages[s].uniforms_len = ulen;
                } else {
                    oom = 1;
                }
            } else {
                const char *us = JS_ToCStringLen(ctx, &ulen, uni);
                if (us && ulen > 0) {
                    op->stage_uniforms[s] = malloc(ulen);
                    if (op->stage_uniforms[s]) {
                        memcpy(op->stage_uniforms[s], us, ulen);
                        op->stages[s].uniforms_len = ulen;
                    } else {
                        oom = 1;
                    }
                }
                if (us) JS_FreeCString(ctx, us);
            }
        }
        JS_FreeValue(ctx, uni);

        /* Buffers (deep-copy) */
        JSValue bufs_val = JS_GetPropertyStr(ctx, stage_val, "buffers");
        if (JS_IsArray(ctx, bufs_val)) {
            JSValue blen = JS_GetPropertyStr(ctx, bufs_val, "length");
            int32_t bc = 0; JS_ToInt32(ctx, &bc, blen);
            JS_FreeValue(ctx, blen);
            if (bc > 16) bc = 16;
            op->stages[s].buffer_count = bc;

            for (int32_t b = 0; b < bc && buf_off < total_bufs; b++) {
                JSValue elem = JS_GetPropertyUint32(ctx, bufs_val, (uint32_t)b);
                if (JS_IsObject(elem)) {
                    /* Name (deep-copy) */
                    JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
                    if (JS_IsString(nv)) {
                        const char *ns = JS_ToCString(ctx, nv);
                        if (ns) {
                            op->buffers[buf_off].name = strdup(ns);
                            if (!op->buffers[buf_off].name) oom = 1;
                            JS_FreeCString(ctx, ns);
                        }
                    }
                    JS_FreeValue(ctx, nv);

                    /* Data (deep-copy) */
                    JSValue dv = JS_GetPropertyStr(ctx, elem, "data");
                    if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                        size_t dlen;
                        uint8_t *dab = JS_GetArrayBuffer(ctx, &dlen, dv);
                        if (dab && dlen > 0) {
                            op->buffer_data[buf_off] = malloc(dlen);
                            if (op->buffer_data[buf_off]) {
                                memcpy(op->buffer_data[buf_off], dab, dlen);
                                op->buffers[buf_off].data = op->buffer_data[buf_off];
                                op->buffers[buf_off].size = dlen;
                            } else {
                                oom = 1;
                            }
                        } else {
                            const char *ds = JS_ToCStringLen(ctx, &dlen, dv);
                            if (ds) {
                                if (dlen > 0) {
                                    op->buffer_data[buf_off] = malloc(dlen);
                                    if (op->buffer_data[buf_off]) {
                                        memcpy(op->buffer_data[buf_off], ds, dlen);
                                        op->buffers[buf_off].data = op->buffer_data[buf_off];
                                        op->buffers[buf_off].size = dlen;
                                    } else {
                                        oom = 1;
                                    }
                                }
                                JS_FreeCString(ctx, ds);
                            }
                        }
                    }
                    JS_FreeValue(ctx, dv);

                    /* Size override */
                    JSValue szv = JS_GetPropertyStr(ctx, elem, "size");
                    if (!JS_IsUndefined(szv)) {
                        int64_t sz; JS_ToInt64(ctx, &sz, szv);
                        op->buffers[buf_off].size = (size_t)sz;
                    }
                    JS_FreeValue(ctx, szv);
                    op->buffers[buf_off].binding = -1;
                }
                JS_FreeValue(ctx, elem);
                buf_off++;
            }
        }
        JS_FreeValue(ctx, bufs_val);
        JS_FreeValue(ctx, stage_val);
    }

    /* Parse opts (second argument) */
    op->pipe_opts.device = -1;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue dv = JS_GetPropertyStr(ctx, argv[1], "device");
        if (!JS_IsUndefined(dv)) { int32_t d; JS_ToInt32(ctx, &d, dv); op->pipe_opts.device = d; }
        JS_FreeValue(ctx, dv);

        /* output: false → fire-and-forget */
        JSValue of = JS_GetPropertyStr(ctx, argv[1], "output");
        if (JS_IsBool(of) && !JS_ToBool(ctx, of))
            op->pipe_opts.output_count = HL_GPU_OUTPUT_NONE;
        JS_FreeValue(ctx, of);

        /* outputs array */
        JSValue ov = JS_GetPropertyStr(ctx, argv[1], "outputs");
        if (op->pipe_opts.output_count != HL_GPU_OUTPUT_NONE && JS_IsArray(ctx, ov)) {
            JSValue olen = JS_GetPropertyStr(ctx, ov, "length");
            int32_t oc = 0; JS_ToInt32(ctx, &oc, olen);
            JS_FreeValue(ctx, olen);
            if (oc > HL_GPU_MAX_PIPELINE_OUTPUTS) oc = HL_GPU_MAX_PIPELINE_OUTPUTS;
            if (oc > 0) {
                op->pipe_outputs = calloc((size_t)oc, sizeof(HlGpuPipelineOutput));
                if (op->pipe_outputs) {
                    op->pipe_opts.output_count = oc;
                    op->pipe_opts.outputs = op->pipe_outputs;
                    for (int i = 0; i < oc; i++) {
                        JSValue oe = JS_GetPropertyUint32(ctx, ov, (uint32_t)i);
                        if (JS_IsObject(oe)) {
                            JSValue sv3 = JS_GetPropertyStr(ctx, oe, "stage");
                            JSValue bv2 = JS_GetPropertyStr(ctx, oe, "buffer");
                            int32_t si = 0, bi = 0;
                            JS_ToInt32(ctx, &si, sv3); JS_ToInt32(ctx, &bi, bv2);
                            op->pipe_outputs[i].stage = si;
                            op->pipe_outputs[i].buffer = bi;
                            JS_FreeValue(ctx, sv3); JS_FreeValue(ctx, bv2);
                        }
                        JS_FreeValue(ctx, oe);
                    }
                }
            }
        }
        JS_FreeValue(ctx, ov);

        JSValue tv = JS_GetPropertyStr(ctx, argv[1], "timeout");
        if (!JS_IsUndefined(tv)) {
            uint32_t t = 0;
            JS_ToUint32(ctx, &t, tv);
            op->pipe_opts.timeout_ms = t;
        }
        JS_FreeValue(ctx, tv);
    }

    /* Abort if any deep-copy hit OOM (M-3) — must happen before the op is
     * handed off to the worker thread, otherwise the worker dereferences
     * NULL shader names / buffer data. */
    if (oom) {
        hl_worker_gpu_op_free(op); free(op);
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: out of memory");
    }

    /* Create async ctx + Promise + continuation */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx, js->base.alloc);
    if (!actx) {
        hl_worker_gpu_op_free(op); free(op);
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: out of memory");
    }

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_worker_gpu_result);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_gpu_op_free_all;
    actx->op.on_cancel = hl_worker_gpu_async_cancel;
    actx->detached = (js->active_conn == NULL);

    op->async_ctx = actx;
    atomic_store(&op->cancelled, 0);

    if (hl_worker_gpu_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: thread pool full");
    }

    if (!actx->detached &&
        hl_net_op_suspend(js->base.net_ctx, (HlReqHandle *)js->active_conn, (HlSuspendOp *)&actx->op) < 0) {
        atomic_store(&op->cancelled, 1);
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: failed to suspend");
    }

    return promise;
}

static int js_gpu_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/gpu", "hull:gpu") != 0) return -1;

    JSValue gpu = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, gpu, "available",
                      JS_NewCFunction(ctx, js_gpu_available, "available", 0));
    JS_SetPropertyStr(ctx, gpu, "devices",
                      JS_NewCFunction(ctx, js_gpu_devices, "devices", 0));
    JS_SetPropertyStr(ctx, gpu, "compile",
                      JS_NewCFunction(ctx, js_gpu_compile, "compile", 2));
    JS_SetPropertyStr(ctx, gpu, "load",
                      JS_NewCFunction(ctx, js_gpu_load, "load", 1));
    JS_SetPropertyStr(ctx, gpu, "dispatch",
                      JS_NewCFunction(ctx, js_gpu_dispatch, "dispatch", 2));
    JS_SetPropertyStr(ctx, gpu, "pipeline",
                      JS_NewCFunction(ctx, js_gpu_pipeline, "pipeline", 2));
    JS_SetPropertyStr(ctx, gpu, "buffer",
                      JS_NewCFunction(ctx, js_gpu_buffer, "buffer", 3));
    JS_SetPropertyStr(ctx, gpu, "bufferRead",
                      JS_NewCFunction(ctx, js_gpu_buffer_read, "bufferRead", 1));
    JS_SetPropertyStr(ctx, gpu, "bufferCopy",
                      JS_NewCFunction(ctx, js_gpu_buffer_copy, "bufferCopy", 3));
    JS_SetPropertyStr(ctx, gpu, "texture",
                      JS_NewCFunction(ctx, js_gpu_texture, "texture", 3));
    JS_SetPropertyStr(ctx, gpu, "textureRead",
                      JS_NewCFunction(ctx, js_gpu_texture_read, "textureRead", 1));

    /* gpu.async sub-object */
    JSValue async_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, async_obj, "dispatch",
                      JS_NewCFunction(ctx, js_gpu_async_dispatch, "dispatch", 2));
    JS_SetPropertyStr(ctx, async_obj, "pipeline",
                      JS_NewCFunction(ctx, js_gpu_async_pipeline, "pipeline", 2));
    JS_SetPropertyStr(ctx, gpu, "async", async_obj);

    JS_SetModuleExport(ctx, m, "gpu", gpu);
    return 0;
}

int hl_js_init_gpu_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:gpu", js_gpu_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "gpu");
    return 0;
}
#else /* !HL_ENABLE_GPU */
/* ISO C requires a translation unit to contain at least one declaration. */
typedef int hl_js_mod_gpu_disabled;
#endif /* HL_ENABLE_GPU */
