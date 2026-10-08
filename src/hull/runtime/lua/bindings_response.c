/*
 * bindings_response.c - the res:* response helpers, extracted from bindings.c.
 *
 * Moved out (#114) so the core lua_rt_bindings.o holds ZERO Keel-response /
 * compress references (kl_http_response_*, hl_maybe_compress): those live only here,
 * on the HTTP side of the seam, composed into the `http` feature alongside
 * http_register.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "log.h"
#include "hull/shared/req_life.h"
#include "hull/runtime/lua.h"     /* HlLua, KlHttpResponse, hl_lua_make_response */
#include "hull/utils/compress.h"  /* hl_maybe_compress */
#include "hull/http_feature.h"    /* hl_lua_http_error_response (seam strong) */
#include "internal.h"             /* get_hl_lua_from_L (shared with bindings.c) */
#include "hull/limits/runtime.h"  /* HL_RES_HEADER_BYTES_MAX */
#include "hull/shared/res_headers.h" /* the cap, has / re-encode helpers */

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/http_response.h>
#include <keel/http_connection.h>  /* kl_http_conn_response (finalize seam) */
#include <keel/http_request.h>     /* kl_http_request_send_response (finalize seam) */

#include <string.h>

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

/* The userdata: the response, and the life of the request it belongs to
 * (shared/req_life.h). Used after its request is over - kept in a closure a
 * timer runs, stashed in a global - it fails closed instead of writing into a
 * response that was already sent, on a connection that may be gone. */
typedef struct {
    KlHttpResponse *res;
    HlReqLife      *life;   /* NULL: not tracked (always live) */
    int             ct_hull; /* the Content-Type is the one a body call added
                              * (res_headers.h, audit 10) */
} HlLuaResUD;

static HlLuaResUD *check_response_ud(lua_State *L, int idx)
{
    HlLuaResUD *ud = (HlLuaResUD *)luaL_checkudata(L, idx, HL_RESPONSE_MT);
    if (!ud->res || !hl_req_life_live(ud->life))
        luaL_error(L, "res: the request this response belongs to has finished");
    return ud;
}

static KlHttpResponse *check_response(lua_State *L, int idx)
{
    return check_response_ud(L, idx)->res;
}

static int lua_res_gc(lua_State *L)
{
    HlLuaResUD *ud = (HlLuaResUD *)luaL_checkudata(L, 1, HL_RESPONSE_MT);
    hl_req_life_release(ud->life);
    ud->life = NULL;
    ud->res = NULL;
    return 0;
}

/* Room for one more "Name: value\r\n" in the response's headers, charged
 * (audit 9 M1). The header buffer is Keel's, outside the script heap, so
 * neither the heap limit nor the allocation charge saw it grow: a loop of
 * res:header grew it to all of memory. Raises past HL_RES_HEADER_BYTES_MAX
 * (hull/shared/res_headers.h, shared with the JS twin). */
static void res_header_room(lua_State *L, KlHttpResponse *res,
                            const char *what, size_t name_len,
                            size_t value_len)
{
    lua_hlcharge(L, 0, name_len + value_len);
    if (!hl_res_header_fits(res, name_len, value_len))
        luaL_error(L, "%s: the response's headers would exceed %d bytes",
                   what, (int)HL_RES_HEADER_BYTES_MAX);
}

/* Add a header Hull itself sets (Content-Type, the default CSP, Location),
 * under the same cap as res:header. */
static void res_set_header(lua_State *L, KlHttpResponse *res,
                           const char *what, const char *name,
                           const char *value)
{
    res_header_room(L, res, what, strlen(name), strlen(value));
    (void)kl_http_response_header(res, name, value);
}

/* The Content-Type res:json / html / text set. An app's
 * res:header("Content-Type", ...) wins (as the CSP below), and there is only
 * ever one (audit 9) - but a Content-Type an earlier body call added is
 * Hull's own and is replaced: res:html then res:json kept text/html for the
 * JSON (audit 10). */
static void res_default_content_type(lua_State *L, HlLuaResUD *ud,
                                     const char *what, const char *value)
{
    if (hl_res_content_type_prepare(ud->res, &ud->ct_hull)) {
        res_set_header(L, ud->res, what, "Content-Type", value);
        ud->ct_hull = 1;
    }
}

/* A body set by res:json / res:html / res:text is copied out of the heap and
 * may be gzipped, inside one call (audit 9 M2): charged before the work,
 * a unit per 8 bytes (the rate of a digest; gzip costs more per byte than a
 * copy, and either may run), so calling them in a loop is not free. */
static void res_charge_body(lua_State *L, size_t len)
{
    lua_hlwork(L, len / 8, 0);
}

/* res:status(code) */
static int lua_res_status(lua_State *L)
{
    KlHttpResponse *res = check_response(L, 1);
    int code = (int)luaL_checkinteger(L, 2);
    kl_http_response_status(res, code);
    lua_pushvalue(L, 1); /* chainable */
    return 1;
}

/* res:header(name, value) */
static int lua_res_header(lua_State *L)
{
    HlLuaResUD *ud = check_response_ud(L, 1);
    KlHttpResponse *res = ud->res;
    size_t name_len, value_len;
    const char *name = luaL_checklstring(L, 2, &name_len);
    const char *value = luaL_checklstring(L, 3, &value_len);
    res_header_room(L, res, "res:header", name_len, value_len);
    /* The app's Content-Type replaces one an earlier body call added
     * (audit 10): one header, the app's. */
    if (hl_res_is_content_type(name, name_len))
        hl_res_drop_default_content_type(res, &ud->ct_hull);
    /* Rejected for a CR or LF (the header-injection guard). Not named in the
     * log: the name may be the part carrying the CR/LF. */
    if (kl_http_response_header(res, name, value) != 0)
        log_warn("[hull] res:header: a header was dropped - its name or value "
                 "contains CR or LF");
    lua_pushvalue(L, 1); /* chainable */
    return 1;
}

/* res:json(data, code?) - uses json.encode() from Lua stdlib */
static int lua_res_json(lua_State *L)
{
    HlLuaResUD *ud = check_response_ud(L, 1);
    KlHttpResponse *res = ud->res;
    HlLua *hlua = get_hl_lua_from_L(L);

    /* Optional status code */
    if (lua_gettop(L) >= 3) {
        int code = (int)luaL_checkinteger(L, 3);
        kl_http_response_status(res, code);
    }

    /* Call json.encode(data) via the runtime's cached decoder
     * (registry stash from mod_fs.c init). Works regardless of
     * whether the app declared hull/json - res:json() is a
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
        return luaL_error(L, "res:json - json.encode did not return a string");
    }
    res_default_content_type(L, ud, "res:json", "application/json");
    res_charge_body(L, json_len);
    hl_res_body_reencode(res);
    if (hl_maybe_compress(hlua ? hlua->active_req : NULL, res,
                          hlua ? hlua->base.compress : NULL,
                          json_str, json_len) != 0)
        return luaL_error(L, "res:json: out of memory");
    lua_pop(L, 1); /* pop JSON string */
    lua_pop(L, 1); /* pop json table */

    return 0;
}

/* res:html(string) */
static int lua_res_html(lua_State *L)
{
    HlLuaResUD *ud = check_response_ud(L, 1);
    KlHttpResponse *res = ud->res;
    HlLua *hlua = get_hl_lua_from_L(L);
    size_t len;
    const char *html = luaL_checklstring(L, 2, &len);
    res_default_content_type(L, ud, "res:html",
                             "text/html; charset=utf-8");
    /* Skip the default CSP if middleware already wrote one - two CSP
     * headers cause browsers to enforce the strict intersection
     * (typically blocking the page's own scripts). The app-supplied
     * one wins. */
    if (hlua && hlua->base.csp_policy &&
        !hl_res_header_has(res, "Content-Security-Policy"))
        res_set_header(L, res, "res:html", "Content-Security-Policy",
                       hlua->base.csp_policy);
    res_charge_body(L, len);
    hl_res_body_reencode(res);
    if (hl_maybe_compress(hlua ? hlua->active_req : NULL, res,
                          hlua ? hlua->base.compress : NULL,
                          html, len) != 0)
        return luaL_error(L, "res:html: out of memory");
    return 0;
}

/* res:text(string) */
static int lua_res_text(lua_State *L)
{
    HlLuaResUD *ud = check_response_ud(L, 1);
    KlHttpResponse *res = ud->res;
    HlLua *hlua = get_hl_lua_from_L(L);
    size_t len;
    const char *text = luaL_checklstring(L, 2, &len);
    res_default_content_type(L, ud, "res:text",
                             "text/plain; charset=utf-8");
    res_charge_body(L, len);
    hl_res_body_reencode(res);
    if (hl_maybe_compress(hlua ? hlua->active_req : NULL, res,
                          hlua ? hlua->base.compress : NULL,
                          text, len) != 0)
        return luaL_error(L, "res:text: out of memory");
    return 0;
}

/* res:bytes(string) - binary-safe response primitive.
 *
 * Unlike res:text/res:html/res:json, this does NOT set Content-Type
 * (caller's responsibility; binary content can be anything from
 * image/png to application/zip to application/octet-stream) and does
 * NOT route through hl_maybe_compress (Content-Encoding: gzip on
 * already-compressed payloads is pointless and hides the SHA from
 * any ETag computed on the response bytes).
 *
 * The body is copied into a response-owned buffer via
 * kl_http_response_body_copy, so the Lua string can be GC'd safely. Lua
 * strings are binary-safe (#str gives the byte count), so this is
 * a true bytes API. */
static int lua_res_bytes(lua_State *L)
{
    HlLuaResUD *ud = check_response_ud(L, 1);
    KlHttpResponse *res = ud->res;
    size_t len;
    const char *bytes = luaL_checklstring(L, 2, &len);
    lua_hlwork(L, 0, len);   /* the copy (audit 9 M2) */
    /* The headers an earlier res:json / html / text left describe that body,
     * not these bytes (audit 10): its gzip's Content-Encoding and Vary, and
     * the Content-Type Hull chose for it. An app-set Content-Type stays. */
    hl_res_body_reencode(res);
    hl_res_drop_default_content_type(res, &ud->ct_hull);
    if (kl_http_response_body_copy(res, bytes, len) != 0)
        return luaL_error(L, "res:bytes: out of memory");
    return 0;
}

/* res:redirect(url, code?) */
static int lua_res_redirect(lua_State *L)
{
    KlHttpResponse *res = check_response(L, 1);
    const char *url = luaL_checkstring(L, 2);
    int code = 302;
    if (lua_gettop(L) >= 3)
        code = (int)luaL_checkinteger(L, 3);

    kl_http_response_status(res, code);
    res_set_header(L, res, "res:redirect", "Location", url);
    kl_http_response_body_borrow(res, "", 0);
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
        /* First time - set up metatable */
        luaL_newlib(L, response_methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_res_gc);
        lua_setfield(L, -2, "__gc");
        /* Locked: Lua decides finalization when the metatable is set, so
         * `getmetatable(res).__gc = nil` made every later res unfinalized
         * (one HlReqLife leaked per request), and the methods table the
         * stdlib calls was the app's to rewrite. */
        lua_pushliteral(L, "locked");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1); /* pop metatable */
}

/* ── Public: create Lua response userdata ───────────────────────────── */

void hl_lua_make_response_life(lua_State *L, KlHttpResponse *res,
                               HlReqLife *life)
{
    ensure_response_metatable(L);

    HlLuaResUD *ud = (HlLuaResUD *)lua_newuserdatauv(L, sizeof *ud, 0);
    ud->res = res;
    ud->life = life;
    ud->ct_hull = 0;
    hl_req_life_retain(life);
    luaL_setmetatable(L, HL_RESPONSE_MT);
}

void hl_lua_make_response(lua_State *L, KlHttpResponse *res)
{
    hl_lua_make_response_life(L, res, NULL);
}

/* ── HTTP-feature seam: 500-error response ──────────────────────────── */
/* Strong override for the Lua runtime. Extracted from lua/dispatch.c +
 * lua/async.c so those core objects hold no kl_http_response_* refs. */
void hl_lua_http_error_response(struct KlHttpResponse *res)
{
    /* Whatever the handler set is dropped: its Set-Cookie / Location, a
     * second Content-Type (audit 10). */
    hl_res_error_reset(res, 500, "Internal Server Error", 21);
}

/* Strong overrides: finalize + send a resumed request's response. Keeps ALL
 * kl_http_* refs (incl. kl_http_request_send_response from the heavy
 * http_server_core object) out of the base runtime's lua_rt_async.o; see
 * include/hull/http_feature.h. */
void hl_lua_http_resume_send(struct KlHttpConn *conn, struct KlHttpRequest *req)
{
    KlHttpResponse *res = kl_http_conn_response(conn);
    if (res && res->body_mode == KL_HTTP_BODY_STREAM)
        kl_http_response_end_stream(res);
    kl_http_request_send_response(req);
}

void hl_lua_http_resume_error(struct KlHttpConn *conn, struct KlHttpRequest *req)
{
    hl_lua_http_error_response(kl_http_conn_response(conn));
    kl_http_request_send_response(req);
}
