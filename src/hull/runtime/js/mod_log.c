/*
 * mod_log.c - hull:log module (structured logging)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "log.h"
#include "internal.h"   /* hl_js_export_frozen (audit 18) */
#include "hull/shared/work_charge.h"   /* hl_work_bytes_units */

/* Write each argument as one log record: converted (a toString / valueOf
 * that throws fails the call - it was skipped and the call returned with
 * the exception pending), charged 1 unit per 8 bytes before the write (log
 * output was free), and with CR and LF written as the two characters \r
 * and \n, so app text cannot start a forged log line (audit 24). Shared by
 * hull:log and the console polyfill. Returns 0, or -1 with an exception
 * pending. */
int hl_js_log_args(JSContext *ctx, int level, const char *file,
                   const char *tag, int argc, JSValueConst *argv)
{
    /* The call, then every argument is a record of its own (audit 41: the
     * bytes alone were charged, so log.info("") - a timestamp, an escape
     * pass and an unbuffered stderr write - was free, and an 8-argument
     * console.log loop ran ~590 s per budget). */
    if (hl_js_budget_charge(ctx, HL_WORK_LOG_CALL_UNITS) < 0) return -1;
    for (int i = 0; i < argc; i++) {
        size_t len = 0;
        const char *str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!str) return -1;
        if (hl_js_budget_charge(ctx, hl_work_log_record_units(len)) < 0) {
            JS_FreeCString(ctx, str);
            return -1;
        }
        size_t extra = 0;
        for (size_t k = 0; k < len; k++)
            if (str[k] == '\r' || str[k] == '\n') extra++;
        if (extra == 0) {
            log_log(level, file, 0, "%s %s", tag, str);
            JS_FreeCString(ctx, str);
            continue;
        }
        char *esc = (len > SIZE_MAX - extra - 1) ? NULL
                  : js_malloc(ctx, len + extra + 1);
        if (!esc) {
            JS_FreeCString(ctx, str);
            if (!JS_HasException(ctx)) JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        size_t o = 0;
        for (size_t k = 0; k < len; k++) {
            char c = str[k];
            if (c == '\r' || c == '\n') {
                esc[o++] = '\\';
                esc[o++] = c == '\r' ? 'r' : 'n';
            } else {
                esc[o++] = c;
            }
        }
        esc[o] = '\0';
        log_log(level, file, 0, "%s %s", tag, esc);
        js_free(ctx, esc);
        JS_FreeCString(ctx, str);
    }
    return 0;
}

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

    int rc = hl_js_log_args(ctx, level, mod ? mod : "js", tag, argc, argv);
    if (mod) JS_FreeCString(ctx, mod);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
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
    hl_js_export_frozen(ctx, m, "log", log);
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
