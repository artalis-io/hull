/*
 * http_feature.h - the HTTP-as-a-composable-feature seam (issue #114).
 *
 * Mirrors the tui feature seam (cap/tui.h): a weak no-op default lives in the
 * runtime-agnostic base (src/hull/cap/http_feature.c); a strong override that
 * registers the http/ws/sse/smtp modules lives on the HTTP side of the seam
 * (src/hull/runtime/{lua,js}/http_register.c). The composed `http` feature
 * supplies those overrides; a reduced base with no HTTP composed keeps the
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

struct KlHttpResponse;
struct KlHttpRequest;
struct KlHttpConn;

/* Write a 500 "Internal Server Error" (status + text/plain + body) to the
 * response. Extracted from the core dispatch/async error paths so they hold no
 * Keel-response refs; weak no-op when no HTTP is composed (that path is never
 * reached without a request in flight). Per-runtime so each strong override
 * rides its own runtime's http side. */
void hl_lua_http_error_response(struct KlHttpResponse *res);
void hl_js_http_error_response(struct KlHttpResponse *res);

/* Finalize + send a resumed request's response. The base runtime's async resume
 * (lua_rt_async.o / js_async.o) is composed for compute apps too, so it must
 * hold NO Keel refs at all - every kl_http_* call here (kl_http_conn_response,
 * kl_http_response_end_stream, and especially kl_http_request_send_response,
 * which lives in Keel's heavy http_server_core object) would otherwise drag the
 * HTTP server into a Keel-less compute app. So the whole finalize block lives
 * behind this seam: weak no-op when no HTTP is composed (a compute app never
 * resumes a request), strong per-runtime override in bindings_response.c.
 * _resume_send is the OK path (end a streamed body, then transition the conn to
 * SENDING - the v3 successor to the old conn->state = KL_CONN_SENDING write);
 * _resume_error writes a 500 then sends. The SENDING transition is required on
 * the poll backend, where kl_async_complete alone does not drive a resumed
 * handler's send. */
void hl_lua_http_resume_send(struct KlHttpConn *conn, struct KlHttpRequest *req);
void hl_js_http_resume_send(struct KlHttpConn *conn, struct KlHttpRequest *req);
void hl_lua_http_resume_error(struct KlHttpConn *conn, struct KlHttpRequest *req);
void hl_js_http_resume_error(struct KlHttpConn *conn, struct KlHttpRequest *req);

/* Free the WebSocket registry (runtime teardown). Extracted from the runtime
 * teardown paths so lua/js runtime.o hold no hl_ws_* refs; weak no-op when no
 * HTTP server is composed (a CLI build never creates a registry). Runtime-
 * agnostic (the registry is a shared base field). */
void hl_http_ws_registry_free(void *ws_registry);

/* Register the HTTP-dependent hull.* modules (http-client, http-server, smtp,
 * ws-server, ws-client) + the sse/multipart request metatables into the given
 * runtime. Weak no-op when no HTTP is composed. The signatures mirror the tui
 * seam: Lua registration can't fail (void), JS module init can (returns
 * 0 / -1) and needs the HlJS for per-module base config. */
void hl_lua_register_http_modules(void *lua_state);
int  hl_js_register_http_modules(void *js_ctx, void *hl_js);

/* 1 iff an HTTP subsystem is composed (strong override present), else 0. */
int hl_http_feature_present(void);

/* Cosmo HTTP-bridge force-link anchors (0.13.1 PR#1).
 *
 * These are unique STRONG symbols that exist ONLY in the per-runtime HTTP bridge
 * objects (routes.o carries hl_<rt>_http_bridge_anchor; http_register.o carries
 * hl_<rt>_http_register_anchor), with NO weak counterpart in http_weakstub.o /
 * http_feature.o. On the native SLIM base the bridge is whole-archived, so these
 * are unused there. On cosmo the bridge lives as MEMBERS of the fat platform
 * archive alongside the weak stubs; a produced cosmo app resolves serve.o's
 * undefined hl_<rt>_wire_routes_server against the weak no-op (which returns -1
 * and aborts serving) and never pulls the strong member. `hull build` emits a
 * reference to the selected runtime's anchors so the linker force-pulls the
 * strong bridge members, overriding the weak stubs. Because an anchor has no
 * weak twin, referencing one is a hard link error if the bridge is absent
 * (fail closed). Defined under HL_ENABLE_HTTP_SERVER in the matching TU. */
extern int hl_lua_http_bridge_anchor;
extern int hl_lua_http_register_anchor;
extern int hl_js_http_bridge_anchor;
extern int hl_js_http_register_anchor;

#ifdef __cplusplus
}
#endif

#endif /* HL_HTTP_FEATURE_H */
