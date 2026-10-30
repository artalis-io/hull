/* mod_http.c - hull.http module: HTTP client, sync and async
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "refusal.h"
#include "mod_buffer.h"
#include "hull/cap/http.h"
#include "hull/cap/http_async.h"
#include "hull/shared/async.h"
#include "internal.h"   /* hl_lua_check_can_wait */
#include "hull/shared/work_charge.h"   /* hl_work_bytes_units */

#include <keel/http_server.h>

#include <sh_arena.h>
#include <limits.h>
#include <stdio.h>
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
 *
 * opts.timeout_ms: ONE deadline for the whole request (DNS, connect, TLS,
 * send, receive, redirects). Absent = manifest http.timeout_ms, else 30 s.
 * Clamped to 60 s for the sync calls (they block the event loop) and to
 * 10 min for http.fetch / http.async.* (hull/limits/http.h).
 * ════════════════════════════════════════════════════════════════════ */

/* opts.timeout_ms of the opts table at @p idx (if it is one): an integer of
 * at least 1, returned as is (the cap layer clamps it to the path's ceiling);
 * absent / nil = 0, the app default. Anything else raises: a mistyped
 * timeout must not silently become the default. */
static int lua_http_timeout_opt(lua_State *L, int idx, const char *fn)
{
    if (!lua_istable(L, idx))
        return 0;
    lua_getfield(L, idx, "timeout_ms");
    int t = 0;
    if (!lua_isnil(L, -1)) {
        int isint = 0;
        lua_Integer v = lua_type(L, -1) == LUA_TNUMBER
                        ? lua_tointegerx(L, -1, &isint) : 0;
        if (!isint || v < 1)
            return luaL_error(L, "%s: opts.timeout_ms must be a positive "
                                 "integer (milliseconds)", fn);
        t = v > INT_MAX ? INT_MAX : (int)v;
    }
    lua_pop(L, 1);
    return t;
}

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

    if (!scratch)   /* sh_arena_calloc would fail the parse all the same */
        return -1;
    size_t mark = scratch->used;   /* given back on failure */
    HlHttpHeader *hdrs = sh_arena_calloc(scratch, (size_t)count, sizeof(HlHttpHeader));
    if (!hdrs)
        return -1;

    /* Nothing in this walk may allocate on the Lua heap (audit 24):
     * lua_tolstring on a NUMBER value converts it in place, which can run
     * the collector - and an app __gc finalizer that adds keys to this
     * table, so the walk met more entries than it counted and wrote past
     * hdrs. A number is formatted into a C buffer instead, and the walk
     * fails past `count` entries (the table changed under it). */
    int i = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        /* A string KEY only: lua_tolstring converts a number key in place,
         * and lua_next then fails on the changed key ("invalid key to
         * 'next'"). */
        int vt = lua_type(L, -1);
        if (lua_type(L, -2) == LUA_TSTRING
            && (vt == LUA_TSTRING || vt == LUA_TNUMBER)) {
            if (i >= count) {
                lua_pop(L, 2);
                scratch->used = mark;   /* the copies so far (audit 26) */
                return -1;
            }
            size_t nlen, vlen;
            char num[64];
            const char *n = lua_tolstring(L, -2, &nlen);
            const char *v;
            if (vt == LUA_TSTRING) {
                v = lua_tolstring(L, -1, &vlen);
            } else {
                int w;
                if (lua_isinteger(L, -1)) {
                    w = snprintf(num, sizeof(num), LUA_INTEGER_FMT,
                                 (LUAI_UACINT)lua_tointeger(L, -1));
                } else {
                    w = snprintf(num, sizeof(num), LUA_NUMBER_FMT,
                                 (LUAI_UACNUMBER)lua_tonumber(L, -1));
                    /* As Lua's tostring: a float that reads as an integer
                     * keeps a ".0". */
                    if (w > 0 && (size_t)w + 2 < sizeof(num)
                        && num[strspn(num, "-0123456789")] == '\0') {
                        num[w++] = '.';
                        num[w++] = '0';
                        num[w] = '\0';
                    }
                }
                if (w < 0 || (size_t)w >= sizeof(num)) {
                    lua_pop(L, 2);
                    scratch->used = mark;   /* as above (audit 26) */
                    return -1;
                }
                v = num;
                vlen = (size_t)w;
            }
            char *nc = sh_arena_alloc(scratch, nlen + 1);
            char *vc = sh_arena_alloc(scratch, vlen + 1);
            if (!nc || !vc) {
                /* Out of scratch space: fail, never send the request without
                 * the header (an Authorization quietly dropped). */
                lua_pop(L, 2);   /* value and key: the traversal ends here */
                scratch->used = mark;
                return -1;
            }
            memcpy(nc, n, nlen + 1);
            memcpy(vc, v, vlen + 1);
            hdrs[i].name = nc;
            hdrs[i].value = vc;
            i++;
        }
        lua_pop(L, 1); /* pop value, keep key */
    }

    /* Raises: the headers are in the scratch arena, nothing else is held. */
    const char *bad = hl_http_reserved_header(hdrs, i);
    if (bad) {
        scratch->used = mark;   /* nothing else holds it */
        luaL_error(L, "http: header '%s' may not be set (Hull's client writes "
                   "it, or it is not a header name)", bad);
    }

    *out_headers = hdrs;
    *out_count = i;
    return 0;
}

/* Give back the scratch a sync call's headers took, once the request is
 * done: the arena is reset only when a run starts, so a run making
 * requests in a loop filled it, and every later scratch user of the run
 * (db, smtp, a module load) failed (audit 23, after #812). @p headers is
 * the parse's first allocation. */
static void lua_http_scratch_release(HlLua *lua, const HlHttpHeader *headers)
{
    SHArena *a = lua ? lua->scratch : NULL;
    const char *p = (const char *)headers;
    if (!a || !p || p < a->buffer || (size_t)(p - a->buffer) > a->used)
        return;
    a->used = (size_t)(p - a->buffer);
}

/* Push HTTP response as Lua table: { status, body, headers } */
static void lua_push_http_response(lua_State *L, const KlHttpClientResponse *resp)
{
    lua_newtable(L);

    lua_pushinteger(L, resp->status);
    lua_setfield(L, -2, "status");

    if (resp->body && resp->body_len > 0)
        lua_pushlstring(L, resp->body, resp->body_len);
    else
        lua_pushstring(L, "");
    lua_setfield(L, -2, "body");

    /* Headers as { ["name"] = "value" } table, names lowercased on the Lua
     * stack (as the async response's are): in the scratch arena they took
     * from it for the rest of the run, and once it was full a name kept
     * its case, so resp.headers["location"] was silently nil (audit 23). */
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

/* lua_pcall body for lua_push_http_response: (resp) lightuserdata in. */
static int lua_push_http_response_k(lua_State *L)
{
    const KlHttpClientResponse *resp = lua_touserdata(L, 1);
    lua_settop(L, 0);
    lua_push_http_response(L, resp);
    return 1;
}

/* Push the response table and free the response, whatever happens: building
 * the table allocates (the body, every header), and a raise there skipped the
 * free that followed it - a leak of the whole response, up to its 4 MB cap,
 * outside the VM's memory limit, per failed call. */
static void lua_push_http_response_owned(lua_State *L, KlHttpClientResponse *resp)
{
    luaL_checkstack(L, 3, "http");
    lua_pushcfunction(L, lua_push_http_response_k);
    lua_pushlightuserdata(L, resp);
    int st = lua_pcall(L, 1, 1, 0);
    kl_http_client_response_free(resp);
    if (st != LUA_OK) lua_error(L);
}

/* The response, pushed and charged: its body was received, decompressed
 * and copied into the VM, 1 unit per 8 bytes (audit 39). Known only once it
 * arrived, so charged after; the request's floor was charged before. */
static void lua_push_http_response_charged(lua_State *L, KlHttpClientResponse *resp)
{
    size_t n = resp->body_len;
    lua_push_http_response_owned(L, resp);
    lua_hlwork(L, (size_t)hl_work_bytes_units((uint64_t)n), 0);
}

/* Every sync request costs DNS, a socket, a connect and maybe a TLS
 * handshake whatever its size: a floor charged before anything is held
 * (audit 39 - a refused connect in a pcall loop ran ~10 min per budget). */
static void lua_http_charge_request(lua_State *L)
{
    lua_hlwork(L, (size_t)HL_WORK_HTTP_REQUEST_UNITS, 0);
}

/* http.request(method, url, opts?) */
static int lua_http_request(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *method = luaL_checkstring(L, 1);
    const char *url = luaL_checkstring(L, 2);
    int timeout_ms = lua_http_timeout_opt(L, 3, "http.request");
    lua_http_charge_request(L);   /* audit 39 */

    const char *body = NULL;
    size_t body_len = 0;
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse optional opts table at position 3 */
    if (lua_istable(L, 3)) {
        /* Left on the stack: lua_tolstring converts a number in place and an
         * __index may return a fresh string, and once popped nothing referenced
         * it - parsing the headers below allocates, so it could be collected
         * and the freed bytes sent as the request body. */
        lua_getfield(L, 3, "body");
        if (lua_isstring(L, -1))
            body = lua_tolstring(L, -1, &body_len);
        /* Keel copies and frames the body: charged before the work, with
         * nothing held yet, as http.fetch's is (audit 28). */
        lua_hlwork(L, (size_t)hl_work_bytes_units((uint64_t)body_len), 0);

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

    KlHttpClientResponse resp = {0}; /* zero-init: resp.error is read on the rc!=0
                                  * path; don't depend on the cap layer's
                                  * internal memset ordering. */
    int rc = hl_cap_http_request(lua->base.http_cfg, timeout_ms, method, url,
                                    headers, num_headers, body, body_len, &resp);
    lua_http_scratch_release(lua, headers);
    if (rc != 0)
        return luaL_error(L, "http request failed: %s",
                          kl_strerror(resp.error));

    lua_push_http_response_charged(L, &resp);
    return 1;
}

/* http.get(url, opts?) */
static int lua_http_get(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *url = luaL_checkstring(L, 1);
    int timeout_ms = lua_http_timeout_opt(L, 2, "http.get");
    lua_http_charge_request(L);   /* audit 39 */
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            if (lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                       lua->scratch) != 0)
                return luaL_error(L, "invalid headers table (or out of scratch space)");
        }
        lua_pop(L, 1);
    }

    KlHttpClientResponse resp = {0}; /* zero-init: resp.error is read on the rc!=0
                                  * path; don't depend on the cap layer's
                                  * internal memset ordering. */
    int rc = hl_cap_http_request(lua->base.http_cfg, timeout_ms, "GET", url,
                                    headers, num_headers, NULL, 0, &resp);
    lua_http_scratch_release(lua, headers);
    if (rc != 0)
        return luaL_error(L, "http.get failed: %s", kl_strerror(resp.error));

    lua_push_http_response_charged(L, &resp);
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
    int timeout_ms = lua_http_timeout_opt(L, 3, "http.post/put/patch");
    /* Charged before the headers take scratch (audit 28; http.fetch's is). */
    lua_hlwork(L, (size_t)hl_work_http_request_units(body_len), 0);

    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            if (lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                       lua->scratch) != 0)
                return luaL_error(L, "invalid headers table (or out of scratch space)");
        }
        lua_pop(L, 1);
    }

    KlHttpClientResponse resp = {0}; /* zero-init: resp.error is read on the rc!=0
                                  * path; don't depend on the cap layer's
                                  * internal memset ordering. */
    int rc = hl_cap_http_request(lua->base.http_cfg, timeout_ms, method, url,
                                    headers, num_headers, body, body_len, &resp);
    lua_http_scratch_release(lua, headers);
    if (rc != 0)
        return luaL_error(L, "http.%s failed: %s", method,
                          kl_strerror(resp.error));

    lua_push_http_response_charged(L, &resp);
    return 1;
}

static int lua_http_post(lua_State *L)   { return lua_http_body_method(L, "POST"); }
static int lua_http_put(lua_State *L)    { return lua_http_body_method(L, "PUT"); }
static int lua_http_patch(lua_State *L)  { return lua_http_body_method(L, "PATCH"); }

/* http.delete(url, opts?) - same signature as http.get */
static int lua_http_delete(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");

    const char *url = luaL_checkstring(L, 1);
    int timeout_ms = lua_http_timeout_opt(L, 2, "http.delete");
    lua_http_charge_request(L);   /* audit 39 */
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            if (lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                       lua->scratch) != 0)
                return luaL_error(L, "invalid headers table (or out of scratch space)");
        }
        lua_pop(L, 1);
    }

    KlHttpClientResponse resp = {0}; /* zero-init: resp.error is read on the rc!=0
                                  * path; don't depend on the cap layer's
                                  * internal memset ordering. */
    int rc = hl_cap_http_request(lua->base.http_cfg, timeout_ms, "DELETE", url,
                                    headers, num_headers, NULL, 0, &resp);
    lua_http_scratch_release(lua, headers);
    if (rc != 0)
        return luaL_error(L, "http.delete failed: %s",
                          kl_strerror(resp.error));

    lua_push_http_response_charged(L, &resp);
    return 1;
}

/* ── Push async HTTP response onto Lua stack ──────────────────────── */

static void lua_push_async_http_response(lua_State *L, void *driver)
{
    const KlHttpClientResponse *resp = hl_http_async_response(driver);
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

    /* Headers as { ["name"] = "value" } - lowercase names */
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

/* http.fetch(method, url, opts?) - async non-blocking HTTP request.
 * Yields the coroutine; event loop drives socket I/O via KlWatcher.
 * Returns { status, body, headers } on resume. */
static int lua_http_fetch(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.http_cfg)
        return luaL_error(L, "http not configured (no hosts in manifest)");
    if (!lua->base.async_ctx)
        return luaL_error(L, "http.fetch() requires an active event loop");

    const char *method = luaL_checkstring(L, 1);
    const char *url = luaL_checkstring(L, 2);
    int timeout_ms = lua_http_timeout_opt(L, 3, "http.fetch");
    lua_http_charge_request(L);   /* the same floor per start (audit 39) */

    const char *body = NULL;
    size_t body_len = 0;
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse optional opts table at position 3 */
    if (lua_istable(L, 3)) {
        /* Left on the stack: lua_tolstring converts a number in place and an
         * __index may return a fresh string, and once popped nothing referenced
         * it - parsing the headers below allocates, so it could be collected
         * and the freed bytes sent as the request body. */
        lua_getfield(L, 3, "body");
        if (lua_isstring(L, -1))
            body = lua_tolstring(L, -1, &body_len);
        /* Keel copies and frames the body: charged before the work, with
         * nothing held yet, as http.fetch's is (audit 28). */
        lua_hlwork(L, (size_t)hl_work_bytes_units((uint64_t)body_len), 0);

        lua_getfield(L, 3, "headers");
        if (lua_istable(L, -1)) {
            int hdr_idx = lua_gettop(L);
            if (lua_parse_http_headers(L, hdr_idx, &headers, &num_headers,
                                       lua->scratch) != 0)
                return luaL_error(L, "invalid headers table (or out of scratch space)");
        }
        lua_pop(L, 1);
    }

    /* After the opts (their __index is app code that could BEGIN), before
     * anything is armed; nothing below runs app code (audit 15). */
    /* The header copies in the scratch arena are given back on every exit
     * that does not start the request, and once it has started (Keel
     * serializes them into its request buffer, a redirect client deep-copies
     * them): the arena is reset only when a run starts, so a pcall'd loop of
     * refused fetches filled it (audit 25). */
    if (!hl_lua_can_wait(L)) {
        lua_http_scratch_release(lua, headers);
        hl_lua_check_can_wait(L, "http.fetch()");
    }

    /* Keel copies the body and keeps it until the request is done: charged
     * and held on the VM's off-heap total under its heap limit - N fetches
     * in flight held N copies of one big body (audit 22). Keel also buffers
     * each response (up to max_response_size) off the heap, so at most
     * HL_HTTP_MAX_INFLIGHT fetches are in flight: those buffers are bounded
     * by that count x max_response_size (audit 31). Both raise with nothing
     * allocated, and run no app code. */
    lua_hlwork(L, (size_t)hl_work_bytes_units((uint64_t)body_len), 0);
    HlOffheapHold body_hold = {0};
    int admit = hl_http_fetch_admit(lua->base.http_inflight, &body_hold,
                                    &lua->wasm_buf_bytes, lua->mem_limit,
                                    body_len);
    if (admit != HL_HTTP_ADMIT_OK) {
        lua_http_scratch_release(lua, headers);
        if (admit == HL_HTTP_ADMIT_INFLIGHT)
            return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "http.fetch: too many requests in flight "
                              "(at most %d per VM)", HL_HTTP_MAX_INFLIGHT);
        return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "http.fetch: a %I-byte body would take the VM's "
                          "off-heap copies past its heap limit",
                          (lua_Integer)body_len);
    }

    /* The continuation first: created after the request was started (and
     * the connection suspended), a failed allocation left ctx->cont NULL,
     * and the completion dereferenced it. */
    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *lua, HlAllocator *alloc,
                                                   HlLuaPushResultFn push_result);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                   lua_push_async_http_response);
    if (!cont) {
        hl_offheap_drop(&body_hold);
        lua_http_scratch_release(lua, headers);
        return luaL_error(L, "http.fetch: out of memory");
    }

    /* Start the async HTTP request - checks allowlist, creates KlHttpClient,
     * creates HlAsyncCtx, and suspends the inbound connection */
    HlAsyncCtx *ctx = hl_async_http_start(
        lua->server, lua->active_conn, lua->base.net_ctx, lua->base.alloc,
        lua->base.http_cfg, timeout_ms, method, url, headers, num_headers,
        body, body_len);
    lua_http_scratch_release(lua, headers);   /* copied, or refused */
    if (!ctx) {
        hl_offheap_drop(&body_hold);
        cont->destroy(cont);
        return luaL_error(L, "http.fetch: failed to start request");
    }
    ctx->cont = cont;
    ctx->hold = body_hold;   /* given back when the ctx goes */
    ctx->inflight = &lua->base.http_inflight;   /* audit 31 */
    lua->base.http_inflight++;
    /* No request holds a detached fetch: teardown cancels it (audit 14). */
    if (ctx->detached)
        hl_async_ctx_track(ctx, &lua->base.async_live);

    /* Yield the coroutine - on resume, the driver result
     * will be pushed onto the stack by lua_push_async_http_response */
    return lua_yieldk(L, 0, 0, NULL);
}

/* ── http.async.* - async HTTP convenience methods ─────────────────── */

/* http.async.request(method, url, opts?) - same as http.fetch */
static int lua_http_async_request(lua_State *L)
{
    return lua_http_fetch(L);
}

/* Helper for no-body methods (GET/DELETE).
 * Caller's args are (url, opts?) → becomes (method, url, opts?). */
static int lua_http_async_no_body(lua_State *L, const char *method)
{
    lua_pushstring(L, method);
    lua_insert(L, 1);
    return lua_http_fetch(L);
}

/* Helper for body-bearing methods (POST/PUT/PATCH).
 *
 * Caller's args are (url, body?, opts?) - body is a string (or nil),
 * opts is a table with `headers`. The async path delegates to
 * lua_http_fetch which takes (method, url, opts), so we must fold
 * the positional body into a synthesized opts.body BEFORE delegating.
 * Without this fold, the body silently never reaches the wire - and
 * a typed `Content-Type` header still rides along, which makes the
 * bug invisible in network captures (the empty POST looks like an
 * intended ping). Mirrors js_http_async_with_body in mod_http_client.c
 * on the JS side. */
static int lua_http_async_with_body(lua_State *L, const char *method)
{
    /* Snapshot the URL + body + caller-opts before we rearrange. */
    const char *url = luaL_checkstring(L, 1);
    int has_body = lua_isstring(L, 2);
    int has_user_opts = lua_istable(L, 3);

    /* Build the merged opts table at the top of the stack. */
    lua_newtable(L);                  /* opts */
    if (has_body) {
        lua_pushvalue(L, 2);          /* opts, body */
        lua_setfield(L, -2, "body");  /* opts.body = body */
    }
    if (has_user_opts) {
        /* Copy user-supplied opts (headers etc.) into the new table,
         * but never let user-supplied `body` override the positional
         * one - positional wins, like the sync POST helper. */
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {  /* opts, key, val */
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                int keep = !has_body || strcmp(k, "body") != 0;
                if (keep) {
                    lua_pushvalue(L, -2);  /* opts, key, val, key */
                    lua_pushvalue(L, -2);  /* opts, key, val, key, val */
                    lua_settable(L, -5);   /* opts[key] = val */
                }
            }
            lua_pop(L, 1);              /* opts, key */
        }
    }
    /* Stack now: url, body?, user_opts?, merged_opts */
    /* Drop everything except url + merged_opts, then prepend method.  */
    lua_replace(L, 2);                /* url, merged_opts (drops body) */
    /* If user_opts was present at slot 3, the replace above shifted
     * merged_opts to slot 2 and slot 3 still holds the old user_opts.
     * Pop until only (url, merged_opts) remain. */
    while (lua_gettop(L) > 2) lua_remove(L, 3);

    lua_pushstring(L, method);
    lua_insert(L, 1);                 /* method, url, merged_opts */
    (void)url;                        /* silence: only snapshot to assert */
    return lua_http_fetch(L);
}

/* http.async.get(url, opts?) */
static int lua_http_async_get(lua_State *L)
{
    return lua_http_async_no_body(L, "GET");
}

/* http.async.post(url, body, opts?) */
static int lua_http_async_post(lua_State *L)
{
    return lua_http_async_with_body(L, "POST");
}

/* http.async.put(url, body, opts?) */
static int lua_http_async_put(lua_State *L)
{
    return lua_http_async_with_body(L, "PUT");
}

/* http.async.patch(url, body, opts?) */
static int lua_http_async_patch(lua_State *L)
{
    return lua_http_async_with_body(L, "PATCH");
}

/* http.async.delete(url, opts?) */
static int lua_http_async_delete(lua_State *L)
{
    return lua_http_async_no_body(L, "DELETE");
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
