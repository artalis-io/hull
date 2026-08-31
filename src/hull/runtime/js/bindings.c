/*
 * js_bindings.c - Request/Response bridge to QuickJS
 *
 * Marshals Keel's KlHttpRequest/KlHttpResponse to JS objects and back.
 * This file contains ONLY data marshaling - all enforcement logic
 * lives in hl_cap_* functions.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "hull/reqctx.h"
#include "hull/limits/core.h"
#include "hull/cap/body.h"
#include "hull/utils/compress.h"
#include "mod_buffer.h"  /* js_get_buffer + HlBufferView (for res.bytes) */
#include "internal.h"  /* hl_js_request_install_multipart */
#include "quickjs.h"

_Static_assert(sizeof(JSValue) <= sizeof(((HlReqCtx *)0)->js_val_bytes),
               "JSValue too large for HlReqCtx.js_val_bytes");

#include <keel/http_request.h>
#include <keel/http_response.h>
#include <keel/http_router.h>
#include <keel/http_connection.h>  /* kl_http_conn_peer_addr */
#include <keel/sockaddr.h>         /* KlSockAddr, kl_sockaddr_format_ip */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Request object ─────────────────────────────────────────────────── */

/* req.header(name) - case-insensitive header lookup.
 * Since headers are already stored lowercase, this lowercases the
 * input name and looks it up in req.headers. */
static JSValue js_req_header(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;

    /* Lowercase the lookup key - reject names that exceed buffer */
    size_t len = strlen(name);
    char lower[256];
    if (len >= sizeof(lower)) {
        JS_FreeCString(ctx, name);
        return JS_UNDEFINED;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    lower[len] = '\0';
    JS_FreeCString(ctx, name);

    JSValue headers = JS_GetPropertyStr(ctx, this_val, "headers");
    if (JS_IsUndefined(headers) || JS_IsNull(headers))
        return JS_UNDEFINED;

    JSValue val = JS_GetPropertyStr(ctx, headers, lower);
    JS_FreeValue(ctx, headers);
    return val;
}

/*
 * Build a JS object representing the HTTP request:
 *   {
 *     method:  "GET",
 *     path:    "/invoices/42",
 *     params:  { id: "42" },
 *     query:   { limit: "10" },
 *     headers: { "content-type": "application/json" },
 *     body:    "..." or parsed object,
 *     ctx:     {}
 *   }
 */
/* Percent-decode a query-string token in place (also turns `+` into
 * space). Returns the new length. Invalid `%XX` (truncated or non-hex)
 * is left as-is so we never silently drop bytes from a malformed URL.
 * Mirrors hl_query_decode_inplace in src/hull/runtime/lua/bindings.c. */
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
 * Sibling copy in src/hull/runtime/lua/bindings.c (request_peer_ip). See there
 * for the rationale; best-effort, absent when unavailable. */
static void hl_request_peer_ip_js(KlHttpRequest *req, char *buf, size_t buflen)
{
    buf[0] = '\0';
    KlHttpConn *conn = kl_http_request_conn(req);
    if (!conn) return;
    /* KlHttpConn is opaque in Keel 3.x; peer address via the accessor + IP-only
     * format (matches the previous getpeername path). NULL when unavailable. */
    const KlSockAddr *pa = kl_http_conn_peer_addr(conn);
    if (!pa) return;
    kl_sockaddr_format_ip(pa, buf, buflen);
}

JSValue hl_js_make_request(JSContext *ctx, KlHttpRequest *req)
{
    JSValue obj = JS_NewObject(ctx);

    /* method (Keel stores as string).  All reads via kl_http_request_*
     * accessors so they route through req->sealed (mprotect-RO) when
     * KEEL_SEAL_REQUEST=1 is in the Keel build, or fall back to direct
     * fields otherwise.  See vendor/keel/include/keel/http_request.h. */
    const char *m = req->method;
    if (m)
        JS_SetPropertyStr(ctx, obj, "method",
                          JS_NewStringLen(ctx, m, req->method_len));
    else
        JS_SetPropertyStr(ctx, obj, "method", JS_NewString(ctx, "GET"));

    /* path */
    const char *p = req->path;
    if (p)
        JS_SetPropertyStr(ctx, obj, "path",
                          JS_NewStringLen(ctx, p, req->path_len));
    else
        JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, "/"));

    /* query string → object */
    JSValue query_obj = JS_NewObject(ctx);
    const char *q = req->query;
    size_t q_len = req->query_len;
    if (q && q_len > 0) {
        /* Parse query string: key=val&key2=val2 */
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
            char *val;
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
                vlen = hl_query_decode_inplace(val, vlen);
                val[vlen] = '\0';
            }
            JS_SetPropertyStr(ctx, query_obj, pair,
                              JS_NewStringLen(ctx, val, vlen));
            pair = strtok_r(NULL, "&", &saveptr);
        }
    }
    JS_SetPropertyStr(ctx, obj, "query", query_obj);

    /* params - route params from Keel (e.g. :id → params.id) */
    JSValue params_obj = JS_NewObject(ctx);
    int n_params = req->num_params;
    for (int i = 0; i < n_params; i++) {
        KlHttpParam param = req->params[i];
        char name[HL_PARAM_NAME_MAX];
        size_t nlen = param.name_len < HL_PARAM_NAME_MAX - 1
                      ? param.name_len : HL_PARAM_NAME_MAX - 1;
        memcpy(name, param.name, nlen);
        name[nlen] = '\0';
        JS_SetPropertyStr(ctx, params_obj, name,
            JS_NewStringLen(ctx, param.value, param.value_len));
    }
    JS_SetPropertyStr(ctx, obj, "params", params_obj);

    /* headers → object (names lowercased for case-insensitive lookup) */
    JSValue headers_obj = JS_NewObject(ctx);
    int n_headers = req->num_headers;
    for (int i = 0; i < n_headers; i++) {
        /* KlHttpRequest is a concrete struct in Keel 3.x; headers[] is read
         * directly (the sealed-request accessor layer was removed in Keel
         * v2.8.0). See docs/security.md § 4e. */
        if (req->headers[i].name && req->headers[i].value) {
            char name_buf[256];
            size_t nlen = req->headers[i].name_len;
            if (nlen >= sizeof(name_buf)) continue; /* skip oversized names */
            for (size_t j = 0; j < nlen; j++) {
                unsigned char c = (unsigned char)req->headers[i].name[j];
                name_buf[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
            }
            name_buf[nlen] = '\0';
            JS_SetPropertyStr(ctx, headers_obj, name_buf,
                              JS_NewStringLen(ctx, req->headers[i].value, req->headers[i].value_len));
        }
    }
    JS_SetPropertyStr(ctx, obj, "headers", headers_obj);

    /* remote_addr: numeric peer IP from the socket, absent when unavailable.
     * The un-spoofable client address; IP-gating middleware uses it as the
     * trusted source and consults X-Forwarded-For only under trust_proxy. */
    {
        char peer[INET6_ADDRSTRLEN];
        hl_request_peer_ip_js(req, peer, sizeof peer);
        if (peer[0])
            JS_SetPropertyStr(ctx, obj, "remote_addr", JS_NewString(ctx, peer));
    }

    /* body - extract from buffer reader if available. Streaming-multipart
     * routes have a wrapper reader (not a KlHttpBufReader), so body = null
     * and the handler iterates via req.multipart() (installed below). */
    int is_multipart_stream =
        req->body_reader != NULL &&
        hl_cap_multipart_inner(req->body_reader) != NULL;
    if (req->body_reader && !is_multipart_stream) {
        const char *data;
        size_t len = hl_cap_body_data(req->body_reader, &data);
        if (len > 0)
            JS_SetPropertyStr(ctx, obj, "body",
                              JS_NewStringLen(ctx, data, len));
        else
            JS_SetPropertyStr(ctx, obj, "body", JS_NewString(ctx, ""));
    } else {
        JS_SetPropertyStr(ctx, obj, "body", JS_NULL);
    }

    /* req.multipart() - only installed for streaming-multipart routes.
     * Defined in mod_request.c. */
    if (is_multipart_stream)
        hl_js_request_install_multipart(ctx, obj, req->body_reader);

    /* ctx - per-request context object (middleware → handler).
     * If req->ctx carries a native JS ref, retrieve it directly;
     * if it carries a JSON string (from test dispatch), parse it;
     * otherwise start with an empty object. */
    if (req->ctx) {
        HlReqCtx *rctx = (HlReqCtx *)req->ctx;
        if (rctx->kind == HL_REQCTX_JS_VAL) {
            /* Native JS object - reconstruct JSValue from stored bytes */
            JSValue val;
            memcpy(&val, rctx->js_val_bytes, sizeof(val));
            JS_SetPropertyStr(ctx, obj, "ctx", JS_DupValue(ctx, val));
        } else if (rctx->kind == HL_REQCTX_JSON) {
            /* JSON string (from test dispatch) - parse it */
            JSValue parsed = JS_ParseJSON(ctx, rctx->json.data,
                                          rctx->json.len, "<ctx>");
            if (JS_IsException(parsed)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                JS_SetPropertyStr(ctx, obj, "ctx", JS_NewObject(ctx));
            } else {
                JS_SetPropertyStr(ctx, obj, "ctx", parsed);
            }
        } else {
            JS_SetPropertyStr(ctx, obj, "ctx", JS_NewObject(ctx));
        }
    } else {
        JS_SetPropertyStr(ctx, obj, "ctx", JS_NewObject(ctx));
    }

    /* req.header(name) - convenience method for case-insensitive lookup */
    JS_SetPropertyStr(ctx, obj, "header",
                      JS_NewCFunction(ctx, js_req_header, "header", 1));

    return obj;
}
