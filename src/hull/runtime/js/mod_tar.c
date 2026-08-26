/* mod_tar.c - hull:tar module (QuickJS bindings)
 *
 * Parallel to the Lua binding (src/hull/runtime/lua/mod_tar.c). Thin wrappers
 * over the shared ustar core (cap/tar.h). parse/create are pure byte<->object
 * transforms accepting any buffer type; extract/pack compose the fs capability
 * (hl_cap_fs_*) so app archive I/O stays inside the manifest fs sandbox.
 *
 *   tar.parse(bytes)          -> [ {name,data,size,mode,isDir}, ... ]
 *   tar.create(entries)       -> ArrayBuffer
 *   tar.extract(bytes, dir)   -> true               (writes via fs.write)
 *   tar.pack(files [, opts])  -> ArrayBuffer          (reads via fs.read)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/tar.h"
#include "hull/cap/fs.h"

#include <stdlib.h>
#include <string.h>

/* ── tar.parse ─────────────────────────────────────────────────────── */

struct js_parse_ctx {
    JSContext *ctx;
    JSValue    arr;
    uint32_t   n;
    int        oom;
};

static int js_parse_collect(const HlTarEntry *e, void *vctx)
{
    struct js_parse_ctx *c = (struct js_parse_ctx *)vctx;
    JSContext *ctx = c->ctx;

    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) { c->oom = 1; return -1; }
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e->name));
    JS_SetPropertyStr(ctx, o, "data",
                      JS_NewArrayBufferCopy(ctx, e->data ? e->data : (const uint8_t *)"",
                                            e->size));
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt64(ctx, (int64_t)e->size));
    JS_SetPropertyStr(ctx, o, "mode", JS_NewInt32(ctx, (int32_t)e->mode));
    JS_SetPropertyStr(ctx, o, "isDir", JS_NewBool(ctx, e->is_dir));
    JS_SetPropertyUint32(ctx, c->arr, c->n++, o);
    return 0;
}

static JSValue js_tar_parse(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBufferView view;
    const char *str = NULL;
    int needs_free = 0;
    if (argc < 1 || !js_get_buffer(ctx, argv[0], &view, &str, &needs_free))
        return JS_ThrowTypeError(ctx, "tar.parse: arg 1 must be a buffer");

    JSValue arr = JS_NewArray(ctx);
    struct js_parse_ctx c = { ctx, arr, 0, 0 };
    int rc = hl_tar_parse((const unsigned char *)view.data, view.len,
                          js_parse_collect, &c);
    if (needs_free) JS_FreeCString(ctx, str);

    if (rc != 0) {
        JS_FreeValue(ctx, arr);
        if (c.oom) return JS_EXCEPTION;
        return JS_ThrowTypeError(ctx, "tar.parse: malformed or unsafe archive");
    }
    return arr;
}

/* ── tar.create ────────────────────────────────────────────────────── */

static JSValue js_tar_create(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "tar.create: arg 1 must be an array of entries");

    int64_t n = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToInt64(ctx, &n, lenv);
    JS_FreeValue(ctx, lenv);
    if (n < 0) return JS_ThrowTypeError(ctx, "tar.create: bad length");

    HlTarEntry *ents = n ? (HlTarEntry *)calloc((size_t)n, sizeof(HlTarEntry)) : NULL;
    /* Keep each entry's name CString + data buffer alive until create returns. */
    const char **names = n ? (const char **)calloc((size_t)n, sizeof(char *)) : NULL;
    HlBufferView *views = n ? (HlBufferView *)calloc((size_t)n, sizeof(HlBufferView)) : NULL;
    const char **dstrs = n ? (const char **)calloc((size_t)n, sizeof(char *)) : NULL;
    int *dfree = n ? (int *)calloc((size_t)n, sizeof(int)) : NULL;
    if (n && (!ents || !names || !views || !dstrs || !dfree)) {
        free(ents); free(names); free(views); free(dstrs); free(dfree);
        return JS_ThrowOutOfMemory(ctx);
    }

    const char *err = NULL;
    int64_t i;
    for (i = 0; i < n; i++) {
        JSValue ent = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        if (!JS_IsObject(ent)) { JS_FreeValue(ctx, ent); err = "each entry must be an object"; break; }

        JSValue nv = JS_GetPropertyStr(ctx, ent, "name");
        names[i] = JS_ToCString(ctx, nv);
        JS_FreeValue(ctx, nv);
        if (!names[i]) { JS_FreeValue(ctx, ent); err = "entry missing 'name'"; break; }
        ents[i].name = names[i];

        JSValue dv = JS_GetPropertyStr(ctx, ent, "isDir");
        ents[i].is_dir = JS_ToBool(ctx, dv);
        JS_FreeValue(ctx, dv);

        JSValue mv = JS_GetPropertyStr(ctx, ent, "mode");
        int32_t mode = 0;
        if (!JS_IsUndefined(mv)) JS_ToInt32(ctx, &mode, mv);
        JS_FreeValue(ctx, mv);
        ents[i].mode = (unsigned)mode;

        if (!ents[i].is_dir) {
            JSValue data = JS_GetPropertyStr(ctx, ent, "data");
            if (!JS_IsUndefined(data) && !JS_IsNull(data)) {
                if (!js_get_buffer(ctx, data, &views[i], &dstrs[i], &dfree[i])) {
                    JS_FreeValue(ctx, data); JS_FreeValue(ctx, ent);
                    err = "entry 'data' must be a buffer"; break;
                }
                ents[i].data = (const unsigned char *)views[i].data;
                ents[i].size = views[i].len;
            }
            JS_FreeValue(ctx, data);
        }
        JS_FreeValue(ctx, ent);
    }

    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc = -1;
    if (!err && i == n)
        rc = hl_tar_create(ents, (size_t)n, &out, &out_len);

    for (int64_t j = 0; j < n; j++) {
        if (names && names[j]) JS_FreeCString(ctx, names[j]);
        if (dfree && dfree[j] && dstrs[j]) JS_FreeCString(ctx, dstrs[j]);
    }
    free(ents); free(names); free(views); free(dstrs); free(dfree);

    if (rc != 0) {
        return err ? JS_ThrowTypeError(ctx, "tar.create: %s", err)
                   : JS_ThrowTypeError(ctx, "tar.create: unsafe name or out of memory");
    }
    JSValue ab = JS_NewArrayBufferCopy(ctx, out, out_len);
    free(out);
    return ab;
}

/* ── tar.extract (fs-composed, sandboxed) ──────────────────────────── */

struct js_extract_ctx {
    const HlFsConfig *fs;
    const char       *dir;
    const char       *err;
};

static int js_extract_write(const HlTarEntry *e, void *vctx)
{
    struct js_extract_ctx *c = (struct js_extract_ctx *)vctx;
    if (e->is_dir) return 0;

    char path[4096];
    int pn;
    if (c->dir && c->dir[0])
        pn = snprintf(path, sizeof(path), "%s/%s", c->dir, e->name);
    else
        pn = snprintf(path, sizeof(path), "%s", e->name);
    if (pn < 0 || (size_t)pn >= sizeof(path)) { c->err = "path too long"; return -1; }

    int rc = hl_cap_fs_write(c->fs, path,
                             (const char *)(e->data ? e->data : (const unsigned char *)""),
                             e->size, &c->err);
    return rc == 0 ? 0 : -1;
}

static JSValue js_tar_extract(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.fs_cfg)
        return JS_ThrowTypeError(ctx, "tar.extract: not available (declare fs.write in manifest)");

    HlBufferView view;
    const char *str = NULL;
    int needs_free = 0;
    if (argc < 1 || !js_get_buffer(ctx, argv[0], &view, &str, &needs_free))
        return JS_ThrowTypeError(ctx, "tar.extract: arg 1 must be a buffer");

    const char *dir = argc >= 2 ? JS_ToCString(ctx, argv[1]) : NULL;

    struct js_extract_ctx c = { js->base.fs_cfg, dir ? dir : "", NULL };
    int rc = hl_tar_parse((const unsigned char *)view.data, view.len,
                          js_extract_write, &c);

    if (dir) JS_FreeCString(ctx, dir);
    if (needs_free) JS_FreeCString(ctx, str);

    if (rc != 0)
        return JS_ThrowTypeError(ctx, "tar.extract: %s",
                                 c.err ? c.err : "malformed or unsafe archive");
    return JS_TRUE;
}

/* ── tar.pack (fs-composed, sandboxed) ─────────────────────────────── */

static JSValue js_tar_pack(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.fs_cfg)
        return JS_ThrowTypeError(ctx, "tar.pack: not available (declare fs.read in manifest)");
    if (argc < 1 || !JS_IsArray(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "tar.pack: arg 1 must be an array of files");

    int64_t n = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, argv[0], "length");
    JS_ToInt64(ctx, &n, lenv);
    JS_FreeValue(ctx, lenv);
    if (n <= 0) return JS_ThrowTypeError(ctx, "tar.pack: no files");

    HlTarEntry *ents = (HlTarEntry *)calloc((size_t)n, sizeof(HlTarEntry));
    char **bufs = (char **)calloc((size_t)n, sizeof(char *));
    /* Own the CStrings the whole loop: `paths` feeds fs.read, `names` feeds the
     * archive member name (NULL means "use paths[i]"). Both freed after create,
     * so ents[i].name stays valid through hl_tar_create. */
    const char **paths = (const char **)calloc((size_t)n, sizeof(char *));
    const char **names = (const char **)calloc((size_t)n, sizeof(char *));
    if (!ents || !bufs || !paths || !names) {
        free(ents); free(bufs); free(paths); free(names);
        return JS_ThrowOutOfMemory(ctx);
    }

    const char *err = NULL;
    int64_t i;
    for (i = 0; i < n; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        if (JS_IsString(el)) {
            paths[i] = JS_ToCString(ctx, el);       /* member name = path */
        } else if (JS_IsObject(el)) {
            JSValue pv = JS_GetPropertyStr(ctx, el, "path");
            paths[i] = JS_ToCString(ctx, pv);
            JS_FreeValue(ctx, pv);
            JSValue nv = JS_GetPropertyStr(ctx, el, "name");
            names[i] = JS_IsUndefined(nv) ? NULL : JS_ToCString(ctx, nv);
            JS_FreeValue(ctx, nv);
        }
        JS_FreeValue(ctx, el);
        if (!paths[i]) { err = "tar.pack: entry needs a path"; break; }

        int64_t size = hl_cap_fs_read(js->base.fs_cfg, paths[i], NULL, 0, &err);
        if (size < 0) break;
        char *buf = (char *)malloc(size ? (size_t)size : 1);
        if (!buf) { err = "out_of_memory"; break; }
        if (size > 0) {
            int64_t got = hl_cap_fs_read(js->base.fs_cfg, paths[i], buf, (size_t)size, &err);
            if (got < 0) { free(buf); break; }
        }
        bufs[i] = buf;
        ents[i].name = names[i] ? names[i] : paths[i];
        ents[i].data = (const unsigned char *)buf;
        ents[i].size = (size_t)size;
        ents[i].mode = 0644;
        ents[i].is_dir = 0;
    }

    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc = -1;
    if (!err && i == n)
        rc = hl_tar_create(ents, (size_t)n, &out, &out_len);

    for (int64_t j = 0; j < n; j++) {
        free(bufs[j]);
        if (paths[j]) JS_FreeCString(ctx, paths[j]);
        if (names[j]) JS_FreeCString(ctx, names[j]);
    }
    free(ents); free(bufs); free(paths); free(names);

    if (rc != 0)
        return JS_ThrowTypeError(ctx, "tar.pack: %s", err ? err : "failed");
    JSValue ab = JS_NewArrayBufferCopy(ctx, out, out_len);
    free(out);
    return ab;
}

/* ── Registration ──────────────────────────────────────────────────── */

static int js_tar_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/archive/tar", "hull:archive:tar") != 0)
        return -1;

    JSValue tar_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tar_obj, "parse",
                      JS_NewCFunction(ctx, js_tar_parse, "parse", 1));
    JS_SetPropertyStr(ctx, tar_obj, "create",
                      JS_NewCFunction(ctx, js_tar_create, "create", 1));
    JS_SetPropertyStr(ctx, tar_obj, "extract",
                      JS_NewCFunction(ctx, js_tar_extract, "extract", 2));
    JS_SetPropertyStr(ctx, tar_obj, "pack",
                      JS_NewCFunction(ctx, js_tar_pack, "pack", 2));

    JS_SetModuleExport(ctx, m, "tar", tar_obj);
    return 0;
}

int hl_js_init_tar_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:archive:tar", js_tar_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "tar");
    return 0;
}
