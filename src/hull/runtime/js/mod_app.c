/*
 * mod_app.c - hull:app module (route registration, manifest, timers)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

/* Returns 1 if app.main was already registered. */
static int js_app_main_registered(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue m = JS_GetPropertyStr(ctx, global, "__hull_main");
    int has = !JS_IsUndefined(m) && !JS_IsNull(m);
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, global);
    return has;
}

/* Phase gate.  Throws a structured TypeError if the runtime has moved
 * past the boot phase (top-level + app.main).  Returns 1 when it
 * threw, 0 when the call should proceed.  Caller pattern matches the
 * existing call sites:
 *
 *     if (js_app_throw_if_serving(ctx, "app.use")) return JS_EXCEPTION;
 *
 * Mirrors Lua's lua_app_reject_if_serving - see that doc-comment for
 * the full rationale (router seal + clear-error UX instead of silent
 * drop).  NULL HlJS is the test-harness-only path: real serve setups
 * always have it wired; harnesses that drive QuickJS directly without
 * spinning up a serve loop never set the flag anyway. */
static int js_app_throw_if_serving(JSContext *ctx, const char *call)
{
    HlJS *js = get_hl_js(ctx);
    if (js && js->base.registration_closed) {
        JS_ThrowTypeError(ctx,
            "%s can only be called at app startup (top-level code or "
            "inside app.main). Hull seals the router after wire-up so "
            "dynamic registration from request handlers / timer "
            "callbacks is intentionally not supported. Move the "
            "registration to top level, or to an app.main(fn) that "
            "runs before the serve loop starts.",
            call);
        return 1;
    }
    return 0;
}

/* Helper: register a route with given method string.
 *
 * Signature: app.<verb>(pattern, handler [, opts])
 *
 *   opts.multipart = { maxPartSize?, maxTotalSize?, maxParts?,
 *                      maxHeadersSize?, maxInputBuffer? }
 *     When present, this route uses Keel's streaming-multipart body
 *     reader instead of the default buffered reader. The handler can
 *     iterate parts via req.multipart() (see §1.5.b-2 iterator
 *     bindings). All caps are numbers; 0 or missing = unlimited.
 *     The whole opts.multipart subobject is stashed on the route def
 *     and re-read in routes.c (which allocates the KlMultipartConfig
 *     and registers the route via kl_server_route_streaming).
 */
static JSValue js_app_route(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    if (js_app_throw_if_serving(ctx, "app.get/post/put/delete/patch/options"))
        return JS_EXCEPTION;
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

    /* Arg 2 is the optional opts object - must be a plain object if present. */
    int has_opts = (argc >= 3) && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]);
    if (has_opts && !JS_IsObject(argv[2])) {
        JS_FreeCString(ctx, pattern);
        return JS_ThrowTypeError(ctx, "app.%s opts must be an object",
                                 method_names[magic]);
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

    /* Stash opts.multipart (if a plain object) verbatim on the def.
     * routes.c reads the integer caps off this sub-object when
     * materializing the KlMultipartConfig. */
    if (has_opts) {
        JSValue mp = JS_GetPropertyStr(ctx, argv[2], "multipart");
        if (JS_IsObject(mp) && !JS_IsFunction(ctx, mp) && !JS_IsArray(ctx, mp)) {
            JS_SetPropertyStr(ctx, def, "multipart", mp); /* takes ownership */
        } else {
            JS_FreeValue(ctx, mp);
        }
    }

    JS_SetPropertyUint32(ctx, defs, (uint32_t)idx, def);

    JS_FreeValue(ctx, defs);
    JS_FreeValue(ctx, routes);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, pattern);

    return JS_UNDEFINED;
}

/* app.use(method, pattern, handler) - middleware registration */
static JSValue js_app_use(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (js_app_throw_if_serving(ctx, "app.use")) return JS_EXCEPTION;
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

/* app.usePost(method, pattern, fn) - register post-body middleware */
static JSValue js_app_use_post(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (js_app_throw_if_serving(ctx, "app.usePost")) return JS_EXCEPTION;
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

/* app.every(interval_ms, handler) - repeating timer */
static JSValue js_app_every(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (js_app_throw_if_serving(ctx, "app.every")) return JS_EXCEPTION;
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

/* app.daily(time_str, handler [, opts]) - daily timer at HH:MM */
static JSValue js_app_daily(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (js_app_throw_if_serving(ctx, "app.daily")) return JS_EXCEPTION;
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

/* app.ws(path, { onOpen, onMessage, onClose }) - WebSocket endpoint */
static JSValue js_app_ws(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (js_app_throw_if_serving(ctx, "app.ws")) return JS_EXCEPTION;
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

/* app.sse(path, handler) - SSE endpoint registration */
static JSValue js_app_sse(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (js_app_throw_if_serving(ctx, "app.sse")) return JS_EXCEPTION;
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

/* app.manifest(obj) - declare application capabilities (one-shot) */
/* Forward decls for the conditionally-installed timer methods. */
static JSValue js_app_every(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);
static JSValue js_app_daily(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);

/* Scan a manifest object for a `modules` array entry that starts
 * with `prefix` (e.g. "hull/timers"). Returns 1 if found.
 * Manifest and modules array are inspected via JS_GetPropertyStr;
 * caller owns the manifest reference. */
/* Walk a registry spec's deps recursively for a name-prefix match.
 * Mirrors the Lua-side spec_chain_matches; see mod_app.c (lua) for
 * design rationale. */
static int js_spec_chain_matches(const HlModuleSpec *spec, const char *prefix,
                                 size_t plen, int depth)
{
    if (!spec || depth > 8) return 0;
    if (strncmp(spec->name, prefix, plen) == 0) return 1;
    for (int i = 0; i < HL_MODULE_MAX_DEPS && spec->deps[i]; i++) {
        const HlModuleSpec *dep = hl_module_registry_find(spec->deps[i]);
        if (js_spec_chain_matches(dep, prefix, plen, depth + 1)) return 1;
    }
    return 0;
}

static int js_manifest_declares_module(JSContext *ctx, JSValueConst manifest,
                                       const char *prefix)
{
    int found = 0;
    size_t plen = strlen(prefix);

    JSValue modules = JS_GetPropertyStr(ctx, manifest, "modules");
    if (JS_IsArray(ctx, modules)) {
        JSValue len_val = JS_GetPropertyStr(ctx, modules, "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, len_val);
        JS_FreeValue(ctx, len_val);
        for (uint32_t i = 0; i < len; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, modules, i);
            if (JS_IsString(item)) {
                const char *s = JS_ToCString(ctx, item);
                if (s && strncmp(s, prefix, plen) == 0) {
                    /* Fast path: direct prefix match on the declared
                     * string (e.g. "hull/http-server@1"). */
                    found = 1;
                } else if (s) {
                    /* Slow path: walk transitive deps so middleware
                     * declarations decorate the app intrinsic the same
                     * way explicit declarations do. */
                    char nameonly[HL_MODULE_NAME_MAX];
                    const char *at = strchr(s, '@');
                    size_t nlen = at ? (size_t)(at - s) : strlen(s);
                    if (nlen + 1 <= sizeof(nameonly)) {
                        memcpy(nameonly, s, nlen);
                        nameonly[nlen] = '\0';
                        const HlModuleSpec *spec =
                            hl_module_registry_find(nameonly);
                        if (js_spec_chain_matches(spec, prefix, plen, 0))
                            found = 1;
                    }
                }
                if (s) JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, item);
            if (found) break;
        }
    }
    JS_FreeValue(ctx, modules);
    return found;
}

/* Install app.every / app.daily on the exported `app` object.
 * The app object is stashed on globalThis under __hull_app_ref by
 * js_app_module_init so this function can reach it. Mirrors the
 * Lua partial-class pattern. */
static void js_install_app_timers(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue app = JS_GetPropertyStr(ctx, global, "__hull_app_ref");
    if (JS_IsObject(app)) {
        JS_SetPropertyStr(ctx, app, "every",
            JS_NewCFunction(ctx, js_app_every, "every", 2));
        JS_SetPropertyStr(ctx, app, "daily",
            JS_NewCFunction(ctx, js_app_daily, "daily", 3));
    }
    JS_FreeValue(ctx, app);
    JS_FreeValue(ctx, global);
}

/* Install the REST + middleware + router surface. Called when the
 * manifest declares "hull/http-server@1". Mirrors install_app_http_server
 * on the Lua side. The router is a pure-JS class evaluated against
 * the live `app` object. */
static void js_install_app_http_server(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue app = JS_GetPropertyStr(ctx, global, "__hull_app_ref");
    if (JS_IsObject(app)) {
        JS_SetPropertyStr(ctx, app, "get",
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                                 "get", 2, JS_CFUNC_generic_magic, 0));
        JS_SetPropertyStr(ctx, app, "post",
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                                 "post", 2, JS_CFUNC_generic_magic, 1));
        JS_SetPropertyStr(ctx, app, "put",
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                                 "put", 2, JS_CFUNC_generic_magic, 2));
        JS_SetPropertyStr(ctx, app, "delete",
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_app_route,
                                 "delete", 2, JS_CFUNC_generic_magic, 3));
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
    }

    /* Router (pure JS, evaluated against the freshly-decorated app
     * via a temporary globalThis stash that we clear immediately). */
    static const char router_src[] =
"(function() {\n"
"  const app = globalThis.__hull_app_for_router;\n"
"  class Router {\n"
"    constructor(prefix, opts) { this.prefix = prefix || ''; this.opts = opts; }\n"
"    get(p, h)     { app.get    (this.prefix + p, h); return this; }\n"
"    post(p, h)    { app.post   (this.prefix + p, h); return this; }\n"
"    put(p, h)     { app.put    (this.prefix + p, h); return this; }\n"
"    delete(p, h)  { app.delete (this.prefix + p, h); return this; }\n"
"    patch(p, h)   { app.patch  (this.prefix + p, h); return this; }\n"
"    options(p, h) { app.options(this.prefix + p, h); return this; }\n"
"    use(...args) {\n"
"      if (typeof args[0] === 'function') app.use('*', this.prefix + '/*', args[0]);\n"
"      else app.use(args[0], this.prefix + args[1], args[2]);\n"
"      return this;\n"
"    }\n"
"    usePost(...args) {\n"
"      if (typeof args[0] === 'function') app.usePost('*', this.prefix + '/*', args[0]);\n"
"      else app.usePost(args[0], this.prefix + args[1], args[2]);\n"
"      return this;\n"
"    }\n"
"    ws(p, h)  { if (app.ws)  app.ws (this.prefix + p, h); return this; }\n"
"    sse(p, h) { if (app.sse) app.sse(this.prefix + p, h); return this; }\n"
"    router(sub, opts) { return new Router(this.prefix + (sub || ''), opts); }\n"
"  }\n"
"  app.router = function(prefix, opts) { return new Router(prefix, opts); };\n"
"})();\n";

    JS_SetPropertyStr(ctx, global, "__hull_app_for_router",
                      JS_DupValue(ctx, app));
    JSValue rret = JS_Eval(ctx, router_src, sizeof(router_src) - 1,
                           "<hull-router-init>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, rret);
    JSAtom temp_atom = JS_NewAtom(ctx, "__hull_app_for_router");
    JS_DeleteProperty(ctx, global, temp_atom, 0);
    JS_FreeAtom(ctx, temp_atom);
    JS_FreeValue(ctx, app);
    JS_FreeValue(ctx, global);
}

/* Install app.ws on the exported `app` object. */
static void js_install_app_ws_server(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue app = JS_GetPropertyStr(ctx, global, "__hull_app_ref");
    if (JS_IsObject(app)) {
        JS_SetPropertyStr(ctx, app, "ws",
            JS_NewCFunction(ctx, js_app_ws, "ws", 2));
    }
    JS_FreeValue(ctx, app);
    JS_FreeValue(ctx, global);
}

/* Install app.sse on the exported `app` object. */
static void js_install_app_sse(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue app = JS_GetPropertyStr(ctx, global, "__hull_app_ref");
    if (JS_IsObject(app)) {
        JS_SetPropertyStr(ctx, app, "sse",
            JS_NewCFunction(ctx, js_app_sse, "sse", 2));
    }
    JS_FreeValue(ctx, app);
    JS_FreeValue(ctx, global);
}

static JSValue js_app_manifest(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "app.manifest requires an object");

    /* Reject second call - manifest is immutable once declared */
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

    /* Module-conditional method installation. Each declared module
     * may decorate the `app` intrinsic with additional methods. */
    if (js_manifest_declares_module(ctx, argv[0], "hull/http-server")) {
        js_install_app_http_server(ctx);
    }
    if (js_manifest_declares_module(ctx, argv[0], "hull/web/ws-server")) {
        js_install_app_ws_server(ctx);
    }
    if (js_manifest_declares_module(ctx, argv[0], "hull/web/sse")) {
        js_install_app_sse(ctx);
    }
    if (js_manifest_declares_module(ctx, argv[0], "hull/timers")) {
        js_install_app_timers(ctx);
    }

    return JS_UNDEFINED;
}

/* app.getManifest() - retrieve the stored manifest object */
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

/* app.main(fn) - register a startup hook.
 *
 * Lifecycle: serve.c invokes the function once after manifest extraction
 * + sandbox + migrations, on the event loop thread. If the app also
 * registered routes / middleware / timers / WebSocket / SSE handlers,
 * the HTTP listener auto-starts after main resolves. Returning a
 * non-zero exit code from main short-circuits - the process exits with
 * that code even if routes are registered. Apps with main only and no
 * routes exit when main returns. */
static JSValue js_app_main(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "app.main requires a function");

    if (js_app_main_registered(ctx))
        return JS_ThrowTypeError(ctx, "app.main() can only be called once");

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__hull_main", JS_DupValue(ctx, argv[0]));
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

static int js_app_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/app", "hull:app") != 0) return -1;

    JSValue app = JS_NewObject(ctx);

    /* Default app surface is just bootstrap registration. Every other
     * method (get/post/use/router/ws/sse/every/daily) is installed
     * conditionally by js_app_manifest based on which modules the
     * manifest declares:
     *   hull/http-server@1 → get/post/put/delete/del/patch/options +
     *                         use/usePost + router
     *   hull/web/ws-server@1   → ws
     *   hull/web/sse@1         → sse
     *   hull/timers@1      → every/daily
     */
    JS_SetPropertyStr(ctx, app, "main",
                      JS_NewCFunction(ctx, js_app_main, "main", 1));
    JS_SetPropertyStr(ctx, app, "manifest",
                      JS_NewCFunction(ctx, js_app_manifest, "manifest", 1));
    JS_SetPropertyStr(ctx, app, "getManifest",
                      JS_NewCFunction(ctx, js_app_get_manifest, "getManifest", 0));

    /* Stash the app object on globalThis under an internal name so
     * js_app_manifest can find it later to install module-conditional
     * methods. */
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__hull_app_ref", JS_DupValue(ctx, app));
    JS_FreeValue(ctx, global);

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
