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
 * original function is upvalue 1. */
static int hl_lua_co_guarded(lua_State *L)
{
    lua_State *co = lua_tothread(L, 1);
    if (hl_lua_thread_is_parked(co))
        return luaL_error(L, "cannot resume or close a coroutine that is "
                             "waiting on a Hull operation");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

void hl_lua_guard_coroutine_lib(lua_State *L)
{
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
        status = lua_resume(co, lua->L, nargs, &nres);
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

        luaL_unref(lua->L, LUA_REGISTRYINDEX, lc->thread_ref);
        lc->thread_ref = LUA_NOREF;
        lc->co = NULL;
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;

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
        /* Handler yielded again - new HlAsyncCtx already set up. The new
         * continuation captured co/conn/thread_ref, and the timer through
         * active_timer set above. */
    } else {
        /* Error */
        char ebuf[512];
        const char *msg = hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf));
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
    hl_lua_set_parked(lc->co, 0);

    if (lc->thread_ref != LUA_NOREF) {
        luaL_unref(lua->L, LUA_REGISTRYINDEX, lc->thread_ref);
        lc->thread_ref = LUA_NOREF;
        lc->co = NULL;
    }
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

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!lua)
        return luaL_error(L, "hull._spawn: no runtime context");

    lua_State *co = lua_newthread(L);
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

    if (sr == LUA_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
    } else if (sr == LUA_YIELD) {
        /* Bg yielded; hl_lua_async_resume owns cleanup when it returns. */
    } else {
        char ebuf[512];
        log_error("[hull:async] coroutine error: %s",
                  hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf)));
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
