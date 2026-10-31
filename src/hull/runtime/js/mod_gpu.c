/*
 * mod_gpu.c - hull:gpu module (GPU compute via wgpu-native)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */


#include "refusal.h"
#include "hull/cap/fs_resolve.h"  /* hl_fs_fopen_read */
#include "db_wait.h"   /* hl_js_db_refuse_wait */
#include "mod_buffer.h"
#include "internal.h"            /* async gate, instruction budget */
#include "hull/cap/gpu.h"
#include "hull/cap/image.h"
#include "hull/worker_gpu.h"
#include "hull/shared/async.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"
#include "hull/shared/work_charge.h"   /* hl_work_bulk_units */
#include "hull/vfs.h"

#include <keel/http_server.h>
#include <stdio.h>
#include <stdatomic.h>

/* opts.timeout (ms) as a dispatch's timeout_ms, as Lua's gpu_timeout_arg reads
 * it: 0 (the backend default) for a non-positive value, at most
 * HL_GPU_TIMEOUT_MAX_MS. Read as uint32, -1 was 2^32-1 ms, ~49.7 days of
 * waiting on the event loop (audit 33; the cap layer clamps too). Returns as
 * hl_js_opt_int64. */
static int js_gpu_timeout_opt(JSContext *ctx, JSValueConst obj, uint32_t *out)
{
    int64_t t = 0;
    int r = hl_js_opt_int64(ctx, obj, "timeout", &t);
    if (r == 1)
        *out = t <= 0 ? 0 : t > HL_GPU_TIMEOUT_MAX_MS ? HL_GPU_TIMEOUT_MAX_MS
                                                      : (uint32_t)t;
    return r;
}

/* The largest WGSL source gpu.load reads or gpu.compile takes (audit 36:
 * 1 MiB, from 10). */
#define JS_GPU_WGSL_MAX HL_GPU_WGSL_MAX_BYTES

/* ── Two-pass ArrayBuffer views (audit 9 H1) ──────────────────────
 *
 * A synchronous call parses its options with getters, valueOf and toString
 * - app code - and an ArrayBuffer whose bytes were taken early in the parse
 * can be transfer()ed or resize()d by a later one, which frees or moves the
 * backing store: the dispatch then read freed memory. Holding the JS object
 * does not prevent that. So the parse only RECORDS each ArrayBuffer (held
 * by the caller's keep array) and where its pointer goes; once the last
 * piece of app code has run, js_gpu_retake_views takes every pointer again.
 * A buffer detached or resized in between fails the call. (The async calls
 * copy every byte as they parse, so they need none of this.) */
typedef struct {
    JSValueConst  ab;     /* the ArrayBuffer (a reference the caller holds) */
    const void  **slot;   /* where its bytes go */
    size_t        len;    /* its length when the parse saw it */
} HlJsGpuView;

static void js_gpu_note_view(HlJsGpuView *views, int *n, int max,
                             JSValueConst ab, const void **slot, size_t len)
{
    if (*n < max) {
        views[*n].ab = ab;
        views[*n].slot = slot;
        views[*n].len = len;
        (*n)++;
    }
}

/* Re-take every recorded view. No app code may run between this and the
 * cap call that reads them. -1 with a TypeError pending on a buffer that
 * was detached or resized. */
static int js_gpu_retake_views(JSContext *ctx, const HlJsGpuView *views,
                               int n, const char *what)
{
    for (int i = 0; i < n; i++) {
        HlBufferView bv = {0};
        const char *s = NULL;
        int nf = 0;
        /* Every recorded value is an object, so js_get_buffer runs no
         * toString here (audit 29: typed arrays, WasmBuffer, MappedBuffer). */
        if (!js_get_buffer(ctx, views[i].ab, &bv, &s, &nf) || nf ||
            bv.len != views[i].len) {
            if (nf) JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "%s: a buffer was detached, resized or "
                                   "closed while the arguments were read", what);
            return -1;
        }
        *views[i].slot = bv.data;
    }
    return 0;
}

/* The bytes of a buffer / uniforms / texture `data` value (audit 29). An
 * object must be a byte object - ArrayBuffer, typed array, WasmBuffer,
 * MappedBuffer or image - (1, *p / *len its bytes, borrowed: a sync caller
 * records it as a view, an async one copies at once); any other object is a
 * TypeError (-1): a Float32Array uploaded its toString "1,2,3" and a
 * WasmBuffer "[object WasmBuffer]". A primitive is 0: the caller converts it
 * as a string (no app code runs for a primitive). */
static int js_gpu_bytes_of(JSContext *ctx, JSValueConst v, const char *what,
                           const void **p, size_t *len)
{
    *p = NULL;
    *len = 0;
    if (!JS_IsObject(v)) return 0;
    HlBufferView bv = {0};
    const char *s = NULL;
    int nf = 0;
    if (js_get_buffer(ctx, v, &bv, &s, &nf) && !nf) {
        *p = bv.data;
        *len = bv.len;
        return 1;
    }
    if (nf) JS_FreeCString(ctx, s);
    JS_ThrowTypeError(ctx, "%s: data must be a string, ArrayBuffer, typed "
                           "array, WasmBuffer or MappedBuffer", what);
    return -1;
}

/* Forward declaration */
static int js_parse_texture_descs(JSContext *ctx, JSValueConst arr,
                                   HlGpuTextureDesc *descs, int max_descs,
                                   const char **tracked_strs, int *str_count,
                                   int str_max, JSValue *keep,
                                   HlJsGpuView *views, int *nviews, int max_views);

static HlGpuCtx *js_get_gpu_ctx(JSContext *ctx)
{
    /* HlJS is stashed as the context opaque (runtime.c: JS_SetContextOpaque),
     * the same idiom every other JS cap module uses. The old code read a
     * `__hull_js` global that is never set, so this always returned NULL and
     * every gpu.* call failed (compile surfaced as a misleading "shader_error"). */
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    return js ? js->base.gpu_ctx : NULL;
}

/* The VM heap limit (0 = none): a read-back is refused past it before the
 * backend copies it off the heap (audit 17). */
static size_t js_gpu_heap_limit(JSContext *ctx)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    return js ? js->max_heap_bytes : 0;
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
    /* Checked builds (audit 29): a value that cannot be made fails the call. */
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) return arr;
    for (int i = 0; i < count; i++) {
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) goto fail;
        const char *dn = hl_cap_gpu_device_name(gpu, i);
        JSValue nm = JS_NewString(ctx, dn ? dn : "");
        if (JS_IsException(nm)) {
            JS_FreeValue(ctx, obj);
            goto fail;
        }
        if (JS_DefinePropertyValueStr(ctx, obj, "id", JS_NewInt32(ctx, i),
                                      JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, nm);
            JS_FreeValue(ctx, obj);
            goto fail;
        }
        if (JS_DefinePropertyValueStr(ctx, obj, "name", nm,
                                      JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, obj);
            goto fail;
        }
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, obj,
                                         JS_PROP_C_W_E) < 0)
            goto fail;
    }
    return arr;
fail:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

/* Load WGSL shader from shaders/<name>.wgsl (VFS or disk), compiled on the
 * device the opts name, as gpu.compile is (default device only before). */
static JSValue js_gpu_load(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "gpu.load requires name");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu) return JS_ThrowInternalError(ctx, "gpu not available");

    /* Read first: a getter is app code, run before anything is held. */
    int device = -1;
    if (argc > 1 && JS_IsObject(argv[1])) {
        int32_t d;
        int r = hl_js_opt_int32(ctx, argv[1], "device", &d);
        if (r < 0) return JS_EXCEPTION;
        if (r) device = d;
    }

    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    /* Validate shader name: reject path traversal */
    if (!name[0] || name[0] == '/' || strstr(name, "..") || strchr(name, '\\')) {
        JS_FreeCString(ctx, name);
        return JS_ThrowRangeError(ctx, "invalid shader name");
    }

    char vfs_name[512];
    int vn = snprintf(vfs_name, sizeof(vfs_name), "shaders/%s.wgsl", name);
    /* A cut name would open a different file: no ".wgsl", a prefix of it. */
    if (vn < 0 || (size_t)vn >= sizeof(vfs_name)) {
        JS_FreeCString(ctx, name);
        return JS_ThrowRangeError(ctx, "shader name too long");
    }
    const HlEntry *entry = NULL;
    if (js && js->base.app_vfs)
        entry = hl_vfs_find(js->base.app_vfs, vfs_name);

    const char *wgsl = NULL;
    size_t wgsl_len = 0;
    char *file_buf = NULL;
    int too_large = 0;

    if (entry && entry->data) {
        wgsl = (const char *)entry->data;
        wgsl_len = entry->len;
    } else if (js && js->base.app_vfs && js->base.app_vfs->root_dir &&
               !hl_vfs_embedded(js->base.app_vfs)) {
        /* Development only: a built binary (app files embedded) runs the
         * shaders it was built with, not whatever lies under its cwd. */
        char path[4096];
        int pn = snprintf(path, sizeof(path), "%s/shaders/%s.wgsl",
                 js->base.app_vfs->root_dir, name);
        if (pn < 0 || (size_t)pn >= sizeof(path)) {
            JS_FreeCString(ctx, name);
            return JS_ThrowInternalError(ctx, "gpu.load: shader path too long");
        }
        /* Opened under the app root: the name check above is lexical, and
         * a symlink below shaders/ used to be followed wherever it led. */
        FILE *f = hl_fs_fopen_read(js->base.app_vfs->root_dir, vfs_name);
        if (f) {
            long flen = -1;
            if (fseek(f, 0, SEEK_END) == 0) {
                flen = ftell(f);
                if (fseek(f, 0, SEEK_SET) != 0) flen = -1;
            }
            if (flen > 0 && (unsigned long)flen > JS_GPU_WGSL_MAX)
                too_large = 1;
            if (flen > 0 && (unsigned long)flen <= JS_GPU_WGSL_MAX) {
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
                JSValue err = JS_ThrowInternalError(
                    ctx, "gpu.load: shader '%s' is not a signed file (--verify-sig)", name);
                JS_FreeCString(ctx, name);
                return err;
            }
        }
    }

    if (too_large || (wgsl && wgsl_len > JS_GPU_WGSL_MAX)) {
        free(file_buf);
        JSValue err = JS_ThrowRangeError(ctx, "gpu.load: shader '%s' is too large (at most %d bytes)",
                                         name, (int)JS_GPU_WGSL_MAX);
        JS_FreeCString(ctx, name);
        return err;
    }
    if (!wgsl) {
        free(file_buf);
        JSValue err = JS_ThrowInternalError(ctx, "gpu.load: shader '%s' not found in shaders/", name);
        JS_FreeCString(ctx, name);
        return err;
    }

    /* The read and the compile are charged before the compile: WGSL
     * compilation was free (audit 24); its expansion is charged too
     * (audit 39). */
    if (hl_js_budget_charge(ctx, hl_gpu_wgsl_compile_units(wgsl, wgsl_len)) < 0) {
        free(file_buf);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    int rc = hl_cap_gpu_compile_ex(gpu, device, name, wgsl, wgsl_len,
                                   HL_GPU_COMPILE_FROM_SHADERS);
    free(file_buf);
    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.load: %s", hl_gpu_compile_errstr(rc));

    return JS_TRUE;
}

static JSValue js_gpu_compile(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "gpu.compile requires name and wgsl");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    /* The device opts name, as Lua's gpu.compile reads it: compiled on the
     * default device only, a shader could not be dispatched on another. */
    int device = -1;
    if (argc > 2 && JS_IsObject(argv[2])) {
        int32_t d;
        int r = hl_js_opt_int32(ctx, argv[2], "device", &d);
        if (r < 0) return JS_EXCEPTION;
        if (r) device = d;
    }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    size_t wgsl_len;
    const char *wgsl = JS_ToCStringLen(ctx, &wgsl_len, argv[1]);
    if (!wgsl) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }

    /* Capped as gpu.load's file read is, and charged before the compile:
     * WGSL of any length compiled free of the budget (audit 24); its
     * expansion is charged too (audit 39). */
    if (wgsl_len > JS_GPU_WGSL_MAX) {
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, wgsl);
        return JS_ThrowRangeError(ctx, "gpu.compile: WGSL source too large");
    }
    if (hl_js_budget_charge(ctx, hl_gpu_wgsl_compile_units(wgsl, wgsl_len)) < 0) {
        JS_FreeCString(ctx, name);
        JS_FreeCString(ctx, wgsl);
        return JS_EXCEPTION;
    }
    int rc = hl_cap_gpu_compile(gpu, device, name, wgsl, wgsl_len);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, wgsl);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.compile failed: %s", hl_gpu_compile_errstr(rc));

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
    opts.max_output = js_gpu_heap_limit(ctx);   /* audit 17 */

    JSValue opts_val = argv[1];

    /* A getter, valueOf or conversion that throws (or the budget interrupt)
     * fails the call with its exception: perr skips the parse that is left
     * and the dispatch (audit 23 - the results were ignored, so the dispatch
     * ran on defaults with the exception pending). Everything the cleanup
     * below releases is initialised here, before the first getter. */
    int perr = 0;
    HlJsGpuView views[1 + 16 + 8];
    int nviews = 0;
    const char *uni_str = NULL;  /* non-NULL if JS_ToCStringLen was used */
    JSValue uni_val = JS_UNDEFINED;
    HlGpuBufferDesc bufs[16];
    memset(bufs, 0, sizeof(bufs));
    int buf_count = 0;
    /* Keep JS string refs alive until after dispatch */
    const char *buf_name_strs[16] = {0};
    const char *buf_data_strs[16] = {0};
    /* ...and the ArrayBuffers bufs[].data points into. `data` may be a getter
     * that returns a fresh one, freed the moment its value is released - the
     * dispatch then read freed memory. */
    JSValue buf_data_vals[16];
    for (int i = 0; i < 16; i++) buf_data_vals[i] = JS_UNDEFINED;
    HlGpuTextureDesc tex_descs[8];
    const char *tex_strs[32] = {0};
    int tex_str_count = 0;
    JSValue tex_keep[8];
    for (int i = 0; i < 8; i++) tex_keep[i] = JS_UNDEFINED;

    /* Parse device */
    int32_t d32;
    int r = hl_js_opt_int32(ctx, opts_val, "device", &d32);
    if (r < 0) { perr = 1; goto parsed; }
    if (r) opts.device = d32;

    /* Parse workgroups */
    JSValue wg_val = JS_GetPropertyStr(ctx, opts_val, "workgroups");
    if (JS_IsException(wg_val)) { perr = 1; goto parsed; }
    if (JS_IsObject(wg_val)) {
        uint32_t x = 1, y = 1, z = 1;
        if (hl_js_get_uint32(ctx, wg_val, "x", &x) < 0 ||
            hl_js_get_uint32(ctx, wg_val, "y", &y) < 0 ||
            hl_js_get_uint32(ctx, wg_val, "z", &z) < 0) {
            JS_FreeValue(ctx, wg_val);
            perr = 1;
            goto parsed;
        }
        opts.workgroups.x = x;
        opts.workgroups.y = y;
        opts.workgroups.z = z;
    }
    JS_FreeValue(ctx, wg_val);

    /* Parse output buffer index (0-indexed in JS).
     * output = N     → read back buffer N
     * output = false → fire-and-forget (skip readback)
     * output absent  → default to 0 (backward compat) */
    JSValue out_val = JS_GetPropertyStr(ctx, opts_val, "output");
    if (JS_IsException(out_val)) { perr = 1; goto parsed; }
    if (JS_IsBool(out_val) && !JS_ToBool(ctx, out_val)) {
        opts.output_buffer = HL_GPU_OUTPUT_NONE;
    } else if (!JS_IsUndefined(out_val)) {
        int32_t o;
        if (JS_ToInt32(ctx, &o, out_val) < 0) {
            JS_FreeValue(ctx, out_val);
            perr = 1;
            goto parsed;
        }
        opts.output_buffer = o;
    }
    JS_FreeValue(ctx, out_val);

    /* Parse timeout */
    uint32_t t32;
    r = js_gpu_timeout_opt(ctx, opts_val, &t32);
    if (r < 0) { perr = 1; goto parsed; }
    if (r) opts.timeout_ms = t32;

    /* Every ArrayBuffer pointer is re-taken after the last getter (see
     * HlJsGpuView): uniforms + 16 buffers + 8 textures. */

    /* Parse uniforms (string or ArrayBuffer) */
    uni_val = JS_GetPropertyStr(ctx, opts_val, "uniforms");
    if (JS_IsException(uni_val)) {
        uni_val = JS_UNDEFINED;
        perr = 1;
        goto parsed;
    }
    if (!JS_IsUndefined(uni_val) && !JS_IsNull(uni_val)) {
        size_t uni_len;
        const void *uni_ab;
        int bk = js_gpu_bytes_of(ctx, uni_val, "gpu.dispatch", &uni_ab, &uni_len);
        if (bk < 0) { perr = 1; goto parsed; }
        if (bk) {
            opts.uniforms = uni_ab;
            opts.uniforms_len = uni_len;
            js_gpu_note_view(views, &nviews, 1 + 16 + 8, uni_val,
                             &opts.uniforms, uni_len);
        } else {
            uni_str = JS_ToCStringLen(ctx, &uni_len, uni_val);
            if (!uni_str) { perr = 1; goto parsed; }
            opts.uniforms = uni_str;
            opts.uniforms_len = uni_len;
        }
    }
    /* uni_val is released after the dispatch: opts.uniforms may point into
     * its ArrayBuffer. */

    /* Parse buffers array */
    JSValue bufs_val = JS_GetPropertyStr(ctx, opts_val, "buffers");
    if (JS_IsException(bufs_val)) { perr = 1; goto parsed; }
    if (JS_IsArray(ctx, bufs_val)) {
        int32_t len = 0;
        if (hl_js_get_length32(ctx, bufs_val, &len) < 0) perr = 1;
        if (len > 16) len = 16;

        for (int32_t i = 0; i < len && !perr; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, bufs_val, (uint32_t)i);
            if (JS_IsException(elem)) { perr = 1; break; }
            if (JS_IsObject(elem)) {
                /* An entry's strings and buffer are released by index up
                 * to buf_count, so count it before it can fail. */
                int bi = buf_count++;
                bufs[bi].binding = -1;
                JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
                if (JS_IsException(nv)) perr = 1;
                else if (JS_IsString(nv)) {
                    buf_name_strs[bi] = JS_ToCString(ctx, nv);
                    if (!buf_name_strs[bi]) perr = 1;
                    bufs[bi].name = buf_name_strs[bi];
                }
                JS_FreeValue(ctx, nv);

                JSValue dv = perr ? JS_UNDEFINED
                                  : JS_GetPropertyStr(ctx, elem, "data");
                if (JS_IsException(dv)) perr = 1;
                else if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                    size_t dlen;
                    const void *dab;
                    int bk = js_gpu_bytes_of(ctx, dv, "gpu.dispatch", &dab, &dlen);
                    if (bk < 0) {
                        perr = 1;
                    } else if (bk) {
                        buf_data_vals[bi] = JS_DupValue(ctx, dv);
                        bufs[bi].data = dab;
                        bufs[bi].data_len = dlen;
                        bufs[bi].size = dlen;
                        js_gpu_note_view(views, &nviews, 1 + 16 + 8,
                                         buf_data_vals[bi],
                                         &bufs[bi].data, dlen);
                    } else {
                        buf_data_strs[bi] = JS_ToCStringLen(ctx, &dlen, dv);
                        if (buf_data_strs[bi]) {
                            bufs[bi].data = buf_data_strs[bi];
                            bufs[bi].data_len = dlen;
                            bufs[bi].size = dlen;
                        } else {
                            perr = 1;
                        }
                    }
                }
                JS_FreeValue(ctx, dv);

                int64_t sz;
                int sr = perr ? 0 : hl_js_opt_int64(ctx, elem, "size", &sz);
                if (sr < 0) perr = 1;
                else if (sr) bufs[bi].size = (size_t)sz;

                JSValue uv = perr ? JS_UNDEFINED
                                  : JS_GetPropertyStr(ctx, elem, "usage");
                if (JS_IsException(uv)) perr = 1;
                else if (JS_IsString(uv)) {
                    const char *us = JS_ToCString(ctx, uv);
                    if (us) {
                        if (strcmp(us, "read") == 0)
                            bufs[bi].usage = HL_GPU_USAGE_READ;
                        else if (strcmp(us, "write") == 0)
                            bufs[bi].usage = HL_GPU_USAGE_WRITE;
                        else if (strcmp(us, "readwrite") == 0)
                            bufs[bi].usage = HL_GPU_USAGE_READWRITE;
                        JS_FreeCString(ctx, us);
                    } else {
                        perr = 1;
                    }
                }
                JS_FreeValue(ctx, uv);
            }
            JS_FreeValue(ctx, elem);
        }
    }
    JS_FreeValue(ctx, bufs_val);
    if (perr) goto parsed;

    opts.buffers = bufs;
    opts.buffer_count = buf_count;
    opts.output_texture = -1;

    /* Parse textures */
    JSValue tex_arr = JS_GetPropertyStr(ctx, opts_val, "textures");
    if (JS_IsException(tex_arr)) { perr = 1; goto parsed; }
    int tex_count = js_parse_texture_descs(ctx, tex_arr, tex_descs, 8,
                                            tex_strs, &tex_str_count, 32,
                                            tex_keep, views, &nviews,
                                            1 + 16 + 8);
    JS_FreeValue(ctx, tex_arr);
    if (tex_count < 0) { perr = 1; goto parsed; }
    opts.textures = tex_descs;
    opts.texture_count = tex_count;

    /* Parse output_texture (0-indexed in JS) */
    int32_t ot;
    r = hl_js_opt_int32(ctx, opts_val, "outputTexture", &ot);
    if (r < 0) { perr = 1; goto parsed; }
    if (r) opts.output_texture = ot;

parsed:;
    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    /* Fire-and-forget: pass NULL output pointers */
    void **out_ptr = (opts.output_buffer >= 0 || opts.output_texture >= 0)
                      ? &output : NULL;
    size_t *out_len_ptr = (opts.output_buffer >= 0 || opts.output_texture >= 0)
                           ? &output_len : NULL;

    /* The call's own buffers, uniforms and inline textures are charged
     * before they are made, one unit per 64 bytes, as gpu.buffer's create
     * is (audit 17); a trip leaves its exception pending. Then, the last
     * getter having run, take the ArrayBuffers' bytes. */
    int retake_failed = perr ||
        hl_js_budget_charge(ctx, hl_work_bulk_units(
                                (uint64_t)hl_gpu_dispatch_charge_bytes(&opts))) ||
        js_gpu_retake_views(ctx, views, nviews, "gpu.dispatch") != 0;
    int rc = retake_failed ? HL_GPU_OK
           : hl_cap_gpu_dispatch(gpu, name, &opts,
                                 out_ptr, out_len_ptr, &err_msg);

    /* Free uniform string if we used JS_ToCStringLen */
    if (uni_str) JS_FreeCString(ctx, uni_str);

    JS_FreeValue(ctx, uni_val);

    /* Free buffer name/data strings, and release their ArrayBuffers */
    for (int i = 0; i < buf_count; i++) {
        if (buf_name_strs[i]) JS_FreeCString(ctx, buf_name_strs[i]);
        if (buf_data_strs[i]) JS_FreeCString(ctx, buf_data_strs[i]);
        JS_FreeValue(ctx, buf_data_vals[i]);
    }

    /* Free texture tracked strings, and release their kept ArrayBuffers */
    for (int i = 0; i < tex_str_count; i++)
        JS_FreeCString(ctx, tex_strs[i]);
    for (int i = 0; i < 8; i++)
        JS_FreeValue(ctx, tex_keep[i]);

    JS_FreeCString(ctx, name);

    if (retake_failed)
        return JS_EXCEPTION;
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

    /* null = destroy, on the device the opts name (it went to the default
     * device). A throwing getter fails the call. */
    if (JS_IsNull(argv[1])) {
        if (argc > 2 && JS_IsObject(argv[2])) {
            int32_t d;
            int r = hl_js_opt_int32(ctx, argv[2], "device", &d);
            if (r < 0) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
            if (r) device = d;
        }
        hl_cap_gpu_buffer_destroy(gpu, device, name);
        JS_FreeCString(ctx, name);
        return JS_TRUE;
    }

    size_t offset = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        /* A throwing getter / valueOf fails the call (audit 23). */
        int64_t o; int32_t d; int r1, r2 = 0;
        r1 = hl_js_opt_int64(ctx, argv[2], "offset", &o);
        if (r1 >= 0) r2 = hl_js_opt_int32(ctx, argv[2], "device", &d);
        if (r1 < 0 || r2 < 0) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        if (r1) offset = (size_t)o;
        if (r2) device = d;
    }

    /* The source view after the options: their getters are app code, which
     * could close or free the buffer a view taken first named. */
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

    /* The upload (and a create, sized by it) is charged before it runs, one
     * unit per 64 bytes (audit 17): a MappedBuffer source is off-heap, and
     * the device memory it fills is outside the heap limit. The charge runs
     * no app code, so the view stays good. */
    if (hl_js_budget_charge(ctx, hl_work_bulk_units((uint64_t)data_len))) {
        if (str_data) JS_FreeCString(ctx, str_data);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
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
        return JS_ThrowInternalError(ctx, "gpu.buffer %s", rc == HL_GPU_ERR_BUSY ? "gpu_busy" : rc == HL_GPU_ERR_TIMEOUT ? "gpu_timeout" : "failed");

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

    /* The device opts name (it read the default device's). A throwing
     * getter fails the call. */
    int device = -1;
    if (argc > 1 && JS_IsObject(argv[1])) {
        int32_t d;
        int r = hl_js_opt_int32(ctx, argv[1], "device", &d);
        if (r < 0) return JS_EXCEPTION;
        if (r) device = d;
    }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    void *data = NULL;
    size_t len = 0;

    int rc = hl_cap_gpu_buffer_read(gpu, device, name, js_gpu_heap_limit(ctx),
                                    &data, &len);
    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.bufferRead %s", rc == HL_GPU_ERR_BUSY ? "gpu_busy" : rc == HL_GPU_ERR_TIMEOUT ? "gpu_timeout" : "failed");

    /* As textureRead: 1 unit per 8 bytes before the copy (audit 26). */
    if (hl_js_budget_charge(ctx, hl_work_bytes_units((uint64_t)len)) < 0) {
        free(data);
        return JS_EXCEPTION;
    }

    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)data, len);
    free(data);
    return ab;
}

/* The bytes a gpu.bufferCopy moves: @p size, or the rest of the source past
 * @p src_offset when it is 0, never more than the source holds - a bogus
 * size (-1 read as SIZE_MAX) is refused by the cap as out_of_bounds, a
 * catchable error, and must not trip the budget first (audit 28). 0 when the
 * device or buffer is not there (the copy then fails). Read under the device
 * lock, the way hl_cap_gpu_buffer_copy resolves them; mirrors Lua's
 * lua_gpu_copy_bytes. */
static size_t js_gpu_copy_bytes(HlGpuCtx *gpu, int device, const char *src,
                                size_t src_offset, size_t size)
{
    if (!gpu || !src) return 0;
    int idx = device < 0 ? gpu->default_device : device;
    if (idx < 0 || idx >= gpu->device_count || idx >= HL_GPU_MAX_DEVICES ||
        (gpu->device_restriction && !gpu->allowed_devices[idx]))
        return 0;
    HlGpuDevice *dev = &gpu->devices[idx];
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
        /* A throwing getter / valueOf fails the call: the copy ran on the
         * defaults with the exception pending (audit 23). */
        int64_t x; int32_t d; int r;
        if ((r = hl_js_opt_int64(ctx, argv[2], "srcOffset", &x)) < 0) goto opt_fail;
        if (r) src_offset = (size_t)x;
        if ((r = hl_js_opt_int64(ctx, argv[2], "dstOffset", &x)) < 0) goto opt_fail;
        if (r) dst_offset = (size_t)x;
        if ((r = hl_js_opt_int64(ctx, argv[2], "size", &x)) < 0) goto opt_fail;
        if (r) size = (size_t)x;
        if ((r = hl_js_opt_int32(ctx, argv[2], "device", &d)) < 0) goto opt_fail;
        if (r) device = d;
    }

    /* Charged before it runs, as gpu.buffer is (audit 27: a copy of up to
     * the device's whole live memory was free). */
    uint64_t copy_bytes =
        (uint64_t)js_gpu_copy_bytes(gpu, device, src, src_offset, size);
    if (hl_js_budget_charge(ctx, hl_work_bulk_units(copy_bytes)) < 0)
        goto opt_fail;

    int rc = hl_cap_gpu_buffer_copy(gpu, device, src, dst,
                                     src_offset, dst_offset, size);
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, dst);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.bufferCopy: %s",
                   rc == HL_GPU_ERR_NOT_FOUND ? "buffer_not_found"
                 : rc == HL_GPU_ERR_BUFFER ? "out_of_bounds"
                 : rc == HL_GPU_ERR_BUSY ? "gpu_busy"
                 : rc == HL_GPU_ERR_TIMEOUT ? "gpu_timeout"
                 : "copy_failed");

    return JS_TRUE;

opt_fail:
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, dst);
    return JS_EXCEPTION;
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

    /* null = destroy, on the device the opts name (it went to the default
     * device). A throwing getter fails the call. */
    if (JS_IsNull(argv[1])) {
        if (argc > 2 && JS_IsObject(argv[2])) {
            int32_t d;
            int r = hl_js_opt_int32(ctx, argv[2], "device", &d);
            if (r < 0) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
            if (r) device = d;
        }
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

#ifdef HL_ENABLE_IMAGE
    /* Check if arg 1 is an HlImage object */
    HlImage *img = JS_GetOpaque(argv[1], js_image_class_id);
    if (img) {
        width = img->width;
        height = img->height;
        format = (HlGpuTexFormat)img->format;
    } else
#endif
    {
        /* Validate the source now; its bytes are taken after the options. */
        HlBufferView bv;
        if (!js_get_buffer(ctx, argv[1], &bv, &str_data, &str_needs_free)) {
            JS_FreeCString(ctx, name);
            return JS_ThrowTypeError(ctx, "gpu.texture: data must be "
#ifdef HL_ENABLE_IMAGE
                                     "HlImage, "
#endif
                                     "ArrayBuffer, or string");
        }
    }

    /* Parse opts */
    if (argc > 2 && JS_IsObject(argv[2])) {
        /* A getter or valueOf that throws fails the call (audit 23): its
         * result was ignored and the texture made with the defaults. */
        JSValue v;
        int32_t d; uint32_t u; int r;
        if ((r = hl_js_opt_int32(ctx, argv[2], "device", &d)) < 0) goto opt_fail;
        if (r) device = d;
        if ((r = hl_js_opt_uint32(ctx, argv[2], "width", &u)) < 0) goto opt_fail;
        if (r) width = u;
        if ((r = hl_js_opt_uint32(ctx, argv[2], "height", &u)) < 0) goto opt_fail;
        if (r) height = u;
        v = JS_GetPropertyStr(ctx, argv[2], "format");
        if (JS_IsException(v)) goto opt_fail;
        if (JS_IsString(v)) {
            /* A failed conversion (the budget, out of memory) fails the
             * call: it read as the option being absent (audit 27). */
            const char *f = JS_ToCString(ctx, v);
            if (!f) { JS_FreeValue(ctx, v); goto opt_fail; }
            format = js_parse_tex_format(f);
            JS_FreeCString(ctx, f);
        }
        JS_FreeValue(ctx, v);
        if ((r = hl_js_opt_bool(ctx, argv[2], "storage")) < 0) goto opt_fail;
        if (r) storage = 1;
        v = JS_GetPropertyStr(ctx, argv[2], "filter");
        if (JS_IsException(v)) goto opt_fail;
        if (JS_IsString(v)) {
            const char *f = JS_ToCString(ctx, v);
            if (!f) { JS_FreeValue(ctx, v); goto opt_fail; }
            if (strcmp(f, "linear") == 0) filter = HL_GPU_FILTER_LINEAR;
            JS_FreeCString(ctx, f);
        }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "wrap");
        if (JS_IsException(v)) goto opt_fail;
        if (JS_IsString(v)) {
            const char *w = JS_ToCString(ctx, v);
            if (!w) { JS_FreeValue(ctx, v); goto opt_fail; }
            if (strcmp(w, "repeat") == 0) {
                address_u = HL_GPU_ADDRESS_REPEAT;
                address_v = HL_GPU_ADDRESS_REPEAT;
            } else if (strcmp(w, "mirror") == 0) {
                address_u = HL_GPU_ADDRESS_MIRROR;
                address_v = HL_GPU_ADDRESS_MIRROR;
            }
            JS_FreeCString(ctx, w);
        }
        JS_FreeValue(ctx, v);
    }

    /* Take the source again: the options above are app code (getters,
     * valueOf) and could have closed or freed what was taken first. Their
     * dimensions/format still win over the source's. */
#ifdef HL_ENABLE_IMAGE
    if (img) {
        img = JS_GetOpaque(argv[1], js_image_class_id);
        if (!img) {
            JS_FreeCString(ctx, name);
            return JS_ThrowTypeError(ctx, "gpu.texture: the image was closed");
        }
        pixels = img->pixels;
        pixel_len = img->pixel_len;
    } else
#endif
    {
        if (str_data) JS_FreeCString(ctx, str_data);
        str_data = NULL;
        str_needs_free = 0;
        HlBufferView bv2;
        if (!js_get_buffer(ctx, argv[1], &bv2, &str_data, &str_needs_free)) {
            JS_FreeCString(ctx, name);
            return JS_ThrowTypeError(ctx, "gpu.texture: the data was closed");
        }
        pixels = bv2.data;
        pixel_len = bv2.len;
    }

    if (width == 0 || height == 0) {
        if (str_data) JS_FreeCString(ctx, str_data);
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "gpu.texture: width and height required");
    }

    /* The texture's device memory and the upload are charged before they
     * are made, one unit per 64 bytes: a create with no data allocated up
     * to 16384^2 x 8 bytes for nothing (audit 17). Runs no app code. */
    if (hl_js_budget_charge(ctx,
            hl_work_bulk_units((uint64_t)hl_gpu_tex_bytes(width, height, format)) +
            hl_work_bulk_units((uint64_t)pixel_len))) {
        if (str_data) JS_FreeCString(ctx, str_data);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    int rc = hl_cap_gpu_texture_create(gpu, device, name, width, height,
                                        format, storage, filter,
                                        address_u, address_v);
    if (rc == HL_GPU_OK && pixels && pixel_len > 0)
        rc = hl_cap_gpu_texture_write(gpu, device, name, pixels, pixel_len);

    if (str_data) JS_FreeCString(ctx, str_data);
    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.texture %s", rc == HL_GPU_ERR_BUSY ? "gpu_busy" : rc == HL_GPU_ERR_TIMEOUT ? "gpu_timeout" : "failed");
    return JS_TRUE;

opt_fail:
    if (str_data) JS_FreeCString(ctx, str_data);
    JS_FreeCString(ctx, name);
    return JS_EXCEPTION;
}

/* ── gpu.textureRead(name, opts?) → HlImage ──────────────────────── */

#ifdef HL_ENABLE_IMAGE
static JSValue js_gpu_texture_read(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "gpu.textureRead requires name");

    HlGpuCtx *gpu = js_get_gpu_ctx(ctx);
    if (!gpu) return JS_ThrowInternalError(ctx, "gpu not available");

    /* The device opts name (it read the default device's). A throwing
     * getter fails the call. */
    int device = -1;
    if (argc > 1 && JS_IsObject(argv[1])) {
        int32_t d;
        int r = hl_js_opt_int32(ctx, argv[1], "device", &d);
        if (r < 0) return JS_EXCEPTION;
        if (r) device = d;
    }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    void *data = NULL;
    size_t len = 0;
    uint32_t w = 0, h = 0;
    HlGpuTexFormat fmt = HL_GPU_TEX_RGBA8;

    int rc = hl_cap_gpu_texture_read(gpu, device, name, js_gpu_heap_limit(ctx),
                                      &data, &len, &w, &h, &fmt);
    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.textureRead %s", rc == HL_GPU_ERR_BUSY ? "gpu_busy" : rc == HL_GPU_ERR_TIMEOUT ? "gpu_timeout" : "failed");

    /* The read-back and its copy into the image: 1 unit per 8 bytes, before
     * the copy (audit 26). */
    if (hl_js_budget_charge(ctx, hl_work_bytes_units((uint64_t)len)) < 0) {
        free(data);
        return JS_EXCEPTION;
    }

    /* Create HlImage, then wrap as JS object */
    HlImageAlloc ia;
    hl_js_image_alloc(ctx, &ia);
    HlImage *img = hl_image_new(w, h, (HlImageFormat)fmt, data, len, &ia);
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
#endif /* HL_ENABLE_IMAGE */

/* ── JS texture desc parsing helper ──────────────────────────────── */

/* keep[i] (max_descs of them, set to JS_UNDEFINED by the caller) holds desc
 * i's data ArrayBuffer until the caller frees it after the dispatch: `data`
 * may be a getter returning a fresh one, freed when its value is released. */
static int js_parse_texture_descs(JSContext *ctx, JSValueConst arr,
                                   HlGpuTextureDesc *descs, int max_descs,
                                   const char **tracked_strs, int *str_count,
                                   int str_max, JSValue *keep,
                                   HlJsGpuView *views, int *nviews, int max_views)
{
    /* Returns the count, or -1 with the exception pending: a getter, a
     * valueOf or a conversion that threw stays the call's error (audit 23;
     * it was ignored and the dispatch went on). Every name / data string
     * taken so far is in tracked_strs and every kept buffer in keep[], for
     * the caller to release either way. */
    if (!JS_IsArray(ctx, arr))
        return 0;
    int32_t count = 0;
    if (hl_js_get_length32(ctx, arr, &count) < 0) return -1;
    if (count > max_descs) count = max_descs;

    for (int32_t i = 0; i < count; i++) {
        memset(&descs[i], 0, sizeof(descs[i]));
        descs[i].binding = -1;
        JSValue elem = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        if (JS_IsException(elem)) return -1;

        if (JS_IsObject(elem)) {
            JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
            if (JS_IsException(nv)) goto fail;
            if (JS_IsString(nv)) {
                descs[i].name = JS_ToCString(ctx, nv);
                if (!descs[i].name) { JS_FreeValue(ctx, nv); goto fail; }
                if (*str_count < str_max)
                    tracked_strs[(*str_count)++] = descs[i].name;
            }
            JS_FreeValue(ctx, nv);

            int st = hl_js_opt_bool(ctx, elem, "storage");
            if (st < 0) goto fail;
            descs[i].storage = st;

            uint32_t u;
            int r = hl_js_opt_uint32(ctx, elem, "width", &u);
            if (r < 0) goto fail;
            if (r) descs[i].width = u;
            r = hl_js_opt_uint32(ctx, elem, "height", &u);
            if (r < 0) goto fail;
            if (r) descs[i].height = u;

            JSValue fv = JS_GetPropertyStr(ctx, elem, "format");
            if (JS_IsException(fv)) goto fail;
            if (JS_IsString(fv)) {
                const char *f = JS_ToCString(ctx, fv);
                if (!f) { JS_FreeValue(ctx, fv); goto fail; }
                descs[i].format = js_parse_tex_format(f);
                JS_FreeCString(ctx, f);
            }
            JS_FreeValue(ctx, fv);

            /* data: ArrayBuffer or string */
            JSValue dv = JS_GetPropertyStr(ctx, elem, "data");
            if (JS_IsException(dv)) goto fail;
            if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                size_t dlen;
                const void *dab;
                int bk = js_gpu_bytes_of(ctx, dv, "gpu: texture", &dab, &dlen);
                if (bk < 0) { JS_FreeValue(ctx, dv); goto fail; }
                if (bk) {
                    keep[i] = JS_DupValue(ctx, dv);
                    descs[i].data = dab;
                    descs[i].data_len = dlen;
                    js_gpu_note_view(views, nviews, max_views, keep[i],
                                     &descs[i].data, dlen);
                } else {
                    const char *ds = JS_ToCStringLen(ctx, &dlen, dv);
                    if (!ds) { JS_FreeValue(ctx, dv); goto fail; }
                    descs[i].data = ds;
                    descs[i].data_len = dlen;
                    if (*str_count < str_max)
                        tracked_strs[(*str_count)++] = ds;
                }
            }
            JS_FreeValue(ctx, dv);
        }
        JS_FreeValue(ctx, elem);
        continue;
    fail:
        JS_FreeValue(ctx, elem);
        return -1;
    }
    return count;
}

/* push_result callback: convert HlWorkerGpuOp result to JS value */
static JSValue js_push_worker_gpu_result(JSContext *ctx, void *driver)
{
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)driver;
    if (op->error)
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: %s", op->error_msg);
    /* A pipeline with more than one output resolves to an array of
     * ArrayBuffers, as gpu.pipeline returns (audit 29: it resolved to an
     * empty buffer - the worker moves only a single output to op->output). */
    if (op->is_pipeline && op->pipe_result.count > 1) {
        JSValue arr = JS_NewArray(ctx);
        if (JS_IsException(arr)) return arr;
        for (int i = 0; i < op->pipe_result.count; i++) {
            JSValue ab = JS_NewArrayBufferCopy(ctx,
                (const uint8_t *)op->pipe_result.data[i], op->pipe_result.len[i]);
            if (JS_IsException(ab) ||
                JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, ab,
                                             JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        }
        return arr;
    }
    if (op->output && op->output_len > 0)
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)op->output, op->output_len);
    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

/* Async dispatch - submits GPU work to thread pool, returns Promise */
/* Frees js_gpu_async_dispatch's copies before an op owns them: each
 * buffer's data and name, the arrays, and the uniforms. */
/* A copy of @p len bytes an async op makes off the VM heap (its uniforms and
 * buffer data, which a worker reads after the binding returns). Each source is
 * on the JS heap, but a call holds up to 16 (a pipeline 64) of them, for as
 * long as the op waits in the pool queue, and a copy whose malloc failed ran
 * as an empty buffer (audit 20). The copy holds its bytes on the VM's off-heap
 * total (`wasm_buf_bytes`, shared with OWNED WasmBuffers) under the heap
 * limit, through @p hold, and is charged 1 unit per 8 bytes. Returns the copy,
 * or NULL with an exception pending and nothing held for it. */
static void *js_gpu_offheap_copy(JSContext *ctx, HlJS *js, HlOffheapHold *hold,
                                 const void *src, size_t len, const char *who)
{
    if (hl_offheap_take(hold, &js->wasm_buf_bytes, js->max_heap_bytes,
                        len) != 0) {
        hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowRangeError(ctx, "%s: its buffers would take the VM past its "
                           "heap limit (close unused buffers)", who));
        return NULL;
    }
    if (hl_js_budget_charge(ctx, hl_work_bytes_units((uint64_t)len)) < 0) {
        hl_offheap_give(hold, len);
        return NULL;
    }
    void *p = malloc(len);
    if (!p) {
        hl_offheap_give(hold, len);
        JS_ThrowInternalError(ctx, "%s: out of memory", who);
        return NULL;
    }
    memcpy(p, src, len);
    return p;
}

/* The pipeline's form of js_gpu_offheap_copy: its parse loop keeps reading
 * (getters are app code) after a failure, so this throws nothing. Sets
 * *@p over when the hold would pass the heap limit, *@p oom when malloc
 * fails; the loop's end reports them and charges the hold's total. */
static void *js_gpu_offheap_take_copy(HlJS *js, HlOffheapHold *hold,
                                      const void *src, size_t len,
                                      int *over, int *oom)
{
    if (hl_offheap_take(hold, &js->wasm_buf_bytes, js->max_heap_bytes,
                        len) != 0) {
        *over = 1;
        return NULL;
    }
    void *p = malloc(len);
    if (!p) {
        hl_offheap_give(hold, len);
        *oom = 1;
        return NULL;
    }
    memcpy(p, src, len);
    return p;
}

static void js_gpu_async_dispatch_free_copies(HlGpuBufferDesc *descs,
                                              void **data, int count,
                                              void *uniforms)
{
    for (int i = 0; i < count; i++) {
        free(data[i]);
        hl_free_const(descs[i].name);
    }
    free(data); free(descs); free(uniforms);
}

static JSValue js_gpu_async_dispatch(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx, "gpu.async not available (no thread pool)");
    if (!js->base.async_ctx)
        return JS_ThrowInternalError(ctx, "gpu.async requires an active event loop");
    if (hl_js_async_gate(ctx, js, "gpu.async.dispatch") != 0)
        return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, "gpu.async")) return JS_EXCEPTION;
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
    opts.max_output = js_gpu_heap_limit(ctx);   /* audit 17 */

    JSValue opts_val = argv[1];

    /* A getter, valueOf or conversion that throws (or the budget interrupt)
     * fails the call with its exception (audit 23: the results were ignored
     * and the op submitted on defaults with the exception pending). */
    int32_t i32; uint32_t u32; int r;
    if ((r = hl_js_opt_int32(ctx, opts_val, "device", &i32)) < 0) goto early_fail;
    if (r) opts.device = i32;

    JSValue wg_val = JS_GetPropertyStr(ctx, opts_val, "workgroups");
    if (JS_IsException(wg_val)) goto early_fail;
    if (JS_IsObject(wg_val)) {
        uint32_t x = 1, y = 1, z = 1;
        if (hl_js_get_uint32(ctx, wg_val, "x", &x) < 0 ||
            hl_js_get_uint32(ctx, wg_val, "y", &y) < 0 ||
            hl_js_get_uint32(ctx, wg_val, "z", &z) < 0) {
            JS_FreeValue(ctx, wg_val);
            goto early_fail;
        }
        opts.workgroups.x = x; opts.workgroups.y = y; opts.workgroups.z = z;
    }
    JS_FreeValue(ctx, wg_val);

    /* output as the sync call reads it (audit 29): false = no read-back
     * (it converted to 0 and read buffer 0 back). */
    {
        JSValue out_val = JS_GetPropertyStr(ctx, opts_val, "output");
        if (JS_IsException(out_val)) goto early_fail;
        if (JS_IsBool(out_val) && !JS_ToBool(ctx, out_val)) {
            opts.output_buffer = HL_GPU_OUTPUT_NONE;
        } else if (!JS_IsUndefined(out_val)) {
            if (JS_ToInt32(ctx, &i32, out_val) < 0) {
                JS_FreeValue(ctx, out_val);
                goto early_fail;
            }
            opts.output_buffer = i32;
        }
        JS_FreeValue(ctx, out_val);
    }

    if ((r = js_gpu_timeout_opt(ctx, opts_val, &u32)) < 0) goto early_fail;
    if (r) opts.timeout_ms = u32;

    /* The async dispatch runs no textures: refuse the options rather than
     * ignore them (audit 29). */
    {
        static const char *const tex_keys[] = { "textures", "outputTexture" };
        for (size_t k = 0; k < sizeof(tex_keys) / sizeof(tex_keys[0]); k++) {
            JSValue tv = JS_GetPropertyStr(ctx, opts_val, tex_keys[k]);
            if (JS_IsException(tv)) goto early_fail;
            int present = !JS_IsUndefined(tv) && !JS_IsNull(tv);
            JS_FreeValue(ctx, tv);
            if (present) {
                JS_ThrowTypeError(ctx, "gpu.async.dispatch: %s is not "
                                  "supported (use gpu.dispatch)", tex_keys[k]);
                goto early_fail;
            }
        }
    }

    /* Deep-copy uniforms */
    void *uni_copy = NULL;
    size_t uni_len = 0;
    HlOffheapHold hold = {0};   /* every copy below (js_gpu_offheap_copy) */
    int copy_failed = 0;
    JSValue uni_val = JS_GetPropertyStr(ctx, opts_val, "uniforms");
    if (JS_IsException(uni_val)) {
        copy_failed = 1;
    } else if (!JS_IsUndefined(uni_val) && !JS_IsNull(uni_val)) {
        const void *uni_ab;
        int bk = js_gpu_bytes_of(ctx, uni_val, "gpu.async.dispatch",
                                 &uni_ab, &uni_len);
        if (bk < 0) {
            copy_failed = 1;
        } else if (bk) {
            if (uni_len > 0) {
                uni_copy = js_gpu_offheap_copy(ctx, js, &hold, uni_ab, uni_len,
                                               "gpu.async.dispatch");
                if (!uni_copy) copy_failed = 1;
            }
        } else {
            const char *uni_str = JS_ToCStringLen(ctx, &uni_len, uni_val);
            if (uni_str) {
                if (uni_len > 0) {
                    uni_copy = js_gpu_offheap_copy(ctx, js, &hold, uni_str,
                                                   uni_len, "gpu.async.dispatch");
                    if (!uni_copy) copy_failed = 1;
                }
                JS_FreeCString(ctx, uni_str);
            } else {
                copy_failed = 1;
            }
        }
    }
    JS_FreeValue(ctx, uni_val);
    if (copy_failed) {
        free(uni_copy);
        hl_offheap_drop(&hold);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    /* Deep-copy buffers */
    int buf_count = 0;
    HlGpuBufferDesc *buf_descs = NULL;
    void **buf_data_ptrs = NULL;

    JSValue bufs_val = JS_GetPropertyStr(ctx, opts_val, "buffers");
    if (JS_IsException(bufs_val)) {
        copy_failed = 1;
    } else if (JS_IsArray(ctx, bufs_val)) {
        int32_t len = 0;
        if (hl_js_get_length32(ctx, bufs_val, &len) < 0) {
            copy_failed = 1;
            len = 0;
        }
        if (len > 16) len = 16;
        if (len > 0) {
            buf_descs = calloc((size_t)len, sizeof(HlGpuBufferDesc));
            buf_data_ptrs = calloc((size_t)len, sizeof(void *));
            if (!buf_descs || !buf_data_ptrs) {
                free(buf_descs); free(buf_data_ptrs); free(uni_copy);
                hl_offheap_drop(&hold);
                JS_FreeValue(ctx, bufs_val);
                JS_FreeCString(ctx, name);
                return JS_ThrowInternalError(ctx, "gpu.async.dispatch: out of memory");
            }
            for (int32_t i = 0; i < len && !copy_failed; i++) {
                JSValue elem = JS_GetPropertyUint32(ctx, bufs_val, (uint32_t)i);
                if (JS_IsException(elem)) { copy_failed = 1; break; }
                if (JS_IsObject(elem)) {
                    /* Counted before anything can fail, so the cleanup
                     * below frees this entry's copies too. */
                    int bi = buf_count++;
                    buf_descs[bi].binding = -1;
                    /* Deep-copy buffer name for thread safety */
                    JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
                    if (JS_IsException(nv)) {
                        copy_failed = 1;
                    } else if (JS_IsString(nv)) {
                        size_t nlen;
                        const char *ns = JS_ToCStringLen(ctx, &nlen, nv);
                        if (!ns) copy_failed = 1;
                        /* Copied whatever its length, as the sync call
                         * passes it; a failed copy fails the call (audit
                         * 29: the name was dropped, so the entry ran as a
                         * transient buffer instead of the named one). */
                        if (ns) {
                            char *ncopy = malloc(nlen + 1);
                            if (ncopy) {
                                memcpy(ncopy, ns, nlen);
                                ncopy[nlen] = '\0';
                                buf_descs[bi].name = ncopy;
                            } else {
                                JS_ThrowInternalError(ctx,
                                    "gpu.async.dispatch: out of memory");
                                copy_failed = 1;
                            }
                            JS_FreeCString(ctx, ns);
                        }
                    }
                    JS_FreeValue(ctx, nv);

                    JSValue dv = copy_failed ? JS_UNDEFINED
                                             : JS_GetPropertyStr(ctx, elem, "data");
                    if (JS_IsException(dv)) {
                        copy_failed = 1;
                    } else if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                        size_t dlen;
                        const void *dab;
                        int bk = js_gpu_bytes_of(ctx, dv, "gpu.async.dispatch",
                                                 &dab, &dlen);
                        if (bk < 0) {
                            copy_failed = 1;
                        } else if (bk) {
                            if (dlen > 0) {
                                buf_data_ptrs[bi] = js_gpu_offheap_copy(
                                    ctx, js, &hold, dab, dlen,
                                    "gpu.async.dispatch");
                                if (buf_data_ptrs[bi]) {
                                    buf_descs[bi].data = buf_data_ptrs[bi];
                                    buf_descs[bi].data_len = dlen;
                                    buf_descs[bi].size = dlen;
                                } else {
                                    copy_failed = 1;
                                }
                            }
                        } else {
                            const char *ds = JS_ToCStringLen(ctx, &dlen, dv);
                            if (ds) {
                                if (dlen > 0) {
                                    buf_data_ptrs[bi] = js_gpu_offheap_copy(
                                        ctx, js, &hold, ds, dlen,
                                        "gpu.async.dispatch");
                                    if (buf_data_ptrs[bi]) {
                                        buf_descs[bi].data = buf_data_ptrs[bi];
                                        buf_descs[bi].data_len = dlen;
                                        buf_descs[bi].size = dlen;
                                    } else {
                                        copy_failed = 1;
                                    }
                                }
                                JS_FreeCString(ctx, ds);
                            } else {
                                copy_failed = 1;
                            }
                        }
                    }
                    JS_FreeValue(ctx, dv);

                    int64_t sz;
                    int sr = copy_failed ? 0
                           : hl_js_opt_int64(ctx, elem, "size", &sz);
                    if (sr < 0) copy_failed = 1;
                    else if (sr) buf_descs[bi].size = (size_t)sz;

                    JSValue uv = copy_failed ? JS_UNDEFINED
                                             : JS_GetPropertyStr(ctx, elem, "usage");
                    if (JS_IsException(uv)) {
                        copy_failed = 1;
                    } else if (JS_IsString(uv)) {
                        const char *us = JS_ToCString(ctx, uv);
                        if (us) {
                            if (strcmp(us, "read") == 0) buf_descs[bi].usage = HL_GPU_USAGE_READ;
                            else if (strcmp(us, "write") == 0) buf_descs[bi].usage = HL_GPU_USAGE_WRITE;
                            else if (strcmp(us, "readwrite") == 0) buf_descs[bi].usage = HL_GPU_USAGE_READWRITE;
                            JS_FreeCString(ctx, us);
                        } else {
                            copy_failed = 1;
                        }
                    }
                    JS_FreeValue(ctx, uv);
                }
                JS_FreeValue(ctx, elem);
            }
        }
    }
    JS_FreeValue(ctx, bufs_val);
    if (copy_failed) {
        /* A failed copy fails the call: it ran as an empty buffer (audit
         * 20); so does a getter that threw (audit 23). */
        js_gpu_async_dispatch_free_copies(buf_descs, buf_data_ptrs,
                                          buf_count, uni_copy);
        hl_offheap_drop(&hold);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    /* The call's own buffers and uniforms are charged before the op is
     * submitted, as the sync call's (audit 17). */
    {
        HlGpuDispatchOpts c = opts;
        c.buffers = buf_descs;
        c.buffer_count = buf_count;
        c.uniforms = uni_copy;
        c.uniforms_len = uni_len;
        if (hl_js_budget_charge(ctx, hl_work_bulk_units(
                                    (uint64_t)hl_gpu_dispatch_charge_bytes(&c)))) {
            js_gpu_async_dispatch_free_copies(buf_descs, buf_data_ptrs,
                                              buf_count, uni_copy);
            hl_offheap_drop(&hold);
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
    }

    /* Allocate worker op */
    HlWorkerGpuOp *op = calloc(1, sizeof(HlWorkerGpuOp));
    if (!op) {
        js_gpu_async_dispatch_free_copies(buf_descs, buf_data_ptrs,
                                          buf_count, uni_copy);
        hl_offheap_drop(&hold);
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: out of memory");
    }

    op->server = js->server;
    op->gpu_ctx = js->base.gpu_ctx;
    /* Read-backs on the VM's in-flight result total (audit 29). */
    hl_offheap_shared_init(&op->result_hold, &js->async_result_bytes,
                           js->max_heap_bytes);
    snprintf(op->shader_name, sizeof(op->shader_name), "%s", name);
    op->opts = opts;
    op->buffers = buf_descs;
    op->buffer_data = buf_data_ptrs;
    op->buffer_count = buf_count;
    op->uniforms = uni_copy;
    op->uniforms_len = uni_len;
    op->copy_hold = hold;

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
    actx->detached = (hl_js_cont_suspend_conn(cont) == NULL);

    op->async_ctx = actx;
    atomic_store(&op->cancelled, 0);

    if (hl_worker_gpu_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: thread pool full");
    }

    if (!actx->detached &&
        hl_js_op_suspend(js, (HlSuspendOp *)&actx->op) < 0) {
        atomic_store(&op->cancelled, 1);
        /* Never armed: destroy frees resolve / reject. cancel() would also
         * end the still-running handler's request life. */
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: failed to suspend");
    }

    return promise;

early_fail:
    JS_FreeCString(ctx, name);
    return JS_EXCEPTION;
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
    int32_t stage_count = 0;
    if (hl_js_get_length32(ctx, argv[0], &stage_count) < 0)
        return JS_EXCEPTION;   /* audit 23 */
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

    /* The ArrayBuffers stage uniforms and buffer data point into, held until
     * after the dispatch: a getter may return a fresh one, freed as soon as
     * its value is released. At most one per stage uniform and per buffer. */
    JSValue js_keep[HL_GPU_MAX_PIPELINE_STAGES + HL_GPU_MAX_PIPELINE_BUFFERS];
    int js_keep_count = 0;
    /* ...and where each one's bytes go, re-taken after the last getter. */
    HlJsGpuView views[HL_GPU_MAX_PIPELINE_STAGES + HL_GPU_MAX_PIPELINE_BUFFERS];
    int nviews = 0;
    const int max_views = HL_GPU_MAX_PIPELINE_STAGES + HL_GPU_MAX_PIPELINE_BUFFERS;

    /* The options (second argument), declared before the first getter so a
     * failed parse can skip to the cleanup. */
    HlGpuPipelineOutput outputs[HL_GPU_MAX_PIPELINE_OUTPUTS];
    int output_count = 0;
    int device = -1;
    uint32_t pipe_timeout_ms = 0;
    /* A getter, valueOf or conversion that throws (or the budget interrupt)
     * fails the call with its exception: perr skips the rest of the parse
     * and the dispatch (audit 23 - the results were ignored). */
    int perr = 0;

    for (int32_t s = 0; s < stage_count && !perr; s++) {
        JSValue stage_val = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)s);
        if (JS_IsException(stage_val)) { perr = 1; break; }
        if (!JS_IsObject(stage_val)) { JS_FreeValue(ctx, stage_val); continue; }

        /* shader */
        JSValue sv2 = JS_GetPropertyStr(ctx, stage_val, "shader");
        stages[s].shader = JS_IsException(sv2) ? NULL : JS_ToCString(ctx, sv2);
        JS_FreeValue(ctx, sv2);
        if (!stages[s].shader) { JS_FreeValue(ctx, stage_val); perr = 1; break; }
        JS_TRACK_STR(stages[s].shader);

        /* workgroups */
        JSValue wg = JS_GetPropertyStr(ctx, stage_val, "workgroups");
        if (JS_IsException(wg)) { JS_FreeValue(ctx, stage_val); perr = 1; break; }
        if (JS_IsObject(wg)) {
            uint32_t x = 1, y = 1, z = 1;
            if (hl_js_get_uint32(ctx, wg, "x", &x) < 0 ||
                hl_js_get_uint32(ctx, wg, "y", &y) < 0 ||
                hl_js_get_uint32(ctx, wg, "z", &z) < 0)
                perr = 1;
            stages[s].workgroups = (HlGpuWorkgroups){ x, y, z };
        }
        JS_FreeValue(ctx, wg);
        if (perr) { JS_FreeValue(ctx, stage_val); break; }

        /* uniforms */
        JSValue uni = JS_GetPropertyStr(ctx, stage_val, "uniforms");
        if (JS_IsException(uni)) { JS_FreeValue(ctx, stage_val); perr = 1; break; }
        if (!JS_IsUndefined(uni) && !JS_IsNull(uni)) {
            size_t ulen;
            const void *uab;
            int bk = js_gpu_bytes_of(ctx, uni, "gpu.pipeline", &uab, &ulen);
            if (bk < 0) {
                perr = 1;
            } else if (bk) {
                js_keep[js_keep_count] = JS_DupValue(ctx, uni);
                js_gpu_note_view(views, &nviews, max_views,
                                 js_keep[js_keep_count],
                                 &stages[s].uniforms, ulen);
                js_keep_count++;
                stages[s].uniforms = uab;
                stages[s].uniforms_len = ulen;
            } else {
                const char *us = JS_ToCStringLen(ctx, &ulen, uni);
                if (us) {
                    stages[s].uniforms = us;
                    stages[s].uniforms_len = ulen;
                    JS_TRACK_STR(us);
                } else {
                    perr = 1;
                }
            }
        }
        JS_FreeValue(ctx, uni);
        if (perr) { JS_FreeValue(ctx, stage_val); break; }

        /* buffers */
        JSValue bufs_val = JS_GetPropertyStr(ctx, stage_val, "buffers");
        if (JS_IsException(bufs_val)) { JS_FreeValue(ctx, stage_val); perr = 1; break; }
        if (JS_IsArray(ctx, bufs_val)) {
            int32_t bc = 0;
            if (hl_js_get_length32(ctx, bufs_val, &bc) < 0) perr = 1;
            /* A Proxy can report any length. A negative one moved buf_offset
             * below zero, and the next stage wrote before all_bufs. */
            if (bc < 0) bc = 0;
            if (bc > 16) bc = 16;
            if (buf_offset + bc > HL_GPU_MAX_PIPELINE_BUFFERS)
                bc = HL_GPU_MAX_PIPELINE_BUFFERS - buf_offset;

            stages[s].buffers = &all_bufs[buf_offset];
            stages[s].buffer_count = bc;

            for (int32_t b = 0; b < bc && !perr; b++) {
                JSValue elem = JS_GetPropertyUint32(ctx, bufs_val, (uint32_t)b);
                if (JS_IsException(elem)) { perr = 1; break; }
                if (JS_IsObject(elem)) {
                    JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
                    if (JS_IsException(nv)) {
                        perr = 1;
                    } else if (JS_IsString(nv)) {
                        all_bufs[buf_offset + b].name = JS_ToCString(ctx, nv);
                        if (!all_bufs[buf_offset + b].name) perr = 1;
                        JS_TRACK_STR(all_bufs[buf_offset + b].name);
                    }
                    JS_FreeValue(ctx, nv);

                    JSValue dv = perr ? JS_UNDEFINED
                                      : JS_GetPropertyStr(ctx, elem, "data");
                    if (JS_IsException(dv)) {
                        perr = 1;
                    } else if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                        size_t dlen;
                        const void *dab;
                        int bk = js_gpu_bytes_of(ctx, dv, "gpu.pipeline",
                                                 &dab, &dlen);
                        if (bk < 0) {
                            perr = 1;
                        } else if (bk) {
                            js_keep[js_keep_count] = JS_DupValue(ctx, dv);
                            js_gpu_note_view(views, &nviews, max_views,
                                             js_keep[js_keep_count],
                                             &all_bufs[buf_offset + b].data,
                                             dlen);
                            js_keep_count++;
                            all_bufs[buf_offset + b].data = dab;
                            all_bufs[buf_offset + b].data_len = dlen;
                            all_bufs[buf_offset + b].size = dlen;
                        } else {
                            const char *ds = JS_ToCStringLen(ctx, &dlen, dv);
                            if (ds) {
                                all_bufs[buf_offset + b].data = ds;
                                all_bufs[buf_offset + b].data_len = dlen;
                                all_bufs[buf_offset + b].size = dlen;
                                JS_TRACK_STR(ds);
                            } else {
                                perr = 1;
                            }
                        }
                    }
                    JS_FreeValue(ctx, dv);

                    int64_t sz;
                    int sr = perr ? 0 : hl_js_opt_int64(ctx, elem, "size", &sz);
                    if (sr < 0) perr = 1;
                    else if (sr) all_bufs[buf_offset + b].size = (size_t)sz;

                    all_bufs[buf_offset + b].binding = -1;
                }
                JS_FreeValue(ctx, elem);
            }
            buf_offset += bc;
        }
        JS_FreeValue(ctx, bufs_val);
        JS_FreeValue(ctx, stage_val);
    }
    if (perr) goto parsed;

    /* Parse opts (second argument) */
    if (argc > 1 && JS_IsObject(argv[1])) {
        int32_t d;
        int r = hl_js_opt_int32(ctx, argv[1], "device", &d);
        if (r < 0) { perr = 1; goto parsed; }
        if (r) device = d;

        /* output: false → fire-and-forget pipeline */
        JSValue out_flag = JS_GetPropertyStr(ctx, argv[1], "output");
        if (JS_IsException(out_flag)) { perr = 1; goto parsed; }
        if (JS_IsBool(out_flag) && !JS_ToBool(ctx, out_flag))
            output_count = HL_GPU_OUTPUT_NONE;
        JS_FreeValue(ctx, out_flag);

        /* outputs: [{stage: 0, buffer: 0}, ...] (0-indexed) */
        JSValue ov = JS_GetPropertyStr(ctx, argv[1], "outputs");
        if (JS_IsException(ov)) { perr = 1; goto parsed; }
        if (output_count != HL_GPU_OUTPUT_NONE && JS_IsArray(ctx, ov)) {
            if (hl_js_get_length32(ctx, ov, &output_count) < 0) {
                output_count = 0;
                perr = 1;
            }
            if (output_count > HL_GPU_MAX_PIPELINE_OUTPUTS)
                output_count = HL_GPU_MAX_PIPELINE_OUTPUTS;
            for (int i = 0; i < output_count && !perr; i++) {
                JSValue oe = JS_GetPropertyUint32(ctx, ov, (uint32_t)i);
                outputs[i].stage = 0; outputs[i].buffer = 0;
                if (JS_IsException(oe)) { perr = 1; break; }
                if (JS_IsObject(oe)) {
                    JSValue sv3 = JS_GetPropertyStr(ctx, oe, "stage");
                    JSValue bv = JS_IsException(sv3) ? JS_UNDEFINED
                                                     : JS_GetPropertyStr(ctx, oe, "buffer");
                    int32_t si = 0, bi = 0;
                    if (JS_IsException(sv3) || JS_IsException(bv) ||
                        JS_ToInt32(ctx, &si, sv3) < 0 ||
                        JS_ToInt32(ctx, &bi, bv) < 0)
                        perr = 1;
                    outputs[i].stage = si; outputs[i].buffer = bi;
                    JS_FreeValue(ctx, sv3); JS_FreeValue(ctx, bv);
                }
                JS_FreeValue(ctx, oe);
            }
        }
        JS_FreeValue(ctx, ov);
        if (perr) goto parsed;

        uint32_t t;
        r = js_gpu_timeout_opt(ctx, argv[1], &t);
        if (r < 0) { perr = 1; goto parsed; }
        if (r) pipe_timeout_ms = t;
    }

parsed:;

    HlGpuPipelineOpts opts = {
        .stages = stages,
        .stage_count = stage_count,
        .outputs = output_count > 0 ? outputs : NULL,
        .output_count = output_count,
        .device = device,
        .timeout_ms = pipe_timeout_ms,
        .max_output = js_gpu_heap_limit(ctx),   /* audit 17 */
    };

    HlGpuPipelineResult result;
    const char *err_msg = NULL;
    /* The stages' own buffers are charged before they are made (audit 17);
     * then, the last getter having run, take the ArrayBuffers' bytes. */
    int retake_failed = perr ||
        hl_js_budget_charge(ctx, hl_work_bulk_units(
                                (uint64_t)hl_gpu_pipeline_charge_bytes(&opts))) ||
        js_gpu_retake_views(ctx, views, nviews, "gpu.pipeline") != 0;
    int rc = retake_failed ? HL_GPU_OK
           : hl_cap_gpu_pipeline(gpu, &opts, &result, &err_msg);

    /* Free tracked JS strings, and release the kept ArrayBuffers */
    for (int i = 0; i < js_string_count; i++)
        JS_FreeCString(ctx, js_strings[i]);
    for (int i = 0; i < js_keep_count; i++)
        JS_FreeValue(ctx, js_keep[i]);
    #undef JS_TRACK_STR

    if (retake_failed)
        return JS_EXCEPTION;

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

    /* Checked builds (audit 29): a failed element fails the call. */
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < result.count && !JS_IsException(arr); i++) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)result.data[i],
                                           result.len[i]);
        if (JS_IsException(ab) ||
            JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, ab,
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            arr = JS_EXCEPTION;
        }
    }
    hl_cap_gpu_pipeline_result_free(&result);
    return arr;
}

/* Async pipeline - falls back to sync for now */
/* JS async pipeline - deep-copy all stage data, submit to thread pool */
static JSValue js_gpu_async_pipeline(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx, "gpu.async not available (no thread pool)");
    if (!js->base.async_ctx)
        return JS_ThrowInternalError(ctx, "gpu.async requires an active event loop");
    if (hl_js_async_gate(ctx, js, "gpu.async.pipeline") != 0)
        return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, "gpu.async")) return JS_EXCEPTION;
    if (!js->base.gpu_ctx)
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: GPU not initialized");
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "gpu.async.pipeline requires stages array");

    /* Parse stage count */
    int32_t stage_count = 0;
    if (hl_js_get_length32(ctx, argv[0], &stage_count) < 0)
        return JS_EXCEPTION;   /* audit 23 */
    if (stage_count <= 0 || stage_count > HL_GPU_MAX_PIPELINE_STAGES)
        return JS_ThrowRangeError(ctx, "gpu.async.pipeline: invalid stage count");

    /* Allocate worker op */
    HlWorkerGpuOp *op = calloc(1, sizeof(HlWorkerGpuOp));
    if (!op) return JS_ThrowInternalError(ctx, "gpu.async.pipeline: out of memory");

    op->server = js->server;
    op->gpu_ctx = js->base.gpu_ctx;
    /* Read-backs on the VM's in-flight result total (audit 29). */
    hl_offheap_shared_init(&op->result_hold, &js->async_result_bytes,
                           js->max_heap_bytes);
    op->is_pipeline = 1;
    op->stage_count = stage_count;

    op->stages = calloc((size_t)stage_count, sizeof(HlGpuPipelineStage));
    op->stage_uniforms = calloc((size_t)stage_count, sizeof(void *));
    if (!op->stages || !op->stage_uniforms) {
        hl_worker_gpu_op_free(op); free(op);
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: out of memory");
    }

    /* A getter, valueOf or conversion that throws (or the budget interrupt)
     * fails the call with its exception: perr stops the parse, and the op
     * is freed before it is submitted (audit 23 - the results were ignored
     * and the op submitted on defaults with the exception pending). */
    int perr = 0;

    /* Count total buffers across all stages for flat allocation */
    int total_bufs = 0;
    for (int32_t s = 0; s < stage_count && !perr; s++) {
        JSValue sv = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)s);
        if (JS_IsException(sv)) { perr = 1; break; }
        if (JS_IsObject(sv)) {
            JSValue bv = JS_GetPropertyStr(ctx, sv, "buffers");
            if (JS_IsException(bv)) perr = 1;
            else if (JS_IsArray(ctx, bv)) {
                int32_t bc = 0;
                if (hl_js_get_length32(ctx, bv, &bc) < 0) perr = 1;
                if (bc < 0) bc = 0;
                if (bc > 16) bc = 16;
                total_bufs += bc;
                /* The pipeline holds at most this many buffers in all (the
                 * per-stage cap alone allowed 80). */
                if (total_bufs > HL_GPU_MAX_PIPELINE_BUFFERS) {
                    JS_FreeValue(ctx, bv);
                    JS_FreeValue(ctx, sv);
                    hl_worker_gpu_op_free(op); free(op);
                    return JS_ThrowRangeError(ctx, "gpu.async.pipeline: more than %d "
                                              "buffers in all", HL_GPU_MAX_PIPELINE_BUFFERS);
                }
            }
            JS_FreeValue(ctx, bv);
        }
        JS_FreeValue(ctx, sv);
    }

    if (perr) {
        hl_worker_gpu_op_free(op); free(op);
        return JS_EXCEPTION;
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
     * M-3: any OOM during deep-copy must abort the dispatch - previously
     * NULL was silently stored in op->stages[s].shader and op->buffer_data[]
     * and the worker thread would crash. We set `oom` and continue parsing
     * (to keep the JS_FreeValue/JS_FreeCString pairs balanced), then bail
     * before submitting to the thread pool. */
    int oom = 0;
    int over = 0;   /* a copy would pass the heap limit (audit 20) */
    int buf_off = 0;
    for (int32_t s = 0; s < stage_count && !perr; s++) {
        JSValue stage_val = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)s);
        if (JS_IsException(stage_val)) { perr = 1; break; }
        if (!JS_IsObject(stage_val)) { JS_FreeValue(ctx, stage_val); continue; }

        /* Shader name (deep-copy) */
        JSValue sv2 = JS_GetPropertyStr(ctx, stage_val, "shader");
        const char *sname = JS_IsException(sv2) ? NULL : JS_ToCString(ctx, sv2);
        if (sname) {
            op->stages[s].shader = strdup(sname);
            if (!op->stages[s].shader) oom = 1;
            JS_FreeCString(ctx, sname);
        } else {
            perr = 1;
        }
        JS_FreeValue(ctx, sv2);
        if (perr) { JS_FreeValue(ctx, stage_val); break; }

        /* Workgroups */
        JSValue wg = JS_GetPropertyStr(ctx, stage_val, "workgroups");
        if (JS_IsException(wg)) {
            perr = 1;
        } else if (JS_IsObject(wg)) {
            uint32_t x = 1, y = 1, z = 1;
            if (hl_js_get_uint32(ctx, wg, "x", &x) < 0 ||
                hl_js_get_uint32(ctx, wg, "y", &y) < 0 ||
                hl_js_get_uint32(ctx, wg, "z", &z) < 0)
                perr = 1;
            op->stages[s].workgroups = (HlGpuWorkgroups){ x, y, z };
        }
        JS_FreeValue(ctx, wg);
        if (perr) { JS_FreeValue(ctx, stage_val); break; }

        /* Per-stage uniforms (deep-copy) */
        JSValue uni = JS_GetPropertyStr(ctx, stage_val, "uniforms");
        if (JS_IsException(uni)) {
            perr = 1;
        } else if (!JS_IsUndefined(uni) && !JS_IsNull(uni)) {
            size_t ulen;
            const void *uab;
            int bk = js_gpu_bytes_of(ctx, uni, "gpu.async.pipeline",
                                     &uab, &ulen);
            if (bk < 0) {
                perr = 1;
            } else if (bk) {
                if (ulen > 0) {
                    op->stage_uniforms[s] = js_gpu_offheap_take_copy(js,
                        &op->copy_hold, uab, ulen, &over, &oom);
                    if (op->stage_uniforms[s])
                        op->stages[s].uniforms_len = ulen;
                }
            } else {
                const char *us = JS_ToCStringLen(ctx, &ulen, uni);
                if (!us) perr = 1;
                if (us && ulen > 0) {
                    op->stage_uniforms[s] = js_gpu_offheap_take_copy(js,
                        &op->copy_hold, us, ulen, &over, &oom);
                    if (op->stage_uniforms[s])
                        op->stages[s].uniforms_len = ulen;
                }
                if (us) JS_FreeCString(ctx, us);
            }
        }
        JS_FreeValue(ctx, uni);
        if (perr) { JS_FreeValue(ctx, stage_val); break; }

        /* Buffers (deep-copy) */
        JSValue bufs_val = JS_GetPropertyStr(ctx, stage_val, "buffers");
        if (JS_IsException(bufs_val)) { JS_FreeValue(ctx, stage_val); perr = 1; break; }
        if (JS_IsArray(ctx, bufs_val)) {
            int32_t bc = 0;
            if (hl_js_get_length32(ctx, bufs_val, &bc) < 0) perr = 1;
            if (bc < 0) bc = 0;
            if (bc > 16) bc = 16;
            int first_buf = buf_off;

            for (int32_t b = 0; b < bc && buf_off < total_bufs && !perr; b++) {
                JSValue elem = JS_GetPropertyUint32(ctx, bufs_val, (uint32_t)b);
                if (JS_IsException(elem)) { perr = 1; break; }
                if (JS_IsObject(elem)) {
                    /* Name (deep-copy) */
                    JSValue nv = JS_GetPropertyStr(ctx, elem, "name");
                    if (JS_IsException(nv)) {
                        perr = 1;
                    } else if (JS_IsString(nv)) {
                        const char *ns = JS_ToCString(ctx, nv);
                        if (ns) {
                            op->buffers[buf_off].name = strdup(ns);
                            if (!op->buffers[buf_off].name) oom = 1;
                            JS_FreeCString(ctx, ns);
                        } else {
                            perr = 1;
                        }
                    }
                    JS_FreeValue(ctx, nv);

                    /* Data (deep-copy) */
                    JSValue dv = perr ? JS_UNDEFINED
                                      : JS_GetPropertyStr(ctx, elem, "data");
                    if (JS_IsException(dv)) {
                        perr = 1;
                    } else if (!JS_IsUndefined(dv) && !JS_IsNull(dv)) {
                        size_t dlen;
                        const void *dab;
                        int bk = js_gpu_bytes_of(ctx, dv, "gpu.async.pipeline",
                                                 &dab, &dlen);
                        if (bk < 0) {
                            perr = 1;
                        } else if (bk) {
                            if (dlen > 0) {
                                op->buffer_data[buf_off] = js_gpu_offheap_take_copy(
                                    js, &op->copy_hold, dab, dlen, &over, &oom);
                                if (op->buffer_data[buf_off]) {
                                    op->buffers[buf_off].data = op->buffer_data[buf_off];
                                    op->buffers[buf_off].data_len = dlen;
                                    op->buffers[buf_off].size = dlen;
                                }
                            }
                        } else {
                            const char *ds = JS_ToCStringLen(ctx, &dlen, dv);
                            if (ds) {
                                if (dlen > 0) {
                                    op->buffer_data[buf_off] = js_gpu_offheap_take_copy(
                                        js, &op->copy_hold, ds, dlen, &over, &oom);
                                    if (op->buffer_data[buf_off]) {
                                        op->buffers[buf_off].data = op->buffer_data[buf_off];
                                        op->buffers[buf_off].data_len = dlen;
                                        op->buffers[buf_off].size = dlen;
                                    }
                                }
                                JS_FreeCString(ctx, ds);
                            } else {
                                perr = 1;
                            }
                        }
                    }
                    JS_FreeValue(ctx, dv);

                    /* Size override */
                    int64_t sz;
                    int sr = perr ? 0 : hl_js_opt_int64(ctx, elem, "size", &sz);
                    if (sr < 0) perr = 1;
                    else if (sr) op->buffers[buf_off].size = (size_t)sz;
                    op->buffers[buf_off].binding = -1;
                }
                JS_FreeValue(ctx, elem);
                buf_off++;
            }
            /* What was copied, not what `length` said: a getter can report
             * a larger length here than in the counting pass above, and the
             * worker lays the stages over op->buffers by these counts. */
            op->stages[s].buffer_count = buf_off - first_buf;
        }
        JS_FreeValue(ctx, bufs_val);
        JS_FreeValue(ctx, stage_val);
    }

    /* Parse opts (second argument) */
    op->pipe_opts.device = -1;
    if (!perr && argc > 1 && JS_IsObject(argv[1])) {
        int32_t d;
        int r = hl_js_opt_int32(ctx, argv[1], "device", &d);
        if (r < 0) goto opts_done;
        if (r) op->pipe_opts.device = d;

        /* output: false → fire-and-forget */
        JSValue of = JS_GetPropertyStr(ctx, argv[1], "output");
        if (JS_IsException(of)) goto opts_done;
        if (JS_IsBool(of) && !JS_ToBool(ctx, of))
            op->pipe_opts.output_count = HL_GPU_OUTPUT_NONE;
        JS_FreeValue(ctx, of);

        /* outputs array */
        JSValue ov = JS_GetPropertyStr(ctx, argv[1], "outputs");
        if (JS_IsException(ov)) goto opts_done;
        if (op->pipe_opts.output_count != HL_GPU_OUTPUT_NONE && JS_IsArray(ctx, ov)) {
            int32_t oc = 0;
            if (hl_js_get_length32(ctx, ov, &oc) < 0) { oc = 0; perr = 1; }
            if (oc > HL_GPU_MAX_PIPELINE_OUTPUTS) oc = HL_GPU_MAX_PIPELINE_OUTPUTS;
            if (oc > 0) {
                op->pipe_outputs = calloc((size_t)oc, sizeof(HlGpuPipelineOutput));
                if (op->pipe_outputs) {
                    op->pipe_opts.output_count = oc;
                    op->pipe_opts.outputs = op->pipe_outputs;
                    for (int i = 0; i < oc && !perr; i++) {
                        JSValue oe = JS_GetPropertyUint32(ctx, ov, (uint32_t)i);
                        if (JS_IsException(oe)) { perr = 1; break; }
                        if (JS_IsObject(oe)) {
                            JSValue sv3 = JS_GetPropertyStr(ctx, oe, "stage");
                            JSValue bv2 = JS_IsException(sv3) ? JS_UNDEFINED
                                        : JS_GetPropertyStr(ctx, oe, "buffer");
                            int32_t si = 0, bi = 0;
                            if (JS_IsException(sv3) || JS_IsException(bv2) ||
                                JS_ToInt32(ctx, &si, sv3) < 0 ||
                                JS_ToInt32(ctx, &bi, bv2) < 0)
                                perr = 1;
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
        if (perr) goto opts_done;

        uint32_t t;
        r = js_gpu_timeout_opt(ctx, argv[1], &t);
        if (r < 0) goto opts_done;
        if (r) op->pipe_opts.timeout_ms = t;
    }
    if (0) {
opts_done:
        perr = 1;
    }
    op->pipe_opts.max_output = js_gpu_heap_limit(ctx);   /* audit 17 */

    if (perr) {
        hl_worker_gpu_op_free(op); free(op);
        return JS_EXCEPTION;
    }

    /* Abort if any deep-copy hit OOM (M-3) - must happen before the op is
     * handed off to the worker thread, otherwise the worker dereferences
     * NULL shader names / buffer data. */
    if (oom) {
        hl_worker_gpu_op_free(op); free(op);
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: out of memory");
    }
    /* The copies are held on the VM's off-heap total under the heap limit
     * (a failed one fails the call, it ran as an empty buffer), and charged
     * 1 unit per 8 bytes - after the parse, which must not throw midway
     * (audit 20). */
    if (over) {
        hl_worker_gpu_op_free(op); free(op);
        return hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowRangeError(ctx, "gpu.async.pipeline: its buffers would "
                                  "take the VM past its heap limit (close "
                                  "unused buffers)"));
    }
    if (hl_js_budget_charge(ctx,
            hl_work_bytes_units((uint64_t)op->copy_hold.bytes)) < 0) {
        hl_worker_gpu_op_free(op); free(op);
        return JS_EXCEPTION;
    }

    /* The stages' own buffers are charged before the op is submitted, as
     * the sync call's (audit 17). The worker wires each stage's slice of
     * op->buffers and its uniforms; a copy of the stages, wired the same
     * way, is what gets counted here. */
    {
        HlGpuPipelineStage cs[HL_GPU_MAX_PIPELINE_STAGES];
        int off = 0;
        for (int s = 0; s < op->stage_count; s++) {
            cs[s] = op->stages[s];
            int bc = cs[s].buffer_count;
            if (bc < 0) bc = 0;
            if (bc > op->buffer_count - off) bc = op->buffer_count - off;
            cs[s].buffer_count = bc;
            cs[s].buffers = bc > 0 ? &op->buffers[off] : NULL;
            off += bc;
            cs[s].uniforms = op->stage_uniforms ? op->stage_uniforms[s] : NULL;
        }
        HlGpuPipelineOpts c = { .stages = cs, .stage_count = op->stage_count };
        if (hl_js_budget_charge(ctx, hl_work_bulk_units(
                                    (uint64_t)hl_gpu_pipeline_charge_bytes(&c)))) {
            hl_worker_gpu_op_free(op); free(op);
            return JS_EXCEPTION;
        }
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
    actx->detached = (hl_js_cont_suspend_conn(cont) == NULL);

    op->async_ctx = actx;
    atomic_store(&op->cancelled, 0);

    if (hl_worker_gpu_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "gpu.async.pipeline: thread pool full");
    }

    if (!actx->detached &&
        hl_js_op_suspend(js, (HlSuspendOp *)&actx->op) < 0) {
        atomic_store(&op->cancelled, 1);
        /* Never armed: destroy frees resolve / reject. cancel() would also
         * end the still-running handler's request life. */
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
                      JS_NewCFunction(ctx, js_gpu_load, "load", 2));
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
#ifdef HL_ENABLE_IMAGE
    JS_SetPropertyStr(ctx, gpu, "textureRead",
                      JS_NewCFunction(ctx, js_gpu_texture_read, "textureRead", 1));
#endif

    /* gpu.async sub-object */
    JSValue async_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, async_obj, "dispatch",
                      JS_NewCFunction(ctx, js_gpu_async_dispatch, "dispatch", 2));
    JS_SetPropertyStr(ctx, async_obj, "pipeline",
                      JS_NewCFunction(ctx, js_gpu_async_pipeline, "pipeline", 2));
    JS_SetPropertyStr(ctx, gpu, "async", async_obj);

    hl_js_export_frozen(ctx, m, "gpu", gpu);
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
