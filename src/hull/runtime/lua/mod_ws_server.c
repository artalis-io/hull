/*
 * mod_ws_server.c - hull.web.ws-server module (server-side WebSocket helpers + conn userdata)
 *
 * Exposes: ws.broadcast(path, data [, binary])
 *          ws.connections(path)
 *          per-connection methods: conn:send / conn:send_binary / conn:close / conn:ping
 *          conn.data (per-connection table storage)
 *
 * Server-endpoint registration (app.ws) lives in mod_app.c; the C helpers
 * hl_lua_ws_push_conn / hl_lua_ws_invalidate_conn / hl_lua_ws_register_conn_mt
 * are declared in mod_buffer.h and called from runtime/lua/ws.c (Keel
 * callback dispatcher) and mod_app.c (install_app_ws_server).
 *
 * Client-side WebSocket (ws.connect, outbound) lives in mod_ws_client.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/ws.h"
#include "hull/utils/alloc.h"

#include <keel/keel.h>
#include <keel/websocket_server.h>

#include "log.h"

#include <string.h>

/* ── Metatable name ────────────────────────────────────────────────── */

#define HL_WS_CONN_MT "HlWsConn"

/* ── Conn userdata ─────────────────────────────────────────────────── */

typedef struct {
    HlWsConn *conn;      /* NULL after close */
    int       data_ref;  /* registry ref for conn.data table (LUA_NOREF = none) */
} HlWsConnUD;

/* Push or create the conn userdata for a given HlWsConn.
 * Uses the registry ref stored in conn->user_data. */
void hl_lua_ws_push_conn(lua_State *L, HlWsConn *conn)
{
    if (conn->user_data) {
        /* Already have a userdata - push it via registry ref */
        int ref = (int)(intptr_t)conn->user_data;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        return;
    }

    /* Create new userdata */
    HlWsConnUD *ud = (HlWsConnUD *)lua_newuserdata(L, sizeof(HlWsConnUD));
    ud->conn = conn;
    ud->data_ref = LUA_NOREF;

    luaL_setmetatable(L, HL_WS_CONN_MT);

    /* Store a registry ref to the userdata on the conn */
    lua_pushvalue(L, -1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    conn->user_data = (void *)(intptr_t)ref;
}

/* Invalidate and unref the conn userdata. Called from on_close trampoline. */
void hl_lua_ws_invalidate_conn(lua_State *L, HlWsConn *conn)
{
    if (!conn->user_data)
        return;

    int ref = (int)(intptr_t)conn->user_data;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (lua_isuserdata(L, -1)) {
        HlWsConnUD *ud = (HlWsConnUD *)lua_touserdata(L, -1);
        /* Free the data table ref */
        if (ud->data_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->data_ref);
            ud->data_ref = LUA_NOREF;
        }
        ud->conn = NULL;
    }
    lua_pop(L, 1);

    /* Unref the userdata itself */
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    conn->user_data = NULL;
}

/* ── Conn methods ──────────────────────────────────────────────────── */

static int lua_ws_conn_id(lua_State *L)
{
    HlWsConnUD *ud = (HlWsConnUD *)luaL_checkudata(L, 1, HL_WS_CONN_MT);
    if (!ud->conn)
        return luaL_error(L, "WebSocket connection is closed");
    lua_pushinteger(L, (lua_Integer)ud->conn->id);
    return 1;
}

static int lua_ws_conn_path(lua_State *L)
{
    HlWsConnUD *ud = (HlWsConnUD *)luaL_checkudata(L, 1, HL_WS_CONN_MT);
    if (!ud->conn)
        return luaL_error(L, "WebSocket connection is closed");
    lua_pushstring(L, ud->conn->path);
    return 1;
}

static int lua_ws_conn_send(lua_State *L)
{
    HlWsConnUD *ud = (HlWsConnUD *)luaL_checkudata(L, 1, HL_WS_CONN_MT);
    if (!ud->conn || ud->conn->closed || !ud->conn->kl_ws) {
        lua_pushnil(L);
        lua_pushstring(L, "connection closed");
        return 2;
    }

    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    int rc = kl_ws_server_send_text(ud->conn->kl_ws, data, len);
    if (rc == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "send failed");
    return 2;
}

static int lua_ws_conn_send_binary(lua_State *L)
{
    HlWsConnUD *ud = (HlWsConnUD *)luaL_checkudata(L, 1, HL_WS_CONN_MT);
    if (!ud->conn || ud->conn->closed || !ud->conn->kl_ws) {
        lua_pushnil(L);
        lua_pushstring(L, "connection closed");
        return 2;
    }

    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    int rc = kl_ws_server_send_binary(ud->conn->kl_ws, data, len);
    if (rc == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "send failed");
    return 2;
}

static int lua_ws_conn_close(lua_State *L)
{
    HlWsConnUD *ud = (HlWsConnUD *)luaL_checkudata(L, 1, HL_WS_CONN_MT);
    if (!ud->conn || ud->conn->closed || !ud->conn->kl_ws)
        return 0;

    uint16_t code = (uint16_t)luaL_optinteger(L, 2, 1000);
    size_t reason_len = 0;
    const char *reason = luaL_optlstring(L, 3, NULL, &reason_len);

    kl_ws_server_close(ud->conn->kl_ws, code, reason, reason_len);
    ud->conn->closed = 1;
    return 0;
}

static int lua_ws_conn_ping(lua_State *L)
{
    HlWsConnUD *ud = (HlWsConnUD *)luaL_checkudata(L, 1, HL_WS_CONN_MT);
    if (!ud->conn || ud->conn->closed || !ud->conn->kl_ws)
        return 0;

    size_t len = 0;
    const char *data = luaL_optlstring(L, 2, NULL, &len);

    kl_ws_server_send_ping(ud->conn->kl_ws, data, len);
    return 0;
}

/* conn.data - lazy-created per-connection table */
static int lua_ws_conn_index(lua_State *L)
{
    HlWsConnUD *ud = (HlWsConnUD *)luaL_checkudata(L, 1, HL_WS_CONN_MT);
    const char *key = luaL_checkstring(L, 2);

    if (strcmp(key, "data") == 0) {
        if (ud->data_ref == LUA_NOREF) {
            /* Create lazy data table */
            lua_newtable(L);
            lua_pushvalue(L, -1);
            ud->data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ud->data_ref);
        }
        return 1;
    }

    /* Fall through to metatable methods */
    luaL_getmetatable(L, HL_WS_CONN_MT);
    lua_getfield(L, -1, key);
    return 1;
}

static int lua_ws_conn_newindex(lua_State *L)
{
    HlWsConnUD *ud = (HlWsConnUD *)luaL_checkudata(L, 1, HL_WS_CONN_MT);
    const char *key = luaL_checkstring(L, 2);

    if (strcmp(key, "data") == 0) {
        luaL_checktype(L, 3, LUA_TTABLE);
        /* Free old ref */
        if (ud->data_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, ud->data_ref);
        lua_pushvalue(L, 3);
        ud->data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }

    return luaL_error(L, "cannot set field '%s' on WebSocket connection", key);
}

static const luaL_Reg ws_conn_methods[] = {
    {"id",          lua_ws_conn_id},
    {"path",        lua_ws_conn_path},
    {"send",        lua_ws_conn_send},
    {"send_binary", lua_ws_conn_send_binary},
    {"close",       lua_ws_conn_close},
    {"ping",        lua_ws_conn_ping},
    {NULL, NULL}
};

void hl_lua_ws_register_conn_mt(lua_State *L)
{
    luaL_newmetatable(L, HL_WS_CONN_MT);

    /* Store methods in the metatable itself (accessed via __index fallback) */
    luaL_setfuncs(L, ws_conn_methods, 0);

    /* __index: first check "data" key, then methods */
    lua_pushcfunction(L, lua_ws_conn_index);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lua_ws_conn_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pop(L, 1); /* pop metatable */
}


/* ── Module functions ──────────────────────────────────────────────── */

static int lua_ws_broadcast(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    HlWsRegistry *reg = lua->base.ws_registry;
    if (!reg) {
        lua_pushinteger(L, 0);
        return 1;
    }

    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    int binary = lua_toboolean(L, 3);

    int sent;
    if (binary)
        sent = hl_ws_broadcast_binary(reg, path, data, len);
    else
        sent = hl_ws_broadcast_text(reg, path, data, len);

    lua_pushinteger(L, sent);
    return 1;
}

static int lua_ws_connections(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    HlWsRegistry *reg = lua->base.ws_registry;
    if (!reg) {
        lua_pushinteger(L, 0);
        return 1;
    }

    const char *path = luaL_checkstring(L, 1);
    size_t count = hl_ws_connections(reg, path);
    lua_pushinteger(L, (lua_Integer)count);
    return 1;
}

/* Split into server-side and client-side reg tables. The single
 * old hull/ws@1 module is replaced by hull/web/ws-server@1 (broadcast,
 * connections, plus app.ws decoration) and hull/web/ws-client@1
 * (connect, gated on hosts allowlist at call time). */

/* ── Module opener ─────────────────────────────────────────────────── */

static const luaL_Reg ws_server_funcs[] = {
    {"broadcast",   lua_ws_broadcast},
    {"connections", lua_ws_connections},
    {NULL, NULL}
};

int luaopen_hull_ws_server(lua_State *L)
{
    hl_lua_ws_register_conn_mt(L);
    luaL_newlib(L, ws_server_funcs);
    return 1;
}
