/*
 * refusal.h - tag the pending JS exception with a refusal code (audit 35;
 * the codes are in include/hull/shared/refusal.h).
 */
#ifndef HULL_RUNTIME_JS_REFUSAL_H
#define HULL_RUNTIME_JS_REFUSAL_H

#include <quickjs.h>
#include "hull/shared/refusal.h"

/* Give the Error just thrown on @p ctx (by a JS_Throw*Error the caller made
 * a line earlier) an own `code` = @p code, and throw it again. Returns
 * JS_EXCEPTION, so
 *     JS_ThrowRangeError(ctx, ...); return hl_js_refusal_tag(ctx, HL_REFUSAL_BUSY);
 * replaces `return JS_ThrowRangeError(ctx, ...)`. */
static inline JSValue hl_js_refusal_tag(JSContext *ctx, const char *code)
{
    JSValue e = JS_GetException(ctx);
    if (JS_IsError(ctx, e)) {
        JSValue s = JS_NewString(ctx, code);
        if (JS_IsException(s))
            JS_FreeValue(ctx, JS_GetException(ctx));
        else
            JS_DefinePropertyValueStr(ctx, e, "code", s,
                                      JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    }
    return JS_Throw(ctx, e);
}

/* The same, as an expression around the throw:
 *     return hl_js_refuse(ctx, HL_REFUSAL_BUSY, JS_ThrowRangeError(ctx, ...));
 * (@p thrown, JS_EXCEPTION, is evaluated first, so the Error is pending). */
static inline JSValue hl_js_refuse(JSContext *ctx, const char *code,
                                   JSValue thrown)
{
    (void)thrown;
    return hl_js_refusal_tag(ctx, code);
}

#endif /* HULL_RUNTIME_JS_REFUSAL_H */
