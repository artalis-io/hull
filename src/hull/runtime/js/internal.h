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

/* Make the four code-compiling constructors unreachable from script. Deleting
 * the `Function` global is not enough: (() => 0).constructor is Function, and
 * the async / generator / async-generator prototypes reach their own
 * constructors the same way, each of which compiles a string into code. Each
 * prototype's `constructor` is replaced by a non-writable, non-configurable
 * stub with the same name that throws. Returns 0, or -1 if it could not. Used
 * by the main runtime and the worker VMs. */
int hl_js_poison_code_constructors(JSContext *ctx);

#endif /* HL_RUNTIME_JS_INTERNAL_H */
