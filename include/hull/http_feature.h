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

struct KlResponse;

/* Write a 500 "Internal Server Error" (status + text/plain + body) to the
 * response. Extracted from the core dispatch/async error paths so they hold no
 * Keel-response refs; weak no-op when no HTTP is composed (that path is never
 * reached without a request in flight). Per-runtime so each strong override
 * rides its own runtime's http side. */
void hl_lua_http_error_response(struct KlResponse *res);
void hl_js_http_error_response(struct KlResponse *res);

/* Register the HTTP-dependent hull.* modules (http-client, http-server, smtp,
 * ws-server, ws-client) + the sse/multipart request metatables into the given
 * runtime. Weak no-op when no HTTP is composed. The signatures mirror the tui
 * seam: Lua registration can't fail (void), JS module init can (returns
 * 0 / -1) and needs the HlJS for per-module base config. */
void hl_lua_register_http_modules(void *lua_state);
int  hl_js_register_http_modules(void *js_ctx, void *hl_js);

/* 1 iff an HTTP subsystem is composed (strong override present), else 0. */
int hl_http_feature_present(void);

#ifdef __cplusplus
}
#endif

#endif /* HL_HTTP_FEATURE_H */
