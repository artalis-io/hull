/*
 * mod_json.c — hull:json module (encode/decode wrappers)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

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

    const char *str = JS_ToCString(ctx, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    JSValue result = JS_ParseJSON(ctx, str, strlen(str), "<json>");
    JS_FreeCString(ctx, str);
    return result;
}

static int js_json_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue json = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, json, "encode",
                      JS_NewCFunction(ctx, js_json_encode, "encode", 1));
    JS_SetPropertyStr(ctx, json, "decode",
                      JS_NewCFunction(ctx, js_json_decode, "decode", 1));
    JS_SetModuleExport(ctx, m, "json", json);
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
