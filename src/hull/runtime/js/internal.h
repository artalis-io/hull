/*
 * internal.h - private cross-file declarations for JS runtime
 *
 * Shared declarations for runtime.c / dispatch.c / routes.c / timers.c /
 * ws.c / sse.c after the runtime.c god-module was split. Each of those
 * files used to live in a single TU and freely called static helpers
 * from a sibling section; here we surface only the symbols that now
 * need to cross the new file boundaries.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_JS_INTERNAL_H
#define HL_RUNTIME_JS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "hull/runtime/js.h"
#include "hull/cap/types.h"   /* HlKV for worker dispatch op */
#include "hull/limits/core.h" /* HL_WORKER_ERR_SIZE */
#include "quickjs.h"
#include "hull/shared/async.h"   /* HlAsyncCont (the run links below) */

/* Forward declarations to keep internal.h small. */
typedef struct HlAsyncCtx     HlAsyncCtx;
typedef struct HlAllocator    HlAllocator;
typedef struct KlHttpServer       KlHttpServer;
typedef struct KlAsyncOp      KlAsyncOp;
typedef struct HlAsyncBackendPool HlAsyncBackendPool;

/* ── Internal op structs (moved from public js.h, roadmap item J) ── */

/* Per-timer context. Allocated by app.every/app.daily registration. */
typedef struct HlJSTimer {
    struct HlJS *js;
    int         handler_id;
    int64_t     interval_ms;
    int64_t     timer_id;
    int         daily;
    int         localtime;
    int         hour;
    int         minute;
    int         in_flight;
} HlJSTimer;

/* Worker dispatch operation - runtime-specific, submitted to thread pool. */
typedef struct HlJsWorkerDispatchOp {
    HlAsyncCtx   *async_ctx;
    HlAllocator  *alloc;
    KlHttpServer     *server;

    /* Input (deep-copied, owned) */
    char         *fn_source;
    size_t        fn_source_len;
    HlKV         *ctx_kvs;
    int           ctx_count;

    /* The app's own limits, applied to the worker VM (0 = none), and
     * whether it declared hull/db (the worker `db` global). */
    size_t        max_heap_bytes;
    size_t        max_stack_bytes;
    int64_t       max_instructions;
    int           with_db;

    /* Output (set by worker thread) */
    int           result_kind;
    int64_t       result_int;
    double        result_double;
    int           result_bool;
    char         *result_str;
    size_t        result_str_len;
    HlKV         *result_kvs;
    int           result_count;

    int           error;
    char          error_msg[HL_WORKER_ERR_SIZE];
    int           cancelled;
} HlJsWorkerDispatchOp;

int  hl_js_worker_dispatch_submit(HlAsyncBackendPool *pool,
                                  HlJsWorkerDispatchOp *op);
void hl_js_worker_dispatch_op_free(HlJsWorkerDispatchOp *op);
void hl_js_worker_dispatch_op_free_all(void *ptr);
void hl_js_worker_dispatch_cancel(KlAsyncOp *op, void *user_data);
#ifdef HL_ENABLE_DB
void hl_js_worker_db_init(void);
/* 1 while this pool thread's worker.dispatch is over its instruction budget
 * (its db bindings then throw the uncatchable interrupt). */
int  hl_js_worker_budget_tripped(void);
#endif

/* Forward declaration for Keel WS server connection (avoid pulling
 * <keel/websocket_server.h> into every TU). */
struct KlWsServerConn;

/* Forward struct types used across files */
typedef struct HlJSWsRoute HlJSWsRoute;
typedef struct HlJSSseRoute HlJSSseRoute;
struct HlJSWsRoute {
    HlJS *js;
    int   on_open_id;
    int   on_message_id;
    int   on_close_id;
    char  path[256];
};
struct HlJSSseRoute {
    HlJS *js;
    int   handler_id;
};

/* ── Promoted: defined in timers.c, used in routes.c (initial schedule
 * during wire_routes_server) and from within timers.c itself. */
int64_t hl_js_compute_daily_delay_ms(int hour, int minute, int use_local);
void hl_js_timer_trampoline(void *user_data);
int hl_js_track_timer(HlJS *js, void *timer);

/* ── Promoted: defined in ws.c, used in routes.c (kl_ws_server_config
 * wiring during wire_routes_server). */
void hl_js_ws_on_open(struct KlWsServerConn *ws_conn, void *user_data);
void hl_js_ws_on_message(struct KlWsServerConn *ws_conn, const char *data,
                         size_t len, int is_binary, void *user_data);
void hl_js_ws_on_close(struct KlWsServerConn *ws_conn, uint16_t code,
                       const char *reason, size_t reason_len,
                       void *user_data);

/* ── Promoted: defined in sse.c, used in routes.c (kl_http_server_route for
 * SSE endpoints during wire_routes_server). */
void hl_js_sse_handler(struct KlHttpRequest *req, struct KlHttpResponse *res,
                       void *user_data);

/* ── Promoted: defined in routes.c, used by sibling files when they
 * need to register additional dynamically-created routes. Currently
 * only routes.c uses them, but keep declarations here so future split
 * files can reuse without re-introducing duplicates. */
int hl_js_track_route(HlJS *js, void *route);
int hl_js_track_alloc(HlJS *js, void ***arr, size_t *count,
                      size_t *cap, void *ptr);

/* ── Module-declaration gate ─────────────────────────────────────────
 *
 * Called from each native module's QuickJS init callback to enforce
 * the resolved module set. Returns 0 if the import is admitted
 * (permissive when no set is wired, or the name is not in the
 * registry - e.g. private bridges like hull:_template). Returns -1
 * after throwing a ReferenceError when the module is registry-known
 * but absent from the app's declared modules.
 *
 *   canonical_name: registry-canonical form, e.g. "hull/db"
 *   runtime_name  : how the user wrote it, e.g. "hull:db" (used in
 *                   the thrown error message)
 *
 * Native modules ARE registered with QuickJS up front (otherwise
 * `import "hull:db"` would fail with "module not found" even when
 * declared), but their init callback - which populates exports -
 * doesn't run until first import. That's the hook this gate uses.
 */
int hl_js_check_module_declared(JSContext *ctx,
                                 const char *canonical_name,
                                 const char *runtime_name);

/* ── Defined in mod_request.c, called from bindings.c + modules.c. */
struct KlHttpBodyReader;
/* Register MultipartIter / MultipartPart / MultipartChunks classes
 * (once per VM, called from hl_js_register_modules). */
void hl_js_request_register(JSContext *ctx);
/* Install req.multipart() on the request object - no-op for non-
 * streaming routes (body_reader is not a multipart wrapper). */
/* ── One handler run's continuations ─────────────────────────────────
 *
 * A handler awaiting several Hull operations at once (Promise.all / race)
 * has one continuation per operation - standard ones (async.c) and
 * multipart ones (mod_request.c) alike. Only one may run the completion
 * (the response, a timer reschedule, a ws teardown), and whichever resumes
 * LAST must still hold the handler promise. Every JS continuation type
 * starts with HlJsContHead, so the chain links any of them. */
typedef struct {
    int refs;
    int done;
    /* The run went over its instruction budget. Its handler promise will
     * never settle (a tripped run settles nothing), so the continuation that
     * resumes next completes the run as failed instead of waiting. */
    int tripped;
    /* The run waited (suspended on an op, or parked on a multipart read)
     * while a registry connection was inside a transaction. The transaction
     * was rolled back at that point; the run is failed at its next resume
     * without being continued - continued, its remaining statements would
     * autocommit and its COMMIT "succeed" (audit 6 M2). */
    int txn_held;
    /* A request run waiting only on ops that run detached (the one op that
     * had the connection suspended has resumed): the hold op (HlAsyncCtx *)
     * that keeps the connection suspended until the run completes, or NULL.
     * Without it Keel sent the response as it stood - an empty 200 - and the
     * handler's later res.json wrote into a recycled connection slot. */
    void *hold;
} HlJsRunOnce;

typedef struct HlJsRunLink {
    JSValue             handler_promise;  /* outer handler promise */
    struct HlJsRunLink *unwired_prev;     /* earlier cont of this run, not yet wired */
    HlJsRunOnce        *once;             /* shared with the run's other conts */
} HlJsRunLink;

typedef struct {
    HlAsyncCont base;
    HlJsRunLink link;
} HlJsContHead;

/* The link of a JS continuation (every one js->last_async_cont can hold). */
static inline HlJsRunLink *hl_js_cont_link(HlAsyncCont *c)
{
    return c ? &((HlJsContHead *)c)->link : NULL;
}

/* async.c: give @p promise to @p last and every unwired continuation made
 * before it in the run, all sharing @p once (a new one when NULL). */
void hl_js_run_wire(HlJsRunLink *last, JSContext *ctx, JSValue promise,
                    HlJsRunOnce *once);
/* True once per run: the caller runs the handler's completion. */
int  hl_js_run_claim(HlJsRunLink *link);
/* Drop the link's references (destroy). */
void hl_js_run_unlink(HlJsRunLink *link, JSContext *ctx);
/* Chain a new continuation behind js->last_async_cont and make it the last. */
void hl_js_run_push(HlJS *js, HlAsyncCont *cont);
/* An entry point's handler returned pending with continuations made: give
 * them @p promise (js->last_async_cont and the run chain behind it), mark the
 * run tripped if the handler went over its budget, and clear
 * last_async_cont. Returns the run record with a reference held (release
 * with hl_js_run_drop), or NULL - for a caller that drains more of the run
 * afterwards and must mark a trip that happens there. */
HlJsRunOnce *hl_js_run_attach(HlJS *js, JSValue promise);
/* Drop a reference hl_js_run_attach returned, marking the run tripped first
 * when the runtime's budget is now tripped. */
void hl_js_run_drop(HlJS *js, HlJsRunOnce *run);

/* An entry point's handler returned @p ret (not an exception). Run the jobs
 * it queued with the entry still active - the caller keeps js->active_* (the
 * request, its life, the timer, the teardown hook) set - so its own code
 * after an `await` runs as part of it, and an op that code starts belongs to
 * it; left for a later drain they ran with no entry active (a detached op the
 * run never waited for: an early, empty response, then res.json into a
 * recycled connection slot) or inside another entry's. Then, when the
 * handler is still pending with continuations made, wire them all into one
 * run and check the wait (hl_js_run_yield_check) - after the drain, which may
 * open the transaction. Returns 1 when the run now waits on its
 * continuations, else 0; *state is the handler's promise state after the
 * drain (-1 when @p ret is not a promise, in which case nothing is drained).
 * Continuations made by a handler that settled are left in
 * js->last_async_cont for the caller (they belong to no run). */
int hl_js_entry_park(HlJS *js, JSValue ret, int *state);

/* The connection an op made by @p cont must suspend (attached mode), or NULL
 * when it runs detached: outside a request, or while another op of the same
 * request already has the connection suspended - then the op still belongs to
 * the request's run (it completes it if it resumes last). @p cont is an
 * HlJsAsyncCont (hl_js_async_cont_create). */
KlHttpConn *hl_js_cont_suspend_conn(HlAsyncCont *cont);

/* ── Detached tasks: hull:_task (async.c) ───────────────────────────── */

/* Register the stdlib-only hull:_task module (`_task.spawn(fn)`). */
int hl_js_init_task_module(JSContext *ctx, HlJS *js);

/* Cancel every spawned task that has not run yet and release its function.
 * Called by hl_js_free while the context is still alive. */
void hl_js_tasks_free(HlJS *js);

/* ── Instruction budget (runtime.c) ─────────────────────────────────── */

/* (hl_js_budget_arm, which re-arms it at each entry point, is in js.h.) */
/* Throw the instruction-limit interrupt as an uncatchable error: for a C
 * binding whose callback (a SQL UDF, a compute.stream sink) was interrupted
 * and would otherwise report it as an ordinary, catchable error. */
JSValue hl_js_budget_throw(JSContext *ctx);

/* Charge @p units to the run's instruction budget before a C binding does
 * work one call cannot otherwise be charged for (a hash over megabytes, a
 * PBKDF2 derivation): the interrupt handler only counts calls and backward
 * jumps, so a loop of them was not bounded in time. Trips the budget (sticky)
 * when that goes over it; then -1 with the uncatchable interrupt pending,
 * and the binding returns JS_EXCEPTION without doing the work. The Lua twin
 * is lua_hlcharge (runtime/lua/mod_crypto.c crypto_charge). */
int hl_js_budget_charge(JSContext *ctx, uint64_t units);

/* ── Async-op gate (async.c) ────────────────────────────────────────── */

/* Every Hull async op (hull.sleep, db.async, compute.async, gpu.async,
 * worker.dispatch, http.fetch, smtp.send, ...) calls this before it makes its
 * continuation. Refuses - throwing, returns -1 - when the op could not be
 * driven safely: inside middleware (synchronous: an attached op would suspend
 * the connection the rest of the chain still runs on), or while
 * req.multipart() is parked for more body on this request (the op's suspend
 * and the park each take over the connection's state). @p what names the op
 * in the error. */
int hl_js_async_gate(JSContext *ctx, HlJS *js, const char *what);

/* A resume whose handler has settled while an op of its run (one it did not
 * await) still has the request's connection suspended (life->held, and not
 * by the run's hold op): wire the continuations made in this resume
 * (js->last_async_cont) into the run, so the holder completes it - the
 * response is never sent over a live suspension. 1 = deferred (treat the run
 * as still pending), 0 = not. */
int hl_js_run_defer_to_holder(HlJS *js, HlJsRunLink *link, struct HlReqLife *life);

/* Defence in depth (audit 8 H1): 1 - logged - when an op still has @p conn
 * suspended in Keel at the point a resume would send its request's response.
 * The run's own ops are deferred to before that (hl_js_run_defer_to_holder,
 * the hold op), so this one is not accounted for: sending drove the
 * connection to SENDING over its live suspension, and its completion later
 * drove a recycled slot (audit 6 H3). The caller does not send; Keel sends
 * the response as it stands when that op completes. */
int hl_js_conn_held_elsewhere(HlJS *js, KlHttpConn *conn);

/* A run is about to wait (its handler returned / re-yielded pending with a
 * continuation, or a multipart read re-parks). The creation-time check
 * (hl_js_db_refuse_wait) cannot see a transaction opened AFTER the op was
 * started and before the await: `const p = http.fetch(..);
 * conn.exec("BEGIN"); await p`. If a registry connection is in a
 * transaction now, roll it back, log, and mark @p run (txn_held) so its next
 * resume fails it instead of continuing it. 1 = the run is failed. */
int hl_js_run_yield_check(HlJS *js, HlJsRunOnce *run);

/* Suspend js->active_conn for an attached op - hl_net_op_suspend, after the
 * gate's checks once more: the binding's argument conversions, between the
 * gate and here, can run app code (a getter) that parked a multipart read.
 * Returns -1 (nothing suspended) when it may not. */
struct HlSuspendOp;
int hl_js_op_suspend(HlJS *js, struct HlSuspendOp *op);

/* QuickJS calls the interrupt handler once per JS_INTERRUPT_COUNTER_INIT
 * (10000, quickjs.c) countdown steps - calls and backward jumps - not once
 * per instruction. Each call is charged that much, so max_instructions means
 * what its name says (counted one per call, the default 100M allowed ~10^12
 * steps: a `while (true) {}` held the event loop for hours). */
#define HL_JS_INTERRUPT_WEIGHT 10000

struct HlReqLife;
void hl_js_request_install_multipart(JSContext *ctx, JSValue req_obj,
                                      struct KlHttpBodyReader *body_reader,
                                      struct HlReqLife *life, KlHttpConn *conn,
                                      KlHttpRequest *req);

/* db.batch's synchronous-fn checks (mod_db.c), shared with the worker VM's
 * batch (worker_db.c). hl_js_fn_is_async: 1 when @p fn is an async (or
 * async generator) function. hl_js_is_thenable: 1 when @p v is a promise or
 * has a callable `then` (reading it can run a getter). Both 0 when not, -1
 * on an exception. */
int hl_js_fn_is_async(JSContext *ctx, JSValueConst fn);
int hl_js_is_thenable(JSContext *ctx, JSValueConst v);

/* Make the four code-compiling constructors unreachable from script. Deleting
 * the `Function` global is not enough: (() => 0).constructor is Function, and
 * the async / generator / async-generator prototypes reach their own
 * constructors the same way, each of which compiles a string into code. Each
 * prototype's `constructor` is replaced by a non-writable, non-configurable
 * stub with the same name that throws. Returns 0, or -1 if it could not. Used
 * by the main runtime and the worker VMs. */
int hl_js_poison_code_constructors(JSContext *ctx);

/* Reserve globalThis.__hull_manifest (mod_app.c): a non-configurable getter
 * onto HlJS.manifest, defined before any app code runs. 0 / -1. */
int hl_js_define_manifest_global(JSContext *ctx);

/* Free req->ctx (a middleware's req.ctx, or a test dispatch's JSON) and
 * clear it, and any ctx an earlier request on the same connection slot left
 * tracked (dispatch.c). Idempotent. */
void hl_js_req_ctx_free(HlJS *js, KlHttpRequest *req);

#endif /* HL_RUNTIME_JS_INTERNAL_H */
