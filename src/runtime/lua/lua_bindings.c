/*
 * lua_bindings.c — Request/Response bridge to Lua 5.4
 *
 * Marshals Keel's KlRequest/KlResponse to Lua tables/userdata.
 * This file contains ONLY data marshaling — all enforcement logic
 * lives in hl_cap_* functions.
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
    free(hlua->response_body);
    hlua->response_body = malloc(len + 1);
    if (!hlua->response_body)
        return NULL;
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

    /* params — route params from Keel (e.g. :id → params.id) */
    lua_newtable(L);
    for (int i = 0; i < req->num_params; i++) {
        char name[256];
        size_t nlen = req->params[i].name_len < 255
                      ? req->params[i].name_len : 255;
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

/* ── Iterative JSON encoder (explicit stack, no C recursion) ─────────── */

#define JSON_MAX_DEPTH 20

static void json_encode_string(luaL_Buffer *b, lua_State *L, int idx)
{
    size_t slen;
    const char *s = lua_tolstring(L, idx, &slen);
    luaL_addchar(b, '"');
    for (size_t i = 0; i < slen; i++) {
        char c = s[i];
        switch (c) {
        case '"':  luaL_addstring(b, "\\\""); break;
        case '\\': luaL_addstring(b, "\\\\"); break;
        case '\n': luaL_addstring(b, "\\n");  break;
        case '\r': luaL_addstring(b, "\\r");  break;
        case '\t': luaL_addstring(b, "\\t");  break;
        default:
            if ((unsigned char)c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)c);
                luaL_addstring(b, esc);
            } else {
                luaL_addchar(b, c);
            }
        }
    }
    luaL_addchar(b, '"');
}

static void json_encode_number(luaL_Buffer *b, lua_State *L, int idx)
{
    if (lua_isinteger(L, idx)) {
        char num[32];
        snprintf(num, sizeof(num), "%lld",
                 (long long)lua_tointeger(L, idx));
        luaL_addstring(b, num);
    } else {
        char num[64];
        snprintf(num, sizeof(num), "%g",
                 (double)lua_tonumber(L, idx));
        luaL_addstring(b, num);
    }
}

static void json_encode_scalar(luaL_Buffer *b, lua_State *L, int idx)
{
    int t = lua_type(L, idx);
    if (t == LUA_TSTRING)
        json_encode_string(b, L, idx);
    else if (t == LUA_TNUMBER)
        json_encode_number(b, L, idx);
    else if (t == LUA_TBOOLEAN)
        luaL_addstring(b, lua_toboolean(L, idx) ? "true" : "false");
    else
        luaL_addstring(b, "null");
}

/*
 * Frame on the explicit work stack. Each frame tracks iteration state
 * for one table (array or object) being serialized.
 *
 * For arrays:  iterate i from 1..arr_len using lua_rawgeti.
 * For objects: iterate with lua_next (key kept on Lua stack between calls).
 */
typedef struct {
    int         lua_idx;    /* absolute Lua stack index of the table */
    int         is_array;
    lua_Integer arr_len;    /* total array length (arrays only) */
    lua_Integer arr_i;      /* next index to emit (arrays only) */
    int         first;      /* true if no elements emitted yet */
} JsonFrame;

/*
 * Encode the Lua value at stack index `idx` into the luaL_Buffer.
 * Uses an explicit JsonFrame stack instead of C recursion.
 */
static void json_encode(lua_State *L, luaL_Buffer *b, int idx)
{
    int abs_idx = lua_absindex(L, idx);

    /* If top-level value is not a table, encode directly */
    if (lua_type(L, abs_idx) != LUA_TTABLE) {
        json_encode_scalar(b, L, abs_idx);
        return;
    }

    JsonFrame frames[JSON_MAX_DEPTH];
    int depth = 0;

    /* Helper: detect array vs object */
    #define INIT_FRAME(fr, tbl_idx) do {                           \
        (fr)->lua_idx  = (tbl_idx);                                \
        (fr)->first    = 1;                                        \
        lua_len(L, (tbl_idx));                                     \
        (fr)->arr_len  = lua_tointeger(L, -1);                     \
        lua_pop(L, 1);                                             \
        (fr)->is_array = 0;                                        \
        if ((fr)->arr_len > 0) {                                   \
            lua_rawgeti(L, (tbl_idx), 1);                          \
            (fr)->is_array = !lua_isnil(L, -1);                    \
            lua_pop(L, 1);                                         \
        }                                                          \
        (fr)->arr_i = 1;                                           \
    } while (0)

    /* Push root table frame */
    INIT_FRAME(&frames[0], abs_idx);
    luaL_addchar(b, frames[0].is_array ? '[' : '{');
    if (!frames[0].is_array)
        lua_pushnil(L); /* seed lua_next */
    depth = 1;

    while (depth > 0) {
        JsonFrame *f = &frames[depth - 1];

        if (f->is_array) {
            /* Array iteration */
            if (f->arr_i > f->arr_len) {
                luaL_addchar(b, ']');
                depth--;
                continue;
            }

            if (!f->first)
                luaL_addchar(b, ',');
            f->first = 0;

            lua_rawgeti(L, f->lua_idx, f->arr_i);
            f->arr_i++;

            if (lua_type(L, -1) == LUA_TTABLE && depth < JSON_MAX_DEPTH) {
                int tbl = lua_absindex(L, -1);
                INIT_FRAME(&frames[depth], tbl);
                luaL_addchar(b, frames[depth].is_array ? '[' : '{');
                if (!frames[depth].is_array)
                    lua_pushnil(L); /* seed lua_next */
                depth++;
            } else {
                json_encode_scalar(b, L, lua_absindex(L, -1));
                lua_pop(L, 1);
            }
        } else {
            /* Object iteration — lua_next key is on top of Lua stack */
            if (lua_next(L, f->lua_idx) == 0) {
                luaL_addchar(b, '}');
                depth--;
                /* Pop the table we pushed for this frame (if nested) */
                if (depth > 0)
                    lua_pop(L, 1); /* pop the nested table value */
                continue;
            }

            /* Key at -2, value at -1 */
            if (lua_type(L, -2) != LUA_TSTRING) {
                /* Skip non-string keys */
                lua_pop(L, 1);
                continue;
            }

            if (!f->first)
                luaL_addchar(b, ',');
            f->first = 0;

            /* Emit key */
            json_encode_string(b, L, lua_absindex(L, -2));
            luaL_addchar(b, ':');

            if (lua_type(L, -1) == LUA_TTABLE && depth < JSON_MAX_DEPTH) {
                /* Nested table in object value — push new frame.
                 * Keep key and value on Lua stack:
                 *   key stays for lua_next resume (but we won't resume
                 *   the parent until this sub-frame completes).
                 * Actually, lua_next needs the key on top when we resume.
                 * The sub-table's iteration will push/pop its own values.
                 * After the sub-frame pops, the value is still at -1,
                 * and we pop it in the close-bracket handler above.
                 * The key then becomes -1 for the parent's lua_next. */
                int tbl = lua_absindex(L, -1);
                INIT_FRAME(&frames[depth], tbl);
                luaL_addchar(b, frames[depth].is_array ? '[' : '{');
                if (!frames[depth].is_array)
                    lua_pushnil(L); /* seed lua_next for nested object */
                depth++;
            } else {
                json_encode_scalar(b, L, lua_absindex(L, -1));
                lua_pop(L, 1); /* pop value, keep key for next lua_next */
            }
        }
    }

    #undef INIT_FRAME
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

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    json_encode(L, &b, 2);
    luaL_pushresult(&b);

    size_t json_len;
    const char *json_str = lua_tolstring(L, -1, &json_len);
    const char *copy = hl_lua_stash_body(L, json_str, json_len);
    lua_pop(L, 1); /* pop JSON string */
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
