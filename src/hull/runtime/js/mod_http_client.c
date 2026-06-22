/*
 * mod_http.c — hull:http module (HTTP client + async fetch)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/http.h"
#include "hull/cap/http_async.h"
#include "hull/shared/async.h"
#include "hull/utils/alloc.h"

#include <keel/server.h>

/* Parse JS headers object { name: value } into HlHttpHeader array.
 * Returns count. Caller must free with js_free_http_headers(). */
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

    int count = 0;
    for (uint32_t i = 0; i < prop_count; i++) {
        const char *name = JS_AtomToCString(ctx, props[i].atom);
        JSValue val = JS_GetProperty(ctx, obj, props[i].atom);
        const char *value = JS_ToCString(ctx, val);
        JS_FreeValue(ctx, val);

        if (name && value) {
            hdrs[count].name = name;
            hdrs[count].value = value;
            count++;
        } else {
            if (name) JS_FreeCString(ctx, name);
            if (value) JS_FreeCString(ctx, value);
        }
        JS_FreeAtom(ctx, props[i].atom);
    }
    js_free(ctx, props);

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

/* Push HTTP response as JS object: { status, body, headers } */
static JSValue js_push_http_response(JSContext *ctx, const KlClientResponse *resp)
{
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, resp->status));

    if (resp->body && resp->body_len > 0)
        JS_SetPropertyStr(ctx, obj, "body",
                          JS_NewStringLen(ctx, resp->body, resp->body_len));
    else
        JS_SetPropertyStr(ctx, obj, "body", JS_NewString(ctx, ""));

    /* Headers as { "name": "value" } — lowercase names */
    JSValue headers = JS_NewObject(ctx);
    for (int i = 0; i < resp->num_headers; i++) {
        /* Lowercase header name */
        size_t nlen = strlen(resp->headers[i].name);
        char *lower = js_malloc(ctx, nlen + 1);
        if (lower) {
            for (size_t j = 0; j < nlen; j++)
                lower[j] = (char)((resp->headers[i].name[j] >= 'A' &&
                                    resp->headers[i].name[j] <= 'Z')
                    ? resp->headers[i].name[j] + 32
                    : resp->headers[i].name[j]);
            lower[nlen] = '\0';
            JS_SetPropertyStr(ctx, headers, lower,
                              JS_NewString(ctx, resp->headers[i].value));
            js_free(ctx, lower);
        } else {
            JS_SetPropertyStr(ctx, headers, resp->headers[i].name,
                              JS_NewString(ctx, resp->headers[i].value));
        }
    }
    JS_SetPropertyStr(ctx, obj, "headers", headers);

    return obj;
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

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!method || !url) {
        if (method) JS_FreeCString(ctx, method);
        if (url) JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    const char *body = NULL;
    size_t body_len = 0;
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse opts */
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue body_val = JS_GetPropertyStr(ctx, argv[2], "body");
        if (JS_IsString(body_val))
            body = JS_ToCStringLen(ctx, &body_len, body_val);
        JS_FreeValue(ctx, body_val);

        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(js->base.http_cfg, method, url,
                                    headers, num_headers, body, body_len, &resp);

    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    // cppcheck-suppress knownConditionTrueFalse
    if (body) JS_FreeCString(ctx, body);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http request failed: %s",
                                      kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_client_response_free(&resp);
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

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(js->base.http_cfg, "GET", url,
                                    headers, num_headers, NULL, 0, &resp);
    JS_FreeCString(ctx, url);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http.get failed: %s",
                                      kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_client_response_free(&resp);
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

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    const char *body = NULL;
    size_t body_len = 0;
    if (argc >= 2 && JS_IsString(argv[1]))
        body = JS_ToCStringLen(ctx, &body_len, argv[1]);

    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(js->base.http_cfg, method_name, url,
                                    headers, num_headers, body, body_len, &resp);
    JS_FreeCString(ctx, url);
    // cppcheck-suppress knownConditionTrueFalse
    if (body) JS_FreeCString(ctx, body);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http.%s failed: %s",
                                      method_name, kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_client_response_free(&resp);
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

/* http.del(url, opts?) — same as http.get but DELETE */
static JSValue js_http_del(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.http_cfg)
        return JS_ThrowInternalError(ctx, "http not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http.del requires (url, opts?)");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    HlHttpHeader *headers = NULL;
    int num_headers = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    KlClientResponse resp;
    int rc = hl_cap_http_request(js->base.http_cfg, "DELETE", url,
                                    headers, num_headers, NULL, 0, &resp);
    JS_FreeCString(ctx, url);
    js_free_http_headers(ctx, headers, num_headers);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "http.del failed: %s",
                                      kl_strerror(resp.error));

    JSValue result = js_push_http_response(ctx, &resp);
    kl_client_response_free(&resp);
    return result;
}

/* ── Push async HTTP response into JS ─────────────────────────────── */

static JSValue js_push_async_http_response(JSContext *ctx, void *driver)
{
    const KlClientResponse *resp = hl_http_async_response(driver);
    if (!resp)
        return JS_UNDEFINED;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, resp->status));

    if (resp->body && resp->body_len > 0)
        JS_SetPropertyStr(ctx, obj, "body",
                          JS_NewStringLen(ctx, resp->body, resp->body_len));
    else
        JS_SetPropertyStr(ctx, obj, "body", JS_NewString(ctx, ""));

    /* Headers as { "name": "value" } — lowercase names */
    JSValue headers = JS_NewObject(ctx);
    for (int i = 0; i < resp->num_headers; i++) {
        size_t nlen = strlen(resp->headers[i].name);
        char *lower = js_malloc(ctx, nlen + 1);
        if (lower) {
            for (size_t j = 0; j < nlen; j++)
                lower[j] = (char)((resp->headers[i].name[j] >= 'A' &&
                                    resp->headers[i].name[j] <= 'Z')
                    ? resp->headers[i].name[j] + 32
                    : resp->headers[i].name[j]);
            lower[nlen] = '\0';
            JS_SetPropertyStr(ctx, headers, lower,
                              JS_NewString(ctx, resp->headers[i].value));
            js_free(ctx, lower);
        } else {
            JS_SetPropertyStr(ctx, headers, resp->headers[i].name,
                              JS_NewString(ctx, resp->headers[i].value));
        }
    }
    JS_SetPropertyStr(ctx, obj, "headers", headers);

    return obj;
}

/* http.fetch(method, url, opts?) — async non-blocking HTTP request. */
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

    if (argc < 2)
        return JS_ThrowTypeError(ctx, "http.fetch requires (method, url, opts?)");

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!method || !url) {
        if (method) JS_FreeCString(ctx, method);
        if (url) JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }

    const char *body = NULL;
    size_t body_len = 0;
    HlHttpHeader *headers = NULL;
    int num_headers = 0;

    /* Parse opts */
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue body_val = JS_GetPropertyStr(ctx, argv[2], "body");
        if (JS_IsString(body_val))
            body = JS_ToCStringLen(ctx, &body_len, body_val);
        JS_FreeValue(ctx, body_val);

        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (JS_IsObject(hdrs_val))
            js_parse_http_headers(ctx, hdrs_val, &headers, &num_headers);
        JS_FreeValue(ctx, hdrs_val);
    }

    /* Start async HTTP */
    HlAsyncCtx *async_ctx = hl_async_http_start(
        js->server, js->active_conn, js->base.net_ctx, js->base.alloc,
        js->base.http_cfg, method, url, headers, num_headers, body, body_len);

    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    // cppcheck-suppress knownConditionTrueFalse
    if (body) JS_FreeCString(ctx, body);
    js_free_http_headers(ctx, headers, num_headers);

    if (!async_ctx)
        return JS_ThrowInternalError(ctx, "http.fetch: failed to start request");

    /* Create Promise */
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;

    /* Create JS continuation */
    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_push_async_http_response);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "http.fetch: out of memory");
    }
    async_ctx->cont = cont;

    return promise;
}

/* ── http.async.* — async HTTP convenience methods ─────────────────── */

static JSValue js_http_async_request(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{ return js_http_fetch(ctx, this_val, argc, argv); }

static JSValue js_http_async_no_body(JSContext *ctx, JSValueConst this_val,
                                      const char *method,
                                      int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue method_val = JS_NewString(ctx, method);
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

    JSValue method_val = JS_NewString(ctx, method);

    JSValue opts = JS_NewObject(ctx);
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
        JS_SetPropertyStr(ctx, opts, "body", JS_DupValue(ctx, argv[1]));
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue hdrs = JS_GetPropertyStr(ctx, argv[2], "headers");
        if (!JS_IsUndefined(hdrs))
            JS_SetPropertyStr(ctx, opts, "headers", hdrs);
        else
            JS_FreeValue(ctx, hdrs);
    }

    JSValue args[3] = { method_val, argv[0], opts };
    JSValue result = js_http_fetch(ctx, JS_UNDEFINED, 3, args);
    JS_FreeValue(ctx, method_val);
    JS_FreeValue(ctx, opts);
    return result;
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

    JS_SetModuleExport(ctx, m, "httpClient", http);
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
