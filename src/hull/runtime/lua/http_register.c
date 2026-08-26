/*
 * http_register.c - strong override of the HTTP-feature seam, Lua side (#114).
 *
 * Registers the HTTP-dependent hull.* modules (http-client, http-server, smtp,
 * ws-server, ws-client) + the sse/multipart request metatables. Extracted out
 * of modules.c so the core module registry (lua_rt_modules.o) no longer
 * references the web-module openers directly - they are referenced only here,
 * on the HTTP side of the seam. Compiled only when an HTTP half is enabled; a
 * pure-compute base compiles this to an empty TU and the base weak no-op wins.
 *
 * This composes into the `http` feature.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"  /* luaopen_hull_{http,smtp,server,ws_server,ws_client}, hl_lua_sse_register_mt */
#include "internal.h"    /* hl_lua_request_register */
#include "hull/http_feature.h"

#if defined(HL_ENABLE_HTTP_SERVER) || defined(HL_ENABLE_HTTP_CLIENT)

/* Local copy of the modules.c helper: requiref into _LOADED, drop the value so
 * the module is import-only (not a global). */
static void register_native_module(lua_State *L, const char *name,
                                    lua_CFunction openf)
{
    luaL_requiref(L, name, openf, 0);
    lua_pop(L, 1);
}

void hl_lua_register_http_modules(void *lua_state)
{
    lua_State *L = (lua_State *)lua_state;

#ifdef HL_ENABLE_HTTP_CLIENT
    register_native_module(L, "hull.http-client", luaopen_hull_http);
    register_native_module(L, "hull.smtp",        luaopen_hull_smtp);
#endif
#ifdef HL_ENABLE_HTTP_SERVER
    /* hull.http-server provides server stats; registration verbs land on the
     * app intrinsic via install_app_http_server. */
    register_native_module(L, "hull.http-server",   luaopen_hull_server);
    register_native_module(L, "hull.web.ws-server", luaopen_hull_ws_server);
    register_native_module(L, "hull.web.ws-client", luaopen_hull_ws_client);
    /* SSE stream metatable (app.sse handler dispatch) + streaming-multipart
     * request bindings (req:multipart() / Part / Chunks). Server-only. */
    hl_lua_sse_register_mt(L);
    hl_lua_request_register(L);
#endif
}

/* hl_http_feature_present()'s strong override is runtime-agnostic (not defined
 * here, to avoid a duplicate symbol across the lua+js link). It lands with the
 * Kind-B slice that first needs it; the base weak default (0) holds until then. */

#endif /* HL_ENABLE_HTTP_SERVER || HL_ENABLE_HTTP_CLIENT */
