/*
 * http_register.c — strong override of the HTTP-feature seam, JS side (#114).
 *
 * Mirror of runtime/lua/http_register.c: registers the HTTP-dependent hull:*
 * modules (http-client, http-server, smtp, ws-server, ws-client) + the
 * sse/multipart request classes, so the core JS module registry
 * (js_modules.o) no longer references them directly. Compiled only when an
 * HTTP half is enabled; a pure-compute base compiles this to an empty TU and
 * the base weak no-op wins. This composes into the `http` feature.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"  /* hl_js_init_{http,smtp,server,ws_server,ws_client}_module, hl_js_sse_register_class */
#include "internal.h"    /* hl_js_request_register */
#include "hull/http_feature.h"

#if defined(HL_ENABLE_HTTP_SERVER) || defined(HL_ENABLE_HTTP_CLIENT)

int hl_js_register_http_modules(void *js_ctx, void *hl_js)
{
    JSContext *ctx = (JSContext *)js_ctx;
    HlJS *js = (HlJS *)hl_js;

#ifdef HL_ENABLE_HTTP_CLIENT
    /* hull:http-client (http.fetch) + hull:smtp; per-function checks enforce
     * that http_cfg / smtp_cfg is set (wired from the manifest after load). */
    if (hl_js_init_http_module(ctx, js) != 0)
        return -1;
    if (hl_js_init_smtp_module(ctx, js) != 0)
        return -1;
#endif
#ifdef HL_ENABLE_HTTP_SERVER
    /* hull:http-server (server.stats); the get/post/use verbs land on the app
     * intrinsic via mod_app.c's install_app_http_server. Then the ws-server /
     * ws-client modules, the SSE class, and the streaming-multipart request
     * classes. Server-only. */
    if (hl_js_init_server_module(ctx, js) != 0)
        return -1;
    hl_js_sse_register_class(ctx);
    if (hl_js_init_ws_server_module(ctx, js) != 0)
        return -1;
    if (hl_js_init_ws_client_module(ctx, js) != 0)
        return -1;
    hl_js_request_register(ctx);
#endif
    return 0;
}

#endif /* HL_ENABLE_HTTP_SERVER || HL_ENABLE_HTTP_CLIENT */
