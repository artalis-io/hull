/*
 * mod_env.c - hull:env module (environment variable access)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/env.h"

static JSValue js_env_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.env_cfg)
        return JS_ThrowInternalError(ctx, "env not configured");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "env.get requires (name)");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    const char *val = hl_cap_env_get(js->base.env_cfg, name);
    JS_FreeCString(ctx, name);

    if (val)
        return JS_NewString(ctx, val);
    return JS_NULL;
}

/* env.allowed(name) → boolean: is name in the manifest allowlist? (Membership
 * only, never a value - lets callers tell "not declared" from "declared but
 * unset", which env.get folds together. Used by hull/config.) */
static JSValue js_env_allowed(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "env.allowed requires (name)");

    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;

    int allowed = (js && js->base.env_cfg)
        ? hl_cap_env_allowed(js->base.env_cfg, name) : 0;
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, allowed);
}

static int js_env_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/env", "hull:env") != 0) return -1;

    JSValue env = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, env, "get",
                      JS_NewCFunction(ctx, js_env_get, "get", 1));
    JS_SetPropertyStr(ctx, env, "allowed",
                      JS_NewCFunction(ctx, js_env_allowed, "allowed", 1));
    JS_SetModuleExport(ctx, m, "env", env);
    return 0;
}

int hl_js_init_env_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:env", js_env_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "env");
    return 0;
}
