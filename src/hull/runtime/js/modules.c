/*
 * js_modules.c — hull:* built-in module implementations for QuickJS
 *
 * Each module is registered as a native C module via JS_NewCModule().
 * All capability calls go through hl_cap_* — no direct SQLite,
 * filesystem, or network access from this file.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "hull/alloc.h"
#include "hull/limits.h"
#include "hull/cap/db.h"
#include "hull/cap/time.h"
#include "hull/cap/env.h"
#include "hull/cap/http.h"
#include "hull/cap/http_async.h"
#include "hull/async.h"
#include "hull/cap/smtp.h"
#include "hull/cap/crypto.h"
#include "hull/cap/fs.h"
#include "hull/worker_db.h"
#include "hull/worker_wasm.h"

#include <keel/server.h>
#include "quickjs.h"

#include "log.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Compiler-safe memory zeroing that won't be optimized away */
static void secure_zero(void *p, size_t n)
{
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--) *vp++ = 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:app module
 *
 * Provides route registration: app.get(), app.post(), app.use(), etc.
 * Routes are stored in globalThis.__hull_routes (array of functions)
 * and globalThis.__hull_route_defs (array of {method, pattern} objects)
 * for the C router to consume at startup.
 * ════════════════════════════════════════════════════════════════════ */

/* Helper: register a route with given method string */
static JSValue js_app_route(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    static const char *method_names[] = {
        "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "*"
    };

    if (magic < 0 || magic >= (int)(sizeof(method_names)/sizeof(method_names[0])))
        return JS_ThrowInternalError(ctx, "invalid route method index");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "app.%s requires (pattern, handler)",
                                 method_names[magic]);

    const char *pattern = JS_ToCString(ctx, argv[0]);
    if (!pattern)
        return JS_EXCEPTION;

    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, pattern);
        return JS_ThrowTypeError(ctx, "handler must be a function");
    }

    JSValue global = JS_GetGlobalObject(ctx);

    /* Ensure __hull_routes array exists */
    JSValue routes = JS_GetPropertyStr(ctx, global, "__hull_routes");
    if (JS_IsUndefined(routes)) {
        routes = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_routes", JS_DupValue(ctx, routes));
    }

    /* Ensure __hull_route_defs array exists */
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_route_defs");
    if (JS_IsUndefined(defs)) {
        defs = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_route_defs", JS_DupValue(ctx, defs));
    }

    /* Get current length (= next index) */
    JSValue len_val = JS_GetPropertyStr(ctx, routes, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    /* Store handler function */
    JS_SetPropertyUint32(ctx, routes, (uint32_t)idx, JS_DupValue(ctx, argv[1]));

    /* Store route definition */
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, def, "method",
                      JS_NewString(ctx, method_names[magic]));
    JS_SetPropertyStr(ctx, def, "pattern", JS_NewString(ctx, pattern));
    JS_SetPropertyStr(ctx, def, "handler_id", JS_NewInt32(ctx, idx));
    JS_SetPropertyUint32(ctx, defs, (uint32_t)idx, def);

    JS_FreeValue(ctx, defs);
    JS_FreeValue(ctx, routes);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, pattern);

    return JS_UNDEFINED;
}

/* app.use(method, pattern, handler) — middleware registration */
static JSValue js_app_use(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "app.use requires (method, pattern, handler)");

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *pattern = JS_ToCString(ctx, argv[1]);
    if (!method || !pattern || !JS_IsFunction(ctx, argv[2])) {
        if (method) JS_FreeCString(ctx, method);
        if (pattern) JS_FreeCString(ctx, pattern);
        return JS_ThrowTypeError(ctx, "app.use requires (method, pattern, handler)");
    }

    JSValue global = JS_GetGlobalObject(ctx);

    /* Store handler in __hull_routes (same array as route handlers) */
    JSValue routes = JS_GetPropertyStr(ctx, global, "__hull_routes");
    if (JS_IsUndefined(routes)) {
        routes = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_routes", JS_DupValue(ctx, routes));
    }

    JSValue routes_len_val = JS_GetPropertyStr(ctx, routes, "length");
    int32_t handler_id = 0;
    JS_ToInt32(ctx, &handler_id, routes_len_val);
    JS_FreeValue(ctx, routes_len_val);

    JS_SetPropertyUint32(ctx, routes, (uint32_t)handler_id,
                         JS_DupValue(ctx, argv[2]));
    JS_FreeValue(ctx, routes);

    /* Store in __hull_middleware array with handler_id */
    JSValue mw = JS_GetPropertyStr(ctx, global, "__hull_middleware");
    if (JS_IsUndefined(mw)) {
        mw = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_middleware", JS_DupValue(ctx, mw));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, mw, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "method", JS_NewString(ctx, method));
    JS_SetPropertyStr(ctx, entry, "pattern", JS_NewString(ctx, pattern));
    JS_SetPropertyStr(ctx, entry, "handler_id", JS_NewInt32(ctx, handler_id));
    JS_SetPropertyUint32(ctx, mw, (uint32_t)idx, entry);

    JS_FreeValue(ctx, mw);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, pattern);
    JS_FreeCString(ctx, method);

    return JS_UNDEFINED;
}

/* app.usePost(method, pattern, fn) — register post-body middleware */
static JSValue js_app_use_post(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "app.usePost requires (method, pattern, handler)");

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *pattern = JS_ToCString(ctx, argv[1]);
    if (!method || !pattern || !JS_IsFunction(ctx, argv[2])) {
        if (method) JS_FreeCString(ctx, method);
        if (pattern) JS_FreeCString(ctx, pattern);
        return JS_ThrowTypeError(ctx, "app.usePost requires (method, pattern, handler)");
    }

    JSValue global = JS_GetGlobalObject(ctx);

    /* Store handler in __hull_routes (same array as route handlers) */
    JSValue routes = JS_GetPropertyStr(ctx, global, "__hull_routes");
    if (JS_IsUndefined(routes)) {
        routes = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_routes", JS_DupValue(ctx, routes));
    }

    JSValue routes_len_val = JS_GetPropertyStr(ctx, routes, "length");
    int32_t handler_id = 0;
    JS_ToInt32(ctx, &handler_id, routes_len_val);
    JS_FreeValue(ctx, routes_len_val);

    JS_SetPropertyUint32(ctx, routes, (uint32_t)handler_id,
                         JS_DupValue(ctx, argv[2]));
    JS_FreeValue(ctx, routes);

    /* Store in __hull_post_middleware array with handler_id */
    JSValue mw = JS_GetPropertyStr(ctx, global, "__hull_post_middleware");
    if (JS_IsUndefined(mw)) {
        mw = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_post_middleware", JS_DupValue(ctx, mw));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, mw, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "method", JS_NewString(ctx, method));
    JS_SetPropertyStr(ctx, entry, "pattern", JS_NewString(ctx, pattern));
    JS_SetPropertyStr(ctx, entry, "handler_id", JS_NewInt32(ctx, handler_id));
    JS_SetPropertyUint32(ctx, mw, (uint32_t)idx, entry);

    JS_FreeValue(ctx, mw);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, pattern);
    JS_FreeCString(ctx, method);

    return JS_UNDEFINED;
}

/* app.every(interval_ms, handler) — repeating timer */
static JSValue js_app_every(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "app.every requires (interval_ms, handler)");

    int64_t interval_ms;
    if (JS_ToInt64(ctx, &interval_ms, argv[0]) != 0)
        return JS_EXCEPTION;

    if (interval_ms < 100)
        return JS_ThrowRangeError(ctx, "app.every() minimum interval is 100ms");

    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "app.every requires a function handler");

    JSValue global = JS_GetGlobalObject(ctx);

    /* Store handler in __hull_timers */
    JSValue timers = JS_GetPropertyStr(ctx, global, "__hull_timers");
    if (JS_IsUndefined(timers)) {
        timers = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_timers", JS_DupValue(ctx, timers));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, timers, "length");
    int32_t handler_id = 0;
    JS_ToInt32(ctx, &handler_id, len_val);
    JS_FreeValue(ctx, len_val);

    JS_SetPropertyUint32(ctx, timers, (uint32_t)handler_id,
                         JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, timers);

    /* Store timer def in __hull_timer_defs */
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_timer_defs");
    if (JS_IsUndefined(defs)) {
        defs = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_timer_defs", JS_DupValue(ctx, defs));
    }

    JSValue defs_len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, defs_len_val);
    JS_FreeValue(ctx, defs_len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "type", JS_NewString(ctx, "every"));
    JS_SetPropertyStr(ctx, entry, "interval_ms", JS_NewInt64(ctx, interval_ms));
    JS_SetPropertyStr(ctx, entry, "handler_id", JS_NewInt32(ctx, handler_id));
    JS_SetPropertyUint32(ctx, defs, (uint32_t)idx, entry);

    JS_FreeValue(ctx, defs);
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

/* app.daily(time_str, handler [, opts]) — daily timer at HH:MM */
static JSValue js_app_daily(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "app.daily requires (time_str, handler)");

    const char *time_str = JS_ToCString(ctx, argv[0]);
    if (!time_str)
        return JS_EXCEPTION;

    /* Character-level validation, no sscanf */
    int bad_fmt = (strlen(time_str) != 5 || time_str[2] != ':' ||
        time_str[0] < '0' || time_str[0] > '9' ||
        time_str[1] < '0' || time_str[1] > '9' ||
        time_str[3] < '0' || time_str[3] > '9' ||
        time_str[4] < '0' || time_str[4] > '9');
    int hour = bad_fmt ? -1 : (time_str[0] - '0') * 10 + (time_str[1] - '0');
    int minute = bad_fmt ? -1 : (time_str[3] - '0') * 10 + (time_str[4] - '0');
    if (bad_fmt || hour > 23 || minute > 59) {
        JS_FreeCString(ctx, time_str);
        return JS_ThrowRangeError(ctx, "app.daily() requires time in HH:MM format");
    }
    JS_FreeCString(ctx, time_str);

    if (!JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "app.daily requires a function handler");

    /* Check opts for localtime */
    int use_localtime = 0;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue lt_val = JS_GetPropertyStr(ctx, argv[2], "localtime");
        if (JS_IsBool(lt_val))
            use_localtime = JS_ToBool(ctx, lt_val);
        JS_FreeValue(ctx, lt_val);
    }

    JSValue global = JS_GetGlobalObject(ctx);

    /* Store handler in __hull_timers */
    JSValue timers = JS_GetPropertyStr(ctx, global, "__hull_timers");
    if (JS_IsUndefined(timers)) {
        timers = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_timers", JS_DupValue(ctx, timers));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, timers, "length");
    int32_t handler_id = 0;
    JS_ToInt32(ctx, &handler_id, len_val);
    JS_FreeValue(ctx, len_val);

    JS_SetPropertyUint32(ctx, timers, (uint32_t)handler_id,
                         JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, timers);

    /* Store timer def in __hull_timer_defs */
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_timer_defs");
    if (JS_IsUndefined(defs)) {
        defs = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_timer_defs", JS_DupValue(ctx, defs));
    }

    JSValue defs_len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, defs_len_val);
    JS_FreeValue(ctx, defs_len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "type", JS_NewString(ctx, "daily"));
    JS_SetPropertyStr(ctx, entry, "hour", JS_NewInt32(ctx, hour));
    JS_SetPropertyStr(ctx, entry, "minute", JS_NewInt32(ctx, minute));
    JS_SetPropertyStr(ctx, entry, "localtime", JS_NewBool(ctx, use_localtime));
    JS_SetPropertyStr(ctx, entry, "handler_id", JS_NewInt32(ctx, handler_id));
    JS_SetPropertyUint32(ctx, defs, (uint32_t)idx, entry);

    JS_FreeValue(ctx, defs);
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

/* app.config(obj) — application configuration */
static JSValue js_app_config(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "app.config requires an object");

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__hull_config", JS_DupValue(ctx, argv[0]));
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

/* app.manifest(obj) — declare application capabilities (one-shot) */
static JSValue js_app_manifest(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "app.manifest requires an object");

    /* Reject second call — manifest is immutable once declared */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue existing = JS_GetPropertyStr(ctx, global, "__hull_manifest");
    int already_set = !JS_IsUndefined(existing) && !JS_IsNull(existing);
    JS_FreeValue(ctx, existing);
    if (already_set) {
        JS_FreeValue(ctx, global);
        return JS_ThrowTypeError(ctx, "app.manifest() can only be called once");
    }

    JS_SetPropertyStr(ctx, global, "__hull_manifest", JS_DupValue(ctx, argv[0]));
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

/* app.getManifest() — retrieve the stored manifest object */
static JSValue js_app_get_manifest(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue manifest = JS_GetPropertyStr(ctx, global, "__hull_manifest");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(manifest))
        return JS_NULL;
    return manifest;
}

/* app.static(prefix, directory) — static file serving */
static JSValue js_app_static(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "app.static requires (prefix, directory)");

    /* Store static config for C router to process */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue statics = JS_GetPropertyStr(ctx, global, "__hull_statics");
    if (JS_IsUndefined(statics)) {
        statics = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_statics", JS_DupValue(ctx, statics));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, statics, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "prefix", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, entry, "directory", JS_DupValue(ctx, argv[1]));
    JS_SetPropertyUint32(ctx, statics, (uint32_t)idx, entry);

    JS_FreeValue(ctx, statics);
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

static int js_app_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue app = JS_NewObject(ctx);

    /* Route methods: magic encodes the HTTP method index */
    JS_SetPropertyStr(ctx, app, "get",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "get", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, app, "post",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "post", 2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, app, "put",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "put", 2, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, app, "del",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "del", 2, JS_CFUNC_generic_magic, 3));
    JS_SetPropertyStr(ctx, app, "patch",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "patch", 2, JS_CFUNC_generic_magic, 4));
    JS_SetPropertyStr(ctx, app, "options",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                             "options", 2, JS_CFUNC_generic_magic, 6));

    JS_SetPropertyStr(ctx, app, "use",
                      JS_NewCFunction(ctx, js_app_use, "use", 3));
    JS_SetPropertyStr(ctx, app, "usePost",
                      JS_NewCFunction(ctx, js_app_use_post, "usePost", 3));
    JS_SetPropertyStr(ctx, app, "every",
                      JS_NewCFunction(ctx, js_app_every, "every", 2));
    JS_SetPropertyStr(ctx, app, "daily",
                      JS_NewCFunction(ctx, js_app_daily, "daily", 3));
    JS_SetPropertyStr(ctx, app, "config",
                      JS_NewCFunction(ctx, js_app_config, "config", 1));
    JS_SetPropertyStr(ctx, app, "static",
                      JS_NewCFunction(ctx, js_app_static, "static", 2));
    JS_SetPropertyStr(ctx, app, "manifest",
                      JS_NewCFunction(ctx, js_app_manifest, "manifest", 1));
    JS_SetPropertyStr(ctx, app, "getManifest",
                      JS_NewCFunction(ctx, js_app_get_manifest, "getManifest", 0));

    JS_SetModuleExport(ctx, m, "app", app);
    return 0;
}

int hl_js_init_app_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:app", js_app_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "app");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:db module
 *
 * db.query(sql, params?) → array of row objects
 * db.exec(sql, params?)  → number of rows affected
 * db.lastId()            → last insert rowid
 * ════════════════════════════════════════════════════════════════════ */

/* Callback context for building JS result array from hl_cap_db_query */
typedef struct {
    JSContext *ctx;
    JSValue    array;
    int32_t    row_count;
} JsQueryCtx;

static int js_query_row_cb(void *opaque, HlColumn *cols, int ncols)
{
    JsQueryCtx *qc = (JsQueryCtx *)opaque;

    JSValue row = JS_NewObject(qc->ctx);
    for (int i = 0; i < ncols; i++) {
        JSValue val;
        switch (cols[i].value.type) {
        case HL_TYPE_INT:
            val = JS_NewInt64(qc->ctx, cols[i].value.i);
            break;
        case HL_TYPE_DOUBLE:
            val = JS_NewFloat64(qc->ctx, cols[i].value.d);
            break;
        case HL_TYPE_TEXT:
            val = JS_NewStringLen(qc->ctx, cols[i].value.s,
                                  cols[i].value.len);
            break;
        case HL_TYPE_BLOB:
            val = JS_NewArrayBufferCopy(qc->ctx,
                                         (const uint8_t *)cols[i].value.s,
                                         cols[i].value.len);
            break;
        case HL_TYPE_BOOL:
            val = JS_NewBool(qc->ctx, cols[i].value.b);
            break;
        case HL_TYPE_NIL:
        default:
            val = JS_NULL;
            break;
        }
        JS_SetPropertyStr(qc->ctx, row, cols[i].name, val);
    }

    JS_SetPropertyUint32(qc->ctx, qc->array, (uint32_t)qc->row_count, row);
    qc->row_count++;
    return 0;
}

/* Marshal JS values to HlValue array for parameter binding */
static int js_to_hl_values(JSContext *ctx, JSValueConst arr,
                              HlValue **out_params, int *out_count)
{
    *out_params = NULL;
    *out_count = 0;

    if (JS_IsUndefined(arr) || JS_IsNull(arr))
        return 0;

    if (!JS_IsArray(ctx, arr))
        return -1;

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    if (len <= 0)
        return 0;

    /* Overflow guard */
    if ((size_t)len > SIZE_MAX / sizeof(HlValue))
        return -1;

    HlValue *params = js_mallocz(ctx, (size_t)len * sizeof(HlValue));
    if (!params)
        return -1;

    for (int32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        int tag = JS_VALUE_GET_NORM_TAG(v);

        switch (tag) {
        case JS_TAG_INT:
            params[i].type = HL_TYPE_INT;
            params[i].i = JS_VALUE_GET_INT(v);
            break;
        case JS_TAG_FLOAT64: {
            double d;
            JS_ToFloat64(ctx, &d, v);
            params[i].type = HL_TYPE_DOUBLE;
            params[i].d = d;
            break;
        }
        case JS_TAG_STRING: {
            size_t slen;
            const char *s = JS_ToCStringLen(ctx, &slen, v);
            params[i].type = HL_TYPE_TEXT;
            params[i].s = s; /* kept alive until JS_FreeCString */
            params[i].len = slen;
            break;
        }
        case JS_TAG_BOOL:
            params[i].type = HL_TYPE_BOOL;
            params[i].b = JS_VALUE_GET_BOOL(v);
            break;
        case JS_TAG_NULL:
        case JS_TAG_UNDEFINED:
        default:
            params[i].type = HL_TYPE_NIL;
            break;
        }
        JS_FreeValue(ctx, v);
    }

    *out_params = params;
    *out_count = len;
    return 0;
}

static void js_free_hl_values(JSContext *ctx, HlValue *params, int count)
{
    if (!params)
        return;
    /* Free any strings we borrowed via JS_ToCStringLen */
    for (int i = 0; i < count; i++) {
        if (params[i].type == HL_TYPE_TEXT && params[i].s)
            JS_FreeCString(ctx, params[i].s);
    }
    js_free(ctx, params);
}

/* Check if the immediate JS caller is a stdlib module (module name starts
 * with "hull:").  User modules start with "./" — so a simple prefix check
 * is sufficient.  Returns 1 for stdlib, 0 for user code.
 *
 * n_stack_levels=1 skips the C function stack frame (level 0) to reach
 * the JS caller's frame. */
static int js_is_stdlib_caller(JSContext *ctx)
{
    JSAtom name = JS_GetScriptOrModuleName(ctx, 1);
    if (name == JS_ATOM_NULL)
        return 0;
    const char *str = JS_AtomToCString(ctx, name);
    JS_FreeAtom(ctx, name);
    if (!str)
        return 0;
    int is_stdlib = (strncmp(str, "hull:", 5) == 0);
    JS_FreeCString(ctx, str);
    return is_stdlib;
}

/* db.query implementation */
static JSValue js_db_query_impl(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.stmt_cache)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.query requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    if (!js_is_stdlib_caller(ctx) && hl_cap_db_check_namespace(sql) != 0) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

    HlValue *params = NULL;
    int nparams = 0;
    if (argc >= 2) {
        if (js_to_hl_values(ctx, argv[1], &params, &nparams) != 0) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "params must be an array");
        }
    }

    JsQueryCtx qc = {
        .ctx = ctx,
        .array = JS_NewArray(ctx),
        .row_count = 0,
    };

    int rc = hl_cap_db_query(js->base.stmt_cache, sql, params, nparams,
                               js_query_row_cb, &qc, js->base.alloc);

    js_free_hl_values(ctx, params, nparams);
    JS_FreeCString(ctx, sql);

    if (rc != 0) {
        JS_FreeValue(ctx, qc.array);
        return JS_ThrowInternalError(ctx, "query failed: %s",
                                     sqlite3_errmsg(js->base.db));
    }

    return qc.array;
}

/* db.exec implementation */
static JSValue js_db_exec_impl(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.stmt_cache)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.exec requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    if (!js_is_stdlib_caller(ctx) && hl_cap_db_check_namespace(sql) != 0) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

    HlValue *params = NULL;
    int nparams = 0;
    if (argc >= 2) {
        if (js_to_hl_values(ctx, argv[1], &params, &nparams) != 0) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "params must be an array");
        }
    }

    int rc = hl_cap_db_exec(js->base.stmt_cache, sql, params, nparams);

    js_free_hl_values(ctx, params, nparams);
    JS_FreeCString(ctx, sql);

    if (rc < 0)
        return JS_ThrowInternalError(ctx, "exec failed: %s",
                                     sqlite3_errmsg(js->base.db));

    return JS_NewInt32(ctx, rc);
}

static JSValue js_db_query(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{ return js_db_query_impl(ctx, this_val, argc, argv); }

static JSValue js_db_exec(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{ return js_db_exec_impl(ctx, this_val, argc, argv); }

/* db.lastId() */
static JSValue js_db_last_id(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db)
        return JS_ThrowInternalError(ctx, "database not available");

    return JS_NewInt64(ctx, hl_cap_db_last_id(js->base.db));
}

/* db.batch(fn) — execute fn() inside a transaction (BEGIN IMMEDIATE..COMMIT) */
static JSValue js_db_batch(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.db)
        return JS_ThrowInternalError(ctx, "database not available");

    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "db.batch requires a function argument");

    if (hl_cap_db_begin(js->base.db) != 0)
        return JS_ThrowInternalError(ctx, "BEGIN failed: %s",
                                     sqlite3_errmsg(js->base.db));

    JSValue result = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);

    if (JS_IsException(result)) {
        hl_cap_db_rollback(js->base.db);
        return result; /* propagate exception */
    }
    JS_FreeValue(ctx, result);

    if (hl_cap_db_commit(js->base.db) != 0) {
        hl_cap_db_rollback(js->base.db);
        return JS_ThrowInternalError(ctx, "COMMIT failed: %s",
                                     sqlite3_errmsg(js->base.db));
    }

    return JS_UNDEFINED;
}

/* ── db.async.query / db.async.exec ─────────────────────────────────── */

/* push_result callback: convert HlWorkerDbOp result to JSValue */
static JSValue js_push_worker_db_result(JSContext *ctx, void *driver)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)driver;

    if (op->error) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "error",
                          JS_NewString(ctx, op->error_msg));
        return obj;
    }

    if (op->kind == HL_WORK_DB_EXEC) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "changes",
                          JS_NewInt32(ctx, op->exec_changes));
        JS_SetPropertyStr(ctx, obj, "lastId",
                          JS_NewInt64(ctx, op->last_id));
        return obj;
    }

    /* HL_WORK_DB_QUERY — array of row objects */
    HlDbResult *r = &op->result;
    JSValue arr = JS_NewArray(ctx);

    for (int row = 0; row < r->nrows; row++) {
        JSValue obj = JS_NewObject(ctx);
        HlDbValue *vals = &r->values[row * r->ncols];
        for (int col = 0; col < r->ncols; col++) {
            JSValue v;
            switch (vals[col].type) {
            case HL_TYPE_INT:
                v = JS_NewInt64(ctx, vals[col].i);
                break;
            case HL_TYPE_DOUBLE:
                v = JS_NewFloat64(ctx, vals[col].d);
                break;
            case HL_TYPE_TEXT:
            case HL_TYPE_BLOB:
                v = JS_NewStringLen(ctx, vals[col].s, vals[col].len);
                break;
            case HL_TYPE_NIL:
            default:
                v = JS_NULL;
                break;
            }
            JS_SetPropertyStr(ctx, obj, r->col_names[col], v);
        }
        JS_SetPropertyUint32(ctx, arr, (uint32_t)row, obj);
    }

    return arr;
}

/* Common implementation for db.async.query and db.async.exec */
static JSValue js_db_async_common(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv,
                                   HlWorkerDbKind kind)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx,
            "db.async not available (no thread pool)");
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx,
            "db.async can only be called from a request handler");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "db.async requires (sql, params?)");

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql)
        return JS_EXCEPTION;

    if (!js_is_stdlib_caller(ctx) && hl_cap_db_check_namespace(sql) != 0) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx,
            "access denied: _hull_* tables are reserved");
    }

    /* Parse params */
    HlValue *params = NULL;
    int nparams = 0;
    if (argc >= 2) {
        if (js_to_hl_values(ctx, argv[1], &params, &nparams) != 0) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "params must be an array");
        }
    }

    /* Allocate op */
    HlWorkerDbOp *op = calloc(1, sizeof(HlWorkerDbOp));
    if (!op) {
        js_free_hl_values(ctx, params, nparams);
        JS_FreeCString(ctx, sql);
        return JS_ThrowInternalError(ctx, "db.async: out of memory");
    }

    op->kind = kind;
    op->server = js->server;
    op->alloc = js->base.alloc;
    op->sql = strdup(sql);
    JS_FreeCString(ctx, sql);
    if (!op->sql) {
        js_free_hl_values(ctx, params, nparams);
        free(op);
        return JS_ThrowInternalError(ctx, "db.async: out of memory");
    }

    /* Deep-copy params */
    if (nparams > 0) {
        op->params = hl_deep_copy_params(params, nparams);
        op->nparams = nparams;
        if (!op->params) {
            js_free_hl_values(ctx, params, nparams);
            free(op->sql);
            free(op);
            return JS_ThrowInternalError(ctx, "db.async: out of memory");
        }
    }
    js_free_hl_values(ctx, params, nparams);

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.alloc);
    if (!actx) {
        hl_worker_db_op_free(op);
        free(op);
        return JS_ThrowInternalError(ctx, "db.async: out of memory");
    }

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_worker_db_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    /* Create JS continuation */
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_worker_db_result);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_worker_db_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "db.async: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_db_op_free_all;
    actx->op.on_cancel = hl_worker_db_async_cancel;

    op->async_ctx = actx;
    op->cancelled = 0;

    /* Submit to thread pool */
    if (hl_worker_db_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_db_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "db.async: thread pool full");
    }

    /* Suspend the connection */
    if (kl_async_suspend(js->server, js->active_conn, &actx->op) < 0) {
        op->cancelled = 1;
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "db.async: failed to suspend connection");
    }

    return promise;
}

static JSValue js_db_async_query(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return js_db_async_common(ctx, this_val, argc, argv, HL_WORK_DB_QUERY);
}

static JSValue js_db_async_exec(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_db_async_common(ctx, this_val, argc, argv, HL_WORK_DB_EXEC);
}

static int js_db_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue db = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, db, "query",
                      JS_NewCFunction(ctx, js_db_query, "query", 2));
    JS_SetPropertyStr(ctx, db, "exec",
                      JS_NewCFunction(ctx, js_db_exec, "exec", 2));
    JS_SetPropertyStr(ctx, db, "lastId",
                      JS_NewCFunction(ctx, js_db_last_id, "lastId", 0));
    JS_SetPropertyStr(ctx, db, "batch",
                      JS_NewCFunction(ctx, js_db_batch, "batch", 1));

    /* db.async sub-object */
    JSValue async_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, async_obj, "query",
                      JS_NewCFunction(ctx, js_db_async_query, "query", 2));
    JS_SetPropertyStr(ctx, async_obj, "exec",
                      JS_NewCFunction(ctx, js_db_async_exec, "exec", 2));
    JS_SetPropertyStr(ctx, db, "async", async_obj);

    JS_SetModuleExport(ctx, m, "db", db);
    return 0;
}

int hl_js_init_db_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:db", js_db_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "db");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:time module
 *
 * time.now()      → Unix timestamp (seconds)
 * time.nowMs()    → milliseconds since epoch
 * time.clock()    → monotonic ms
 * time.date()     → "YYYY-MM-DD"
 * time.datetime() → "YYYY-MM-DDTHH:MM:SSZ"
 * ════════════════════════════════════════════════════════════════════ */

static JSValue js_time_now(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt64(ctx, hl_cap_time_now());
}

static JSValue js_time_now_ms(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt64(ctx, hl_cap_time_now_ms());
}

static JSValue js_time_clock(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewInt64(ctx, hl_cap_time_clock());
}

static JSValue js_time_date(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char buf[16];
    if (hl_cap_time_date(buf, sizeof(buf)) != 0)
        return JS_ThrowInternalError(ctx, "time.date() failed");
    return JS_NewString(ctx, buf);
}

static JSValue js_time_datetime(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    char buf[32];
    if (hl_cap_time_datetime(buf, sizeof(buf)) != 0)
        return JS_ThrowInternalError(ctx, "time.datetime() failed");
    return JS_NewString(ctx, buf);
}

static int js_time_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue time_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, time_obj, "now",
                      JS_NewCFunction(ctx, js_time_now, "now", 0));
    JS_SetPropertyStr(ctx, time_obj, "nowMs",
                      JS_NewCFunction(ctx, js_time_now_ms, "nowMs", 0));
    JS_SetPropertyStr(ctx, time_obj, "clock",
                      JS_NewCFunction(ctx, js_time_clock, "clock", 0));
    JS_SetPropertyStr(ctx, time_obj, "date",
                      JS_NewCFunction(ctx, js_time_date, "date", 0));
    JS_SetPropertyStr(ctx, time_obj, "datetime",
                      JS_NewCFunction(ctx, js_time_datetime, "datetime", 0));
    JS_SetModuleExport(ctx, m, "time", time_obj);
    return 0;
}

int hl_js_init_time_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:time", js_time_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "time");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:env module
 *
 * env.get(name) → string or null
 * ════════════════════════════════════════════════════════════════════ */

static JSValue js_env_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.env_cfg)
        return JS_ThrowInternalError(ctx, "env not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "env.get requires (name)");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    const char *val = hl_cap_env_get(js->base.env_cfg, name);
    JS_FreeCString(ctx, name);

    if (val)
        return JS_NewString(ctx, val);
    return JS_NULL;
}

static int js_env_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue env = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, env, "get",
                      JS_NewCFunction(ctx, js_env_get, "get", 1));
    JS_SetModuleExport(ctx, m, "env", env);
    return 0;
}

int hl_js_init_env_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:env", js_env_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "env");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:crypto module
 *
 * crypto.sha256(data)          → hex string
 * crypto.random(n)             → ArrayBuffer of n random bytes
 * crypto.hashPassword(pw)      → hash string
 * crypto.verifyPassword(pw, h) → boolean
 * ════════════════════════════════════════════════════════════════════ */

static JSValue js_crypto_sha256(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.sha256 requires (data)");

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!data)
        return JS_EXCEPTION;

    uint8_t hash[32];
    if (hl_cap_crypto_sha256(data, len, hash) != 0) {
        JS_FreeCString(ctx, data);
        return JS_ThrowInternalError(ctx, "sha256 failed");
    }
    JS_FreeCString(ctx, data);

    /* Convert to hex string */
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[64] = '\0';

    return JS_NewString(ctx, hex);
}

static JSValue js_crypto_random(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.random requires (n)");

    int32_t n;
    if (JS_ToInt32(ctx, &n, argv[0]))
        return JS_EXCEPTION;

    if (n <= 0 || n > HL_RANDOM_MAX_BYTES)
        return JS_ThrowRangeError(ctx, "random bytes must be 1-%d",
                                  HL_RANDOM_MAX_BYTES);

    uint8_t *buf = js_malloc(ctx, (size_t)n);
    if (!buf)
        return JS_EXCEPTION;

    if (hl_cap_crypto_random(buf, (size_t)n) != 0) {
        js_free(ctx, buf);
        return JS_ThrowInternalError(ctx, "random failed");
    }

    /* Copy into ArrayBuffer and free temp */
    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, (size_t)n);
    js_free(ctx, buf);
    return ab;
}

/* Hex nibble helper (no sscanf — Cosmopolitan compat) */
static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* crypto.hashPassword(password) → "pbkdf2:iterations:salt_hex:hash_hex" */
static JSValue js_crypto_hash_password(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.hashPassword requires (password)");

    size_t pw_len;
    const char *pw = JS_ToCStringLen(ctx, &pw_len, argv[0]);
    if (!pw)
        return JS_EXCEPTION;

    /* Generate 16-byte salt */
    uint8_t salt[16];
    if (hl_cap_crypto_random(salt, sizeof(salt)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_ThrowInternalError(ctx, "random failed");
    }

    /* PBKDF2-HMAC-SHA256, 32-byte output */
    uint8_t hash[32];
    int iterations = HL_PBKDF2_ITERATIONS;
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                               iterations, hash, sizeof(hash)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_ThrowInternalError(ctx, "pbkdf2 failed");
    }
    JS_FreeCString(ctx, pw);

    /* Format: "pbkdf2:100000:salt_hex:hash_hex" */
    char salt_hex[33], hash_hex[65];
    for (int i = 0; i < 16; i++)
        snprintf(salt_hex + i * 2, 3, "%02x", salt[i]);
    for (int i = 0; i < 32; i++)
        snprintf(hash_hex + i * 2, 3, "%02x", hash[i]);

    char result[128];
    snprintf(result, sizeof(result), "pbkdf2:%d:%s:%s",
             iterations, salt_hex, hash_hex);

    secure_zero(hash, sizeof(hash));
    secure_zero(salt, sizeof(salt));

    return JS_NewString(ctx, result);
}

/* crypto.verifyPassword(password, hash_string) → boolean */
static JSValue js_crypto_verify_password(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.verifyPassword requires (password, hash)");

    size_t pw_len;
    const char *pw = JS_ToCStringLen(ctx, &pw_len, argv[0]);
    const char *stored = JS_ToCString(ctx, argv[1]);
    if (!pw || !stored) {
        if (pw) JS_FreeCString(ctx, pw);
        if (stored) JS_FreeCString(ctx, stored);
        return JS_EXCEPTION;
    }

    /* Parse "pbkdf2:iterations:salt_hex:hash_hex" manually (no scansets
     * — Cosmopolitan libc doesn't support sscanf %[...] scansets). */
    if (strncmp(stored, "pbkdf2:", 7) != 0) {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    const char *p = stored + 7;

    char *end = NULL;
    long iterations = strtol(p, &end, 10);
    if (!end || *end != ':' || iterations < 100000) {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    p = end + 1;

    if (strlen(p) < 32 + 1 + 64 || p[32] != ':') {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    char salt_hex[33];
    memcpy(salt_hex, p, 32);
    salt_hex[32] = '\0';
    p += 33;

    if (strlen(p) < 64) {
        JS_FreeCString(ctx, pw);
        JS_FreeCString(ctx, stored);
        return JS_FALSE;
    }
    char hash_hex[65];
    memcpy(hash_hex, p, 64);
    hash_hex[64] = '\0';

    JS_FreeCString(ctx, stored);

    /* Decode hex salt (manual — sscanf %x broken on Cosmopolitan) */
    uint8_t salt[16];
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble((unsigned char)salt_hex[i * 2]);
        int lo = hex_nibble((unsigned char)salt_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { JS_FreeCString(ctx, pw); return JS_FALSE; }
        salt[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Recompute hash */
    uint8_t computed[32];
    if (hl_cap_crypto_pbkdf2(pw, pw_len, salt, sizeof(salt),
                               (int)iterations, computed, sizeof(computed)) != 0) {
        JS_FreeCString(ctx, pw);
        return JS_FALSE;
    }
    JS_FreeCString(ctx, pw);

    /* Decode stored hash and compare (constant-time) */
    uint8_t stored_hash[32];
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble((unsigned char)hash_hex[i * 2]);
        int lo = hex_nibble((unsigned char)hash_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return JS_FALSE;
        stored_hash[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Constant-time comparison */
    volatile uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= computed[i] ^ stored_hash[i];

    secure_zero(computed, sizeof(computed));
    secure_zero(stored_hash, sizeof(stored_hash));
    secure_zero(salt, sizeof(salt));

    return diff == 0 ? JS_TRUE : JS_FALSE;
}

/* ── Hex decode helper (no sscanf — Cosmopolitan compat) ──────────── */

static int hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2)
        return -1;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble((unsigned char)hex[i * 2]);
        int lo = hex_nibble((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ── Ed25519 bindings ──────────────────────────────────────────────── */

/* crypto.ed25519Keypair() → { publicKey: hex, secretKey: hex } */
static JSValue js_crypto_ed25519_keypair(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;

    uint8_t pk[32], sk[64];
    if (hl_cap_crypto_ed25519_keypair(pk, sk) != 0)
        return JS_ThrowInternalError(ctx, "ed25519 keypair generation failed");

    char pk_hex[65], sk_hex[129];
    for (int i = 0; i < 32; i++)
        snprintf(pk_hex + i * 2, 3, "%02x", pk[i]);
    pk_hex[64] = '\0';
    for (int i = 0; i < 64; i++)
        snprintf(sk_hex + i * 2, 3, "%02x", sk[i]);
    sk_hex[128] = '\0';

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "publicKey", JS_NewString(ctx, pk_hex));
    JS_SetPropertyStr(ctx, obj, "secretKey", JS_NewString(ctx, sk_hex));
    secure_zero(sk, sizeof(sk));
    secure_zero(sk_hex, sizeof(sk_hex));
    return obj;
}

/* crypto.ed25519Sign(data, secretKeyHex) → signatureHex */
static JSValue js_crypto_ed25519_sign(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.ed25519Sign requires (data, secretKeyHex)");

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[0]);
    if (!data) return JS_EXCEPTION;

    size_t sk_hex_len;
    const char *sk_hex = JS_ToCStringLen(ctx, &sk_hex_len, argv[1]);
    if (!sk_hex) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    if (sk_hex_len != 128) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sk_hex);
        return JS_ThrowTypeError(ctx, "secret key must be 128 hex chars (64 bytes)");
    }

    uint8_t sk[64];
    if (hex_decode(sk_hex, sk_hex_len, sk, 64) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sk_hex);
        secure_zero(sk, sizeof(sk));
        return JS_ThrowTypeError(ctx, "invalid hex in secret key");
    }
    JS_FreeCString(ctx, sk_hex);

    uint8_t sig[64];
    if (hl_cap_crypto_ed25519_sign((const uint8_t *)data, data_len, sk, sig) != 0) {
        JS_FreeCString(ctx, data);
        secure_zero(sk, sizeof(sk));
        return JS_ThrowInternalError(ctx, "ed25519 sign failed");
    }
    JS_FreeCString(ctx, data);
    secure_zero(sk, sizeof(sk));

    char sig_hex[129];
    for (int i = 0; i < 64; i++)
        snprintf(sig_hex + i * 2, 3, "%02x", sig[i]);
    sig_hex[128] = '\0';

    return JS_NewString(ctx, sig_hex);
}

/* crypto.ed25519Verify(data, sigHex, pubkeyHex) → boolean */
static JSValue js_crypto_ed25519_verify(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.ed25519Verify requires (data, sigHex, pubkeyHex)");

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[0]);
    if (!data) return JS_EXCEPTION;

    size_t sig_hex_len;
    const char *sig_hex = JS_ToCStringLen(ctx, &sig_hex_len, argv[1]);
    if (!sig_hex) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    size_t pk_hex_len;
    const char *pk_hex = JS_ToCStringLen(ctx, &pk_hex_len, argv[2]);
    if (!pk_hex) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sig_hex);
        return JS_EXCEPTION;
    }

    if (sig_hex_len != 128 || pk_hex_len != 64) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sig_hex);
        JS_FreeCString(ctx, pk_hex);
        return JS_FALSE;
    }

    uint8_t sig[64], pk[32];
    if (hex_decode(sig_hex, sig_hex_len, sig, 64) != 0 ||
        hex_decode(pk_hex, pk_hex_len, pk, 32) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, sig_hex);
        JS_FreeCString(ctx, pk_hex);
        return JS_FALSE;
    }

    JS_FreeCString(ctx, sig_hex);
    JS_FreeCString(ctx, pk_hex);

    int rc = hl_cap_crypto_ed25519_verify((const uint8_t *)data, data_len, sig, pk);
    JS_FreeCString(ctx, data);

    return rc == 0 ? JS_TRUE : JS_FALSE;
}

/* ── SHA-512 ──────────────────────────────────────────────────────── */

static JSValue js_crypto_sha512(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.sha512 requires (data)");

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!data) return JS_EXCEPTION;

    uint8_t hash[64];
    if (hl_cap_crypto_sha512(data, len, hash) != 0) {
        JS_FreeCString(ctx, data);
        return JS_ThrowInternalError(ctx, "sha512 failed");
    }
    JS_FreeCString(ctx, data);

    char hex[129];
    for (int i = 0; i < 64; i++)
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[128] = '\0';

    return JS_NewString(ctx, hex);
}

/* ── HMAC-SHA512/256 auth ─────────────────────────────────────────── */

/* crypto.auth(msg, keyHex) → hex */
static JSValue js_crypto_auth(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.auth requires (msg, keyHex)");

    size_t msg_len;
    const char *msg = JS_ToCStringLen(ctx, &msg_len, argv[0]);
    if (!msg) return JS_EXCEPTION;

    size_t key_hex_len;
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[1]);
    if (!key_hex) { JS_FreeCString(ctx, msg); return JS_EXCEPTION; }

    if (key_hex_len != 64) {
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "key must be 64 hex chars (32 bytes)");
    }

    uint8_t key[32];
    if (hex_decode(key_hex, key_hex_len, key, 32) != 0) {
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "invalid hex in key");
    }
    JS_FreeCString(ctx, key_hex);

    uint8_t tag[32];
    hl_cap_crypto_auth(msg, msg_len, key, tag);
    JS_FreeCString(ctx, msg);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", tag[i]);
    hex[64] = '\0';

    return JS_NewString(ctx, hex);
}

/* crypto.authVerify(tagHex, msg, keyHex) → boolean */
static JSValue js_crypto_auth_verify(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.authVerify requires (tagHex, msg, keyHex)");

    size_t tag_hex_len;
    const char *tag_hex = JS_ToCStringLen(ctx, &tag_hex_len, argv[0]);
    if (!tag_hex) return JS_EXCEPTION;

    size_t msg_len;
    const char *msg = JS_ToCStringLen(ctx, &msg_len, argv[1]);
    if (!msg) { JS_FreeCString(ctx, tag_hex); return JS_EXCEPTION; }

    size_t key_hex_len;
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[2]);
    if (!key_hex) {
        JS_FreeCString(ctx, tag_hex);
        JS_FreeCString(ctx, msg);
        return JS_EXCEPTION;
    }

    if (tag_hex_len != 64 || key_hex_len != 64) {
        JS_FreeCString(ctx, tag_hex);
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, key_hex);
        return JS_FALSE;
    }

    uint8_t tag[32], key[32];
    if (hex_decode(tag_hex, tag_hex_len, tag, 32) != 0 ||
        hex_decode(key_hex, key_hex_len, key, 32) != 0) {
        JS_FreeCString(ctx, tag_hex);
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, key_hex);
        return JS_FALSE;
    }
    JS_FreeCString(ctx, tag_hex);
    JS_FreeCString(ctx, key_hex);

    int rc = hl_cap_crypto_auth_verify(tag, msg, msg_len, key);
    JS_FreeCString(ctx, msg);

    return rc == 0 ? JS_TRUE : JS_FALSE;
}

/* ── Secretbox ────────────────────────────────────────────────────── */

/* crypto.secretbox(msg, nonceHex, keyHex) → hex */
static JSValue js_crypto_secretbox(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.secretbox requires (msg, nonceHex, keyHex)");

    size_t msg_len;
    const char *msg = JS_ToCStringLen(ctx, &msg_len, argv[0]);
    if (!msg) return JS_EXCEPTION;

    size_t nonce_hex_len, key_hex_len;
    const char *nonce_hex = JS_ToCStringLen(ctx, &nonce_hex_len, argv[1]);
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[2]);
    if (!nonce_hex || !key_hex) {
        JS_FreeCString(ctx, msg);
        if (nonce_hex) JS_FreeCString(ctx, nonce_hex);
        if (key_hex) JS_FreeCString(ctx, key_hex);
        return JS_EXCEPTION;
    }

    if (nonce_hex_len != 48 || key_hex_len != 64) {
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "nonce must be 48 hex (24 bytes), key 64 hex (32 bytes)");
    }

    uint8_t nonce[24], key[32];
    if (hex_decode(nonce_hex, 48, nonce, 24) != 0 ||
        hex_decode(key_hex, 64, key, 32) != 0) {
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "invalid hex");
    }
    JS_FreeCString(ctx, nonce_hex);
    JS_FreeCString(ctx, key_hex);

    if (msg_len > SIZE_MAX - HL_SECRETBOX_MACBYTES) {
        JS_FreeCString(ctx, msg);
        return JS_ThrowRangeError(ctx, "message too large");
    }
    size_t ct_len = msg_len + HL_SECRETBOX_MACBYTES;
    uint8_t *ct = js_malloc(ctx, ct_len);
    if (!ct) { JS_FreeCString(ctx, msg); return JS_EXCEPTION; }

    if (hl_cap_crypto_secretbox(ct, msg, msg_len, nonce, key) != 0) {
        JS_FreeCString(ctx, msg);
        js_free(ctx, ct);
        return JS_ThrowInternalError(ctx, "secretbox failed");
    }
    JS_FreeCString(ctx, msg);

    /* Hex encode */
    if (ct_len > SIZE_MAX / 2) { js_free(ctx, ct); return JS_ThrowRangeError(ctx, "ciphertext too large"); }
    char *hex = js_malloc(ctx, ct_len * 2 + 1);
    if (!hex) { js_free(ctx, ct); return JS_EXCEPTION; }
    for (size_t i = 0; i < ct_len; i++)
        snprintf(hex + i * 2, 3, "%02x", ct[i]);
    hex[ct_len * 2] = '\0';
    js_free(ctx, ct);

    JSValue result = JS_NewString(ctx, hex);
    js_free(ctx, hex);
    return result;
}

/* crypto.secretboxOpen(ctHex, nonceHex, keyHex) → string/null */
static JSValue js_crypto_secretbox_open(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.secretboxOpen requires (ctHex, nonceHex, keyHex)");

    size_t ct_hex_len;
    const char *ct_hex = JS_ToCStringLen(ctx, &ct_hex_len, argv[0]);
    if (!ct_hex) return JS_EXCEPTION;

    size_t nonce_hex_len, key_hex_len;
    const char *nonce_hex = JS_ToCStringLen(ctx, &nonce_hex_len, argv[1]);
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[2]);
    if (!nonce_hex || !key_hex) {
        JS_FreeCString(ctx, ct_hex);
        if (nonce_hex) JS_FreeCString(ctx, nonce_hex);
        if (key_hex) JS_FreeCString(ctx, key_hex);
        return JS_EXCEPTION;
    }

    if (ct_hex_len % 2 != 0 || nonce_hex_len != 48 || key_hex_len != 64) {
        JS_FreeCString(ctx, ct_hex);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_NULL;
    }

    size_t ct_len = ct_hex_len / 2;
    if (ct_len < HL_SECRETBOX_MACBYTES) {
        JS_FreeCString(ctx, ct_hex);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_NULL;
    }

    uint8_t nonce[24], key[32];
    if (hex_decode(nonce_hex, 48, nonce, 24) != 0 ||
        hex_decode(key_hex, 64, key, 32) != 0) {
        JS_FreeCString(ctx, ct_hex);
        JS_FreeCString(ctx, nonce_hex);
        JS_FreeCString(ctx, key_hex);
        return JS_NULL;
    }
    JS_FreeCString(ctx, nonce_hex);
    JS_FreeCString(ctx, key_hex);

    uint8_t *ct = js_malloc(ctx, ct_len);
    if (!ct) { JS_FreeCString(ctx, ct_hex); return JS_EXCEPTION; }
    if (hex_decode(ct_hex, ct_hex_len, ct, ct_len) != 0) {
        JS_FreeCString(ctx, ct_hex);
        js_free(ctx, ct);
        return JS_NULL;
    }
    JS_FreeCString(ctx, ct_hex);

    size_t pt_len = ct_len - HL_SECRETBOX_MACBYTES;
    uint8_t *pt = js_malloc(ctx, pt_len + 1);
    if (!pt) { js_free(ctx, ct); return JS_EXCEPTION; }

    if (hl_cap_crypto_secretbox_open(pt, ct, ct_len, nonce, key) != 0) {
        js_free(ctx, ct);
        js_free(ctx, pt);
        return JS_NULL;
    }
    js_free(ctx, ct);

    JSValue result = JS_NewStringLen(ctx, (const char *)pt, pt_len);
    js_free(ctx, pt);
    return result;
}

/* ── Box (public-key encryption) ──────────────────────────────────── */

/* crypto.box(msg, nonceHex, pkHex, skHex) → hex */
static JSValue js_crypto_box(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "crypto.box requires (msg, nonceHex, pkHex, skHex)");

    size_t msg_len;
    const char *msg = JS_ToCStringLen(ctx, &msg_len, argv[0]);
    if (!msg) return JS_EXCEPTION;

    size_t nh_len, pkh_len, skh_len;
    const char *nh = JS_ToCStringLen(ctx, &nh_len, argv[1]);
    const char *pkh = JS_ToCStringLen(ctx, &pkh_len, argv[2]);
    const char *skh = JS_ToCStringLen(ctx, &skh_len, argv[3]);
    if (!nh || !pkh || !skh) {
        JS_FreeCString(ctx, msg);
        if (nh) JS_FreeCString(ctx, nh);
        if (pkh) JS_FreeCString(ctx, pkh);
        if (skh) JS_FreeCString(ctx, skh);
        return JS_EXCEPTION;
    }

    if (nh_len != 48 || pkh_len != 64 || skh_len != 64) {
        JS_FreeCString(ctx, msg); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_ThrowTypeError(ctx, "nonce 48 hex, pk/sk 64 hex each");
    }

    uint8_t nonce[24], pk[32], sk[32];
    if (hex_decode(nh, 48, nonce, 24) != 0 ||
        hex_decode(pkh, 64, pk, 32) != 0 ||
        hex_decode(skh, 64, sk, 32) != 0) {
        JS_FreeCString(ctx, msg); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_ThrowTypeError(ctx, "invalid hex");
    }
    JS_FreeCString(ctx, nh);
    JS_FreeCString(ctx, pkh);
    JS_FreeCString(ctx, skh);

    if (msg_len > SIZE_MAX - HL_BOX_MACBYTES) {
        JS_FreeCString(ctx, msg);
        return JS_ThrowRangeError(ctx, "message too large");
    }
    size_t ct_len = msg_len + HL_BOX_MACBYTES;
    uint8_t *ct = js_malloc(ctx, ct_len);
    if (!ct) { JS_FreeCString(ctx, msg); return JS_EXCEPTION; }

    if (hl_cap_crypto_box(ct, msg, msg_len, nonce, pk, sk) != 0) {
        JS_FreeCString(ctx, msg);
        js_free(ctx, ct);
        return JS_ThrowInternalError(ctx, "box failed");
    }
    JS_FreeCString(ctx, msg);

    if (ct_len > SIZE_MAX / 2) { js_free(ctx, ct); return JS_ThrowRangeError(ctx, "ciphertext too large"); }
    char *hex = js_malloc(ctx, ct_len * 2 + 1);
    if (!hex) { js_free(ctx, ct); return JS_EXCEPTION; }
    for (size_t i = 0; i < ct_len; i++)
        snprintf(hex + i * 2, 3, "%02x", ct[i]);
    hex[ct_len * 2] = '\0';
    js_free(ctx, ct);

    JSValue result = JS_NewString(ctx, hex);
    js_free(ctx, hex);
    return result;
}

/* crypto.boxOpen(ctHex, nonceHex, pkHex, skHex) → string/null */
static JSValue js_crypto_box_open(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 4)
        return JS_ThrowTypeError(ctx, "crypto.boxOpen requires (ctHex, nonceHex, pkHex, skHex)");

    size_t cth_len, nh_len, pkh_len, skh_len;
    const char *cth = JS_ToCStringLen(ctx, &cth_len, argv[0]);
    const char *nh = JS_ToCStringLen(ctx, &nh_len, argv[1]);
    const char *pkh = JS_ToCStringLen(ctx, &pkh_len, argv[2]);
    const char *skh = JS_ToCStringLen(ctx, &skh_len, argv[3]);
    if (!cth || !nh || !pkh || !skh) {
        if (cth) JS_FreeCString(ctx, cth);
        if (nh) JS_FreeCString(ctx, nh);
        if (pkh) JS_FreeCString(ctx, pkh);
        if (skh) JS_FreeCString(ctx, skh);
        return JS_EXCEPTION;
    }

    if (cth_len % 2 != 0 || nh_len != 48 || pkh_len != 64 || skh_len != 64) {
        JS_FreeCString(ctx, cth); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_NULL;
    }

    size_t ct_len = cth_len / 2;
    if (ct_len < HL_BOX_MACBYTES) {
        JS_FreeCString(ctx, cth); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_NULL;
    }

    uint8_t nonce[24], pk[32], sk[32];
    if (hex_decode(nh, 48, nonce, 24) != 0 ||
        hex_decode(pkh, 64, pk, 32) != 0 ||
        hex_decode(skh, 64, sk, 32) != 0) {
        JS_FreeCString(ctx, cth); JS_FreeCString(ctx, nh);
        JS_FreeCString(ctx, pkh); JS_FreeCString(ctx, skh);
        return JS_NULL;
    }
    JS_FreeCString(ctx, nh);
    JS_FreeCString(ctx, pkh);
    JS_FreeCString(ctx, skh);

    uint8_t *ct = js_malloc(ctx, ct_len);
    if (!ct) { JS_FreeCString(ctx, cth); return JS_EXCEPTION; }
    if (hex_decode(cth, cth_len, ct, ct_len) != 0) {
        JS_FreeCString(ctx, cth);
        js_free(ctx, ct);
        return JS_NULL;
    }
    JS_FreeCString(ctx, cth);

    size_t pt_len = ct_len - HL_BOX_MACBYTES;
    uint8_t *pt = js_malloc(ctx, pt_len + 1);
    if (!pt) { js_free(ctx, ct); return JS_EXCEPTION; }

    if (hl_cap_crypto_box_open(pt, ct, ct_len, nonce, pk, sk) != 0) {
        js_free(ctx, ct);
        js_free(ctx, pt);
        return JS_NULL;
    }
    js_free(ctx, ct);

    JSValue result = JS_NewStringLen(ctx, (const char *)pt, pt_len);
    js_free(ctx, pt);
    return result;
}

/* crypto.boxKeypair() → { publicKey: hex, secretKey: hex } */
static JSValue js_crypto_box_keypair(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;

    uint8_t pk[32], sk[32];
    if (hl_cap_crypto_box_keypair(pk, sk) != 0)
        return JS_ThrowInternalError(ctx, "box keypair generation failed");

    char pk_hex[65], sk_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(pk_hex + i * 2, 3, "%02x", pk[i]);
    pk_hex[64] = '\0';
    for (int i = 0; i < 32; i++)
        snprintf(sk_hex + i * 2, 3, "%02x", sk[i]);
    sk_hex[64] = '\0';

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "publicKey", JS_NewString(ctx, pk_hex));
    JS_SetPropertyStr(ctx, obj, "secretKey", JS_NewString(ctx, sk_hex));
    secure_zero(sk, sizeof(sk));
    secure_zero(sk_hex, sizeof(sk_hex));
    return obj;
}

/* crypto.hmacSha256(data, keyHex) → hex string */
static JSValue js_crypto_hmac_sha256(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "crypto.hmacSha256 requires (data, keyHex)");

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[0]);
    if (!data) return JS_EXCEPTION;

    size_t key_hex_len;
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[1]);
    if (!key_hex) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    if (key_hex_len % 2 != 0 || key_hex_len == 0 || key_hex_len > 256) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "key must be 1-128 bytes (2-256 hex chars)");
    }

    size_t key_len = key_hex_len / 2;
    uint8_t key[128];
    if (hex_decode(key_hex, key_hex_len, key, key_len) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        return JS_ThrowTypeError(ctx, "invalid hex in key");
    }
    JS_FreeCString(ctx, key_hex);

    uint8_t out[32];
    if (hl_cap_crypto_hmac_sha256(key, key_len,
                                  (const uint8_t *)data, data_len, out) != 0) {
        JS_FreeCString(ctx, data);
        secure_zero(key, sizeof(key));
        return JS_ThrowInternalError(ctx, "hmacSha256 failed");
    }
    JS_FreeCString(ctx, data);
    secure_zero(key, sizeof(key));

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", out[i]);
    hex[64] = '\0';

    return JS_NewString(ctx, hex);
}

/* crypto.hmacSha256Verify(data, keyHex, expectedHex) → boolean */
static JSValue js_crypto_hmac_sha256_verify(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "crypto.hmacSha256Verify requires (data, keyHex, expectedHex)");

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[0]);
    if (!data) return JS_EXCEPTION;

    size_t key_hex_len;
    const char *key_hex = JS_ToCStringLen(ctx, &key_hex_len, argv[1]);
    if (!key_hex) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    size_t expected_hex_len;
    const char *expected_hex = JS_ToCStringLen(ctx, &expected_hex_len, argv[2]);
    if (!expected_hex) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        return JS_EXCEPTION;
    }

    if (key_hex_len % 2 != 0 || key_hex_len == 0 || key_hex_len > 256) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        JS_FreeCString(ctx, expected_hex);
        return JS_ThrowTypeError(ctx, "key must be 1-128 bytes (2-256 hex chars)");
    }
    if (expected_hex_len != 64) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        JS_FreeCString(ctx, expected_hex);
        return JS_ThrowTypeError(ctx, "expected mac must be 64 hex chars (32 bytes)");
    }

    size_t key_len = key_hex_len / 2;
    uint8_t key[128];
    if (hex_decode(key_hex, key_hex_len, key, key_len) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, key_hex);
        JS_FreeCString(ctx, expected_hex);
        return JS_ThrowTypeError(ctx, "invalid hex in key");
    }
    JS_FreeCString(ctx, key_hex);

    uint8_t expected[32];
    if (hex_decode(expected_hex, expected_hex_len, expected, 32) != 0) {
        JS_FreeCString(ctx, data);
        JS_FreeCString(ctx, expected_hex);
        secure_zero(key, sizeof(key));
        return JS_ThrowTypeError(ctx, "invalid hex in expected mac");
    }
    JS_FreeCString(ctx, expected_hex);

    int rc = hl_cap_crypto_hmac_sha256_verify(key, key_len,
                                               (const uint8_t *)data, data_len,
                                               expected);
    JS_FreeCString(ctx, data);
    secure_zero(key, sizeof(key));

    return JS_NewBool(ctx, rc == 0);
}

/* crypto.base64urlEncode(data) → string (no padding) */
static JSValue js_crypto_base64url_encode(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.base64urlEncode requires (data)");

    size_t len;
    const char *data = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!data) return JS_EXCEPTION;

    if (len > SIZE_MAX / 4) {
        JS_FreeCString(ctx, data);
        return JS_ThrowRangeError(ctx, "input too large for base64url");
    }
    size_t out_size = ((len * 4) + 2) / 3 + 1;
    char *out = js_malloc(ctx, out_size);
    if (!out) { JS_FreeCString(ctx, data); return JS_EXCEPTION; }

    size_t out_len;
    if (hl_cap_crypto_base64url_encode(data, len, out, out_size, &out_len) != 0) {
        JS_FreeCString(ctx, data);
        js_free(ctx, out);
        return JS_ThrowInternalError(ctx, "base64urlEncode failed");
    }
    JS_FreeCString(ctx, data);

    JSValue result = JS_NewStringLen(ctx, out, out_len);
    js_free(ctx, out);
    return result;
}

/* crypto.base64urlDecode(str) → string or null on error */
static JSValue js_crypto_base64url_decode(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "crypto.base64urlDecode requires (str)");

    size_t str_len;
    const char *str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str) return JS_EXCEPTION;

    size_t out_size = (str_len * 3) / 4 + 1;
    uint8_t *out = js_malloc(ctx, out_size);
    if (!out) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }

    size_t out_len;
    if (hl_cap_crypto_base64url_decode(str, str_len, out, out_size, &out_len) != 0) {
        JS_FreeCString(ctx, str);
        js_free(ctx, out);
        return JS_NULL;
    }
    JS_FreeCString(ctx, str);

    JSValue result = JS_NewStringLen(ctx, (const char *)out, out_len);
    js_free(ctx, out);
    return result;
}

static int js_crypto_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "sha256",
                      JS_NewCFunction(ctx, js_crypto_sha256, "sha256", 1));
    JS_SetPropertyStr(ctx, crypto, "sha512",
                      JS_NewCFunction(ctx, js_crypto_sha512, "sha512", 1));
    JS_SetPropertyStr(ctx, crypto, "random",
                      JS_NewCFunction(ctx, js_crypto_random, "random", 1));
    JS_SetPropertyStr(ctx, crypto, "hashPassword",
                      JS_NewCFunction(ctx, js_crypto_hash_password, "hashPassword", 1));
    JS_SetPropertyStr(ctx, crypto, "verifyPassword",
                      JS_NewCFunction(ctx, js_crypto_verify_password, "verifyPassword", 2));
    JS_SetPropertyStr(ctx, crypto, "ed25519Keypair",
                      JS_NewCFunction(ctx, js_crypto_ed25519_keypair, "ed25519Keypair", 0));
    JS_SetPropertyStr(ctx, crypto, "ed25519Sign",
                      JS_NewCFunction(ctx, js_crypto_ed25519_sign, "ed25519Sign", 2));
    JS_SetPropertyStr(ctx, crypto, "ed25519Verify",
                      JS_NewCFunction(ctx, js_crypto_ed25519_verify, "ed25519Verify", 3));
    JS_SetPropertyStr(ctx, crypto, "auth",
                      JS_NewCFunction(ctx, js_crypto_auth, "auth", 2));
    JS_SetPropertyStr(ctx, crypto, "authVerify",
                      JS_NewCFunction(ctx, js_crypto_auth_verify, "authVerify", 3));
    JS_SetPropertyStr(ctx, crypto, "secretbox",
                      JS_NewCFunction(ctx, js_crypto_secretbox, "secretbox", 3));
    JS_SetPropertyStr(ctx, crypto, "secretboxOpen",
                      JS_NewCFunction(ctx, js_crypto_secretbox_open, "secretboxOpen", 3));
    JS_SetPropertyStr(ctx, crypto, "box",
                      JS_NewCFunction(ctx, js_crypto_box, "box", 4));
    JS_SetPropertyStr(ctx, crypto, "boxOpen",
                      JS_NewCFunction(ctx, js_crypto_box_open, "boxOpen", 4));
    JS_SetPropertyStr(ctx, crypto, "boxKeypair",
                      JS_NewCFunction(ctx, js_crypto_box_keypair, "boxKeypair", 0));
    JS_SetPropertyStr(ctx, crypto, "hmacSha256",
                      JS_NewCFunction(ctx, js_crypto_hmac_sha256, "hmacSha256", 2));
    JS_SetPropertyStr(ctx, crypto, "hmacSha256Verify",
                      JS_NewCFunction(ctx, js_crypto_hmac_sha256_verify, "hmacSha256Verify", 3));
    JS_SetPropertyStr(ctx, crypto, "base64urlEncode",
                      JS_NewCFunction(ctx, js_crypto_base64url_encode, "base64urlEncode", 1));
    JS_SetPropertyStr(ctx, crypto, "base64urlDecode",
                      JS_NewCFunction(ctx, js_crypto_base64url_decode, "base64urlDecode", 1));
    JS_SetModuleExport(ctx, m, "crypto", crypto);
    return 0;
}

int hl_js_init_crypto_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:crypto", js_crypto_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "crypto");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:http module
 *
 * http.request(method, url, opts?) → { status, body, headers }
 * http.get(url, opts?)             → { status, body, headers }
 * http.post(url, body, opts?)      → { status, body, headers }
 * http.put(url, body, opts?)       → { status, body, headers }
 * http.patch(url, body, opts?)     → { status, body, headers }
 * http.del(url, opts?)             → { status, body, headers }
 * ════════════════════════════════════════════════════════════════════ */

/* Parse JS headers object { name: value } into HlHttpHeader array.
 * Returns count. Caller must free with js_free_http_headers(). */
static int js_parse_http_headers(JSContext *ctx, JSValueConst obj,
                                    HlHttpHeader **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (JS_IsUndefined(obj) || JS_IsNull(obj))
        return 0;

    /* Get property names */
    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, obj,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;

    if (prop_count == 0) {
        js_free(ctx, props);
        return 0;
    }

    HlHttpHeader *hdrs = js_mallocz(ctx, prop_count * sizeof(HlHttpHeader));
    if (!hdrs) {
        for (uint32_t i = 0; i < prop_count; i++)
            JS_FreeAtom(ctx, props[i].atom);
        js_free(ctx, props);
        return -1;
    }

    int count = 0;
    for (uint32_t i = 0; i < prop_count; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        JSValue val = JS_GetProperty(ctx, obj, props[i].atom);
        const char *value = JS_ToCString(ctx, val);
        JS_FreeValue(ctx, val);

        if (name && value) {
            hdrs[count].name = name;
            hdrs[count].value = value;
            count++;
        } else {
            if (name) JS_FreeCString(ctx, name);
            if (value) JS_FreeCString(ctx, value);
        }
        JS_FreeAtom(ctx, props[i].atom);
    }
    js_free(ctx, props);

    *out = hdrs;
    *out_count = count;
    return 0;
}

static void js_free_http_headers(JSContext *ctx, HlHttpHeader *hdrs, int count)
{
    if (!hdrs) return;
    for (int i = 0; i < count; i++) {
        JS_FreeCString(ctx, hdrs[i].name);
        JS_FreeCString(ctx, hdrs[i].value);
    }
    js_free(ctx, hdrs);
}

/* Push HTTP response as JS object: { status, body, headers } */
static JSValue js_push_http_response(JSContext *ctx, const KlClientResponse *resp)
{
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, resp->status));

    if (resp->body && resp->body_len > 0)
        JS_SetPropertyStr(ctx, obj, "body",
                          JS_NewStringLen(ctx, resp->body, resp->body_len));
    else
        JS_SetPropertyStr(ctx, obj, "body", JS_NewString(ctx, ""));

    /* Headers as { "name": "value" } — lowercase names */
    JSValue headers = JS_NewObject(ctx);
    for (int i = 0; i < resp->num_headers; i++) {
        /* Lowercase header name */
        size_t nlen = strlen(resp->headers[i].name);
        char *lower = js_malloc(ctx, nlen + 1);
        if (lower) {
            for (size_t j = 0; j < nlen; j++)
                lower[j] = (char)((resp->headers[i].name[j] >= 'A' &&
                                    resp->headers[i].name[j] <= 'Z')
                    ? resp->headers[i].name[j] + 32
                    : resp->headers[i].name[j]);
            lower[nlen] = '\0';
            JS_SetPropertyStr(ctx, headers, lower,
                              JS_NewString(ctx, resp->headers[i].value));
            js_free(ctx, lower);
        } else {
            JS_SetPropertyStr(ctx, headers, resp->headers[i].name,
                              JS_NewString(ctx, resp->headers[i].value));
        }
    }
    JS_SetPropertyStr(ctx, obj, "headers", headers);

    return obj;
}

/* http.request(method, url, opts?) */
static JSValue js_http_request(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured (no hosts in manifest)");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "http.request requires (method, url, opts?)");

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!method || !url) {
        if (method) JS_FreeCString(ctx, method);
        if (url) JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    const char *body = NULL;
    size_t body_len = 0;
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse opts */
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue body_val = JS_GetPropertyStr(ctx, argv[2], "body");
        if (JS_IsString(body_val))
            body = JS_ToCStringLen(ctx, &body_len, body_val);
        JS_FreeValue(ctx, body_val);

        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(js->base.http_cfg, method, url,
                                    headers, num_headers, body, body_len, &resp);

    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    // cppcheck-suppress knownConditionTrueFalse
    if (body) JS_FreeCString(ctx, body);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http request failed: %s",
                                      kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_client_response_free(&resp);
    return result;
}

/* http.get(url, opts?) */
static JSValue js_http_get(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http.get requires (url, opts?)");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(js->base.http_cfg, "GET", url,
                                    headers, num_headers, NULL, 0, &resp);
    JS_FreeCString(ctx, url);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http.get failed: %s",
                                      kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_client_response_free(&resp);
    return result;
}

/* Helper for POST/PUT/PATCH: (url, body, opts?) */
static JSValue js_http_body_method(JSContext *ctx, int argc, JSValueConst *argv,
                                    const char *method_name)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http.%s requires (url, body?, opts?)",
                                  method_name);

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    const char *body = NULL;
    size_t body_len = 0;
    if (argc >= 2 && JS_IsString(argv[1]))
        body = JS_ToCStringLen(ctx, &body_len, argv[1]);

    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(js->base.http_cfg, method_name, url,
                                    headers, num_headers, body, body_len, &resp);
    JS_FreeCString(ctx, url);
    // cppcheck-suppress knownConditionTrueFalse
    if (body) JS_FreeCString(ctx, body);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http.%s failed: %s",
                                      method_name, kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_client_response_free(&resp);
    return result;
}

static JSValue js_http_post(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_http_body_method(ctx, argc, argv, "POST");
}

static JSValue js_http_put(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_http_body_method(ctx, argc, argv, "PUT");
}

static JSValue js_http_patch(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_http_body_method(ctx, argc, argv, "PATCH");
}

/* http.del(url, opts?) — same as http.get but DELETE */
static JSValue js_http_del(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http.del requires (url, opts?)");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(js->base.http_cfg, "DELETE", url,
                                    headers, num_headers, NULL, 0, &resp);
    JS_FreeCString(ctx, url);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http.del failed: %s",
                                      kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_client_response_free(&resp);
    return result;
}

/* ── Push async HTTP response into JS ─────────────────────────────── */

static JSValue js_push_async_http_response(JSContext *ctx, void *driver)
{
    const KlClientResponse *resp = hl_http_async_response(driver);
    if (!resp)
        return JS_UNDEFINED;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, resp->status));

    if (resp->body && resp->body_len > 0)
        JS_SetPropertyStr(ctx, obj, "body",
                          JS_NewStringLen(ctx, resp->body, resp->body_len));
    else
        JS_SetPropertyStr(ctx, obj, "body", JS_NewString(ctx, ""));

    /* Headers as { "name": "value" } — lowercase names */
    JSValue headers = JS_NewObject(ctx);
    for (int i = 0; i < resp->num_headers; i++) {
        size_t nlen = strlen(resp->headers[i].name);
        char *lower = js_malloc(ctx, nlen + 1);
        if (lower) {
            for (size_t j = 0; j < nlen; j++)
                lower[j] = (char)((resp->headers[i].name[j] >= 'A' &&
                                    resp->headers[i].name[j] <= 'Z')
                    ? resp->headers[i].name[j] + 32
                    : resp->headers[i].name[j]);
            lower[nlen] = '\0';
            JS_SetPropertyStr(ctx, headers, lower,
                              JS_NewString(ctx, resp->headers[i].value));
            js_free(ctx, lower);
        } else {
            JS_SetPropertyStr(ctx, headers, resp->headers[i].name,
                              JS_NewString(ctx, resp->headers[i].value));
        }
    }
    JS_SetPropertyStr(ctx, obj, "headers", headers);

    return obj;
}

/* http.fetch(method, url, opts?) — async non-blocking HTTP request.
 * Returns a Promise; event loop drives socket I/O via KlWatcher.
 * Resolves with { status, body, headers }. */
static JSValue js_http_fetch(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured (no hosts in manifest)");
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx,
            "http.fetch() can only be called from a request handler");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "http.fetch requires (method, url, opts?)");

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!method || !url) {
        if (method) JS_FreeCString(ctx, method);
        if (url) JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    const char *body = NULL;
    size_t body_len = 0;
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse opts */
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue body_val = JS_GetPropertyStr(ctx, argv[2], "body");
        if (JS_IsString(body_val))
            body = JS_ToCStringLen(ctx, &body_len, body_val);
        JS_FreeValue(ctx, body_val);

        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    /* Start async HTTP — checks allowlist, creates KlClient, suspends conn */
    HlAsyncCtx *async_ctx = hl_async_http_start(
        js->server, js->active_conn, js->base.alloc,
        js->base.http_cfg, method, url, headers, num_headers, body, body_len);

    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    // cppcheck-suppress knownConditionTrueFalse
    if (body) JS_FreeCString(ctx, body);
    js_free_http_headers(ctx, headers, num_headers);

    if (!async_ctx)
        return JS_ThrowInternalError(ctx, "http.fetch: failed to start request");

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;

    /* Create JS continuation */
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_async_http_response);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "http.fetch: out of memory");
    }
    async_ctx->cont = cont;

    return promise;
}

/* ── http.async.* — async HTTP convenience methods ─────────────────── */

/* http.async.request(method, url, opts?) — same as http.fetch */
static JSValue js_http_async_request(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    return js_http_fetch(ctx, this_val, argc, argv);
}

/* Helper: build args array with method prepended, delegate to js_http_fetch.
 * For GET/DELETE: (url, opts?) → ("METHOD", url, opts?)
 * For POST/PUT/PATCH: (url, body, opts?) → ("METHOD", url, {body, headers}) */
static JSValue js_http_async_no_body(JSContext *ctx, JSValueConst this_val,
                                      const char *method,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    /* Build: fetch(method, url, opts?) */
    JSValue method_val = JS_NewString(ctx, method);
    JSValue args[3];
    args[0] = method_val;
    int nargs = 1;
    for (int i = 0; i < argc && nargs < 3; i++)
        args[nargs++] = argv[i];

    JSValue result = js_http_fetch(ctx, JS_UNDEFINED, nargs, args);
    JS_FreeValue(ctx, method_val);
    return result;
}

static JSValue js_http_async_with_body(JSContext *ctx, JSValueConst this_val,
                                        const char *method,
                                        int argc, JSValueConst *argv)
{
    (void)this_val;
    /* Build: fetch(method, url, {body, headers})
     * Caller args: (url, body, opts?)
     * fetch args: (method, url, {body=body, headers=opts.headers}) */
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http.async.%s requires (url, body?, opts?)",
                                 method);

    JSValue method_val = JS_NewString(ctx, method);

    /* Build opts object merging body into it */
    JSValue opts = JS_NewObject(ctx);
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
        JS_SetPropertyStr(ctx, opts, "body", JS_DupValue(ctx, argv[1]));
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue hdrs = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (!JS_IsUndefined(hdrs))
            JS_SetPropertyStr(ctx, opts, "headers", hdrs);
        else
            JS_FreeValue(ctx, hdrs);
    }

    JSValue args[3] = { method_val, argv[0], opts };
    JSValue result = js_http_fetch(ctx, JS_UNDEFINED, 3, args);
    JS_FreeValue(ctx, method_val);
    JS_FreeValue(ctx, opts);
    return result;
}

static JSValue js_http_async_get(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{ return js_http_async_no_body(ctx, this_val, "GET", argc, argv); }

static JSValue js_http_async_post(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{ return js_http_async_with_body(ctx, this_val, "POST", argc, argv); }

static JSValue js_http_async_put(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{ return js_http_async_with_body(ctx, this_val, "PUT", argc, argv); }

static JSValue js_http_async_patch(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{ return js_http_async_with_body(ctx, this_val, "PATCH", argc, argv); }

static JSValue js_http_async_delete(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{ return js_http_async_no_body(ctx, this_val, "DELETE", argc, argv); }

static int js_http_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue http = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, http, "request",
                      JS_NewCFunction(ctx, js_http_request, "request", 3));
    JS_SetPropertyStr(ctx, http, "get",
                      JS_NewCFunction(ctx, js_http_get, "get", 2));
    JS_SetPropertyStr(ctx, http, "post",
                      JS_NewCFunction(ctx, js_http_post, "post", 3));
    JS_SetPropertyStr(ctx, http, "put",
                      JS_NewCFunction(ctx, js_http_put, "put", 3));
    JS_SetPropertyStr(ctx, http, "patch",
                      JS_NewCFunction(ctx, js_http_patch, "patch", 3));
    JS_SetPropertyStr(ctx, http, "del",
                      JS_NewCFunction(ctx, js_http_del, "del", 2));
    /* Also expose as http["delete"] for JS compatibility */
    JS_SetPropertyStr(ctx, http, "delete",
                      JS_NewCFunction(ctx, js_http_del, "delete", 2));
    JS_SetPropertyStr(ctx, http, "fetch",
                      JS_NewCFunction(ctx, js_http_fetch, "fetch", 3));

    /* http.async sub-object */
    JSValue async_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, async_obj, "request",
                      JS_NewCFunction(ctx, js_http_async_request, "request", 3));
    JS_SetPropertyStr(ctx, async_obj, "get",
                      JS_NewCFunction(ctx, js_http_async_get, "get", 2));
    JS_SetPropertyStr(ctx, async_obj, "post",
                      JS_NewCFunction(ctx, js_http_async_post, "post", 3));
    JS_SetPropertyStr(ctx, async_obj, "put",
                      JS_NewCFunction(ctx, js_http_async_put, "put", 3));
    JS_SetPropertyStr(ctx, async_obj, "patch",
                      JS_NewCFunction(ctx, js_http_async_patch, "patch", 3));
    JS_SetPropertyStr(ctx, async_obj, "delete",
                      JS_NewCFunction(ctx, js_http_async_delete, "delete", 2));
    JS_SetPropertyStr(ctx, http, "async", async_obj);

    JS_SetModuleExport(ctx, m, "http", http);
    return 0;
}

int hl_js_init_http_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:http", js_http_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "http");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:smtp module
 *
 * smtp.send(opts) → { ok: true } or { ok: false, error: "..." }
 * ════════════════════════════════════════════════════════════════════ */

/* Helper: extract JS string array from an Array object */
static int js_get_string_array(JSContext *ctx, JSValueConst arr,
                               const char ***out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (JS_IsUndefined(arr) || JS_IsNull(arr))
        return 0;

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    if (len <= 0)
        return 0;

    const char **strs = js_mallocz(ctx, (size_t)len * sizeof(const char *));
    if (!strs)
        return -1;

    int count = 0;
    for (int32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        if (JS_IsString(v)) {
            strs[count] = JS_ToCString(ctx, v);
            if (strs[count])
                count++;
        }
        JS_FreeValue(ctx, v);
    }

    *out = strs;
    *out_count = count;
    return 0;
}

static void js_free_string_array(JSContext *ctx, const char **strs, int count)
{
    if (!strs) return;
    for (int i = 0; i < count; i++) {
        if (strs[i])
            JS_FreeCString(ctx, strs[i]);
    }
    js_free(ctx, strs);
}

/* smtp.send(opts) */
static JSValue js_smtp_send(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.smtp_cfg)
        return JS_ThrowInternalError(ctx, "smtp not configured (no hosts in manifest)");

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "smtp.send requires an options object");

    JSValueConst opts = argv[0];

    /* Extract fields */
    JSValue v_host = JS_GetPropertyStr(ctx, opts, "host");
    JSValue v_port = JS_GetPropertyStr(ctx, opts, "port");
    JSValue v_user = JS_GetPropertyStr(ctx, opts, "username");
    JSValue v_pass = JS_GetPropertyStr(ctx, opts, "password");
    JSValue v_tls  = JS_GetPropertyStr(ctx, opts, "tls");
    JSValue v_from = JS_GetPropertyStr(ctx, opts, "from");
    JSValue v_to   = JS_GetPropertyStr(ctx, opts, "to");
    JSValue v_subj = JS_GetPropertyStr(ctx, opts, "subject");
    JSValue v_body = JS_GetPropertyStr(ctx, opts, "body");
    JSValue v_ct   = JS_GetPropertyStr(ctx, opts, "content_type");
    JSValue v_rto  = JS_GetPropertyStr(ctx, opts, "reply_to");
    JSValue v_cc   = JS_GetPropertyStr(ctx, opts, "cc");

    const char *host = JS_IsString(v_host) ? JS_ToCString(ctx, v_host) : NULL;
    int32_t port = 587;
    if (JS_IsNumber(v_port))
        JS_ToInt32(ctx, &port, v_port);
    const char *username = JS_IsString(v_user) ? JS_ToCString(ctx, v_user) : NULL;
    const char *password = JS_IsString(v_pass) ? JS_ToCString(ctx, v_pass) : NULL;

    int use_tls = 0;
    if (JS_IsBool(v_tls)) {
        use_tls = JS_ToBool(ctx, v_tls) ? 1 : 0;
    } else if (JS_IsNumber(v_tls)) {
        int32_t t = 0;
        JS_ToInt32(ctx, &t, v_tls);
        use_tls = t;
    }

    const char *from = JS_IsString(v_from) ? JS_ToCString(ctx, v_from) : NULL;
    const char *to   = JS_IsString(v_to)   ? JS_ToCString(ctx, v_to)   : NULL;
    const char *subject = JS_IsString(v_subj) ? JS_ToCString(ctx, v_subj) : NULL;
    const char *body = JS_IsString(v_body) ? JS_ToCString(ctx, v_body) : NULL;
    const char *content_type = JS_IsString(v_ct) ? JS_ToCString(ctx, v_ct) : NULL;
    const char *reply_to = JS_IsString(v_rto) ? JS_ToCString(ctx, v_rto) : NULL;

    /* CC array */
    const char **cc = NULL;
    int cc_count = 0;
    if (JS_IsArray(ctx, v_cc))
        js_get_string_array(ctx, v_cc, &cc, &cc_count);

    /* Free JS values */
    JS_FreeValue(ctx, v_host);
    JS_FreeValue(ctx, v_port);
    JS_FreeValue(ctx, v_user);
    JS_FreeValue(ctx, v_pass);
    JS_FreeValue(ctx, v_tls);
    JS_FreeValue(ctx, v_from);
    JS_FreeValue(ctx, v_to);
    JS_FreeValue(ctx, v_subj);
    JS_FreeValue(ctx, v_body);
    JS_FreeValue(ctx, v_ct);
    JS_FreeValue(ctx, v_rto);
    JS_FreeValue(ctx, v_cc);

    /* Build result early for validation errors */
    JSValue result;

    if (!host || !from || !to || !subject || !body) {
        result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "ok", JS_FALSE);
        const char *missing = !host ? "host" : !from ? "from" :
                              !to ? "to" : !subject ? "subject" : "body";
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "%s required", missing);
        JS_SetPropertyStr(ctx, result, "error", JS_NewString(ctx, errbuf));
        goto cleanup;
    }

    /* Build message struct */
    HlSmtpMessage msg = {
        .host = host,
        .port = port,
        .username = username,
        .password = password,
        .use_tls = use_tls,
        .from = from,
        .to = to,
        .cc = cc,
        .cc_count = cc_count,
        .reply_to = reply_to,
        .subject = subject,
        .body = body,
        .content_type = content_type,
    };

    int rc = hl_cap_smtp_send(js->base.smtp_cfg, &msg);

    result = JS_NewObject(ctx);
    if (rc == 0) {
        JS_SetPropertyStr(ctx, result, "ok", JS_TRUE);
    } else {
        JS_SetPropertyStr(ctx, result, "ok", JS_FALSE);
        JS_SetPropertyStr(ctx, result, "error",
                          JS_NewString(ctx, "smtp send failed"));
    }

cleanup:
    if (host)         JS_FreeCString(ctx, host);
    if (username)     JS_FreeCString(ctx, username);
    if (password)     JS_FreeCString(ctx, password);
    if (from)         JS_FreeCString(ctx, from);
    if (to)           JS_FreeCString(ctx, to);
    if (subject)      JS_FreeCString(ctx, subject);
    if (body)         JS_FreeCString(ctx, body);
    if (content_type) JS_FreeCString(ctx, content_type);
    if (reply_to)     JS_FreeCString(ctx, reply_to);
    js_free_string_array(ctx, cc, cc_count);

    return result;
}

static int js_smtp_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue smtp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, smtp, "send",
                      JS_NewCFunction(ctx, js_smtp_send, "send", 1));
    JS_SetModuleExport(ctx, m, "smtp", smtp);
    return 0;
}

int hl_js_init_smtp_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:smtp", js_smtp_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "smtp");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:json module
 *
 * Wraps the built-in JSON.stringify/JSON.parse as:
 *   json.encode(value) → string
 *   json.decode(str)   → value
 * ════════════════════════════════════════════════════════════════════ */

static JSValue js_json_encode(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "json.encode requires (value)");

    JSValue result = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(result))
        return JS_EXCEPTION;
    /* JSON.stringify returns undefined for unsupported types */
    if (JS_IsUndefined(result))
        return JS_NewString(ctx, "null");
    return result;
}

static JSValue js_json_decode(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "json.decode requires (str)");

    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    JSValue result = JS_ParseJSON(ctx, str, strlen(str), "<json>");
    JS_FreeCString(ctx, str);
    return result;
}

static int js_json_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue json = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, json, "encode",
                      JS_NewCFunction(ctx, js_json_encode, "encode", 1));
    JS_SetPropertyStr(ctx, json, "decode",
                      JS_NewCFunction(ctx, js_json_decode, "decode", 1));
    JS_SetModuleExport(ctx, m, "json", json);
    return 0;
}

int hl_js_init_json_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:json", js_json_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "json");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:log module (placeholder — full implementation needs hl_cap_db)
 *
 * log.info(msg)  → logs to stderr (and DB when available)
 * log.warn(msg)
 * log.error(msg)
 * ════════════════════════════════════════════════════════════════════ */

static JSValue js_log_level(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    static const int levels[] = { LOG_INFO, LOG_WARN, LOG_ERROR, LOG_DEBUG };
    int level = (magic >= 0 && magic < 4) ? levels[magic] : LOG_INFO;

    /* Detect stdlib vs app: hull:* modules → [hull:js], else [app] */
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

/* ════════════════════════════════════════════════════════════════════
 * hull:_template module (internal — called only by hull:template stdlib)
 *
 * _template.compile(code, name?)   → compiled JS function
 * _template.loadRaw(name)          → raw template string or null
 * ════════════════════════════════════════════════════════════════════ */

/* VFS: O(log n) lookups into sorted entry arrays */
#include "hull/vfs.h"

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
        /* Reject ".." components to prevent path traversal (R1 fix).
         * We use string-level validation rather than realpath() because
         * the OS sandbox may block stat() on parent directories. */
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
            FILE *f = fopen(path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
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

/* ════════════════════════════════════════════════════════════════════
 * hull:worker module
 *
 * worker.dispatch(fn, ctx) — dispatch fn to a worker thread
 * ════════════════════════════════════════════════════════════════════ */

/* Deep-copy a JS object (own string properties only, flat) to HlKV array. */
static int js_object_to_kv(JSContext *ctx, JSValueConst obj,
                            HlKV **out_kvs, int *out_count)
{
    *out_kvs = NULL;
    *out_count = 0;

    if (JS_IsNull(obj) || JS_IsUndefined(obj))
        return 0;

    JSPropertyEnum *tab = NULL;
    uint32_t tab_len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, obj,
                                JS_GPN_STRING_MASK |
                                JS_GPN_ENUM_ONLY) < 0)
        return -1;

    if (tab_len == 0) {
        js_free(ctx, tab);
        return 0;
    }

    HlKV *kvs = calloc(tab_len, sizeof(HlKV));
    if (!kvs) {
        for (uint32_t i = 0; i < tab_len; i++)
            JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
        return -1;
    }

    int count = 0;
    for (uint32_t i = 0; i < tab_len; i++) {
        const char *key = JS_AtomToCString(ctx, tab[i].atom);
        if (!key) {
            JS_FreeAtom(ctx, tab[i].atom);
            continue;
        }

        kvs[count].key = strdup(key);
        JS_FreeCString(ctx, key);

        JSValue val = JS_GetProperty(ctx, obj, tab[i].atom);
        JS_FreeAtom(ctx, tab[i].atom);

        if (JS_IsBool(val)) {
            kvs[count].value.type = HL_TYPE_BOOL;
            kvs[count].value.b = JS_ToBool(ctx, val);
        } else if (JS_IsNumber(val)) {
            double d;
            JS_ToFloat64(ctx, &d, val);
            if (d == (double)(int64_t)d && d >= -9007199254740992.0 &&
                d <= 9007199254740992.0) {
                kvs[count].value.type = HL_TYPE_INT;
                kvs[count].value.i = (int64_t)d;
            } else {
                kvs[count].value.type = HL_TYPE_DOUBLE;
                kvs[count].value.d = d;
            }
        } else if (JS_IsString(val)) {
            size_t slen;
            const char *sv = JS_ToCStringLen(ctx, &slen, val);
            kvs[count].value.type = HL_TYPE_TEXT;
            if (sv) {
                kvs[count].value.s = malloc(slen + 1);
                if (kvs[count].value.s) {
                    memcpy((void *)kvs[count].value.s, sv, slen);
                    ((char *)kvs[count].value.s)[slen] = '\0';
                    kvs[count].value.len = slen;
                }
                JS_FreeCString(ctx, sv);
            }
        } else {
            kvs[count].value.type = HL_TYPE_NIL;
        }
        JS_FreeValue(ctx, val);
        count++;
    }
    js_free(ctx, tab);

    *out_kvs = kvs;
    *out_count = count;
    return 0;
}

/* push_result callback: convert HlJsWorkerDispatchOp result to JSValue */
static JSValue js_push_worker_dispatch_result(JSContext *ctx, void *driver)
{
    HlJsWorkerDispatchOp *op = (HlJsWorkerDispatchOp *)driver;

    if (op->error) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "error",
                          JS_NewString(ctx, op->error_msg));
        return obj;
    }

    switch (op->result_kind) {
    case 0: /* nil */
        return JS_NULL;
    case 1: /* bool */
        return JS_NewBool(ctx, op->result_bool);
    case 2: /* int */
        return JS_NewInt64(ctx, op->result_int);
    case 3: /* double */
        return JS_NewFloat64(ctx, op->result_double);
    case 4: /* string */
        return JS_NewStringLen(ctx, op->result_str, op->result_str_len);
    case 5: { /* table/object */
        JSValue obj = JS_NewObject(ctx);
        for (int i = 0; i < op->result_count; i++) {
            JSValue val;
            switch (op->result_kvs[i].value.type) {
            case HL_TYPE_INT:
                val = JS_NewInt64(ctx, op->result_kvs[i].value.i);
                break;
            case HL_TYPE_DOUBLE:
                val = JS_NewFloat64(ctx, op->result_kvs[i].value.d);
                break;
            case HL_TYPE_TEXT:
                val = JS_NewStringLen(ctx, op->result_kvs[i].value.s,
                                     op->result_kvs[i].value.len);
                break;
            case HL_TYPE_BOOL:
                val = JS_NewBool(ctx, op->result_kvs[i].value.b);
                break;
            default:
                val = JS_NULL;
                break;
            }
            JS_SetPropertyStr(ctx, obj, op->result_kvs[i].key, val);
        }
        return obj;
    }
    default:
        return JS_NULL;
    }
}

/* worker.dispatch(fn, ctx) — serialize fn + ctx, submit to thread pool */
static JSValue js_worker_dispatch(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx,
            "worker.dispatch not available (no thread pool)");
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx,
            "worker.dispatch can only be called from a request handler");

    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx,
            "worker.dispatch requires (fn, ctx?)");

    /* Get function source text via fn.toString() */
    JSValue fn_str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(fn_str))
        return JS_ThrowInternalError(ctx,
            "worker.dispatch: cannot serialize function");
    size_t src_len = 0;
    const char *src_cstr = JS_ToCStringLen(ctx, &src_len, fn_str);
    JS_FreeValue(ctx, fn_str);
    if (!src_cstr)
        return JS_ThrowInternalError(ctx,
            "worker.dispatch: cannot serialize function");

    /* Deep-copy ctx object */
    HlKV *ctx_kvs = NULL;
    int ctx_count = 0;
    if (argc >= 2 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        if (js_object_to_kv(ctx, argv[1], &ctx_kvs, &ctx_count) != 0) {
            JS_FreeCString(ctx, src_cstr);
            return JS_ThrowInternalError(ctx,
                "worker.dispatch: failed to serialize ctx object");
        }
    }

    /* Allocate dispatch op */
    HlJsWorkerDispatchOp *op = calloc(1, sizeof(HlJsWorkerDispatchOp));
    if (!op) {
        JS_FreeCString(ctx, src_cstr);
        hl_kv_free(ctx_kvs, ctx_count);
        return JS_ThrowInternalError(ctx, "worker.dispatch: out of memory");
    }

    op->server = js->server;
    op->alloc = js->base.alloc;
    /* Copy source text (JS_ToCStringLen returns js_malloc'd memory) */
    op->fn_source = malloc(src_len + 1);
    if (!op->fn_source) {
        JS_FreeCString(ctx, src_cstr);
        hl_kv_free(ctx_kvs, ctx_count);
        free(op);
        return JS_ThrowInternalError(ctx, "worker.dispatch: out of memory");
    }
    memcpy(op->fn_source, src_cstr, src_len);
    op->fn_source[src_len] = '\0';
    op->fn_source_len = src_len;
    JS_FreeCString(ctx, src_cstr);

    op->ctx_kvs = ctx_kvs;
    op->ctx_count = ctx_count;

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.alloc);
    if (!actx) {
        hl_js_worker_dispatch_op_free(op);
        free(op);
        return JS_ThrowInternalError(ctx, "worker.dispatch: out of memory");
    }

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_js_worker_dispatch_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    /* Create JS continuation */
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_worker_dispatch_result);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_js_worker_dispatch_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "worker.dispatch: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_js_worker_dispatch_op_free_all;
    actx->op.on_cancel = hl_js_worker_dispatch_cancel;

    op->async_ctx = actx;
    op->cancelled = 0;

    /* Submit to thread pool */
    if (hl_js_worker_dispatch_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_js_worker_dispatch_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "worker.dispatch: thread pool full");
    }

    /* Suspend the connection */
    if (kl_async_suspend(js->server, js->active_conn, &actx->op) < 0) {
        op->cancelled = 1;
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "worker.dispatch: failed to suspend connection");
    }

    return promise;
}

static int js_worker_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue worker = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, worker, "dispatch",
                      JS_NewCFunction(ctx, js_worker_dispatch, "dispatch", 2));
    JS_SetModuleExport(ctx, m, "worker", worker);
    return 0;
}

int hl_js_init_worker_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:worker", js_worker_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "worker");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:server — Server stats
 * ════════════════════════════════════════════════════════════════════ */

/* server.stats() → { activeConnections, maxConnections, asyncSuspended,
 *                     listenPaused } */
static JSValue js_server_stats(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;

    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->server)
        return JS_ThrowInternalError(ctx, "server.stats: server not available");

    KlServerStats stats;
    kl_server_stats(js->server, &stats);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "activeConnections",
                      JS_NewInt32(ctx, stats.active_connections));
    JS_SetPropertyStr(ctx, obj, "maxConnections",
                      JS_NewInt32(ctx, stats.max_connections));
    JS_SetPropertyStr(ctx, obj, "asyncSuspended",
                      JS_NewInt32(ctx, stats.async_suspended));
    JS_SetPropertyStr(ctx, obj, "listenPaused",
                      JS_NewBool(ctx, stats.listen_paused));
    return obj;
}

static int js_server_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue server = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, server, "stats",
                      JS_NewCFunction(ctx, js_server_stats, "stats", 0));
    JS_SetModuleExport(ctx, m, "server", server);
    return 0;
}

static int hl_js_init_server_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:server", js_server_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "server");
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * hull:compute module — WASM compute plugins
 *
 * compute.call(name, input, opts?) -> output (Uint8Array)
 * compute.preload(name) -> true
 * ════════════════════════════════════════════════════════════════════ */

#ifdef HL_ENABLE_WASM
/* ════════════════════════════════════════════════════════════════════
 * hull:fs module — filesystem capabilities
 *
 * fs.mmap(path) -> opaque MappedBuffer object
 * ════════════════════════════════════════════════════════════════════ */

#include "hull/cap/fs.h"

static JSClassID js_mmap_class_id;

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

static int hl_js_init_fs_module(JSContext *ctx, HlJS *js)
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

#include "hull/cap/wasm.h"
#include "hull/cap/wasm_buffer.h"

/* ── WasmBuffer JS class ──────────────────────────────────────────── */

static JSClassID js_wasm_buf_class_id;

static void js_wasm_buf_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlWasmBuffer *buf = JS_GetOpaque(val, js_wasm_buf_class_id);
    if (buf) {
        HlAllocator *a = buf->alloc;
        hl_wasm_buffer_destroy(buf);
        hl_alloc_free(a, buf, sizeof(HlWasmBuffer));
    }
}

static JSClassDef js_wasm_buf_class = {
    "WasmBuffer",
    .finalizer = js_wasm_buf_finalizer,
};

static JSValue js_wasm_buf_close(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmBuffer *buf = JS_GetOpaque2(ctx, this_val, js_wasm_buf_class_id);
    if (buf) {
        HlAllocator *a = buf->alloc;
        hl_wasm_buffer_destroy(buf);
        hl_alloc_free(a, buf, sizeof(HlWasmBuffer));
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

static JSValue js_wasm_buf_bytes(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmBuffer *buf = JS_GetOpaque2(ctx, this_val, js_wasm_buf_class_id);
    if (!buf || buf->closed)
        return JS_NULL;
    const void *data = hl_wasm_buffer_data(buf);
    size_t len = hl_wasm_buffer_len(buf);
    if (data && len > 0)
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)data, len);
    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

static JSValue js_wasm_buf_get_length(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmBuffer *buf = JS_GetOpaque2(ctx, this_val, js_wasm_buf_class_id);
    if (!buf || buf->closed) return JS_NewInt32(ctx, 0);
    return JS_NewInt64(ctx, (int64_t)hl_wasm_buffer_len(buf));
}

/* Helper: wrap HlWasmBuffer* as JS object. Takes ownership. */
static JSValue js_push_wasm_buffer(JSContext *ctx, HlWasmBuffer *buf)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)js_wasm_buf_class_id);
    if (JS_IsException(obj)) {
        HlAllocator *a = buf->alloc;
        hl_wasm_buffer_destroy(buf);
        hl_alloc_free(a, buf, sizeof(HlWasmBuffer));
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, buf);
    return obj;
}

static JSValue js_compute_buffer(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "compute.buffer requires (input)");

    size_t len = 0;
    const uint8_t *data = JS_GetArrayBuffer(ctx, &len, argv[0]);
    int is_string = 0;
    if (!data) {
        data = (const uint8_t *)JS_ToCStringLen(ctx, &len, argv[0]);
        if (!data)
            return JS_ThrowTypeError(ctx, "compute.buffer: input must be a string or ArrayBuffer");
        is_string = 1;
    }

    HlJS *js_ctx = (HlJS *)JS_GetContextOpaque(ctx);
    HlWasmBuffer *buf = hl_wasm_buffer_create_owned(data, len,
                                                      js_ctx ? js_ctx->base.alloc : NULL);
    if (is_string) JS_FreeCString(ctx, (const char *)data);

    if (!buf)
        return JS_ThrowInternalError(ctx, "compute.buffer: out of memory");

    return js_push_wasm_buffer(ctx, buf);
}

static void js_register_wasm_buf_class(JSContext *ctx)
{
    JS_NewClassID(&js_wasm_buf_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_wasm_buf_class_id, &js_wasm_buf_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "close",
                      JS_NewCFunction(ctx, js_wasm_buf_close, "close", 0));
    JS_SetPropertyStr(ctx, proto, "bytes",
                      JS_NewCFunction(ctx, js_wasm_buf_bytes, "bytes", 0));

    JSAtom length_atom = JS_NewAtom(ctx, "length");
    JS_DefinePropertyGetSet(ctx, proto, length_atom,
                            JS_NewCFunction(ctx, js_wasm_buf_get_length, "length", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, length_atom);

    JS_SetClassProto(ctx, js_wasm_buf_class_id, proto);
}

/* Clamp per-call opts to runtime ceiling */
static void js_wasm_clamp_opts(HlWasmCallOpts *opts, const HlRuntime *base)
{
    hl_cap_wasm_clamp_opts(opts,
        base->wasm_config.max_input, base->wasm_config.max_output,
        base->wasm_config.heap_size, base->wasm_config.stack_size,
        base->wasm_config.gas);
}

static JSValue js_compute_call(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.call: WASM runtime not initialized");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "compute.call requires (name, input [, opts])");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    /* Input can be a string, ArrayBuffer, WasmBuffer, or MappedBuffer */
    size_t input_len = 0;
    const uint8_t *input = NULL;
    int input_is_string = 0;

    /* Check for WasmBuffer first */
    HlWasmBuffer *wbuf_in = JS_GetOpaque2(ctx, argv[1], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        /* Check for MappedBuffer */
        HlMappedBuffer *mmap_buf = JS_GetOpaque2(ctx, argv[1], js_mmap_class_id);
        if (mmap_buf && !mmap_buf->closed) {
            input = (const uint8_t *)mmap_buf->addr;
            input_len = mmap_buf->len;
        } else {
            input = JS_GetArrayBuffer(ctx, &input_len, argv[1]);
            if (!input) {
                /* Try as string */
                input = (const uint8_t *)JS_ToCStringLen(ctx, &input_len, argv[1]);
                if (!input) {
                    JS_FreeCString(ctx, name);
                    return JS_ThrowTypeError(ctx, "compute.call: input must be a string, ArrayBuffer, WasmBuffer, or MappedBuffer");
                }
                input_is_string = 1;
            }
        }
    }

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;

    /* Parse opts object if provided */
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue val;

        val = JS_GetPropertyStr(ctx, argv[2], "maxInput");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "maxOutput");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "heap");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.heap_size = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "stack");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.stack_size = (uint32_t)v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "gas");
        if (!JS_IsUndefined(val)) {
            int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v;
        }
        JS_FreeValue(ctx, val);

        val = JS_GetPropertyStr(ctx, argv[2], "buffer");
        if (JS_ToBool(ctx, val)) want_buffer = 1;
        JS_FreeValue(ctx, val);
    }

    js_wasm_clamp_opts(&opts, &js->base);

    if (want_buffer) {
        HlWasmBuffer *out_buf = NULL;
        const char *err_msg = NULL;

        int rc = hl_cap_wasm_call_buf(js->base.wasm_cache, name,
                                       input, input_len,
                                       &out_buf, &opts, NULL, NULL,
                                       js->base.app_vfs,
                                       js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                                       js->base.alloc, &err_msg);

        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        JS_FreeCString(ctx, name);

        if (rc != 0)
            return JS_ThrowInternalError(ctx, "compute.call: %s",
                                         err_msg ? err_msg : "unknown error");

        return js_push_wasm_buffer(ctx, out_buf);
    }

    /* Non-buffer path (original behavior) */
    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    int rc = hl_cap_wasm_call(js->base.wasm_cache, name,
                               input, input_len,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               js->base.app_vfs,
                               js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                               js->base.alloc, &err_msg);

    if (input_is_string)
        JS_FreeCString(ctx, (const char *)input);
    JS_FreeCString(ctx, name);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "compute.call: %s",
                                     err_msg ? err_msg : "unknown error");

    /* Return result as ArrayBuffer */
    if (output && output_len > 0) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, output, output_len);
        hl_alloc_free(js->base.alloc, output, output_len);
        return ab;
    }
    hl_alloc_free(js->base.alloc, output, output_len);
    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

static JSValue js_compute_preload(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.preload: WASM runtime not initialized");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "compute.preload requires (name)");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    int rc = hl_cap_wasm_load(js->base.wasm_cache, name,
                               js->base.app_vfs,
                               js->base.app_vfs ? js->base.app_vfs->root_dir : NULL);
    JS_FreeCString(ctx, name);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "compute.preload: %s",
                                     rc == HL_WASM_ERR_NOT_FOUND ? "not_found" : "load_failed");

    return JS_TRUE;
}

/* ── compute.async.call (Promise-based, thread pool dispatch) ──────── */

static JSValue js_push_worker_wasm_result(JSContext *ctx, void *driver)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)driver;

    if (op->error)
        return JS_ThrowInternalError(ctx, "compute.async.call: %s", op->error_msg);

    if (op->output_buf) {
        /* Buffer mode: wrap as WasmBuffer, transfer ownership */
        HlWasmBuffer *buf = op->output_buf;
        op->output_buf = NULL; /* prevent hl_worker_wasm_op_free from destroying */
        return js_push_wasm_buffer(ctx, buf);
    }

    if (op->output && op->output_len > 0)
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)op->output, op->output_len);

    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

static JSValue js_compute_async_call(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx, "compute.async not available (no thread pool)");
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx, "compute.async can only be called from a request handler");
    if (!js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.async.call: WASM runtime not initialized");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "compute.async.call requires (name, input [, opts])");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    /* Get input (string, ArrayBuffer, or WasmBuffer) */
    const uint8_t *input = NULL;
    size_t input_len = 0;
    int input_is_string = 0;

    HlWasmBuffer *wbuf_in = JS_GetOpaque2(ctx, argv[1], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer *mmap_in = JS_GetOpaque2(ctx, argv[1], js_mmap_class_id);
        if (mmap_in && !mmap_in->closed) {
            input = (const uint8_t *)mmap_in->addr;
            input_len = mmap_in->len;
        } else {
            size_t ab_len;
            input = JS_GetArrayBuffer(ctx, &ab_len, argv[1]);
            if (input) {
                input_len = ab_len;
            } else {
                input = (const uint8_t *)JS_ToCStringLen(ctx, &input_len, argv[1]);
                if (!input) {
                    JS_FreeCString(ctx, name);
                    return JS_ThrowTypeError(ctx, "compute.async.call: input must be a string, ArrayBuffer, WasmBuffer, or MappedBuffer");
                }
                input_is_string = 1;
            }
        }
    }

    /* Parse opts */
    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue val;
        val = JS_GetPropertyStr(ctx, argv[2], "maxInput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "maxOutput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "heap");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.heap_size = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "stack");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.stack_size = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "gas");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[2], "buffer");
        if (JS_ToBool(ctx, val)) want_buffer = 1;
        JS_FreeValue(ctx, val);
    }

    js_wasm_clamp_opts(&opts, &js->base);

    /* Pre-load module on event loop thread (cache writes are not thread-safe) */
    int rc = hl_cap_wasm_load(js->base.wasm_cache, name,
                               js->base.app_vfs,
                               js->base.app_vfs ? js->base.app_vfs->root_dir : NULL);
    if (rc != 0) {
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "compute.async.call: %s",
                                     rc == HL_WASM_ERR_NOT_FOUND ? "not_found" : "load_failed");
    }

    /* Allocate op */
    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    if (!op) {
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        JS_FreeCString(ctx, name);
        return JS_ThrowInternalError(ctx, "compute.async.call: out of memory");
    }

    op->server = js->server;
    op->wasm_cache = js->base.wasm_cache;
    op->app_vfs = js->base.app_vfs;
    op->app_dir = js->base.app_vfs ? js->base.app_vfs->root_dir : NULL;
    op->alloc = js->base.alloc;
    snprintf(op->name, sizeof(op->name), "%s", name);
    op->opts = opts;
    op->want_buffer = want_buffer;

    /* Deep-copy input */
    if (input_len > 0) {
        op->input = malloc(input_len);
        if (!op->input) {
            if (input_is_string) JS_FreeCString(ctx, (const char *)input);
            JS_FreeCString(ctx, name);
            free(op);
            return JS_ThrowInternalError(ctx, "compute.async.call: out of memory");
        }
        memcpy(op->input, input, input_len);
    }
    op->input_len = input_len;

    if (input_is_string) JS_FreeCString(ctx, (const char *)input);
    JS_FreeCString(ctx, name);

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.alloc);
    if (!actx) {
        hl_worker_wasm_op_free(op);
        free(op);
        return JS_ThrowInternalError(ctx, "compute.async.call: out of memory");
    }

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    /* Create JS continuation */
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_worker_wasm_result);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "compute.async.call: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_wasm_op_free_all;
    actx->op.on_cancel = hl_worker_wasm_async_cancel;

    op->async_ctx = actx;
    op->cancelled = 0;

    /* Submit to thread pool */
    if (hl_worker_wasm_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "compute.async.call: thread pool full");
    }

    /* Suspend connection */
    if (kl_async_suspend(js->server, js->active_conn, &actx->op) < 0) {
        op->cancelled = 1;
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "compute.async.call: failed to suspend");
    }

    return promise;
}

/* ── WasmInstance JS class ──────────────────────────────────────────── */

static JSClassID js_wasm_inst_class_id;

static void js_wasm_inst_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlWasmInstance *pi = JS_GetOpaque(val, js_wasm_inst_class_id);
    if (pi)
        hl_cap_wasm_instance_destroy(pi);
}

static JSClassDef js_wasm_inst_class = {
    "WasmInstance",
    .finalizer = js_wasm_inst_finalizer,
};

static JSValue js_wasm_inst_call(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    if (!pi)
        return JS_ThrowInternalError(ctx, "WasmInstance.call: invalid instance");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WasmInstance.call requires (input [, opts])");

    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);

    /* Parse input */
    size_t input_len = 0;
    const uint8_t *input = NULL;
    int input_is_string = 0;

    HlWasmBuffer *wbuf_in = JS_GetOpaque2(ctx, argv[0], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer *mmap_in = JS_GetOpaque2(ctx, argv[0], js_mmap_class_id);
        if (mmap_in && !mmap_in->closed) {
            input = (const uint8_t *)mmap_in->addr;
            input_len = mmap_in->len;
        } else {
            input = JS_GetArrayBuffer(ctx, &input_len, argv[0]);
            if (!input) {
                input = (const uint8_t *)JS_ToCStringLen(ctx, &input_len, argv[0]);
                if (!input)
                    return JS_ThrowTypeError(ctx, "WasmInstance.call: input must be a string, ArrayBuffer, WasmBuffer, or MappedBuffer");
                input_is_string = 1;
            }
        }
    }

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue val;
        val = JS_GetPropertyStr(ctx, argv[1], "gas");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxInput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxOutput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "buffer");
        if (JS_ToBool(ctx, val)) want_buffer = 1;
        JS_FreeValue(ctx, val);
    }

    if (js) js_wasm_clamp_opts(&opts, &js->base);

    if (want_buffer) {
        HlWasmBuffer *out_buf = NULL;
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_instance_call_buf(pi, input, input_len,
                                                &out_buf, &opts, NULL, NULL,
                                                js ? js->base.alloc : NULL, &err_msg);
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        if (rc != 0)
            return JS_ThrowInternalError(ctx, "WasmInstance.call: %s",
                                         err_msg ? err_msg : "unknown error");
        return js_push_wasm_buffer(ctx, out_buf);
    }

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;
    int rc = hl_cap_wasm_instance_call(pi, input, input_len,
                                        &output, &output_len,
                                        &opts, NULL, NULL,
                                        js ? js->base.alloc : NULL, &err_msg);
    if (input_is_string) JS_FreeCString(ctx, (const char *)input);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "WasmInstance.call: %s",
                                     err_msg ? err_msg : "unknown error");

    if (output && output_len > 0) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, output, output_len);
        if (js) hl_alloc_free(js->base.alloc, output, output_len);
        else free(output);
        return ab;
    }
    if (js) hl_alloc_free(js->base.alloc, output, output_len);
    else free(output);
    return JS_NewArrayBufferCopy(ctx, NULL, 0);
}

static JSValue js_wasm_inst_async_call(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.thread_pool)
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: no thread pool");
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: can only be called from a request handler");

    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    if (!pi || pi->closed)
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: instance closed");
    if (atomic_load(&pi->busy))
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: instance busy");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "WasmInstance.asyncCall requires (input [, opts])");

    /* Parse input */
    const uint8_t *input = NULL;
    size_t input_len = 0;
    int input_is_string = 0;

    HlWasmBuffer *wbuf_in = JS_GetOpaque2(ctx, argv[0], js_wasm_buf_class_id);
    if (wbuf_in && !wbuf_in->closed) {
        input = (const uint8_t *)hl_wasm_buffer_data(wbuf_in);
        input_len = hl_wasm_buffer_len(wbuf_in);
    } else {
        HlMappedBuffer *mmap_in = JS_GetOpaque2(ctx, argv[0], js_mmap_class_id);
        if (mmap_in && !mmap_in->closed) {
            input = (const uint8_t *)mmap_in->addr;
            input_len = mmap_in->len;
        } else {
            input = JS_GetArrayBuffer(ctx, &input_len, argv[0]);
            if (!input) {
                input = (const uint8_t *)JS_ToCStringLen(ctx, &input_len, argv[0]);
                if (!input)
                    return JS_ThrowTypeError(ctx, "WasmInstance.async.call: input must be a string, ArrayBuffer, WasmBuffer, or MappedBuffer");
                input_is_string = 1;
            }
        }
    }

    HlWasmCallOpts opts = {0};
    int want_buffer = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue val;
        val = JS_GetPropertyStr(ctx, argv[1], "gas");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxInput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxOutput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "buffer");
        if (JS_ToBool(ctx, val)) want_buffer = 1;
        JS_FreeValue(ctx, val);
    }
    js_wasm_clamp_opts(&opts, &js->base);

    /* Allocate op */
    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    if (!op) {
        if (input_is_string) JS_FreeCString(ctx, (const char *)input);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: out of memory");
    }

    op->server = js->server;
    op->persistent_inst = pi;
    op->alloc = js->base.alloc;
    snprintf(op->name, sizeof(op->name), "%s", pi->name);
    op->opts = opts;
    op->want_buffer = want_buffer;

    /* Deep-copy input */
    if (input_len > 0) {
        op->input = malloc(input_len);
        if (!op->input) {
            if (input_is_string) JS_FreeCString(ctx, (const char *)input);
            free(op);
            return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: out of memory");
        }
        memcpy(op->input, input, input_len);
    }
    op->input_len = input_len;

    if (input_is_string) JS_FreeCString(ctx, (const char *)input);

    /* Set busy before dispatch */
    atomic_store(&pi->busy, 1);

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.alloc);
    if (!actx) {
        atomic_store(&pi->busy, 0);
        hl_worker_wasm_op_free(op);
        free(op);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: out of memory");
    }

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        atomic_store(&pi->busy, 0);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_worker_wasm_result);
    if (!cont) {
        atomic_store(&pi->busy, 0);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: out of memory");
    }
    actx->cont = cont;
    actx->driver = op;
    actx->free_driver = hl_worker_wasm_op_free_all;
    actx->op.on_cancel = hl_worker_wasm_async_cancel;

    op->async_ctx = actx;
    op->cancelled = 0;

    if (hl_worker_wasm_submit(js->base.thread_pool, op) != 0) {
        atomic_store(&pi->busy, 0);
        actx->cont->destroy(actx->cont);
        hl_worker_wasm_op_free(op);
        free(op);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: thread pool full");
    }

    if (kl_async_suspend(js->server, js->active_conn, &actx->op) < 0) {
        atomic_store(&pi->busy, 0);
        op->cancelled = 1;
        actx->cont->cancel(actx->cont);
        actx->cont->destroy(actx->cont);
        actx->cont = NULL;
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "WasmInstance.asyncCall: failed to suspend");
    }

    return promise;
}

static JSValue js_wasm_inst_close(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    if (pi) {
        hl_cap_wasm_instance_destroy(pi);
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

static JSValue js_wasm_inst_get_name(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    if (!pi || pi->closed) return JS_NULL;
    return JS_NewString(ctx, pi->name);
}

static JSValue js_wasm_inst_get_closed(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlWasmInstance *pi = JS_GetOpaque2(ctx, this_val, js_wasm_inst_class_id);
    return JS_NewBool(ctx, !pi || pi->closed);
}

static JSValue js_push_wasm_instance(JSContext *ctx, HlWasmInstance *pi)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)js_wasm_inst_class_id);
    if (JS_IsException(obj)) {
        hl_cap_wasm_instance_destroy(pi);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, pi);
    return obj;
}

/* inst.async.call(input, opts?) — proxy that forwards to async_call with the instance.
 * The instance JSValue is passed as magic data[0]. */
static JSValue js_wasm_inst_async_proxy_call(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv,
                                              int magic, JSValueConst *func_data)
{
    (void)this_val; (void)magic;
    /* func_data[0] is the instance object — call async_call with it as this */
    return js_wasm_inst_async_call(ctx, func_data[0], argc, argv);
}

/* Getter for inst.async — returns { call: <bound to instance> } */
static JSValue js_wasm_inst_get_async(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue async_obj = JS_NewObject(ctx);
    JSValue inst = JS_DupValue(ctx, this_val);
    JS_SetPropertyStr(ctx, async_obj, "call",
                      JS_NewCFunctionData(ctx, js_wasm_inst_async_proxy_call, 2, 0, 1, &inst));
    JS_FreeValue(ctx, inst);
    return async_obj;
}

static void js_register_wasm_inst_class(JSContext *ctx)
{
    JS_NewClassID(&js_wasm_inst_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_wasm_inst_class_id, &js_wasm_inst_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "call",
                      JS_NewCFunction(ctx, js_wasm_inst_call, "call", 2));
    JS_SetPropertyStr(ctx, proto, "close",
                      JS_NewCFunction(ctx, js_wasm_inst_close, "close", 0));

    /* Getters: name, closed, async */
    JSAtom async_atom = JS_NewAtom(ctx, "async");
    JS_DefinePropertyGetSet(ctx, proto, async_atom,
                            JS_NewCFunction(ctx, js_wasm_inst_get_async, "async", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, async_atom);

    JSAtom name_atom = JS_NewAtom(ctx, "name");
    JS_DefinePropertyGetSet(ctx, proto, name_atom,
                            JS_NewCFunction(ctx, js_wasm_inst_get_name, "name", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, name_atom);

    JSAtom closed_atom = JS_NewAtom(ctx, "closed");
    JS_DefinePropertyGetSet(ctx, proto, closed_atom,
                            JS_NewCFunction(ctx, js_wasm_inst_get_closed, "closed", 0),
                            JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, closed_atom);

    JS_SetClassProto(ctx, js_wasm_inst_class_id, proto);
}

/* compute.instance(name, opts?) -> WasmInstance | throws */
static JSValue js_compute_instance(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.instance: WASM runtime not initialized");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "compute.instance requires (name [, opts])");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    HlWasmCallOpts opts = {0};
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue val;
        val = JS_GetPropertyStr(ctx, argv[1], "heap");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.heap_size = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "stack");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.stack_size = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "gas");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.gas = v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxInput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_input = (uint32_t)v; }
        JS_FreeValue(ctx, val);
        val = JS_GetPropertyStr(ctx, argv[1], "maxOutput");
        if (!JS_IsUndefined(val)) { int64_t v; JS_ToInt64(ctx, &v, val); opts.max_output = (uint32_t)v; }
        JS_FreeValue(ctx, val);
    }

    js_wasm_clamp_opts(&opts, &js->base);

    const char *err_msg = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(
        js->base.wasm_cache, name, &opts,
        js->base.app_vfs,
        js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
        js->base.alloc, &err_msg);

    JS_FreeCString(ctx, name);

    if (!pi)
        return JS_ThrowInternalError(ctx, "compute.instance: %s",
                                     err_msg ? err_msg : "unknown error");

    return js_push_wasm_instance(ctx, pi);
}

/* compute.data(module, segment, data) */
static JSValue js_compute_data(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.wasm_cache)
        return JS_ThrowInternalError(ctx, "compute.data: WASM runtime not initialized");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "compute.data requires (module, segment [, data])");

    const char *module_name = JS_ToCString(ctx, argv[0]);
    if (!module_name)
        return JS_EXCEPTION;

    /* compute.data(module, null) → remove all */
    if (JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_data_load(js->base.wasm_cache, module_name,
                                        NULL, NULL, 0, NULL,
                                        js->base.app_vfs,
                                        js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                                        &err_msg);
        JS_FreeCString(ctx, module_name);
        if (rc != 0)
            return JS_ThrowInternalError(ctx, "compute.data: %s",
                                         err_msg ? err_msg : "unknown error");
        return JS_TRUE;
    }

    const char *segment_name = JS_ToCString(ctx, argv[1]);
    if (!segment_name) {
        JS_FreeCString(ctx, module_name);
        return JS_EXCEPTION;
    }

    /* compute.data(module, segment, null/undefined) → remove segment */
    if (argc < 3 || JS_IsNull(argv[2]) || JS_IsUndefined(argv[2])) {
        const char *err_msg = NULL;
        int rc = hl_cap_wasm_data_load(js->base.wasm_cache, module_name,
                                        segment_name, NULL, 0, NULL,
                                        js->base.app_vfs,
                                        js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                                        &err_msg);
        JS_FreeCString(ctx, module_name);
        JS_FreeCString(ctx, segment_name);
        if (rc != 0)
            return JS_ThrowInternalError(ctx, "compute.data: %s",
                                         err_msg ? err_msg : "unknown error");
        return JS_TRUE;
    }

    /* compute.data(module, segment, data) → add/replace */
    const void *data = NULL;
    size_t data_len = 0;
    void *pre_alloc = NULL;
    uint8_t *ab_buf = NULL;

    if (JS_IsString(argv[2])) {
        data = JS_ToCStringLen(ctx, &data_len, argv[2]);
        if (!data) {
            JS_FreeCString(ctx, module_name);
            JS_FreeCString(ctx, segment_name);
            return JS_EXCEPTION;
        }
    } else {
        /* Try MappedBuffer (zero-copy) */
        HlMappedBuffer *mmap_buf = JS_GetOpaque2(ctx, argv[2], js_mmap_class_id);
        if (mmap_buf && !mmap_buf->closed) {
            data = mmap_buf->addr;
            data_len = mmap_buf->len;
            pre_alloc = mmap_buf->addr;
        } else {
            /* Try ArrayBuffer */
            ab_buf = JS_GetArrayBuffer(ctx, &data_len, argv[2]);
            if (ab_buf) {
                data = ab_buf;
            } else {
                JS_FreeCString(ctx, module_name);
                JS_FreeCString(ctx, segment_name);
                return JS_ThrowTypeError(ctx, "compute.data: data must be a string, ArrayBuffer, or MappedBuffer");
            }
        }
    }

    const char *err_msg = NULL;
    int rc = hl_cap_wasm_data_load(js->base.wasm_cache, module_name,
                                    segment_name, data, data_len, pre_alloc,
                                    js->base.app_vfs,
                                    js->base.app_vfs ? js->base.app_vfs->root_dir : NULL,
                                    &err_msg);

    if (JS_IsString(argv[2]))
        JS_FreeCString(ctx, (const char *)data);
    JS_FreeCString(ctx, module_name);
    JS_FreeCString(ctx, segment_name);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "compute.data: %s",
                                     err_msg ? err_msg : "unknown error");
    return JS_TRUE;
}

static int js_compute_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue compute = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, compute, "call",
                      JS_NewCFunction(ctx, js_compute_call, "call", 3));
    JS_SetPropertyStr(ctx, compute, "preload",
                      JS_NewCFunction(ctx, js_compute_preload, "preload", 1));
    JS_SetPropertyStr(ctx, compute, "buffer",
                      JS_NewCFunction(ctx, js_compute_buffer, "buffer", 1));
    JS_SetPropertyStr(ctx, compute, "instance",
                      JS_NewCFunction(ctx, js_compute_instance, "instance", 2));
    JS_SetPropertyStr(ctx, compute, "data",
                      JS_NewCFunction(ctx, js_compute_data, "data", 3));

    /* compute.async sub-object */
    JSValue async_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, async_obj, "call",
                      JS_NewCFunction(ctx, js_compute_async_call, "call", 3));
    JS_SetPropertyStr(ctx, compute, "async", async_obj);

    JS_SetModuleExport(ctx, m, "compute", compute);
    return 0;
}

static int hl_js_init_compute_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    js_register_wasm_buf_class(ctx);
    js_register_wasm_inst_class(ctx);
    JSModuleDef *m = JS_NewCModule(ctx, "hull:compute", js_compute_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "compute");
    return 0;
}
#endif /* HL_ENABLE_WASM */

/* ════════════════════════════════════════════════════════════════════
 * hull:gpu module — GPU compute (wgpu-native)
 *
 * gpu.available() -> boolean
 * gpu.devices() -> [{id, name}, ...]
 * gpu.compile(name, wgsl) -> true
 * gpu.dispatch(name, opts) -> ArrayBuffer
 * gpu.async.dispatch(name, opts) -> Promise<ArrayBuffer>
 * gpu.buffer(name, data, opts?) -> true
 * gpu.buffer(name, null) -> destroy
 * gpu.bufferRead(name) -> ArrayBuffer
 * ════════════════════════════════════════════════════════════════════ */

#ifdef HL_ENABLE_GPU
#include "hull/cap/gpu.h"
#include "hull/worker_gpu.h"

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

    /* Parse output buffer index (0-indexed in JS) */
    JSValue out_val = JS_GetPropertyStr(ctx, opts_val, "output");
    if (!JS_IsUndefined(out_val)) {
        int32_t o;
        JS_ToInt32(ctx, &o, out_val);
        opts.output_buffer = o;
    }
    JS_FreeValue(ctx, out_val);

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

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    int rc = hl_cap_gpu_dispatch(gpu, name, &opts,
                                 &output, &output_len, &err_msg);

    /* Free uniform string if we used JS_ToCStringLen */
    if (uni_str) JS_FreeCString(ctx, uni_str);

    /* Free buffer name/data strings */
    for (int i = 0; i < buf_count; i++) {
        if (buf_name_strs[i]) JS_FreeCString(ctx, buf_name_strs[i]);
        if (buf_data_strs[i]) JS_FreeCString(ctx, buf_data_strs[i]);
    }

    JS_FreeCString(ctx, name);

    if (rc != HL_GPU_OK)
        return JS_ThrowInternalError(ctx, "gpu.dispatch: %s",
                                     err_msg ? err_msg : "dispatch_failed");

    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)output, output_len);
    free(output);
    return ab;
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

    /* Get data from MappedBuffer, ArrayBuffer, or string */
    size_t data_len = 0;
    const uint8_t *data = NULL;
    const char *str_data = NULL;

    HlMappedBuffer *mmap_buf = JS_GetOpaque2(ctx, argv[1], js_mmap_class_id);
    if (mmap_buf && !mmap_buf->closed) {
        data = (const uint8_t *)mmap_buf->addr;
        data_len = mmap_buf->len;
    } else {
        data = JS_GetArrayBuffer(ctx, &data_len, argv[1]);
        if (!data) {
            str_data = JS_ToCStringLen(ctx, &data_len, argv[1]);
            if (!str_data) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
            data = (const uint8_t *)str_data;
        }
    }

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
    if (!js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx, "gpu.async can only be called from a request handler");
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
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.alloc);
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

    op->async_ctx = actx;
    atomic_store(&op->cancelled, 0);

    if (hl_worker_gpu_submit(js->base.thread_pool, op) != 0) {
        actx->cont->destroy(actx->cont);
        hl_worker_gpu_op_free(op); free(op); hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "gpu.async.dispatch: thread pool full");
    }

    if (kl_async_suspend(js->server, js->active_conn, &actx->op) < 0) {
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

    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue dv = JS_GetPropertyStr(ctx, argv[1], "device");
        if (!JS_IsUndefined(dv)) { int32_t d; JS_ToInt32(ctx, &d, dv); device = d; }
        JS_FreeValue(ctx, dv);

        /* outputs: [{stage: 0, buffer: 0}, ...] (0-indexed) */
        JSValue ov = JS_GetPropertyStr(ctx, argv[1], "outputs");
        if (JS_IsArray(ctx, ov)) {
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
    }

    HlGpuPipelineOpts opts = {
        .stages = stages,
        .stage_count = stage_count,
        .outputs = output_count > 0 ? outputs : NULL,
        .output_count = output_count,
        .device = device,
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
static JSValue js_gpu_async_pipeline(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    return js_gpu_pipeline(ctx, this_val, argc, argv);
}

static int js_gpu_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue gpu = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, gpu, "available",
                      JS_NewCFunction(ctx, js_gpu_available, "available", 0));
    JS_SetPropertyStr(ctx, gpu, "devices",
                      JS_NewCFunction(ctx, js_gpu_devices, "devices", 0));
    JS_SetPropertyStr(ctx, gpu, "compile",
                      JS_NewCFunction(ctx, js_gpu_compile, "compile", 2));
    JS_SetPropertyStr(ctx, gpu, "dispatch",
                      JS_NewCFunction(ctx, js_gpu_dispatch, "dispatch", 2));
    JS_SetPropertyStr(ctx, gpu, "pipeline",
                      JS_NewCFunction(ctx, js_gpu_pipeline, "pipeline", 2));
    JS_SetPropertyStr(ctx, gpu, "buffer",
                      JS_NewCFunction(ctx, js_gpu_buffer, "buffer", 3));
    JS_SetPropertyStr(ctx, gpu, "bufferRead",
                      JS_NewCFunction(ctx, js_gpu_buffer_read, "bufferRead", 1));

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

static int hl_js_init_gpu_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:gpu", js_gpu_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "gpu");
    return 0;
}
#endif /* HL_ENABLE_GPU */

/* ════════════════════════════════════════════════════════════════════
 * Module registry — called by hl_js_init() to register all
 * hull:* built-in modules.
 * ════════════════════════════════════════════════════════════════════ */

int hl_js_register_modules(HlJS *js)
{
    if (!js || !js->ctx)
        return -1;

    /* Register hull:app module */
    if (hl_js_init_app_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:db module (only if database is available) */
    if (js->base.db) {
        if (hl_js_init_db_module(js->ctx, js) != 0)
            return -1;
    }

    /* Register hull:json module */
    if (hl_js_init_json_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:time module */
    if (hl_js_init_time_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:env module */
    if (hl_js_init_env_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:crypto module */
    if (hl_js_init_crypto_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:log module */
    if (hl_js_init_log_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:http module — always available; per-function checks
     * enforce that http_cfg is set (wired from manifest after load_app). */
    if (hl_js_init_http_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:smtp module — always available; per-function checks
     * enforce that smtp_cfg is set (wired from manifest after load_app). */
    if (hl_js_init_smtp_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:_template — internal bridge for hull:template stdlib */
    if (hl_js_init_template_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:worker module (only if thread pool is available) */
    if (js->base.thread_pool) {
        if (hl_js_init_worker_module(js->ctx, js) != 0)
            return -1;
    }

    /* Register hull:server module (always available) */
    if (hl_js_init_server_module(js->ctx, js) != 0)
        return -1;

    /* Register hull:fs module — always available; per-function checks
     * enforce that fs_cfg is set (wired from manifest after load_app). */
    if (hl_js_init_fs_module(js->ctx, js) != 0)
        return -1;

#ifdef HL_ENABLE_WASM
    /* Register hull:compute module (only if WASM runtime is available) */
    if (js->base.wasm_cache) {
        if (hl_js_init_compute_module(js->ctx, js) != 0)
            return -1;
    }
#endif

#ifdef HL_ENABLE_GPU
    /* Register hull:gpu module (only if GPU context is available) */
    if (js->base.gpu_ctx) {
        if (hl_js_init_gpu_module(js->ctx, js) != 0)
            return -1;
    }
#endif

    return 0;
}
