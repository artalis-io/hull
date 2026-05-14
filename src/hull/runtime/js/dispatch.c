/*
 * dispatch.c — JS request/middleware dispatch bridges
 *
 * Bridges Keel's per-request callbacks to the JS handler/middleware
 * registry. Builds JS request/response objects, calls handlers,
 * captures pending promises for async resume, and cleans up
 * middleware ctx.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/reqctx.h"
#include "hull/async.h"
#include "hull/alloc.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"

#include <keel/keel.h>

#include "log.h"

#include <string.h>

/* Forward declarations from bindings.c */
JSValue hl_js_make_request(JSContext *ctx, KlRequest *req);
JSValue hl_js_make_response(HlJS *js, KlResponse *res);

/* From async.c */
extern void hl_js_async_cont_set_handler_promise(HlAsyncCont *cont,
                                                   JSContext *ctx,
                                                   JSValue promise);

/* ── Request dispatch ───────────────────────────────────────────────── */

int hl_js_dispatch(HlJS *js, int handler_id,
                     KlRequest *req, KlResponse *res)
{
    if (!js || !js->ctx || !req || !res)
        return -1;

    /* dispatch_depth may be > 0 during self-fetch (outbox.flush → same server).
     * This is safe because the original handler is yielded and the new
     * dispatch runs on its own coroutine/promise with independent state. */
    js->dispatch_depth++;

    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_guard_stale_txn(js->base.db_handle);

    hl_js_reset_request(js);

    /* Set per-request async context (for hull.sleep / http.get access) */
    js->active_conn = kl_request_conn(req);
    js->active_req = req;
    js->last_async_cont = NULL;

    /* Get the handler function from the route registry */
    JSValue global = JS_GetGlobalObject(js->ctx);
    JSValue routes = JS_GetPropertyStr(js->ctx, global, "__hull_routes");
    if (JS_IsUndefined(routes) || !JS_IsArray(js->ctx, routes)) {
        JS_FreeValue(js->ctx, routes);
        JS_FreeValue(js->ctx, global);
        js->active_conn = NULL;
        js->active_req = NULL;
        return -1;
    }

    JSValue handler = JS_GetPropertyUint32(js->ctx, routes,
                                            (uint32_t)handler_id);
    JS_FreeValue(js->ctx, routes);

    if (!JS_IsFunction(js->ctx, handler)) {
        JS_FreeValue(js->ctx, handler);
        JS_FreeValue(js->ctx, global);
        js->active_conn = NULL;
        js->active_req = NULL;
        return -1;
    }

    /* Build JS request and response objects */
    JSValue js_req = hl_js_make_request(js->ctx, req);
    JSValue js_res = hl_js_make_response(js, res);

    /* Call handler(req, res) */
    JSValue argv[2] = { js_req, js_res };
    JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);

    int result = 0;
    if (JS_IsException(ret)) {
        hl_js_dump_error(js);
        result = -1;
    } else if (JS_PromiseState(js->ctx, ret) == JS_PROMISE_PENDING) {
        /* Async handler — connection already suspended by hull.sleep
         * or similar async call. Store the outer handler promise on
         * the continuation (per-connection, not global) so the resume
         * callback can check when the handler completes. */
        extern void hl_js_async_cont_set_handler_promise(
            HlAsyncCont *cont, JSContext *ctx, JSValue promise);
        if (js->last_async_cont) {
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont,
                js->ctx, ret);
            js->last_async_cont = NULL;
        }
        js->async_pending = 1;
        result = 1; /* signal: handler suspended */
    } else if (JS_PromiseState(js->ctx, ret) == JS_PROMISE_REJECTED) {
        /* Async handler threw before its first await — the Promise is
         * immediately rejected (not an exception).  Log and return -1
         * so the caller writes a 500 response. */
        JSValue err = JS_PromiseResult(js->ctx, ret);
        const char *msg = JS_ToCString(js->ctx, err);
        log_error("[hull:c] async handler rejected: %s",
                  msg ? msg : "(unknown)");
        if (msg) JS_FreeCString(js->ctx, msg);
        JS_FreeValue(js->ctx, err);
        result = -1;
    }

    JS_FreeValue(js->ctx, ret);
    JS_FreeValue(js->ctx, js_res);
    JS_FreeValue(js->ctx, js_req);
    JS_FreeValue(js->ctx, handler);
    JS_FreeValue(js->ctx, global);

    if (result != 1) {
        /* Sync path — clean up middleware ctx */
        js->active_conn = NULL;
        js->active_req = NULL;
        js->dispatch_depth--;

        if (req->ctx) {
            HlReqCtx *rctx = (HlReqCtx *)req->ctx;
            if (rctx->kind == HL_REQCTX_JS_VAL) {
                JSValue val;
                memcpy(&val, rctx->js_val_bytes, sizeof(val));
                JS_FreeValue(js->ctx, val);
            } else if (rctx->kind == HL_REQCTX_JSON) {
                hl_alloc_free(js->base.alloc, rctx->json.data, rctx->json.len + 1);
            }
            hl_alloc_free(js->base.alloc, rctx, sizeof(HlReqCtx));
            req->ctx = NULL;
        }
    }
    /* result == 1: handler suspended, dispatch_depth stays elevated
     * until async resume completes */

    /* Run any pending microtasks */
    hl_js_run_jobs(js);

    return result;
}

void hl_js_keel_handler(KlRequest *req, KlResponse *res, void *user_data)
{
    HlJSRoute *route = (HlJSRoute *)user_data;
    int rc = hl_js_dispatch(route->js, route->handler_id, req, res);
    if (rc < 0) {
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body_borrow(res, "Internal Server Error", 21);
    }
    /* rc == 1: handler suspended — don't write response.
     * Keel checks conn->state == KL_CONN_SUSPENDED and returns. */
}

/* ── Middleware dispatch ────────────────────────────────────────────── */

int hl_js_dispatch_middleware(HlJS *js, int handler_id,
                              KlRequest *req, KlResponse *res)
{
    if (!js || !js->ctx || !req || !res)
        return -1;

    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_guard_stale_txn(js->base.db_handle);

    hl_js_reset_request(js);

    /* Get the handler function from the route registry */
    JSValue global = JS_GetGlobalObject(js->ctx);
    JSValue routes = JS_GetPropertyStr(js->ctx, global, "__hull_routes");
    if (JS_IsUndefined(routes) || !JS_IsArray(js->ctx, routes)) {
        JS_FreeValue(js->ctx, routes);
        JS_FreeValue(js->ctx, global);
        return -1;
    }

    JSValue handler = JS_GetPropertyUint32(js->ctx, routes,
                                            (uint32_t)handler_id);
    JS_FreeValue(js->ctx, routes);

    if (!JS_IsFunction(js->ctx, handler)) {
        JS_FreeValue(js->ctx, handler);
        JS_FreeValue(js->ctx, global);
        return -1;
    }

    /* Build JS request and response objects */
    JSValue js_req = hl_js_make_request(js->ctx, req);
    JSValue js_res = hl_js_make_response(js, res);

    /* Call handler(req, res) — capture return value */
    JSValue argv[2] = { js_req, js_res };
    JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);

    int result = 0;
    if (JS_IsException(ret)) {
        hl_js_dump_error(js);
        result = -1;
    } else {
        /* Capture return value: 0 = continue, non-zero = short-circuit */
        int32_t val = 0;
        if (JS_ToInt32(js->ctx, &val, ret) == 0)
            result = val;
    }

    /* Store req.ctx as a JS value ref so the next middleware
     * or handler can retrieve the object directly (no JSON round-trip). */
    JSValue ctx_val = JS_GetPropertyStr(js->ctx, js_req, "ctx");
    if (JS_IsObject(ctx_val)) {
        /* Free previous ctx if any */
        if (req->ctx) {
            HlReqCtx *old = (HlReqCtx *)req->ctx;
            if (old->kind == HL_REQCTX_JS_VAL) {
                JSValue old_val;
                memcpy(&old_val, old->js_val_bytes, sizeof(old_val));
                JS_FreeValue(js->ctx, old_val);
            } else if (old->kind == HL_REQCTX_JSON) {
                hl_alloc_free(js->base.alloc, old->json.data, old->json.len + 1);
            }
            hl_alloc_free(js->base.alloc, old, sizeof(HlReqCtx));
            req->ctx = NULL;
        }
        /* Store native JS value */
        HlReqCtx *rctx = hl_alloc_malloc(js->base.alloc, sizeof(HlReqCtx));
        if (rctx) {
            rctx->kind = HL_REQCTX_JS_VAL;
            JSValue dup = JS_DupValue(js->ctx, ctx_val);
            memcpy(rctx->js_val_bytes, &dup, sizeof(dup));
            req->ctx = rctx;
        }
    }
    JS_FreeValue(js->ctx, ctx_val);

    JS_FreeValue(js->ctx, ret);
    JS_FreeValue(js->ctx, js_res);
    JS_FreeValue(js->ctx, js_req);
    JS_FreeValue(js->ctx, handler);
    JS_FreeValue(js->ctx, global);

    /* Run any pending microtasks */
    hl_js_run_jobs(js);

    return result;
}

int hl_js_keel_middleware(KlRequest *req, KlResponse *res, void *user_data)
{
    HlJSRoute *ctx = (HlJSRoute *)user_data;
    int rc = hl_js_dispatch_middleware(ctx->js, ctx->handler_id, req, res);
    if (rc < 0) {
        /* Middleware error — short-circuit with 500 */
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body_borrow(res, "Internal Server Error", 21);
        return 1; /* short-circuit */
    }
    return rc;
}
