/*
 * lua_bindings.c — Request/Response bridge to Lua 5.4
 *
 * Marshals Keel's KlRequest/KlResponse to Lua tables/userdata.
 * This file contains ONLY data marshaling — all enforcement logic
 * lives in hl_cap_* functions.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/lua.h"
#include "hull/alloc.h"
#include "hull/limits.h"
#include "hull/cap.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/request.h>
#include <keel/response.h>
#include <keel/router.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Response metatable name ────────────────────────────────────────── */

#define HL_RESPONSE_MT "HlResponse"

/* ── Helper: retrieve HlLua from Lua registry ────────────────────── */

static HlLua *get_hl_lua_from_L(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return lua;
}

/*
 * Copy body data into runtime-owned buffer so it survives until
 * Keel sends the response (kl_response_body borrows the pointer).
 */
static const char *hl_lua_stash_body(lua_State *L, const char *data,
                                        size_t len)
{
    HlLua *hlua = get_hl_lua_from_L(L);
    if (!hlua)
        return NULL;
    if (hlua->response_body) {
        hl_alloc_free(hlua->alloc, hlua->response_body,
                      hlua->response_body_size);
        hlua->response_body = NULL;
        hlua->response_body_size = 0;
    }
    hlua->response_body = hl_alloc_malloc(hlua->alloc, len + 1);
    if (!hlua->response_body)
        return NULL;
    hlua->response_body_size = len + 1;
    memcpy(hlua->response_body, data, len);
    hlua->response_body[len] = '\0';
    return hlua->response_body;
}

/* ── Request object ─────────────────────────────────────────────────── */

/*
 * Push a Lua table representing the HTTP request:
 *   {
 *     method  = "GET",
 *     path    = "/invoices/42",
 *     params  = { id = "42" },
 *     query   = { limit = "10" },
 *     headers = { ["content-type"] = "application/json" },
 *     body    = "..." or nil,
 *     ctx     = {}
 *   }
 */
void hl_lua_make_request(lua_State *L, KlRequest *req)
{
    lua_newtable(L);

    /* method (Keel stores as string) */
    if (req->method)
        lua_pushlstring(L, req->method, req->method_len);
    else
        lua_pushstring(L, "GET");
    lua_setfield(L, -2, "method");

    /* path */
    if (req->path)
        lua_pushlstring(L, req->path, req->path_len);
    else
        lua_pushstring(L, "/");
    lua_setfield(L, -2, "path");

    /* query string → table */
    lua_newtable(L);
    if (req->query && req->query_len > 0) {
        char qbuf[HL_QUERY_BUF_SIZE];
        size_t qlen = req->query_len < sizeof(qbuf) - 1
                      ? req->query_len : sizeof(qbuf) - 1;
        memcpy(qbuf, req->query, qlen);
        qbuf[qlen] = '\0';

        char *saveptr = NULL;
        char *pair = strtok_r(qbuf, "&", &saveptr);
        while (pair) {
            char *eq = strchr(pair, '=');
            if (eq) {
                *eq = '\0';
                lua_pushstring(L, eq + 1);
                lua_setfield(L, -2, pair);
            } else {
                lua_pushstring(L, "");
                lua_setfield(L, -2, pair);
            }
            pair = strtok_r(NULL, "&", &saveptr);
        }
    }
    lua_setfield(L, -2, "query");

    /* params — route params from Keel (e.g. :id → params.id) */
    lua_newtable(L);
    for (int i = 0; i < req->num_params; i++) {
        char name[HL_PARAM_NAME_MAX];
        size_t nlen = req->params[i].name_len < HL_PARAM_NAME_MAX - 1
                      ? req->params[i].name_len : HL_PARAM_NAME_MAX - 1;
        memcpy(name, req->params[i].name, nlen);
        name[nlen] = '\0';
        lua_pushlstring(L, req->params[i].value, req->params[i].value_len);
        lua_setfield(L, -2, name);
    }
    lua_setfield(L, -2, "params");

    /* headers → table */
    lua_newtable(L);
    for (int i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name && req->headers[i].value) {
            lua_pushlstring(L, req->headers[i].value,
                            req->headers[i].value_len);
            lua_setfield(L, -2, req->headers[i].name);
        }
    }
    lua_setfield(L, -2, "headers");

    /* body — extract from buffer reader if available */
    if (req->body_reader) {
        const char *data;
        size_t len = hl_cap_body_data(req->body_reader, &data);
        if (len > 0)
            lua_pushlstring(L, data, len);
        else
            lua_pushstring(L, "");
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "body");

    /* ctx — per-request context table (middleware → handler) */
    lua_newtable(L);
    lua_setfield(L, -2, "ctx");
}

/* ── Response object ────────────────────────────────────────────────── */

/*
 * Response is a Lua userdata with a metatable providing methods:
 *   res:status(code)        → set status (chainable)
 *   res:header(name, val)   → add header (chainable)
 *   res:json(data, code?)   → send JSON response
 *   res:html(str)           → send HTML response
 *   res:text(str)           → send text response
 *   res:redirect(url, code) → HTTP redirect
 */

static KlResponse *check_response(lua_State *L, int idx)
{
    KlResponse **pp = (KlResponse **)luaL_checkudata(L, idx, HL_RESPONSE_MT);
    return *pp;
}

/* res:status(code) */
static int lua_res_status(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    int code = (int)luaL_checkinteger(L, 2);
    kl_response_status(res, code);
    lua_pushvalue(L, 1); /* chainable */
    return 1;
}

/* res:header(name, value) */
static int lua_res_header(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const char *value = luaL_checkstring(L, 3);
    kl_response_header(res, name, value);
    lua_pushvalue(L, 1); /* chainable */
    return 1;
}

/* res:json(data, code?) — uses json.encode() from Lua stdlib */
static int lua_res_json(lua_State *L)
{
    KlResponse *res = check_response(L, 1);

    /* Optional status code */
    if (lua_gettop(L) >= 3) {
        int code = (int)luaL_checkinteger(L, 3);
        kl_response_status(res, code);
    }

    /* Call json.encode(data) via the global 'json' table */
    lua_getglobal(L, "json");
    lua_getfield(L, -1, "encode");
    lua_pushvalue(L, 2); /* push the data argument */
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_remove(L, -2); /* remove json table */
        return lua_error(L);
    }

    size_t json_len;
    const char *json_str = lua_tolstring(L, -1, &json_len);
    const char *copy = hl_lua_stash_body(L, json_str, json_len);
    lua_pop(L, 1); /* pop JSON string */
    lua_pop(L, 1); /* pop json table */
    if (copy) {
        kl_response_header(res, "Content-Type", "application/json");
        kl_response_body(res, copy, json_len);
    }

    return 0;
}

/* res:html(string) */
static int lua_res_html(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    size_t len;
    const char *html = luaL_checklstring(L, 2, &len);
    const char *copy = hl_lua_stash_body(L, html, len);
    if (copy) {
        kl_response_header(res, "Content-Type", "text/html; charset=utf-8");
        kl_response_body(res, copy, len);
    }
    return 0;
}

/* res:text(string) */
static int lua_res_text(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    size_t len;
    const char *text = luaL_checklstring(L, 2, &len);
    const char *copy = hl_lua_stash_body(L, text, len);
    if (copy) {
        kl_response_header(res, "Content-Type", "text/plain; charset=utf-8");
        kl_response_body(res, copy, len);
    }
    return 0;
}

/* res:redirect(url, code?) */
static int lua_res_redirect(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    const char *url = luaL_checkstring(L, 2);
    int code = 302;
    if (lua_gettop(L) >= 3)
        code = (int)luaL_checkinteger(L, 3);

    kl_response_status(res, code);
    kl_response_header(res, "Location", url);
    kl_response_body(res, "", 0);
    return 0;
}

/* ── Response metatable registration ────────────────────────────────── */

static const luaL_Reg response_methods[] = {
    {"status",   lua_res_status},
    {"header",   lua_res_header},
    {"json",     lua_res_json},
    {"html",     lua_res_html},
    {"text",     lua_res_text},
    {"redirect", lua_res_redirect},
    {NULL, NULL}
};

static void ensure_response_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, HL_RESPONSE_MT)) {
        /* First time — set up metatable */
        luaL_newlib(L, response_methods);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1); /* pop metatable */
}

/* ── Public: create Lua response userdata ───────────────────────────── */

void hl_lua_make_response(lua_State *L, KlResponse *res)
{
    ensure_response_metatable(L);

    KlResponse **pp = (KlResponse **)lua_newuserdata(L, sizeof(KlResponse *));
    *pp = res;
    luaL_setmetatable(L, HL_RESPONSE_MT);
}
