/*
 * mod_ws.c — hull.ws module + WebSocket conn userdata (Lua)
 *
 * Provides:
 *   - ws.broadcast(path, data [, binary])
 *   - ws.connections(path)
 *   - conn:send(text) / conn:send_binary(data) / conn:close() / conn:ping()
 *   - conn:id() / conn:path()
 *   - conn.data table (per-connection storage)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/ws.h"
#include "hull/cap/http.h"
#include "hull/alloc.h"

#include <keel/keel.h>
#include <keel/websocket_server.h>
#include <keel/websocket_client.h>
#include <keel/url.h>

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
        /* Already have a userdata — push it via registry ref */
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

/* conn.data — lazy-created per-connection table */
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

/* ════════════════════════════════════════════════════════════════════
 * Client WebSocket conn
 * ════════════════════════════════════════════════════════════════════ */

#define HL_WS_CLIENT_CONN_MT "HlWsClientConn"

typedef struct HlLuaWsClientUD {
    KlWsClientConn *client;      /* NULL after free */
    int             on_open_ref;
    int             on_message_ref;
    int             on_close_ref;
    int             on_error_ref;
    int             self_ref;    /* registry ref to keep self alive */
    lua_State      *L;           /* main thread */
    HlLua          *lua;
} HlLuaWsClientUD;

/* Client conn methods */

static int lua_ws_client_send(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    if (!ud->client)
        return luaL_error(L, "client connection closed");

    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    int rc = kl_ws_client_send_text(ud->client, data, len);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int lua_ws_client_send_binary(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    if (!ud->client)
        return luaL_error(L, "client connection closed");

    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    int rc = kl_ws_client_send_binary(ud->client, data, len);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int lua_ws_client_close(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    if (!ud->client)
        return 0;

    uint16_t code = (uint16_t)luaL_optinteger(L, 2, 1000);
    size_t reason_len = 0;
    const char *reason = luaL_optlstring(L, 3, NULL, &reason_len);

    kl_ws_client_close(ud->client, code, reason, reason_len);
    return 0;
}

static int lua_ws_client_ping(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    if (!ud->client)
        return 0;

    size_t len = 0;
    const char *data = luaL_optlstring(L, 2, NULL, &len);
    kl_ws_client_send_ping(ud->client, data, len);
    return 0;
}

static int lua_ws_client_gc(lua_State *L)
{
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)luaL_checkudata(L, 1,
                                                                HL_WS_CLIENT_CONN_MT);
    if (ud->client) {
        kl_ws_client_free(ud->client);
        ud->client = NULL;
    }
    if (ud->on_open_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, ud->on_open_ref);
    if (ud->on_message_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, ud->on_message_ref);
    if (ud->on_close_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, ud->on_close_ref);
    if (ud->on_error_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, ud->on_error_ref);
    if (ud->self_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, ud->self_ref);
    return 0;
}

static const luaL_Reg ws_client_conn_methods[] = {
    {"send",        lua_ws_client_send},
    {"send_binary", lua_ws_client_send_binary},
    {"close",       lua_ws_client_close},
    {"ping",        lua_ws_client_ping},
    {"__gc",        lua_ws_client_gc},
    {NULL, NULL}
};

static void hl_lua_ws_register_client_conn_mt(lua_State *L)
{
    luaL_newmetatable(L, HL_WS_CLIENT_CONN_MT);
    luaL_setfuncs(L, ws_client_conn_methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

/* Client callbacks */

static void lua_ws_client_on_open(KlWsClientConn *ws, void *user_data)
{
    (void)ws;
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    if (ud->on_open_ref == LUA_NOREF)
        return;

    lua_rawgeti(ud->L, LUA_REGISTRYINDEX, ud->on_open_ref);
    lua_rawgeti(ud->L, LUA_REGISTRYINDEX, ud->self_ref);
    if (lua_pcall(ud->L, 1, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(ud->L, -1);
        log_error("[hull:ws:client] on_open error: %s", err ? err : "unknown");
        lua_pop(ud->L, 1);
    }
}

static void lua_ws_client_on_message(KlWsClientConn *ws, const char *data,
                                       size_t len, int is_binary,
                                       void *user_data)
{
    (void)ws;
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    if (ud->on_message_ref == LUA_NOREF)
        return;

    lua_rawgeti(ud->L, LUA_REGISTRYINDEX, ud->on_message_ref);
    lua_rawgeti(ud->L, LUA_REGISTRYINDEX, ud->self_ref);
    lua_pushlstring(ud->L, data, len);
    lua_pushboolean(ud->L, is_binary);
    if (lua_pcall(ud->L, 3, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(ud->L, -1);
        log_error("[hull:ws:client] on_message error: %s",
                  err ? err : "unknown");
        lua_pop(ud->L, 1);
    }
}

static void lua_ws_client_on_close(KlWsClientConn *ws, uint16_t code,
                                     const char *reason, size_t reason_len,
                                     void *user_data)
{
    (void)ws;
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;

    if (ud->on_close_ref != LUA_NOREF) {
        lua_rawgeti(ud->L, LUA_REGISTRYINDEX, ud->on_close_ref);
        lua_rawgeti(ud->L, LUA_REGISTRYINDEX, ud->self_ref);
        lua_pushinteger(ud->L, code);
        if (reason && reason_len > 0)
            lua_pushlstring(ud->L, reason, reason_len);
        else
            lua_pushnil(ud->L);
        if (lua_pcall(ud->L, 3, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(ud->L, -1);
            log_error("[hull:ws:client] on_close error: %s",
                      err ? err : "unknown");
            lua_pop(ud->L, 1);
        }
    }

    /* Release self-ref — allow GC */
    if (ud->self_ref != LUA_NOREF) {
        luaL_unref(ud->L, LUA_REGISTRYINDEX, ud->self_ref);
        ud->self_ref = LUA_NOREF;
    }
    ud->client = NULL;
}

static void lua_ws_client_on_error(KlWsClientConn *ws, const char *msg,
                                     void *user_data)
{
    (void)ws;
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)user_data;
    if (ud->on_error_ref == LUA_NOREF) {
        log_error("[hull:ws:client] error: %s", msg ? msg : "unknown");
        return;
    }

    lua_rawgeti(ud->L, LUA_REGISTRYINDEX, ud->on_error_ref);
    lua_rawgeti(ud->L, LUA_REGISTRYINDEX, ud->self_ref);
    lua_pushstring(ud->L, msg ? msg : "unknown");
    if (lua_pcall(ud->L, 2, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(ud->L, -1);
        log_error("[hull:ws:client] on_error callback error: %s",
                  err ? err : "unknown");
        lua_pop(ud->L, 1);
    }
}

/* ws.connect(url, handlers [, opts]) */
static int lua_ws_connect(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);

    const char *url = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    /* Check host allowlist */
    KlUrl parsed;
    if (kl_url_parse(url, &parsed) != 0)
        return luaL_error(L, "invalid WebSocket URL");

#ifdef HL_ENABLE_HTTP_CLIENT
    if (lua->base.http_cfg) {
        if (hl_http_check_host(lua->base.http_cfg, parsed.host,
                                parsed.host_len) != 0)
            return luaL_error(L, "host not in allowlist");
    }
#else
    return luaL_error(L,
        "ws.connect requires HL_ENABLE_HTTP_CLIENT (build-time)");
#endif

    if (!lua->server)
        return luaL_error(L, "ws.connect requires running server");

    /* Create userdata */
    HlLuaWsClientUD *ud = (HlLuaWsClientUD *)lua_newuserdata(L,
                                                                sizeof(HlLuaWsClientUD));
    ud->client = NULL;
    ud->on_open_ref = LUA_NOREF;
    ud->on_message_ref = LUA_NOREF;
    ud->on_close_ref = LUA_NOREF;
    ud->on_error_ref = LUA_NOREF;
    ud->self_ref = LUA_NOREF;
    ud->L = lua->L; /* use main thread for callbacks */
    ud->lua = lua;

    luaL_setmetatable(L, HL_WS_CLIENT_CONN_MT);

    /* Extract callbacks */
    lua_getfield(L, 2, "on_open");
    if (lua_isfunction(L, -1))
        ud->on_open_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    lua_getfield(L, 2, "on_message");
    if (lua_isfunction(L, -1))
        ud->on_message_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    lua_getfield(L, 2, "on_close");
    if (lua_isfunction(L, -1))
        ud->on_close_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    lua_getfield(L, 2, "on_error");
    if (lua_isfunction(L, -1))
        ud->on_error_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    /* Store self-ref to prevent GC while connected */
    lua_pushvalue(L, -1); /* dup userdata */
    ud->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* Connect */
    KlWsClientCallbacks cbs = {
        .on_open = lua_ws_client_on_open,
        .on_message = lua_ws_client_on_message,
        .on_close = lua_ws_client_on_close,
        .on_error = lua_ws_client_on_error,
    };

    KlWsClientConn *client = kl_ws_client_connect(
        &lua->server->ev, &lua->server->alloc_storage, NULL, url, &cbs, ud);

    if (!client) {
        /* Clean up self_ref */
        luaL_unref(L, LUA_REGISTRYINDEX, ud->self_ref);
        ud->self_ref = LUA_NOREF;
        return luaL_error(L, "WebSocket connect failed");
    }

    ud->client = client;

    return 1; /* return userdata on stack */
}

/* ── ws module functions ───────────────────────────────────────────── */

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
 * old hull/ws@1 module is replaced by hull/ws-server@1 (broadcast,
 * connections, plus app.ws decoration) and hull/ws-client@1
 * (connect, gated on hosts allowlist at call time). */
static const luaL_Reg ws_server_funcs[] = {
    {"broadcast",   lua_ws_broadcast},
    {"connections", lua_ws_connections},
    {NULL, NULL}
};

static const luaL_Reg ws_client_funcs[] = {
    {"connect",     lua_ws_connect},
    {NULL, NULL}
};

/* Shared init for metatables — both openers need these registered
 * exactly once per Lua state. Idempotent because luaL_newmetatable
 * checks before creating. */
static void hl_lua_ws_register_metatables(lua_State *L);

int luaopen_hull_ws_server(lua_State *L)
{
    hl_lua_ws_register_metatables(L);
    luaL_newlib(L, ws_server_funcs);
    return 1;
}

int luaopen_hull_ws_client(lua_State *L)
{
    hl_lua_ws_register_metatables(L);
    luaL_newlib(L, ws_client_funcs);
    return 1;
}

static void hl_lua_ws_register_metatables(lua_State *L)
{
    /* Both connection metatables — idempotent across multiple
     * luaopen_hull_ws_{server,client} calls. */
    hl_lua_ws_register_conn_mt(L);
    hl_lua_ws_register_client_conn_mt(L);
}
