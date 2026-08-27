/*
 * lua_bindings.c - Request/Response bridge to Lua 5.4
 *
 * Marshals Keel's KlRequest/KlResponse to Lua tables/userdata.
 * This file contains ONLY data marshaling - all enforcement logic
 * lives in hl_cap_* functions.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "hull/reqctx.h"
#include "hull/limits/core.h"
#include "hull/cap/body.h"
#include "hull/utils/compress.h"
#include "internal.h"  /* hl_lua_request_install_multipart */

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/request.h>
#include <keel/response.h>
#include <keel/router.h>
#include <keel/connection.h>  /* KlConn.fd for the peer address */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* get_hl_lua_from_L moved to runtime.c (always linked): it is a general
 * lua_State -> HlLua accessor with no HTTP dependency, and mod_tool.c (built in
 * every flavor, including pure-compute) now references it, so it must not live
 * in these HTTP-gated web bindings. Declared in internal.h. */

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

/* Numeric client IP from the connection's peer address, "" on failure.
 * Sibling copy in src/hull/runtime/js/bindings.c (hl_request_peer_ip_js).
 * Best-effort: getpeername fails on an already-closed connection or the
 * in-process test harness (no socket), in which case remote_addr is absent
 * and stdlib callers fall back per their own policy (per-user, "_anon"). */
static void request_peer_ip(KlRequest *req, char *buf, size_t buflen)
{
    buf[0] = '\0';
    KlConn *conn = kl_request_conn(req);
    if (!conn || conn->fd < 0) return;
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    if (getpeername(conn->fd, (struct sockaddr *)&ss, &slen) != 0) return;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &s4->sin_addr, buf, (socklen_t)buflen);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &s6->sin6_addr, buf, (socklen_t)buflen);
    }
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

    /* params - route params from Keel (e.g. :id → params.id) */
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

    /* remote_addr: numeric peer IP from the socket, absent (nil) when
     * unavailable. This is the un-spoofable client address; middleware that
     * gates on IP (session, audit-log, totp) uses it as the trusted source and
     * only consults X-Forwarded-For when the app opts into trust_proxy. */
    {
        char peer[INET6_ADDRSTRLEN];
        request_peer_ip(req, peer, sizeof peer);
        if (peer[0]) {
            lua_pushstring(L, peer);
            lua_setfield(L, -2, "remote_addr");
        }
    }

    /* body - extract from buffer reader if available. For streaming-
     * multipart routes the body_reader is the parkable wrapper (not a
     * buffer reader), so body is nil and req:multipart() is the only
     * way to read bytes - installed just below. */
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

    /* req.multipart() - only installed for streaming-multipart routes
     * (no-op otherwise). Defined in mod_request.c. */
    if (is_multipart_stream)
        hl_lua_request_install_multipart(L, get_hl_lua_from_L(L),
                                          req->body_reader);

    /* ctx - per-request context table (middleware → handler).
     * If req->ctx carries a native Lua ref, retrieve it directly;
     * if it carries a JSON string (from test dispatch), parse it;
     * otherwise start with an empty table. */
    if (req->ctx) {
        HlReqCtx *rctx = (HlReqCtx *)req->ctx;
        if (rctx->kind == HL_REQCTX_LUA_REF) {
            /* Native Lua table - retrieve directly from registry */
            lua_rawgeti(L, LUA_REGISTRYINDEX, rctx->lua_ref);
        } else if (rctx->kind == HL_REQCTX_JSON) {
            /* JSON string (from test dispatch) - parse it via the
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
            lua_newtable(L); /* unknown kind - empty ctx */
        }
    } else {
        lua_newtable(L);
    }
    lua_setfield(L, -2, "ctx");
}
