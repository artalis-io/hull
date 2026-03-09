/*
 * lua_async.c — Lua async continuation + hull.sleep()
 *
 * Implements HlLuaAsyncCont (the Lua-specific HlAsyncCont vtable) and
 * the hull.sleep() yielding C function.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "hull/async.h"
#include "hull/alloc.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>

#include "log.h"

/* ── HlLuaAsyncCont ───────────────────────────────────────────────── */

typedef struct HlLuaAsyncCont {
    HlAsyncCont  base;     /* vtable — must be first member */
    HlLua       *lua;      /* runtime instance */
    HlAllocator *alloc;
} HlLuaAsyncCont;

/*
 * Resume the Lua handler by calling lua_resume on the active coroutine.
 * Sets conn->state based on the result:
 *   LUA_OK    → KL_CONN_SENDING (handler completed)
 *   LUA_YIELD → KL_CONN_SUSPENDED (handler re-yielded, new op active)
 *   error     → KL_CONN_SENDING (500 response written)
 */
static void lua_push_async_http_response(lua_State *L, void *driver);

static void hl_lua_async_resume(HlAsyncCont *self, void *driver)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)self;
    HlLua *lua = lc->lua;
    KlConn *conn = lua->active_conn;

    if (!lua->active_co || !conn) return;

    /* Push driver result onto the coroutine stack so lua_resume
     * delivers it as the return value of the yield point */
    int nargs = 0;
    if (driver) {
        lua_push_async_http_response(lua->active_co, driver);
        nargs = 1;
    }

    int nres = 0;
    int status = lua_resume(lua->active_co, lua->L, nargs, &nres);

    if (status == LUA_OK) {
        /* Handler completed — response is ready */
        luaL_unref(lua->L, LUA_REGISTRYINDEX, lua->active_thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;

        /* Pop any return values from coroutine */
        if (nres > 0)
            lua_pop(lua->active_co ? lua->active_co : lua->L, nres);

        /* Check for streaming mode */
        if (conn->res.body_mode == KL_BODY_STREAM) {
            conn->state = KL_CONN_CLOSED;
        } else {
            conn->state = KL_CONN_SENDING;
        }
    } else if (status == LUA_YIELD) {
        /* Handler yielded again — new HlAsyncCtx already set up.
         * conn->state was set to KL_CONN_SUSPENDED by kl_async_suspend.
         * Don't touch the coroutine ref — it's still in use. */
    } else {
        /* Error */
        const char *msg = lua_tostring(lua->active_co, -1);
        log_error("[hull:c] async lua handler error: %s", msg ? msg : "(unknown)");

        luaL_unref(lua->L, LUA_REGISTRYINDEX, lua->active_thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;

        kl_response_status(&conn->res, 500);
        kl_response_header(&conn->res, "Content-Type", "text/plain");
        kl_response_body(&conn->res, "Internal Server Error", 21);
        conn->state = KL_CONN_SENDING;
    }
}

/*
 * Cancel the Lua handler — free the coroutine registry ref without
 * invoking the handler.
 */
static void hl_lua_async_cancel(HlAsyncCont *self)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)self;
    HlLua *lua = lc->lua;

    if (lua->active_thread_ref != LUA_NOREF) {
        luaL_unref(lua->L, LUA_REGISTRYINDEX, lua->active_thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
    }
    lua->active_conn = NULL;
}

/*
 * Destroy the cont struct. Does NOT free the coroutine ref — that's
 * managed by the resume/cancel functions above.
 */
static void hl_lua_async_destroy(HlAsyncCont *self)
{
    HlLuaAsyncCont *lc = (HlLuaAsyncCont *)self;
    hl_alloc_free(lc->alloc, lc, sizeof(HlLuaAsyncCont));
}

/*
 * Create a Lua async continuation.
 */
HlAsyncCont *hl_lua_async_cont_create(HlLua *lua, HlAllocator *alloc)
{
    HlLuaAsyncCont *lc = hl_alloc_malloc(alloc, sizeof(HlLuaAsyncCont));
    if (!lc) return NULL;

    lc->base.resume  = hl_lua_async_resume;
    lc->base.cancel  = hl_lua_async_cancel;
    lc->base.destroy = hl_lua_async_destroy;
    lc->lua   = lua;
    lc->alloc = alloc;

    return &lc->base;
}

/* ── Push async HTTP response onto Lua stack ──────────────────────── */

#include "hull/cap/http_async.h"

static void lua_push_async_http_response(lua_State *L, void *driver)
{
    HlHttpClient *client = (HlHttpClient *)driver;
    HlHttpResponse *resp = &client->resp;

    lua_newtable(L);

    lua_pushinteger(L, resp->status);
    lua_setfield(L, -2, "status");

    if (resp->body && resp->body_len > 0)
        lua_pushlstring(L, resp->body, resp->body_len);
    else
        lua_pushstring(L, "");
    lua_setfield(L, -2, "body");

    /* Headers as { ["name"] = "value" } — lowercase names */
    lua_newtable(L);
    for (int i = 0; i < resp->num_headers; i++) {
        const char *name = resp->headers[i].name;
        size_t nlen = strlen(name);
        /* Lowercase in-place on stack via luaL_Buffer */
        luaL_Buffer buf;
        luaL_buffinit(L, &buf);
        for (size_t j = 0; j < nlen; j++) {
            char ch = (name[j] >= 'A' && name[j] <= 'Z')
                        ? (char)(name[j] + 32) : name[j];
            luaL_addchar(&buf, ch);
        }
        luaL_pushresult(&buf);
        lua_pushstring(L, resp->headers[i].value);
        lua_settable(L, -3);
    }
    lua_setfield(L, -2, "headers");
}

/* ── hull.sleep(ms) ───────────────────────────────────────────────── */

/*
 * hull.sleep(ms) — yield the handler for `ms` milliseconds.
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

    if (!lua || !lua->server || !lua->active_conn)
        return luaL_error(L, "hull.sleep() can only be called from a request handler");

    KlServer *server = lua->server;
    KlConn *conn = lua->active_conn;

    /* Create async ctx */
    HlAsyncCtx *ctx = hl_async_ctx_create(server, lua->base.alloc);
    if (!ctx)
        return luaL_error(L, "hull.sleep(): out of memory");

    /* Create Lua continuation */
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc);
    if (!cont) {
        hl_async_ctx_free(ctx);
        return luaL_error(L, "hull.sleep(): out of memory");
    }
    ctx->cont = cont;

    /* Set up sleep op (deadline-only, no driver) */
    ctx->op.deadline_ms = kl_monotonic_ms() + (uint64_t)ms;
    ctx->op.on_deadline = hl_async_on_deadline_sleep;
    ctx->driver = NULL;
    ctx->free_driver = NULL;

    /* Suspend the connection */
    if (kl_async_suspend(server, conn, &ctx->op) < 0) {
        ctx->cont->destroy(ctx->cont);
        hl_async_ctx_free(ctx);
        return luaL_error(L, "hull.sleep(): failed to suspend connection");
    }

    /* Yield the coroutine — no continuation function needed for sleep.
     * When resumed, Lua continues from after hull.sleep() in the handler. */
    return lua_yieldk(L, 0, 0, NULL);
}

/* ── Module registration ──────────────────────────────────────────── */

static const luaL_Reg hull_funcs[] = {
    {"sleep", lua_hull_sleep},
    {NULL, NULL}
};

int luaopen_hull_hull(lua_State *L)
{
    luaL_newlib(L, hull_funcs);
    return 1;
}
