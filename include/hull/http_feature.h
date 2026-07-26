/*
 * http_feature.h — the HTTP-as-a-composable-feature seam (issue #114).
 *
 * Mirrors the tui feature seam (cap/tui.h): a weak no-op default lives in the
 * runtime-agnostic base (src/hull/cap/http_feature.c); a strong override that
 * registers the http/ws/sse/smtp modules lives on the HTTP side of the seam
 * (src/hull/runtime/{lua,js}/http_register.c). In Phase A those overrides ride
 * the runtime archive (behavior unchanged); Phase C relocates them into the
 * composed `http` feature, so a reduced base with no HTTP composed keeps the
 * weak no-op and never pulls the web bindings.
 *
 * Params are void* so this header pulls neither lua.h nor quickjs.h into base
 * consumers; the impls cast back to lua_State* / JSContext*.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_HTTP_FEATURE_H
#define HL_HTTP_FEATURE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the HTTP-dependent hull.* modules (http-client, http-server, smtp,
 * ws-server, ws-client) + the sse/multipart request metatables into the given
 * runtime. Weak no-op when no HTTP is composed. */
void hl_lua_register_http_modules(void *lua_state);
void hl_js_register_http_modules(void *js_ctx);

/* 1 iff an HTTP subsystem is composed (strong override present), else 0. */
int hl_http_feature_present(void);

#ifdef __cplusplus
}
#endif

#endif /* HL_HTTP_FEATURE_H */
