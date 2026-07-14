/*
 * dispatch.c — Lua request/middleware dispatch bridges
 *
 * Bridges Keel's per-request callbacks to the Lua handler/middleware
 * registry. Creates coroutines, marshals req/res, drives the
 * instruction-count hook, and cleans up middleware ctx.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

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

int hl_lua_dispatch(HlLua *lua, int handler_id,
                       KlRequest *req, KlResponse *res)
{
    if (!lua || !lua->L || !req || !res)
        return -1;

    /* dispatch_depth may be > 0 during self-fetch (outbox.flush → same server).
     * This is safe because the original handler is yielded and the new
     * dispatch runs on its own coroutine with independent per-request state. */
    lua->dispatch_depth++;

    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_guard_stale_txn(hl_db_registry_default(lua->base.db_registry));

    /* Re-arm instruction limit for this request */
    if (lua->max_instructions > 0)
        lua_sethook(lua->L, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(lua->max_instructions));

    /* Reset scratch arena for this request */
    sh_arena_reset(lua->scratch);

    /* Set per-request async context (for hull.sleep / http.get access) */
    lua->active_conn = kl_request_conn(req);
    lua->active_req = req;

    /* Get the handler function from the route registry */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_routes");
    if (!lua_istable(lua->L, -1)) {
        lua_pop(lua->L, 1);
        lua->active_conn = NULL;
        lua->active_req = NULL;
        return -1;
    }

    lua_rawgeti(lua->L, -1, handler_id);
    if (!lua_isfunction(lua->L, -1)) {
        lua_pop(lua->L, 2); /* pop function + routes table */
        lua->active_conn = NULL;
        lua->active_req = NULL;
        return -1;
    }

    /* Create coroutine for this handler invocation */
    lua_State *co = lua_newthread(lua->L);
    int thread_ref = luaL_ref(lua->L, LUA_REGISTRYINDEX);

    /* Move handler function from main state to coroutine */
    lua_xmove(lua->L, co, 1);

    /* Build request and response objects on the coroutine stack */
    hl_lua_make_request(co, req);
    hl_lua_make_response(co, res);

    /* Set coroutine state for async C functions */
    lua->active_co = co;
    lua->active_thread_ref = thread_ref;

    /* Re-arm instruction hook on coroutine */
    if (lua->max_instructions > 0)
        lua_sethook(co, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(lua->max_instructions));

    /* Resume coroutine: handler(req, res) */
    int nres = 0;
    int status = lua_resume(co, lua->L, 2, &nres);

    if (status == LUA_OK) {
        /* Synchronous completion — same as lua_pcall path */
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->active_req = NULL;
        lua->dispatch_depth--;

        /* Pop any return values and routes table */
        if (nres > 0)
            lua_settop(co, 0);
        lua_pop(lua->L, 1); /* pop routes table */

        /* Free ctx if middleware set it */
        if (req->ctx) {
            HlReqCtx *rctx = (HlReqCtx *)req->ctx;
            if (rctx->kind == HL_REQCTX_LUA_REF)
                luaL_unref(lua->L, LUA_REGISTRYINDEX, rctx->lua_ref);
            else if (rctx->kind == HL_REQCTX_JSON)
                hl_alloc_free(lua->base.alloc, rctx->json.data, rctx->json.len + 1);
            hl_alloc_free(lua->base.alloc, rctx, sizeof(HlReqCtx));
            req->ctx = NULL;
        }
        return 0;
    }

    if (status == LUA_YIELD) {
        /* Handler yielded — connection is suspended.
         * Don't clean up coroutine ref, don't free ctx.
         * dispatch_depth stays at 1 — decremented on async resume.
         * kl_async_suspend already removed client FD from event loop.
         * Routes table stays on main state stack — cleaned up on resume. */
        lua_pop(lua->L, 1); /* pop routes table */
        return 1; /* signal: handler suspended */
    }

    /* Error */
    log_error("[hull:c] lua handler error: %s",
              lua_tostring(co, -1));
    luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
    lua->active_thread_ref = LUA_NOREF;
    lua->active_co = NULL;
    lua->active_conn = NULL;
    lua->active_req = NULL;
    lua->dispatch_depth--;

    lua_pop(lua->L, 1); /* pop routes table */

    /* Free ctx if middleware set it */
    if (req->ctx) {
        HlReqCtx *rctx = (HlReqCtx *)req->ctx;
        if (rctx->kind == HL_REQCTX_LUA_REF)
            luaL_unref(lua->L, LUA_REGISTRYINDEX, rctx->lua_ref);
        else if (rctx->kind == HL_REQCTX_JSON)
            hl_alloc_free(lua->base.alloc, rctx->json.data, rctx->json.len + 1);
        hl_alloc_free(lua->base.alloc, rctx, sizeof(HlReqCtx));
        req->ctx = NULL;
    }
    return -1;
}

void hl_lua_keel_handler(KlRequest *req, KlResponse *res, void *user_data)
{
    HlLuaRoute *route = (HlLuaRoute *)user_data;
    int rc = hl_lua_dispatch(route->lua, route->handler_id, req, res);
    if (rc < 0) {
        /* Error — write 500 response */
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body_borrow(res, "Internal Server Error", 21);
    }
    /* rc == 1 → handler suspended, conn_process checks SUSPENDED state */
}

/* ── Middleware dispatch ────────────────────────────────────────────── */

int hl_lua_dispatch_middleware(HlLua *lua, int handler_id,
                               KlRequest *req, KlResponse *res)
{
    if (!lua || !lua->L || !req || !res)
        return -1;

    /* Guard: roll back any stale transaction left by a crashed handler */
    hl_db_guard_stale_txn(hl_db_registry_default(lua->base.db_registry));

    /* Re-arm instruction limit for this middleware call */
    if (lua->max_instructions > 0)
        lua_sethook(lua->L, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(lua->max_instructions));

    /* Reset scratch arena for this middleware call */
    sh_arena_reset(lua->scratch);

    /* Get the handler function from the route registry */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_routes");
    if (!lua_istable(lua->L, -1)) {
        lua_pop(lua->L, 1);
        return -1;
    }

    lua_rawgeti(lua->L, -1, handler_id);
    if (!lua_isfunction(lua->L, -1)) {
        lua_pop(lua->L, 2); /* pop function + routes table */
        return -1;
    }

    /* Build request and response objects */
    hl_lua_make_request(lua->L, req);
    hl_lua_make_response(lua->L, res);

    /* Save a reference to the req table in the registry so we can
     * read ctx after pcall (which consumes the arguments). */
    lua_pushvalue(lua->L, -2); /* copy req table */
    lua_setfield(lua->L, LUA_REGISTRYINDEX, "__hull_mw_req");

    /* Call handler(req, res) — expect 1 return value */
    if (lua_pcall(lua->L, 2, 1, 0) != LUA_OK) {
        log_error("[hull:c] lua middleware error: %s",
                  lua_tostring(lua->L, -1));
        lua_pop(lua->L, 1); /* pop error message */
        lua_pop(lua->L, 1); /* pop routes table */
        /* Clean up registry ref */
        lua_pushnil(lua->L);
        lua_setfield(lua->L, LUA_REGISTRYINDEX, "__hull_mw_req");
        return -1;
    }

    /* Capture return value: 0 = continue, non-zero = short-circuit */
    int result = 0;
    if (lua_isnumber(lua->L, -1))
        result = (int)lua_tointeger(lua->L, -1);
    else if (lua_isboolean(lua->L, -1))
        result = lua_toboolean(lua->L, -1) ? 1 : 0;
    lua_pop(lua->L, 1); /* pop return value */

    /* Store req.ctx as a Lua registry ref so the next middleware
     * or handler can retrieve the table directly (no JSON round-trip). */
    lua_checkstack(lua->L, 4);
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_mw_req");
    lua_getfield(lua->L, -1, "ctx");
    if (lua_istable(lua->L, -1)) {
        /* Free previous ctx if any */
        if (req->ctx) {
            HlReqCtx *old = (HlReqCtx *)req->ctx;
            if (old->kind == HL_REQCTX_LUA_REF)
                luaL_unref(lua->L, LUA_REGISTRYINDEX, old->lua_ref);
            else if (old->kind == HL_REQCTX_JSON)
                hl_alloc_free(lua->base.alloc, old->json.data, old->json.len + 1);
            hl_alloc_free(lua->base.alloc, old, sizeof(HlReqCtx));
            req->ctx = NULL;
        }
        /* Create registry ref to the ctx table */
        int ref = luaL_ref(lua->L, LUA_REGISTRYINDEX);
        HlReqCtx *rctx = hl_alloc_malloc(lua->base.alloc, sizeof(HlReqCtx));
        if (rctx) {
            rctx->kind = HL_REQCTX_LUA_REF;
            rctx->lua_ref = ref;
            req->ctx = rctx;
        } else {
            luaL_unref(lua->L, LUA_REGISTRYINDEX, ref);
        }
    } else {
        lua_pop(lua->L, 1); /* pop non-table ctx */
    }
    lua_pop(lua->L, 1); /* pop saved req table */

    /* Clean up registry ref */
    lua_pushnil(lua->L);
    lua_setfield(lua->L, LUA_REGISTRYINDEX, "__hull_mw_req");

    lua_pop(lua->L, 1); /* pop routes table */
    return result;
}

int hl_lua_keel_middleware(KlRequest *req, KlResponse *res, void *user_data)
{
    HlLuaRoute *ctx = (HlLuaRoute *)user_data;
    int rc = hl_lua_dispatch_middleware(ctx->lua, ctx->handler_id, req, res);
    if (rc < 0) {
        /* Middleware error — short-circuit with 500 */
        kl_response_status(res, 500);
        kl_response_header(res, "Content-Type", "text/plain");
        kl_response_body_borrow(res, "Internal Server Error", 21);
        return 1; /* short-circuit */
    }
    return rc;
}
