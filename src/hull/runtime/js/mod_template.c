/*
 * mod_template.c — hull:_template module (compile + loadRaw bridge)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/vfs.h"
#include "hull/limits/core.h"
#include "hull/runtime/js_template_cache.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* _template.compile(code, name?) — compile generated JS source to a function */
static JSValue js_template_compile(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "_template.compile requires (code)");

    size_t len;
    const char *code = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!code)
        return JS_EXCEPTION;

    const char *name = "<template>";
    if (argc >= 2 && JS_IsString(argv[1])) {
        name = JS_ToCString(ctx, argv[1]);
        if (!name) {
            JS_FreeCString(ctx, code);
            return JS_EXCEPTION;
        }
    }

    /* Compile + execute the IIFE through the on-disk template
     * cache — on hit the render function is deserialized via
     * JS_ReadObject and we skip both the parse pass AND the IIFE
     * execute that creates the closure. See
     * include/hull/runtime/js_template_cache.h. */
    JSValue result = hl_js_template_compile_cached(ctx, code, len, name);

    if (argc >= 2 && JS_IsString(argv[1]))
        JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, code);

    return result;
}

/* _template.loadRaw(name) — load raw template bytes from embedded
 * entries or filesystem fallback. Returns string or null. */
static JSValue js_template_load_raw(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "_template.loadRaw requires (name)");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    /* Quick-reject for both VFS and filesystem paths */
    if (name[0] == '/' || name[0] == '\0') {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "invalid template name");
    }

    /* 1. Search embedded app templates via app VFS */
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    char tpl_name[HL_MODULE_PATH_MAX];
    int tpl_n = snprintf(tpl_name, sizeof(tpl_name), "templates/%s", name);
    int tpl_ok = (tpl_n > 0 && (size_t)tpl_n < sizeof(tpl_name));
    if (tpl_ok && js && js->base.app_vfs) {
        const HlEntry *e = hl_vfs_find(js->base.app_vfs, tpl_name);
        if (e) {
            JSValue result = JS_NewStringLen(ctx, (const char *)e->data, e->len);
            JS_FreeCString(ctx, name);
            return result;
        }
    }

    /* 2. Stdlib platform VFS (widget partials shipped under
     *    templates/hull/<module>/<file>.html). App-shipped templates
     *    at the same path won in step 1 above; this is the fallback
     *    for stdlib-shipped widget partials. */
    if (tpl_ok && js && js->base.platform_vfs) {
        const HlEntry *e = hl_vfs_find(js->base.platform_vfs, tpl_name);
        if (e) {
            JSValue result = JS_NewStringLen(ctx, (const char *)e->data, e->len);
            JS_FreeCString(ctx, name);
            return result;
        }
    }

    /* 3. Filesystem fallback (dev mode): app_dir/templates/<name> */
    if (js && js->app_dir) {
        /* Reject ".." components to prevent path traversal */
        const char *p = name;
        while (*p) {
            if (p[0] == '.' && p[1] == '.' &&
                (p[2] == '/' || p[2] == '\0')) {
                JS_FreeCString(ctx, name);
                return JS_ThrowTypeError(ctx, "invalid template name");
            }
            const char *slash = strchr(p, '/');
            if (!slash) break;
            p = slash + 1;
        }

        char path[HL_MODULE_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/templates/%s",
                         js->app_dir, name);
        if (n > 0 && (size_t)n < sizeof(path)) {
            /* Verify resolved path stays within app_dir (symlink escape check).
             * Canonicalize app_dir too — it may be a relative path when
             * invoked as `hull test relative/path/`. */
            char resolved[PATH_MAX];
            if (realpath(path, resolved)) {
                char real_app_dir[PATH_MAX];
                if (!realpath(js->app_dir, real_app_dir)) {
                    JS_FreeCString(ctx, name);
                    return JS_ThrowTypeError(ctx, "invalid template name");
                }
                size_t app_dir_len = strlen(real_app_dir);
                if (strncmp(resolved, real_app_dir, app_dir_len) != 0 ||
                    (resolved[app_dir_len] != '/' && resolved[app_dir_len] != '\0')) {
                    JS_FreeCString(ctx, name);
                    return JS_ThrowTypeError(ctx, "invalid template name");
                }
            }

            FILE *f = fopen(path, "rb");
            if (f) {
                if (fseek(f, 0, SEEK_END) != 0) {
                    fclose(f);
                    JS_FreeCString(ctx, name);
                    return JS_ThrowInternalError(ctx, "seek failed: %s", path);
                }
                long size = ftell(f);
                if (size < 0 || size > 1048576) { /* 1 MB limit */
                    fclose(f);
                    JS_FreeCString(ctx, name);
                    return JS_ThrowInternalError(ctx, "template too large: %s", path);
                }
                if (fseek(f, 0, SEEK_SET) != 0) {
                    fclose(f);
                    JS_FreeCString(ctx, name);
                    return JS_ThrowInternalError(ctx, "seek failed: %s", path);
                }

                char *buf = js_malloc(ctx, (size_t)size);
                if (!buf) {
                    fclose(f);
                    JS_FreeCString(ctx, name);
                    return JS_EXCEPTION;
                }
                size_t nread = fread(buf, 1, (size_t)size, f);
                int read_err = ferror(f);
                fclose(f);

                if (read_err || nread != (size_t)size) {
                    js_free(ctx, buf);
                    JS_FreeCString(ctx, name);
                    return JS_ThrowInternalError(ctx, "read error: %s", path);
                }

                JSValue result = JS_NewStringLen(ctx, buf, nread);
                js_free(ctx, buf);
                JS_FreeCString(ctx, name);
                return result;
            }
        }
    }

    JS_FreeCString(ctx, name);
    return JS_NULL;
}

static int js_template_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue tpl = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tpl, "compile",
                      JS_NewCFunction(ctx, js_template_compile, "compile", 2));
    JS_SetPropertyStr(ctx, tpl, "loadRaw",
                      JS_NewCFunction(ctx, js_template_load_raw, "loadRaw", 1));
    JS_SetModuleExport(ctx, m, "_template", tpl);
    return 0;
}

int hl_js_init_template_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:_template", js_template_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "_template");
    return 0;
}
