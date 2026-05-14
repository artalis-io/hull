/*
 * internal.h — private cross-file declarations for JS runtime
 *
 * Shared declarations for runtime.c / dispatch.c / routes.c / timers.c /
 * ws.c / sse.c after the runtime.c god-module was split. Each of those
 * files used to live in a single TU and freely called static helpers
 * from a sibling section; here we surface only the symbols that now
 * need to cross the new file boundaries.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_JS_INTERNAL_H
#define HL_RUNTIME_JS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "hull/runtime/js.h"
#include "quickjs.h"

/* Forward declaration for Keel WS server connection (avoid pulling
 * <keel/websocket_server.h> into every TU). */
struct KlWsServerConn;

/* Forward struct types used across files */
typedef struct HlJSWsRoute HlJSWsRoute;
typedef struct HlJSSseRoute HlJSSseRoute;
struct HlJSWsRoute {
    HlJS *js;
    int   on_open_id;
    int   on_message_id;
    int   on_close_id;
    char  path[256];
};
struct HlJSSseRoute {
    HlJS *js;
    int   handler_id;
};

/* ── Promoted: defined in timers.c, used in routes.c (initial schedule
 * during wire_routes_server) and from within timers.c itself. */
int64_t hl_js_compute_daily_delay_ms(int hour, int minute, int use_local);
void hl_js_timer_trampoline(void *user_data);
int hl_js_track_timer(HlJS *js, void *timer);

/* ── Promoted: defined in ws.c, used in routes.c (kl_ws_server_config
 * wiring during wire_routes_server). */
void hl_js_ws_on_open(struct KlWsServerConn *ws_conn, void *user_data);
void hl_js_ws_on_message(struct KlWsServerConn *ws_conn, const char *data,
                         size_t len, int is_binary, void *user_data);
void hl_js_ws_on_close(struct KlWsServerConn *ws_conn, uint16_t code,
                       const char *reason, size_t reason_len,
                       void *user_data);

/* ── Promoted: defined in sse.c, used in routes.c (kl_server_route for
 * SSE endpoints during wire_routes_server). */
void hl_js_sse_handler(struct KlRequest *req, struct KlResponse *res,
                       void *user_data);

/* ── Promoted: defined in routes.c, used by sibling files when they
 * need to register additional dynamically-created routes. Currently
 * only routes.c uses them, but keep declarations here so future split
 * files can reuse without re-introducing duplicates. */
int hl_js_track_route(HlJS *js, void *route);
int hl_js_track_alloc(HlJS *js, void ***arr, size_t *count,
                      size_t *cap, void *ptr);

#endif /* HL_RUNTIME_JS_INTERNAL_H */
