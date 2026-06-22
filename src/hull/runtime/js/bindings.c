/*
 * js_bindings.c — Request/Response bridge to QuickJS
 *
 * Marshals Keel's KlRequest/KlResponse to JS objects and back.
 * This file contains ONLY data marshaling — all enforcement logic
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

#include <keel/request.h>
#include <keel/response.h>
#include <keel/router.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Request object ─────────────────────────────────────────────────── */

/* req.header(name) — case-insensitive header lookup.
 * Since headers are already stored lowercase, this lowercases the
 * input name and looks it up in req.headers. */
static JSValue js_req_header(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;

    /* Lowercase the lookup key — reject names that exceed buffer */
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

JSValue hl_js_make_request(JSContext *ctx, KlRequest *req)
{
    JSValue obj = JS_NewObject(ctx);

    /* method (Keel stores as string).  All reads via kl_request_*
     * accessors so they route through req->sealed (mprotect-RO) when
     * KEEL_SEAL_REQUEST=1 is in the Keel build, or fall back to direct
     * fields otherwise.  See vendor/keel/include/keel/request.h. */
    const char *m = kl_request_method(req);
    if (m)
        JS_SetPropertyStr(ctx, obj, "method",
                          JS_NewStringLen(ctx, m, kl_request_method_len(req)));
    else
        JS_SetPropertyStr(ctx, obj, "method", JS_NewString(ctx, "GET"));

    /* path */
    const char *p = kl_request_path(req);
    if (p)
        JS_SetPropertyStr(ctx, obj, "path",
                          JS_NewStringLen(ctx, p, kl_request_path_len(req)));
    else
        JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, "/"));

    /* query string → object */
    JSValue query_obj = JS_NewObject(ctx);
    const char *q = kl_request_query(req);
    size_t q_len = kl_request_query_len(req);
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

    /* params — route params from Keel (e.g. :id → params.id) */
    JSValue params_obj = JS_NewObject(ctx);
    int n_params = kl_request_num_params(req);
    for (int i = 0; i < n_params; i++) {
        KlParam param = kl_request_param_at(req, i);
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
    int n_headers = kl_request_num_headers(req);
    for (int i = 0; i < n_headers; i++) {
        KlHeader hdr = kl_request_header_at(req, i);
        if (hdr.name && hdr.value) {
            char name_buf[256];
            size_t nlen = hdr.name_len;
            if (nlen >= sizeof(name_buf)) continue; /* skip oversized names */
            for (size_t j = 0; j < nlen; j++) {
                unsigned char c = (unsigned char)hdr.name[j];
                name_buf[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
            }
            name_buf[nlen] = '\0';
            JS_SetPropertyStr(ctx, headers_obj, name_buf,
                              JS_NewStringLen(ctx, hdr.value, hdr.value_len));
        }
    }
    JS_SetPropertyStr(ctx, obj, "headers", headers_obj);

    /* body — extract from buffer reader if available. Streaming-multipart
     * routes have a wrapper reader (not a KlBufReader), so body = null
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

    /* req.multipart() — only installed for streaming-multipart routes.
     * Defined in mod_request.c. */
    if (is_multipart_stream)
        hl_js_request_install_multipart(ctx, obj, req->body_reader);

    /* ctx — per-request context object (middleware → handler).
     * If req->ctx carries a native JS ref, retrieve it directly;
     * if it carries a JSON string (from test dispatch), parse it;
     * otherwise start with an empty object. */
    if (req->ctx) {
        HlReqCtx *rctx = (HlReqCtx *)req->ctx;
        if (rctx->kind == HL_REQCTX_JS_VAL) {
            /* Native JS object — reconstruct JSValue from stored bytes */
            JSValue val;
            memcpy(&val, rctx->js_val_bytes, sizeof(val));
            JS_SetPropertyStr(ctx, obj, "ctx", JS_DupValue(ctx, val));
        } else if (rctx->kind == HL_REQCTX_JSON) {
            /* JSON string (from test dispatch) — parse it */
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

    /* req.header(name) — convenience method for case-insensitive lookup */
    JS_SetPropertyStr(ctx, obj, "header",
                      JS_NewCFunction(ctx, js_req_header, "header", 1));

    return obj;
}

/* ── Response object ────────────────────────────────────────────────── */

/*
 * Response is a JS object with C function methods that write to
 * KlResponse. The KlResponse pointer is stored as opaque data.
 *
 * Methods:
 *   res.status(code)        → set status (chainable)
 *   res.header(name, val)   → add header (chainable)
 *   res.json(data, code?)   → send JSON response
 *   res.html(str)           → send HTML response
 *   res.text(str)           → send text response
 *   res.redirect(url, code) → HTTP redirect
 */

static void hl_response_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    (void)val;
    /* KlResponse is owned by the connection pool, not by JS */
}

static JSClassDef hl_response_class = {
    "HlResponse",
    .finalizer = hl_response_finalizer,
};

static KlResponse *get_response(JSContext *ctx, JSValueConst this_val)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    return (KlResponse *)JS_GetOpaque(this_val, (JSClassID)js->response_class_id);
}

/* Has a header with this name (case-insensitive) already been added to
 * the response? Used by js_res_html to avoid stamping Hull's default
 * CSP on top of one already set by application middleware — without
 * this the browser sees two Content-Security-Policy headers and
 * enforces the strict intersection, which typically blocks the page's
 * own scripts. Scans res->hdr_buf line by line; headers are appended
 * as "Name: value\r\n" by kl_response_header. Sibling of Lua's
 * hl_response_has_header in src/hull/runtime/lua/bindings.c. */
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

/* res.status(code) */
static JSValue js_res_status(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    KlResponse *res = get_response(ctx, this_val);
    if (!res || argc < 1)
        return JS_EXCEPTION;

    int32_t code;
    if (JS_ToInt32(ctx, &code, argv[0]))
        return JS_EXCEPTION;

    kl_response_status(res, code);
    return JS_DupValue(ctx, this_val); /* chainable */
}

/* res.header(name, value) */
static JSValue js_res_header(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    KlResponse *res = get_response(ctx, this_val);
    if (!res || argc < 2)
        return JS_EXCEPTION;

    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);

    if (name && value)
        kl_response_header(res, name, value);

    if (value) JS_FreeCString(ctx, value);
    if (name) JS_FreeCString(ctx, name);

    return JS_DupValue(ctx, this_val); /* chainable */
}

/* res.json(data, code?) */
static JSValue js_res_json(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    KlResponse *res = get_response(ctx, this_val);
    if (!res || argc < 1)
        return JS_EXCEPTION;

    /* Optional status code */
    if (argc >= 2) {
        int32_t code;
        if (!JS_ToInt32(ctx, &code, argv[1]))
            kl_response_status(res, code);
    }

    /* JSON.stringify the data */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue stringify = JS_GetPropertyStr(ctx, json_obj, "stringify");

    JSValue result = JS_Call(ctx, stringify, json_obj, 1, (JSValue *)argv);

    if (!JS_IsException(result)) {
        const char *json_str = JS_ToCString(ctx, result);
        if (json_str) {
            size_t json_len = strlen(json_str);
            HlJS *js_rt = (HlJS *)JS_GetContextOpaque(ctx);
            kl_response_header(res, "Content-Type", "application/json");
            hl_maybe_compress(js_rt ? js_rt->active_req : NULL, res,
                              js_rt ? js_rt->base.compress : NULL,
                              json_str, json_len);
            JS_FreeCString(ctx, json_str);
        }
    }

    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, stringify);
    JS_FreeValue(ctx, json_obj);
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

/* res.html(string) */
static JSValue js_res_html(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    KlResponse *res = get_response(ctx, this_val);
    if (!res || argc < 1)
        return JS_EXCEPTION;

    const char *html = JS_ToCString(ctx, argv[0]);
    if (html) {
        size_t html_len = strlen(html);
        HlJS *js_rt = (HlJS *)JS_GetContextOpaque(ctx);
        kl_response_header(res, "Content-Type", "text/html; charset=utf-8");
        /* Skip the default CSP if middleware already wrote one — two
         * CSP headers cause browsers to enforce the strict intersection
         * (typically blocking the page's own scripts). The app-supplied
         * one wins. */
        if (js_rt && js_rt->base.csp_policy &&
            !hl_response_has_header(res, "Content-Security-Policy"))
            kl_response_header(res, "Content-Security-Policy",
                               js_rt->base.csp_policy);
        hl_maybe_compress(js_rt ? js_rt->active_req : NULL, res,
                          js_rt ? js_rt->base.compress : NULL,
                          html, html_len);
        JS_FreeCString(ctx, html);
    }

    return JS_UNDEFINED;
}

/* res.text(string) */
static JSValue js_res_text(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    KlResponse *res = get_response(ctx, this_val);
    if (!res || argc < 1)
        return JS_EXCEPTION;

    const char *text = JS_ToCString(ctx, argv[0]);
    if (text) {
        size_t text_len = strlen(text);
        HlJS *js_rt = (HlJS *)JS_GetContextOpaque(ctx);
        kl_response_header(res, "Content-Type", "text/plain; charset=utf-8");
        hl_maybe_compress(js_rt ? js_rt->active_req : NULL, res,
                          js_rt ? js_rt->base.compress : NULL,
                          text, text_len);
        JS_FreeCString(ctx, text);
    }

    return JS_UNDEFINED;
}

/* res.bytes(buf) — binary-safe response primitive.
 *
 * Accepts an ArrayBuffer, TypedArray, or string. Does NOT set
 * Content-Type (caller's responsibility — binary content can be
 * anything) and does NOT route through hl_maybe_compress (avoids
 * gzipping already-compressed payloads + keeps the response bytes
 * identical to a downstream SHA / ETag check). The body is copied
 * into a response-owned buffer so the JS value can be GC'd safely. */
static JSValue js_res_bytes(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    KlResponse *res = get_response(ctx, this_val);
    if (!res || argc < 1)
        return JS_EXCEPTION;

    HlBufferView view = {0};
    const char *str = NULL;
    int needs_free = 0;
    if (!js_get_buffer(ctx, argv[0], &view, &str, &needs_free))
        return JS_ThrowTypeError(ctx,
            "res.bytes: expected ArrayBuffer, TypedArray, or string");

    int rc = kl_response_body_copy(res, (const char *)view.data, view.len);
    if (needs_free && str) JS_FreeCString(ctx, str);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "res.bytes: out of memory");
    return JS_UNDEFINED;
}

/* res.redirect(url, code?) */
static JSValue js_res_redirect(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    KlResponse *res = get_response(ctx, this_val);
    if (!res || argc < 1)
        return JS_EXCEPTION;

    int32_t code = 302; /* default */
    if (argc >= 2)
        JS_ToInt32(ctx, &code, argv[1]);

    const char *url = JS_ToCString(ctx, argv[0]);
    if (url) {
        kl_response_status(res, code);
        kl_response_header(res, "Location", url);
        kl_response_body_borrow(res, "", 0);
        JS_FreeCString(ctx, url);
    }

    return JS_UNDEFINED;
}

/* ── Response class registration ────────────────────────────────────── */

static int hl_js_ensure_response_class(HlJS *js)
{
    if (js->response_class_registered)
        return 0;

    JSClassID class_id = 0;
    JS_NewClassID(&class_id);
    js->response_class_id = (uint32_t)class_id;

    JSRuntime *rt = JS_GetRuntime(js->ctx);
    if (JS_NewClass(rt, class_id, &hl_response_class) < 0)
        return -1;

    /* Create prototype with methods */
    JSValue proto = JS_NewObject(js->ctx);
    JS_SetPropertyStr(js->ctx, proto, "status",
                      JS_NewCFunction(js->ctx, js_res_status, "status", 1));
    JS_SetPropertyStr(js->ctx, proto, "header",
                      JS_NewCFunction(js->ctx, js_res_header, "header", 2));
    JS_SetPropertyStr(js->ctx, proto, "json",
                      JS_NewCFunction(js->ctx, js_res_json, "json", 2));
    JS_SetPropertyStr(js->ctx, proto, "html",
                      JS_NewCFunction(js->ctx, js_res_html, "html", 1));
    JS_SetPropertyStr(js->ctx, proto, "text",
                      JS_NewCFunction(js->ctx, js_res_text, "text", 1));
    JS_SetPropertyStr(js->ctx, proto, "bytes",
                      JS_NewCFunction(js->ctx, js_res_bytes, "bytes", 1));
    JS_SetPropertyStr(js->ctx, proto, "redirect",
                      JS_NewCFunction(js->ctx, js_res_redirect, "redirect", 2));

    JS_SetClassProto(js->ctx, class_id, proto);
    js->response_class_registered = 1;

    return 0;
}

/* ── Public: create JS request/response objects ─────────────────────── */

JSValue hl_js_make_response(HlJS *js, KlResponse *res)
{
    if (hl_js_ensure_response_class(js) != 0)
        return JS_ThrowInternalError(js->ctx, "failed to register Response class");

    JSValue obj = JS_NewObjectClass(js->ctx, (int)js->response_class_id);
    JS_SetOpaque(obj, res);
    return obj;
}
