/*
 * dispatch.c - JS request/middleware dispatch bridges
 *
 * Bridges Keel's per-request callbacks to the JS handler/middleware
 * registry. Builds JS request/response objects, calls handlers,
 * captures pending promises for async resume, and cleans up
 * middleware ctx.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "hull/shared/req_life.h"
#include "hull/http_feature.h"  /* hl_js_http_error_response (HTTP-feature seam) */

#include "hull/reqctx.h"
#include "hull/shared/async.h"
#include "hull/utils/alloc.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_registry.h"

#include <keel/keel.h>

#include "log.h"

#include <string.h>

/* Forward declarations from bindings.c */
JSValue hl_js_make_request(JSContext *ctx, KlHttpRequest *req, struct HlReqLife *life);
JSValue hl_js_make_response(HlJS *js, KlHttpResponse *res);
JSValue hl_js_make_response_life(HlJS *js, KlHttpResponse *res, HlReqLife *life);
extern void hl_js_async_cont_set_handler_promise(HlAsyncCont *cont,
                                                 JSContext *ctx,
                                                 JSValue promise);

/* From async.c */
extern void hl_js_async_cont_set_handler_promise(HlAsyncCont *cont,
                                                   JSContext *ctx,
                                                   JSValue promise);

/* ── Request dispatch ───────────────────────────────────────────────── */

int hl_js_dispatch(HlJS *js, int handler_id,
                     KlHttpRequest *req, KlHttpResponse *res)
{
    if (!js || !js->ctx || !req || !res)
        return -1;


    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_registry_guard_stale_txns(js->base.db_registry);

    hl_js_reset_request(js);

    /* Set per-request async context (for hull.sleep / http.get access) */
    js->active_conn = kl_http_request_conn(req);
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

    /* The request's life: `res` holds it, and so does every continuation
     * the handler creates (js->active_life). It dies when the handler is
     * done - here, or when an awaiting handler completes or is cancelled. */
    HlReqLife *life = hl_req_life_new();
    if (!life) {
        JS_FreeValue(js->ctx, handler);
        JS_FreeValue(js->ctx, global);
        js->active_conn = NULL;
        js->active_req = NULL;
        return -1;
    }

    /* Build JS request and response objects */
    JSValue js_req = hl_js_make_request(js->ctx, req, life);
    JSValue js_res = hl_js_make_response_life(js, res, life);

    /* Call handler(req, res) */
    JSValue argv[2] = { js_req, js_res };
    js->active_life = life;
    JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);
    js->active_life = NULL;

    int result = 0;
    int attached = 0;   /* a continuation holds the handler promise */
    if (JS_IsException(ret)) {
        hl_js_dump_error(js);
        result = -1;
    } else if (JS_PromiseState(js->ctx, ret) == JS_PROMISE_PENDING) {
        /* Async handler - connection already suspended by hull.sleep
         * or similar async call. Store the outer handler promise on
         * the continuation (per-connection, not global) so the resume
         * callback can check when the handler completes. With no
         * continuation yet (a microtask-only await), see after the job
         * run below. */
        if (js->last_async_cont) {
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont,
                js->ctx, ret);
            js->last_async_cont = NULL;
            attached = 1;
        }
        js->async_pending = 1;
        result = 1; /* signal: handler suspended */
    } else if (JS_PromiseState(js->ctx, ret) == JS_PROMISE_REJECTED) {
        /* Async handler threw before its first await - the Promise is
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

    /* A pending handler with no continuation yet awaits only microtasks (or
     * something Hull does not drive). Run them now, with the life active so
     * a Hull call they make (hull.sleep, db.async, ...) takes it too, and
     * see whether a continuation turned up. If none did, the connection was
     * never suspended: the response goes out when this returns, so the
     * request is over whatever the handler does next - and its `res` must
     * not outlive it (it used to stay usable, onto a finished request). */
    if (result == 1 && !attached) {
        js->active_life = life;
        hl_js_run_jobs(js);
        js->active_life = NULL;
        if (js->last_async_cont) {
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, js->ctx, ret);
            js->last_async_cont = NULL;
        } else {
            int st = JS_PromiseState(js->ctx, ret);
            if (st == JS_PROMISE_PENDING) {
                log_warn("[hull:c] handler awaits a promise Hull does not "
                         "drive; the request ends now and its res is closed");
                result = 0;
            } else if (st == JS_PROMISE_REJECTED) {
                JSValue err = JS_PromiseResult(js->ctx, ret);
                const char *msg = JS_ToCString(js->ctx, err);
                log_error("[hull:c] async handler rejected: %s",
                          msg ? msg : "(unknown)");
                if (msg) JS_FreeCString(js->ctx, msg);
                JS_FreeValue(js->ctx, err);
                result = -1;
            } else {
                result = 0;
            }
            js->async_pending = 0;
        }
    }

    JS_FreeValue(js->ctx, ret);
    JS_FreeValue(js->ctx, js_res);
    JS_FreeValue(js->ctx, js_req);
    JS_FreeValue(js->ctx, handler);
    JS_FreeValue(js->ctx, global);

    /* Suspended: the continuations hold the life now. Otherwise the handler
     * is done with its request. Either way dispatch drops its own ref. */
    if (result != 1)
        hl_req_life_kill(life);
    hl_req_life_release(life);

    if (result != 1) {
        /* Sync path - clean up middleware ctx */
        js->active_conn = NULL;
        js->active_req = NULL;

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
    /* result == 1: handler suspended; async resume completes it (and
     * restores this request as the active one when it does). */
    js->active_conn = NULL;
    js->active_req  = NULL;

    /* Run any pending microtasks */
    hl_js_run_jobs(js);

    /* Whatever an un-awaited op made in that drain belongs to no run. */
    js->last_async_cont = NULL;
    return result;
}

void hl_js_keel_handler(KlHttpRequest *req, KlHttpResponse *res, void *user_data)
{
    HlJSRoute *route = (HlJSRoute *)user_data;
    int rc = hl_js_dispatch(route->js, route->handler_id, req, res);
    if (rc < 0) {
        hl_js_http_error_response(res);
    }
    /* rc == 1: handler suspended - don't write response.
     * Keel checks conn->state == KL_HTTP_CONN_SUSPENDED and returns. */
}

/* ── Middleware dispatch ────────────────────────────────────────────── */

int hl_js_dispatch_middleware(HlJS *js, int handler_id,
                              KlHttpRequest *req, KlHttpResponse *res)
{
    if (!js || !js->ctx || !req || !res)
        return -1;

    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_registry_guard_stale_txns(js->base.db_registry);

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

    /* Middleware returns synchronously, so its `res` belongs to this call
     * alone: the life ends as soon as it returns. */
    HlReqLife *life = hl_req_life_new();
    if (!life) {
        JS_FreeValue(js->ctx, handler);
        JS_FreeValue(js->ctx, global);
        return -1;
    }

    /* This request is the active one while the middleware runs: left as
     * the previous run had it, res.json compressed for ANOTHER request's
     * Accept-Encoding, and an async op suspended (or later completed) a
     * different connection. */
    js->active_conn = kl_http_request_conn(req);
    js->active_req  = req;
    js->last_async_cont = NULL;

    /* Build JS request and response objects */
    JSValue js_req = hl_js_make_request(js->ctx, req, life);
    JSValue js_res = hl_js_make_response_life(js, res, life);

    /* Call handler(req, res) - capture return value */
    JSValue argv[2] = { js_req, js_res };
    JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);
    hl_req_life_end(life);

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

    js->active_conn = NULL;
    js->active_req  = NULL;
    js->last_async_cont = NULL;   /* middleware is synchronous: nothing chains */
    return result;
}

int hl_js_keel_middleware(KlHttpRequest *req, KlHttpResponse *res, void *user_data)
{
    HlJSRoute *ctx = (HlJSRoute *)user_data;
    int rc = hl_js_dispatch_middleware(ctx->js, ctx->handler_id, req, res);
    if (rc < 0) {
        /* Middleware error - short-circuit with 500 */
        hl_js_http_error_response(res);
        return 1; /* short-circuit */
    }
    return rc;
}
