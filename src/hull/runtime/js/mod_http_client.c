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
#include "hull/cap/http_async.h"
#include "hull/shared/async.h"
#include "hull/utils/alloc.h"

#include <keel/http_server.h>

#include <limits.h>
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

    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    js_http_body_free(ctx, &jbody);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http request failed: %s",
                                      kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_http_client_response_free(&resp);
    return result;
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
    JS_FreeCString(ctx, url);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http.get failed: %s",
                                      kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_http_client_response_free(&resp);
    return result;
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
    JS_FreeCString(ctx, url);
    js_http_body_free(ctx, &jbody);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http.%s failed: %s",
                                      method_name, kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_http_client_response_free(&resp);
    return result;
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
    JS_FreeCString(ctx, url);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http.del failed: %s",
                                      kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_http_client_response_free(&resp);
    return result;
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
            return JS_ThrowRangeError(ctx, "http.fetch: too many requests "
                "in flight (at most %d per VM)", HL_HTTP_MAX_INFLIGHT);
        return JS_ThrowRangeError(ctx, "http.fetch: a %zu-byte body would "
            "take the VM's off-heap copies past its heap limit", body_len);
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
