/*
 * mod_fs.c — hull:fs module (filesystem capabilities, mmap)
 *
 * Note: In the original monolithic modules.c, fs was inside #ifdef HL_ENABLE_WASM
 * because it shared js_mmap_class_id with compute. Now the class ID is in
 * mod_buffer.c and available unconditionally.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/fs.h"
#include "hull/alloc.h"

static void js_mmap_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlMappedBuffer *buf = JS_GetOpaque(val, js_mmap_class_id);
    if (buf) hl_cap_fs_munmap(buf);
}

static JSClassDef js_mmap_class = {
    "MappedBuffer",
    .finalizer = js_mmap_finalizer,
};

static JSValue js_mmap_close(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlMappedBuffer *buf = JS_GetOpaque2(ctx, this_val, js_mmap_class_id);
    if (buf) {
        hl_cap_fs_munmap(buf);
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

static JSValue js_mmap_get_length(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlMappedBuffer *buf = JS_GetOpaque2(ctx, this_val, js_mmap_class_id);
    if (!buf || buf->closed) return JS_NewInt32(ctx, 0);
    return JS_NewInt64(ctx, (int64_t)buf->len);
}

static JSValue js_fs_mmap(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.fs_cfg)
        return JS_ThrowInternalError(ctx, "fs.mmap: not available (declare fs_read in manifest)");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fs.mmap requires (path)");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    HlMappedBuffer *buf = hl_cap_fs_mmap(js->base.fs_cfg, path, js->base.alloc);
    JS_FreeCString(ctx, path);

    if (!buf)
        return JS_ThrowInternalError(ctx, "fs.mmap: failed to map file");

    JSValue obj = JS_NewObjectClass(ctx, (int)js_mmap_class_id);
    if (JS_IsException(obj)) {
        hl_cap_fs_munmap(buf);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, buf);
    return obj;
}

static int js_fs_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue fs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs, "mmap",
                      JS_NewCFunction(ctx, js_fs_mmap, "mmap", 1));
    JS_SetModuleExport(ctx, m, "fs", fs);
    return 0;
}

int hl_js_init_fs_module(JSContext *ctx, HlJS *js)
{
    /* Register MappedBuffer class */
    JS_NewClassID(&js_mmap_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_mmap_class_id, &js_mmap_class);

    /* Set prototype with close() method and length getter */
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "close",
                      JS_NewCFunction(ctx, js_mmap_close, "close", 0));

    JSAtom length_atom = JS_NewAtom(ctx, "length");
    JS_DefinePropertyGetSet(ctx, proto, length_atom,
                            JS_NewCFunction(ctx, js_mmap_get_length, "length", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, length_atom);

    JS_SetClassProto(ctx, js_mmap_class_id, proto);

    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:fs", js_fs_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "fs");
    return 0;
}
