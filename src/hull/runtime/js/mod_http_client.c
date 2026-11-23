/*
 * mod_http.c - hull:http module (HTTP client + async fetch)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "internal.h"            /* async gate, instruction budget */
#include "hull/shared/work_charge.h"   /* hl_work_bytes_units */
#include "hull/cap/http.h"
#include "db_wait.h"   /* hl_js_db_refuse_wait */
#include "refusal.h"   /* hl_js_refusal_tag (audit 35) */
#include "hull/cap/http_async.h"
#include "hull/cap/http_stream.h"
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/shared/refusal.h"
#include "hull/net_backend.h"   /* hl_net_op_complete, HlSuspendOp */
#include "hull/utils/alloc.h"

#include <keel/http_server.h>

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>

static void js_free_http_headers(JSContext *ctx, HlHttpHeader *hdrs, int count);

/* Parse JS headers object { name: value } into HlHttpHeader array.
 * Returns 0 (caller frees with js_free_http_headers()), or -1 with a JS
 * exception pending and nothing to free. */
static int js_parse_http_headers(JSContext *ctx, JSValueConst obj,
                                    HlHttpHeader **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (JS_IsUndefined(obj) || JS_IsNull(obj))
        return 0;

    /* Get property names */
    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, obj,
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return -1;

    if (prop_count == 0) {
        js_free(ctx, props);
        return 0;
    }

    HlHttpHeader *hdrs = js_mallocz(ctx, prop_count * sizeof(HlHttpHeader));
    if (!hdrs) {
        for (uint32_t i = 0; i < prop_count; i++)
            JS_FreeAtom(ctx, props[i].atom);
        js_free(ctx, props);
        return -1;
    }

    /* A getter or toString that throws fails the call: the header used to
     * be skipped with its exception left pending, and the request went out
     * without it (audit 17). */
    int count = 0;
    int failed = 0;
    for (uint32_t i = 0; i < prop_count; i++) {
        if (!failed) {
            const char *name = JS_AtomToCString(ctx, props[i].atom);
            JSValue val = name ? JS_GetProperty(ctx, obj, props[i].atom)
                               : JS_EXCEPTION;
            const char *value = JS_IsException(val) ? NULL
                                                    : JS_ToCString(ctx, val);
            JS_FreeValue(ctx, val);

            if (name && value) {
                hdrs[count].name = name;
                hdrs[count].value = value;
                count++;
            } else {
                if (name) JS_FreeCString(ctx, name);
                if (value) JS_FreeCString(ctx, value);
                failed = 1;
            }
        }
        JS_FreeAtom(ctx, props[i].atom);
    }
    js_free(ctx, props);

    if (failed) {
        js_free_http_headers(ctx, hdrs, count);
        return -1;
    }

    const char *bad = hl_http_reserved_header(hdrs, count);
    if (bad) {
        JS_ThrowTypeError(ctx, "http: header '%s' may not be set (Hull's "
                          "client writes it, or it is not a header name)", bad);
        js_free_http_headers(ctx, hdrs, count);
        return -1;
    }

    *out = hdrs;
    *out_count = count;
    return 0;
}

static void js_free_http_headers(JSContext *ctx, HlHttpHeader *hdrs, int count)
{
    if (!hdrs) return;
    for (int i = 0; i < count; i++) {
        JS_FreeCString(ctx, hdrs[i].name);
        JS_FreeCString(ctx, hdrs[i].value);
    }
    js_free(ctx, hdrs);
}

/* Define @p val as own property @p name of @p obj, taking @p val. A value
 * that could not be made (JS_EXCEPTION) or a define that failed leaves the
 * exception pending and returns -1 (audit 26: they were stored unchecked). */
static int js_http_define(JSContext *ctx, JSValue obj, const char *name,
                          JSValue val)
{
    if (JS_IsException(val)) return -1;
    return JS_DefinePropertyValueStr(ctx, obj, name, val, JS_PROP_C_W_E) < 0
        ? -1 : 0;
}

/* HTTP response as a JS object: { status, body, headers }, header names
 * lowercased. Own properties are DEFINED (a header named "__proto__" is a
 * key, not a prototype). Returns JS_EXCEPTION, nothing leaked, when any part
 * cannot be made. */
static JSValue js_push_http_response(JSContext *ctx, const KlHttpClientResponse *resp)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return JS_EXCEPTION;
    if (js_http_define(ctx, obj, "status", JS_NewInt32(ctx, resp->status)) != 0)
        goto fail;
    if (js_http_define(ctx, obj, "body",
                       (resp->body && resp->body_len > 0)
                           ? JS_NewStringLen(ctx, resp->body, resp->body_len)
                           : JS_NewString(ctx, "")) != 0)
        goto fail;

    JSValue headers = JS_NewObject(ctx);
    if (JS_IsException(headers)) goto fail;
    for (int i = 0; i < resp->num_headers; i++) {
        size_t nlen = strlen(resp->headers[i].name);
        char *lower = js_malloc(ctx, nlen + 1);
        if (!lower) { JS_FreeValue(ctx, headers); goto fail; }
        for (size_t j = 0; j < nlen; j++) {
            char c = resp->headers[i].name[j];
            lower[j] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        lower[nlen] = '\0';
        int rc = js_http_define(ctx, headers, lower,
                                JS_NewString(ctx, resp->headers[i].value));
        js_free(ctx, lower);
        if (rc != 0) { JS_FreeValue(ctx, headers); goto fail; }
    }
    if (js_http_define(ctx, obj, "headers", headers) != 0) goto fail;
    return obj;

fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* The response of a sync request, charged and pushed, then freed: its body
 * was received, decompressed and is copied into the VM, 1 unit per 8 bytes
 * (audit 39). Known only once it arrived, so charged after; the floor was
 * charged before. */
static JSValue js_http_response_charged(JSContext *ctx, KlHttpClientResponse *resp)
{
    JSValue result = hl_js_budget_charge(ctx, hl_work_bytes_units(resp->body_len))
                         ? JS_EXCEPTION : js_push_http_response(ctx, resp);
    kl_http_client_response_free(resp);
    return result;
}

/* Every request costs DNS, a socket, a connect and maybe a TLS handshake
 * whatever its size: a floor charged before anything is converted (audit
 * 39 - a refused connect in a try loop ran ~10 min per budget). */
#define JS_HTTP_CHARGE_REQUEST(ctx) \
    do { if (hl_js_budget_charge((ctx), HL_WORK_HTTP_REQUEST_UNITS)) \
             return JS_EXCEPTION; } while (0)

/* opts.timeoutMs of @p opts (if it is an object): ONE deadline for the whole
 * request (DNS, connect, TLS, send, receive, redirects), a number of at least
 * 1, returned truncated (the cap layer clamps it to the path's ceiling: 60 s
 * sync, 10 min for http.fetch). Absent / undefined / null = 0, the app default
 * (manifest http.timeoutMs, else 30 s). Anything else throws: a mistyped
 * timeout must not silently become the default. Returns 0 or -1 (thrown). */
static int js_http_timeout_opt(JSContext *ctx, int argc, JSValueConst *argv,
                               int idx, const char *fn, int *out)
{
    *out = 0;
    if (argc <= idx || !JS_IsObject(argv[idx]))
        return 0;
    JSValue v = JS_GetPropertyStr(ctx, argv[idx], "timeoutMs");
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    double d = 0;
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) < 0 || !(d >= 1)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "%s: opts.timeoutMs must be a positive number "
                               "(milliseconds)", fn);
        return -1;
    }
    *out = d >= (double)INT_MAX ? INT_MAX : (int)d;
    return 0;
}

/* A request body: a string (sent as its UTF-8 bytes), an ArrayBuffer or a
 * typed array (its bytes, copied at once - a later getter could
 * detach or resize the buffer), or undefined / null (no body). Anything else
 * is a TypeError: it was dropped silently, and the request went out with no
 * body (audit 25). */
typedef struct {
    const char *p;
    size_t      len;
    int         kind;   /* 0 none, 1 JS_ToCStringLen, 2 js_malloc'd copy */
} JsHttpBody;

static void js_http_body_free(JSContext *ctx, JsHttpBody *b)
{
    if (b->kind == 1) JS_FreeCString(ctx, b->p);
    else if (b->kind == 2) js_free(ctx, (void *)(uintptr_t)b->p);
    b->p = NULL;
    b->len = 0;
    b->kind = 0;
}

/* 0, or -1 with an exception pending (the conversion's own - the budget's
 * interrupt, out of memory - or the TypeError).
 * The body is charged here, 1 unit per 8 bytes, before a buffer is copied,
 * and the copy is made on the VM heap: an ArrayBuffer body was malloc'd and
 * copied uncharged before anything sized it, and the sync request / post /
 * put / patch never charged a body at all (audit 27). */
static int js_http_body_get(JSContext *ctx, JSValueConst v, JsHttpBody *b,
                            const char *fn)
{
    b->p = NULL;
    b->len = 0;
    b->kind = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v)) return 0;
    if (JS_IsString(v)) {
        b->p = JS_ToCStringLen(ctx, &b->len, v);
        if (!b->p) return -1;
        b->kind = 1;
        if (hl_js_budget_charge(ctx, hl_work_bytes_units(b->len)) < 0) {
            js_http_body_free(ctx, b);
            return -1;
        }
        return 0;
    }
    size_t n = 0;
    const uint8_t *view = hl_js_bytes_view(ctx, &n, v);
    if (view) {
        HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
        if (js && js->max_heap_bytes && n > js->max_heap_bytes) {
            JS_ThrowRangeError(ctx, "%s: a %zu-byte body is larger than the "
                               "VM's heap limit", fn, n);
            return -1;
        }
        if (hl_js_budget_charge(ctx, hl_work_bytes_units(n)) < 0)
            return -1;
        /* js_malloc throws its own out-of-memory. No app code ran since
         * the view was taken. */
        char *copy = js_malloc(ctx, n ? n : 1);
        if (!copy)
            return -1;
        if (n) memcpy(copy, view, n);
        b->p = copy;
        b->len = n;
        b->kind = 2;
        return 0;
    }
    JS_ThrowTypeError(ctx, "%s: body must be a string, ArrayBuffer or typed "
                      "array", fn);
    return -1;
}

/* Gap 3: a host manifest.hosts does not admit throws an Error with an own
 * `code` "denied" and an own `host`, not the network failure's message. */
static JSValue js_http_throw_denied(JSContext *ctx, const char *what,
                                    const HlHttpDenial *d)
{
    if (d->unset_ref[0])
        JS_ThrowTypeError(ctx, "%s: host '%s' is not in manifest.hosts (its "
                          "entry %s names an unset variable)", what, d->host,
                          d->unset_ref);
    else
        JS_ThrowTypeError(ctx, "%s: host '%s' is not in manifest.hosts",
                          what, d->host);
    JSValue e = JS_GetException(ctx);
    JSValue h = JS_NewString(ctx, d->host);
    if (JS_IsException(h)) {
        JS_FreeValue(ctx, e);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValueStr(ctx, e, "host", h,
                              JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JS_Throw(ctx, e);
    return hl_js_refusal_tag(ctx, HL_REFUSAL_DENIED);
}

/* http.request(method, url, opts?) */
static JSValue js_http_request(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured (no hosts in manifest)");

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "http.request requires (method, url, opts?)");

    int timeout_ms = 0;
    if (js_http_timeout_opt(ctx, argc, argv, 2, "http.request", &timeout_ms) != 0)
        return JS_EXCEPTION;
    JS_HTTP_CHARGE_REQUEST(ctx);   /* audit 39 */

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!method || !url) {
        if (method) JS_FreeCString(ctx, method);
        if (url) JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    const char *body = NULL;
    size_t body_len = 0;
    JsHttpBody jbody = {0};
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse opts */
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue body_val = JS_GetPropertyStr(ctx, argv[2], "body");
        /* A throwing getter (or a failed conversion) fails the call: the
         * request went out with no body and the exception pending (audit
         * 23). */
        int body_failed = JS_IsException(body_val) ||
            js_http_body_get(ctx, body_val, &jbody, "http.request") != 0;
        body = jbody.p;
        body_len = jbody.len;
        JS_FreeValue(ctx, body_val);
        if (body_failed) {
            JS_FreeCString(ctx, method);
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }

        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[2], "headers");
        int hrc = JS_IsException(hdrs_val) ? -1
                : JS_IsObject(hdrs_val)
                    ? js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers)
                    : 0;
        JS_FreeValue(ctx, hdrs_val);
        if (hrc != 0) {
            JS_FreeCString(ctx, method);
            JS_FreeCString(ctx, url);
            js_http_body_free(ctx, &jbody);
            return JS_EXCEPTION;
        }
    }

    KlHttpClientResponse resp = {0}; /* zero-init: resp.error is read on the rc!=0
                                  * path; don't depend on the cap layer's
                                  * internal memset ordering. */
    int rc = hl_cap_http_request(js->base.http_cfg, timeout_ms, method, url,
                                    headers, num_headers, body, body_len, &resp);
    HlHttpDenial denial;
    int denied = rc != 0 && hl_http_url_denied(js->base.http_cfg, url, &denial);

    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    js_http_body_free(ctx, &jbody);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0) {
        if (denied) return js_http_throw_denied(ctx, "http request", &denial);
        return JS_ThrowInternalError(ctx, "http request failed: %s",
                                      kl_strerror(resp.error));
    }

    return js_http_response_charged(ctx, &resp);
}

/* http.get(url, opts?) */
static JSValue js_http_get(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http.get requires (url, opts?)");

    int timeout_ms = 0;
    if (js_http_timeout_opt(ctx, argc, argv, 1, "http.get", &timeout_ms) != 0)
        return JS_EXCEPTION;
    JS_HTTP_CHARGE_REQUEST(ctx);   /* audit 39 */

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        int hrc = JS_IsException(hdrs_val) ? -1
                : JS_IsObject(hdrs_val)
                    ? js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers)
                    : 0;
        JS_FreeValue(ctx, hdrs_val);
        if (hrc != 0) {
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }
    }

    KlHttpClientResponse resp = {0}; /* zero-init: resp.error is read on the rc!=0
                                  * path; don't depend on the cap layer's
                                  * internal memset ordering. */
    int rc = hl_cap_http_request(js->base.http_cfg, timeout_ms, "GET", url,
                                    headers, num_headers, NULL, 0, &resp);
    HlHttpDenial denial;
    int denied = rc != 0 && hl_http_url_denied(js->base.http_cfg, url, &denial);
    JS_FreeCString(ctx, url);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0) {
        if (denied) return js_http_throw_denied(ctx, "http.get", &denial);
        return JS_ThrowInternalError(ctx, "http.get failed: %s",
                                      kl_strerror(resp.error));
    }

    return js_http_response_charged(ctx, &resp);
}

/* Helper for POST/PUT/PATCH: (url, body, opts?) */
static JSValue js_http_body_method(JSContext *ctx, int argc, JSValueConst *argv,
                                    const char *method_name)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http.%s requires (url, body?, opts?)",
                                  method_name);

    int timeout_ms = 0;
    if (js_http_timeout_opt(ctx, argc, argv, 2, "http.post/put/patch",
                            &timeout_ms) != 0)
        return JS_EXCEPTION;
    JS_HTTP_CHARGE_REQUEST(ctx);   /* audit 39 */

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    /* A failed conversion (the budget's interrupt, out of memory) fails the
     * call: the request went out with an empty body (audit 25). */
    JsHttpBody jbody = {0};
    if (argc >= 2 && js_http_body_get(ctx, argv[1], &jbody,
                                      "http.post/put/patch") != 0) {
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }
    const char *body = jbody.p;
    size_t body_len = jbody.len;

    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[2], "headers");
        int hrc = JS_IsException(hdrs_val) ? -1
                : JS_IsObject(hdrs_val)
                    ? js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers)
                    : 0;
        JS_FreeValue(ctx, hdrs_val);
        if (hrc != 0) {
            JS_FreeCString(ctx, url);
            js_http_body_free(ctx, &jbody);
            return JS_EXCEPTION;
        }
    }

    KlHttpClientResponse resp = {0}; /* zero-init: resp.error is read on the rc!=0
                                  * path; don't depend on the cap layer's
                                  * internal memset ordering. */
    int rc = hl_cap_http_request(js->base.http_cfg, timeout_ms, method_name, url,
                                    headers, num_headers, body, body_len, &resp);
    HlHttpDenial denial;
    int denied = rc != 0 && hl_http_url_denied(js->base.http_cfg, url, &denial);
    JS_FreeCString(ctx, url);
    js_http_body_free(ctx, &jbody);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0) {
        if (denied) {
            char what[32];
            snprintf(what, sizeof what, "http.%s", method_name);
            for (char *c = what; *c; c++) *c = (char)tolower((unsigned char)*c);
            return js_http_throw_denied(ctx, what, &denial);
        }
        return JS_ThrowInternalError(ctx, "http.%s failed: %s",
                                      method_name, kl_strerror(resp.error));
    }

    return js_http_response_charged(ctx, &resp);
}

static JSValue js_http_post(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{ (void)this_val; return js_http_body_method(ctx, argc, argv, "POST"); }

static JSValue js_http_put(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{ (void)this_val; return js_http_body_method(ctx, argc, argv, "PUT"); }

static JSValue js_http_patch(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{ (void)this_val; return js_http_body_method(ctx, argc, argv, "PATCH"); }

/* http.del(url, opts?) - same as http.get but DELETE */
static JSValue js_http_del(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http.del requires (url, opts?)");

    int timeout_ms = 0;
    if (js_http_timeout_opt(ctx, argc, argv, 1, "http.delete", &timeout_ms) != 0)
        return JS_EXCEPTION;
    JS_HTTP_CHARGE_REQUEST(ctx);   /* audit 39 */

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        int hrc = JS_IsException(hdrs_val) ? -1
                : JS_IsObject(hdrs_val)
                    ? js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers)
                    : 0;
        JS_FreeValue(ctx, hdrs_val);
        if (hrc != 0) {
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }
    }

    KlHttpClientResponse resp = {0}; /* zero-init: resp.error is read on the rc!=0
                                  * path; don't depend on the cap layer's
                                  * internal memset ordering. */
    int rc = hl_cap_http_request(js->base.http_cfg, timeout_ms, "DELETE", url,
                                    headers, num_headers, NULL, 0, &resp);
    HlHttpDenial denial;
    int denied = rc != 0 && hl_http_url_denied(js->base.http_cfg, url, &denial);
    JS_FreeCString(ctx, url);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0) {
        if (denied) return js_http_throw_denied(ctx, "http.del", &denial);
        return JS_ThrowInternalError(ctx, "http.del failed: %s",
                                      kl_strerror(resp.error));
    }

    return js_http_response_charged(ctx, &resp);
}

/* ── Push async HTTP response into JS ─────────────────────────────── */

static JSValue js_push_async_http_response(JSContext *ctx, void *driver)
{
    const KlHttpClientResponse *resp = hl_http_async_response(driver);
    if (!resp)
        return JS_UNDEFINED;
    return js_push_http_response(ctx, resp);
}

/* http.fetch(method, url, opts?) - async non-blocking HTTP request. */
static JSValue js_http_fetch(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured (no hosts in manifest)");
    if (!js->base.async_ctx)
        return JS_ThrowInternalError(ctx,
            "http.fetch() requires an active event loop");
    if (hl_js_async_gate(ctx, js, "http.fetch()") != 0)
        return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, "http.fetch()")) return JS_EXCEPTION;

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "http.fetch requires (method, url, opts?)");

    int timeout_ms = 0;
    if (js_http_timeout_opt(ctx, argc, argv, 2, "http.fetch", &timeout_ms) != 0)
        return JS_EXCEPTION;
    JS_HTTP_CHARGE_REQUEST(ctx);   /* audit 39 */

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!method || !url) {
        if (method) JS_FreeCString(ctx, method);
        if (url) JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    const char *body = NULL;
    size_t body_len = 0;
    JsHttpBody jbody = {0};
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse opts */
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue body_val = JS_GetPropertyStr(ctx, argv[2], "body");
        /* A throwing getter (or a failed conversion) fails the call: the
         * request went out with no body and the exception pending (audit
         * 23). */
        int body_failed = JS_IsException(body_val) ||
            js_http_body_get(ctx, body_val, &jbody, "http.fetch") != 0;
        body = jbody.p;
        body_len = jbody.len;
        JS_FreeValue(ctx, body_val);
        if (body_failed) {
            JS_FreeCString(ctx, method);
            JS_FreeCString(ctx, url);
            return JS_EXCEPTION;
        }

        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[2], "headers");
        int hrc = JS_IsException(hdrs_val) ? -1
                : JS_IsObject(hdrs_val)
                    ? js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers)
                    : 0;
        JS_FreeValue(ctx, hdrs_val);
        if (hrc != 0) {   /* before anything is armed (audit 17) */
            JS_FreeCString(ctx, method);
            JS_FreeCString(ctx, url);
            js_http_body_free(ctx, &jbody);
            return JS_EXCEPTION;
        }
    }

    /* The promise and continuation first, as the Lua binding does:
     * created after the request was started (and the connection
     * suspended), a failed allocation returned with ctx->cont NULL and the
     * request in flight - its completion dereferenced the NULL cont. */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, url);
        js_http_body_free(ctx, &jbody);
        js_free_http_headers(ctx, headers, num_headers);
        return JS_EXCEPTION;
    }
    /* Again, now that app code has run (the method / url toString, the
     * opts.body / opts.headers getters): a getter could have parked a
     * multipart read or opened a transaction, and hl_async_http_start
     * suspends the connection without hl_js_op_suspend's re-check (M1). */
    if (hl_js_async_gate(ctx, js, "http.fetch()") != 0 ||
        hl_js_db_refuse_wait(ctx, "http.fetch()")) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, url);
        js_http_body_free(ctx, &jbody);
        js_free_http_headers(ctx, headers, num_headers);
        return JS_EXCEPTION;
    }

    /* Keel copies the body and keeps it until the request is done: held on
     * the VM's off-heap total under its heap limit - N fetches in flight
     * held N copies of one big body (audit 22). Keel also buffers each
     * response (up to max_response_size) off the heap, so at most
     * HL_HTTP_MAX_INFLIGHT fetches are in flight: those buffers are bounded
     * by that count x max_response_size (audit 31). The body was charged
     * when it was read (js_http_body_get, audit 27). */
    HlOffheapHold body_hold = {0};
    int admit = hl_http_fetch_admit(js->base.http_inflight, &body_hold,
                                    &js->wasm_buf_bytes, js->max_heap_bytes,
                                    body_len);
    if (admit != HL_HTTP_ADMIT_OK) {
        hl_offheap_drop(&body_hold);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, url);
        js_http_body_free(ctx, &jbody);
        js_free_http_headers(ctx, headers, num_headers);
        if (admit == HL_HTTP_ADMIT_INFLIGHT)
            JS_ThrowRangeError(ctx, "http.fetch: too many requests "
                "in flight (at most %d per VM)", HL_HTTP_MAX_INFLIGHT);
        else
            JS_ThrowRangeError(ctx, "http.fetch: a %zu-byte body would "
                "take the VM's off-heap copies past its heap limit", body_len);
        return hl_js_refusal_tag(ctx, HL_REFUSAL_BUSY);
    }

    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_async_http_response);
    if (!cont) {
        hl_offheap_drop(&body_hold);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, url);
        js_http_body_free(ctx, &jbody);
        js_free_http_headers(ctx, headers, num_headers);
        return JS_ThrowInternalError(ctx, "http.fetch: out of memory");
    }

    /* Start async HTTP */
    HlAsyncCtx *async_ctx = hl_async_http_start(
        js->server, hl_js_cont_suspend_conn(cont), js->base.net_ctx, js->base.alloc,
        js->base.http_cfg, timeout_ms, method, url, headers, num_headers,
        body, body_len);
    HlHttpDenial denial;
    int denied = !async_ctx && hl_http_url_denied(js->base.http_cfg, url, &denial);

    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    js_http_body_free(ctx, &jbody);
    js_free_http_headers(ctx, headers, num_headers);

    if (!async_ctx) {
        /* Never armed: destroy frees resolve / reject (cancel would also
         * end this still-running handler's request life). */
        hl_offheap_drop(&body_hold);
        cont->destroy(cont);
        JS_FreeValue(ctx, promise);
        if (denied) return js_http_throw_denied(ctx, "http.fetch", &denial);
        return JS_ThrowInternalError(ctx, "http.fetch: failed to start request");
    }
    async_ctx->cont = cont;
    async_ctx->hold = body_hold;   /* given back when the ctx goes */
    async_ctx->inflight = &js->base.http_inflight;   /* audit 31 */
    js->base.http_inflight++;
    /* No request holds a detached fetch: teardown cancels it (audit 14). */
    if (async_ctx->detached)
        hl_async_ctx_track(async_ctx, &js->base.async_live);

    return promise;
}

/* ── http.async.* - async HTTP convenience methods ─────────────────── */

static JSValue js_http_async_request(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{ return js_http_fetch(ctx, this_val, argc, argv); }

static JSValue js_http_async_no_body(JSContext *ctx, JSValueConst this_val,
                                      const char *method,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue method_val = JS_NewString(ctx, method);
    if (JS_IsException(method_val))
        return JS_EXCEPTION;
    JSValue args[3];
    args[0] = method_val;
    int nargs = 1;
    for (int i = 0; i < argc && nargs < 3; i++)
        args[nargs++] = argv[i];

    JSValue result = js_http_fetch(ctx, JS_UNDEFINED, nargs, args);
    JS_FreeValue(ctx, method_val);
    return result;
}

static JSValue js_http_async_with_body(JSContext *ctx, JSValueConst this_val,
                                        const char *method,
                                        int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http.async.%s requires (url, body?, opts?)",
                                 method);

    /* Every step can throw - an opts getter for headers / timeoutMs runs app
     * code - and a pending exception must stop the call here: stored as a
     * property it used to reach js_http_fetch as an opts value, with the
     * exception still pending (audit 11). Built with no prototype and
     * defined, not set, properties: on a plain object an Object.prototype
     * setter for body / headers / timeoutMs swallowed the value, and a
     * polluted Object.prototype.headers was read back by js_http_fetch as
     * the call's own (audit 12). */
    JSValue opts = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(opts))
        return JS_EXCEPTION;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) &&
        JS_DefinePropertyValueStr(ctx, opts, "body", JS_DupValue(ctx, argv[1]),
                                  JS_PROP_C_W_E) < 0)
        goto fail;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue hdrs = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (JS_IsException(hdrs))
            goto fail;
        if (!JS_IsUndefined(hdrs)) {
            if (JS_DefinePropertyValueStr(ctx, opts, "headers", hdrs,
                                          JS_PROP_C_W_E) < 0)
                goto fail;
        } else {
            JS_FreeValue(ctx, hdrs);
        }
        JSValue tmo = JS_GetPropertyStr(ctx, argv[2], "timeoutMs");
        if (JS_IsException(tmo))
            goto fail;
        if (!JS_IsUndefined(tmo)) {
            if (JS_DefinePropertyValueStr(ctx, opts, "timeoutMs", tmo,
                                          JS_PROP_C_W_E) < 0)
                goto fail;
        } else {
            JS_FreeValue(ctx, tmo);
        }
    }

    JSValue method_val = JS_NewString(ctx, method);
    if (JS_IsException(method_val))
        goto fail;
    JSValue args[3] = { method_val, argv[0], opts };
    JSValue result = js_http_fetch(ctx, JS_UNDEFINED, 3, args);
    JS_FreeValue(ctx, method_val);
    JS_FreeValue(ctx, opts);
    return result;

fail:
    JS_FreeValue(ctx, opts);
    return JS_EXCEPTION;
}

static JSValue js_http_async_get(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{ return js_http_async_no_body(ctx, this_val, "GET", argc, argv); }

static JSValue js_http_async_post(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{ return js_http_async_with_body(ctx, this_val, "POST", argc, argv); }

static JSValue js_http_async_put(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{ return js_http_async_with_body(ctx, this_val, "PUT", argc, argv); }

static JSValue js_http_async_patch(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{ return js_http_async_with_body(ctx, this_val, "PATCH", argc, argv); }

static JSValue js_http_async_delete(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{ return js_http_async_no_body(ctx, this_val, "DELETE", argc, argv); }

/* ── httpClient.async.stream - a streamed response ───────────────────── *
 *
 * stream(method, url, opts) resolves to the response as soon as its status
 * and headers arrive: { status, headers } plus read() (an ArrayBuffer, null at
 * the end), readText() (UTF-8 text, a character split across chunks carried
 * to the next read), close(), and [Symbol.asyncIterator] over the byte chunks.
 * The body arrives in cap/http_stream.c's bounded buffer; a read that finds
 * it empty parks on the stream's op, and the continuation's push_result does
 * the read once resumed (the cap completes the op only when the next read
 * will not wait). The parking follows mod_ssh.c: the async gate, no wait
 * inside a transaction, the request's connection suspended through
 * hl_js_op_suspend, a detached wait tracked with an on_shutdown. An open
 * stream holds a Keel client, so it is on the runtime's teardown list too
 * (stopped while the loop is alive), counts as one of the VM's
 * HL_HTTP_MAX_INFLIGHT requests while the request runs, and holds its buffer
 * on the VM's off-heap total until the reader has had the end (or the error),
 * it is closed, or it is collected.
 *
 * A parked wait pins the handle (a reference to it that no gc_mark reports,
 * so the cycle collector counts it as held from outside): nothing
 * else needs to hold it - `await (await stream(...)).read()` drops the only
 * JS reference while the read waits, and during the header wait the app has
 * none yet. The pin belongs to the wait's ctx and is dropped by its
 * free_driver on every path that retires the ctx (resumed, cancelled with its
 * request, stopped at teardown), so the finalizer never runs while a wait is
 * parked (audit 52). A resume can park the next read before its own ctx is
 * retired, so pins are counted, one per ctx. */

static JSClassID hl_http_stream_class_id;

enum { HS_PARK_HEAD = 1, HS_PARK_BYTES, HS_PARK_TEXT, HS_PARK_NEXT };
enum { HS_OPEN = 0, HS_ENDED, HS_FAILED };

typedef struct {
    HlHttpStream      *s;        /* owned; NULL once released              */
    HlAsyncCtx        *ctx;      /* live only while parked (or a wake pends) */
    HlAsyncBackendCtx *loop;
    JSRuntime         *rt;
    int                kind;     /* HS_PARK_* of the parked call           */
    JSValue            self;     /* the handle, not owned: a reference to it
                                  * is taken per parked wait (pins)         */
    int                pins;     /* parked waits whose ctx is not retired  */
    HlAsyncTeardown    track;    /* linked while s is open                 */
    HlOffheapHold      hold;     /* the buffer, on the VM's off-heap total */
    int               *inflight; /* the VM's http_inflight while it runs   */
    uint8_t            carry[4]; /* readText: a split UTF-8 sequence       */
    int                ncarry;
    int                term;     /* HS_ENDED / HS_FAILED once released so  */
    int                term_rc;  /* the error a failed stream ended with   */
    char               term_msg[128];
} HlJsHttpStream;

/* The in-flight slot bounds the response buffers Keel holds: given back as
 * soon as the Keel client is gone, buffered bytes or not. */
static void hs_slot_check(HlJsHttpStream *o)
{
    if (o->inflight && hl_http_stream_client_done(o->s)) {
        (*o->inflight)--;
        o->inflight = NULL;
    }
}

/* Give the stream's request back: Keel client, timers, the in-flight slot,
 * the off-heap bytes. The handle stays (closed); idempotent. */
static void hs_release(HlJsHttpStream *o)
{
    if (!o->s) return;
    hl_async_teardown_unlink(&o->track);
    hl_http_stream_set_user(o->s, NULL);
    hl_http_stream_free(o->s);
    o->s = NULL;
    hl_offheap_drop(&o->hold);
    if (o->inflight) { (*o->inflight)--; o->inflight = NULL; }
}

/* The reader has had the end (rc 0) or the error that ended the stream,
 * with nothing left buffered: release it now, keeping only the outcome, so
 * an ended stream the app keeps holds neither a slot nor its buffer. */
static void hs_finish(HlJsHttpStream *o, int rc)
{
    if (!o->s) return;
    o->term = rc == 0 ? HS_ENDED : HS_FAILED;
    o->term_rc = rc;
    if (rc != 0)
        snprintf(o->term_msg, sizeof o->term_msg, "%s",
                 hl_http_stream_strerror(o->s, rc));
    hs_release(o);
}

/* The runtime is going: stop the request while the loop is alive. */
static void hs_teardown(HlAsyncTeardown *t, HlAsyncBackendCtx *loop)
{
    (void)loop;
    HlJsHttpStream *o = (HlJsHttpStream *)(void *)((char *)t -
                                                  offsetof(HlJsHttpStream, track));
    hs_release(o);
}

static void hs_unpark(HlJsHttpStream *o)
{
    HlAsyncCtx *ctx = o->ctx;
    if (!ctx) return;
    o->ctx = NULL;
    ctx->detached_timer = 0;
    /* ctx->driver stays o: the continuation's push_result reads from it. */
    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
        hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
}

/* Bytes of a complete UTF-8 prefix of buf[0..n): the length of a trailing
 * sequence that is a valid start but not yet complete is held back. */
static size_t hs_utf8_complete(const uint8_t *b, size_t n)
{
    size_t back = n < 3 ? n : 3;
    for (size_t k = 1; k <= back; k++) {
        uint8_t c = b[n - k];
        if ((c & 0xC0) == 0x80) continue;            /* continuation */
        size_t need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                    : (c & 0xF8) == 0xF0 ? 4 : 1;
        return need > k ? n - k : n;                 /* incomplete: hold it */
    }
    return n;
}

/* A parked readText() woken by bytes that only continue a character: they go
 * to the carry and the wait parks again, so readText() never returns "" in
 * the middle of the body. 1 when re-parked. An incomplete sequence is at most
 * 3 bytes, so more than that buffered always holds a whole character. */
static int hs_text_repark(HlJsHttpStream *o, HlAsyncOp *op)
{
    if (o->kind != HS_PARK_TEXT || !o->s || !o->loop ||
        (size_t)o->ncarry + hl_http_stream_buffered(o->s) > 3)
        return 0;
    for (;;) {
        if (o->ncarry && hs_utf8_complete(o->carry, (size_t)o->ncarry) > 0)
            return 0;
        if (o->ncarry >= 4) return 0;
        long r = hl_http_stream_read(o->s, o->carry + o->ncarry, 1);
        if (r == 1) { o->ncarry++; continue; }
        if (r != HL_HTTP_STREAM_E_AGAIN) return 0;   /* the end, an error */
        const HlAsyncBackend *be = hl_async_backend();
        return be && be->op_suspend && be->op_suspend(o->loop, op) == 0;
    }
}

static void hs_on_resume(HlAsyncOp *op)
{
    HlHttpStream *s = hl_http_stream_from_op(op);
    HlJsHttpStream *o = s ? hl_http_stream_user(s) : NULL;
    if (!o || !o->ctx) return;
    if (hs_text_repark(o, op)) return;
    hs_unpark(o);
}

static void hs_on_op_cancel(HlAsyncOp *op)
{
    HlHttpStream *s = hl_http_stream_from_op(op);
    HlJsHttpStream *o = s ? hl_http_stream_user(s) : NULL;
    if (o) hs_unpark(o);
}

/* The deferred wake of a close() made while a read was parked. */
static void hs_wake_fire(void *user_data)
{
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlJsHttpStream *o = (HlJsHttpStream *)ctx->driver;
    ctx->detached_timer = 0;
    if (o && o->ctx == ctx) hs_unpark(o);
}

static void hs_cancel_wake(HlAsyncCtx *ctx, HlAsyncBackendCtx *loop)
{
    if (ctx->detached_timer > 0 && loop)
        hl_async_backend()->timer_cancel(loop, (uint64_t)ctx->detached_timer);
    ctx->detached_timer = 0;
}

/* The wait ctx is retired (its free_driver, on every path): drop its pin.
 * It may be the handle's last reference, so the finalizer may free o here:
 * nothing touches o after. */
static void hs_ctx_done(void *driver)
{
    HlJsHttpStream *o = (HlJsHttpStream *)driver;
    JSRuntime *rt = o->rt;
    JSValue self = o->self;
    o->pins--;
    JS_FreeValueRT(rt, self);
}

/* The request a waiter was attached to is going away: the stream stops
 * pointing at the ctx and is released (nobody is left to read it). The
 * ctx's cancel then drops the pin (hs_ctx_done). */
static void hs_on_request_cancel(KlAsyncOp *kop, void *user_data)
{
    (void)kop;
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlJsHttpStream *o = (HlJsHttpStream *)ctx->driver;
    if (o && o->ctx == ctx) {
        o->ctx = NULL;
        hs_cancel_wake(ctx, o->loop);
        hs_release(o);
    }
    hl_async_ctx_cancel(ctx);
}

static void hs_on_shutdown(HlAsyncCtx *ctx, HlAsyncBackendCtx *loop)
{
    HlJsHttpStream *o = (HlJsHttpStream *)ctx->driver;
    if (o && o->ctx == ctx) {
        o->ctx = NULL;
        hs_cancel_wake(ctx, loop);
        hs_release(o);
    }
}

/* An Error carrying an own `code` ("timeout", "idle", "overflow",
 * "too_large", "closed", "io_error"). */
static JSValue hs_throw_msg(JSContext *ctx, const char *msg, int rc)
{
    JS_ThrowInternalError(ctx, "httpClient stream: %s", msg);
    JSValue e = JS_GetException(ctx);
    JSValue c = JS_NewString(ctx, hl_http_stream_code(rc));
    if (!JS_IsException(c))
        JS_DefinePropertyValueStr(ctx, e, "code", c, JS_PROP_C_W_E);
    return JS_Throw(ctx, e);
}

static JSValue hs_throw(JSContext *ctx, const HlJsHttpStream *o, int rc)
{
    return hs_throw_msg(ctx, hl_http_stream_strerror(o ? o->s : NULL, rc), rc);
}

/* { value, done } for the async iterator; takes @p value. */
static JSValue hs_iter_result(JSContext *ctx, JSValue value, int done)
{
    if (JS_IsException(value)) return value;
    JSValue r = JS_NewObject(ctx);
    if (JS_IsException(r)) { JS_FreeValue(ctx, value); return r; }
    if (JS_DefinePropertyValueStr(ctx, r, "value", value, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, r, "done", JS_NewBool(ctx, done),
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, r);
        return JS_EXCEPTION;
    }
    return r;
}

/* One read, done now: the value for the parked or immediate call, or
 * JS_EXCEPTION. AGAIN is reported as rc through *again (caller parks). */
static JSValue hs_read_now(JSContext *ctx, HlJsHttpStream *o, int kind, int *again)
{
    *again = 0;
    /* Bytes readText() carried over from a split character come first, for
     * a byte read too: they are earlier in the body than anything buffered. */
    if (kind != HS_PARK_TEXT && o->ncarry) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, o->carry, (size_t)o->ncarry);
        if (JS_IsException(ab)) return ab;
        o->ncarry = 0;
        return kind == HS_PARK_NEXT ? hs_iter_result(ctx, ab, 0) : ab;
    }
retry:
    if (!o->s) {
        if (o->term == HS_FAILED)
            return hs_throw_msg(ctx, o->term_msg, o->term_rc);
        if (kind == HS_PARK_NEXT) return hs_iter_result(ctx, JS_UNDEFINED, 1);
        if (o->term == HS_ENDED) return JS_NULL;
        return hs_throw(ctx, o, HL_HTTP_STREAM_E_CLOSED);
    }
    size_t want = hl_http_stream_buffered(o->s);
    /* A parked readText() woken with a whole character in its carry and
     * nothing buffered (hs_text_repark): the carry is the read. */
    if (kind == HS_PARK_TEXT && want == 0 && o->ncarry &&
        hs_utf8_complete(o->carry, (size_t)o->ncarry) > 0) {
        size_t keep = hs_utf8_complete(o->carry, (size_t)o->ncarry);
        JSValue t = JS_NewStringLen(ctx, (const char *)o->carry, keep);
        if (JS_IsException(t)) return t;
        o->ncarry -= (int)keep;
        memmove(o->carry, o->carry + keep, (size_t)o->ncarry);
        return t;
    }
    if (want > 64u * 1024u) want = 64u * 1024u;
    if (want == 0) want = 1;   /* asks the cap: AGAIN, the end or the error */
    /* The copy into the VM, charged before the bytes leave the stream: a
     * trip after would lose them. */
    if (hl_js_budget_charge(ctx, 64 + hl_work_bytes_units(want)))
        return JS_EXCEPTION;
    uint8_t *p = js_malloc(ctx, 4 + want);
    if (!p) return JS_EXCEPTION;
    size_t off = 0;
    if (kind == HS_PARK_TEXT && o->ncarry) {
        memcpy(p, o->carry, (size_t)o->ncarry);
        off = (size_t)o->ncarry;
    }
    long n = hl_http_stream_read(o->s, p + off, want);
    hs_slot_check(o);
    if (n <= 0) js_free(ctx, p);
    if (n == HL_HTTP_STREAM_E_AGAIN) { *again = 1; return JS_UNDEFINED; }
    if (n < 0) {
        /* The body ended with an error once its bytes were read. */
        JSValue e = hs_throw(ctx, o, (int)n);
        hs_finish(o, (int)n);
        return e;
    }
    if (n == 0) {
        hs_finish(o, 0);
        if (kind == HS_PARK_TEXT && o->ncarry) {   /* a truncated character */
            o->ncarry = 0;
            return JS_NewStringLen(ctx, (const char *)o->carry, off);
        }
        if (kind == HS_PARK_NEXT) return hs_iter_result(ctx, JS_UNDEFINED, 1);
        return JS_NULL;
    }
    size_t total = off + (size_t)n;
    if (kind == HS_PARK_TEXT) {
        size_t keep = hs_utf8_complete(p, total);
        o->ncarry = (int)(total - keep);
        memcpy(o->carry, p + keep, (size_t)o->ncarry);
        if (keep == 0) {   /* only part of a character: wait for the rest */
            js_free(ctx, p);
            goto retry;
        }
        JSValue t = JS_NewStringLen(ctx, (const char *)p, keep);
        js_free(ctx, p);
        return t;
    }
    JSValue ab = JS_NewArrayBufferCopy(ctx, p, total);
    js_free(ctx, p);
    return kind == HS_PARK_NEXT ? hs_iter_result(ctx, ab, 0) : ab;
}

/* The handle with its status and headers defined on it, once they arrived. */
static JSValue hs_head_now(JSContext *ctx, HlJsHttpStream *o, JSValueConst self,
                           int *again)
{
    *again = 0;
    if (!o->s) return hs_throw(ctx, o, HL_HTTP_STREAM_E_CLOSED);
    int status = 0, n = 0;
    const HlHttpHeader *h = NULL;
    int rc = hl_http_stream_head(o->s, &status, &h, &n);
    if (rc == HL_HTTP_STREAM_E_AGAIN) { *again = 1; return JS_UNDEFINED; }
    if (rc != 0) {
        JSValue e = hs_throw(ctx, o, rc);
        hs_finish(o, rc);
        return e;
    }
    hs_slot_check(o);
    JSValue headers = JS_NewObject(ctx);
    if (JS_IsException(headers)) return headers;
    for (int i = 0; i < n; i++) {
        size_t nlen = strlen(h[i].name);
        char *lower = js_malloc(ctx, nlen + 1);
        if (!lower) { JS_FreeValue(ctx, headers); return JS_EXCEPTION; }
        for (size_t j = 0; j < nlen; j++) {
            char c = h[i].name[j];
            lower[j] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        lower[nlen] = '\0';
        int drc = js_http_define(ctx, headers, lower, JS_NewString(ctx, h[i].value));
        js_free(ctx, lower);
        if (drc != 0) { JS_FreeValue(ctx, headers); return JS_EXCEPTION; }
    }
    JSValue obj = JS_DupValue(ctx, self);
    if (js_http_define(ctx, obj, "status", JS_NewInt32(ctx, status)) != 0 ||
        js_http_define(ctx, obj, "headers", headers) != 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

/* The pin keeps o alive through this call: it is dropped only when the ctx
 * is retired, after the resume. */
static JSValue hs_push_result(JSContext *ctx, void *driver)
{
    HlJsHttpStream *o = (HlJsHttpStream *)driver;
    int again = 0, kind = o->kind;
    JSValue v;
    o->kind = 0;
    if (kind == HS_PARK_HEAD)
        v = hs_head_now(ctx, o, o->self, &again);
    else
        v = hs_read_now(ctx, o, kind, &again);
    if (again) {   /* the cap only wakes a waiter that will not wait again */
        JS_FreeValue(ctx, v);
        return JS_ThrowInternalError(ctx, "httpClient stream: woken with "
                                     "nothing to read");
    }
    return v;
}

/* Park the call, returning the Promise its result settles; JS_EXCEPTION when
 * the runtime refuses the wait or the park cannot be armed. */
static JSValue hs_park(JSContext *ctx, HlJsHttpStream *o, int kind,
                       JSValueConst self, const char *what)
{
    HlJS *js = get_hl_js(ctx);
    if (hl_js_async_gate(ctx, js, what) != 0) return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, what)) return JS_EXCEPTION;
    if (!js || !js->base.async_ctx || o->ctx)
        return JS_ThrowInternalError(ctx, "%s: cannot wait", what);
    HlAsyncOp *op = hl_http_stream_pending_op(o->s);
    const HlAsyncBackend *be = hl_async_backend();
    if (!op || !be || !be->op_suspend || !be->op_cancel)
        return JS_ThrowInternalError(ctx, "%s: cannot wait", what);

    HlAsyncCtx *actx = hl_async_ctx_create(js->server, js->base.net_ctx,
                                           js->base.alloc);
    if (!actx) return JS_ThrowOutOfMemory(ctx);
    JSValue rf[2];
    JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) { hl_async_ctx_free(actx); return promise; }
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js, JSValue resolve,
        JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js, rf[0], rf[1],
                                                js->base.alloc, hs_push_result);
    if (!cont) {
        JS_FreeValue(ctx, rf[0]);
        JS_FreeValue(ctx, rf[1]);
        JS_FreeValue(ctx, promise);
        hl_async_ctx_free(actx);
        return JS_ThrowOutOfMemory(ctx);
    }
    actx->cont = cont;
    actx->driver = o;
    actx->op.on_cancel = hs_on_request_cancel;
    actx->detached = (hl_js_cont_suspend_conn(cont) == NULL);

    hl_http_stream_set_user(o->s, o);
    op->on_resume = hs_on_resume;
    op->on_cancel = hs_on_op_cancel;

    if (be->op_suspend(js->base.async_ctx, op) != 0) {
        actx->cont = NULL;
        cont->destroy(cont);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "%s: cannot wait", what);
    }
    if (!actx->detached && hl_js_op_suspend(js, (HlSuspendOp *)&actx->op) < 0) {
        be->op_cancel(js->base.async_ctx, op);
        actx->cont = NULL;
        cont->destroy(cont);
        hl_async_ctx_free(actx);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "%s: cannot wait", what);
    }
    o->ctx = actx;
    o->loop = js->base.async_ctx;
    o->kind = kind;
    /* The pin, for every kind of wait (the handle may have no other
     * reference); dropped when the ctx is retired. */
    (void)JS_DupValue(ctx, self);
    o->pins++;
    actx->free_driver = hs_ctx_done;
    if (actx->detached) {
        actx->on_shutdown = hs_on_shutdown;
        hl_async_ctx_track(actx, &js->base.async_live);
    }
    return promise;
}

/* A Promise already settled with @p v (taken), or rejected with the pending
 * exception when v is JS_EXCEPTION. */
static JSValue hs_settled(JSContext *ctx, JSValue v)
{
    JSValue rf[2];
    JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, v); return promise; }
    JSValue arg = v, ret;
    if (JS_IsException(v)) {
        arg = JS_GetException(ctx);
        ret = JS_Call(ctx, rf[1], JS_UNDEFINED, 1, &arg);
    } else {
        ret = JS_Call(ctx, rf[0], JS_UNDEFINED, 1, &arg);
    }
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, rf[0]);
    JS_FreeValue(ctx, rf[1]);
    return promise;
}

static HlJsHttpStream *hs_this(JSContext *ctx, JSValueConst this_val)
{
    HlJsHttpStream *o = JS_GetOpaque(this_val, hl_http_stream_class_id);
    if (!o) JS_ThrowTypeError(ctx, "httpClient stream: not a stream response");
    return o;
}

static JSValue hs_read_common(JSContext *ctx, JSValueConst this_val, int kind,
                              const char *what)
{
    HlJsHttpStream *o = hs_this(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    if (o->ctx && o->s) {
        JS_ThrowTypeError(ctx, "%s: another read is waiting on this stream", what);
        return hl_js_refusal_tag(ctx, HL_REFUSAL_BUSY);
    }
    int again = 0;
    JSValue v = hs_read_now(ctx, o, kind, &again);
    if (!again) return hs_settled(ctx, v);
    return hs_park(ctx, o, kind, this_val, what);
}

static JSValue js_hs_read(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return hs_read_common(ctx, this_val, HS_PARK_BYTES, "stream.read()");
}

static JSValue js_hs_read_text(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return hs_read_common(ctx, this_val, HS_PARK_TEXT, "stream.readText()");
}

static JSValue js_hs_next(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return hs_read_common(ctx, this_val, HS_PARK_NEXT, "stream iterator");
}

static JSValue js_hs_async_iterator(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}

/* close(): stops the request. A read parked on the stream is handed back
 * from the loop (never inside this caller's run) and finds it closed. A
 * stream whose end the reader already had stays as it was. */
static JSValue js_hs_close(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsHttpStream *o = hs_this(ctx, this_val);
    if (!o) return JS_EXCEPTION;
    if (o->s && o->ctx && o->ctx->detached_timer == 0) {
        uint64_t tid = o->loop
            ? hl_async_backend()->timer_add(o->loop, 0, hs_wake_fire, o->ctx) : 0;
        if (tid == 0)
            return JS_ThrowInternalError(ctx, "stream.close(): cannot schedule");
        o->ctx->detached_timer = (int64_t)tid;
    }
    hs_release(o);
    return JS_UNDEFINED;
}

/* The iterator's return(): a for-await that breaks out closes the stream. */
static JSValue js_hs_return(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    JSValue r = js_hs_close(ctx, this_val, argc, argv);
    if (JS_IsException(r)) return hs_settled(ctx, r);
    return hs_settled(ctx, hs_iter_result(ctx, JS_UNDEFINED, 1));
}

static void js_hs_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlJsHttpStream *o = JS_GetOpaque(val, hl_http_stream_class_id);
    if (!o) return;
    /* A parked wait pins the handle, so this runs with no wait parked. Were
     * one left, its ctx must not reach this struct, and the wake a close()
     * armed must not fire into a ctx its owner then frees. */
    if (o->ctx) {
        hs_cancel_wake(o->ctx, o->loop);
        o->ctx->driver = NULL;
        o->ctx = NULL;
    }
    hs_release(o);
    free(o);
}

/* No gc_mark: the struct owns no JSValue but the pins, and reporting them
 * would let the cycle collector free a handle a parked wait still needs. */
static const JSClassDef js_hs_class = {
    "HttpStream",
    .finalizer = js_hs_finalizer,
};

static const JSCFunctionListEntry js_hs_methods[] = {
    JS_CFUNC_DEF("read", 0, js_hs_read),
    JS_CFUNC_DEF("readText", 0, js_hs_read_text),
    JS_CFUNC_DEF("close", 0, js_hs_close),
    JS_CFUNC_DEF("next", 0, js_hs_next),
    JS_CFUNC_DEF("return", 0, js_hs_return),
};

/* A non-negative integer option, or the default; a value that is present
 * and not an integer of at least @p lo throws. */
static int hs_opt_num(JSContext *ctx, JSValueConst opts, const char *name,
                      double lo, double hi, double *out)
{
    if (!JS_IsObject(opts)) return 0;
    JSValue v = JS_GetPropertyStr(ctx, opts, name);
    if (JS_IsException(v)) return -1;
    if (JS_IsUndefined(v) || JS_IsNull(v)) return 0;
    double d = 0;
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) < 0 || !(d >= lo) ||
        (d <= 9007199254740991.0 && d != (double)(int64_t)d)) {
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "httpClient.async.stream: opts.%s must be an "
                          "integer of at least %.0f", name, lo);
        return -1;
    }
    *out = d > hi ? hi : d;
    return 0;
}

/* httpClient.async.stream(method, url, opts?) -> Promise<response> */
static JSValue js_http_stream(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    const char *what = "httpClient.async.stream()";
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured (no hosts in manifest)");
    if (!js->base.async_ctx || !js->server)
        return JS_ThrowInternalError(ctx, "%s requires an active event loop", what);
    if (hl_js_async_gate(ctx, js, what) != 0) return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, what)) return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "httpClient.async.stream requires (method, url, opts?)");

    int timeout_ms = 0;
    if (js_http_timeout_opt(ctx, argc, argv, 2, "httpClient.async.stream", &timeout_ms) != 0)
        return JS_EXCEPTION;
    JSValueConst opts = argc >= 3 ? argv[2] : JS_UNDEFINED;
    double idle = 0, max_bytes = 0, buf_bytes = 0, life = 0;
    if (hs_opt_num(ctx, opts, "idleTimeoutMs", 1, HL_HTTP_STREAM_IDLE_MAX_MS, &idle) ||
        hs_opt_num(ctx, opts, "lifeMs", 1, HL_HTTP_STREAM_MAX_LIFE_MS, &life) ||
        hs_opt_num(ctx, opts, "maxBytes", 1, 9007199254740991.0, &max_bytes) ||
        hs_opt_num(ctx, opts, "bufferBytes", HL_HTTP_STREAM_BUF_MIN,
                   HL_HTTP_STREAM_BUF_MAX, &buf_bytes))
        return JS_EXCEPTION;
    JS_HTTP_CHARGE_REQUEST(ctx);

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = method ? JS_ToCString(ctx, argv[1]) : NULL;
    if (!method || !url) {
        if (method) JS_FreeCString(ctx, method);
        return JS_EXCEPTION;
    }
    JsHttpBody jbody = {0};
    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (JS_IsObject(opts)) {
        JSValue bv = JS_GetPropertyStr(ctx, opts, "body");
        int bad = JS_IsException(bv) || js_http_body_get(ctx, bv, &jbody, what) != 0;
        JS_FreeValue(ctx, bv);
        if (!bad) {
            JSValue hv = JS_GetPropertyStr(ctx, opts, "headers");
            bad = JS_IsException(hv) ||
                  (JS_IsObject(hv) && js_parse_http_headers(ctx, hv, &headers,
                                                            &num_headers) != 0);
            JS_FreeValue(ctx, hv);
        }
        if (bad) {
            JS_FreeCString(ctx, method);
            JS_FreeCString(ctx, url);
            js_http_body_free(ctx, &jbody);
            return JS_EXCEPTION;
        }
    }
    JSValue obj = JS_UNDEFINED;
    HlJsHttpStream *o = NULL;
    HlOffheapHold hold = {0};
    /* App code ran (toString, getters): the wait rules again. */
    if (hl_js_async_gate(ctx, js, what) != 0 || hl_js_db_refuse_wait(ctx, what))
        goto fail;
    size_t cap = buf_bytes > 0 ? (size_t)buf_bytes : HL_HTTP_STREAM_BUF_DEFAULT;
    /* At the in-flight cap, streams the app dropped without close() hold
     * slots until they are collected: collect once (charged) first. */
    if (js->base.http_inflight >= HL_HTTP_MAX_INFLIGHT && hl_js_gc_for_retry(ctx))
        goto fail;
    int admit = hl_http_fetch_admit(js->base.http_inflight, &hold,
                                    &js->wasm_buf_bytes, js->max_heap_bytes,
                                    jbody.len + cap);
    if (admit != HL_HTTP_ADMIT_OK) {
        if (admit == HL_HTTP_ADMIT_INFLIGHT)
            JS_ThrowRangeError(ctx, "%s: too many requests in flight (at most "
                               "%d per VM)", what, HL_HTTP_MAX_INFLIGHT);
        else
            JS_ThrowRangeError(ctx, "%s: the stream's buffer would take the "
                               "VM's off-heap copies past its heap limit", what);
        hl_js_refusal_tag(ctx, HL_REFUSAL_BUSY);
        goto fail;
    }
    obj = JS_NewObjectClass(ctx, (int)hl_http_stream_class_id);
    if (JS_IsException(obj)) goto fail;
    o = calloc(1, sizeof *o);
    if (!o) { JS_ThrowOutOfMemory(ctx); goto fail; }
    o->self = obj;   /* not owned */
    o->rt = JS_GetRuntime(ctx);
    JS_SetOpaque(obj, o);

    HlHttpStreamConfig cfg = {
        .server = js->server, .async = js->base.async_ctx,
        .http_cfg = js->base.http_cfg, .timeout_ms = timeout_ms,
        .idle_ms = (int)idle, .life_ms = (int)life,
        .buf_cap = cap, .max_bytes = (size_t)max_bytes,
        .method = method, .url = url, .headers = headers,
        .num_headers = num_headers, .body = jbody.p, .body_len = jbody.len,
    };
    if (hl_http_stream_start(&o->s, &cfg) != 0) {
        HlHttpDenial denial;
        if (hl_http_url_denied(js->base.http_cfg, url, &denial))
            js_http_throw_denied(ctx, "httpClient.async.stream", &denial);
        else
            JS_ThrowInternalError(ctx, "%s: failed to start the request", what);
        goto fail;
    }
    o->hold = hold;
    hold = (HlOffheapHold){0};
    o->inflight = &js->base.http_inflight;
    js->base.http_inflight++;
    o->track.stop = hs_teardown;
    hl_async_teardown_link(&js->base.async_live, &o->track);
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    js_http_body_free(ctx, &jbody);
    js_free_http_headers(ctx, headers, num_headers);

    int again = 0;
    JSValue v = hs_head_now(ctx, o, obj, &again);
    JSValue r = again ? hs_park(ctx, o, HS_PARK_HEAD, obj, what) : hs_settled(ctx, v);
    if (JS_IsException(r)) hs_release(o);
    JS_FreeValue(ctx, obj);
    return r;

fail:
    hl_offheap_drop(&hold);
    JS_FreeValue(ctx, obj);   /* the finalizer frees o */
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    js_http_body_free(ctx, &jbody);
    js_free_http_headers(ctx, headers, num_headers);
    return JS_EXCEPTION;
}

/* The class and its prototype, once per runtime (the id once per process). */
static int hs_register_class(JSContext *ctx)
{
    JS_NewClassID(&hl_http_stream_class_id);
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (JS_IsRegisteredClass(rt, hl_http_stream_class_id)) return 0;
    if (JS_NewClass(rt, hl_http_stream_class_id, &js_hs_class) < 0) return -1;
    JSValue proto = JS_NewObject(ctx);
    if (JS_IsException(proto)) return -1;
    hl_js_set_function_list(ctx, proto, js_hs_methods,
        (int)(sizeof js_hs_methods / sizeof js_hs_methods[0]));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue ai = JS_GetPropertyStr(ctx, sym, "asyncIterator");
    if (JS_IsSymbol(ai)) {
        JSAtom atom = JS_ValueToAtom(ctx, ai);
        JS_DefinePropertyValue(ctx, proto, atom,
            JS_NewCFunction(ctx, js_hs_async_iterator, "[Symbol.asyncIterator]", 0),
            JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, ai);
    JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, global);
    hl_js_set_class_proto_frozen(ctx, hl_http_stream_class_id, proto);
    return 0;
}

static int js_http_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/http-client", "hull:http-client") != 0) return -1;

    JSValue http = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, http, "request",
                      JS_NewCFunction(ctx, js_http_request, "request", 3));
    JS_SetPropertyStr(ctx, http, "get",
                      JS_NewCFunction(ctx, js_http_get, "get", 2));
    JS_SetPropertyStr(ctx, http, "post",
                      JS_NewCFunction(ctx, js_http_post, "post", 3));
    JS_SetPropertyStr(ctx, http, "put",
                      JS_NewCFunction(ctx, js_http_put, "put", 3));
    JS_SetPropertyStr(ctx, http, "patch",
                      JS_NewCFunction(ctx, js_http_patch, "patch", 3));
    JS_SetPropertyStr(ctx, http, "delete",
                      JS_NewCFunction(ctx, js_http_del, "delete", 2));

    /* http.async sub-object */
    JSValue async_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, async_obj, "request",
                      JS_NewCFunction(ctx, js_http_async_request, "request", 3));
    JS_SetPropertyStr(ctx, async_obj, "get",
                      JS_NewCFunction(ctx, js_http_async_get, "get", 2));
    JS_SetPropertyStr(ctx, async_obj, "post",
                      JS_NewCFunction(ctx, js_http_async_post, "post", 3));
    JS_SetPropertyStr(ctx, async_obj, "put",
                      JS_NewCFunction(ctx, js_http_async_put, "put", 3));
    JS_SetPropertyStr(ctx, async_obj, "patch",
                      JS_NewCFunction(ctx, js_http_async_patch, "patch", 3));
    JS_SetPropertyStr(ctx, async_obj, "delete",
                      JS_NewCFunction(ctx, js_http_async_delete, "delete", 2));
    if (hs_register_class(ctx) != 0) {
        JS_FreeValue(ctx, async_obj);
        JS_FreeValue(ctx, http);
        return -1;
    }
    JS_SetPropertyStr(ctx, async_obj, "stream",
                      JS_NewCFunction(ctx, js_http_stream, "stream", 3));
    JS_SetPropertyStr(ctx, http, "async", async_obj);

    hl_js_export_frozen(ctx, m, "httpClient", http);
    return 0;
}

int hl_js_init_http_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:http-client", js_http_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "httpClient");
    return 0;
}
