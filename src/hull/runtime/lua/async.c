/*
 * lua_async.c - Lua async continuation + hull.sleep()
 *
 * Implements HlLuaAsyncCont (the Lua-specific HlAsyncCont vtable) and
 * the hull.sleep() yielding C function.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "internal.h"
#include "protected.h"
#include "hull/http_feature.h"  /* hl_lua_http_error_response (HTTP-feature seam) */
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"
#include "hull/cap/db_registry.h"   /* hl_db_registry_open_txn */
#ifdef HL_ENABLE_DB
#include "hull/cap/db_budget.h"     /* hl_db_budget_current (hull._task) */
#endif

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>

#include "log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── HlLuaAsyncCont ───────────────────────────────────────────────── */

typedef struct HlLuaAsyncCont {
    HlAsyncCont        base;          /* vtable - must be first member */
    HlLua             *lua;           /* runtime instance */
    HlAllocator       *alloc;
    HlLuaPushResultFn  push_result;   /* NULL = no result (sleep) */
    lua_State         *co;            /* coroutine to resume */
    int                thread_ref;    /* registry ref for coroutine */
    KlHttpConn            *conn;          /* connection to resume (NULL = detached) */
    KlHttpRequest         *req;           /* request (for kl_http_request_send_response) */
    void              *timer_ctx;     /* HlLuaTimer* if running in a timer callback */
    /* Generic "handler finally completed" hook. Lets a dispatch site defer
     * teardown that must not run while the handler is still suspended (e.g.
     * the ws on_close conn teardown in ws.c) without coupling this async
     * core to any subsystem. Called once on LUA_OK / error completion. */
    void             (*on_complete)(HlLua *lua, void *ctx);
    void              *on_complete_ctx;
    int                settled;       /* resumed or cancelled */
    int                is_main;       /* co is app.main's: its ref is vt_lua_run_main's */
} HlLuaAsyncCont;

/*
 * Resume the Lua handler by calling lua_resume on the saved coroutine.
 *
 * The coroutine state (co, thread_ref, conn) is stored per-continuation
 * rather than in the HlLua singleton.  This allows multiple connections
 * to be suspended concurrently (e.g., self-fetch: the original connection
 * is suspended for the async HTTP response, while the server-side
 * connection for /api/slow can also suspend for hull.sleep).
 *
 * Under Keel v3 this only FINALIZES the response; it does NOT drive the
 * connection state machine (Keel owns the send after on_resume; see
 * include/hull/shared/async.h). Per result:
 *   LUA_OK    → build the response, kl_async_complete lets Keel send it
 *   LUA_YIELD → handler re-yielded; a new op is active, conn stays suspended
 *   error     → build a 500, kl_async_complete lets Keel send it
 */
/* Forward declarations for timer reschedule (defined in timers.c -
 * dropped under HL_ENABLE_HTTP=0; the corresponding call sites are
 * guarded so the symbol is never referenced in CLI builds). */
static void hl_lua_task_failed_now(HlLua *lua, int thread_ref, const char *msg);

#ifdef HL_ENABLE_HTTP_SERVER
void hl_lua_timer_reschedule(HlLuaTimer *t);
#endif

/* ── Parked coroutines ─────────────────────────────────────────────────
 *
 * A coroutine waiting on a Hull operation is marked in its thread's extra
 * space (no allocation, so marking cannot raise). The app can reach such a
 * coroutine - coroutine.running() inside a handler, passed to another task -
 * and coroutine.resume on it fed the yield values the app chose while a
 * worker could still be writing the op's result, after which the real
 * continuation resumed a dead coroutine. coroutine.resume / close refuse a
 * parked coroutine (hl_lua_guard_coroutine_lib). */
static char hl_lua_parked_marker;

static void hl_lua_set_parked(lua_State *co, int on)
{
    if (co) *(void **)lua_getextraspace(co) = on ? &hl_lua_parked_marker : NULL;
}

static int hl_lua_thread_is_parked(lua_State *co)
{
    return co && *(void **)lua_getextraspace(co) == &hl_lua_parked_marker;
}

/* coroutine.resume / coroutine.close, refusing a parked coroutine; the
 * original function is upvalue 1. A failure that was the instruction budget
 * running out is re-raised, not returned as (false, err): returned, a loop
 * resuming coroutine after coroutine never ended (budget.c). */
static int hl_lua_co_guarded(lua_State *L)
{
    lua_State *co = lua_tothread(L, 1);
    if (hl_lua_thread_is_parked(co))
        return luaL_error(L, "cannot resume or close a coroutine that is "
                             "waiting on a Hull operation");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    if (hl_lua_budget_tripped(L))
        return hl_lua_budget_raise(L);
    return lua_gettop(L);
}

int hl_lua_resume_status(lua_State *co, int status)
{
    if (status != LUA_YIELD || hl_lua_thread_is_parked(co))
        return status;
    /* Yielded with no Hull operation behind it: a bare coroutine.yield()
     * in the handler. Taken as "suspended", nothing would ever resume it -
     * the request's life never ended (its `res` stayed usable over a
     * response Keel had already sent and reused), a timer stayed in
     * flight for good. It ends here, as an error. Closed first, so a
     * reference the app kept to it cannot run it again. */
    (void)lua_closethread(co, NULL);
    /* Pushed protected: out of memory here aborted the process (no panic
     * handler). Without room for the message, nil stands in - the callers
     * read the error object with hl_lua_error_text, which takes any value. */
    static const char msg[] = "coroutine.yield() in a handler: only a Hull "
                              "operation may suspend it";
    if (hl_lua_pushlstring_safe(co, msg, sizeof msg - 1) != 0)
        lua_pushnil(co);
    return LUA_ERRRUN;
}

void hl_lua_guard_coroutine_lib(lua_State *L)
{
    /* Every thread starts with a copy of the main thread's extra space, so
     * clearing it here makes "not parked" the default for all of them (it
     * was never written: the parked check read uninitialized memory). */
    *(void **)lua_getextraspace(L) = NULL;
    lua_getglobal(L, "coroutine");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    static const char *const fns[] = { "resume", "close" };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        lua_getfield(L, -1, fns[i]);
        if (lua_isfunction(L, -1)) {
            lua_pushcclosure(L, hl_lua_co_guarded, 1);
            lua_setfield(L, -2, fns[i]);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

/* lua_pcall body for building an op's result: (cont, driver) lightuserdata
 * in, the result out. */
static int hl_lua_async_push_k(lua_State *L)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)lua_touserdata(L, 1);
    void *driver = lua_touserdata(L, 2);
    lua_settop(L, 0);
    lc->push_result(L, driver);
    return 1;
}

static void hl_lua_async_resume(HlAsyncCont *self, void *driver)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)self;
    lc->settled = 1;
    HlLua *lua = lc->lua;
    lua_State *co = lc->co;
    KlHttpConn *conn = lc->conn;

    if (!co) return;
    hl_lua_set_parked(co, 0);   /* the runtime resumes it now */

    /* Restore per-request context so C functions called during resume
     * (e.g., another http.async.get) can find the active connection */
    lua->active_co = co;
    lua->active_conn = conn;
    lua->active_req = lc->req;
    lua->active_thread_ref = lc->thread_ref;
    /* Re-arm the deferred-teardown hook so a further yield inside the handler
     * carries it onto the next continuation. */
    lua->active_on_complete     = lc->on_complete;
    lua->active_on_complete_ctx = lc->on_complete_ctx;

    /* Push driver result onto the coroutine stack so lua_resume
     * delivers it as the return value of the yield point */
    /* The result is built on the main state under lua_pcall, then moved to
     * the coroutine. Built straight onto the suspended coroutine, an
     * allocation failure (a large db.async result, an http.fetch body near
     * the heap limit) raised with no handler anywhere on that path, and Lua
     * aborted the whole process. Now the memory error itself is handed to the
     * coroutine, which takes the error path below - one request fails. */
    int nargs = 0;
    int push_failed = 0;
    if (driver && lc->push_result) {
        lua_State *M = lua->L;
        if (!lua_checkstack(M, 3) || !lua_checkstack(co, 2)) {
            push_failed = -1;
        } else {
            lua_pushcfunction(M, hl_lua_async_push_k);
            lua_pushlightuserdata(M, lc);
            lua_pushlightuserdata(M, driver);
            if (lua_pcall(M, 2, 1, 0) != LUA_OK)
                push_failed = 1;            /* the error object is on M */
            lua_xmove(M, co, 1);            /* the result, or the error */
            nargs = 1;
        }
    }

    /* ...and the timer, so a further yield inside a timer handler carries it
     * too: hl_lua_async_cont_create reads active_timer. Without it the second
     * wait of a timer handler lost the timer, in_flight was never cleared,
     * and the timer never fired again. */
    void *saved_timer = lua->active_timer;
    lua->active_timer = lc->timer_ctx;

    int nres = 0;
    int status;
    if (push_failed) {
        /* Not resumed: the coroutine ends here, through the error path,
         * with the error object on top (or, with no stack room even for
         * that, whatever is on top - hl_lua_error_text reads any value). */
        status = LUA_ERRMEM;
    } else {
        hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: a stale txn must not be joined */
        /* A new run: budget.c. A resume of app.main itself keeps main's
         * deadline default (cap/run_watchdog.h). */
        HL_LUA_ARM_KIND(lua, co, co == lua->cli_main_co ? HL_RUN_MAIN
                                                        : HL_RUN_ENTRY);
        status = lua_resume(co, lua->L, nargs, &nres);
        status = hl_lua_resume_status(co, status);
        hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: any open txn is stale now */
    }

    lua->active_timer           = saved_timer;
    lua->active_on_complete     = NULL;
    lua->active_on_complete_ctx = NULL;

    if (status == LUA_OK) {
        /* Handler completed */
        int cancelled = 0;
        if (lc->timer_ctx && nres > 0 &&
            lua_isboolean(co, -1) && !lua_toboolean(co, -1))
            cancelled = 1;

        /* CLI main coroutine just finished - stop the server so the
         * dispatching event loop returns. Detection is set-based: only
         * one coroutine is ever stored as cli_main_co (mutually
         * exclusive with server-mode handlers + timers + ws + sse, all
         * gated at registration). */
        int was_main = (lua->cli_main_co == co);

        /* The main coroutine's registry ref belongs to vt_lua_run_main, which
         * reads its results once the loop stops and then releases it. Released
         * here as well, the same slot went onto Lua's free list twice: the
         * next two luaL_ref calls returned one slot, so a later request's
         * parked coroutine lost its only reference and was resumed from freed
         * memory. */
        if (!was_main)
            luaL_unref(lua->L, LUA_REGISTRYINDEX, lc->thread_ref);
        lc->thread_ref = LUA_NOREF;
        lc->co = NULL;
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->active_req = NULL;

        /* Handler that yielded has now completed - run any deferred-teardown
         * hook (e.g. ws on_close conn teardown). */
        if (lc->on_complete) {
            lc->on_complete(lua, lc->on_complete_ctx);
            lc->on_complete = NULL;
        }

        /* Attached mode - finalize + send the resumed handler's response entirely
         * behind the HTTP-feature seam, so this base-runtime object holds NO Keel
         * refs (a compute app composes no HTTP and must link zero Keel). The seam
         * ends a streamed body and transitions the conn to SENDING - needed both
         * on the poll backend (kl_async_complete alone does not drive a resumed
         * handler's send) and on the streaming-async multipart route resumed from
         * the body reader's on_data. */
        if (conn)
            hl_lua_http_resume_send(lua, conn, lc->req);

        /* Timer async completion: clear in_flight and reschedule.
         * Timers only exist in HTTP builds (app.every / app.daily); the
         * timer_ctx field is always NULL in CLI-only builds so the
         * branch is dead but the symbol reference would still need to
         * link - guard it out entirely. */
#ifdef HL_ENABLE_HTTP_SERVER
        if (lc->timer_ctx) {
            HlLuaTimer *t = (HlLuaTimer *)lc->timer_ctx;
            t->in_flight = 0;
            if (!cancelled)
                hl_lua_timer_reschedule(t);
        }
#else
        (void)cancelled;
#endif

        if (was_main && lua->base.async_ctx) {
            lua->cli_main_co = NULL;
            hl_async_backend()->stop(lua->base.async_ctx);
        }
    } else if (status == LUA_YIELD) {
        /* Handler yielded again - new HlAsyncCtx already set up. The new
         * continuation captured co/conn/thread_ref, and the timer through
         * active_timer set above; the globals no longer describe a running
         * handler (audit 9 L6, as dispatch.c). */
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->active_req = NULL;
    } else {
        /* Error */
        char ebuf[512];
        const char *msg = hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf));
        int was_main = (lua->cli_main_co == co);
        if (conn)
            log_error("[hull:c] async lua handler error: %s",
                      msg ? msg : "(unknown)");
        else if (!was_main)   /* main: vt_lua_run_main reports it */
            log_error("[hull:timer] error: %s", msg ? msg : "(unknown)");

        /* A hull.async task that died uncaught: finish it, so its waiters
         * wake (audit 9 M3). A no-op for anything without a hook. */
        if (!was_main && !conn)
            hl_lua_task_failed_now(lua, lc->thread_ref, msg);

        /* As on success: the main coroutine's ref is vt_lua_run_main's. */
        if (!was_main)
            luaL_unref(lua->L, LUA_REGISTRYINDEX, lc->thread_ref);
        lc->thread_ref = LUA_NOREF;
        lc->co = NULL;
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->active_req = NULL;

        /* Run any deferred-teardown hook (handler errored after yielding). */
        if (lc->on_complete) {
            lc->on_complete(lua, lc->on_complete_ctx);
            lc->on_complete = NULL;
        }

#ifdef HL_ENABLE_HTTP_SERVER
        if (conn)
            hl_lua_http_resume_error(lua, conn, lc->req);  /* 500 + send, behind the seam */
#endif

        /* Timer error: clear in_flight and reschedule anyway. CLI-only
         * builds have no timers, so this branch is dead - guard out. */
#ifdef HL_ENABLE_HTTP_SERVER
        if (lc->timer_ctx) {
            HlLuaTimer *t = (HlLuaTimer *)lc->timer_ctx;
            t->in_flight = 0;
            hl_lua_timer_reschedule(t);
        }
#endif

        if (was_main && lua->base.async_ctx) {
            lua->cli_main_co = NULL;
            hl_async_backend()->stop(lua->base.async_ctx);
        }
    }
}

/*
 * Cancel the Lua handler - free the coroutine registry ref without
 * invoking the handler.
 */
static void hl_lua_async_cancel(HlAsyncCont *self)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)self;
    HlLua *lua = lc->lua;
    lc->settled = 1;

    if (lc->is_main) {
        /* app.main's coroutine is owned by vt_lua_run_main, which releases
         * its ref once the loop stops. A cancel at shutdown (pool teardown,
         * the SMTP sweep) comes after that: unref'd here as well, the slot
         * went onto the free list twice, and the parked mark was written
         * into a coroutine that may already have been collected. Touched
         * only while main is still alive. */
        if (lua->cli_main_co == lc->co)
            hl_lua_set_parked(lc->co, 0);
        lc->thread_ref = LUA_NOREF;
        lc->co = NULL;
    } else {
        hl_lua_set_parked(lc->co, 0);
        if (lc->thread_ref != LUA_NOREF) {
            luaL_unref(lua->L, LUA_REGISTRYINDEX, lc->thread_ref);
            lc->thread_ref = LUA_NOREF;
            lc->co = NULL;
        }
    }
    /* Its connection is gone: so is the snapshot its 500 would have kept
     * (audit 12). */
    hl_res_base_forget_conn(&lua->res_bases, lc->conn);
    lc->conn = NULL;
    /* The handler will never complete, so this is its end too: run the
     * completion hook (end the request's life, tear down a ws conn) rather
     * than leaving objects that point into a closed connection usable. */
    if (lc->on_complete) {
        lc->on_complete(lua, lc->on_complete_ctx);
        lc->on_complete = NULL;
    }
}

/*
 * Destroy the cont struct. Does NOT free the coroutine ref - that's
 * managed by the resume/cancel functions above.
 */
static void hl_lua_async_destroy(HlAsyncCont *self)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)self;
    /* Destroyed before it was ever resumed or cancelled: the operation that
     * made it failed to arm, and the handler carries on (it got an error).
     * Its coroutine is not waiting on anything - left marked parked, it
     * refused coroutine.resume / close for good. */
    if (!lc->settled && (!lc->is_main || lc->lua->cli_main_co == lc->co))
        hl_lua_set_parked(lc->co, 0);
    hl_alloc_free(lc->alloc, lc, sizeof(HlLuaAsyncCont));
}

/*
 * Create a Lua async continuation.
 * push_result: called on resume to push driver result onto Lua stack.
 *              NULL for sleep (no result to push).
 */
HlAsyncCont *hl_lua_async_cont_create(HlLua *lua, HlAllocator *alloc,
                                       HlLuaPushResultFn push_result)
{
    HlLuaAsyncCont *lc = hl_alloc_malloc(alloc, sizeof(HlLuaAsyncCont));
    if (!lc) return NULL;

    lc->base.resume  = hl_lua_async_resume;
    lc->base.cancel  = hl_lua_async_cancel;
    lc->base.destroy = hl_lua_async_destroy;
    lc->lua         = lua;
    lc->alloc       = alloc;
    lc->push_result = push_result;

    /* Capture per-request coroutine state so multiple connections can
     * be suspended concurrently without clobbering each other */
    lc->co         = lua->active_co;
    lc->thread_ref = lua->active_thread_ref;
    lc->conn       = lua->active_conn;
    lc->req        = lua->active_req;
    lc->timer_ctx  = lua->active_timer;  /* inherit timer ctx if in timer callback */
    lc->on_complete     = lua->active_on_complete;     /* deferred-teardown hook */
    lc->on_complete_ctx = lua->active_on_complete_ctx;
    lc->settled         = 0;
    lc->is_main         = (lc->co != NULL && lc->co == lua->cli_main_co);
    hl_lua_set_parked(lc->co, 1);

    return &lc->base;
}

/*
 * Set the timer context on a Lua async continuation.
 * Called by the timer trampoline so that async resume can reschedule.
 */
void hl_lua_async_cont_set_timer(HlAsyncCont *cont, void *timer)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)cont;
    lc->timer_ctx = timer;
}

/* A primitive that waits may only run on the coroutine the runtime is
 * driving: its continuation resumes lua->active_co, and lua_yieldk raises only
 * after the op is armed. So check first. Not yieldable: module load (require
 * runs under lua_pcall), a C callback such as string.gsub or table.sort.
 * Yieldable but not active_co: a coroutine the app created itself. */
int hl_lua_check_can_wait(lua_State *L, const char *what)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!lua || !lua_isyieldable(L) || L != lua->active_co)
        return luaL_error(L, "%s can only wait in a handler, a task or "
                          "app.main - not while a module loads, inside a C "
                          "callback such as string.gsub or table.sort, or in "
                          "a coroutine the app created", what);
    /* Another request, SSE event or timer runs while this one is parked, on
     * the SAME connection: a transaction held across the wait was rolled
     * back under it (or joined by the other entry), and the rest of this
     * handler then autocommitted (audit 5 M1, db_registry.h). */
    const char *txn = hl_db_registry_open_txn(lua->base.db_registry);
    /* Always NULL only in a DB-less build (the inline stub). */
    // cppcheck-suppress knownConditionTrueFalse
    if (txn)
        return luaL_error(L, "%s cannot wait while a transaction is open on "
                          "database connection '%s': other requests use the "
                          "same connection while this one waits. Commit or "
                          "roll back first (and do the waiting outside "
                          "db.batch)", what, txn);
    return 0;
}

/* ── hull.sleep(ms) ───────────────────────────────────────────────── */

/*
 * hull.sleep(ms) - yield the handler for `ms` milliseconds.
 * Uses KlAsyncOp deadline (no driver, no FD). The Keel deadline sweep
 * fires hl_async_on_deadline_sleep, which calls kl_async_complete.
 */
static int lua_hull_sleep(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms <= 0) return 0; /* no-op for zero/negative */

    /* Retrieve runtime context */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (!lua || !lua->base.async_ctx)
        return luaL_error(L, "hull.sleep() requires an active event loop");
    hl_lua_check_can_wait(L, "hull.sleep()");

    KlHttpServer *server = lua->server;
    KlHttpConn *conn = lua->active_conn;

    /* Create async ctx */
    HlAsyncCtx *ctx = hl_async_ctx_create(server, lua->base.net_ctx, lua->base.alloc);
    if (!ctx)
        return luaL_error(L, "hull.sleep(): out of memory");

    /* Create Lua continuation (no push_result - sleep has no return value) */
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc, NULL);
    if (!cont) {
        hl_async_ctx_free(ctx);
        return luaL_error(L, "hull.sleep(): out of memory");
    }
    ctx->cont = cont;

    if (conn) {
        /* Attached mode: use KlAsyncOp deadline via kl_async_suspend */
        ctx->op.deadline_ms = hl_async_backend()->monotonic_ms() + (uint64_t)ms;
        ctx->op.on_deadline = hl_async_on_deadline_sleep;
        ctx->driver = NULL;
        ctx->free_driver = NULL;
        ctx->detached = 0;

        if (hl_net_op_suspend(lua->base.net_ctx, (HlReqHandle *)conn, (HlSuspendOp *)&ctx->op) < 0) {
            ctx->cont->destroy(ctx->cont);
            hl_async_ctx_free(ctx);
            return luaL_error(L, "hull.sleep(): failed to suspend connection");
        }
    } else {
        /* Detached mode: schedule via the async backend vtable. The
         * underlying loop is the same one KlHttpServer drives (wrapped in
         * wire_caps), so the timer fires alongside HTTP work. */
        ctx->driver = NULL;
        ctx->free_driver = NULL;
        ctx->detached = 1;

        const HlAsyncBackend *be = hl_async_backend();
        uint64_t tid = be->timer_add(lua->base.async_ctx, (uint64_t)ms,
                                      hl_detached_timer_fire, ctx);
        if (tid == 0) {
            ctx->cont->destroy(ctx->cont);
            hl_async_ctx_free(ctx);
            return luaL_error(L, "hull.sleep(): failed to add timer");
        }
    }

    return lua_yieldk(L, 0, 0, NULL);
}

/* ── A task's failure hook (audit 9 M3) ───────────────────────────── */

/* hull._spawn(fn, on_fail): when fn's coroutine dies with an error that its
 * own code did not catch - a tripped instruction budget, which pcall
 * re-raises, or a memory error in the task wrapper - on_fail(message) runs
 * as an entry of its own, on the main thread, under a freshly armed budget.
 *
 * hull._async marks a task done from inside the task, after pcall(fn): a
 * task that tripped the budget after its first yield never got there, so its
 * waiters (task:wait, hull.gather / map, jobs.run_worker) stayed parked for
 * good and hull._running stayed raised. Lua cannot finish the task from the
 * dead coroutine - the trip is sticky for that run - so the runtime calls the
 * hook once the run is over.
 *
 * The hooks live in a weak-keyed registry table, keyed by the task's thread:
 * a task that returns is unreferenced and its entry goes with it. */
static const char hl_lua_task_fail_key = 0;

/* Pushes and removes the failure hook of the thread at the top of L's stack
 * (popping the thread): 1 with the hook on the stack, 0 with nothing pushed.
 * Allocates nothing. */
static int task_take_hook(lua_State *L)
{
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &hl_lua_task_fail_key) != LUA_TTABLE) {
        lua_pop(L, 2);
        return 0;
    }
    lua_pushvalue(L, -2);                /* thread */
    if (lua_rawget(L, -2) != LUA_TFUNCTION) {
        lua_pop(L, 3);
        return 0;
    }
    lua_pushvalue(L, -3);                /* thread */
    lua_pushnil(L);
    lua_rawset(L, -4);                   /* t[thread] = nil: an existing key */
    lua_replace(L, -3);                  /* hook where the thread was */
    lua_pop(L, 1);                       /* the table */
    return 1;
}

static int task_hook_call(lua_State *L)
{
    const char *msg = (const char *)lua_touserdata(L, 2);
    lua_settop(L, 1);
    lua_pushstring(L, msg ? msg : "(an error)");
    lua_call(L, 1, 0);
    return 0;
}

/* Run the hook at the top of lua->L's stack (popped) with msg, as an entry. */
static void task_hook_run(HlLua *lua, const char *msg)
{
    lua_State *L = lua->L;
    if (!lua_checkstack(L, 3)) {
        lua_pop(L, 1);
        log_error("[hull:async] a failed task's waiters could not be woken");
        return;
    }
    /* Nothing of the run that ended is active while the hook runs, and what
     * this clobbers - the active run's state, its budget and SQL budget
     * binding, its completion hook - is put back after, as lua_task_fire
     * does (audit 12: a hook run from inside a resumed run's error branch
     * left that run with the hook's budget, and an op the hook made captured
     * the run's on_complete). */
    lua_State     *saved_co   = lua->active_co;
    int            saved_ref  = lua->active_thread_ref;
    KlHttpConn    *saved_conn = lua->active_conn;
    KlHttpRequest *saved_req  = lua->active_req;
    void          *saved_tmr  = lua->active_timer;
    void         (*saved_oc)(struct HlLua *, void *) = lua->active_on_complete;
    void          *saved_oc_ctx = lua->active_on_complete_ctx;
    HlLuaBudget    saved_budget = lua->budget;
#ifdef HL_ENABLE_DB
    HlDbBudgetBinding saved_db_budget = hl_db_budget_current();
#endif
    lua->active_co              = NULL;
    lua->active_thread_ref      = LUA_NOREF;
    lua->active_conn            = NULL;
    lua->active_req             = NULL;
    lua->active_timer           = NULL;
    lua->active_on_complete     = NULL;
    lua->active_on_complete_ctx = NULL;

    hl_db_registry_guard_stale_txns(lua->base.db_registry);
    HL_LUA_ARM(lua, L);   /* a run of its own: the task's run is over */
    lua_pushcfunction(L, task_hook_call);
    lua_insert(L, -2);
    lua_pushlightuserdata(L, (void *)(uintptr_t)msg);   /* read only */
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        char ebuf[512];
        log_error("[hull:async] failed task: %s",
                  hl_lua_error_text(lua, L, -1, ebuf, sizeof(ebuf)));
        lua_pop(L, 1);
    }
    hl_db_registry_guard_stale_txns(lua->base.db_registry);

    lua->active_co              = saved_co;
    lua->active_thread_ref      = saved_ref;
    lua->active_conn            = saved_conn;
    lua->active_req             = saved_req;
    lua->active_timer           = saved_tmr;
    lua->active_on_complete     = saved_oc;
    lua->active_on_complete_ctx = saved_oc_ctx;
    lua->budget                 = saved_budget;
#ifdef HL_ENABLE_DB
    hl_db_budget_restore(saved_db_budget);
#endif
}

/* From the end of a resumed run (hl_lua_async_resume's error branch): the
 * coroutine `thread_ref` names died with msg. */
static void hl_lua_task_failed_now(HlLua *lua, int thread_ref, const char *msg)
{
    lua_State *L = lua->L;
    if (thread_ref == LUA_NOREF || thread_ref == LUA_REFNIL ||
        !lua_checkstack(L, 4))
        return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, thread_ref);
    if (task_take_hook(L))
        task_hook_run(lua, msg);
}

typedef struct {
    HlLua *lua;
    int    hook_ref;
    char   msg[512];
} HlLuaTaskFail;

static void task_fail_fire(void *user_data)
{
    HlLuaTaskFail *f = (HlLuaTaskFail *)user_data;
    lua_State *L = f->lua->L;
    if (lua_checkstack(L, 4)) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, f->hook_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, f->hook_ref);
        task_hook_run(f->lua, f->msg);
    } else {
        luaL_unref(L, LUA_REGISTRYINDEX, f->hook_ref);
    }
    free(f);
}

/* From inside the spawner's run (lua_hull_spawn's first resume failed): the
 * spawner's run may be the one that tripped, so the hook runs later, from
 * the loop. The thread is at the top of L's stack (popped). */
static void hl_lua_task_failed_later(HlLua *lua, lua_State *L, const char *msg)
{
    if (!task_take_hook(L))
        return;
    HlLuaTaskFail *f = calloc(1, sizeof *f);
    if (!f || !lua->base.async_ctx) {
        free(f);
        lua_pop(L, 1);
        return;   /* no loop: nothing can be waiting on the task */
    }
    f->lua = lua;
    snprintf(f->msg, sizeof f->msg, "%s", msg ? msg : "(an error)");
    f->hook_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (hl_async_backend()->timer_add(lua->base.async_ctx, 0,
                                      task_fail_fire, f) == 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, f->hook_ref);
        free(f);
        log_error("[hull:async] a failed task's waiters could not be woken");
    }
}

/* hull._spawn(fn) - spawn fn in a detached coroutine running on the event loop.
 *
 * Private: apps reach it through hull.async (hull._async), which wraps the body
 * so it can be joined. This is the raw fire-and-forget primitive underneath.
 * The body may call async-yielding primitives (hull.sleep, compute.async,
 * http.fetch, db.async); those capture `lua->active_co` at suspension, so we
 * set active_co (+ dispatch bookkeeping) to the bg coroutine for its first
 * resume. Subsequent resumes go through hl_lua_async_resume, which sets it
 * itself. A registry ref keeps the coroutine alive until it returns (here on a
 * synchronous finish, or later in hl_lua_async_resume's OK/error branches).
 *
 * Detached = fire-and-forget (no join). Used by jobs.run_worker's concurrency
 * (N in-flight claim-loops) and by hull.tui (tui.async aliases this). */
int lua_hull_spawn(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int has_hook = !lua_isnoneornil(L, 2);
    if (has_hook)
        luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_settop(L, 2);

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!lua)
        return luaL_error(L, "hull._spawn: no runtime context");

    lua_State *co = lua_newthread(L);
    if (has_hook) {
        /* The failure hook (audit 9 M3), keyed by the thread, weakly. */
        if (lua_rawgetp(L, LUA_REGISTRYINDEX, &hl_lua_task_fail_key) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_newtable(L);
            lua_pushliteral(L, "k");
            lua_setfield(L, -2, "__mode");
            lua_setmetatable(L, -2);
            lua_pushvalue(L, -1);
            lua_rawsetp(L, LUA_REGISTRYINDEX, &hl_lua_task_fail_key);
        }
        lua_pushvalue(L, -2);   /* the thread */
        lua_pushvalue(L, 2);    /* the hook */
        lua_rawset(L, -3);
        lua_pop(L, 1);
    }
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);

    /* The task inherits none of its spawner's per-dispatch context. A
     * continuation it creates captures all of these: with the spawner's timer
     * its completion would clear the timer's in_flight and reschedule it while
     * the timer's own handler still runs; with the spawner's teardown hook (a
     * ws on_close) it would run that teardown a second time. */
    lua_State      *saved_co         = lua->active_co;
    int             saved_thread_ref = lua->active_thread_ref;
    KlHttpConn     *saved_conn       = lua->active_conn;
    KlHttpRequest  *saved_req        = lua->active_req;
    void           *saved_timer      = lua->active_timer;
    void          (*saved_oc)(struct HlLua *, void *) = lua->active_on_complete;
    void           *saved_oc_ctx     = lua->active_on_complete_ctx;

    lua->active_co              = co;
    lua->active_thread_ref      = co_ref;
    lua->active_conn            = NULL;  /* bg is always detached */
    lua->active_req             = NULL;
    lua->active_timer           = NULL;
    lua->active_on_complete     = NULL;
    lua->active_on_complete_ctx = NULL;

    int nres = 0;
    int sr   = lua_resume(co, L, 0, &nres);
    sr = hl_lua_resume_status(co, sr);   /* a bare yield: an error */

    if (sr == LUA_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
    } else if (sr == LUA_YIELD) {
        /* Bg yielded; hl_lua_async_resume owns cleanup when it returns. */
    } else {
        char ebuf[512];
        const char *msg = hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf));
        log_error("[hull:async] coroutine error: %s", msg);
        /* A task's failure hook runs later, from the loop (audit 9 M3). */
        if (lua_checkstack(L, 4)) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, co_ref);
            hl_lua_task_failed_later(lua, L, msg);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
    }

    lua->active_co              = saved_co;
    lua->active_thread_ref      = saved_thread_ref;
    lua->active_conn            = saved_conn;
    lua->active_req             = saved_req;
    lua->active_timer           = saved_timer;
    lua->active_on_complete     = saved_oc;
    lua->active_on_complete_ctx = saved_oc_ctx;
    return 0;
}

/* ── hull._task: detached tasks (the twin of JS hull:_task) ───────────
 *
 * require("hull._task").spawn(fn) runs fn on a loop turn of its own (a
 * zero-delay backend timer), after the entry that spawned it has finished,
 * as a run that belongs to nothing: no request, connection, timer or
 * teardown hook is active, so every op it makes runs detached and a `res`
 * its closure captured belongs to a request that is over.
 *
 * hull._spawn (under hull.async) runs its body at once, inside the
 * spawner's run: a body that must not run on the request's clock or inside
 * its transaction had to wait first (hull.sleep), and a wait is refused
 * while a transaction is open (hl_lua_check_can_wait) - so under db.batch /
 * transaction middleware there was no way out of the request. A task fires
 * from the loop, after the request (and its transaction) ended.
 *
 * The task is an entry like the others: its own coroutine, so it may wait;
 * its own instruction budget; a stale transaction rolled back before and
 * after it. An error is logged, never raised into the spawner, which has
 * moved on. Pending tasks are released by hl_lua_free.
 *
 * Stdlib-only (an underscore module): auth-flows defers its mail with it. */

typedef struct HlLuaTask {
    struct HlLuaTask *next;
    struct HlLuaTask *prev;
    HlLua            *lua;
    int               fn_ref;
    uint64_t          timer_id;
} HlLuaTask;

static void lua_task_unlink(HlLuaTask *t)
{
    if (t->prev) t->prev->next = t->next;
    else         t->lua->tasks = t->next;
    if (t->next) t->next->prev = t->prev;
    t->next = t->prev = NULL;
}

/* The task's coroutine, made under lua_pcall: lua_newthread and luaL_ref
 * allocate, and a memory error raised from a timer callback has no handler
 * (Lua would abort the process). */
typedef struct {
    int        fn_ref;
    int        co_ref;
    lua_State *co;
} HlLuaTaskPrep;

static int lua_task_prepare_k(lua_State *L)
{
    HlLuaTaskPrep *p = (HlLuaTaskPrep *)lua_touserdata(L, 1);
    lua_State *co = lua_newthread(L);
    lua_rawgeti(co, LUA_REGISTRYINDEX, p->fn_ref);
    p->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);   /* pops the thread */
    p->co = co;
    return 0;
}

static void lua_task_fire(void *user)
{
    HlLuaTask *t = (HlLuaTask *)user;
    HlLua *lua = t->lua;
    lua_State *L = lua->L;
    HlLuaTaskPrep prep = { t->fn_ref, LUA_NOREF, NULL };
    lua_task_unlink(t);
    hl_alloc_free(lua->base.alloc, t, sizeof *t);

    if (!lua_checkstack(L, 3)) {
        luaL_unref(L, LUA_REGISTRYINDEX, prep.fn_ref);
        log_error("[hull:lua] spawned task error: out of stack");
        return;
    }
    lua_pushcfunction(L, lua_task_prepare_k);
    lua_pushlightuserdata(L, &prep);
    int prc = lua_pcall(L, 1, 0, 0);
    luaL_unref(L, LUA_REGISTRYINDEX, prep.fn_ref);
    if (prc != LUA_OK) {
        lua_pop(L, 1);
        log_error("[hull:lua] spawned task error: out of memory");
        return;
    }
    lua_State *co = prep.co;

    /* What this clobbers, put back after. From the event loop nothing is
     * active; a loop pumped from inside another run (an in-process test
     * harness) fires the task inside that run, whose state and budget must
     * come back as they were (audit 9 L1, the JS twin). */
    lua_State      *save_co         = lua->active_co;
    int             save_thread_ref = lua->active_thread_ref;
    KlHttpConn     *save_conn       = lua->active_conn;
    KlHttpRequest  *save_req        = lua->active_req;
    void           *save_timer      = lua->active_timer;
    void          (*save_oc)(struct HlLua *, void *) = lua->active_on_complete;
    void           *save_oc_ctx     = lua->active_on_complete_ctx;
    HlLuaBudget     save_budget     = lua->budget;
    /* And its wall-clock deadline: the arm below sets the task's own
     * (cap/run_watchdog.h). */
    uint64_t        save_deadline   = hl_run_watch_save(&lua->run_watch);
#ifdef HL_ENABLE_DB
    /* The arm below rebinds this thread's SQL budget to this run. */
    HlDbBudgetBinding save_db_budget = hl_db_budget_current();
#endif

    /* Nothing of whatever ran last is active: an op the task makes captures
     * all of these, and runs detached. */
    lua->active_co              = co;
    lua->active_thread_ref      = prep.co_ref;
    lua->active_conn            = NULL;
    lua->active_req             = NULL;
    lua->active_timer           = NULL;
    lua->active_on_complete     = NULL;
    lua->active_on_complete_ctx = NULL;

    hl_db_registry_guard_stale_txns(lua->base.db_registry);   /* audit 6 M1 */
    HL_LUA_ARM(lua, co);                                       /* its own run */
    int nres = 0;
    int st = lua_resume(co, L, 0, &nres);
    st = hl_lua_resume_status(co, st);   /* a bare yield: an error */
    hl_db_registry_guard_stale_txns(lua->base.db_registry);   /* audit 6 M1 */

    if (st == LUA_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, prep.co_ref);
    } else if (st == LUA_YIELD) {
        /* Parked on a Hull op: hl_lua_async_resume owns the ref now. */
    } else {
        char ebuf[512];
        log_error("[hull:lua] spawned task error: %s",
                  hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf)));
        luaL_unref(L, LUA_REGISTRYINDEX, prep.co_ref);
    }

    lua->active_co              = save_co;
    lua->active_thread_ref      = save_thread_ref;
    lua->active_conn            = save_conn;
    lua->active_req             = save_req;
    lua->active_timer           = save_timer;
    lua->active_on_complete     = save_oc;
    lua->active_on_complete_ctx = save_oc_ctx;
    lua->budget                 = save_budget;
    hl_run_watch_restore(&lua->run_watch, save_deadline);
#ifdef HL_ENABLE_DB
    hl_db_budget_restore(save_db_budget);
#endif
}

/* hull._task.spawn(fn) */
static int lua_task_spawn(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_settop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!lua || !lua->base.async_ctx)
        return luaL_error(L, "hull._task.spawn() requires an active event loop");

    /* The ref first: luaL_ref can raise (out of memory), and nothing is
     * allocated yet to leak. */
    lua_pushvalue(L, 1);
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    HlLuaTask *t = hl_alloc_malloc(lua->base.alloc, sizeof *t);
    if (!t) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
        return luaL_error(L, "hull._task.spawn(): out of memory");
    }
    t->lua      = lua;
    t->fn_ref   = fn_ref;
    t->timer_id = 0;
    t->prev     = NULL;
    t->next = (HlLuaTask *)lua->tasks;
    if (t->next) t->next->prev = t;
    lua->tasks = t;
    t->timer_id = hl_async_backend()->timer_add(lua->base.async_ctx, 0,
                                                lua_task_fire, t);
    if (t->timer_id == 0) {
        lua_task_unlink(t);
        luaL_unref(L, LUA_REGISTRYINDEX, t->fn_ref);
        hl_alloc_free(lua->base.alloc, t, sizeof *t);
        return luaL_error(L, "hull._task.spawn(): failed to add timer");
    }
    return 0;
}

void hl_lua_tasks_free(HlLua *lua)
{
    if (!lua) return;
    const HlAsyncBackend *be = hl_async_backend();
    while (lua->tasks) {
        HlLuaTask *t = (HlLuaTask *)lua->tasks;
        lua_task_unlink(t);
        if (lua->base.async_ctx)
            be->timer_cancel(lua->base.async_ctx, t->timer_id);
        if (lua->L)
            luaL_unref(lua->L, LUA_REGISTRYINDEX, t->fn_ref);
        hl_alloc_free(lua->base.alloc, t, sizeof *t);
    }
}

int luaopen_hull_task(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_task_spawn);
    lua_setfield(L, -2, "spawn");
    return 1;
}

/* ── Park / wake: the join primitive ─────────────────────────────── */

/*
 * hull._token()        -> a wake token, not yet fired
 * hull._park(token)    -> suspend until the token is fired (at once if it
 *                         already has been)
 * hull._wake(token)    -> fire it; a coroutine parked on it resumes
 *
 * Private to hull._async, which builds joinable tasks on them. The wake is
 * always DEFERRED to the event loop (a zero-delay timer), never an inline
 * resume: the waker is usually a task finishing inside its own resume, and
 * resuming another coroutine from there would replace the active-coroutine
 * state it is still running under.
 *
 * The token's state lives in a separate heap slot, not in the userdata:
 * resuming the parked coroutine runs Lua, which can collect the token before
 * the async ctx releases it (free_driver). The slot is freed by whichever of
 * its holders - the token's __gc, the parked ctx, a pending wake timer - lets
 * go last.
 */

#define HL_LUA_TOKEN_MT "hull.token"

typedef struct HlParkSlot {
    HlLua      *lua;
    HlAsyncCtx *ctx;      /* set while parked; cleared when the ctx lets go */
    int         fired;
    int         refs;
} HlParkSlot;

typedef struct { HlParkSlot *slot; } HlLuaToken;

static void slot_release(HlParkSlot *s)
{
    if (s && --s->refs == 0) free(s);
}

/* free_driver for a parked ctx: every resume and cancel path calls it before
 * the ctx is freed, so the slot never points at a freed ctx. */
static void slot_ctx_gone(void *driver)
{
    HlParkSlot *s = (HlParkSlot *)driver;
    s->ctx = NULL;
    slot_release(s);
}

static HlLuaToken *check_token(lua_State *L, int idx)
{
    return (HlLuaToken *)luaL_checkudata(L, idx, HL_LUA_TOKEN_MT);
}

static int lua_token_gc(lua_State *L)
{
    HlLuaToken *t = check_token(L, 1);
    slot_release(t->slot);
    t->slot = NULL;
    return 0;
}

static int lua_hull_token(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!lua) return luaL_error(L, "hull._token: no runtime context");

    HlLuaToken *t = lua_newuserdatauv(L, sizeof *t, 0);
    t->slot = NULL;
    luaL_setmetatable(L, HL_LUA_TOKEN_MT);
    HlParkSlot *s = calloc(1, sizeof *s);
    if (!s) return luaL_error(L, "hull._token: out of memory");
    s->lua  = lua;
    s->refs = 1;                      /* the token's */
    t->slot = s;
    return 1;
}

static int lua_hull_park(lua_State *L)
{
    HlLuaToken *t = check_token(L, 1);
    HlParkSlot *s = t->slot;
    if (!s) return luaL_error(L, "hull._park: dead token");
    if (s->fired) return 0;           /* already woken: nothing to wait for */
    if (s->ctx) return luaL_error(L, "hull._park: token already has a waiter");

    HlLua *lua = s->lua;
    if (!lua->base.async_ctx)
        return luaL_error(L, "hull._park: requires an active event loop");
    hl_lua_check_can_wait(L, "task:wait()");

    HlAsyncCtx *ctx = hl_async_ctx_create(lua->server, lua->base.net_ctx,
                                          lua->base.alloc);
    if (!ctx) return luaL_error(L, "hull._park: out of memory");
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc, NULL);
    if (!cont) {
        hl_async_ctx_free(ctx);
        return luaL_error(L, "hull._park: out of memory");
    }
    ctx->cont        = cont;
    ctx->driver      = s;             /* not pushed: push_result is NULL */
    ctx->free_driver = slot_ctx_gone;

#ifdef HL_ENABLE_HTTP_SERVER
    KlHttpConn *conn = lua->active_conn;
    if (conn) {
        /* Attached (a request handler): suspend the request with no
         * deadline; the wake completes the op. */
        ctx->op.deadline_ms = 0;
        ctx->op.on_deadline = hl_async_on_deadline_sleep;
        ctx->detached = 0;
        if (hl_net_op_suspend(lua->base.net_ctx, (HlReqHandle *)conn,
                              (HlSuspendOp *)&ctx->op) < 0) {
            ctx->cont->destroy(ctx->cont);
            hl_async_ctx_free(ctx);
            return luaL_error(L, "hull._park: failed to suspend connection");
        }
    } else
#endif
    {
        /* Detached (app.main, a timer, a task): nothing is armed; only the
         * wake resumes it. */
        ctx->detached = 1;
    }
    s->ctx = ctx;
    s->refs++;                        /* the ctx's, until slot_ctx_gone */
    return lua_yieldk(L, 0, 0, NULL);
}

/* The deferred half of a wake, on the loop. */
static void slot_wake_fire(void *user_data)
{
    HlParkSlot *s = (HlParkSlot *)user_data;
    HlAsyncCtx *ctx = s->ctx;
    if (ctx) {
        /* Still parked (not cancelled with its request meanwhile). */
        if (ctx->detached) {
            hl_async_ctx_resume_detached(ctx);
        }
#ifdef HL_ENABLE_HTTP_SERVER
        else {
            hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
        }
#endif
    }
    slot_release(s);                  /* the timer's */
}

static int lua_hull_wake(lua_State *L)
{
    HlLuaToken *t = check_token(L, 1);
    HlParkSlot *s = t->slot;
    if (!s || s->fired) return 0;
    s->fired = 1;
    if (!s->ctx) return 0;            /* nobody parked yet: park returns at once */

    HlLua *lua = s->lua;
    s->refs++;                        /* the timer's, until slot_wake_fire */
    uint64_t tid = hl_async_backend()->timer_add(lua->base.async_ctx, 0,
                                                 slot_wake_fire, s);
    if (tid == 0) {
        s->refs--;
        s->fired = 0;                 /* not woken after all */
        return luaL_error(L, "hull._wake: failed to schedule the wake");
    }
    return 0;
}

/* ── Module registration ──────────────────────────────────────────── */

static const luaL_Reg hull_funcs[] = {
    {"sleep",  lua_hull_sleep},
    {"_spawn", lua_hull_spawn},
    {"_token", lua_hull_token},
    {"_park",  lua_hull_park},
    {"_wake",  lua_hull_wake},
    {NULL, NULL}
};

/* async / gather / map live in Lua (hull._async) and are loaded the first time
 * one is touched, so a program that never uses them never loads it. */
static int lua_hull_index(lua_State *L)
{
    const char *k = lua_tostring(L, 2);
    if (!k || (strcmp(k, "async") != 0 && strcmp(k, "gather") != 0
               && strcmp(k, "map") != 0))
        return 0;
    lua_pushcfunction(L, hl_lua_require_trusted);
    lua_pushliteral(L, "hull._async");
    lua_call(L, 1, 1);
    const char *names[] = { "async", "gather", "map" };
    for (int i = 0; i < 3; i++) {
        lua_getfield(L, -1, names[i]);
        lua_setfield(L, 1, names[i]);  /* cache on the hull table */
    }
    lua_pop(L, 1);
    lua_rawget(L, 1);                  /* the key (still at 2) */
    return 1;
}

int luaopen_hull_hull(lua_State *L)
{
    if (luaL_newmetatable(L, HL_LUA_TOKEN_MT)) {
        lua_pushcfunction(L, lua_token_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushliteral(L, "hull.token");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    luaL_newlib(L, hull_funcs);
    lua_newtable(L);
    lua_pushcfunction(L, lua_hull_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    return 1;
}
