/*
 * js_async.c - JS async continuation + hull.sleep()
 *
 * Implements HlJsAsyncCont (the JS-specific HlAsyncCont vtable) and
 * the hull.sleep() Promise-returning C function.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "internal.h"
#include "db_wait.h"   /* hl_js_db_refuse_wait */
#include "hull/shared/req_life.h"
#include "hull/http_feature.h"  /* hl_js_http_error_response (HTTP-feature seam) */
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"
#include "hull/cap/db_budget.h"   /* the SQL budget binding (audit 9 H4) */

#include "quickjs.h"

#include <keel/keel.h>

#include "log.h"

#include <stddef.h>   /* offsetof */
#include <stdlib.h>
#include <string.h>   /* memset */

/* ── HlJsAsyncCont ─────────────────────────────────────────────────── */

typedef JSValue (*HlJsPushResultFn)(JSContext *ctx, void *driver);

typedef struct HlJsAsyncCont HlJsAsyncCont;

struct HlJsAsyncCont {
    HlAsyncCont       base;         /* vtable - must be first member */
    HlJsRunLink       link;         /* second: the HlJsContHead layout */
    HlJS             *js;           /* runtime instance */
    JSValue           resolve;      /* Promise resolve function */
    JSValue           reject;       /* Promise reject function */
    HlAllocator      *alloc;
    HlJsPushResultFn  push_result;  /* NULL = no result (sleep) */
    KlHttpConn           *conn;         /* connection to resume (NULL = detached) */
    KlHttpRequest    *req;          /* request for kl_http_request_send_response (transition to SENDING) */
    int               holds;        /* this op has `conn` suspended (life->held) */
    void             *timer_ctx;    /* HlJSTimer* if running in a timer callback */
    /* Generic "handler finally completed" hook (subsystem-agnostic). Lets a
     * dispatch site defer teardown that must not run while the handler is
     * still suspended (e.g. the ws on_close conn teardown in ws.c). Called
     * once on fulfilled / rejected completion. */
    void            (*on_complete)(HlJS *js, void *ctx);
    void             *on_complete_ctx;
    HlReqLife        *life;         /* the handler's request life (ref held) */
    int               attached;     /* counted in life->attached */
    int               cli_main;     /* made by app.main's own code */
};

/* The op no longer holds the connection (its resume is over, or it was
 * cancelled / never armed). Idempotent. */
static void hl_js_cont_unattach(HlJsAsyncCont *jc)
{
    if (!jc->attached) return;
    jc->attached = 0;
    if (jc->life && jc->life->attached > 0)
        jc->life->attached--;
}

/* The op no longer has the connection suspended: Keel retired it (its
 * resume, or a cancel), or it was never armed. Idempotent. */
static void hl_js_cont_unhold(HlJsAsyncCont *jc)
{
    if (!jc->holds) return;
    jc->holds = 0;
    if (jc->life) jc->life->held = 0;
}

KlHttpConn *hl_js_cont_suspend_conn(HlAsyncCont *cont)
{
    const HlJsAsyncCont *jc = (const HlJsAsyncCont *)cont;
    return jc && jc->holds ? jc->conn : NULL;
}

/*
 * Resume the JS handler by resolving the inner promise and draining
 * microtasks. Then check the outer handler promise state. Under Keel v3
 * this only FINALIZES the response; Keel owns the send after on_resume
 * (see include/hull/shared/async.h). Per promise state:
 *   FULFILLED → build the response, kl_async_complete lets Keel send it
 *   PENDING   → handler re-yielded; a new op is active, conn stays suspended
 *   REJECTED  → build a 500, kl_async_complete lets Keel send it
 *
 * The connection and handler promise are stored per-continuation
 * rather than in the HlJS singleton.  This allows multiple connections
 * to be suspended concurrently (e.g., self-fetch: the original connection
 * is suspended for the async HTTP response, while the server-side
 * connection for /api/slow can also suspend for hull.sleep).
 */
/* Forward declarations for timer reschedule (defined in timers.c -
 * dropped under HL_ENABLE_HTTP=0; call sites are guarded). */
#ifdef HL_ENABLE_HTTP_SERVER
void hl_js_timer_reschedule(HlJSTimer *t);
#endif

static void hl_js_run_once_release(HlJsRunOnce *o)
{
    if (o && --o->refs <= 0) free(o);
}

/* An earlier continuation of the same handler run that has not been given
 * the handler promise yet: dispatch hands it to the LAST one created, and
 * used to drop every other one's - so when those resumed last, the
 * handler's completion was never seen (a timer stopped for good, a ws conn
 * was never torn down, a request never answered). Walked and cleared when
 * the promise is attached. */
void hl_js_run_wire(HlJsRunLink *last, JSContext *ctx, JSValue promise,
                    HlJsRunOnce *once)
{
    if (!once) {
        once = (HlJsRunOnce *)malloc(sizeof *once);
        if (once) {
            once->refs = 0; once->done = 0; once->tripped = 0;
            once->txn_held = 0; once->hold = NULL;
        }
    }
    for (HlJsRunLink *c = last; c; ) {
        HlJsRunLink *prev = c->unwired_prev;
        c->unwired_prev = NULL;
        if (JS_IsUndefined(c->handler_promise))
            c->handler_promise = JS_DupValue(ctx, promise);
        if (!c->once && once) {
            c->once = once;
            once->refs++;
        }
        c = prev;
    }
    if (once && once->refs == 0) free(once);
}

/* A cont with no run record (allocation failed) completes as before. */
int hl_js_run_claim(HlJsRunLink *link)
{
    if (!link->once) return 1;
    if (link->once->done) return 0;
    link->once->done = 1;
    return 1;
}

void hl_js_run_unlink(HlJsRunLink *link, JSContext *ctx)
{
    if (ctx && !JS_IsUndefined(link->handler_promise))
        JS_FreeValue(ctx, link->handler_promise);
    link->handler_promise = JS_UNDEFINED;
    hl_js_run_once_release(link->once);
    link->once = NULL;
}

void hl_js_run_push(HlJS *js, HlAsyncCont *cont)
{
    HlJsRunLink *link = hl_js_cont_link(cont);
    link->handler_promise = JS_UNDEFINED;
    link->once = NULL;
    link->unwired_prev = hl_js_cont_link((HlAsyncCont *)js->last_async_cont);
    js->last_async_cont = cont;
}

HlJsRunOnce *hl_js_run_attach(HlJS *js, JSValue promise)
{
    HlAsyncCont *cont = (HlAsyncCont *)js->last_async_cont;
    js->last_async_cont = NULL;
    if (!cont) return NULL;
    if (cont->set_handler_promise)
        cont->set_handler_promise(cont, js->ctx, &promise);
    HlJsRunOnce *run = hl_js_cont_link(cont)->once;
    if (!run) return NULL;
    /* Over budget already: the handler's promise never settles, so the
     * first continuation to resume finishes the run as failed. */
    if (js->budget_tripped) run->tripped = 1;
    run->refs++;
    return run;
}

void hl_js_run_drop(HlJS *js, HlJsRunOnce *run)
{
    if (!run) return;
    if (js->budget_tripped) run->tripped = 1;
    hl_js_run_once_release(run);
}

/* Did this continuation's run go over its budget (now, or in an earlier
 * resume)? Marks the run when it is the current resume that tripped. */
static int hl_js_run_tripped(HlJS *js, HlJsRunLink *link)
{
    if (js->budget_tripped && link->once) link->once->tripped = 1;
    return js->budget_tripped || (link->once && link->once->tripped);
}

/* app.main's promise is watched by js_cli_main_settle, which a run that
 * failed without settling it never reaches: one over its budget (a tripped
 * run settles nothing), or one that waited holding a transaction (failed at
 * its next resume, not continued). Fail main with @p why and stop the loop
 * instead of leaving the CLI waiting forever. Only for main's own
 * continuations (HlJsAsyncCont.cli_main): a detached op of a ws-client
 * callback, or one nobody awaits, failing does not end the CLI. */
static void hl_js_cli_main_trip(HlJS *js, const char *why)
{
    if (!js->cli_main_active || !js->cli_main_value) return;
    JSValue *slot = (JSValue *)js->cli_main_value;
    JS_FreeValue(js->ctx, *slot);
    *slot = JS_NewString(js->ctx, why);
    js->cli_main_rejected = 1;
    js->cli_main_active = 0;
    if (js->base.async_ctx)
        hl_async_backend()->stop(js->base.async_ctx);
}

int hl_js_conn_held_elsewhere(HlJS *js, KlHttpConn *conn)
{
    if (!conn || !hl_net_op_holder(js->base.net_ctx, (HlReqHandle *)conn))
        return 0;
    log_error("[hull:c] a resumed request's connection is suspended by an "
              "operation its run does not own; its response is left to that "
              "operation's completion");
    return 1;
}

int hl_js_run_defer_to_holder(HlJS *js, HlJsRunLink *link, HlReqLife *life)
{
    if (!life || !hl_req_life_live(life) || !life->held ||
        (link->once && link->once->hold))
        return 0;
    if (js->last_async_cont) {
        hl_js_run_wire(hl_js_cont_link((HlAsyncCont *)js->last_async_cont),
                       js->ctx, link->handler_promise, link->once);
        js->last_async_cont = NULL;
    }
    return 1;
}

/* ── The hold op ─────────────────────────────────────────────────────
 *
 * Only one op at a time can have a connection suspended in Keel, so a
 * request whose handler waits on several Hull ops at once has one attached
 * op (the holder) and runs the others detached. When the holder resumes and
 * the handler is still waiting - on the detached ones - something must keep
 * the connection suspended: Keel otherwise sends the response as it stands
 * as soon as the resume returns. The hold op does that. It has no deadline
 * (each op it waits for has its own), and is completed by whichever
 * continuation finishes the run, which tells it how. */

enum { HL_JS_HOLD_SEND = 1, HL_JS_HOLD_ERROR = 2 };

typedef struct {
    HlAsyncCont   base;
    HlJS         *js;
    HlJsRunOnce  *run;      /* ref held; run->hold points at our ctx */
    HlReqLife    *life;     /* ref held */
    KlHttpConn   *conn;
    KlHttpRequest *req;
    HlAllocator  *alloc;
    int           outcome;  /* HL_JS_HOLD_*, or 0: send what res holds */
} HlJsHoldCont;

static void hl_js_hold_detach(HlJsHoldCont *h)
{
    if (h->run) h->run->hold = NULL;
    if (h->life) h->life->held = 0;
}

static void hl_js_hold_resume(HlAsyncCont *self, void *driver)
{
    (void)driver;
    HlJsHoldCont *h = (HlJsHoldCont *)self;
    hl_js_hold_detach(h);
#ifdef HL_ENABLE_HTTP_SERVER
    if (h->outcome == HL_JS_HOLD_SEND)
        hl_js_http_resume_send(h->js, h->conn, h->req);
    else if (h->outcome == HL_JS_HOLD_ERROR)
        hl_js_http_resume_error(h->js, h->conn, h->req);
#endif
}

static void hl_js_hold_cancel(HlAsyncCont *self)
{
    HlJsHoldCont *h = (HlJsHoldCont *)self;
    hl_js_hold_detach(h);
    /* The connection is gone: so is the snapshot its 500 would have kept
     * (audit 12). */
    hl_res_base_forget_conn(&h->js->res_bases, h->conn);
    hl_req_life_kill(h->life);   /* the connection is gone */
}

static void hl_js_hold_destroy(HlAsyncCont *self)
{
    HlJsHoldCont *h = (HlJsHoldCont *)self;
    hl_req_life_release(h->life);
    hl_js_run_once_release(h->run);
    hl_alloc_free(h->alloc, h, sizeof *h);
}

/* Suspend @p conn with a hold op for @p run. 0, or -1 (nothing suspended). */
static int hl_js_hold_arm(HlJS *js, KlHttpConn *conn, KlHttpRequest *req,
                          HlReqLife *life, HlJsRunOnce *run)
{
    if (!run || !life || run->hold) return -1;
    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx,
                                           js->base.alloc);
    if (!actx) return -1;
    HlJsHoldCont *h = hl_alloc_malloc(js->base.alloc, sizeof *h);
    if (!h) {
        hl_async_ctx_free(actx);
        return -1;
    }
    memset(h, 0, sizeof *h);
    h->base.resume  = hl_js_hold_resume;
    h->base.cancel  = hl_js_hold_cancel;
    h->base.destroy = hl_js_hold_destroy;
    h->js = js; h->conn = conn; h->req = req; h->alloc = js->base.alloc;
    h->life = life; hl_req_life_retain(life);
    h->run = run;   run->refs++;
    actx->cont = &h->base;
    if (hl_net_op_suspend(js->base.net_ctx, (HlReqHandle *)conn,
                          (HlSuspendOp *)&actx->op) < 0) {
        hl_js_hold_destroy(&h->base);
        hl_async_ctx_free(actx);
        return -1;
    }
    run->hold = actx;
    life->held = 1;
    return 0;
}

/* Complete @p run's hold op (the run is over): Keel sends the response,
 * after @p outcome is applied. Call last - Keel may go on to serve the next
 * request on the connection from inside this call. */
static void hl_js_hold_release(HlJS *js, HlAsyncCtx *hold, int outcome)
{
    if (!hold) return;
    ((HlJsHoldCont *)hold->cont)->outcome = outcome;
    hl_net_op_complete(js->base.net_ctx, (HlSuspendOp *)&hold->op);
}

int hl_js_entry_park(HlJS *js, JSValue ret, int *state)
{
    int st = (int)JS_PromiseState(js->ctx, ret);
    if (st == JS_PROMISE_PENDING) {
        hl_js_run_jobs(js);
        st = (int)JS_PromiseState(js->ctx, ret);
    }
    *state = st;
    if (st != JS_PROMISE_PENDING)
        return 0;
    if (!js->last_async_cont) {
        /* Waiting on a promise Hull does not drive: the entry ends now, but
         * the handler may yet resume - from whichever later entry settles
         * the promise, inside that entry's transaction. Not holding one of
         * its own: the check says so and rolls it back (audit 8 c_db L1). */
        hl_js_run_yield_check(js, NULL);
        return 0;
    }
    HlJsRunOnce *run = hl_js_run_attach(js, ret);
    hl_js_run_yield_check(js, run);
    hl_js_run_drop(js, run);
    return 1;
}

int hl_js_run_yield_check(HlJS *js, HlJsRunOnce *run)
{
    const char *txn = hl_db_registry_open_txn(js->base.db_registry);
    /* Always NULL only in a DB-less build (the inline stub). */
    // cppcheck-suppress knownConditionTrueFalse
    if (!txn) return 0;
    log_error("[hull:c] a handler waited while a transaction was open on "
              "database connection '%s': other requests use the same "
              "connection while it waits. Commit or roll back first. The "
              "transaction is rolled back and the run fails", txn);
    hl_db_registry_guard_stale_txns(js->base.db_registry);
    if (run) run->txn_held = 1;
    return 1;
}

int hl_js_async_gate(JSContext *ctx, HlJS *js, const char *what)
{
    if (!js) return 0;
    if (js->in_middleware) {
        JS_ThrowTypeError(ctx,
            "%s cannot run in middleware: middleware is synchronous (it "
            "returns 0 to continue, non-zero to stop); do async work in the "
            "route handler", what);
        return -1;
    }
    if (js->active_conn && js->active_life && js->active_life->parked > 0) {
        JS_ThrowTypeError(ctx,
            "%s cannot start while req.multipart() is waiting for more of "
            "the request body; await the multipart read first", what);
        return -1;
    }
    return 0;
}

int hl_js_op_suspend(HlJS *js, struct HlSuspendOp *op)
{
    /* Only for the active request's own op: a connection active with no
     * live life is never suspended (audit 8 H1, hl_js_async_cont_create). */
    if (!js || !js->active_conn || js->in_middleware || !js->active_life ||
        !hl_req_life_live(js->active_life) || js->active_life->parked > 0)
        return -1;
    return hl_net_op_suspend(js->base.net_ctx,
                             (HlReqHandle *)js->active_conn, op);
}

static void hl_js_async_resume(HlAsyncCont *self, void *driver)
{
    HlJsAsyncCont *jc = (HlJsAsyncCont *)self;
    HlJS *js = jc->js;
    KlHttpConn *conn = jc->conn;
    JSContext *ctx = js->ctx;

    if (!ctx) return;

    /* An entry point: the resumed run gets a budget of its own (it used to
     * inherit whatever count the last entry left - and after one trip,
     * every resume failed at its first poll until the next dispatch). A
     * resume of app.main's own code keeps main's deadline default. */
    hl_js_budget_arm_kind(js, jc->cli_main ? HL_RUN_MAIN : HL_RUN_ENTRY);
    js->active_timer = NULL;
    /* A stale transaction is not this run's to join (audit 6 M1). */
    hl_db_registry_guard_stale_txns(js->base.db_registry);

    /* Keel retired this op: if it had the connection suspended, nothing has
     * now (the connection is PROCESSING until this returns), and an op the
     * handler starts below may suspend it in turn. */
    int was_holder = jc->holds;
    hl_js_cont_unhold(jc);
    /* The request is over (its run completed, or the client went away): an
     * op left running past it resumes with no request - the connection may
     * already serve another client. */
    if (jc->life && !hl_req_life_live(jc->life)) {
        conn = NULL;
        jc->conn = NULL;
        jc->req  = NULL;   /* names whatever request the slot serves now */
    }

    /* Restore per-request context so C functions called during resume
     * (e.g., another http.async.get) can find the active connection and
     * request (a re-yield's new cont captures js->active_req). Mirrors the
     * Lua path. */
    js->active_conn = conn;
    js->active_req  = jc->req;
    /* A continuation made before this resume (an un-awaited op a finished
     * synchronous handler left behind) is not part of this run: chained to
     * it, the stale one was handed this run's handler promise and could
     * complete it on its own connection. */
    js->last_async_cont = NULL;

    /* The whole entry is active - its life, timer and teardown hook - before
     * any app code runs: settling the promise below can run some (a `then`
     * getter on Object.prototype, read from an object result), and an op it
     * made with the connection active but no life suspended the connection
     * unaccounted for (audit 8 H1). A re-await inside the handler carries
     * them onto every continuation it makes. */
    js->active_on_complete     = jc->on_complete;
    js->active_on_complete_ctx = jc->on_complete_ctx;
    js->active_life            = jc->life;
    js->active_timer           = jc->timer_ctx;
    js->active_cli_main        = jc->cli_main;

    /* Settle the inner promise with the driver result.
     *
     * A push_result may signal a driver-side error two ways:
     *   1. by THROWING - returning JS_EXCEPTION with a pending exception on ctx
     *      (compute / db / gpu async: `return JS_ThrowInternalError(...)`), or
     *   2. by RETURNING an ordinary value that encodes the error
     *      (worker async resolves with `{ error: msg }`; http/tui resolve with
     *      undefined/null on their own terms).
     * Case 2 is the callback's own contract and must reach `resolve` unchanged.
     * Case 1 must reach `reject`: routing a JS_EXCEPTION through `resolve` FULFILLS
     * the promise with undefined and silently swallows the error (#319). Detect
     * the thrown case by the returned value and reject with the real exception so
     * `await` throws - catchable by the handler, else its promise rejects and the
     * REJECTED branch below writes a 500. Exactly one of resolve/reject fires. */
    /* The run waited holding a transaction (rolled back then): fail it
     * without continuing the handler (hl_js_run_yield_check). */
    int aborted = jc->link.once && jc->link.once->txn_held;

    if (aborted) {
        /* resolve / reject are freed below, uncalled */
    } else if (driver && jc->push_result) {
        JSValue result = jc->push_result(ctx, driver);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(ctx); /* retrieve + clear pending */
            JSValue ret = JS_Call(ctx, jc->reject, JS_UNDEFINED, 1, &exc);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, exc);
        } else {
            JSValue ret = JS_Call(ctx, jc->resolve, JS_UNDEFINED, 1, &result);
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, result);
    } else {
        JSValue ret = JS_Call(ctx, jc->resolve, JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, ret);
    }

    /* Free resolve/reject - no longer needed */
    JS_FreeValue(ctx, jc->resolve);
    JS_FreeValue(ctx, jc->reject);
    jc->resolve = JS_UNDEFINED;
    jc->reject = JS_UNDEFINED;

    /* Drain microtasks - this continues the handler past the await */
    if (!aborted)
        hl_js_run_jobs(js);
    js->active_cli_main = 0;

    js->active_on_complete     = NULL;
    js->active_on_complete_ctx = NULL;
    js->active_life            = NULL;
    js->active_timer           = NULL;

    /* The op's own hold on the connection ends with its resume (a park made
     * during the drain above was refused while it lasted - see the gate). */
    hl_js_cont_unattach(jc);

    /* Check outer handler promise state (per-continuation ref) */
    JSPromiseStateEnum state = JS_PromiseState(ctx, jc->link.handler_promise);
    int wired = !JS_IsUndefined(jc->link.handler_promise);

    /* Over the instruction budget, in this resume or an earlier one of the
     * run: the handler's promise will never settle, so finish the run as a
     * failure now (500, timer rescheduled, ws teardown) rather than leave it
     * suspended for good. */
    int tripped = hl_js_run_tripped(js, &jc->link);
    int forced = (tripped || aborted) && state == JS_PROMISE_PENDING && wired;
    if (forced)
        state = JS_PROMISE_REJECTED;
    if (js->budget_tripped && !conn && !jc->timer_ctx && !wired && jc->cli_main)
        hl_js_cli_main_trip(js, hl_js_trip_reason(js));
    /* The handler waits again (not a settled one deferred to its holder,
     * below): checked whether or not this resume started a new op - one made
     * before a BEGIN it ran now (`await pa; BEGIN; ...; await pb`) waits
     * across it all the same (audit 8 M7). */
    int waits_again = state == JS_PROMISE_PENDING && wired;

    /* The handler is done, but an op of its run still has the connection
     * suspended (`hull.sleep(5000)` left running before `res.json`). Sending
     * now forced the connection to SENDING over that live suspension; Keel
     * then released or reused the slot, and the op's completion later drove
     * whatever the slot had become (audit 6 H3). The op completes the run
     * instead and sends the response when it resumes - as the synchronous
     * path does, where Keel waits for the suspension. */
    if ((state == JS_PROMISE_FULFILLED || state == JS_PROMISE_REJECTED) &&
        conn && hl_js_run_defer_to_holder(js, &jc->link, jc->life))
        state = JS_PROMISE_PENDING;

    /* Another continuation of this run already completed the handler (a
     * Promise.race, or an early rejection of a Promise.all): nothing left
     * to finish here - running the completion again rescheduled a timer
     * twice or tore a ws conn down twice. */
    if ((state == JS_PROMISE_FULFILLED || state == JS_PROMISE_REJECTED) &&
        !hl_js_run_claim(&jc->link)) {
        JS_FreeValue(ctx, jc->link.handler_promise);
        jc->link.handler_promise = JS_UNDEFINED;
        jc->conn = NULL;
        jc->timer_ctx = NULL;
        jc->on_complete = NULL;
        js->active_conn = NULL;
        js->active_req  = NULL;
        js->last_async_cont = NULL;   /* an un-awaited op belongs to no run */
        hl_db_registry_guard_stale_txns(js->base.db_registry);   /* audit 6 M1 */
        return;
    }

    /* The run's hold op, completed last (below) when this resume ends it. */
    HlAsyncCtx *release = NULL;
    int release_outcome = 0;

    if (state == JS_PROMISE_FULFILLED) {
        /* Handler completed - clean up */
        int cancelled = 0;
        if (jc->timer_ctx) {
            JSValue result = JS_PromiseResult(ctx, jc->link.handler_promise);
            if (JS_IsBool(result) && JS_ToBool(ctx, result) == 0)
                cancelled = 1;
            JS_FreeValue(ctx, result);
        }

        JS_FreeValue(ctx, jc->link.handler_promise);
        jc->link.handler_promise = JS_UNDEFINED;
        jc->conn = NULL;

        js->async_pending = 0;
        js->active_conn = NULL;
        js->active_req  = NULL;

        /* Handler that awaited has now completed - its request objects (res,
         * an SSE stream) are done, and run any deferred-teardown hook (e.g.
         * ws on_close conn teardown). */
        hl_req_life_kill(jc->life);
        if (jc->on_complete) {
            jc->on_complete(js, jc->on_complete_ctx);
            jc->on_complete = NULL;
        }

#ifdef HL_ENABLE_HTTP_SERVER
        /* Finalize + send the resumed handler's response entirely behind the
         * HTTP-feature seam, so this base-runtime object holds NO Keel refs
         * (a compute app composes no HTTP and must link zero Keel). The seam
         * ends a streamed body and transitions the conn to SENDING (required on
         * the poll backend). Mirrors the Lua path (runtime/lua/async.c). An
         * op that ran detached sends through the run's hold op instead. */
        if (conn && was_holder && !hl_js_conn_held_elsewhere(js, conn))
            hl_js_http_resume_send(js, conn, jc->req);
#endif
        if (conn && !was_holder && jc->link.once) {
            release = (HlAsyncCtx *)jc->link.once->hold;
            release_outcome = HL_JS_HOLD_SEND;
        }

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
        /* Handler error - extract message, write 500. A tripped run cannot
         * run a toString, and its promise may not even be rejected. */
        const char *msg = NULL;
        if (!tripped && !aborted) {
            /* The rejection's toString is app code, run after this resume's
             * drain: with no request active (the life, timer and app.main
             * flag are already cleared), and its jobs drained right after -
             * left queued they ran in the next entry's drain, with that
             * entry's request active (audit 9 M1). The request is over: its
             * res is closed to them. */
            js->active_conn = NULL;
            js->active_req  = NULL;
            hl_req_life_kill(jc->life);
            JSValue result = JS_PromiseResult(ctx, jc->link.handler_promise);
            msg = JS_ToCString(ctx, result);
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, JS_GetException(ctx));
            hl_js_run_jobs(js);
            js->last_async_cont = NULL;
        }
        const char *shown = msg ? msg : tripped
            ? hl_js_trip_reason(js)
            : aborted ? "waited holding a database transaction" : "(unknown)";
        if (conn)
            log_error("[hull:c] async js handler error: %s", shown);
        else if (jc->timer_ctx)
            log_error("[hull:timer] error: %s", shown);
        else if (!jc->cli_main)   /* app.main reports its own rejection */
            log_error("[hull:js] async callback error: %s", shown);
        if (msg) JS_FreeCString(ctx, msg);
        /* app.main failed without settling its promise: end the CLI. */
        if (forced && jc->cli_main)
            hl_js_cli_main_trip(js, shown);

        JS_FreeValue(ctx, jc->link.handler_promise);
        jc->link.handler_promise = JS_UNDEFINED;
        jc->conn = NULL;

        js->async_pending = 0;
        js->active_conn = NULL;
        js->active_req  = NULL;

        /* Run any deferred-teardown hook (handler rejected after awaiting). */
        hl_req_life_kill(jc->life);
        if (jc->on_complete) {
            jc->on_complete(js, jc->on_complete_ctx);
            jc->on_complete = NULL;
        }

#ifdef HL_ENABLE_HTTP_SERVER
        if (conn && was_holder && !hl_js_conn_held_elsewhere(js, conn))
            hl_js_http_resume_error(js, conn, jc->req);  /* 500 + send, behind the seam */
#endif
        if (conn && !was_holder && jc->link.once) {
            release = (HlAsyncCtx *)jc->link.once->hold;
            release_outcome = HL_JS_HOLD_ERROR;
        }

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
        /* PENDING - handler re-yielded (another async op in flight). The
         * new continuations - of either type, HlJsAsyncCont or HlJsMpCont -
         * join THIS run, so the run still completes exactly once across old
         * and new continuations. */
        HlJsRunOnce *run = jc->link.once;
        if (js->last_async_cont) {
            HlAsyncCont *nc = (HlAsyncCont *)js->last_async_cont;
            hl_js_run_wire(hl_js_cont_link(nc), ctx, jc->link.handler_promise,
                           jc->link.once);
            if (!run) run = hl_js_cont_link(nc)->once;
            js->last_async_cont = NULL;
        }
        /* A run an entry waits on (a request, a timer, a ws or ws-client
         * callback, app.main) waits again: not holding a transaction - on a
         * new op, an earlier one, or a promise Hull does not drive. */
        if (waits_again)
            hl_js_run_yield_check(js, run);
        /* A request still waiting must keep its connection suspended until
         * the run completes: Keel otherwise sends the response as it stands
         * when this resume returns (an empty 200), while the handler goes on
         * to write into a connection that serves another client by then. */
        if (conn && wired && jc->life && hl_req_life_live(jc->life)) {
            if (!jc->life->held && !was_holder) {
                /* Nothing suspends it, and this resume is not inside Keel's
                 * completion of it: no way left to answer it. */
                hl_req_life_kill(jc->life);
                if (run) run->done = 1;
            } else if (!jc->life->held) {
                /* Nothing suspends it now: this op had it, and the handler
                 * started no op that took it over. */
                if (jc->life->attached > 0) {
                    if (hl_js_hold_arm(js, conn, jc->req, jc->life, run) != 0) {
                        log_error("[hull:c] async js handler: cannot keep the "
                                  "request waiting; it is answered 500");
                        hl_req_life_kill(jc->life);
                        if (run) run->done = 1;
#ifdef HL_ENABLE_HTTP_SERVER
                        if (!hl_js_conn_held_elsewhere(js, conn))
                            hl_js_http_resume_error(js, conn, jc->req);
#endif
                    }
                } else {
                    log_warn("[hull:c] handler awaits a promise Hull does not "
                             "drive; the request ends now and its res is closed");
                    hl_req_life_kill(jc->life);
                    if (run) run->done = 1;
                }
            } else if (run && run->hold && jc->life->attached == 0) {
                /* Only the hold keeps it: no op of the run is left. */
                log_warn("[hull:c] handler awaits a promise Hull does not "
                         "drive; the request ends now and its res is closed");
                hl_req_life_kill(jc->life);
                run->done = 1;
                release = (HlAsyncCtx *)run->hold;
            }
        }
        JS_FreeValue(ctx, jc->link.handler_promise);
        jc->link.handler_promise = JS_UNDEFINED;
        jc->conn = NULL;
        /* Waiting again: the next run (another request, middleware) must
         * not find this request as the active one. */
        js->active_conn = NULL;
        js->active_req  = NULL;
    }
    /* An un-awaited op made by a handler that settled belongs to no run. */
    js->last_async_cont = NULL;
    /* The run is over or parked: roll back a transaction left open (audit 6
     * M1). Only now, after the re-wait's hl_js_run_yield_check: run before
     * it, the guard rolled the transaction back unseen and the handler
     * resumed later without it (audit 6 M2). */
    hl_db_registry_guard_stale_txns(js->base.db_registry);
    /* Last: Keel sends the response, and may serve the connection's next
     * request from inside this call. */
    hl_js_hold_release(js, release, release_outcome);
}

/*
 * Cancel the JS handler - free promise refs without invoking.
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
    if (!JS_IsUndefined(jc->link.handler_promise)) {
        JS_FreeValue(ctx, jc->link.handler_promise);
        jc->link.handler_promise = JS_UNDEFINED;
    }
    /* Its connection is gone (and its pool slot may be reused): nothing
     * may still name it as the active one. */
    if (js->active_conn == jc->conn) {
        js->active_conn = NULL;
        js->active_req  = NULL;
    }
    /* ...and the snapshot its 500 would have kept (audit 12). */
    hl_res_base_forget_conn(&js->res_bases, jc->conn);
    jc->conn = NULL;
    hl_js_cont_unattach(jc);
    hl_js_cont_unhold(jc);
    /* The connection is gone: so is the request every `res` / stream object
     * of this handler points into. */
    hl_req_life_kill(jc->life);
}

/*
 * Destroy the cont struct. Does NOT free the promise refs - that's
 * managed by the resume/cancel functions above.
 */
static void hl_js_async_destroy(HlAsyncCont *self)
{
    HlJsAsyncCont *jc = (HlJsAsyncCont *)self;
    /* resume/cancel already resolved-or-freed and set these to UNDEFINED (so this
     * is a no-op on those paths). When destroy runs WITHOUT a prior resume/cancel
     * (e.g. an immediate scheduling-failure teardown, or a submit-failure cleanup),
     * it is the sole owner of the resolve/reject capability values - free them here
     * so they never leak. JS_FreeValue is a no-op on UNDEFINED. */
    if (jc->js && jc->js->ctx) {
        JS_FreeValue(jc->js->ctx, jc->resolve);
        JS_FreeValue(jc->js->ctx, jc->reject);
    }
    /* Never leave dispatch a pointer to a freed continuation: a failure path
     * that destroys a just-registered cont left last_async_cont dangling, and
     * dispatch then attached the handler promise to freed memory. */
    if (jc->js && jc->js->last_async_cont == jc)
        jc->js->last_async_cont = jc->link.unwired_prev   /* keep the run's chain */
            ? (void *)((char *)jc->link.unwired_prev - offsetof(HlJsContHead, link))
            : NULL;
    hl_js_run_unlink(&jc->link, jc->js ? jc->js->ctx : NULL);
    hl_js_cont_unattach(jc);   /* a never-armed op held nothing */
    hl_js_cont_unhold(jc);
    hl_req_life_release(jc->life);
    hl_alloc_free(jc->alloc, jc, sizeof(HlJsAsyncCont));
}

/*
 * Create a JS async continuation. Takes ownership of resolve/reject
 * JSValues (caller must not free them).
 * push_result: called on resume to convert driver result to JSValue.
 *              NULL for sleep (no result to push).
 */
/* set_handler_promise vtable slot for HlJsAsyncCont - accessed via
 * the public hl_js_async_cont_set_handler_promise dispatcher (which
 * just calls cont->set_handler_promise). Each JS cont type defines
 * its own setter; the dispatcher no longer cares about the concrete
 * cont type. */
static void hl_js_async_cont_set_handler_promise_impl(HlAsyncCont *self,
                                                         void *ctx_v,
                                                         void *promise_v)
{
    hl_js_run_wire(hl_js_cont_link(self), (JSContext *)ctx_v,
                   *(JSValue *)promise_v, NULL);
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
    jc->req             = js->active_req;
    jc->timer_ctx       = js->active_timer;  /* inherit timer ctx if in timer callback */
    jc->on_complete     = js->active_on_complete;     /* deferred-teardown hook */
    jc->on_complete_ctx = js->active_on_complete_ctx;
    jc->life            = js->active_life;
    jc->cli_main        = js->active_cli_main;
    hl_req_life_retain(jc->life);
    /* An op belongs to a request only while the request's live life is the
     * active one. App code can run while a connection is active and its life
     * is not - an Object.prototype `then` getter that a resume's resolve
     * reads, an inherited setter - and an op made there suspended the
     * connection uncounted in life->held / attached: the response then went
     * out over that live suspension, and the op's completion drove whatever
     * the slot served next (audit 8 H1). Such an op runs detached. */
    if (!jc->life || !hl_req_life_live(jc->life)) {
        jc->conn = NULL;
        jc->req  = NULL;
    }
    /* An op of a request counts until its resume is over: a multipart park
     * in that time is refused (mod_request.c). The first one suspends the
     * connection (hl_js_cont_suspend_conn); one made while another has it
     * suspended runs detached and still belongs to the request's run. */
    jc->attached        = 0;
    jc->holds           = 0;
    if (jc->conn) {
        jc->attached = 1;
        jc->life->attached++;
        if (!jc->life->held) {
            jc->holds = 1;
            jc->life->held = 1;
        }
    }
    /* Chained behind an earlier, not yet wired continuation of the same
     * run (a parallel await, of either type), and made the last one so
     * dispatch / resume can wire the handler promise. */
    hl_js_run_push(js, &jc->base);

    return &jc->base;
}

/*
 * Set the outer handler promise on a continuation.
 *
 * Called by hl_js_dispatch after detecting a PENDING handler return,
 * since the handler promise only exists after JS_Call returns. The
 * cont may be any JS cont type (HlJsAsyncCont, HlJsMpCont, …) - each
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
 * hull.sleep(ms) - return a Promise that resolves after `ms` milliseconds.
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
    if (hl_js_async_gate(ctx, js, "hull.sleep()") != 0)
        return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, "hull.sleep()")) return JS_EXCEPTION;

    KlHttpServer *server = js->server;

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

    /* Create JS continuation - takes ownership of resolve/reject.
     * No push_result - sleep has no return value. */
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

    if (hl_js_cont_suspend_conn(cont)) {
        /* Attached mode: use KlAsyncOp deadline via kl_async_suspend */
        actx->op.deadline_ms = hl_async_backend()->monotonic_ms() + (uint64_t)ms;
        actx->op.on_deadline = hl_async_on_deadline_sleep;
        actx->detached = 0;

        if (hl_js_op_suspend(js, (HlSuspendOp *)&actx->op) < 0) {
            /* Never armed: destroy frees resolve / reject. cancel() would also
             * end the still-running handler's request life. */
            actx->cont->destroy(actx->cont);
            hl_async_ctx_free(actx);
            JS_FreeValue(ctx, promise);
            return JS_ThrowInternalError(ctx,
                "hull.sleep(): failed to suspend connection");
        }
    } else {
        /* Detached mode: schedule via the async backend vtable. The
         * underlying loop is the same one KlHttpServer drives (wrapped in
         * serve.c::wire_caps), so the timer fires alongside HTTP work. */
        actx->detached = 1;

        const HlAsyncBackend *be = hl_async_backend();
        uint64_t tid = be->timer_add(js->base.async_ctx, (uint64_t)ms,
                                      hl_detached_timer_fire, actx);
        if (tid == 0) {
            /* Never armed: destroy frees resolve / reject. cancel() would also
             * end the still-running handler's request life. */
            actx->cont->destroy(actx->cont);
            hl_async_ctx_free(actx);
            JS_FreeValue(ctx, promise);
            return JS_ThrowInternalError(ctx,
                "hull.sleep(): failed to add timer");
        }
    }

    return promise;
}

/* ── hull:_task - detached tasks ─────────────────────────────────── */

/*
 * _task.spawn(fn) - run fn later, on a loop turn of its own, as a run that
 * belongs to nothing: no request, timer, ws callback or app.main is active
 * while it runs, so every op it makes runs detached and nothing it does
 * reaches its spawner's connection, response or completion. The JS twin of
 * Lua's hull._spawn, except that the body never starts inside the
 * spawner's entry: it is queued on a zero-delay loop timer, so it runs
 * after that entry - and the response a synchronous handler produced - is
 * over.
 *
 * The task is an entry like any other: its own instruction budget, a
 * stale transaction rolled back before and after it, its queued jobs
 * drained, an async body's continuations wired into one run (whose wait is
 * checked for an open transaction). A throw or a rejection is logged, never
 * raised into the spawner, which has moved on.
 *
 * Stdlib-only (an underscore module): auth-flows defers its mail with it,
 * so that work runs after the response in every path.
 */

typedef struct HlJsTask {
    struct HlJsTask *next;
    struct HlJsTask *prev;
    HlJS            *js;
    JSValue          fn;
    uint64_t         timer_id;
} HlJsTask;

static void js_task_unlink(HlJsTask *t)
{
    if (t->prev) t->prev->next = t->next;
    else         t->js->tasks = t->next;
    if (t->next) t->next->prev = t->prev;
    t->next = t->prev = NULL;
}

static void js_task_log(HlJS *js, JSValueConst err)
{
    const char *msg = js->budget_tripped ? NULL : JS_ToCString(js->ctx, err);
    log_error("[hull:js] spawned task error: %s",
              msg ? msg : js->budget_tripped ? hl_js_trip_reason(js)
                                             : "(unknown)");
    if (msg) JS_FreeCString(js->ctx, msg);
    /* A toString can throw: leave nothing pending for the next run. */
    JS_FreeValue(js->ctx, JS_GetException(js->ctx));
}

static void js_task_fire(void *user)
{
    HlJsTask *t = (HlJsTask *)user;
    HlJS *js = t->js;
    JSContext *ctx = js->ctx;
    JSValue fn = t->fn;
    js_task_unlink(t);
    hl_alloc_free(js->base.alloc, t, sizeof *t);

    /* What this clobbers, put back after (audit 9 L1). From the event loop
     * nothing is active. But `hull test` pumps the loop while an async case
     * is parked on a promise, so the task fires inside the case's run: it
     * re-armed the case's budget (and a task that tripped failed the case
     * as over the limit) and cleared the case's active state. */
    KlHttpConn   *save_conn        = js->active_conn;
    KlHttpRequest *save_req        = js->active_req;
    void         *save_timer       = js->active_timer;
    struct HlReqLife *save_life    = js->active_life;
    void        (*save_on_complete)(struct HlJS *, void *) = js->active_on_complete;
    void         *save_on_complete_ctx = js->active_on_complete_ctx;
    int           save_cli_main    = js->active_cli_main;
    void         *save_last_cont   = js->last_async_cont;
    int           save_pending     = js->async_pending;
    int64_t       save_count       = js->instruction_count;
    int           save_tripped     = js->budget_tripped;
    int           save_timed_out   = js->budget_timed_out;
    /* The interrupted run's wall-clock deadline: the arm below sets the
     * task's own (cap/run_watchdog.h). */
    uint64_t      save_deadline    = hl_run_watch_save(&js->run_watch);
#ifdef HL_ENABLE_DB
    /* The arm below rebinds this thread's SQL budget to this run. */
    HlDbBudgetBinding save_budget  = hl_db_budget_current();
#endif

    /* Nothing of whatever ran last is active (an op made now captures all
     * of it), and the run has a budget of its own. */
    js->active_conn            = NULL;
    js->active_req             = NULL;
    js->active_timer           = NULL;
    js->active_life            = NULL;
    js->active_on_complete     = NULL;
    js->active_on_complete_ctx = NULL;
    js->active_cli_main        = 0;
    js->last_async_cont        = NULL;
    js->async_pending          = 0;
    hl_js_budget_arm(js);
    hl_db_registry_guard_stale_txns(js->base.db_registry);   /* audit 6 M1 */

    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        js_task_log(js, exc);
        JS_FreeValue(ctx, exc);
    } else {
        /* An async body is a run: its continuations are wired to its
         * promise, whose rejection after a wait the resume logs. */
        int st;
        (void)hl_js_entry_park(js, ret, &st);
        if (st == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, ret);
            js_task_log(js, reason);
            JS_FreeValue(ctx, reason);
        }
    }
    JS_FreeValue(ctx, ret);
    hl_js_run_jobs(js);
    js->last_async_cont = NULL;   /* an un-awaited op belongs to no run */
    hl_db_registry_guard_stale_txns(js->base.db_registry);   /* audit 6 M1 */

    /* Back to whatever run the task interrupted (see above). Its own trip
     * left nothing pending: the logs above drained the exception. */
    if (js->budget_tripped)
        JS_FreeValue(ctx, JS_GetException(ctx));
    js->active_conn            = save_conn;
    js->active_req             = save_req;
    js->active_timer           = save_timer;
    js->active_life            = save_life;
    js->active_on_complete     = save_on_complete;
    js->active_on_complete_ctx = save_on_complete_ctx;
    js->active_cli_main        = save_cli_main;
    js->last_async_cont        = save_last_cont;
    js->async_pending          = save_pending;
    js->instruction_count      = save_count;
    js->budget_tripped         = save_tripped;
    js->budget_timed_out       = save_timed_out;
    hl_run_watch_restore(&js->run_watch, save_deadline);
#ifdef HL_ENABLE_DB
    hl_db_budget_restore(save_budget);
#endif
}

static JSValue js_task_spawn(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "_task.spawn requires (fn)");
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.async_ctx)
        return JS_ThrowInternalError(ctx,
            "_task.spawn() requires an active event loop");

    HlJsTask *t = hl_alloc_malloc(js->base.alloc, sizeof *t);
    if (!t)
        return JS_ThrowInternalError(ctx, "_task.spawn(): out of memory");
    t->js   = js;
    t->fn   = JS_DupValue(ctx, argv[0]);
    t->prev = NULL;
    t->next = (HlJsTask *)js->tasks;
    if (t->next) t->next->prev = t;
    js->tasks = t;
    t->timer_id = hl_async_backend()->timer_add(js->base.async_ctx, 0,
                                                js_task_fire, t);
    if (t->timer_id == 0) {
        js_task_unlink(t);
        JS_FreeValue(ctx, t->fn);
        hl_alloc_free(js->base.alloc, t, sizeof *t);
        return JS_ThrowInternalError(ctx, "_task.spawn(): failed to add timer");
    }
    return JS_UNDEFINED;
}

void hl_js_tasks_free(HlJS *js)
{
    if (!js) return;
    const HlAsyncBackend *be = hl_async_backend();
    while (js->tasks) {
        HlJsTask *t = (HlJsTask *)js->tasks;
        js_task_unlink(t);
        if (js->base.async_ctx)
            be->timer_cancel(js->base.async_ctx, t->timer_id);
        if (js->ctx)
            JS_FreeValue(js->ctx, t->fn);
        hl_alloc_free(js->base.alloc, t, sizeof *t);
    }
}

static int js_task_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue task = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, task, "spawn",
                      JS_NewCFunction(ctx, js_task_spawn, "spawn", 1));
    JS_SetModuleExport(ctx, m, "_task", task);
    return 0;
}

int hl_js_init_task_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:_task", js_task_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "_task");
    return 0;
}

/* ── Global registration ─────────────────────────────────────────── */

void hl_js_add_hull_global(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue hull = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, hull, "sleep",
        JS_NewCFunction(ctx, js_hull_sleep, "sleep", 1));

    JS_SetPropertyStr(ctx, global, "hull", hull);

    /* hull.map(items, fn, { limit }) - bounded fan-out. Promises already give
     * JS tasks, and Promise.allSettled a gather that waits for everything
     * (Promise.all rejects at the first failure and leaves the rest running);
     * this is the one piece missing, and the one fleet code needs: at most
     * `limit` items in flight (default 16), results in input order, and every
     * item finishes before a failure is reported - the first (by index) as
     * the message, all of them on `.errors`. The Lua twin is hull._async;
     * docs/task_join_design.md. */
    static const char map_src[] =
"(function(hull) {\n"
"  const DEFAULT_LIMIT = 16;\n"
"  hull.map = async function map(items, fn, opts) {\n"
"    if (!Array.isArray(items)) throw new TypeError('hull.map: expected an array');\n"
"    if (typeof fn !== 'function') throw new TypeError('hull.map: expected a function');\n"
"    if (opts != null && typeof opts !== 'object')\n"
"      throw new TypeError('hull.map: opts must be an object');\n"
"    const limit = opts != null && opts.limit != null ? opts.limit : DEFAULT_LIMIT;\n"
"    if (limit !== Infinity && !(Number.isInteger(limit) && limit >= 1))\n"
"      throw new RangeError('hull.map: limit must be a positive integer (or Infinity)');\n"
"    const n = items.length, results = new Array(n);\n"
"    let next = 0, errors = null;\n"
"    async function worker() {\n"
"      while (next < n) {\n"
"        const i = next++;\n"
"        try { results[i] = await fn(items[i], i); }\n"
"        catch (e) { if (!errors) errors = []; errors[i] = e; }\n"
"      }\n"
"    }\n"
"    const workers = [];\n"
"    for (let k = 0; k < Math.min(limit, n); k++) workers.push(worker());\n"
"    await Promise.all(workers);\n"
"    if (errors) {\n"
"      let first = -1;\n"
"      for (let i = 0; i < errors.length; i++) if (i in errors) { first = i; break; }\n"
"      const e0 = errors[first];\n"
"      let msg;\n"
"      try { msg = e0 instanceof Error ? String(e0.message) : String(e0); }\n"
"      catch (_) { msg = '(an error)'; }\n"
"      const err = new Error(msg);\n"
"      err.errors = errors;\n"
"      throw err;\n"
"    }\n"
"    return results;\n"
"  };\n"
"})(globalThis.hull);\n";
    JSValue r = JS_Eval(ctx, map_src, sizeof(map_src) - 1, "<hull-map-init>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        /* Out of memory at init. Clear the exception rather than leave it
         * pending for whatever the context runs next; hull.map is missing. */
        JSValue e = JS_GetException(ctx);
        log_error("[hull:js] hull.map could not be installed");
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);

    JS_FreeValue(ctx, global);
}
