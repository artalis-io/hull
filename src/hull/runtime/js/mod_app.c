/*
 * mod_app.c — hull:app module (route registration, manifest, timers)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

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

/* ── Helper: store handler in __hull_routes, return handler_id ───── */

static int32_t js_store_handler(JSContext *ctx, JSValueConst func)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue routes = JS_GetPropertyStr(ctx, global, "__hull_routes");
    if (JS_IsUndefined(routes)) {
        routes = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_routes", JS_DupValue(ctx, routes));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, routes, "length");
    int32_t handler_id = 0;
    JS_ToInt32(ctx, &handler_id, len_val);
    JS_FreeValue(ctx, len_val);

    JS_SetPropertyUint32(ctx, routes, (uint32_t)handler_id, JS_DupValue(ctx, func));
    JS_FreeValue(ctx, routes);
    JS_FreeValue(ctx, global);
    return handler_id;
}

/* app.ws(path, { onOpen, onMessage, onClose }) — WebSocket endpoint */
static JSValue js_app_ws(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "app.ws requires (path, handlers)");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    if (!JS_IsObject(argv[1])) {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "app.ws requires handlers object");
    }

    JSValue global = JS_GetGlobalObject(ctx);

    /* Ensure __hull_ws_defs array exists */
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_ws_defs");
    if (JS_IsUndefined(defs)) {
        defs = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_ws_defs", JS_DupValue(ctx, defs));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "path", JS_NewString(ctx, path));

    /* Extract and store each callback */
    JSValue on_open = JS_GetPropertyStr(ctx, argv[1], "onOpen");
    if (JS_IsFunction(ctx, on_open)) {
        int32_t hid = js_store_handler(ctx, on_open);
        JS_SetPropertyStr(ctx, entry, "on_open_id", JS_NewInt32(ctx, hid));
    }
    JS_FreeValue(ctx, on_open);

    JSValue on_message = JS_GetPropertyStr(ctx, argv[1], "onMessage");
    if (JS_IsFunction(ctx, on_message)) {
        int32_t hid = js_store_handler(ctx, on_message);
        JS_SetPropertyStr(ctx, entry, "on_message_id", JS_NewInt32(ctx, hid));
    }
    JS_FreeValue(ctx, on_message);

    JSValue on_close = JS_GetPropertyStr(ctx, argv[1], "onClose");
    if (JS_IsFunction(ctx, on_close)) {
        int32_t hid = js_store_handler(ctx, on_close);
        JS_SetPropertyStr(ctx, entry, "on_close_id", JS_NewInt32(ctx, hid));
    }
    JS_FreeValue(ctx, on_close);

    JS_SetPropertyUint32(ctx, defs, (uint32_t)idx, entry);
    JS_FreeValue(ctx, defs);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, path);

    return JS_UNDEFINED;
}

/* app.sse(path, handler) — SSE endpoint registration */
static JSValue js_app_sse(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "app.sse requires (path, handler)");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "handler must be a function");
    }

    int32_t handler_id = js_store_handler(ctx, argv[1]);

    JSValue global = JS_GetGlobalObject(ctx);

    /* Ensure __hull_sse_defs array exists */
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_sse_defs");
    if (JS_IsUndefined(defs)) {
        defs = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "__hull_sse_defs", JS_DupValue(ctx, defs));
    }

    JSValue len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "path", JS_NewString(ctx, path));
    JS_SetPropertyStr(ctx, entry, "handler_id", JS_NewInt32(ctx, handler_id));
    JS_SetPropertyUint32(ctx, defs, (uint32_t)idx, entry);

    JS_FreeValue(ctx, defs);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, path);

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
    JS_SetPropertyStr(ctx, app, "ws",
                      JS_NewCFunction(ctx, js_app_ws, "ws", 2));
    JS_SetPropertyStr(ctx, app, "sse",
                      JS_NewCFunction(ctx, js_app_sse, "sse", 2));
    JS_SetPropertyStr(ctx, app, "every",
                      JS_NewCFunction(ctx, js_app_every, "every", 2));
    JS_SetPropertyStr(ctx, app, "daily",
                      JS_NewCFunction(ctx, js_app_daily, "daily", 3));
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
