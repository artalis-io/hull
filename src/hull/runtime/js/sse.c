/*
 * sse.c — JS Server-Sent Events handler
 *
 * Adapts Keel SSE routes (registered via `app.sse()`) to JS handler
 * functions. Begins the chunked stream, builds a stream object, and
 * supports async handlers via Promise return (the stream is closed
 * when the resume callback observes the handler promise settled).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/async.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"

#include "mod_buffer.h"

#include <keel/keel.h>
#include <keel/sse.h>

#include <sh_arena.h>

#include "log.h"

/* Forward declarations from bindings.c */
JSValue hl_js_make_request(JSContext *ctx, KlRequest *req);

void hl_js_sse_handler(KlRequest *req, KlResponse *res,
                                void *user_data)
{
    HlJSSseRoute *route = (HlJSSseRoute *)user_data;
    HlJS *js = route ? route->js : NULL;
    if (!js || !js->ctx || !req || !res)
        return;
    JSContext *ctx = js->ctx;

    js->dispatch_depth++;

    /* Guard stale transactions */
    hl_db_guard_stale_txn(js->base.db_handle);

    /* Reset scratch + instruction counter */
    sh_arena_reset(js->scratch);
    js->instruction_count = 0;

    /* Set per-request async context */
    js->active_conn = kl_request_conn(req);
    js->active_req = req;
    js->last_async_cont = NULL;
    js->async_pending = 0;

    /* Get handler function */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue routes_arr = JS_GetPropertyStr(ctx, global, "__hull_routes");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(routes_arr) || !JS_IsArray(ctx, routes_arr)) {
        JS_FreeValue(ctx, routes_arr);
        js->active_conn = NULL;
        js->active_req = NULL;
        js->dispatch_depth--;
        return;
    }

    JSValue handler = JS_GetPropertyUint32(ctx, routes_arr,
                                            (uint32_t)route->handler_id);
    JS_FreeValue(ctx, routes_arr);

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        js->active_conn = NULL;
        js->active_req = NULL;
        js->dispatch_depth--;
        return;
    }

    /* Build request object */
    JSValue js_req = hl_js_make_request(ctx, req);

    /* Create SSE stream object (calls kl_sse_begin) */
    JSValue stream_obj = hl_js_sse_create_stream(ctx, res);
    if (JS_IsException(stream_obj)) {
        JS_FreeValue(ctx, handler);
        JS_FreeValue(ctx, js_req);
        js->active_conn = NULL;
        js->active_req = NULL;
        js->dispatch_depth--;
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body_borrow(res, "SSE init failed", 15);
        return;
    }

    /* Call handler(req, stream) */
    JSValue args[2] = { js_req, stream_obj };
    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, handler);

    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        log_error("[hull:sse] handler error: %s", msg ? msg : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        hl_js_sse_stream_force_close(ctx, stream_obj);
    } else {
        JSPromiseStateEnum state = JS_PromiseState(ctx, ret);
        if (state == JS_PROMISE_PENDING && js->last_async_cont) {
            /* Async SSE handler — wire handler_promise on continuation */
            extern void hl_js_async_cont_set_handler_promise(
                HlAsyncCont *cont, JSContext *c, JSValue promise);
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, ctx, ret);
            js->last_async_cont = NULL;
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, js_req);
            JS_FreeValue(ctx, stream_obj);
            /* dispatch_depth + active_conn stay set — async resume will clear */
            return;
        }
        /* Sync completion — close stream if not already */
        if (!hl_js_sse_stream_is_closed(ctx, stream_obj))
            hl_js_sse_stream_force_close(ctx, stream_obj);
    }

    hl_js_run_jobs(js);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, js_req);
    JS_FreeValue(ctx, stream_obj);
    js->active_conn = NULL;
    js->active_req = NULL;
    js->dispatch_depth--;
}
