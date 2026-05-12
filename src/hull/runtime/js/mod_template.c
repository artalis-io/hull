/*
 * mod_template.c — hull:_template module (compile + loadRaw bridge)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/vfs.h"
#include "hull/limits.h"

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

    /* Evaluate the IIFE source — returns a function */
    JSValue result = JS_Eval(ctx, code, len, name,
                              JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);

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

    /* 1. Search embedded template entries via VFS */
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (js && js->base.app_vfs) {
        char tpl_name[HL_MODULE_PATH_MAX];
        int tpl_n = snprintf(tpl_name, sizeof(tpl_name), "templates/%s", name);
        if (tpl_n > 0 && (size_t)tpl_n < sizeof(tpl_name)) {
            const HlEntry *e = hl_vfs_find(js->base.app_vfs, tpl_name);
            if (e) {
                JSValue result = JS_NewStringLen(ctx, (const char *)e->data, e->len);
                JS_FreeCString(ctx, name);
                return result;
            }
        }
    }

    /* 2. Filesystem fallback (dev mode): app_dir/templates/<name> */
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
            /* Verify resolved path stays within app_dir (symlink escape check) */
            char resolved[PATH_MAX];
            if (realpath(path, resolved)) {
                size_t app_dir_len = strlen(js->app_dir);
                if (strncmp(resolved, js->app_dir, app_dir_len) != 0 ||
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
