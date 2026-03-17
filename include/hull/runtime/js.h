/*
 * runtime/js.h — QuickJS runtime for Hull
 *
 * Manages QuickJS VM lifecycle: init, sandbox, allocator integration,
 * interrupt handler, module loader, and request dispatch.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_JS_H
#define HL_RUNTIME_JS_H

#include <stddef.h>
#include <stdint.h>
#include "hull/limits.h"
#include "hull/runtime.h"
#include "hull/cap/types.h"

/* Forward declarations */
typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef struct KlRequest KlRequest;
typedef struct KlResponse KlResponse;
typedef struct KlRouter KlRouter;
typedef struct KlServer KlServer;
typedef struct KlConn KlConn;
typedef struct KlAsyncOp KlAsyncOp;
typedef struct KlThreadPool KlThreadPool;
typedef struct SHArena SHArena;
typedef struct HlAsyncCtx HlAsyncCtx;
typedef struct HlAllocator HlAllocator;

/* ── Timer context ─────────────────────────────────────────────────── */

typedef struct HlJSTimer {
    struct HlJS *js;
    int         handler_id;    /* index into __hull_timers */
    int64_t     interval_ms;   /* repeat interval (or recomputed for daily) */
    int64_t     timer_id;      /* kl_timer_add return, for cancellation */
    int         daily;         /* 1 = daily timer */
    int         localtime;     /* 1 = local time, 0 = UTC */
    int         hour;          /* for daily: target hour */
    int         minute;        /* for daily: target minute */
    int         in_flight;     /* 1 = callback currently running (async) */
} HlJSTimer;

/* ── Configuration ──────────────────────────────────────────────────── */

typedef struct {
    size_t  max_heap_bytes;       /* JS heap limit (default: 64 MB) */
    size_t  max_stack_bytes;      /* JS stack limit (default: 1 MB) */
    int64_t max_instructions;     /* 0 = unlimited */
    size_t  gc_threshold;         /* bytes before cycle GC (default: 256 KB) */
} HlJSConfig;

/* Sensible defaults */
#define HL_JS_CONFIG_DEFAULT {                   \
    .max_heap_bytes   = HL_JS_DEFAULT_HEAP,      \
    .max_stack_bytes  = HL_JS_DEFAULT_STACK,      \
    .max_instructions = HL_DEFAULT_INSTRUCTIONS,  \
    .gc_threshold     = HL_JS_GC_THRESHOLD,       \
}

/* ── Runtime context ────────────────────────────────────────────────── */

typedef struct HlJS {
    HlRuntime       base;          /* vtable + shared capabilities */

    JSRuntime      *rt;
    JSContext      *ctx;

    /* Interrupt / gas metering */
    int64_t         instruction_count;
    int64_t         max_instructions;

    /* Module search paths */
    const char     *app_dir;         /* application root directory */
    size_t          app_dir_size;    /* allocation size for tracked free */

    /* Per-request scratch arena (reset between dispatches) */
    SHArena        *scratch;

    /* Per-runtime response class (avoids global statics) */
    uint32_t        response_class_id;
    int             response_class_registered;

    /* Tracked route allocations (freed in hl_js_free) */
    void          **routes;
    size_t          route_count;
    size_t          route_cap;

    /* Tracked timer allocations (freed in hl_js_free) */
    void          **timers;
    size_t          timer_count;
    size_t          timer_cap;

    /* Per-request async state (set during dispatch, cleared after) */
    KlServer       *server;          /* set once during wire_routes_server */
    KlConn         *active_conn;     /* current connection (per dispatch) */
    KlRequest      *active_req;      /* current request (for compression check) */
    int             async_pending;   /* 1 = handler returned pending Promise */
    void           *last_async_cont; /* last-created HlJsAsyncCont (for handler_promise wiring) */

    /* Timer callback context: if non-NULL, hl_js_async_cont_create
     * will wire timer_ctx on the new continuation. */
    void           *active_timer;    /* HlJSTimer* during timer callback */
} HlJS;

/* ── Vtable ────────────────────────────────────────────────────────── */

extern const HlRuntimeVtable hl_js_vtable;

/* ── Lifecycle ──────────────────────────────────────────────────────── */

/*
 * Initialize the QuickJS runtime with custom allocator routing all
 * allocations through the process malloc (future: KlAllocator).
 * Applies sandbox: removes eval, does not load std/os modules,
 * sets memory limit, sets interrupt handler, registers module loader.
 *
 * Returns 0 on success, -1 on error.
 */
int hl_js_init(HlJS *js, const HlJSConfig *cfg);

/*
 * Load and evaluate the application entry point (app.js).
 * This registers routes, middleware, config, etc.
 * Returns 0 on success, -1 on error.
 */
int hl_js_load_app(HlJS *js, const char *filename);

/*
 * Dispatch an HTTP request to the JS handler that matched the route.
 * Called from Hull's Keel middleware/handler bridge.
 *
 * `handler_id` is the route index registered during app loading.
 * Creates JS request/response objects, calls the handler, and
 * marshals the response back to KlResponse.
 *
 * Returns 0 on success, -1 on error.
 */
int hl_js_dispatch(HlJS *js, int handler_id,
                     KlRequest *req, KlResponse *res);

/*
 * Run pending microtasks / jobs (e.g. from app.spawn()).
 * Call between event loop iterations with a time budget.
 * Returns number of jobs executed.
 */
int hl_js_run_jobs(HlJS *js);

/*
 * Optional: run garbage collector between requests if memory pressure
 * is high. Safe to call at any time.
 */
void hl_js_gc(HlJS *js);

/*
 * Reset per-request state (instruction counter, etc.).
 * Call before each request dispatch.
 */
void hl_js_reset_request(HlJS *js);

/*
 * Destroy the QuickJS runtime and free all resources.
 */
void hl_js_free(HlJS *js);

/* ── Module registration ────────────────────────────────────────────── */

/*
 * Register all hull:* built-in modules (app, db, json, session, etc.).
 * Called internally by hl_js_init().
 */
int hl_js_register_modules(HlJS *js);

/*
 * Register embedded JS stdlib modules (from auto-generated registry).
 * Populates the embedded module lookup table used by the module loader.
 * Called internally by hl_js_init().
 */
int hl_js_register_stdlib(HlJS *js);

/* ── Error reporting ────────────────────────────────────────────────── */

/*
 * Print the current JS exception to stderr with stack trace.
 * Call after any JS_IsException() check.
 */
void hl_js_dump_error(HlJS *js);

/* ── Route wiring ──────────────────────────────────────────────────── */

/*
 * Per-route context: associates a Keel route with a JS handler.
 */
typedef struct {
    HlJS *js;
    int    handler_id;
} HlJSRoute;

/*
 * Keel handler bridge: dispatches a request to the JS handler.
 */
void hl_js_keel_handler(KlRequest *req, KlResponse *res, void *user_data);

/*
 * Wire JS routes from __hull_route_defs into a KlRouter.
 * Returns 0 on success, -1 on error.
 */
int hl_js_wire_routes(HlJS *js, KlRouter *router);

/*
 * Wire JS routes into a KlServer (with body reader factory).
 * `alloc_fn` allocates per-route context (pass NULL to use malloc).
 * Returns 0 on success, -1 on error.
 */
int hl_js_wire_routes_server(HlJS *js, KlServer *server,
                              void *(*alloc_fn)(size_t));

/*
 * Dispatch a middleware call to the JS handler.
 * Returns 0 (continue), positive (short-circuit), or -1 (error).
 */
int hl_js_dispatch_middleware(HlJS *js, int handler_id,
                              KlRequest *req, KlResponse *res);

/*
 * Keel middleware bridge: dispatches a request to the JS middleware.
 * Returns 0 (continue) or non-zero (short-circuit).
 */
int hl_js_keel_middleware(KlRequest *req, KlResponse *res, void *user_data);

/* ── Worker dispatch ────────────────────────────────────────────────── */

/* Init hook: called when creating a per-worker JS VM.
 * Use to register modules (e.g. db.*) into the worker environment. */
typedef int (*HlJsWorkerInitFn)(JSContext *ctx);

/* Register an init hook for worker JS VMs. Call before workers spawn. */
void hl_js_worker_register_init(HlJsWorkerInitFn fn);

/* Worker dispatch operation — runtime-specific, submitted to thread pool. */
typedef struct HlJsWorkerDispatchOp {
    HlAsyncCtx   *async_ctx;
    HlAllocator  *alloc;
    KlServer     *server;

    /* Input (deep-copied, owned) */
    char         *fn_source;        /* function source text from fn.toString() */
    size_t        fn_source_len;
    HlKV         *ctx_kvs;
    int           ctx_count;

    /* Output (set by worker thread) */
    int           result_kind;       /* 0=nil,1=bool,2=int,3=double,4=text,5=table */
    int64_t       result_int;
    double        result_double;
    int           result_bool;
    char         *result_str;        /* owned */
    size_t        result_str_len;
    HlKV         *result_kvs;       /* owned, for table result */
    int           result_count;

    int           error;
    char          error_msg[HL_WORKER_ERR_SIZE];
    int           cancelled;
} HlJsWorkerDispatchOp;

/* Submit a worker.dispatch operation to the thread pool.
 * Returns 0 on success, -1 on error. */
int hl_js_worker_dispatch_submit(KlThreadPool *pool,
                                   HlJsWorkerDispatchOp *op);

/* Free resources owned by a dispatch op (fn_source, kvs, result). */
void hl_js_worker_dispatch_op_free(HlJsWorkerDispatchOp *op);

/* Free dispatch op struct and all owned data (for use as free_driver). */
void hl_js_worker_dispatch_op_free_all(void *ptr);

/* KlAsyncOp on_cancel handler for worker.dispatch operations. */
void hl_js_worker_dispatch_cancel(KlAsyncOp *op, void *user_data);

/* Wire db.* module into worker JS VMs. Call during runtime init. */
void hl_js_worker_db_init(void);

#endif /* HL_RUNTIME_JS_H */
