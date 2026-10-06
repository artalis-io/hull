/*
 * sse.c - JS Server-Sent Events handler
 *
 * Adapts Keel SSE routes (registered via `app.sse()`) to JS handler
 * functions. Begins the chunked stream, builds a stream object, and
 * supports async handlers via Promise return (the stream is closed
 * when the resume callback observes the handler promise settled).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "hull/shared/req_life.h"

#include "hull/shared/async.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_registry.h"

#include "mod_buffer.h"

#include <keel/keel.h>
#include <keel/http_sse.h>

#include <sh_arena.h>

#include "log.h"

/* Forward declarations from bindings.c */
JSValue hl_js_make_request(JSContext *ctx, KlHttpRequest *req, struct HlReqLife *life);

void hl_js_sse_handler(KlHttpRequest *req, KlHttpResponse *res,
                                void *user_data)
{
    HlJSSseRoute *route = (HlJSSseRoute *)user_data;
    HlJS *js = route ? route->js : NULL;
    if (!js || !js->ctx || !req || !res)
        return;
    JSContext *ctx = js->ctx;


    /* Guard stale transactions */
    hl_db_registry_guard_stale_txns(js->base.db_registry);

    /* Reset scratch + instruction counter */
    sh_arena_reset(js->scratch);
    hl_js_budget_arm(js);

    /* Set per-request async context */
    js->active_conn = kl_http_request_conn(req);
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
        return;
    }

    JSValue handler = JS_GetPropertyUint32(ctx, routes_arr,
                                            (uint32_t)route->handler_id);
    JS_FreeValue(ctx, routes_arr);

    if (!JS_IsFunction(ctx, handler)) {
        JS_FreeValue(ctx, handler);
        js->active_conn = NULL;
        js->active_req = NULL;
        return;
    }

    /* Build request object */
    JSValue js_req = hl_js_make_request(ctx, req, NULL);   /* not a multipart route */
    hl_js_req_ctx_free(js, req);   /* js_req.ctx holds its own reference */

    /* The request's life: the stream holds it, and so does every
     * continuation the handler creates. It dies when the handler is done -
     * including when the client goes away mid-stream - so a stream kept for
     * fan-out fails closed instead of writing into a connection that is gone. */
    HlReqLife *life = hl_req_life_new();

    /* Create SSE stream object (calls kl_http_sse_begin) */
    JSValue stream_obj = life ? hl_js_sse_create_stream(ctx, res, life)
                              : JS_EXCEPTION;
    if (JS_IsException(stream_obj)) {
        hl_req_life_end(life);
        JS_FreeValue(ctx, handler);
        JS_FreeValue(ctx, js_req);
        js->active_conn = NULL;
        js->active_req = NULL;
        kl_http_response_status(res, 500);
        kl_http_response_header(res, "Content-Type", "text/plain");
        kl_http_response_body_borrow(res, "SSE init failed", 15);
        return;
    }

    /* Call handler(req, stream) */
    JSValue args[2] = { js_req, stream_obj };
    js->last_async_cont = NULL;   /* only this run's continuations chain */
    js->active_life = life;
    JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, handler);

    if (JS_IsException(ret)) {
        js->active_life = NULL;
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        log_error("[hull:web:sse] handler error: %s", msg ? msg : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        else JS_FreeValue(ctx, JS_GetException(ctx));   /* a throwing toString (L2) */
        JS_FreeValue(ctx, exc);
        hl_js_sse_stream_force_close(ctx, stream_obj);
    } else {
        /* Its code after an `await` of something already settled runs now,
         * with the request and its life active (`await null; ...; await
         * hull.sleep()` - deciding at once closed such a stream at once, and
         * an op that code started was detached and never waited for), then
         * every continuation is wired into the run (audit 7 H1 / M6). */
        int st;
        int waiting = hl_js_entry_park(js, ret, &st);
        js->active_life = NULL;
        if (waiting) {
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, js_req);
            JS_FreeValue(ctx, stream_obj);
            /* The continuation holds the life now; drop ours. */
            hl_req_life_release(life);
            /* The resume restores them; the next run must not see them. */
            js->active_conn = NULL;
            js->active_req = NULL;
            hl_db_registry_guard_stale_txns(js->base.db_registry);   /* audit 6 M1 */
            return;
        }
        /* Sync completion - close stream if not already */
        if (!hl_js_sse_stream_is_closed(ctx, stream_obj))
            hl_js_sse_stream_force_close(ctx, stream_obj);
    }

    /* The handler is done with its request (the stream was ended above), so
     * its leftover microtasks run with no request active: an op they start
     * must not suspend this connection. */
    hl_req_life_end(life);
    js->active_conn = NULL;
    js->active_req = NULL;
    hl_js_run_jobs(js);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, js_req);
    JS_FreeValue(ctx, stream_obj);
    js->last_async_cont = NULL;   /* an un-awaited op belongs to no run */
    hl_db_registry_guard_stale_txns(js->base.db_registry);   /* audit 6 M1 */
}
