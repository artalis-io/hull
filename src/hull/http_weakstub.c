/*
 * http_weakstub.c — weak no-op defaults for the per-runtime HTTP web bindings
 * (issue #114, Phase C).
 *
 * Phase C splits the per-runtime web bindings (routes / dispatch / the res:*
 * helpers / ws / sse / http-client / smtp / the in-process test harness /
 * timers) out of the runtime feature archive into a separate
 * libhull_feature-http-<rt>.a. The *pure* runtime archive still references a
 * few of those symbols from its always-present core objects:
 *
 *   runtime.o  -> hl_<rt>_wire_routes[_server], hl_<rt>_test_register/run/clear
 *                 (+ hl_js_test_free on JS) via the serve / test vtable slots.
 *   async.o    -> hl_<rt>_timer_reschedule via the timer-resume path.
 *
 * Those call sites are already `#ifdef HL_ENABLE_HTTP_SERVER`-guarded, so they
 * exist in a feature build (compiled with the flag on). When the web-bindings
 * archive is composed the linker takes its strong definitions (it is
 * whole-archived at `hull build`); when it is NOT composed (a genuinely
 * HTTP-free CLI / pure-compute app) these weak no-ops satisfy the link, and the
 * guarded code paths (serve / test / timer-fire) are simply never reached.
 *
 * This mirrors cap/http_feature.c, but with the real prototypes (the base can
 * already see the runtime headers — serve.c includes them) rather than void*,
 * so the definitions are ODR/LTO-clean against the strong ones. The two timer
 * structs are runtime-internal (internal.h); forward-declared here by their
 * named tag, which is type-compatible with the real HlLuaTimer / HlJSTimer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>

#ifdef HL_ENABLE_HTTP_SERVER

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"    /* HlLua, KlServer, KlRouter, lua_State + wire protos */
#include "hull/runtime/test.h"   /* HlTestCaseResult + test protos */

struct HlLuaTimer;

__attribute__((weak)) int hl_lua_wire_routes(HlLua *lua, KlRouter *router)
{
    (void)lua; (void)router;
    return -1;
}

__attribute__((weak)) int hl_lua_wire_routes_server(HlLua *lua, KlServer *server,
                                                    void *(*alloc_fn)(size_t))
{
    (void)lua; (void)server; (void)alloc_fn;
    return -1;
}

__attribute__((weak)) void hl_lua_test_register(lua_State *L, KlRouter *router,
                                                HlLua *lua)
{
    (void)L; (void)router; (void)lua;
}

__attribute__((weak)) void hl_lua_test_clear(lua_State *L)
{
    (void)L;
}

__attribute__((weak)) void hl_lua_test_run(lua_State *L, int *total, int *passed,
                                           int *failed, FILE *out,
                                           HlTestCaseResult *results, int max_results)
{
    (void)L; (void)total; (void)passed; (void)failed;
    (void)out; (void)results; (void)max_results;
}

__attribute__((weak)) void hl_lua_timer_reschedule(struct HlLuaTimer *t)
{
    (void)t;
}
#endif /* HL_ENABLE_LUA */

#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"     /* HlJS, JSContext, KlServer, KlRouter + wire protos */
#include "hull/runtime/test.h"   /* HlTestCaseResult + test protos */

struct HlJSTimer;

__attribute__((weak)) int hl_js_wire_routes(HlJS *js, KlRouter *router)
{
    (void)js; (void)router;
    return -1;
}

__attribute__((weak)) int hl_js_wire_routes_server(HlJS *js, KlServer *server,
                                                   void *(*alloc_fn)(size_t))
{
    (void)js; (void)server; (void)alloc_fn;
    return -1;
}

__attribute__((weak)) void hl_js_test_register(JSContext *ctx, KlRouter *router,
                                               HlJS *js)
{
    (void)ctx; (void)router; (void)js;
}

__attribute__((weak)) void hl_js_test_free(JSContext *ctx)
{
    (void)ctx;
}

__attribute__((weak)) void hl_js_test_clear(JSContext *ctx)
{
    (void)ctx;
}

__attribute__((weak)) void hl_js_test_run(JSContext *ctx, int *total, int *passed,
                                          int *failed, FILE *out,
                                          HlTestCaseResult *results, int max_results)
{
    (void)ctx; (void)total; (void)passed; (void)failed;
    (void)out; (void)results; (void)max_results;
}
#endif /* HL_ENABLE_JS */

#endif /* HL_ENABLE_HTTP_SERVER */
