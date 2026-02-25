/*
 * lua_bindings.c — Request/Response bridge to Lua 5.4
 *
 * Marshals Keel's KlRequest/KlResponse to Lua tables/userdata.
 * This file contains ONLY data marshaling — all enforcement logic
 * lives in hull_cap_* functions.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/lua_runtime.h"
#include "hull/hull_cap.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/request.h>
#include <keel/response.h>
#include <keel/router.h>

#include <string.h>
#include <stdio.h>

/* ── Response metatable name ────────────────────────────────────────── */

#define HULL_RESPONSE_MT "HullResponse"

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
void hull_lua_make_request(lua_State *L, KlRequest *req)
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
        char qbuf[4096];
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

    /* params — route params are on KlConn, not KlRequest.
     * TODO: pass params via req->ctx once Keel supports it. */
    lua_newtable(L);
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

    /* body */
    if (req->body_reader)
        lua_pushstring(L, "");
    else
        lua_pushnil(L);
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
    KlResponse **pp = (KlResponse **)luaL_checkudata(L, idx, HULL_RESPONSE_MT);
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

/* res:json(data, code?) */
static int lua_res_json(lua_State *L)
{
    KlResponse *res = check_response(L, 1);

    /* Optional status code */
    if (lua_gettop(L) >= 3) {
        int code = (int)luaL_checkinteger(L, 3);
        kl_response_status(res, code);
    }

    /* Serialize Lua value to JSON manually */
    luaL_Buffer b;
    luaL_buffinit(L, &b);

    /* Use a simple serializer for tables/values */
    /* Push the value we want to serialize */
    lua_pushvalue(L, 2);

    /* We need a JSON encoder. Build one using a recursive approach. */
    /* For simplicity, use a C function that walks the Lua value. */
    /* This is registered as a helper. For now, serialize to JSON. */

    /* Use the approach: push a cjson-like serializer, or manually walk. */
    /* Since we don't have cjson, do a manual walk. */

    int t = lua_type(L, 2);
    if (t == LUA_TTABLE) {
        /* Check if it's an array or object */
        /* Array if sequential integer keys starting at 1 */
        lua_len(L, 2);
        lua_Integer arr_len = lua_tointeger(L, -1);
        lua_pop(L, 1);

        int is_array = 0;
        if (arr_len > 0) {
            /* Check if key 1 exists — heuristic for array */
            lua_rawgeti(L, 2, 1);
            is_array = !lua_isnil(L, -1);
            lua_pop(L, 1);
        }

        if (is_array && arr_len > 0) {
            luaL_addchar(&b, '[');
            for (lua_Integer i = 1; i <= arr_len; i++) {
                if (i > 1)
                    luaL_addchar(&b, ',');
                lua_rawgeti(L, 2, i);
                int vt = lua_type(L, -1);
                if (vt == LUA_TSTRING) {
                    size_t slen;
                    const char *s = lua_tolstring(L, -1, &slen);
                    luaL_addchar(&b, '"');
                    luaL_addlstring(&b, s, slen);
                    luaL_addchar(&b, '"');
                } else if (vt == LUA_TNUMBER) {
                    if (lua_isinteger(L, -1)) {
                        char num[32];
                        snprintf(num, sizeof(num), "%lld",
                                 (long long)lua_tointeger(L, -1));
                        luaL_addstring(&b, num);
                    } else {
                        char num[64];
                        snprintf(num, sizeof(num), "%g",
                                 (double)lua_tonumber(L, -1));
                        luaL_addstring(&b, num);
                    }
                } else if (vt == LUA_TBOOLEAN) {
                    luaL_addstring(&b, lua_toboolean(L, -1) ? "true" : "false");
                } else {
                    luaL_addstring(&b, "null");
                }
                lua_pop(L, 1);
            }
            luaL_addchar(&b, ']');
        } else {
            /* Object: iterate with lua_next */
            luaL_addchar(&b, '{');
            int first = 1;
            lua_pushnil(L);
            while (lua_next(L, 2) != 0) {
                if (!first)
                    luaL_addchar(&b, ',');
                first = 0;

                /* Key must be string */
                if (lua_type(L, -2) == LUA_TSTRING) {
                    size_t klen;
                    const char *k = lua_tolstring(L, -2, &klen);
                    luaL_addchar(&b, '"');
                    luaL_addlstring(&b, k, klen);
                    luaL_addstring(&b, "\":");

                    int vt = lua_type(L, -1);
                    if (vt == LUA_TSTRING) {
                        size_t slen;
                        const char *s = lua_tolstring(L, -1, &slen);
                        luaL_addchar(&b, '"');
                        luaL_addlstring(&b, s, slen);
                        luaL_addchar(&b, '"');
                    } else if (vt == LUA_TNUMBER) {
                        if (lua_isinteger(L, -1)) {
                            char num[32];
                            snprintf(num, sizeof(num), "%lld",
                                     (long long)lua_tointeger(L, -1));
                            luaL_addstring(&b, num);
                        } else {
                            char num[64];
                            snprintf(num, sizeof(num), "%g",
                                     (double)lua_tonumber(L, -1));
                            luaL_addstring(&b, num);
                        }
                    } else if (vt == LUA_TBOOLEAN) {
                        luaL_addstring(&b, lua_toboolean(L, -1)
                                           ? "true" : "false");
                    } else {
                        luaL_addstring(&b, "null");
                    }
                }
                lua_pop(L, 1); /* pop value, keep key for next iteration */
            }
            luaL_addchar(&b, '}');
        }
    } else if (t == LUA_TSTRING) {
        size_t slen;
        const char *s = lua_tolstring(L, 2, &slen);
        luaL_addchar(&b, '"');
        luaL_addlstring(&b, s, slen);
        luaL_addchar(&b, '"');
    } else if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, 2)) {
            char num[32];
            snprintf(num, sizeof(num), "%lld",
                     (long long)lua_tointeger(L, 2));
            luaL_addstring(&b, num);
        } else {
            char num[64];
            snprintf(num, sizeof(num), "%g", (double)lua_tonumber(L, 2));
            luaL_addstring(&b, num);
        }
    } else if (t == LUA_TBOOLEAN) {
        luaL_addstring(&b, lua_toboolean(L, 2) ? "true" : "false");
    } else {
        luaL_addstring(&b, "null");
    }

    lua_pop(L, 1); /* pop the extra pushvalue */
    luaL_pushresult(&b);

    size_t json_len;
    const char *json_str = lua_tolstring(L, -1, &json_len);
    kl_response_header(res, "Content-Type", "application/json");
    kl_response_body(res, json_str, json_len);
    lua_pop(L, 1); /* pop JSON string */

    return 0;
}

/* res:html(string) */
static int lua_res_html(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    size_t len;
    const char *html = luaL_checklstring(L, 2, &len);
    kl_response_header(res, "Content-Type", "text/html; charset=utf-8");
    kl_response_body(res, html, len);
    return 0;
}

/* res:text(string) */
static int lua_res_text(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    size_t len;
    const char *text = luaL_checklstring(L, 2, &len);
    kl_response_header(res, "Content-Type", "text/plain; charset=utf-8");
    kl_response_body(res, text, len);
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
    if (luaL_newmetatable(L, HULL_RESPONSE_MT)) {
        /* First time — set up metatable */
        luaL_newlib(L, response_methods);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1); /* pop metatable */
}

/* ── Public: create Lua response userdata ───────────────────────────── */

void hull_lua_make_response(lua_State *L, KlResponse *res)
{
    ensure_response_metatable(L);

    KlResponse **pp = (KlResponse **)lua_newuserdata(L, sizeof(KlResponse *));
    *pp = res;
    luaL_setmetatable(L, HULL_RESPONSE_MT);
}
