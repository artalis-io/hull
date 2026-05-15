/*
 * internal.h — private cross-file declarations for Lua runtime
 *
 * Shared declarations for runtime.c / dispatch.c / routes.c / timers.c /
 * ws.c / sse.c after the runtime.c god-module was split. Each of those
 * files used to live in a single TU and freely called static helpers
 * from a sibling section; here we surface only the symbols that now
 * need to cross the new file boundaries.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_LUA_INTERNAL_H
#define HL_RUNTIME_LUA_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "hull/runtime/lua.h"
#include "hull/cap/types.h"   /* HlKV for worker dispatch op */
#include "hull/limits/core.h" /* HL_WORKER_ERR_SIZE */
#include "lua.h"

/* Forward declarations to keep internal.h small. */
typedef struct HlAsyncCtx     HlAsyncCtx;
typedef struct HlAllocator    HlAllocator;
typedef struct KlServer       KlServer;
typedef struct KlAsyncOp      KlAsyncOp;
typedef struct KlThreadPool   KlThreadPool;

/* ── Internal op structs (moved from public lua.h, roadmap item J) ── */

/* Per-timer context. Allocated by app.every/app.daily registration. */
typedef struct HlLuaTimer {
    struct HlLua *lua;
    int         handler_id;    /* index into __hull_timers */
    int64_t     interval_ms;   /* repeat interval (or recomputed for daily) */
    int64_t     timer_id;      /* kl_timer_add return, for cancellation */
    int         daily;         /* 1 = daily timer */
    int         localtime;     /* 1 = local time, 0 = UTC */
    int         hour;          /* for daily: target hour */
    int         minute;        /* for daily: target minute */
    int         in_flight;     /* 1 = callback currently running (async) */
} HlLuaTimer;

/* Worker dispatch operation — runtime-specific, submitted to thread pool. */
typedef struct HlLuaWorkerDispatchOp {
    HlAsyncCtx   *async_ctx;
    HlAllocator  *alloc;
    KlServer     *server;

    /* Input (deep-copied, owned) */
    uint8_t      *bytecode;
    size_t        bytecode_len;
    HlKV         *ctx_kvs;
    int           ctx_count;

    /* Output (set by worker thread) */
    int           result_kind;       /* 0=nil,1=bool,2=int,3=double,4=text,5=table */
    int64_t       result_int;
    double        result_double;
    int           result_bool;
    char         *result_str;        /* owned */
    size_t        result_str_len;
    HlKV         *result_kvs;        /* owned, for table result */
    int           result_count;

    int           error;
    char          error_msg[HL_WORKER_ERR_SIZE];
    int           cancelled;
} HlLuaWorkerDispatchOp;

/* Submit a worker.dispatch operation to the thread pool.
 * Returns 0 on success, -1 on error. */
int hl_lua_worker_dispatch_submit(KlThreadPool *pool,
                                   HlLuaWorkerDispatchOp *op);

/* Free resources owned by a dispatch op (bytecode, kvs, result). */
void hl_lua_worker_dispatch_op_free(HlLuaWorkerDispatchOp *op);

/* Free dispatch op struct and all owned data (for use as free_driver). */
void hl_lua_worker_dispatch_op_free_all(void *ptr);

/* KlAsyncOp on_cancel handler for worker.dispatch operations. */
void hl_lua_worker_dispatch_cancel(KlAsyncOp *op, void *user_data);

/* Wire db.* module into worker Lua VMs. Call during runtime init. */
void hl_lua_worker_db_init(void);

/* Forward declaration for Keel WS server connection (avoid pulling
 * <keel/websocket_server.h> into every TU that just needs to call
 * an on_* trampoline). */
struct KlWsServerConn;

/* Forward struct types used across files */
typedef struct HlLuaWsRoute HlLuaWsRoute;
typedef struct HlLuaSseRoute HlLuaSseRoute;
struct HlLuaWsRoute {
    HlLua *lua;
    int     on_open_id;
    int     on_message_id;
    int     on_close_id;
    char    path[256];
};
struct HlLuaSseRoute {
    HlLua *lua;
    int     handler_id;
};

/* Clamp int64_t instruction count to int for lua_sethook */
#define INSTR_COUNT(x) ((x) > INT_MAX ? INT_MAX : (int)(x))

/* ── Promoted: defined in runtime.c, used in dispatch.c/timers.c/ws.c/sse.c
 * Lua instruction-count hook (gas metering). */
void hl_lua_instruction_hook(lua_State *L, lua_Debug *ar);

/* ── Promoted: defined in timers.c, used in routes.c (initial schedule
 * during wire_routes_server) and from within timers.c itself. */
int64_t hl_compute_daily_delay_ms(int hour, int minute, int use_local);
void hl_lua_timer_trampoline(void *user_data);
int hl_lua_track_timer(HlLua *lua, void *timer);

/* ── Promoted: defined in ws.c, used in routes.c (kl_ws_server_config
 * wiring during wire_routes_server). */
void hl_lua_ws_on_open(struct KlWsServerConn *ws_conn, void *user_data);
void hl_lua_ws_on_message(struct KlWsServerConn *ws_conn, const char *data,
                          size_t len, int is_binary, void *user_data);
void hl_lua_ws_on_close(struct KlWsServerConn *ws_conn, uint16_t code,
                        const char *reason, size_t reason_len,
                        void *user_data);

/* ── Promoted: defined in sse.c, used in routes.c (kl_server_route for
 * SSE endpoints during wire_routes_server). */
void hl_lua_sse_handler(struct KlRequest *req, struct KlResponse *res,
                        void *user_data);

/* ── Promoted: defined in routes.c, used elsewhere if route/alloc tracking
 * is needed. Currently only routes.c uses them, but keep declarations
 * here so future split files can reuse without re-introducing dupes. */
int hl_lua_track_route(HlLua *lua, void *route);
int hl_lua_track_alloc(HlLua *lua, void ***arr, size_t *count,
                       size_t *cap, void *ptr);

#endif /* HL_RUNTIME_LUA_INTERNAL_H */
