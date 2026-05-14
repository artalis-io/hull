/*
 * ws.c — Lua WebSocket server callback trampolines
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

#include "mod_buffer.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>
#include <keel/websocket_server.h>

#include "log.h"

#include <limits.h>

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

    /* Create coroutine for this callback */
    lua->dispatch_depth++;

    lua_State *co = lua_newthread(lua->L);
    int thread_ref = luaL_ref(lua->L, LUA_REGISTRYINDEX);
    lua->active_co = co;
    lua->active_thread_ref = thread_ref;
    lua->active_conn = NULL; /* detached — no HTTP connection */
    lua->active_req = NULL;
    lua->active_timer = NULL;

    /* Push handler function */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_routes");
    lua_rawgeti(lua->L, -1, route->on_open_id);
    lua_xmove(lua->L, co, 1);
    lua_pop(lua->L, 1); /* pop __hull_routes */

    /* Push conn userdata */
    hl_lua_ws_push_conn(co, conn);

    /* Re-arm instruction limit */
    if (lua->max_instructions > 0) {
        lua_sethook(co, NULL, 0, 0);
        lua_sethook(co, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(lua->max_instructions));
    }

    int nres = 0;
    int status = lua_resume(co, lua->L, 1, &nres);

    if (status == LUA_OK) {
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->dispatch_depth--;
    } else if (status == LUA_YIELD) {
        /* Handler yielded — async op in flight (detached mode).
         * dispatch_depth stays elevated. */
    } else {
        const char *err = lua_tostring(co, -1);
        log_error("[hull:ws] on_open error: %s", err ? err : "unknown");
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->dispatch_depth--;
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

    lua->dispatch_depth++;

    lua_State *co = lua_newthread(lua->L);
    int thread_ref = luaL_ref(lua->L, LUA_REGISTRYINDEX);
    lua->active_co = co;
    lua->active_thread_ref = thread_ref;
    lua->active_conn = NULL; /* detached */
    lua->active_req = NULL;
    lua->active_timer = NULL;

    /* Push handler function */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_routes");
    lua_rawgeti(lua->L, -1, route->on_message_id);
    lua_xmove(lua->L, co, 1);
    lua_pop(lua->L, 1);

    /* Push conn userdata, message, is_binary */
    hl_lua_ws_push_conn(co, conn);
    lua_pushlstring(co, data, len);
    lua_pushboolean(co, is_binary);

    /* Re-arm instruction limit */
    if (lua->max_instructions > 0) {
        lua_sethook(co, NULL, 0, 0);
        lua_sethook(co, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(lua->max_instructions));
    }

    int nres = 0;
    int status = lua_resume(co, lua->L, 3, &nres);

    if (status == LUA_OK) {
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->dispatch_depth--;
    } else if (status == LUA_YIELD) {
        /* Async op in flight */
    } else {
        const char *err = lua_tostring(co, -1);
        log_error("[hull:ws] on_message error: %s", err ? err : "unknown");
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->dispatch_depth--;
    }
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

    if (route->on_close_id >= 0) {
        lua->dispatch_depth++;

        lua_State *co = lua_newthread(lua->L);
        int thread_ref = luaL_ref(lua->L, LUA_REGISTRYINDEX);
        lua->active_co = co;
        lua->active_thread_ref = thread_ref;
        lua->active_conn = NULL;
        lua->active_req = NULL;
        lua->active_timer = NULL;

        /* Push handler function */
        lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_routes");
        lua_rawgeti(lua->L, -1, route->on_close_id);
        lua_xmove(lua->L, co, 1);
        lua_pop(lua->L, 1);

        /* Push conn, code, reason */
        hl_lua_ws_push_conn(co, conn);
        lua_pushinteger(co, code);
        if (reason && reason_len > 0)
            lua_pushlstring(co, reason, reason_len);
        else
            lua_pushnil(co);

        if (lua->max_instructions > 0) {
            lua_sethook(co, NULL, 0, 0);
            lua_sethook(co, hl_lua_instruction_hook, LUA_MASKCOUNT,
                        INSTR_COUNT(lua->max_instructions));
        }

        int nres = 0;
        int status = lua_resume(co, lua->L, 3, &nres);

        if (status == LUA_OK) {
            luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
            lua->active_thread_ref = LUA_NOREF;
            lua->active_co = NULL;
            lua->dispatch_depth--;
        } else if (status == LUA_YIELD) {
            /* Async op in flight */
        } else {
            const char *err = lua_tostring(co, -1);
            log_error("[hull:ws] on_close error: %s", err ? err : "unknown");
            luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
            lua->active_thread_ref = LUA_NOREF;
            lua->active_co = NULL;
            lua->dispatch_depth--;
        }
    }

    /* Invalidate conn userdata and remove from registry */
    hl_lua_ws_invalidate_conn(lua->L, conn);
    hl_ws_registry_remove(lua->base.ws_registry, conn);
}
