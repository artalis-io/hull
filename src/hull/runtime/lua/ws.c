/*
 * ws.c - Lua WebSocket server callback trampolines
 *
 * Maps Keel's per-connection WebSocket callbacks (on_open/on_message/
 * on_close) to Lua handler functions registered via `app.ws()`. Each
 * trampoline runs on its own coroutine in detached mode (no HTTP req),
 * routes through the WS registry to bind a conn userdata, and supports
 * async yield.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/cap/ws.h"
#include "hull/cap/db_registry.h"   /* hl_db_registry_guard_stale_txns */

#include "mod_buffer.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>
#include <keel/websocket_server.h>

#include "log.h"

#include <limits.h>

/* Arguments for a ws callback, pushed under the protected entry
 * (hl_lua_entry_prepare): the conn, then - for a message - the bytes and the
 * binary flag, or - for a close - the code and the reason. The message is
 * the peer's, of the peer's size: pushed unprotected, a memory error there
 * aborted the process. */
typedef struct {
    HlWsConn   *conn;
    int         kind;        /* 0 open, 1 message, 2 close */
    const char *data;
    size_t      len;
    int         is_binary;
    uint16_t    code;
} HlWsArgs;

static int push_ws_args(lua_State *L, void *ud)
{
    HlWsArgs *a = (HlWsArgs *)ud;
    hl_lua_ws_push_conn(L, a->conn);
    if (a->kind == 1) {
        lua_pushlstring(L, a->data ? a->data : "", a->data ? a->len : 0);
        lua_pushboolean(L, a->is_binary);
        return 3;
    }
    if (a->kind == 2) {
        lua_pushinteger(L, a->code);
        if (a->data && a->len > 0)
            lua_pushlstring(L, a->data, a->len);
        else
            lua_pushnil(L);
        return 3;
    }
    return 1;
}

void hl_lua_ws_on_open(KlWsServerConn *ws_conn, void *user_data)
{
    HlLuaWsRoute *route = (HlLuaWsRoute *)user_data;
    HlLua *lua = route->lua;

    /* Register the connection in the registry */
    HlWsConn *conn = hl_ws_registry_add(lua->base.ws_registry,
                                          route->path, ws_conn);
    if (!conn)
        return;

    if (route->on_open_id < 0)
        return;

    /* The coroutine with on_open(conn) on it, built protected. */
    HlWsArgs args = { conn, 0, NULL, 0, 0, 0 };
    int thread_ref = LUA_NOREF, nargs = 0;
    lua_State *co = hl_lua_entry_prepare(lua, "__hull_routes", route->on_open_id,
                                         push_ws_args, &args, &thread_ref, &nargs);
    if (!co)
        return;
    lua->active_co = co;
    lua->active_thread_ref = thread_ref;
    lua->active_conn = NULL; /* detached - no HTTP connection */
    lua->active_req = NULL;
    lua->active_timer = NULL;

    hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: a stale txn must not be joined */
    /* Arm the instruction budget for this run */
    HL_LUA_ARM(lua, co);

    int nres = 0;
    int status = lua_resume(co, lua->L, nargs, &nres);
    status = hl_lua_resume_status(co, status);
    hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: any open txn is stale now */

    if (status == LUA_OK) {
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
    } else if (status == LUA_YIELD) {
        /* Handler yielded - async op in flight (detached mode). */
    } else {
        char ebuf[512];
        log_error("[hull:ws] on_open error: %s",
                  hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf)));
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
    }
}

void hl_lua_ws_on_message(KlWsServerConn *ws_conn, const char *data,
                                   size_t len, int is_binary, void *user_data)
{
    HlLuaWsRoute *route = (HlLuaWsRoute *)user_data;
    HlLua *lua = route->lua;

    if (route->on_message_id < 0)
        return;

    /* Find the HlWsConn for this KlWsServerConn */
    HlWsConn *conn = hl_ws_registry_find(lua->base.ws_registry,
                                           route->path, ws_conn);
    if (!conn)
        return;


    /* The coroutine with on_message(conn, data, is_binary), protected. */
    HlWsArgs args = { conn, 1, data, len, is_binary, 0 };
    int thread_ref = LUA_NOREF, nargs = 0;
    lua_State *co = hl_lua_entry_prepare(lua, "__hull_routes", route->on_message_id,
                                         push_ws_args, &args, &thread_ref, &nargs);
    if (!co)
        return;
    lua->active_co = co;
    lua->active_thread_ref = thread_ref;
    lua->active_conn = NULL; /* detached */
    lua->active_req = NULL;
    lua->active_timer = NULL;

    hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: a stale txn must not be joined */
    /* Arm the instruction budget for this run */
    HL_LUA_ARM(lua, co);

    int nres = 0;
    int status = lua_resume(co, lua->L, nargs, &nres);
    status = hl_lua_resume_status(co, status);
    hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: any open txn is stale now */

    if (status == LUA_OK) {
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
    } else if (status == LUA_YIELD) {
        /* Async op in flight */
    } else {
        char ebuf[512];
        log_error("[hull:ws] on_message error: %s",
                  hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf)));
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
    }
}

/* Deferred-teardown hook (HlLua::active_on_complete): invalidate the conn
 * userdata and remove it from the registry once an async on_close handler
 * has finally completed. Runs from hl_lua_async_resume's completion path. */
static void hl_lua_ws_close_teardown(HlLua *lua, void *ctx)
{
    HlWsConn *conn = (HlWsConn *)ctx;
    hl_lua_ws_invalidate_conn(lua->L, conn);
    hl_ws_registry_remove(lua->base.ws_registry, conn);
}

void hl_lua_ws_on_close(KlWsServerConn *ws_conn, uint16_t code,
                                 const char *reason, size_t reason_len,
                                 void *user_data)
{
    HlLuaWsRoute *route = (HlLuaWsRoute *)user_data;
    HlLua *lua = route->lua;

    /* Find the HlWsConn */
    HlWsConn *conn = hl_ws_registry_find(lua->base.ws_registry,
                                           route->path, ws_conn);
    if (!conn)
        return;

    conn->closed = 1;

    HlWsArgs args = { conn, 2, reason, reason_len, 0, code };
    int thread_ref = LUA_NOREF, nargs = 0;
    lua_State *co = route->on_close_id >= 0
        ? hl_lua_entry_prepare(lua, "__hull_routes", route->on_close_id,
                               push_ws_args, &args, &thread_ref, &nargs)
        : NULL;
    if (co) {
        lua->active_co = co;
        lua->active_thread_ref = thread_ref;
        lua->active_conn = NULL;
        lua->active_req = NULL;
        lua->active_timer = NULL;

        hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: a stale txn must not be joined */
        HL_LUA_ARM(lua, co);

        /* Arm the deferred-teardown hook: if the handler yields (async op),
         * hl_lua_async_cont_create captures it so the teardown below runs
         * only once the async handler completes, not while it is still
         * suspended and holding the conn. */
        lua->active_on_complete     = hl_lua_ws_close_teardown;
        lua->active_on_complete_ctx = conn;

        int nres = 0;
        int status = lua_resume(co, lua->L, nargs, &nres);
        status = hl_lua_resume_status(co, status);
        hl_db_registry_guard_stale_txns(lua->base.db_registry);  /* audit 6 M1: any open txn is stale now */

        lua->active_on_complete     = NULL;
        lua->active_on_complete_ctx = NULL;

        if (status == LUA_OK) {
            luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
            lua->active_thread_ref = LUA_NOREF;
            lua->active_co = NULL;
        } else if (status == LUA_YIELD) {
            /* Async op in flight - the continuation captured `conn`; the
             * teardown is deferred to hl_lua_async_resume's completion. Do
             * NOT invalidate/remove the conn here while the handler still
             * references it. */
            return;
        } else {
            char ebuf[512];
            log_error("[hull:ws] on_close error: %s",
                      hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf)));
            luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
            lua->active_thread_ref = LUA_NOREF;
            lua->active_co = NULL;
        }
    }

    /* Invalidate conn userdata and remove from registry (sync completion,
     * handler error, or no on_close handler). The async-yield path defers
     * this to async-handler completion via active_ws_close_conn. */
    hl_lua_ws_invalidate_conn(lua->L, conn);
    hl_ws_registry_remove(lua->base.ws_registry, conn);
}
