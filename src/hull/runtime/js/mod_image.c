/*
 * mod_image.c — hull:image module (QuickJS bindings)
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

/* ── JS class for HlImage ──────────────────────────────────────────── */

/* js_image_class_id is defined in mod_buffer.c (extern in mod_buffer.h) */

static void js_image_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlImage *img = JS_GetOpaque(val, js_image_class_id);
    if (img) {
        hl_image_free(img);
    }
}

static JSClassDef js_image_class = {
    "HlImage",
    .finalizer = js_image_finalizer,
};

/* ── Property getters ──────────────────────────────────────────────── */

static JSValue js_image_get_width(JSContext *ctx, JSValueConst this_val)
{
    HlImage *img = JS_GetOpaque2(ctx, this_val, js_image_class_id);
    if (!img) return JS_ThrowInternalError(ctx, "image already closed");
    return JS_NewUint32(ctx, img->width);
}

static JSValue js_image_get_height(JSContext *ctx, JSValueConst this_val)
{
    HlImage *img = JS_GetOpaque2(ctx, this_val, js_image_class_id);
    if (!img) return JS_ThrowInternalError(ctx, "image already closed");
    return JS_NewUint32(ctx, img->height);
}

static JSValue js_image_get_format(JSContext *ctx, JSValueConst this_val)
{
    HlImage *img = JS_GetOpaque2(ctx, this_val, js_image_class_id);
    if (!img) return JS_ThrowInternalError(ctx, "image already closed");
    return JS_NewString(ctx, hl_image_format_name(img->format));
}

static JSValue js_image_get_size(JSContext *ctx, JSValueConst this_val)
{
    HlImage *img = JS_GetOpaque2(ctx, this_val, js_image_class_id);
    if (!img) return JS_ThrowInternalError(ctx, "image already closed");
    return JS_NewInt64(ctx, (int64_t)img->pixel_len);
}

/* ── Methods ───────────────────────────────────────────────────────── */

static JSValue js_image_pixels(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlImage *img = JS_GetOpaque2(ctx, this_val, js_image_class_id);
    if (!img) return JS_ThrowInternalError(ctx, "image already closed");

    /* Return a copy as ArrayBuffer */
    return JS_NewArrayBufferCopy(ctx, (const uint8_t *)img->pixels,
                                  img->pixel_len);
}

static JSValue js_image_close(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlImage *img = JS_GetOpaque2(ctx, this_val, js_image_class_id);
    if (img) {
        hl_image_free(img);
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

/* ── Helper: wrap HlImage* as JS object ────────────────────────────── */

static JSValue js_wrap_image(JSContext *ctx, HlImage *img)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)js_image_class_id);
    if (JS_IsException(obj)) {
        hl_image_free(img);
        return obj;
    }
    JS_SetOpaque(obj, img);
    return obj;
}

/* ── Module functions ──────────────────────────────────────────────── */

/* image.new(w, h, format, data) */
static JSValue js_image_new(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "image.new requires (w, h, format, data)");

    uint32_t w, h;
    if (JS_ToUint32(ctx, &w, argv[0])) return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &h, argv[1])) return JS_EXCEPTION;

    const char *fmt_name = JS_ToCString(ctx, argv[2]);
    if (!fmt_name) return JS_EXCEPTION;

    int fmt = hl_image_format_from_name(fmt_name);
    if (fmt < 0) {
        JSValue err = JS_ThrowTypeError(ctx, "unknown image format: %s", fmt_name);
        JS_FreeCString(ctx, fmt_name);
        return err;
    }
    JS_FreeCString(ctx, fmt_name);

    HlBufferView view;
    const char *str_out;
    int needs_free;
    if (!js_get_buffer(ctx, argv[3], &view, &str_out, &needs_free))
        return JS_ThrowTypeError(ctx, "image.new: arg 4 must be a buffer");

    HlImage *img = hl_image_new(w, h, (HlImageFormat)fmt,
                                 view.data, view.len, NULL);
    if (needs_free) JS_FreeCString(ctx, str_out);

    if (!img)
        return JS_ThrowRangeError(ctx, "image.new: invalid dimensions or data size");

    return js_wrap_image(ctx, img);
}

/* image.fromBuffer(data, w, h, format) */
static JSValue js_image_from_buffer(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "image.fromBuffer requires (data, w, h, format)");

    HlBufferView view;
    const char *str_out;
    int needs_free;
    if (!js_get_buffer(ctx, argv[0], &view, &str_out, &needs_free))
        return JS_ThrowTypeError(ctx, "image.fromBuffer: arg 1 must be a buffer");

    uint32_t w, h;
    if (JS_ToUint32(ctx, &w, argv[1])) {
        if (needs_free) JS_FreeCString(ctx, str_out);
        return JS_EXCEPTION;
    }
    if (JS_ToUint32(ctx, &h, argv[2])) {
        if (needs_free) JS_FreeCString(ctx, str_out);
        return JS_EXCEPTION;
    }

    const char *fmt_name = JS_ToCString(ctx, argv[3]);
    if (!fmt_name) {
        if (needs_free) JS_FreeCString(ctx, str_out);
        return JS_EXCEPTION;
    }

    int fmt = hl_image_format_from_name(fmt_name);
    JS_FreeCString(ctx, fmt_name);

    if (fmt < 0) {
        if (needs_free) JS_FreeCString(ctx, str_out);
        return JS_ThrowTypeError(ctx, "unknown image format");
    }

    /* Borrow refcountable zero-copy sources (mmap / WASM buffer) with deferred
     * source teardown; copy everything else (string, ArrayBuffer, TypedArray,
     * image) since there is no stable refcountable object to pin. */
    HlMappedBuffer *mmap_src = JS_GetOpaque(argv[0], js_mmap_class_id);
    if (mmap_src && mmap_src->closed) mmap_src = NULL;
#ifdef HL_ENABLE_WASM
    HlWasmBuffer *wasm_src = JS_GetOpaque(argv[0], js_wasm_buf_class_id);
    if (wasm_src && wasm_src->closed) wasm_src = NULL;
#endif

    HlImage *img;
    if (mmap_src) {
        img = hl_image_from_view(w, h, (HlImageFormat)fmt,
                                 view.data, view.len, NULL);
        if (img) {
            hl_cap_fs_mmap_borrow(mmap_src);
            img->on_free = hl_cap_fs_mmap_release;
            img->on_free_ctx = mmap_src;
        }
    }
#ifdef HL_ENABLE_WASM
    else if (wasm_src) {
        img = hl_image_from_view(w, h, (HlImageFormat)fmt,
                                 view.data, view.len, NULL);
        if (img) {
            hl_wasm_buffer_borrow(wasm_src);
            img->on_free = hl_wasm_buffer_release;
            img->on_free_ctx = wasm_src;
        }
    }
#endif
    else {
        img = hl_image_new(w, h, (HlImageFormat)fmt, view.data, view.len, NULL);
    }
    if (needs_free) JS_FreeCString(ctx, str_out);

    if (!img)
        return JS_ThrowRangeError(ctx, "image.fromBuffer: invalid dimensions or data size");

    return js_wrap_image(ctx, img);
}

/* image.decode(data, format?) */
static JSValue js_image_decode_fn(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "image.decode requires data");

    HlBufferView view;
    const char *str_out;
    int needs_free;
    if (!js_get_buffer(ctx, argv[0], &view, &str_out, &needs_free))
        return JS_ThrowTypeError(ctx, "image.decode: arg 1 must be a buffer");

    const char *fmt_name = NULL;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        fmt_name = JS_ToCString(ctx, argv[1]);
        if (!fmt_name) {
            if (needs_free) JS_FreeCString(ctx, str_out);
            return JS_EXCEPTION;
        }
    }

    const char *err_msg = NULL;
    HlImage *img = hl_image_decode(view.data, view.len,
                                    fmt_name, NULL, &err_msg);
    if (needs_free) JS_FreeCString(ctx, str_out);
    if (fmt_name) JS_FreeCString(ctx, fmt_name);

    if (!img)
        return JS_ThrowInternalError(ctx, "image.decode: %s",
                                      err_msg ? err_msg : "decode failed");

    return js_wrap_image(ctx, img);
}

/* image.encode(img, format, opts?) */
static JSValue js_image_encode_fn(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "image.encode requires (img, format)");

    HlImage *img = JS_GetOpaque2(ctx, argv[0], js_image_class_id);
    if (!img) return JS_ThrowTypeError(ctx, "image.encode: arg 1 must be an Image");

    const char *fmt_name = JS_ToCString(ctx, argv[1]);
    if (!fmt_name) return JS_EXCEPTION;

    int quality = 90;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue qval = JS_GetPropertyStr(ctx, argv[2], "quality");
        if (JS_IsNumber(qval)) {
            int32_t q;
            JS_ToInt32(ctx, &q, qval);
            quality = q;
        }
        JS_FreeValue(ctx, qval);
    }

    void *out = NULL;
    size_t out_len = 0;
    const char *err_msg = NULL;

    int rc = hl_image_encode(img, fmt_name, quality,
                              &out, &out_len, NULL, &err_msg);
    JS_FreeCString(ctx, fmt_name);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "image.encode: %s",
                                      err_msg ? err_msg : "encode failed");

    /* Return as ArrayBuffer */
    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)out, out_len);
    free(out);
    return ab;
}

/* ── Module registration ───────────────────────────────────────────── */

static const JSCFunctionListEntry js_image_proto[] = {
    JS_CGETSET_DEF("width",  js_image_get_width,  NULL),
    JS_CGETSET_DEF("height", js_image_get_height, NULL),
    JS_CGETSET_DEF("format", js_image_get_format, NULL),
    JS_CGETSET_DEF("size",   js_image_get_size,   NULL),
    JS_CFUNC_DEF("pixels", 0, js_image_pixels),
    JS_CFUNC_DEF("close",  0, js_image_close),
};

/* image.toWasm(img) → ArrayBuffer: [w:u32 LE][h:u32 LE][fmt:u8][pixels...] */
static JSValue js_image_to_wasm(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "image.toWasm: image required");

    HlImage *img = JS_GetOpaque(argv[0], js_image_class_id);
    if (!img) return JS_ThrowTypeError(ctx, "image.toWasm: not an HlImage");

    size_t hdr = 9;
    if (img->pixel_len > SIZE_MAX - hdr)
        return JS_ThrowRangeError(ctx, "image.toWasm: image too large");
    size_t total = hdr + img->pixel_len;
    uint8_t *buf = js_malloc(ctx, total);
    if (!buf) return JS_EXCEPTION;

    buf[0] = img->width & 0xFF;
    buf[1] = (img->width >> 8) & 0xFF;
    buf[2] = (img->width >> 16) & 0xFF;
    buf[3] = (img->width >> 24) & 0xFF;
    buf[4] = img->height & 0xFF;
    buf[5] = (img->height >> 8) & 0xFF;
    buf[6] = (img->height >> 16) & 0xFF;
    buf[7] = (img->height >> 24) & 0xFF;
    buf[8] = (uint8_t)(img->format + 1);
    memcpy(buf + hdr, img->pixels, img->pixel_len);

    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, total);
    js_free(ctx, buf);
    return ab;
}

/* image.fromWasm(arrayBuffer) → HlImage */
static JSValue js_image_from_wasm(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "image.fromWasm: data required");

    size_t len;
    const uint8_t *data = JS_GetArrayBuffer(ctx, &len, argv[0]);
    const char *str_data = NULL;
    if (!data) {
        /* Try string */
        str_data = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!str_data) return JS_ThrowTypeError(ctx, "image.fromWasm: ArrayBuffer or string required");
        data = (const uint8_t *)str_data;
    }

    if (len < 9) {
        if (str_data) JS_FreeCString(ctx, str_data);
        return JS_ThrowRangeError(ctx, "image.fromWasm: data too short");
    }

    uint32_t w = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                 ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    uint32_t h = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                 ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    int fmt_byte = data[8];
    if (fmt_byte < 1 || fmt_byte > 4) {
        if (str_data) JS_FreeCString(ctx, str_data);
        return JS_ThrowRangeError(ctx, "image.fromWasm: invalid format byte");
    }

    HlImageFormat fmt = (HlImageFormat)(fmt_byte - 1);
    int bpp = hl_image_bpp(fmt);
    /* Overflow-safe size check: w * h * bpp */
    if (w > HL_IMAGE_MAX_DIM || h > HL_IMAGE_MAX_DIM ||
        (w > 0 && h > SIZE_MAX / 2 / (size_t)w / (size_t)bpp)) {
        if (str_data) JS_FreeCString(ctx, str_data);
        return JS_ThrowRangeError(ctx, "image.fromWasm: dimensions overflow");
    }
    size_t expected = (size_t)w * (size_t)h * (size_t)bpp;
    size_t pixel_len = len - 9;
    if (pixel_len < expected) {
        if (str_data) JS_FreeCString(ctx, str_data);
        return JS_ThrowRangeError(ctx, "image.fromWasm: pixel data too short");
    }

    HlImage *img = hl_image_new(w, h, fmt, data + 9, expected, NULL);
    if (str_data) JS_FreeCString(ctx, str_data);
    if (!img) return JS_ThrowInternalError(ctx, "image.fromWasm: create failed");

    JSValue obj = JS_NewObjectClass(ctx, js_image_class_id);
    if (JS_IsException(obj)) {
        hl_image_free(img);
        return obj;
    }
    JS_SetOpaque(obj, img);
    return obj;
}

/* Init callback invoked by QuickJS the first time `import "hull:image"`
 * resolves. The gate fires here so an undeclared import throws cleanly. */
static int js_image_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/image", "hull:image") != 0)
        return -1;

    JSValue image_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, image_obj, "new",
                      JS_NewCFunction(ctx, js_image_new, "new", 4));
    JS_SetPropertyStr(ctx, image_obj, "fromBuffer",
                      JS_NewCFunction(ctx, js_image_from_buffer, "fromBuffer", 4));
    JS_SetPropertyStr(ctx, image_obj, "decode",
                      JS_NewCFunction(ctx, js_image_decode_fn, "decode", 2));
    JS_SetPropertyStr(ctx, image_obj, "encode",
                      JS_NewCFunction(ctx, js_image_encode_fn, "encode", 3));
    JS_SetPropertyStr(ctx, image_obj, "toWasm",
                      JS_NewCFunction(ctx, js_image_to_wasm, "toWasm", 1));
    JS_SetPropertyStr(ctx, image_obj, "fromWasm",
                      JS_NewCFunction(ctx, js_image_from_wasm, "fromWasm", 1));

    JS_SetModuleExport(ctx, m, "image", image_obj);
    return 0;
}

int hl_js_init_image_module(JSContext *ctx, HlJS *js)
{
    (void)js;

    /* Register the HlImage JS class up-front. The class ID is shared
     * across mod_buffer (toWasm/fromWasm), so it must exist even before
     * any import — the init callback only runs on first import. */
    JS_NewClassID(&js_image_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_image_class_id, &js_image_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_image_proto,
                               sizeof(js_image_proto) / sizeof(js_image_proto[0]));
    JS_SetClassProto(ctx, js_image_class_id, proto);

    /* Register the "hull:image" module so `import { image } from "hull:image"`
     * resolves through the standard QuickJS module system. The init callback
     * above gates and populates the export on first import. */
    JSModuleDef *m = JS_NewCModule(ctx, "hull:image", js_image_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "image");

    return 0;
}
