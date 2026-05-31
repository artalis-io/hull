/*
 * mod_ws_client.c — hull.web.ws-client module (outbound WebSocket connect)
 *
 * Exposes: ws.connect(url, handlers)
 *          client connection methods: conn:send / conn:send_binary / conn:close / conn:ping
 *
 * Outbound connections require a non-empty manifest hosts allowlist;
 * the check is enforced at ws.connect() call time by hl_http_check_host.
 *
 * Server-side WebSocket helpers live in mod_ws_server.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/http.h"
#include "hull/alloc.h"

#include <keel/keel.h>
#include <keel/websocket_client.h>
#include <keel/url.h>

#include "log.h"

#include <string.h>

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
    /* `lua` is read on lines further down; cppcheck's data-flow
     * loses track across the early luaL_error returns below. */
    /* cppcheck-suppress unreadVariable */
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

/* ── Module opener ─────────────────────────────────────────────────── */

static const luaL_Reg ws_client_funcs[] = {
    {"connect", lua_ws_connect},
    {NULL, NULL}
};

int luaopen_hull_ws_client(lua_State *L)
{
    hl_lua_ws_register_client_conn_mt(L);
    luaL_newlib(L, ws_client_funcs);
    return 1;
}
