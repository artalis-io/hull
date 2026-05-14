/* mod_http.c — hull.http module: HTTP client, sync and async
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/http.h"
#include "hull/cap/http_async.h"
#include "hull/async.h"

#include <keel/server.h>

#include <sh_arena.h>
#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.http module
 *
 * http.request(method, url, opts?) → { status, body, headers }
 * http.get(url, opts?)             → { status, body, headers }
 * http.post(url, body, opts?)      → { status, body, headers }
 * http.put(url, body, opts?)       → { status, body, headers }
 * http.patch(url, body, opts?)     → { status, body, headers }
 * http.delete(url, opts?)          → { status, body, headers }
 * ════════════════════════════════════════════════════════════════════ */

/* Parse optional headers table at stack index `idx` into HlHttpHeader array.
 * Returns 0 on success. Caller must free the returned array. */
static int lua_parse_http_headers(lua_State *L, int idx,
                                     HlHttpHeader **out_headers, int *out_count,
                                     SHArena *scratch)
{
    *out_headers = NULL;
    *out_count = 0;

    if (lua_isnoneornil(L, idx))
        return 0;

    if (!lua_istable(L, idx))
        return -1;

    /* Count entries */
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        count++;
        lua_pop(L, 1); /* pop value, keep key */
    }
    if (count == 0)
        return 0;

    HlHttpHeader *hdrs = sh_arena_calloc(scratch, (size_t)count, sizeof(HlHttpHeader));
    if (!hdrs)
        return -1;

    int i = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
            size_t nlen, vlen;
            const char *n = lua_tolstring(L, -2, &nlen);
            const char *v = lua_tolstring(L, -1, &vlen);
            char *nc = sh_arena_alloc(scratch, nlen + 1);
            char *vc = sh_arena_alloc(scratch, vlen + 1);
            if (nc && vc) {
                memcpy(nc, n, nlen + 1);
                memcpy(vc, v, vlen + 1);
                hdrs[i].name = nc;
                hdrs[i].value = vc;
                i++;
            }
        }
        lua_pop(L, 1); /* pop value, keep key */
    }

    *out_headers = hdrs;
    *out_count = i;
    return 0;
}

/* Push HTTP response as Lua table: { status, body, headers } */
static void lua_push_http_response(lua_State *L, const KlClientResponse *resp)
{
    lua_newtable(L);

    lua_pushinteger(L, resp->status);
    lua_setfield(L, -2, "status");

    if (resp->body && resp->body_len > 0)
        lua_pushlstring(L, resp->body, resp->body_len);
    else
        lua_pushstring(L, "");
    lua_setfield(L, -2, "body");

    /* Headers as { ["name"] = "value" } table */
    lua_newtable(L);
    for (int i = 0; i < resp->num_headers; i++) {
        lua_pushstring(L, resp->headers[i].value);
        /* Lowercase the header name for consistent access */
        size_t nlen = strlen(resp->headers[i].name);
        char *lower = sh_arena_alloc(
            ((HlLua *)get_hl_lua(L))->scratch, nlen + 1);
        if (lower) {
            for (size_t j = 0; j < nlen; j++)
                lower[j] = (char)((resp->headers[i].name[j] >= 'A' &&
                                    resp->headers[i].name[j] <= 'Z')
                    ? resp->headers[i].name[j] + 32
                    : resp->headers[i].name[j]);
            lower[nlen] = '\0';
            lua_setfield(L, -2, lower);
        } else {
            lua_setfield(L, -2, resp->headers[i].name);
        }
    }
    lua_setfield(L, -2, "headers");
}

/* http.request(method, url, opts?) */
static int lua_http_request(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *method = luaL_checkstring(L, 1);
    const char *url = luaL_checkstring(L, 2);

    const char *body = NULL;
    size_t body_len = 0;
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse optional opts table at position 3 */
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "body");
        if (lua_isstring(L, -1))
            body = lua_tolstring(L, -1, &body_len);
        lua_pop(L, 1);

        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            if (lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                        lua->scratch) != 0) {
                lua_pop(L, 1);
                return luaL_error(L, "invalid headers table");
            }
        }
        lua_pop(L, 1);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(lua->base.http_cfg, method, url,
                                    headers, num_headers, body, body_len, &resp);
    if (rc != 0)
        return luaL_error(L, "http request failed: %s",
                          kl_strerror(resp.error));

    lua_push_http_response(L, &resp);
    kl_client_response_free(&resp);
    return 1;
}

/* http.get(url, opts?) */
static int lua_http_get(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *url = luaL_checkstring(L, 1);
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                    lua->scratch);
        }
        lua_pop(L, 1);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(lua->base.http_cfg, "GET", url,
                                    headers, num_headers, NULL, 0, &resp);
    if (rc != 0)
        return luaL_error(L, "http.get failed: %s", kl_strerror(resp.error));

    lua_push_http_response(L, &resp);
    kl_client_response_free(&resp);
    return 1;
}

/* Helper for POST/PUT/PATCH: (url, body, opts?) */
static int lua_http_body_method(lua_State *L, const char *method)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *url = luaL_checkstring(L, 1);
    size_t body_len = 0;
    const char *body = NULL;
    if (lua_isstring(L, 2))
        body = lua_tolstring(L, 2, &body_len);

    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                    lua->scratch);
        }
        lua_pop(L, 1);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(lua->base.http_cfg, method, url,
                                    headers, num_headers, body, body_len, &resp);
    if (rc != 0)
        return luaL_error(L, "http.%s failed: %s", method,
                          kl_strerror(resp.error));

    lua_push_http_response(L, &resp);
    kl_client_response_free(&resp);
    return 1;
}

static int lua_http_post(lua_State *L)   { return lua_http_body_method(L, "POST"); }
static int lua_http_put(lua_State *L)    { return lua_http_body_method(L, "PUT"); }
static int lua_http_patch(lua_State *L)  { return lua_http_body_method(L, "PATCH"); }

/* http.delete(url, opts?) — same signature as http.get */
static int lua_http_delete(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *url = luaL_checkstring(L, 1);
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                    lua->scratch);
        }
        lua_pop(L, 1);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(lua->base.http_cfg, "DELETE", url,
                                    headers, num_headers, NULL, 0, &resp);
    if (rc != 0)
        return luaL_error(L, "http.delete failed: %s",
                          kl_strerror(resp.error));

    lua_push_http_response(L, &resp);
    kl_client_response_free(&resp);
    return 1;
}

/* ── Push async HTTP response onto Lua stack ──────────────────────── */

static void lua_push_async_http_response(lua_State *L, void *driver)
{
    const KlClientResponse *resp = hl_http_async_response(driver);
    if (!resp) {
        lua_pushnil(L);
        return;
    }

    lua_newtable(L);

    lua_pushinteger(L, resp->status);
    lua_setfield(L, -2, "status");

    if (resp->body && resp->body_len > 0)
        lua_pushlstring(L, resp->body, resp->body_len);
    else
        lua_pushstring(L, "");
    lua_setfield(L, -2, "body");

    /* Headers as { ["name"] = "value" } — lowercase names */
    lua_newtable(L);
    for (int i = 0; i < resp->num_headers; i++) {
        const char *name = resp->headers[i].name;
        size_t nlen = strlen(name);
        luaL_Buffer buf;
        luaL_buffinit(L, &buf);
        for (size_t j = 0; j < nlen; j++) {
            char ch = (name[j] >= 'A' && name[j] <= 'Z')
                        ? (char)(name[j] + 32) : name[j];
            luaL_addchar(&buf, ch);
        }
        luaL_pushresult(&buf);
        lua_pushstring(L, resp->headers[i].value);
        lua_settable(L, -3);
    }
    lua_setfield(L, -2, "headers");
}

/* http.fetch(method, url, opts?) — async non-blocking HTTP request.
 * Yields the coroutine; event loop drives socket I/O via KlWatcher.
 * Returns { status, body, headers } on resume. */
static int lua_http_fetch(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");
    if (!lua->server || !lua->active_conn)
        return luaL_error(L, "http.fetch() can only be called from a request handler");

    const char *method = luaL_checkstring(L, 1);
    const char *url = luaL_checkstring(L, 2);

    const char *body = NULL;
    size_t body_len = 0;
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse optional opts table at position 3 */
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "body");
        if (lua_isstring(L, -1))
            body = lua_tolstring(L, -1, &body_len);
        lua_pop(L, 1);

        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                    lua->scratch);
        }
        lua_pop(L, 1);
    }

    /* Start the async HTTP request — checks allowlist, creates KlClient,
     * creates HlAsyncCtx, and suspends the inbound connection */
    HlAsyncCtx *ctx = hl_async_http_start(
        lua->server, lua->active_conn, lua->base.alloc,
        lua->base.http_cfg, method, url, headers, num_headers, body, body_len);
    if (!ctx)
        return luaL_error(L, "http.fetch: failed to start request");

    /* Wire the Lua continuation */
    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *lua, HlAllocator *alloc,
                                                   HlLuaPushResultFn push_result);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                   lua_push_async_http_response);
    if (!cont) {
        /* Connection was already suspended — we can't easily undo that.
         * The cancel callback will clean up when the connection times out. */
        return luaL_error(L, "http.fetch: out of memory");
    }
    ctx->cont = cont;

    /* Yield the coroutine — on resume, the driver result
     * will be pushed onto the stack by lua_push_async_http_response */
    return lua_yieldk(L, 0, 0, NULL);
}

/* ── http.async.* — async HTTP convenience methods ─────────────────── */

/* http.async.request(method, url, opts?) — same as http.fetch */
static int lua_http_async_request(lua_State *L)
{
    return lua_http_fetch(L);
}

/* Helper: insert method string at position 1 and delegate to lua_http_fetch.
 * Caller's args are (url, ...) → becomes (method, url, ...) */
static int lua_http_async_method(lua_State *L, const char *method)
{
    lua_pushstring(L, method);
    lua_insert(L, 1);
    return lua_http_fetch(L);
}

/* http.async.get(url, opts?) */
static int lua_http_async_get(lua_State *L)
{
    return lua_http_async_method(L, "GET");
}

/* http.async.post(url, body, opts?) */
static int lua_http_async_post(lua_State *L)
{
    return lua_http_async_method(L, "POST");
}

/* http.async.put(url, body, opts?) */
static int lua_http_async_put(lua_State *L)
{
    return lua_http_async_method(L, "PUT");
}

/* http.async.patch(url, body, opts?) */
static int lua_http_async_patch(lua_State *L)
{
    return lua_http_async_method(L, "PATCH");
}

/* http.async.delete(url, opts?) */
static int lua_http_async_delete(lua_State *L)
{
    return lua_http_async_method(L, "DELETE");
}

static const luaL_Reg http_async_funcs[] = {
    {"request", lua_http_async_request},
    {"get",     lua_http_async_get},
    {"post",    lua_http_async_post},
    {"put",     lua_http_async_put},
    {"patch",   lua_http_async_patch},
    {"delete",  lua_http_async_delete},
    {NULL, NULL}
};

static const luaL_Reg http_funcs[] = {
    {"request", lua_http_request},
    {"get",     lua_http_get},
    {"post",    lua_http_post},
    {"put",     lua_http_put},
    {"patch",   lua_http_patch},
    {"delete",  lua_http_delete},
    {NULL, NULL}
};

int luaopen_hull_http(lua_State *L)
{
    luaL_newlib(L, http_funcs);

    /* http.async sub-table */
    luaL_newlib(L, http_async_funcs);
    lua_setfield(L, -2, "async");

    return 1;
}
