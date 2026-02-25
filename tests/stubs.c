/*
 * stubs.c — Stub implementations for test builds without Keel
 *
 * Provides dummy hull_js_make_request/hull_js_make_response so that
 * js_runtime.c can link without the full Keel binding layer.
 */

#include "quickjs.h"

/* Stub: creates an empty JS object instead of a real request */
JSValue hull_js_make_request(JSContext *ctx, void *req)
{
    (void)req;
    return JS_NewObject(ctx);
}

/* Stub: creates an empty JS object instead of a real response */
JSValue hull_js_make_response(JSContext *ctx, void *res)
{
    (void)res;
    return JS_NewObject(ctx);
}
