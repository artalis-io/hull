/*
 * mod_log.c - hull:log module (structured logging)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "log.h"

static JSValue js_log_level(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    static const int levels[] = { LOG_INFO, LOG_WARN, LOG_ERROR, LOG_DEBUG };
    int level = (magic >= 0 && magic < 4) ? levels[magic] : LOG_INFO;

    /* Detect stdlib vs app: hull:* modules -> [hull:js], else [app] */
    const char *tag = "[app]";
    const char *mod = NULL;
    JSAtom mod_atom = JS_GetScriptOrModuleName(ctx, 1);
    if (mod_atom != JS_ATOM_NULL) {
        mod = JS_AtomToCString(ctx, mod_atom);
        JS_FreeAtom(ctx, mod_atom);
    }
    if (mod && strncmp(mod, "hull:", 5) == 0)
        tag = "[hull:js]";

    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            log_log(level, mod ? mod : "js", 0, "%s %s", tag, str);
            JS_FreeCString(ctx, str);
        }
    }
    if (mod) JS_FreeCString(ctx, mod);
    return JS_UNDEFINED;
}

static int js_log_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/log", "hull:log") != 0) return -1;

    JSValue log = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, log, "info",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_log_level,
                             "info", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, log, "warn",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_log_level,
                             "warn", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, log, "error",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_log_level,
                             "error", 1, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, log, "debug",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_log_level,
                             "debug", 1, JS_CFUNC_generic_magic, 3));
    JS_SetModuleExport(ctx, m, "log", log);
    return 0;
}

int hl_js_init_log_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:log", js_log_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "log");
    return 0;
}
