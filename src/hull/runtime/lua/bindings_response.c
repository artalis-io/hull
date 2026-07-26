/*
 * bindings_response.c — the res:* response helpers, extracted from bindings.c.
 *
 * Moved out (#114) so the core lua_rt_bindings.o holds ZERO Keel-response /
 * compress references (kl_response_*, hl_maybe_compress): those live only here,
 * on the HTTP side of the seam. Phase A: rides the lua runtime archive; Phase C
 * relocates it into the composed `http` feature alongside http_register.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"     /* HlLua, KlResponse, hl_lua_make_response */
#include "hull/utils/compress.h"  /* hl_maybe_compress */
#include "internal.h"             /* get_hl_lua_from_L (shared with bindings.c) */

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/response.h>

#include <string.h>
#include <strings.h>  /* strncasecmp */

/* ── Response metatable name ────────────────────────────────────────── */

#define HL_RESPONSE_MT "HlResponse"

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
