/*
 * ws.c — JS WebSocket server callback trampolines
 *
 * Maps Keel's per-connection WebSocket callbacks (on_open/on_message/
 * on_close) to JS handler functions registered via `app.ws()`. Each
 * trampoline runs in detached mode (no HTTP req), routes through the
 * WS registry to bind a conn object, and supports async handlers via
 * Promise return.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/async.h"
#include "hull/cap/ws.h"

#include "mod_buffer.h"

#include <keel/keel.h>
#include <keel/websocket_server.h>

#include "log.h"

void hl_js_ws_on_open(KlWsServerConn *ws_conn, void *user_data)
{
    HlJSWsRoute *route = (HlJSWsRoute *)user_data;
    HlJS *js = route->js;
    JSContext *ctx = js->ctx;

    /* Register the connection in the registry */
    HlWsConn *conn = hl_ws_registry_add(js->base.ws_registry,
                                          route->path, ws_conn);
    if (!conn)
        return;

    if (route->on_open_id < 0)
        return;

    js->dispatch_depth++;
    js->active_conn = NULL; /* detached — no HTTP connection */
    js->active_req = NULL;
    js->active_timer = NULL;
    js->last_async_cont = NULL;
    js->async_pending = 0;
    js->instruction_count = 0;

    /* Look up handler */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue routes = JS_GetPropertyStr(ctx, global, "__hull_routes");
    JSValue handler = JS_GetPropertyUint32(ctx, routes,
                                            (uint32_t)route->on_open_id);
    JS_FreeValue(ctx, routes);
    JS_FreeValue(ctx, global);

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        js->dispatch_depth--;
        return;
    }

    /* Push conn object */
    JSValue conn_obj = hl_js_ws_push_conn(ctx, conn);

    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 1, &conn_obj);
    JS_FreeValue(ctx, handler);
    JS_FreeValue(ctx, conn_obj);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        log_error("[hull:ws] on_open error: %s", msg ? msg : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    } else {
        JSPromiseStateEnum state = JS_PromiseState(ctx, ret);
        if (state == JS_PROMISE_PENDING && js->last_async_cont) {
            extern void hl_js_async_cont_set_handler_promise(
                HlAsyncCont *cont, JSContext *c, JSValue promise);
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, ctx, ret);
            js->last_async_cont = NULL;
            JS_FreeValue(ctx, ret);
            /* dispatch_depth stays elevated for async */
            return;
        }
    }

    hl_js_run_jobs(js);
    JS_FreeValue(ctx, ret);
    js->dispatch_depth--;
}

void hl_js_ws_on_message(KlWsServerConn *ws_conn, const char *data,
                                  size_t len, int is_binary, void *user_data)
{
    HlJSWsRoute *route = (HlJSWsRoute *)user_data;
    HlJS *js = route->js;
    JSContext *ctx = js->ctx;

    if (route->on_message_id < 0)
        return;

    /* Find the HlWsConn */
    HlWsConn *conn = hl_ws_registry_find(js->base.ws_registry,
                                           route->path, ws_conn);
    if (!conn)
        return;

    js->dispatch_depth++;
    js->active_conn = NULL;
    js->active_req = NULL;
    js->active_timer = NULL;
    js->last_async_cont = NULL;
    js->async_pending = 0;
    js->instruction_count = 0;

    /* Look up handler */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue routes_arr = JS_GetPropertyStr(ctx, global, "__hull_routes");
    JSValue handler = JS_GetPropertyUint32(ctx, routes_arr,
                                            (uint32_t)route->on_message_id);
    JS_FreeValue(ctx, routes_arr);
    JS_FreeValue(ctx, global);

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        js->dispatch_depth--;
        return;
    }

    /* Build args: conn, message, is_binary */
    JSValue conn_obj = hl_js_ws_push_conn(ctx, conn);
    JSValue msg_val = JS_NewStringLen(ctx, data, len);
    JSValue is_bin_val = JS_NewBool(ctx, is_binary);

    JSValue args[3] = { conn_obj, msg_val, is_bin_val };
    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 3, args);
    JS_FreeValue(ctx, handler);
    JS_FreeValue(ctx, conn_obj);
    JS_FreeValue(ctx, msg_val);
    JS_FreeValue(ctx, is_bin_val);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg2 = JS_ToCString(ctx, exc);
        log_error("[hull:ws] on_message error: %s", msg2 ? msg2 : "unknown");
        if (msg2) JS_FreeCString(ctx, msg2);
        JS_FreeValue(ctx, exc);
    } else {
        JSPromiseStateEnum state = JS_PromiseState(ctx, ret);
        if (state == JS_PROMISE_PENDING && js->last_async_cont) {
            extern void hl_js_async_cont_set_handler_promise(
                HlAsyncCont *cont, JSContext *c, JSValue promise);
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, ctx, ret);
            js->last_async_cont = NULL;
            JS_FreeValue(ctx, ret);
            return;
        }
    }

    hl_js_run_jobs(js);
    JS_FreeValue(ctx, ret);
    js->dispatch_depth--;
}

void hl_js_ws_on_close(KlWsServerConn *ws_conn, uint16_t code,
                                const char *reason, size_t reason_len,
                                void *user_data)
{
    HlJSWsRoute *route = (HlJSWsRoute *)user_data;
    HlJS *js = route->js;
    JSContext *ctx = js->ctx;

    /* Find the HlWsConn */
    HlWsConn *conn = hl_ws_registry_find(js->base.ws_registry,
                                           route->path, ws_conn);
    if (!conn)
        return;

    conn->closed = 1;

    if (route->on_close_id >= 0) {
        js->dispatch_depth++;
        js->active_conn = NULL;
        js->active_req = NULL;
        js->active_timer = NULL;
        js->last_async_cont = NULL;
        js->async_pending = 0;
        js->instruction_count = 0;

        /* Look up handler */
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue routes_arr = JS_GetPropertyStr(ctx, global, "__hull_routes");
        JSValue handler = JS_GetPropertyUint32(ctx, routes_arr,
                                                (uint32_t)route->on_close_id);
        JS_FreeValue(ctx, routes_arr);
        JS_FreeValue(ctx, global);

        if (JS_IsFunction(ctx, handler)) {
            JSValue conn_obj = hl_js_ws_push_conn(ctx, conn);
            JSValue code_val = JS_NewInt32(ctx, code);
            JSValue reason_val = (reason && reason_len > 0)
                                     ? JS_NewStringLen(ctx, reason, reason_len)
                                     : JS_NULL;

            JSValue args[3] = { conn_obj, code_val, reason_val };
            JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 3, args);
            JS_FreeValue(ctx, conn_obj);
            JS_FreeValue(ctx, code_val);
            JS_FreeValue(ctx, reason_val);

            if (JS_IsException(ret)) {
                JSValue exc = JS_GetException(ctx);
                const char *msg = JS_ToCString(ctx, exc);
                log_error("[hull:ws] on_close error: %s", msg ? msg : "unknown");
                if (msg) JS_FreeCString(ctx, msg);
                JS_FreeValue(ctx, exc);
            }
            hl_js_run_jobs(js);
            JS_FreeValue(ctx, ret);
        }

        JS_FreeValue(ctx, handler);
        js->dispatch_depth--;
    }

    /* Invalidate conn object and remove from registry */
    hl_js_ws_invalidate_conn(ctx, conn);
    hl_ws_registry_remove(js->base.ws_registry, conn);
}
