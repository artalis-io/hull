/*
 * internal.h — private cross-file declarations for JS runtime
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

/* Forward declarations to keep internal.h small. */
typedef struct HlAsyncCtx     HlAsyncCtx;
typedef struct HlAllocator    HlAllocator;
typedef struct KlServer       KlServer;
typedef struct KlAsyncOp      KlAsyncOp;
typedef struct KlThreadPool   KlThreadPool;

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

/* Worker dispatch operation — runtime-specific, submitted to thread pool. */
typedef struct HlJsWorkerDispatchOp {
    HlAsyncCtx   *async_ctx;
    HlAllocator  *alloc;
    KlServer     *server;

    /* Input (deep-copied, owned) */
    char         *fn_source;
    size_t        fn_source_len;
    HlKV         *ctx_kvs;
    int           ctx_count;

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

int  hl_js_worker_dispatch_submit(KlThreadPool *pool,
                                  HlJsWorkerDispatchOp *op);
void hl_js_worker_dispatch_op_free(HlJsWorkerDispatchOp *op);
void hl_js_worker_dispatch_op_free_all(void *ptr);
void hl_js_worker_dispatch_cancel(KlAsyncOp *op, void *user_data);
void hl_js_worker_db_init(void);

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

/* ── Promoted: defined in sse.c, used in routes.c (kl_server_route for
 * SSE endpoints during wire_routes_server). */
void hl_js_sse_handler(struct KlRequest *req, struct KlResponse *res,
                       void *user_data);

/* ── Promoted: defined in routes.c, used by sibling files when they
 * need to register additional dynamically-created routes. Currently
 * only routes.c uses them, but keep declarations here so future split
 * files can reuse without re-introducing duplicates. */
int hl_js_track_route(HlJS *js, void *route);
int hl_js_track_alloc(HlJS *js, void ***arr, size_t *count,
                      size_t *cap, void *ptr);

#endif /* HL_RUNTIME_JS_INTERNAL_H */
