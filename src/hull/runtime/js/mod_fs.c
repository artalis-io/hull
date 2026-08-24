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
#include "hull/utils/alloc.h"

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

/* fs.read(path) — returns an ArrayBuffer with the whole file contents.
 * Two-pass: first call hl_cap_fs_read with buf=NULL to learn the size,
 * then allocate + read. Throws on error to match Hull JS conventions
 * (callers can try/catch). */
static JSValue js_fs_read(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.fs_cfg)
        return JS_ThrowInternalError(ctx,
            "fs.read: not available (declare fs.read in manifest)");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fs.read requires (path)");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *err_msg = NULL;
    int64_t size = hl_cap_fs_read(js->base.fs_cfg, path, NULL, 0, &err_msg);
    if (size < 0) {
        JSValue exc = JS_ThrowInternalError(ctx, "fs.read: %s",
                                             err_msg ? err_msg : "read_failed");
        JS_FreeCString(ctx, path);
        return exc;
    }
    if (size == 0) {
        JS_FreeCString(ctx, path);
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
    }

    uint8_t *buf = hl_alloc_malloc(js->base.alloc, (size_t)size);
    if (!buf) {
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    int64_t got = hl_cap_fs_read(js->base.fs_cfg, path,
                                  (char *)buf, (size_t)size, &err_msg);
    if (got < 0) {
        JSValue exc = JS_ThrowInternalError(ctx, "fs.read: %s",
                                             err_msg ? err_msg : "read_failed");
        hl_alloc_free(js->base.alloc, buf, (size_t)size);
        JS_FreeCString(ctx, path);
        return exc;
    }
    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, (size_t)got);
    hl_alloc_free(js->base.alloc, buf, (size_t)size);
    JS_FreeCString(ctx, path);
    return ab;
}

/* fs.write(path, bytes) — accepts ArrayBuffer / TypedArray / string.
 * Throws on error; returns true on success. */
static JSValue js_fs_write(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.fs_cfg)
        return JS_ThrowInternalError(ctx,
            "fs.write: not available (declare fs.write in manifest)");
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "fs.write requires (path, bytes)");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    HlBufferView view = {0};
    const char *str = NULL;
    int needs_free = 0;
    if (!js_get_buffer(ctx, argv[1], &view, &str, &needs_free)) {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx,
            "fs.write: bytes must be an ArrayBuffer, TypedArray, or string");
    }

    const char *err_msg = NULL;
    int rc = hl_cap_fs_write(js->base.fs_cfg, path,
                              (const char *)view.data, view.len, &err_msg);
    if (needs_free && str) JS_FreeCString(ctx, str);
    JS_FreeCString(ctx, path);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "fs.write: %s",
                                     err_msg ? err_msg : "write_failed");
    return JS_TRUE;
}

/* Map the node-type enum to its stable string label (Lua/JS parity). */
static const char *js_fs_node_type_name(HlFsNodeType t)
{
    switch (t) {
    case HL_FS_NODE_FILE:    return "file";
    case HL_FS_NODE_DIR:     return "dir";
    case HL_FS_NODE_SYMLINK: return "symlink";
    default:                 return "other";
    }
}

/* fs.stat(path) -> { type, size, mode, mtime } | null.
 * Present -> an object; absent -> null (subsumes `exists`); error -> throw.
 * lstat semantics: a terminal symlink is type "symlink", never followed. */
static JSValue js_fs_stat(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.fs_cfg)
        return JS_ThrowInternalError(ctx,
            "fs.stat: not available (declare fs.read in manifest)");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fs.stat requires (path)");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *err = NULL;
    HlFsStatInfo info;
    int rc = hl_cap_fs_stat(js->base.fs_cfg, path, &info, &err);
    JS_FreeCString(ctx, path);
    if (rc == 1) return JS_NULL;                          /* absent */
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "fs.stat: %s", err ? err : "stat_failed");

    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) return o;
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, js_fs_node_type_name(info.type)));
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt64(ctx, (int64_t)info.size));
    JS_SetPropertyStr(ctx, o, "mode", JS_NewInt32(ctx, (int32_t)info.mode));
    JS_SetPropertyStr(ctx, o, "mtime", JS_NewInt64(ctx, (int64_t)info.mtime));
    return o;
}

/* fs.list(dir) -> Array<{ name, type, size }>. Deterministic byte-order
 * (unsigned-byte lexicographic, shorter first). Empty dir -> []; missing or
 * denied dir -> throw. */
static JSValue js_fs_list(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.fs_cfg)
        return JS_ThrowInternalError(ctx,
            "fs.list: not available (declare fs.read in manifest)");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fs.list requires (path)");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *err = NULL;
    HlFsDirEntry *entries = NULL;
    size_t count = 0;
    int rc = hl_cap_fs_list(js->base.fs_cfg, path, &entries, &count,
                            js->base.alloc, &err);
    JS_FreeCString(ctx, path);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "fs.list: %s", err ? err : "list_failed");

    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        hl_cap_fs_list_free(entries, count, js->base.alloc);
        return arr;
    }
    for (size_t i = 0; i < count; i++) {
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o)) {
            JS_FreeValue(ctx, arr);
            hl_cap_fs_list_free(entries, count, js->base.alloc);
            return JS_EXCEPTION;
        }
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, entries[i].name));
        JS_SetPropertyStr(ctx, o, "type",
                          JS_NewString(ctx, js_fs_node_type_name(entries[i].type)));
        JS_SetPropertyStr(ctx, o, "size", JS_NewInt64(ctx, (int64_t)entries[i].size));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
    }
    hl_cap_fs_list_free(entries, count, js->base.alloc);
    return arr;
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

    /* Optional second arg { offset, length } selects a windowed, page-aligned
     * mapping (mapped-spans). A bare path stays whole-file. */
    const char *err_msg = NULL;
    HlMappedBuffer *buf;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        int64_t off = 0, length = 0;
        JSValue vo = JS_GetPropertyStr(ctx, argv[1], "offset");
        if (!JS_IsUndefined(vo) && JS_ToInt64(ctx, &off, vo) < 0) {
            JS_FreeValue(ctx, vo); JS_FreeCString(ctx, path);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, vo);
        JSValue vl = JS_GetPropertyStr(ctx, argv[1], "length");
        if (JS_IsUndefined(vl)) {
            JS_FreeCString(ctx, path);
            return JS_ThrowTypeError(ctx, "fs.mmap: window requires a length");
        }
        if (JS_ToInt64(ctx, &length, vl) < 0) {
            JS_FreeValue(ctx, vl); JS_FreeCString(ctx, path);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, vl);
        if (off < 0 || length <= 0) {
            JS_FreeCString(ctx, path);
            return JS_ThrowRangeError(ctx,
                "fs.mmap: offset must be >= 0 and length > 0");
        }
        buf = hl_cap_fs_mmap_window(js->base.fs_cfg, path,
                                    (uint64_t)off, (uint64_t)length,
                                    js->base.alloc, &err_msg);
    } else {
        buf = hl_cap_fs_mmap(js->base.fs_cfg, path, js->base.alloc, &err_msg);
    }
    JS_FreeCString(ctx, path);

    if (!buf)
        return JS_ThrowInternalError(ctx, "fs.mmap: %s",
                                     err_msg ? err_msg : "failed to map file");

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
    if (hl_js_check_module_declared(ctx, "hull/fs", "hull:fs") != 0) return -1;

    JSValue fs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs, "read",
                      JS_NewCFunction(ctx, js_fs_read, "read", 1));
    JS_SetPropertyStr(ctx, fs, "write",
                      JS_NewCFunction(ctx, js_fs_write, "write", 2));
    JS_SetPropertyStr(ctx, fs, "stat",
                      JS_NewCFunction(ctx, js_fs_stat, "stat", 1));
    JS_SetPropertyStr(ctx, fs, "list",
                      JS_NewCFunction(ctx, js_fs_list, "list", 1));
    JS_SetPropertyStr(ctx, fs, "mmap",
                      JS_NewCFunction(ctx, js_fs_mmap, "mmap", 2));
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
