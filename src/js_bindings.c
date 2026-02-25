/*
 * js_bindings.c — Request/Response bridge to QuickJS
 *
 * Marshals Keel's KlRequest/KlResponse to JS objects and back.
 * This file contains ONLY data marshaling — all enforcement logic
 * lives in hull_cap_* functions.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/js_runtime.h"
#include "hull/hull_cap.h"
#include "quickjs.h"

#include <keel/request.h>
#include <keel/response.h>
#include <keel/router.h>

#include <string.h>
#include <stdio.h>

/* ── Request object ─────────────────────────────────────────────────── */

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
JSValue hull_js_make_request(JSContext *ctx, KlRequest *req)
{
    JSValue obj = JS_NewObject(ctx);

    /* method (Keel stores as string) */
    if (req->method)
        JS_SetPropertyStr(ctx, obj, "method",
                          JS_NewStringLen(ctx, req->method, req->method_len));
    else
        JS_SetPropertyStr(ctx, obj, "method", JS_NewString(ctx, "GET"));

    /* path */
    if (req->path)
        JS_SetPropertyStr(ctx, obj, "path",
                          JS_NewStringLen(ctx, req->path, req->path_len));
    else
        JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, "/"));

    /* query string → object */
    JSValue query_obj = JS_NewObject(ctx);
    if (req->query && req->query_len > 0) {
        /* Parse query string: key=val&key2=val2 */
        char qbuf[4096];
        size_t qlen = req->query_len < sizeof(qbuf) - 1
                      ? req->query_len : sizeof(qbuf) - 1;
        memcpy(qbuf, req->query, qlen);
        qbuf[qlen] = '\0';

        char *saveptr = NULL;
        char *pair = strtok_r(qbuf, "&", &saveptr);
        while (pair) {
            char *eq = strchr(pair, '=');
            if (eq) {
                *eq = '\0';
                JS_SetPropertyStr(ctx, query_obj, pair,
                                  JS_NewString(ctx, eq + 1));
            } else {
                JS_SetPropertyStr(ctx, query_obj, pair,
                                  JS_NewString(ctx, ""));
            }
            pair = strtok_r(NULL, "&", &saveptr);
        }
    }
    JS_SetPropertyStr(ctx, obj, "query", query_obj);

    /* params — route params are on KlConn, not KlRequest.
     * TODO: pass params via req->ctx once Keel supports it. */
    JS_SetPropertyStr(ctx, obj, "params", JS_NewObject(ctx));

    /* headers → object */
    JSValue headers_obj = JS_NewObject(ctx);
    for (int i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name && req->headers[i].value) {
            JS_SetPropertyStr(ctx, headers_obj,
                              req->headers[i].name,
                              JS_NewStringLen(ctx, req->headers[i].value,
                                              req->headers[i].value_len));
        }
    }
    JS_SetPropertyStr(ctx, obj, "headers", headers_obj);

    /* body — as a string for now; JSON parsing happens in hull:json */
    if (req->body_reader) {
        /* Try to get body from buffer reader */
        /* TODO: support multipart, chunked, etc. */
        JS_SetPropertyStr(ctx, obj, "body", JS_NewString(ctx, ""));
    } else {
        JS_SetPropertyStr(ctx, obj, "body", JS_NULL);
    }

    /* ctx — per-request context object (middleware → handler) */
    JS_SetPropertyStr(ctx, obj, "ctx", JS_NewObject(ctx));

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

/* Class ID for response opaque data */
static JSClassID hull_response_class_id;

static void hull_response_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    (void)val;
    /* KlResponse is owned by the connection pool, not by JS */
}

static JSClassDef hull_response_class = {
    "HullResponse",
    .finalizer = hull_response_finalizer,
};

static KlResponse *get_response(JSContext *ctx, JSValueConst this_val)
{
    return (KlResponse *)JS_GetOpaque(this_val, hull_response_class_id);
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
            kl_response_header(res, "Content-Type", "application/json");
            kl_response_body(res, json_str, strlen(json_str));
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
        kl_response_header(res, "Content-Type", "text/html; charset=utf-8");
        kl_response_body(res, html, strlen(html));
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
        kl_response_header(res, "Content-Type", "text/plain; charset=utf-8");
        kl_response_body(res, text, strlen(text));
        JS_FreeCString(ctx, text);
    }

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
        kl_response_body(res, "", 0);
        JS_FreeCString(ctx, url);
    }

    return JS_UNDEFINED;
}

/* ── Response class registration ────────────────────────────────────── */

static int hull_js_response_class_registered = 0;

static int hull_js_ensure_response_class(JSContext *ctx)
{
    if (hull_js_response_class_registered)
        return 0;

    JS_NewClassID(&hull_response_class_id);
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (JS_NewClass(rt, hull_response_class_id, &hull_response_class) < 0)
        return -1;

    /* Create prototype with methods */
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "status",
                      JS_NewCFunction(ctx, js_res_status, "status", 1));
    JS_SetPropertyStr(ctx, proto, "header",
                      JS_NewCFunction(ctx, js_res_header, "header", 2));
    JS_SetPropertyStr(ctx, proto, "json",
                      JS_NewCFunction(ctx, js_res_json, "json", 2));
    JS_SetPropertyStr(ctx, proto, "html",
                      JS_NewCFunction(ctx, js_res_html, "html", 1));
    JS_SetPropertyStr(ctx, proto, "text",
                      JS_NewCFunction(ctx, js_res_text, "text", 1));
    JS_SetPropertyStr(ctx, proto, "redirect",
                      JS_NewCFunction(ctx, js_res_redirect, "redirect", 2));

    JS_SetClassProto(ctx, hull_response_class_id, proto);
    hull_js_response_class_registered = 1;

    return 0;
}

/* ── Public: create JS request/response objects ─────────────────────── */

JSValue hull_js_make_response(JSContext *ctx, KlResponse *res)
{
    hull_js_ensure_response_class(ctx);

    JSValue obj = JS_NewObjectClass(ctx, (int)hull_response_class_id);
    JS_SetOpaque(obj, res);
    return obj;
}

/* hull_js_register_modules is in js_modules.c */
