/*
 * dispatch.c - Lua request/middleware dispatch bridges
 *
 * Bridges Keel's per-request callbacks to the Lua handler/middleware
 * registry. Creates coroutines, marshals req/res, drives the
 * instruction-count hook, and cleans up middleware ctx.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/req_life.h"
#include "internal.h"
#include "hull/http_feature.h"  /* hl_lua_http_error_response (HTTP-feature seam) */

#include "hull/reqctx.h"
#include "hull/utils/alloc.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_registry.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>

#include <sh_arena.h>

#include "log.h"

#include <limits.h>
#include <string.h>

/* Forget the request ctx a middleware left (its table, kept by
 * bindings.c), or the JSON one the test harness injected. Never raises. */
void hl_lua_free_req_ctx(HlLua *lua, KlHttpRequest *req)
{
    if (!req->ctx) return;
    HlReqCtx *rctx = (HlReqCtx *)req->ctx;
    req->ctx = NULL;
    if (rctx == &hl_lua_req_ctx_marker) {
        hl_lua_req_ctx_drop(lua->L, req);
        return;
    }
    if (rctx->kind == HL_REQCTX_JSON)
        hl_alloc_free(lua->base.alloc, rctx->json.data, rctx->json.len + 1);
    hl_alloc_free(lua->base.alloc, rctx, sizeof(HlReqCtx));
}

void hl_lua_request_done(HlLua *lua, const KlHttpRequest *req)
{
    if (lua && lua->L && req)
        hl_lua_req_ctx_drop(lua->L, req);
}

typedef struct {
    KlHttpRequest  *req;
    KlHttpResponse *res;
    HlReqLife      *life;
} HlReqArgs;

/* Under the protected entry (hl_lua_entry_prepare): req, res. */
static int push_req_res(lua_State *L, void *ud)
{
    HlReqArgs *a = (HlReqArgs *)ud;
    hl_lua_make_request(L, a->req, a->life);
    hl_lua_make_response_life(L, a->res, a->life);
    return 2;
}

int hl_lua_dispatch(HlLua *lua, int handler_id,
                       KlHttpRequest *req, KlHttpResponse *res)
{
    if (!lua || !lua->L || !req || !res)
        return -1;


    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_registry_guard_stale_txns(lua->base.db_registry);

    /* Reset scratch arena for this request */
    sh_arena_reset(lua->scratch);

    /* Set per-request async context (for hull.sleep / http.get access) */
    lua->active_conn = kl_http_request_conn(req);
    lua->active_req = req;

    /* The request's life: `res` holds it, and it ends when this handler is
     * done (below, or in the async continuation's completion / cancel). */
    HlReqLife *life = hl_req_life_new();
    if (!life) {
        lua->active_conn = NULL;
        lua->active_req = NULL;
        return -1;
    }

    /* The coroutine with handler(req, res) on it, built protected. */
    HlReqArgs args = { req, res, life };
    int thread_ref = LUA_NOREF, nargs = 0;
    lua_State *co = hl_lua_entry_prepare(lua, "__hull_routes", handler_id,
                                         push_req_res, &args,
                                         &thread_ref, &nargs);
    /* The handler's req table holds ctx now; nothing reads req->ctx again.
     * Forgotten here rather than when the handler ends, which a handler
     * that waits on anything never did (the continuation has no req). */
    hl_lua_free_req_ctx(lua, req);
    if (!co) {
        hl_req_life_end(life);
        lua->active_conn = NULL;
        lua->active_req = NULL;
        return -1;
    }

    /* Set coroutine state for async C functions */
    lua->active_co = co;
    lua->active_thread_ref = thread_ref;

    /* Arm the instruction budget for this run */
    HL_LUA_ARM(lua, co);

    /* A continuation created while the handler runs captures this, so the
     * life ends when the handler finally completes, or is cancelled. */
    lua->active_on_complete     = hl_lua_req_life_end_cb;
    lua->active_on_complete_ctx = life;

    /* Resume coroutine: handler(req, res) */
    int nres = 0;
    int status = lua_resume(co, lua->L, nargs, &nres);
    status = hl_lua_resume_status(co, status);
    hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: any open txn is stale now */

    lua->active_on_complete     = NULL;
    lua->active_on_complete_ctx = NULL;
    if (status != LUA_YIELD)
        hl_req_life_end(life);

    if (status == LUA_OK) {
        /* Synchronous completion - same as lua_pcall path */
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->active_req = NULL;

        /* Pop any return values */
        if (nres > 0)
            lua_settop(co, 0);
        return 0;
    }

    if (status == LUA_YIELD) {
        /* Handler yielded - connection is suspended.
         * Don't clean up coroutine ref.
         * kl_async_suspend already removed client FD from event loop.
         * The continuation captured co / conn / req; the globals no longer
         * describe a running handler. Left set, the next middleware's
         * res:json compressed by THIS request's Accept-Encoding and its
         * multipart check took this connection for its own. */
        lua->active_co = NULL;
        lua->active_thread_ref = LUA_NOREF;
        lua->active_conn = NULL;
        lua->active_req = NULL;
        return 1; /* signal: handler suspended */
    }

    /* Error */
    char ebuf[512];
    log_error("[hull:c] lua handler error: %s",
              hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf)));
    luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
    lua->active_thread_ref = LUA_NOREF;
    lua->active_co = NULL;
    lua->active_conn = NULL;
    lua->active_req = NULL;
    return -1;
}

void hl_lua_req_life_end_cb(HlLua *lua, void *life)
{
    (void)lua;
    hl_req_life_end((HlReqLife *)life);
}

void hl_lua_keel_handler(KlHttpRequest *req, KlHttpResponse *res, void *user_data)
{
    HlLuaRoute *route = (HlLuaRoute *)user_data;
    int rc = hl_lua_dispatch(route->lua, route->handler_id, req, res);
    if (rc < 0) {
        /* Error - write 500 response */
        hl_lua_http_error_response(res);
    }
    /* rc == 1 → handler suspended, conn_process checks SUSPENDED state */
}

/* ── Middleware dispatch ────────────────────────────────────────────── */

/* Middleware runs to completion on the main state. Everything that
 * allocates - building req / res, reading back req.ctx, the registry ref
 * that carries ctx to the next stage - runs inside this one protected
 * call: done unprotected around it, a memory error (or an error from an
 * __index the middleware put on req) had nothing to unwind to and aborted
 * the process. Results: (status code, ctx-ref or nil). */
typedef struct {
    HlLua          *lua;
    KlHttpRequest  *req;
    KlHttpResponse *res;
    HlReqLife      *life;
    int             handler_id;
    int             has_ctx;     /* out: req.ctx was a table, kept for req */
    int             result;      /* out: 0 continue, non-zero short-circuit */
} HlMwRun;

static int mw_run_k(lua_State *L)
{
    HlMwRun *m = (HlMwRun *)lua_touserdata(L, 1);
    lua_settop(L, 0);
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    if (!lua_istable(L, -1))
        return luaL_error(L, "no route table");
    lua_rawgeti(L, -1, m->handler_id);
    if (!lua_isfunction(L, -1))
        return luaL_error(L, "middleware %d is not a function", m->handler_id);
    hl_lua_make_request(L, m->req, m->life);              /* 3 */
    lua_pushvalue(L, -1);                                 /* 4: req, kept */
    lua_insert(L, 2);                                     /* routes,req,fn,req */
    hl_lua_make_response_life(L, m->res, m->life);        /* ..., res */
    lua_call(L, 2, 1);                                    /* routes,req,ret */

    /* 0 = continue, non-zero = short-circuit */
    if (lua_isnumber(L, -1))
        m->result = (int)lua_tointeger(L, -1);
    else if (lua_isboolean(L, -1))
        m->result = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);

    /* req.ctx, read raw: an __index (or a __newindex trap) on req is the
     * middleware's own code and does not run here. */
    lua_pushliteral(L, "ctx");
    lua_rawget(L, 2);
    if (lua_istable(L, -1)) {
        hl_lua_req_ctx_store(L, m->req, -1);
        m->has_ctx = 1;
    }
    return 0;
}

int hl_lua_dispatch_middleware(HlLua *lua, int handler_id,
                               KlHttpRequest *req, KlHttpResponse *res)
{
    if (!lua || !lua->L || !req || !res)
        return -1;

    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_registry_guard_stale_txns(lua->base.db_registry);

    /* Arm the instruction budget for this middleware call */
    HL_LUA_ARM(lua, lua->L);

    /* Reset scratch arena for this middleware call */
    sh_arena_reset(lua->scratch);

    /* Middleware runs to completion (lua_pcall, no yield), so its `res`
     * belongs to this call alone: the life ends as soon as it returns. */
    HlReqLife *life = hl_req_life_new();
    if (!life) {
        hl_lua_free_req_ctx(lua, req);   /* answered here: a 500 */
        return -1;
    }

    HlMwRun m = { lua, req, res, life, handler_id, 0, 0 };
    lua_State *L = lua->L;
    int base = lua_gettop(L);
    if (!lua_checkstack(L, 8)) {
        hl_req_life_end(life);
        hl_lua_free_req_ctx(lua, req);
        return -1;
    }
    /* The middleware's own request, for as long as it runs (it cannot
     * yield): res:json / html / text decide compression from active_req, and
     * multipart ownership is checked against active_conn. Both were left
     * describing whatever request ran last. */
    KlHttpRequest *saved_req  = lua->active_req;
    KlHttpConn    *saved_conn = lua->active_conn;
    lua->active_req  = req;
    lua->active_conn = kl_http_request_conn(req);
    lua_pushcfunction(L, mw_run_k);
    lua_pushlightuserdata(L, &m);
    int mw_rc = lua_pcall(L, 1, 0, 0);
    hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: any open txn is stale now */
    lua->active_req  = saved_req;
    lua->active_conn = saved_conn;
    hl_req_life_end(life);
    if (mw_rc != LUA_OK) {
        char ebuf[512];
        log_error("[hull:c] lua middleware error: %s",
                  hl_lua_error_text(lua, L, -1, ebuf, sizeof(ebuf)));
        lua_settop(L, base);
        /* Answered here (a 500): no later stage reads ctx. One stored
         * before the error is under req's key, marker or not. */
        hl_lua_free_req_ctx(lua, req);
        hl_lua_req_ctx_drop(L, req);
        return -1;
    }
    lua_settop(L, base);

    /* req.ctx is kept for this request (bindings.c), so the next middleware
     * or the handler gets the table itself (no JSON round-trip). */
    if (m.has_ctx && req->ctx != &hl_lua_req_ctx_marker) {
        hl_lua_free_req_ctx(lua, req);   /* the test harness's JSON one */
        req->ctx = &hl_lua_req_ctx_marker;
    }
    /* A short-circuit answers the request here: no later stage reads it. */
    if (m.result != 0)
        hl_lua_free_req_ctx(lua, req);
    return m.result;
}

int hl_lua_keel_middleware(KlHttpRequest *req, KlHttpResponse *res, void *user_data)
{
    HlLuaRoute *ctx = (HlLuaRoute *)user_data;
    int rc = hl_lua_dispatch_middleware(ctx->lua, ctx->handler_id, req, res);
    if (rc < 0) {
        /* Middleware error - short-circuit with 500 */
        hl_lua_http_error_response(res);
        return 1; /* short-circuit */
    }
    return rc;
}
