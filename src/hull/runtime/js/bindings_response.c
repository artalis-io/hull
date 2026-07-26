/*
 * bindings_response.c — the res.* response helpers, extracted from bindings.c.
 *
 * Moved out (#114) so the core js_bindings.o holds ZERO Keel-response / compress
 * references (kl_response_*, hl_maybe_compress): those live only here, on the
 * HTTP side of the seam. Phase A: rides the js runtime archive; Phase C
 * relocates it into the composed `http` feature alongside http_register.c.
 * Sibling of the Lua bindings_response.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"      /* HlJS, KlResponse, hl_js_make_response */
#include "hull/utils/compress.h"  /* hl_maybe_compress */
#include "mod_buffer.h"           /* js_get_buffer + HlBufferView (res.bytes) */
#include "quickjs.h"

#include <keel/response.h>

#include <string.h>
#include <strings.h>  /* strncasecmp */

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
