/*
 * js_async.c — JS async continuation + hull.sleep()
 *
 * Implements HlJsAsyncCont (the JS-specific HlAsyncCont vtable) and
 * the hull.sleep() Promise-returning C function.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "internal.h"
#include "hull/http_feature.h"  /* hl_js_http_error_response (HTTP-feature seam) */
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"

#include "quickjs.h"

#include <keel/keel.h>

#include "log.h"

/* ── HlJsAsyncCont ─────────────────────────────────────────────────── */

typedef JSValue (*HlJsPushResultFn)(JSContext *ctx, void *driver);

typedef struct HlJsAsyncCont {
    HlAsyncCont       base;         /* vtable — must be first member */
    HlJS             *js;           /* runtime instance */
    JSValue           resolve;      /* Promise resolve function */
    JSValue           reject;       /* Promise reject function */
    HlAllocator      *alloc;
    HlJsPushResultFn  push_result;  /* NULL = no result (sleep) */
    KlConn           *conn;         /* connection to resume (NULL = detached) */
    JSValue           handler_promise; /* outer handler promise */
    void             *timer_ctx;    /* HlJSTimer* if running in a timer callback */
    /* Generic "handler finally completed" hook (subsystem-agnostic). Lets a
     * dispatch site defer teardown that must not run while the handler is
     * still suspended (e.g. the ws on_close conn teardown in ws.c). Called
     * once on fulfilled / rejected completion. */
    void            (*on_complete)(HlJS *js, void *ctx);
    void             *on_complete_ctx;
} HlJsAsyncCont;

/*
 * Resume the JS handler by resolving the inner promise and draining
 * microtasks. Then check the outer handler promise state:
 *   FULFILLED → KL_CONN_SENDING (handler completed)
 *   PENDING   → KL_CONN_SUSPENDED (handler re-yielded, new op active)
 *   REJECTED  → KL_CONN_SENDING (500 response written)
 *
 * The connection and handler promise are stored per-continuation
 * rather than in the HlJS singleton.  This allows multiple connections
 * to be suspended concurrently (e.g., self-fetch: the original connection
 * is suspended for the async HTTP response, while the server-side
 * connection for /api/slow can also suspend for hull.sleep).
 */
/* Forward declarations for timer reschedule (defined in timers.c —
 * dropped under HL_ENABLE_HTTP=0; call sites are guarded). */
#ifdef HL_ENABLE_HTTP_SERVER
void hl_js_timer_reschedule(HlJSTimer *t);
#endif

/* Forward decl — vtable slot impl defined just below hl_js_async_resume.
 * hl_js_async_resume's PENDING branch uses it as a discriminator to
 * decide whether the new cont is a standard HlJsAsyncCont (and thus
 * has a timer_ctx field to transfer). */
static void hl_js_async_cont_set_handler_promise_impl(HlAsyncCont *self,
                                                         void *ctx_v,
                                                         void *promise_v);

static void hl_js_async_resume(HlAsyncCont *self, void *driver)
{
    HlJsAsyncCont *jc = (HlJsAsyncCont *)self;
    HlJS *js = jc->js;
    KlConn *conn = jc->conn;
    JSContext *ctx = js->ctx;

    if (!ctx) return;

    /* Restore per-request context so C functions called during resume
     * (e.g., another http.async.get) can find the active connection */
    js->active_conn = conn;

    /* Resolve the inner promise with the driver result */
    if (driver && jc->push_result) {
        JSValue result = jc->push_result(ctx, driver);
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

    /* Re-arm the deferred-teardown hook so a re-await inside the handler
     * carries it onto the next continuation. */
    js->active_on_complete     = jc->on_complete;
    js->active_on_complete_ctx = jc->on_complete_ctx;

    /* Drain microtasks — this continues the handler past the await */
    hl_js_run_jobs(js);

    js->active_on_complete     = NULL;
    js->active_on_complete_ctx = NULL;

    /* Check outer handler promise state (per-continuation ref) */
    JSPromiseStateEnum state = JS_PromiseState(ctx, jc->handler_promise);

    if (state == JS_PROMISE_FULFILLED) {
        /* Handler completed — clean up */
        int cancelled = 0;
        if (jc->timer_ctx) {
            JSValue result = JS_PromiseResult(ctx, jc->handler_promise);
            if (JS_IsBool(result) && JS_ToBool(ctx, result) == 0)
                cancelled = 1;
            JS_FreeValue(ctx, result);
        }

        JS_FreeValue(ctx, jc->handler_promise);
        jc->handler_promise = JS_UNDEFINED;
        jc->conn = NULL;

        js->async_pending = 0;
        js->active_conn = NULL;
        js->dispatch_depth--;

        /* Handler that awaited has now completed — run any deferred-teardown
         * hook (e.g. ws on_close conn teardown). */
        if (jc->on_complete) {
            jc->on_complete(js, jc->on_complete_ctx);
            jc->on_complete = NULL;
        }

#ifdef HL_ENABLE_HTTP_SERVER
        if (conn) {
            if (conn->res.body_mode == KL_BODY_STREAM) {
                conn->state = KL_CONN_CLOSED;
            } else {
                conn->state = KL_CONN_SENDING;
            }
        }
#endif

        /* Timer async completion: clear in_flight and reschedule.
         * Timers are HTTP-only (app.every / app.daily); CLI builds
         * never set timer_ctx so the branch is dead. */
#ifdef HL_ENABLE_HTTP_SERVER
        if (jc->timer_ctx) {
            HlJSTimer *t = (HlJSTimer *)jc->timer_ctx;
            t->in_flight = 0;
            if (!cancelled)
                hl_js_timer_reschedule(t);
        }
#else
        (void)cancelled;
#endif
    } else if (state == JS_PROMISE_REJECTED) {
        /* Handler error — extract message, write 500 */
        JSValue result = JS_PromiseResult(ctx, jc->handler_promise);
        const char *msg = JS_ToCString(ctx, result);
        if (conn)
            log_error("[hull:c] async js handler error: %s",
                      msg ? msg : "(unknown)");
        else
            log_error("[hull:timer] error: %s", msg ? msg : "(unknown)");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, result);

        JS_FreeValue(ctx, jc->handler_promise);
        jc->handler_promise = JS_UNDEFINED;
        jc->conn = NULL;

        js->async_pending = 0;
        js->active_conn = NULL;
        js->dispatch_depth--;

        /* Run any deferred-teardown hook (handler rejected after awaiting). */
        if (jc->on_complete) {
            jc->on_complete(js, jc->on_complete_ctx);
            jc->on_complete = NULL;
        }

#ifdef HL_ENABLE_HTTP_SERVER
        if (conn) {
            hl_js_http_error_response(&conn->res);
            conn->state = KL_CONN_SENDING;
        }
#endif

        /* Timer error: clear in_flight and reschedule anyway. CLI
         * builds have no timers. */
#ifdef HL_ENABLE_HTTP_SERVER
        if (jc->timer_ctx) {
            HlJSTimer *t = (HlJSTimer *)jc->timer_ctx;
            t->in_flight = 0;
            hl_js_timer_reschedule(t);
        }
#endif
    } else {
        /* PENDING — handler re-yielded (another async op in flight).
         * Transfer the handler-promise to the new cont via the vtable
         * (it may be an HlJsAsyncCont OR an HlJsMpCont, different
         * layouts). Timer-ctx transfer stays direct: only HlJsAsyncCont
         * carries a timer_ctx, and timer paths only ever create that
         * type (timers can't originate streaming-multipart routes). */
        if (js->last_async_cont) {
            HlAsyncCont *nc = (HlAsyncCont *)js->last_async_cont;
            if (nc->set_handler_promise)
                nc->set_handler_promise(nc, ctx, &jc->handler_promise);
            /* Only transfer timer_ctx when the new cont is a standard
             * HlJsAsyncCont (the only type that has the field). */
            if (nc->set_handler_promise == hl_js_async_cont_set_handler_promise_impl) {
                HlJsAsyncCont *new_jc = (HlJsAsyncCont *)nc;
                new_jc->timer_ctx = jc->timer_ctx;
                jc->timer_ctx = NULL;
            }
            js->last_async_cont = NULL;
        }
        JS_FreeValue(ctx, jc->handler_promise);
        jc->handler_promise = JS_UNDEFINED;
        jc->conn = NULL;
    }
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

    /* Free the per-continuation handler promise */
    if (!JS_IsUndefined(jc->handler_promise)) {
        JS_FreeValue(ctx, jc->handler_promise);
        jc->handler_promise = JS_UNDEFINED;
    }
    jc->conn = NULL;
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
 * push_result: called on resume to convert driver result to JSValue.
 *              NULL for sleep (no result to push).
 */
/* set_handler_promise vtable slot for HlJsAsyncCont — accessed via
 * the public hl_js_async_cont_set_handler_promise dispatcher (which
 * just calls cont->set_handler_promise). Each JS cont type defines
 * its own setter; the dispatcher no longer cares about the concrete
 * cont type. */
static void hl_js_async_cont_set_handler_promise_impl(HlAsyncCont *self,
                                                         void *ctx_v,
                                                         void *promise_v)
{
    HlJsAsyncCont *jc = (HlJsAsyncCont *)self;
    JSContext *ctx = (JSContext *)ctx_v;
    JSValue promise = *(JSValue *)promise_v;
    jc->handler_promise = JS_DupValue(ctx, promise);
}

HlAsyncCont *hl_js_async_cont_create(HlJS *js,
                                              JSValue resolve,
                                              JSValue reject,
                                              HlAllocator *alloc,
                                              JSValue (*push_result)(JSContext *, void *))
{
    HlJsAsyncCont *jc = hl_alloc_malloc(alloc, sizeof(HlJsAsyncCont));
    if (!jc) return NULL;

    jc->base.resume              = hl_js_async_resume;
    jc->base.cancel              = hl_js_async_cancel;
    jc->base.destroy             = hl_js_async_destroy;
    jc->base.set_handler_promise = hl_js_async_cont_set_handler_promise_impl;
    jc->js          = js;
    jc->resolve     = resolve;
    jc->reject      = reject;
    jc->alloc       = alloc;
    jc->push_result = push_result;

    /* Capture per-request connection so multiple connections can be
     * suspended concurrently without clobbering each other.
     * handler_promise is set later by dispatch (two-step wiring). */
    jc->conn            = js->active_conn;
    jc->handler_promise = JS_UNDEFINED;
    jc->timer_ctx       = js->active_timer;  /* inherit timer ctx if in timer callback */
    jc->on_complete     = js->active_on_complete;     /* deferred-teardown hook */
    jc->on_complete_ctx = js->active_on_complete_ctx;

    /* Store pointer so dispatch/resume can wire handler_promise */
    js->last_async_cont = jc;

    return &jc->base;
}

/*
 * Set the outer handler promise on a continuation.
 *
 * Called by hl_js_dispatch after detecting a PENDING handler return,
 * since the handler promise only exists after JS_Call returns. The
 * cont may be any JS cont type (HlJsAsyncCont, HlJsMpCont, …) — each
 * sets its own `set_handler_promise` vtable slot in async.h's
 * HlAsyncCont, so this dispatcher is just a thin pass-through.
 */
void hl_js_async_cont_set_handler_promise(HlAsyncCont *cont,
                                            JSContext *ctx,
                                            JSValue promise)
{
    if (cont && cont->set_handler_promise)
        cont->set_handler_promise(cont, ctx, &promise);
}

/* ── hull.sleep(ms) ───────────────────────────────────────────────── */

/*
 * hull.sleep(ms) — return a Promise that resolves after `ms` milliseconds.
 * Uses KlAsyncOp deadline (no driver, no FD). The Keel deadline sweep
 * fires hl_async_on_deadline_sleep, which calls kl_async_complete.
 */
/*
 * Set the timer context on a JS async continuation.
 * Called by the timer trampoline so that async resume can reschedule.
 */
void hl_js_async_cont_set_timer(HlAsyncCont *cont, void *timer)
{
    HlJsAsyncCont *jc = (HlJsAsyncCont *)cont;
    jc->timer_ctx = timer;
}

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
    if (!js || !js->base.async_ctx)
        return JS_ThrowInternalError(ctx,
            "hull.sleep() requires an active event loop");

    KlServer *server = js->server;
    KlConn *conn = js->active_conn;

    /* Create async ctx */
    HlAsyncCtx *actx = hl_async_ctx_create(server, js->base.net_ctx, js->base.alloc);
    if (!actx)
        return JS_ThrowInternalError(ctx, "hull.sleep(): out of memory");

    /* Create Promise and get resolve/reject functions */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_async_ctx_free(actx);
        return JS_EXCEPTION;
    }

    /* Create JS continuation — takes ownership of resolve/reject.
     * No push_result — sleep has no return value. */
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  NULL);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_async_ctx_free(actx);
        return JS_ThrowInternalError(ctx, "hull.sleep(): out of memory");
    }
    actx->cont = cont;
    actx->driver = NULL;
    actx->free_driver = NULL;

    if (conn) {
        /* Attached mode: use KlAsyncOp deadline via kl_async_suspend */
        actx->op.deadline_ms = hl_async_backend()->monotonic_ms() + (uint64_t)ms;
        actx->op.on_deadline = hl_async_on_deadline_sleep;
        actx->detached = 0;

        if (hl_net_op_suspend(js->base.net_ctx, (HlReqHandle *)conn, (HlSuspendOp *)&actx->op) < 0) {
            actx->cont->cancel(actx->cont);   /* frees the (uninvoked) resolve/reject */
            actx->cont->destroy(actx->cont);
            hl_async_ctx_free(actx);
            JS_FreeValue(ctx, promise);
            return JS_ThrowInternalError(ctx,
                "hull.sleep(): failed to suspend connection");
        }
    } else {
        /* Detached mode: schedule via the async backend vtable. The
         * underlying loop is the same one KlServer drives (wrapped in
         * serve.c::wire_caps), so the timer fires alongside HTTP work. */
        actx->detached = 1;

        const HlAsyncBackend *be = hl_async_backend();
        uint64_t tid = be->timer_add(js->base.async_ctx, (uint64_t)ms,
                                      hl_detached_timer_fire, actx);
        if (tid == 0) {
            actx->cont->cancel(actx->cont);   /* frees the (uninvoked) resolve/reject */
            actx->cont->destroy(actx->cont);
            hl_async_ctx_free(actx);
            JS_FreeValue(ctx, promise);
            return JS_ThrowInternalError(ctx,
                "hull.sleep(): failed to add timer");
        }
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
