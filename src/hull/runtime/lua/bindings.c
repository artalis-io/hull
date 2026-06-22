/*
 * lua_bindings.c — Request/Response bridge to Lua 5.4
 *
 * Marshals Keel's KlRequest/KlResponse to Lua tables/userdata.
 * This file contains ONLY data marshaling — all enforcement logic
 * lives in hl_cap_* functions.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "hull/reqctx.h"
#include "hull/limits/core.h"
#include "hull/cap/body.h"
#include "hull/compress.h"
#include "internal.h"  /* hl_lua_request_install_multipart */

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/request.h>
#include <keel/response.h>
#include <keel/router.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Response metatable name ────────────────────────────────────────── */

#define HL_RESPONSE_MT "HlResponse"

/* ── Helper: retrieve HlLua from Lua registry ────────────────────── */

static HlLua *get_hl_lua_from_L(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *hlua = lua_isuserdata(L, -1)
                  ? (HlLua *)lua_touserdata(L, -1) : NULL;
    lua_pop(L, 1);
    return hlua;
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
/* Percent-decode a query-string token in place (also turns `+` into
 * space, per application/x-www-form-urlencoded convention). Returns
 * the new length. Invalid `%XX` (truncated or non-hex) is left as-is
 * so we never silently drop bytes from a malformed URL. */
static size_t hl_query_decode_inplace(char *s, size_t len)
{
    size_t r = 0, w = 0;
    while (r < len) {
        unsigned char c = (unsigned char)s[r];
        if (c == '+') {
            s[w++] = ' '; r++;
        } else if (c == '%' && r + 2 < len) {
            int hi = s[r + 1], lo = s[r + 2];
            int hv = (hi >= '0' && hi <= '9') ? hi - '0'
                   : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                   : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
            int lv = (lo >= '0' && lo <= '9') ? lo - '0'
                   : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                   : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
            if (hv >= 0 && lv >= 0) {
                s[w++] = (char)((hv << 4) | lv);
                r += 3;
            } else {
                s[w++] = s[r++];
            }
        } else {
            s[w++] = s[r++];
        }
    }
    return w;
}

void hl_lua_make_request(lua_State *L, KlRequest *req)
{
    lua_newtable(L);

    /* method (Keel stores as string).  All reads via kl_request_*
     * accessors so they route through req->sealed (mprotect-RO) when
     * KEEL_SEAL_REQUEST=1 is in the Keel build, or fall back to direct
     * fields otherwise.  See vendor/keel/include/keel/request.h. */
    const char *m = kl_request_method(req);
    if (m)
        lua_pushlstring(L, m, kl_request_method_len(req));
    else
        lua_pushstring(L, "GET");
    lua_setfield(L, -2, "method");

    /* path */
    const char *p = kl_request_path(req);
    if (p)
        lua_pushlstring(L, p, kl_request_path_len(req));
    else
        lua_pushstring(L, "/");
    lua_setfield(L, -2, "path");

    /* query string → table */
    lua_newtable(L);
    const char *q = kl_request_query(req);
    size_t q_len = kl_request_query_len(req);
    if (q && q_len > 0) {
        char qbuf[HL_QUERY_BUF_SIZE];
        size_t qlen = q_len < sizeof(qbuf) - 1
                      ? q_len : sizeof(qbuf) - 1;
        memcpy(qbuf, q, qlen);
        qbuf[qlen] = '\0';

        char *saveptr = NULL;
        char *pair = strtok_r(qbuf, "&", &saveptr);
        while (pair) {
            char *eq = strchr(pair, '=');
            size_t klen, vlen;
            const char *val;
            if (eq) {
                *eq = '\0';
                klen = (size_t)(eq - pair);
                val  = eq + 1;
                vlen = strlen(val);
            } else {
                klen = strlen(pair);
                val  = "";
                vlen = 0;
            }
            klen = hl_query_decode_inplace(pair, klen);
            pair[klen] = '\0';
            if (vlen > 0) {
                vlen = hl_query_decode_inplace((char *)(uintptr_t)val, vlen);
                ((char *)(uintptr_t)val)[vlen] = '\0';
            }
            lua_pushlstring(L, val, vlen);
            lua_setfield(L, -2, pair);
            pair = strtok_r(NULL, "&", &saveptr);
        }
    }
    lua_setfield(L, -2, "query");

    /* params — route params from Keel (e.g. :id → params.id) */
    lua_newtable(L);
    int n_params = kl_request_num_params(req);
    for (int i = 0; i < n_params; i++) {
        KlParam param = kl_request_param_at(req, i);
        char name[HL_PARAM_NAME_MAX];
        size_t nlen = param.name_len < HL_PARAM_NAME_MAX - 1
                      ? param.name_len : HL_PARAM_NAME_MAX - 1;
        memcpy(name, param.name, nlen);
        name[nlen] = '\0';
        lua_pushlstring(L, param.value, param.value_len);
        lua_setfield(L, -2, name);
    }
    lua_setfield(L, -2, "params");

    /* headers → table (names lowercased for case-insensitive lookup) */
    lua_newtable(L);
    lua_checkstack(L, 3); /* key + value + table */
    int n_headers = kl_request_num_headers(req);
    for (int i = 0; i < n_headers; i++) {
        KlHeader hdr = kl_request_header_at(req, i);
        if (hdr.name && hdr.value) {
            char hdr_name[256];
            size_t nlen = hdr.name_len;
            if (nlen >= sizeof(hdr_name)) continue; /* skip oversized */
            for (size_t j = 0; j < nlen; j++) {
                unsigned char c = (unsigned char)hdr.name[j];
                hdr_name[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
            }
            lua_pushlstring(L, hdr_name, nlen);
            lua_pushlstring(L, hdr.value, hdr.value_len);
            lua_settable(L, -3);
        }
    }
    lua_setfield(L, -2, "headers");

    /* body — extract from buffer reader if available. For streaming-
     * multipart routes the body_reader is the parkable wrapper (not a
     * buffer reader), so body is nil and req:multipart() is the only
     * way to read bytes — installed just below. */
    int is_multipart_stream =
        req->body_reader != NULL &&
        hl_cap_multipart_inner(req->body_reader) != NULL;
    if (req->body_reader && !is_multipart_stream) {
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

    /* req.multipart() — only installed for streaming-multipart routes
     * (no-op otherwise). Defined in mod_request.c. */
    if (is_multipart_stream)
        hl_lua_request_install_multipart(L, get_hl_lua_from_L(L),
                                          req->body_reader);

    /* ctx — per-request context table (middleware → handler).
     * If req->ctx carries a native Lua ref, retrieve it directly;
     * if it carries a JSON string (from test dispatch), parse it;
     * otherwise start with an empty table. */
    if (req->ctx) {
        HlReqCtx *rctx = (HlReqCtx *)req->ctx;
        if (rctx->kind == HL_REQCTX_LUA_REF) {
            /* Native Lua table — retrieve directly from registry */
            lua_rawgeti(L, LUA_REGISTRYINDEX, rctx->lua_ref);
        } else if (rctx->kind == HL_REQCTX_JSON) {
            /* JSON string (from test dispatch) — parse it via the
             * runtime's cached decoder (no manifest gate). */
            lua_newtable(L);
            int ctx_idx = lua_absindex(L, -1);
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_json_internal");
            lua_getfield(L, -1, "decode");
            lua_pushstring(L, rctx->json.data);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                /* Merge decoded table into ctx */
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    lua_pushvalue(L, -2); /* copy key */
                    lua_insert(L, -2);    /* stack: ..., key, key, value */
                    lua_settable(L, ctx_idx);  /* ctx[key] = value */
                }
            }
            lua_pop(L, 1); /* pop decoded table or error */
            lua_pop(L, 1); /* pop json table */
        } else {
            lua_newtable(L); /* unknown kind — empty ctx */
        }
    } else {
        lua_newtable(L);
    }
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

/* Has a header with this name (case-insensitive) already been added to
 * the response? Used by res:html to avoid stamping Hull's default CSP
 * on top of one already set by application middleware — without this
 * the browser sees two Content-Security-Policy headers and enforces
 * the strict intersection, which typically blocks the page's own
 * scripts. Scans res->hdr_buf line by line; headers are appended as
 * "Name: value\r\n" by kl_response_header. */
static int hl_response_has_header(KlResponse *res, const char *name)
{
    if (!res || !res->hdr_buf || !name) return 0;
    size_t name_len = strlen(name);
    if (res->hdr_len < name_len + 2) return 0;
    const char *p   = res->hdr_buf;
    const char *end = res->hdr_buf + res->hdr_len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (line_len > name_len && p[name_len] == ':' &&
            strncasecmp(p, name, name_len) == 0) {
            return 1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
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
    HlLua *hlua = get_hl_lua_from_L(L);

    /* Optional status code */
    if (lua_gettop(L) >= 3) {
        int code = (int)luaL_checkinteger(L, 3);
        kl_response_status(res, code);
    }

    /* Call json.encode(data) via the runtime's cached decoder
     * (registry stash from mod_fs.c init). Works regardless of
     * whether the app declared hull/json — res:json() is a
     * response helper, not a user-visible json import. */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_json_internal");
    lua_getfield(L, -1, "encode");
    lua_pushvalue(L, 2); /* push the data argument */
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_remove(L, -2); /* remove json table */
        return lua_error(L);
    }

    size_t json_len;
    const char *json_str = lua_tolstring(L, -1, &json_len);
    if (!json_str) {
        lua_pop(L, 2);
        return luaL_error(L, "res:json — json.encode did not return a string");
    }
    kl_response_header(res, "Content-Type", "application/json");
    hl_maybe_compress(hlua ? hlua->active_req : NULL, res,
                      hlua ? hlua->base.compress : NULL,
                      json_str, json_len);
    lua_pop(L, 1); /* pop JSON string */
    lua_pop(L, 1); /* pop json table */

    return 0;
}

/* res:html(string) */
static int lua_res_html(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    HlLua *hlua = get_hl_lua_from_L(L);
    size_t len;
    const char *html = luaL_checklstring(L, 2, &len);
    kl_response_header(res, "Content-Type", "text/html; charset=utf-8");
    /* Skip the default CSP if middleware already wrote one — two CSP
     * headers cause browsers to enforce the strict intersection
     * (typically blocking the page's own scripts). The app-supplied
     * one wins. */
    if (hlua && hlua->base.csp_policy &&
        !hl_response_has_header(res, "Content-Security-Policy"))
        kl_response_header(res, "Content-Security-Policy",
                           hlua->base.csp_policy);
    hl_maybe_compress(hlua ? hlua->active_req : NULL, res,
                      hlua ? hlua->base.compress : NULL,
                      html, len);
    return 0;
}

/* res:text(string) */
static int lua_res_text(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    HlLua *hlua = get_hl_lua_from_L(L);
    size_t len;
    const char *text = luaL_checklstring(L, 2, &len);
    kl_response_header(res, "Content-Type", "text/plain; charset=utf-8");
    hl_maybe_compress(hlua ? hlua->active_req : NULL, res,
                      hlua ? hlua->base.compress : NULL,
                      text, len);
    return 0;
}

/* res:bytes(string) — binary-safe response primitive.
 *
 * Unlike res:text/res:html/res:json, this does NOT set Content-Type
 * (caller's responsibility; binary content can be anything from
 * image/png to application/zip to application/octet-stream) and does
 * NOT route through hl_maybe_compress (Content-Encoding: gzip on
 * already-compressed payloads is pointless and hides the SHA from
 * any ETag computed on the response bytes).
 *
 * The body is copied into a response-owned buffer via
 * kl_response_body_copy, so the Lua string can be GC'd safely. Lua
 * strings are binary-safe (#str gives the byte count), so this is
 * a true bytes API. */
static int lua_res_bytes(lua_State *L)
{
    KlResponse *res = check_response(L, 1);
    size_t len;
    const char *bytes = luaL_checklstring(L, 2, &len);
    if (kl_response_body_copy(res, bytes, len) != 0)
        return luaL_error(L, "res:bytes: out of memory");
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
    kl_response_body_borrow(res, "", 0);
    return 0;
}

/* ── Response metatable registration ────────────────────────────────── */

static const luaL_Reg response_methods[] = {
    {"status",   lua_res_status},
    {"header",   lua_res_header},
    {"json",     lua_res_json},
    {"html",     lua_res_html},
    {"text",     lua_res_text},
    {"bytes",    lua_res_bytes},
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
