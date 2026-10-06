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

#include <stdint.h>
#include <string.h>

/* Forward declarations from bindings.c */
JSValue hl_js_make_request(JSContext *ctx, KlHttpRequest *req, struct HlReqLife *life);
JSValue hl_js_make_response(HlJS *js, KlHttpResponse *res);
JSValue hl_js_make_response_life(HlJS *js, KlHttpResponse *res, HlReqLife *life);

/* Free the request's middleware ctx (req->ctx: the JS value a middleware
 * left on req.ctx, or a test dispatch's JSON). Called wherever the request's
 * script work ends - once the handler's req object holds its own reference,
 * when a middleware short-circuits, and for an SSE route - since Keel resets
 * the request (dropping the pointer) without telling the runtime: before,
 * only a synchronous handler freed it, and every suspended, rejected or SSE
 * request pinned its ctx object in the JS heap for good (audit 6 H4). A
 * request that passed middleware but never reached a handler is covered by
 * request_done and the tracking below (audit 7 H2). */
static void req_ctx_release(HlJS *js, HlReqCtx *rctx)
{
    hl_reqctx_untrack(&js->req_ctxs, rctx);
    if (rctx->kind == HL_REQCTX_JS_VAL) {
        JSValue val;
        memcpy(&val, rctx->js_val_bytes, sizeof(val));
        if (js->ctx) JS_FreeValue(js->ctx, val);
    } else if (rctx->kind == HL_REQCTX_JSON) {
        hl_alloc_free(js->base.alloc, rctx->json.data, rctx->json.len + 1);
    }
    hl_alloc_free(js->base.alloc, rctx, sizeof(HlReqCtx));
}

/* @p req carries no ctx: any ctx still tracked for it was stored for an
 * earlier request on the same connection slot that never reached a handler
 * and whose end request_done did not see (an upgrade, a client gone
 * mid-body - Keel zeroed the request without telling the runtime, audit 7
 * H2). Free those. */
static void req_ctx_sweep(HlJS *js, KlHttpRequest *req)
{
    if (req->ctx) return;
    HlReqCtx *stale;
    while ((stale = hl_reqctx_find(&js->req_ctxs, req)) != NULL)
        req_ctx_release(js, stale);
}

void hl_js_req_ctx_free(HlJS *js, KlHttpRequest *req)
{
    if (!js || !req) return;
    if (req->ctx) {
        req_ctx_release(js, (HlReqCtx *)req->ctx);
        req->ctx = NULL;
    }
    req_ctx_sweep(js, req);
}

/* The response is sent, so nothing reads this request's ctx again: a
 * handler that ran holds its own reference (js_req.ctx), and no middleware
 * runs after the response. Frees the ctx of a request that passed
 * middleware but never reached a handler (a 404 / 405, a body error) now
 * rather than when its connection slot is next used. Keel's request object
 * is not const (the hook's signature only promises not to change it):
 * req->ctx is cleared so it never points at the freed ctx. */
void hl_js_request_done(HlJS *js, const KlHttpRequest *req)
{
    if (js && js->ctx && req)
        hl_js_req_ctx_free(js, (KlHttpRequest *)(uintptr_t)req);
}

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
    /* js_req.ctx holds its own reference now: nothing reads req->ctx after
     * the handler is called, however the handler ends. */
    hl_js_req_ctx_free(js, req);

    /* Call handler(req, res) */
    JSValue argv[2] = { js_req, js_res };
    js->active_life = life;
    JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);

    int result = 0;
    if (JS_IsException(ret)) {
        js->active_life = NULL;
        hl_js_dump_error(js);
        result = -1;
    } else {
        /* An async handler: its code after an `await` of something already
         * settled is still this request's - run it now, with the request and
         * its life active, so an op it starts belongs to this run; it used to
         * run in a drain with no request active, its op detached and never
         * waited for (audit 7 H1). Then wire every continuation into the run
         * and check the wait. A handler already over its budget is marked
         * so: its promise never settles, and the first resume answers 500. */
        int st;
        int waiting = hl_js_entry_park(js, ret, &st);
        js->active_life = NULL;
        if (waiting) {
            /* Suspended: the connection is held by the handler's first op;
             * the async resume completes the request. */
            js->async_pending = 1;
            result = 1;
        } else if (js->budget_tripped) {
            /* Returned (or rejected) after a trip - e.g. an un-awaited async
             * call that hit the limit: the run still failed. */
            log_error("[hull:c] handler exceeded the instruction limit");
            result = -1;
        } else if (st == JS_PROMISE_PENDING) {
            /* No continuation: the connection was never suspended, so the
             * response goes out when this returns and the request is over
             * whatever the handler does next - its `res` must not outlive it
             * (it used to stay usable, onto a finished request). */
            log_warn("[hull:c] handler awaits a promise Hull does not "
                     "drive; the request ends now and its res is closed");
        } else if (st == JS_PROMISE_REJECTED) {
            /* Async handler threw - the Promise is rejected (not an
             * exception). Log and return -1 so the caller writes a 500. */
            JSValue err = JS_PromiseResult(js->ctx, ret);
            const char *msg = JS_ToCString(js->ctx, err);
            log_error("[hull:c] async handler rejected: %s",
                      msg ? msg : "(unknown)");
            if (msg) JS_FreeCString(js->ctx, msg);
            else JS_FreeValue(js->ctx, JS_GetException(js->ctx));   /* L2 */
            JS_FreeValue(js->ctx, err);
            result = -1;
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

    /* result == 1: handler suspended; async resume completes it (and
     * restores this request as the active one when it does). */
    js->active_conn = NULL;
    js->active_req  = NULL;

    /* Jobs a synchronous handler queued (an async handler's ran above) run
     * with no request active: the request is over. */
    hl_js_run_jobs(js);

    /* Whatever an un-awaited op made belongs to no run. */
    js->last_async_cont = NULL;
    /* The entry is over (or parked, which no transaction may span): a
     * transaction still open is stale (audit 6 M1). */
    hl_db_registry_guard_stale_txns(js->base.db_registry);
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

/* A Promise - or any thenable - returned from middleware: an async
 * middleware. Reading `then` can throw; that fails closed too. */
static int hl_js_middleware_is_async(JSContext *ctx, JSValueConst v)
{
    if (!JS_IsObject(v))
        return 0;
    if ((int)JS_PromiseState(ctx, v) >= 0)
        return 1;
    JSValue then = JS_GetPropertyStr(ctx, v, "then");
    if (JS_IsException(then)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 1;
    }
    int is = JS_IsFunction(ctx, then);
    JS_FreeValue(ctx, then);
    return is;
}

int hl_js_dispatch_middleware(HlJS *js, int handler_id,
                              KlHttpRequest *req, KlHttpResponse *res)
{
    if (!js || !js->ctx || !req || !res)
        return -1;

    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_registry_guard_stale_txns(js->base.db_registry);

    hl_js_reset_request(js);
    /* The first middleware of a request: a ctx an earlier request on this
     * connection slot left behind is freed now. */
    req_ctx_sweep(js, req);

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

    /* Call handler(req, res) - capture return value. Middleware is
     * synchronous: Keel runs the next middleware and the handler on this
     * connection as soon as it returns, so an async op started now - which
     * suspends the connection - refuses (js->in_middleware, the async gate),
     * including from the microtasks drained below. */
    JSValue argv[2] = { js_req, js_res };
    js->in_middleware = 1;
    JSValue ret = JS_Call(js->ctx, handler, JS_UNDEFINED, 2, argv);
    hl_req_life_end(life);

    int result = 0;
    if (JS_IsException(ret)) {
        hl_js_dump_error(js);
        result = -1;
    } else if (js->budget_tripped) {
        log_error("[hull:c] middleware exceeded the instruction limit");
        result = -1;
    } else if (hl_js_middleware_is_async(js->ctx, ret)) {
        /* An async middleware's Promise used to coerce to 0, "continue":
         * `async (req, res) => { if (!(await ok(req))) return 1; return 0; }`
         * let every request through - an auth fail-open. Fail closed. */
        log_error("[hull:c] middleware returned a Promise: middleware must "
                  "be synchronous (return 0 to continue, non-zero to stop); "
                  "the request is answered 500");
        result = -1;
    } else {
        /* Capture return value: 0 = continue, non-zero = short-circuit */
        int32_t val = 0;
        if (JS_ToInt32(js->ctx, &val, ret) == 0)
            result = val;
        else
            JS_FreeValue(js->ctx, JS_GetException(js->ctx));
    }

    /* Store req.ctx as a JS value ref so the next middleware
     * or handler can retrieve the object directly (no JSON round-trip). */
    JSValue ctx_val = JS_GetPropertyStr(js->ctx, js_req, "ctx");
    if (JS_IsObject(ctx_val)) {
        /* Free previous ctx if any */
        if (req->ctx) {
            req_ctx_release(js, (HlReqCtx *)req->ctx);
            req->ctx = NULL;
        }
        /* Store native JS value - tracked, so it is freed even when the
         * request never reaches a handler (req_ctx_sweep). */
        HlReqCtx *rctx = hl_alloc_malloc(js->base.alloc, sizeof(HlReqCtx));
        if (rctx) {
            memset(rctx, 0, sizeof *rctx);
            rctx->kind = HL_REQCTX_JS_VAL;
            JSValue dup = JS_DupValue(js->ctx, ctx_val);
            memcpy(rctx->js_val_bytes, &dup, sizeof(dup));
            hl_reqctx_track(&js->req_ctxs, rctx, req);
            req->ctx = rctx;
        }
    }
    JS_FreeValue(js->ctx, ctx_val);

    JS_FreeValue(js->ctx, ret);
    JS_FreeValue(js->ctx, js_res);
    JS_FreeValue(js->ctx, js_req);
    JS_FreeValue(js->ctx, handler);
    JS_FreeValue(js->ctx, global);

    /* Run any pending microtasks - with no request active: whatever they
     * reach is not this middleware's to suspend (and in_middleware still
     * refuses an async op). */
    js->active_conn = NULL;
    js->active_req  = NULL;
    hl_js_run_jobs(js);
    js->in_middleware = 0;
    hl_db_registry_guard_stale_txns(js->base.db_registry);   /* audit 6 M1 */

    js->last_async_cont = NULL;   /* middleware is synchronous: nothing chains */
    if (js->budget_tripped && result >= 0) {
        log_error("[hull:c] middleware exceeded the instruction limit");
        result = -1;
    }
    /* Short-circuited (or failed): no later middleware or handler reads it. */
    if (result != 0)
        hl_js_req_ctx_free(js, req);
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
