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
#include "hull/http_feature.h"  /* hl_lua_http_error_response (HTTP-feature seam) */
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>

#include "log.h"

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
#ifdef HL_ENABLE_HTTP_SERVER
void hl_lua_timer_reschedule(HlLuaTimer *t);
#endif

static void hl_lua_async_resume(HlAsyncCont *self, void *driver)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)self;
    HlLua *lua = lc->lua;
    lua_State *co = lc->co;
    KlHttpConn *conn = lc->conn;

    if (!co) return;

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
    int nargs = 0;
    if (driver && lc->push_result) {
        lc->push_result(co, driver);
        nargs = 1;
    }

    int nres = 0;
    int status = lua_resume(co, lua->L, nargs, &nres);

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

        luaL_unref(lua->L, LUA_REGISTRYINDEX, lc->thread_ref);
        lc->thread_ref = LUA_NOREF;
        lc->co = NULL;
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->dispatch_depth--;

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
            hl_lua_http_resume_send(conn, lc->req);

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
        /* Handler yielded again - new HlAsyncCtx already set up.
         * The new continuation captured the current co/conn/thread_ref.
         * dispatch_depth stays elevated - decremented on final resume.
         * Transfer timer_ctx to the new continuation if present. */
        if (lc->timer_ctx) {
            /* Find the most recent continuation on the Lua state -
             * it will have been stored via hl_lua_async_cont_create.
             * The new cont has our co/thread_ref already captured. */
        }
    } else {
        /* Error */
        const char *msg = lua_tostring(co, -1);
        int was_main = (lua->cli_main_co == co);
        if (conn)
            log_error("[hull:c] async lua handler error: %s",
                      msg ? msg : "(unknown)");
        else if (was_main)
            log_error("[hull:main] error: %s", msg ? msg : "(unknown)");
        else
            log_error("[hull:timer] error: %s", msg ? msg : "(unknown)");

        luaL_unref(lua->L, LUA_REGISTRYINDEX, lc->thread_ref);
        lc->thread_ref = LUA_NOREF;
        lc->co = NULL;
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->dispatch_depth--;

        /* Run any deferred-teardown hook (handler errored after yielding). */
        if (lc->on_complete) {
            lc->on_complete(lua, lc->on_complete_ctx);
            lc->on_complete = NULL;
        }

#ifdef HL_ENABLE_HTTP_SERVER
        if (conn)
            hl_lua_http_resume_error(conn, lc->req);  /* 500 + send, behind the seam */
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

    if (lc->thread_ref != LUA_NOREF) {
        luaL_unref(lua->L, LUA_REGISTRYINDEX, lc->thread_ref);
        lc->thread_ref = LUA_NOREF;
        lc->co = NULL;
    }
    lc->conn = NULL;
}

/*
 * Destroy the cont struct. Does NOT free the coroutine ref - that's
 * managed by the resume/cancel functions above.
 */
static void hl_lua_async_destroy(HlAsyncCont *self)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)self;
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

/* hull.async(fn) - spawn fn in a detached coroutine running on the event loop.
 * The body may call async-yielding primitives (hull.sleep, compute.async,
 * http.fetch, db.async); those capture `lua->active_co` at suspension, so we
 * set active_co (+ dispatch bookkeeping) to the bg coroutine for its first
 * resume. Subsequent resumes go through hl_lua_async_resume, which sets it
 * itself. A registry ref keeps the coroutine alive until it returns (here on a
 * synchronous finish, or later in hl_lua_async_resume's OK/error branches).
 *
 * Detached = fire-and-forget (no join). Used by jobs.run_worker's concurrency
 * (N in-flight claim-loops) and by hull.tui (tui.async aliases this). */
int lua_hull_async(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!lua)
        return luaL_error(L, "hull.async: no runtime context");

    lua_State *co = lua_newthread(L);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);

    lua_State *saved_co         = lua->active_co;
    int        saved_thread_ref = lua->active_thread_ref;
    KlHttpConn    *saved_conn       = lua->active_conn;

    lua->active_co         = co;
    lua->active_thread_ref = co_ref;
    lua->active_conn       = NULL;  /* bg is always detached */
    lua->dispatch_depth++;

    int nres = 0;
    int sr   = lua_resume(co, L, 0, &nres);

    if (sr == LUA_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
        lua->dispatch_depth--;
    } else if (sr == LUA_YIELD) {
        /* Bg yielded; hl_lua_async_resume owns cleanup when it returns. */
    } else {
        const char *msg = lua_tostring(co, -1);
        log_error("[hull:async] coroutine error: %s", msg ? msg : "(unknown)");
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
        lua->dispatch_depth--;
    }

    lua->active_co         = saved_co;
    lua->active_thread_ref = saved_thread_ref;
    lua->active_conn       = saved_conn;
    return 0;
}

/* ── Module registration ──────────────────────────────────────────── */

static const luaL_Reg hull_funcs[] = {
    {"sleep", lua_hull_sleep},
    {"async", lua_hull_async},
    {NULL, NULL}
};

int luaopen_hull_hull(lua_State *L)
{
    luaL_newlib(L, hull_funcs);
    return 1;
}
