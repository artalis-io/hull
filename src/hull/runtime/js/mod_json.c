/*
 * mod_json.c - hull:json module (encode/decode wrappers)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "internal.h"   /* hl_js_export_frozen (audit 18) */

static JSValue js_json_encode(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "json.encode requires (value)");

    JSValue result = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(result))
        return JS_EXCEPTION;
    /* JSON.stringify returns undefined for unsupported types */
    if (JS_IsUndefined(result))
        return JS_NewString(ctx, "null");
    return result;
}

static JSValue js_json_decode(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "json.decode requires (str)");

    size_t len = 0;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    /* The whole string: strlen stopped at an embedded NUL, so
     * '{"a":1}\u0000junk' decoded as {"a":1} where JSON.parse and the Lua
     * decoder refuse it (audit 22). */
    JSValue result = JS_ParseJSON(ctx, str, len, "<json>");
    JS_FreeCString(ctx, str);
    return result;
}

static int js_json_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/json", "hull:json") != 0) return -1;

    JSValue json = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, json, "encode",
                      JS_NewCFunction(ctx, js_json_encode, "encode", 1));
    JS_SetPropertyStr(ctx, json, "decode",
                      JS_NewCFunction(ctx, js_json_decode, "decode", 1));
    hl_js_export_frozen(ctx, m, "json", json);
    return 0;
}

int hl_js_init_json_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:json", js_json_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "json");
    return 0;
}
