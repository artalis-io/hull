/*
 * bindings_response.c - the res.* response helpers, extracted from bindings.c.
 *
 * Moved out (#114) so the core js_bindings.o holds ZERO Keel-response / compress
 * references (kl_http_response_*, hl_maybe_compress): those live only here, on the
 * HTTP side of the seam, composed into the `http` feature alongside
 * http_register.c.
 * Sibling of the Lua bindings_response.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "log.h"
#include "hull/runtime/js.h"      /* HlJS, KlHttpResponse, hl_js_make_response */
#include "hull/shared/req_life.h"
#include <stdlib.h>
#include "hull/utils/compress.h"  /* hl_maybe_compress */
#include "hull/http_feature.h"    /* hl_js_http_error_response (seam strong) */
#include "mod_buffer.h"           /* js_get_buffer + HlBufferView (res.bytes) */
#include "quickjs.h"

#include <keel/http_response.h>
#include <keel/http_connection.h>  /* kl_http_conn_response (finalize seam) */
#include <keel/http_request.h>     /* kl_http_request_send_response (finalize seam) */

#include <string.h>
#include <strings.h>  /* strncasecmp */

/* ── Response object ────────────────────────────────────────────────── */

/*
 * Response is a JS object with C function methods that write to
 * KlHttpResponse. The KlHttpResponse pointer is stored as opaque data.
 *
 * Methods:
 *   res.status(code)        → set status (chainable)
 *   res.header(name, val)   → add header (chainable)
 *   res.json(data, code?)   → send JSON response
 *   res.html(str)           → send HTML response
 *   res.text(str)           → send text response
 *   res.redirect(url, code) → HTTP redirect
 */

/* The object's opaque: the response (owned by the connection, not by JS) and
 * the life of the request it belongs to (shared/req_life.h). Used after its
 * request is over - stashed, or kept in a closure a timer runs - it fails
 * closed instead of writing into a response that was already sent, on a
 * connection that may be gone. */
typedef struct {
    KlHttpResponse *res;
    KlHttpRequest  *req;    /* its request: the Accept-Encoding it answers */
    HlReqLife      *life;   /* NULL: not tracked (always live) */
} HlJsResBox;

/* The class id, for the finalizer (which has no context to look it up in). */
static JSClassID g_response_class_id;

static void hl_response_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlJsResBox *box = (HlJsResBox *)JS_GetOpaque(val, g_response_class_id);
    if (!box) return;
    hl_req_life_release(box->life);
    free(box);
}

static const JSClassDef hl_response_class = {
    "HlResponse",
    .finalizer = hl_response_finalizer,
};

/* NULL - with a TypeError pending - once the response's request is over, or
 * for a receiver that is not a response. Every method checks the NULL and
 * returns JS_EXCEPTION (which with nothing thrown reached the app as `null`,
 * or as a stale error left pending earlier). */
static KlHttpResponse *get_response_box(JSContext *ctx, JSValueConst this_val,
                                        KlHttpRequest **req_out)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    HlJsResBox *box = (HlJsResBox *)JS_GetOpaque(this_val,
                                                 (JSClassID)js->response_class_id);
    if (!box || !box->res) {
        JS_ThrowTypeError(ctx, "res: not a response object");
        return NULL;
    }
    if (!hl_req_life_live(box->life)) {
        JS_ThrowTypeError(ctx, "res: the request this response belongs to has finished");
        return NULL;
    }
    if (req_out) *req_out = box->req;
    return box->res;
}

static KlHttpResponse *get_response(JSContext *ctx, JSValueConst this_val)
{
    return get_response_box(ctx, this_val, NULL);
}

/* Has a header with this name (case-insensitive) already been added to
 * the response? Used by js_res_html to avoid stamping Hull's default
 * CSP on top of one already set by application middleware - without
 * this the browser sees two Content-Security-Policy headers and
 * enforces the strict intersection, which typically blocks the page's
 * own scripts. Scans res->hdr_buf line by line; headers are appended
 * as "Name: value\r\n" by kl_http_response_header. Sibling of Lua's
 * hl_response_has_header in src/hull/runtime/lua/bindings.c. */
static int hl_response_has_header(KlHttpResponse *res, const char *name)
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
    KlHttpResponse *res = get_response(ctx, this_val);
    if (!res)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "res.status requires (code)");

    int32_t code;
    if (JS_ToInt32(ctx, &code, argv[0]))
        return JS_EXCEPTION;

    kl_http_response_status(res, code);
    return JS_DupValue(ctx, this_val); /* chainable */
}

/* res.header(name, value) */
static JSValue js_res_header(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    KlHttpResponse *res = get_response(ctx, this_val);
    if (!res)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "res.header requires (name, value)");

    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = name ? JS_ToCString(ctx, argv[1]) : NULL;
    if (!name || !value) {   /* a conversion threw: report it */
        if (name) JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    /* Rejected for a CR or LF (the header-injection guard). Not named in the
     * log: the name may be the part carrying the CR/LF. */
    if (kl_http_response_header(res, name, value) != 0)
        log_warn("[hull] res.header: a header was dropped - its name or value "
                 "contains CR or LF");

    JS_FreeCString(ctx, value);
    JS_FreeCString(ctx, name);

    return JS_DupValue(ctx, this_val); /* chainable */
}

/* res.json(data, code?) */
static JSValue js_res_json(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    KlHttpRequest *req = NULL;
    KlHttpResponse *res = get_response_box(ctx, this_val, &req);
    if (!res)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "res.json requires (data, code?)");

    /* Optional status code. A conversion that throws propagates before
     * anything is written: it used to send the body with the old status
     * and leave the exception pending (audit 8 L1). */
    if (argc >= 2) {
        int32_t code;
        if (JS_ToInt32(ctx, &code, argv[1]))
            return JS_EXCEPTION;
        kl_http_response_status(res, code);
    }

    /* JSON.stringify the data */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue stringify = JS_GetPropertyStr(ctx, json_obj, "stringify");

    JSValue result = JS_Call(ctx, stringify, json_obj, 1, (JSValue *)argv);
    JS_FreeValue(ctx, stringify);
    JS_FreeValue(ctx, json_obj);
    JS_FreeValue(ctx, global);

    /* A stringify that threw (a cycle, a BigInt, a throwing toJSON) or a
     * failed conversion propagates: it used to return undefined with the
     * exception still pending, and the response went out without a body. */
    if (JS_IsException(result))
        return JS_EXCEPTION;
    size_t json_len = 0;
    const char *json_str = JS_ToCStringLen(ctx, &json_len, result);
    JS_FreeValue(ctx, result);
    if (!json_str)
        return JS_EXCEPTION;

    HlJS *js_rt = (HlJS *)JS_GetContextOpaque(ctx);
    kl_http_response_header(res, "Content-Type", "application/json");
    /* A failed body copy leaves the response with no body: raise rather
     * than let it go out as a 200 with nothing in it. Compressed for the
     * response's own request: the active one may be another's - a `res`
     * stashed by one request and answered from another's handler was
     * encoded for the wrong client's Accept-Encoding (audit 8 L2). */
    int rc = hl_maybe_compress(req, res,
                               js_rt ? js_rt->base.compress : NULL,
                               json_str, json_len);
    JS_FreeCString(ctx, json_str);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "res.json: out of memory");
    return JS_UNDEFINED;
}

/* res.html(string) */
static JSValue js_res_html(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    KlHttpRequest *req = NULL;
    KlHttpResponse *res = get_response_box(ctx, this_val, &req);
    if (!res)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "res.html requires (string)");

    /* Length-exact: strlen cut the body at an embedded NUL (L5). */
    size_t html_len = 0;
    const char *html = JS_ToCStringLen(ctx, &html_len, argv[0]);
    if (!html)
        return JS_EXCEPTION;   /* a throwing toString: it was left pending */
    HlJS *js_rt = (HlJS *)JS_GetContextOpaque(ctx);
    kl_http_response_header(res, "Content-Type", "text/html; charset=utf-8");
    /* Skip the default CSP if middleware already wrote one - two
     * CSP headers cause browsers to enforce the strict intersection
     * (typically blocking the page's own scripts). The app-supplied
     * one wins. */
    if (js_rt && js_rt->base.csp_policy &&
        !hl_response_has_header(res, "Content-Security-Policy"))
        kl_http_response_header(res, "Content-Security-Policy",
                           js_rt->base.csp_policy);
    int rc = hl_maybe_compress(req, res,   /* the response's own request (L2) */
                               js_rt ? js_rt->base.compress : NULL,
                               html, html_len);
    JS_FreeCString(ctx, html);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "res.html: out of memory");
    return JS_UNDEFINED;
}

/* res.text(string) */
static JSValue js_res_text(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    KlHttpRequest *req = NULL;
    KlHttpResponse *res = get_response_box(ctx, this_val, &req);
    if (!res)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "res.text requires (string)");

    size_t text_len = 0;
    const char *text = JS_ToCStringLen(ctx, &text_len, argv[0]);   /* L5 */
    if (!text)
        return JS_EXCEPTION;
    HlJS *js_rt = (HlJS *)JS_GetContextOpaque(ctx);
    kl_http_response_header(res, "Content-Type", "text/plain; charset=utf-8");
    int rc = hl_maybe_compress(req, res,   /* the response's own request (L2) */
                               js_rt ? js_rt->base.compress : NULL,
                               text, text_len);
    JS_FreeCString(ctx, text);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "res.text: out of memory");
    return JS_UNDEFINED;
}

/* res.bytes(buf) - binary-safe response primitive.
 *
 * Accepts an ArrayBuffer, TypedArray, or string. Does NOT set
 * Content-Type (caller's responsibility - binary content can be
 * anything) and does NOT route through hl_maybe_compress (avoids
 * gzipping already-compressed payloads + keeps the response bytes
 * identical to a downstream SHA / ETag check). The body is copied
 * into a response-owned buffer so the JS value can be GC'd safely. */
static JSValue js_res_bytes(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    KlHttpResponse *res = get_response(ctx, this_val);
    if (!res)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "res.bytes requires (buffer)");

    HlBufferView view = {0};
    const char *str = NULL;
    int needs_free = 0;
    if (!js_get_buffer(ctx, argv[0], &view, &str, &needs_free))
        return JS_ThrowTypeError(ctx,
            "res.bytes: expected ArrayBuffer, TypedArray, or string");

    int rc = kl_http_response_body_copy(res, (const char *)view.data, view.len);
    if (needs_free && str) JS_FreeCString(ctx, str);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "res.bytes: out of memory");
    return JS_UNDEFINED;
}

/* res.redirect(url, code?) */
static JSValue js_res_redirect(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    KlHttpResponse *res = get_response(ctx, this_val);
    if (!res)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "res.redirect requires (url, code?)");

    /* A conversion that throws propagates (audit 8 L1): a failed code was
     * ignored, and a failed url returned undefined with nothing sent and
     * the exception left pending. */
    int32_t code = 302; /* default */
    if (argc >= 2 && JS_ToInt32(ctx, &code, argv[1]))
        return JS_EXCEPTION;

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url)
        return JS_EXCEPTION;
    kl_http_response_status(res, code);
    kl_http_response_header(res, "Location", url);
    kl_http_response_body_borrow(res, "", 0);
    JS_FreeCString(ctx, url);

    return JS_UNDEFINED;
}

/* ── Response class registration ────────────────────────────────────── */

static int hl_js_ensure_response_class(HlJS *js)
{
    if (js->response_class_registered)
        return 0;

    /* One id for the process (JS_NewClassID assigns only once), the class
     * registered in each runtime: a fresh id per runtime overwrote the global
     * the finalizer reads, so an older runtime's finalizer compared against
     * the wrong id and leaked its boxes (and their life references). */
    JS_NewClassID(&g_response_class_id);
    JSClassID class_id = g_response_class_id;
    js->response_class_id = (uint32_t)class_id;

    JSRuntime *rt = JS_GetRuntime(js->ctx);
    if (JS_NewClass(rt, class_id, &hl_response_class) < 0)
        return -1;

    /* Create prototype with methods - defined, not set: the class is
     * registered on the first request, and a set ran any setter an app put
     * on Object.prototype, with that request's connection active (audit 8
     * H1). */
    JSValue proto = JS_NewObject(js->ctx);
    JS_DefinePropertyValueStr(js->ctx, proto, "status",
                      JS_NewCFunction(js->ctx, js_res_status, "status", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(js->ctx, proto, "header",
                      JS_NewCFunction(js->ctx, js_res_header, "header", 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(js->ctx, proto, "json",
                      JS_NewCFunction(js->ctx, js_res_json, "json", 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(js->ctx, proto, "html",
                      JS_NewCFunction(js->ctx, js_res_html, "html", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(js->ctx, proto, "text",
                      JS_NewCFunction(js->ctx, js_res_text, "text", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(js->ctx, proto, "bytes",
                      JS_NewCFunction(js->ctx, js_res_bytes, "bytes", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(js->ctx, proto, "redirect",
                      JS_NewCFunction(js->ctx, js_res_redirect, "redirect", 2), JS_PROP_C_W_E);

    JS_SetClassProto(js->ctx, class_id, proto);
    js->response_class_registered = 1;

    return 0;
}

/* ── Public: create JS request/response objects ─────────────────────── */

JSValue hl_js_make_response_life(HlJS *js, KlHttpRequest *req,
                                 KlHttpResponse *res, HlReqLife *life)
{
    if (hl_js_ensure_response_class(js) != 0)
        return JS_ThrowInternalError(js->ctx, "failed to register Response class");

    JSValue obj = JS_NewObjectClass(js->ctx, (int)js->response_class_id);
    if (JS_IsException(obj)) return obj;
    HlJsResBox *box = (HlJsResBox *)malloc(sizeof *box);
    if (!box) {
        JS_FreeValue(js->ctx, obj);
        return JS_ThrowOutOfMemory(js->ctx);
    }
    box->res = res;
    box->req = req;
    box->life = life;
    hl_req_life_retain(life);
    JS_SetOpaque(obj, box);
    return obj;
}

JSValue hl_js_make_response(HlJS *js, KlHttpResponse *res)
{
    return hl_js_make_response_life(js, NULL, res, NULL);
}

/* ── HTTP-feature seam: 500-error response ──────────────────────────── */
/* Strong override for the JS runtime. Extracted from js/dispatch.c +
 * js/async.c so those core objects hold no kl_http_response_* refs. */
void hl_js_http_error_response(struct KlHttpResponse *res)
{
    kl_http_response_status(res, 500);
    kl_http_response_header(res, "Content-Type", "text/plain");
    kl_http_response_body_borrow(res, "Internal Server Error", 21);
}

/* Strong overrides: finalize + send a resumed request's response. Keeps ALL
 * kl_http_* refs (incl. kl_http_request_send_response from the heavy
 * http_server_core object) out of the base runtime's js_async.o; see
 * include/hull/http_feature.h. */
void hl_js_http_resume_send(struct KlHttpConn *conn, struct KlHttpRequest *req)
{
    KlHttpResponse *res = kl_http_conn_response(conn);
    if (res && res->body_mode == KL_HTTP_BODY_STREAM)
        kl_http_response_end_stream(res);
    kl_http_request_send_response(req);
}

void hl_js_http_resume_error(struct KlHttpConn *conn, struct KlHttpRequest *req)
{
    hl_js_http_error_response(kl_http_conn_response(conn));
    kl_http_request_send_response(req);
}
