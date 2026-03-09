/*
 * js_async.c — JS async continuation + hull.sleep()
 *
 * Implements HlJsAsyncCont (the JS-specific HlAsyncCont vtable) and
 * the hull.sleep() Promise-returning C function.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "hull/async.h"
#include "hull/alloc.h"
#include "hull/cap/http_async.h"

#include "quickjs.h"

#include <keel/keel.h>

#include "log.h"

/* ── HlJsAsyncCont ─────────────────────────────────────────────────── */

typedef struct HlJsAsyncCont {
    HlAsyncCont  base;       /* vtable — must be first member */
    HlJS        *js;         /* runtime instance */
    JSValue      resolve;    /* Promise resolve function */
    JSValue      reject;     /* Promise reject function */
    HlAllocator *alloc;
} HlJsAsyncCont;

/*
 * Resume the JS handler by resolving the inner promise and draining
 * microtasks. Then check the outer handler promise state:
 *   FULFILLED → KL_CONN_SENDING (handler completed)
 *   PENDING   → KL_CONN_SUSPENDED (handler re-yielded, new op active)
 *   REJECTED  → KL_CONN_SENDING (500 response written)
 */
static JSValue js_push_async_http_response(JSContext *ctx, void *driver);

static void hl_js_async_resume(HlAsyncCont *self, void *driver)
{
    HlJsAsyncCont *jc = (HlJsAsyncCont *)self;
    HlJS *js = jc->js;
    KlConn *conn = js->active_conn;
    JSContext *ctx = js->ctx;

    if (!conn || !ctx) return;

    /* Resolve the inner promise with the driver result */
    if (driver) {
        JSValue result = js_push_async_http_response(ctx, driver);
        JSValue ret = JS_Call(ctx, jc->resolve, JS_UNDEFINED, 1, &result);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, result);
    } else {
        JSValue ret = JS_Call(ctx, jc->resolve, JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, ret);
    }

    /* Free resolve/reject — no longer needed */
    JS_FreeValue(ctx, jc->resolve);
    JS_FreeValue(ctx, jc->reject);
    jc->resolve = JS_UNDEFINED;
    jc->reject = JS_UNDEFINED;

    /* Drain microtasks — this continues the handler past the await */
    hl_js_run_jobs(js);

    /* Check outer handler promise state */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promise = JS_GetPropertyStr(ctx, global, "__hull_async_promise");

    JSPromiseStateEnum state = JS_PromiseState(ctx, promise);

    if (state == JS_PROMISE_FULFILLED) {
        /* Handler completed — clean up, set SENDING */
        JS_FreeValue(ctx, promise);

        JSAtom atom = JS_NewAtom(ctx, "__hull_async_promise");
        JS_DeleteProperty(ctx, global, atom, 0);
        JS_FreeAtom(ctx, atom);

        js->async_pending = 0;

        if (conn->res.body_mode == KL_BODY_STREAM) {
            conn->state = KL_CONN_CLOSED;
        } else {
            conn->state = KL_CONN_SENDING;
        }
    } else if (state == JS_PROMISE_REJECTED) {
        /* Handler error — extract message, write 500 */
        JSValue result = JS_PromiseResult(ctx, promise);
        const char *msg = JS_ToCString(ctx, result);
        log_error("[hull:c] async js handler error: %s",
                  msg ? msg : "(unknown)");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, promise);

        JSAtom atom = JS_NewAtom(ctx, "__hull_async_promise");
        JS_DeleteProperty(ctx, global, atom, 0);
        JS_FreeAtom(ctx, atom);

        js->async_pending = 0;

        kl_response_status(&conn->res, 500);
        kl_response_header(&conn->res, "Content-Type", "text/plain");
        kl_response_body(&conn->res, "Internal Server Error", 21);
        conn->state = KL_CONN_SENDING;
    } else {
        /* PENDING — handler re-yielded (another async op in flight).
         * conn->state already set to KL_CONN_SUSPENDED by kl_async_suspend
         * inside the new hull.sleep/http call. */
        JS_FreeValue(ctx, promise);
    }

    JS_FreeValue(ctx, global);
}

/*
 * Cancel the JS handler — free promise refs without invoking.
 * Called when connection closes while handler is suspended.
 */
static void hl_js_async_cancel(HlAsyncCont *self)
{
    HlJsAsyncCont *jc = (HlJsAsyncCont *)self;
    HlJS *js = jc->js;
    JSContext *ctx = js->ctx;

    /* Free resolve/reject without calling them */
    JS_FreeValue(ctx, jc->resolve);
    JS_FreeValue(ctx, jc->reject);
    jc->resolve = JS_UNDEFINED;
    jc->reject = JS_UNDEFINED;

    /* Delete the outer handler promise */
    JSValue global = JS_GetGlobalObject(ctx);
    JSAtom atom = JS_NewAtom(ctx, "__hull_async_promise");
    JS_DeleteProperty(ctx, global, atom, 0);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, global);

    js->async_pending = 0;
    js->active_conn = NULL;
}

/*
 * Destroy the cont struct. Does NOT free the promise refs — that's
 * managed by the resume/cancel functions above.
 */
static void hl_js_async_destroy(HlAsyncCont *self)
{
    HlJsAsyncCont *jc = (HlJsAsyncCont *)self;
    hl_alloc_free(jc->alloc, jc, sizeof(HlJsAsyncCont));
}

/*
 * Create a JS async continuation. Takes ownership of resolve/reject
 * JSValues (caller must not free them).
 */
HlAsyncCont *hl_js_async_cont_create(HlJS *js,
                                              JSValue resolve,
                                              JSValue reject,
                                              HlAllocator *alloc)
{
    HlJsAsyncCont *jc = hl_alloc_malloc(alloc, sizeof(HlJsAsyncCont));
    if (!jc) return NULL;

    jc->base.resume  = hl_js_async_resume;
    jc->base.cancel  = hl_js_async_cancel;
    jc->base.destroy = hl_js_async_destroy;
    jc->js      = js;
    jc->resolve = resolve;
    jc->reject  = reject;
    jc->alloc   = alloc;

    return &jc->base;
}

/* ── Push async HTTP response into JS ─────────────────────────────── */

static JSValue js_push_async_http_response(JSContext *ctx, void *driver)
{
    HlHttpClient *client = (HlHttpClient *)driver;
    HlHttpResponse *resp = &client->resp;

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

/* ── hull.sleep(ms) ───────────────────────────────────────────────── */

/*
 * hull.sleep(ms) — return a Promise that resolves after `ms` milliseconds.
 * Uses KlAsyncOp deadline (no driver, no FD). The Keel deadline sweep
 * fires hl_async_on_deadline_sleep, which calls kl_async_complete.
 */
static JSValue js_hull_sleep(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "hull.sleep requires (ms)");

    int64_t ms;
    if (JS_ToInt64(ctx, &ms, argv[0]) != 0)
        return JS_EXCEPTION;

    if (ms <= 0)
        return JS_UNDEFINED; /* no-op for zero/negative */

    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->server || !js->active_conn)
        return JS_ThrowInternalError(ctx,
            "hull.sleep() can only be called from a request handler");

    KlServer *server = js->server;
    KlConn *conn = js->active_conn;

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(server, js->base.alloc);
    if (!actx)
        return JS_ThrowInternalError(ctx, "hull.sleep(): out of memory");

    /* Create Promise and get resolve/reject functions */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    /* Create JS continuation — takes ownership of resolve/reject */
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "hull.sleep(): out of memory");
    }
    actx->cont = cont;

    /* Set up sleep op (deadline-only, no driver) */
    actx->op.deadline_ms = kl_monotonic_ms() + (uint64_t)ms;
    actx->op.on_deadline = hl_async_on_deadline_sleep;
    actx->driver = NULL;
    actx->free_driver = NULL;

    /* Suspend the connection */
    if (kl_async_suspend(server, conn, &actx->op) < 0) {
        actx->cont->destroy(actx->cont);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "hull.sleep(): failed to suspend connection");
    }

    return promise;
}

/* ── Global registration ─────────────────────────────────────────── */

void hl_js_add_hull_global(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue hull = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, hull, "sleep",
        JS_NewCFunction(ctx, js_hull_sleep, "sleep", 1));

    JS_SetPropertyStr(ctx, global, "hull", hull);
    JS_FreeValue(ctx, global);
}
