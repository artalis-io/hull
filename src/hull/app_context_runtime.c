/*
 * app_context_runtime.c - toolchain-only runtime-typed accessors over
 * HlAppContext.
 *
 * Split out of app_context.c so the base app-runner object references no
 * concrete runtime factory symbol (hl_lua_factory / hl_js_factory). That is the
 * precondition for a slim single-runtime app: app_context.o is pulled by every
 * app, and if it named the non-composed runtime's factory the link would fail.
 *
 * Only the hull toolchain's agent introspection (agent/{eval,template,overview})
 * calls these. This TU is compiled with BOTH runtime macros and force-loaded
 * into hull, where both runtime archives resolve the factory symbols; it is not
 * part of the produced-app platform lib. The accessors build only on the
 * agnostic hl_app_context_factory() / hl_app_context_runtime() getters, so this
 * TU never needs the (opaque) HlAppContext struct layout.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/app_context.h"
#include "hull/runtime/factory.h"

/* A cast to a typed runtime pointer needs only a forward declaration of the
 * type, not its full struct shape (pulling in lua.h / js.h here would surface
 * the VM struct layout to a layer that has no business inspecting it). The
 * runtime's base HlRuntime is the first field, so (HlLua *)rt == the HlLua. */
#ifdef HL_ENABLE_LUA
typedef struct HlLua HlLua;
#endif
#ifdef HL_ENABLE_JS
typedef struct HlJS HlJS;
#endif

int hl_app_context_is_lua(HlAppContext *ctx)
{
    /* Identify via the factory pointer - canonical since roadmap item K, and
     * valid even before rt is created. */
#ifdef HL_ENABLE_LUA
    extern const HlRuntimeFactory hl_lua_factory;
    return (ctx && hl_app_context_factory(ctx) == &hl_lua_factory) ? 1 : 0;
#else
    (void)ctx;
    return 0;
#endif
}

#ifdef HL_ENABLE_LUA
HlLua *hl_app_context_lua(HlAppContext *ctx)
{
    extern const HlRuntimeFactory hl_lua_factory;
    if (!ctx || hl_app_context_factory(ctx) != &hl_lua_factory) return NULL;
    return (HlLua *)hl_app_context_runtime(ctx);   /* base is HlLua's first field */
}
#endif

#ifdef HL_ENABLE_JS
HlJS *hl_app_context_js(HlAppContext *ctx)
{
    extern const HlRuntimeFactory hl_js_factory;
    if (!ctx || hl_app_context_factory(ctx) != &hl_js_factory) return NULL;
    return (HlJS *)hl_app_context_runtime(ctx);
}
#endif
