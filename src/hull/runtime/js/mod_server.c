/*
 * mod_server.c — hull:server module (server stats)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include <keel/server.h>

/* server.stats() -> { activeConnections, maxConnections, asyncSuspended,
 *                     listenPaused } */
static JSValue js_server_stats(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;

    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->server)
        return JS_ThrowInternalError(ctx, "server.stats: server not available");

    KlServerStats stats;
    kl_server_stats(js->server, &stats);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "activeConnections",
                      JS_NewInt32(ctx, stats.active_connections));
    JS_SetPropertyStr(ctx, obj, "maxConnections",
                      JS_NewInt32(ctx, stats.max_connections));
    JS_SetPropertyStr(ctx, obj, "asyncSuspended",
                      JS_NewInt32(ctx, stats.async_suspended));
    JS_SetPropertyStr(ctx, obj, "listenPaused",
                      JS_NewBool(ctx, stats.listen_paused));
    return obj;
}

static int js_server_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue server = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, server, "stats",
                      JS_NewCFunction(ctx, js_server_stats, "stats", 0));
    JS_SetModuleExport(ctx, m, "server", server);
    return 0;
}

int hl_js_init_server_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:server", js_server_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "server");
    return 0;
}
