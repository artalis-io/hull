/*
 * routes.c — JS route + middleware + timer + WS/SSE wiring
 *
 * Reads the route/middleware/timer/ws/sse definition arrays that
 * `app.<verb>()` builds on globalThis and registers them with Keel
 * (KlRouter or KlServer). Also hosts the tracked-allocation helpers
 * used by every wire step so we can free per-route contexts on
 * shutdown without leaking.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/alloc.h"
#include "hull/async_backend.h"
#include "hull/cap/body.h"
#include "hull/cap/ws.h"

#include <keel/keel.h>
#include <keel/body_reader_multipart.h>
#include <keel/websocket_server.h>

#include "log.h"

#include <limits.h>
#include <string.h>
#include <stdio.h>

/* ── Route tracking ────────────────────────────────────────────────── */

int hl_js_track_route(HlJS *js, void *route)
{
    if (js->route_count >= js->route_cap) {
        size_t new_cap = js->route_cap ? js->route_cap * 2 : 8;
        if (new_cap < js->route_cap || new_cap > SIZE_MAX / sizeof(void *))
            return -1; /* overflow */
        size_t old_sz = js->route_cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(js->base.alloc,
                                           js->routes, old_sz, new_sz);
        if (!new_arr)
            return -1;
        js->routes = new_arr;
        js->route_cap = new_cap;
    }
    js->routes[js->route_count++] = route;
    return 0;
}

/* ── Generic tracked-allocation helper ──────────────────────────────── */

int hl_js_track_alloc(HlJS *js, void ***arr, size_t *count,
                               size_t *cap, void *ptr)
{
    if (*count >= *cap) {
        size_t new_cap = *cap ? *cap * 2 : 4;
        if (new_cap < *cap || new_cap > SIZE_MAX / sizeof(void *))
            return -1;
        size_t old_sz = *cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(js->base.alloc,
                                           *arr, old_sz, new_sz);
        if (!new_arr)
            return -1;
        *arr = new_arr;
        *cap = new_cap;
    }
    (*arr)[(*count)++] = ptr;
    return 0;
}

/* ── Route wiring ──────────────────────────────────────────────────── */

int hl_js_wire_routes(HlJS *js, KlRouter *router)
{
    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_route_defs");

    if (JS_IsUndefined(defs) || !JS_IsArray(ctx, defs)) {
        JS_FreeValue(ctx, defs);
        JS_FreeValue(ctx, global);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    JSValue len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t count = 0;
    JS_ToInt32(ctx, &count, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < count; i++) {
        JSValue def = JS_GetPropertyUint32(ctx, defs, (uint32_t)i);
        if (JS_IsUndefined(def))
            continue;

        JSValue method_val = JS_GetPropertyStr(ctx, def, "method");
        JSValue pattern_val = JS_GetPropertyStr(ctx, def, "pattern");
        JSValue id_val = JS_GetPropertyStr(ctx, def, "handler_id");

        const char *method_str = JS_ToCString(ctx, method_val);
        const char *pattern = JS_ToCString(ctx, pattern_val);
        int32_t handler_id = 0;
        JS_ToInt32(ctx, &handler_id, id_val);

        if (method_str && pattern) {
            HlJSRoute *route = hl_alloc_malloc(js->base.alloc,
                                                 sizeof(HlJSRoute));
            if (route) {
                route->js = js;
                route->handler_id = handler_id;
                route->multipart_config = NULL;
                hl_js_track_route(js, route);
                kl_router_add(router, method_str, pattern,
                              hl_js_keel_handler, route, NULL);
            }
        }

        if (pattern) JS_FreeCString(ctx, pattern);
        if (method_str) JS_FreeCString(ctx, method_str);
        JS_FreeValue(ctx, id_val);
        JS_FreeValue(ctx, pattern_val);
        JS_FreeValue(ctx, method_val);
        JS_FreeValue(ctx, def);
    }

    JS_FreeValue(ctx, defs);

    /* Wire pre-body middleware from __hull_middleware */
    JSValue mw_arr = JS_GetPropertyStr(ctx, global, "__hull_middleware");
    if (JS_IsArray(ctx, mw_arr)) {
        JSValue mw_len = JS_GetPropertyStr(ctx, mw_arr, "length");
        int32_t mw_count = 0;
        JS_ToInt32(ctx, &mw_count, mw_len);
        JS_FreeValue(ctx, mw_len);

        for (int32_t i = 0; i < mw_count; i++) {
            JSValue entry = JS_GetPropertyUint32(ctx, mw_arr, (uint32_t)i);
            if (JS_IsUndefined(entry)) continue;

            JSValue m_val = JS_GetPropertyStr(ctx, entry, "method");
            JSValue p_val = JS_GetPropertyStr(ctx, entry, "pattern");
            JSValue id_val = JS_GetPropertyStr(ctx, entry, "handler_id");

            const char *m = JS_ToCString(ctx, m_val);
            const char *p = JS_ToCString(ctx, p_val);
            int32_t hid = 0;
            JS_ToInt32(ctx, &hid, id_val);

            if (m && p) {
                HlJSRoute *r = hl_alloc_malloc(js->base.alloc, sizeof(HlJSRoute));
                if (r) {
                    r->js = js;
                    r->handler_id = hid;
                    r->multipart_config = NULL;
                    hl_js_track_route(js, r);
                    kl_router_use(router, m, p, hl_js_keel_middleware, r);
                }
            }

            if (p) JS_FreeCString(ctx, p);
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, p_val);
            JS_FreeValue(ctx, m_val);
            JS_FreeValue(ctx, entry);
        }
    }
    JS_FreeValue(ctx, mw_arr);

    /* Wire post-body middleware from __hull_post_middleware */
    JSValue post_arr = JS_GetPropertyStr(ctx, global, "__hull_post_middleware");
    if (JS_IsArray(ctx, post_arr)) {
        JSValue post_len = JS_GetPropertyStr(ctx, post_arr, "length");
        int32_t post_count = 0;
        JS_ToInt32(ctx, &post_count, post_len);
        JS_FreeValue(ctx, post_len);

        for (int32_t i = 0; i < post_count; i++) {
            JSValue entry = JS_GetPropertyUint32(ctx, post_arr, (uint32_t)i);
            if (JS_IsUndefined(entry)) continue;

            JSValue m_val = JS_GetPropertyStr(ctx, entry, "method");
            JSValue p_val = JS_GetPropertyStr(ctx, entry, "pattern");
            JSValue id_val = JS_GetPropertyStr(ctx, entry, "handler_id");

            const char *m = JS_ToCString(ctx, m_val);
            const char *p = JS_ToCString(ctx, p_val);
            int32_t hid = 0;
            JS_ToInt32(ctx, &hid, id_val);

            if (m && p) {
                HlJSRoute *r = hl_alloc_malloc(js->base.alloc, sizeof(HlJSRoute));
                if (r) {
                    r->js = js;
                    r->handler_id = hid;
                    r->multipart_config = NULL;
                    hl_js_track_route(js, r);
                    kl_router_use_post(router, m, p, hl_js_keel_middleware, r);
                }
            }

            if (p) JS_FreeCString(ctx, p);
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, p_val);
            JS_FreeValue(ctx, m_val);
            JS_FreeValue(ctx, entry);
        }
    }
    JS_FreeValue(ctx, post_arr);

    JS_FreeValue(ctx, global);

    return 0;
}

/* ── Server route wiring (with body reader factory) ────────────────── */

/* Read a non-negative size_t field off a JS object at `obj`; default 0.
 * Caller still owns `obj`. */
static size_t js_read_size_field(JSContext *ctx, JSValueConst obj,
                                  const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    size_t out = 0;
    if (JS_IsNumber(v)) {
        int64_t i = 0;
        if (JS_ToInt64(ctx, &i, v) == 0 && i > 0) out = (size_t)i;
    }
    JS_FreeValue(ctx, v);
    return out;
}

/* Same but returns int (for max_parts). */
static int js_read_int_field(JSContext *ctx, JSValueConst obj,
                              const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int out = 0;
    if (JS_IsNumber(v)) {
        int64_t i = 0;
        if (JS_ToInt64(ctx, &i, v) == 0 && i > 0 && i <= INT_MAX)
            out = (int)i;
    }
    JS_FreeValue(ctx, v);
    return out;
}

/* Allocate KlMultipartConfig from JS subobject; caller frees with
 * hl_alloc_free(...,sizeof(KlMultipartConfig)). */
static KlMultipartConfig *js_build_multipart_config(HlJS *js, JSValueConst mp)
{
    KlMultipartConfig *cfg = hl_alloc_malloc(js->base.alloc,
                                              sizeof(KlMultipartConfig));
    if (!cfg) return NULL;
    JSContext *ctx = js->ctx;
    /* Accept both snake_case and camelCase for cross-runtime ergonomics —
     * Lua uses snake_case in the same opt names; JS app code idiomatically
     * passes camelCase. Snake-case takes priority if both are present. */
    size_t mps = js_read_size_field(ctx, mp, "max_part_size");
    if (!mps) mps = js_read_size_field(ctx, mp, "maxPartSize");
    size_t mts = js_read_size_field(ctx, mp, "max_total_size");
    if (!mts) mts = js_read_size_field(ctx, mp, "maxTotalSize");
    int    mp_ = js_read_int_field (ctx, mp, "max_parts");
    if (!mp_) mp_ = js_read_int_field (ctx, mp, "maxParts");
    size_t mhs = js_read_size_field(ctx, mp, "max_headers_size");
    if (!mhs) mhs = js_read_size_field(ctx, mp, "maxHeadersSize");
    size_t mib = js_read_size_field(ctx, mp, "max_input_buffer");
    if (!mib) mib = js_read_size_field(ctx, mp, "maxInputBuffer");
    cfg->max_part_size    = mps;
    cfg->max_total_size   = mts;
    cfg->max_parts        = mp_;
    cfg->max_headers_size = mhs;
    cfg->max_input_buffer = mib;
    return cfg;
}

/* Streaming-multipart factory shim: routes the request through
 * hl_cap_multipart_factory (the parkable wrapper around Keel's
 * kl_body_reader_multipart) so the JS iterator can hl_cap_multipart_park
 * on NEED_DATA. The wrapper forwards our per-route config to the inner
 * Keel reader. */
static KlBodyReader *hl_js_multipart_factory(KlAllocator *alloc,
                                              const KlRequest *req,
                                              void *user_data)
{
    HlJSRoute *route = (HlJSRoute *)user_data;
    return hl_cap_multipart_factory(alloc, req, route->multipart_config);
}

int hl_js_wire_routes_server(HlJS *js, KlServer *server,
                              void *(*alloc_fn)(size_t))
{
    (void)alloc_fn; /* routes always use Hull allocator */
    js->server = server; /* store for async operations (hull.sleep, etc.) */
    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue defs = JS_GetPropertyStr(ctx, global, "__hull_route_defs");

    if (JS_IsUndefined(defs) || !JS_IsArray(ctx, defs)) {
        JS_FreeValue(ctx, defs);
        JS_FreeValue(ctx, global);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    JSValue len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t count = 0;
    JS_ToInt32(ctx, &count, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < count; i++) {
        JSValue def = JS_GetPropertyUint32(ctx, defs, (uint32_t)i);
        if (JS_IsUndefined(def))
            continue;

        JSValue method_val = JS_GetPropertyStr(ctx, def, "method");
        JSValue pattern_val = JS_GetPropertyStr(ctx, def, "pattern");
        JSValue id_val = JS_GetPropertyStr(ctx, def, "handler_id");
        JSValue mp_val = JS_GetPropertyStr(ctx, def, "multipart");

        const char *method_str = JS_ToCString(ctx, method_val);
        const char *pattern = JS_ToCString(ctx, pattern_val);
        int32_t handler_id = 0;
        JS_ToInt32(ctx, &handler_id, id_val);

        if (method_str && pattern) {
            HlJSRoute *route = hl_alloc_malloc(js->base.alloc,
                                                 sizeof(HlJSRoute));
            if (route) {
                route->js = js;
                route->handler_id = handler_id;
                route->multipart_config = NULL;

                int is_streaming = JS_IsObject(mp_val) &&
                                   !JS_IsFunction(ctx, mp_val) &&
                                   !JS_IsArray(ctx, mp_val);
                if (is_streaming) {
                    route->multipart_config =
                        js_build_multipart_config(js, mp_val);
                    if (!route->multipart_config) is_streaming = 0;
                }

                hl_js_track_route(js, route);
                if (is_streaming) {
                    /* streaming-async (v2.2.0+) — see Lua sibling. */
                    kl_server_route_streaming_async(server, method_str, pattern,
                                                     hl_js_keel_handler, route,
                                                     hl_js_multipart_factory);
                } else {
                    kl_server_route(server, method_str, pattern,
                                    hl_js_keel_handler, route,
                                    hl_cap_body_factory);
                }
            }
        }

        if (pattern) JS_FreeCString(ctx, pattern);
        if (method_str) JS_FreeCString(ctx, method_str);
        JS_FreeValue(ctx, mp_val);
        JS_FreeValue(ctx, id_val);
        JS_FreeValue(ctx, pattern_val);
        JS_FreeValue(ctx, method_val);
        JS_FreeValue(ctx, def);
    }

    JS_FreeValue(ctx, defs);

    /* Wire middleware from __hull_middleware */
    JSValue mw = JS_GetPropertyStr(ctx, global, "__hull_middleware");
    if (!JS_IsUndefined(mw) && JS_IsArray(ctx, mw)) {
        JSValue mw_len_val = JS_GetPropertyStr(ctx, mw, "length");
        int32_t mw_count = 0;
        JS_ToInt32(ctx, &mw_count, mw_len_val);
        JS_FreeValue(ctx, mw_len_val);

        for (int32_t i = 0; i < mw_count; i++) {
            JSValue entry = JS_GetPropertyUint32(ctx, mw, (uint32_t)i);
            if (JS_IsUndefined(entry))
                continue;

            JSValue method_val = JS_GetPropertyStr(ctx, entry, "method");
            JSValue pattern_val = JS_GetPropertyStr(ctx, entry, "pattern");
            JSValue id_val = JS_GetPropertyStr(ctx, entry, "handler_id");

            const char *method_str = JS_ToCString(ctx, method_val);
            const char *pattern = JS_ToCString(ctx, pattern_val);
            int32_t handler_id = 0;
            JS_ToInt32(ctx, &handler_id, id_val);

            if (method_str && pattern) {
                HlJSRoute *mw_ctx = hl_alloc_malloc(js->base.alloc,
                                                      sizeof(HlJSRoute));
                if (mw_ctx) {
                    mw_ctx->js = js;
                    mw_ctx->handler_id = handler_id;
                    hl_js_track_route(js, mw_ctx);
                    kl_server_use(server, method_str, pattern,
                                  hl_js_keel_middleware, mw_ctx);
                }
            }

            if (pattern) JS_FreeCString(ctx, pattern);
            if (method_str) JS_FreeCString(ctx, method_str);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, pattern_val);
            JS_FreeValue(ctx, method_val);
            JS_FreeValue(ctx, entry);
        }
    }
    JS_FreeValue(ctx, mw);

    /* Wire post-body middleware from __hull_post_middleware */
    JSValue post_mw = JS_GetPropertyStr(ctx, global, "__hull_post_middleware");
    if (!JS_IsUndefined(post_mw) && JS_IsArray(ctx, post_mw)) {
        JSValue post_mw_len_val = JS_GetPropertyStr(ctx, post_mw, "length");
        int32_t post_mw_count = 0;
        JS_ToInt32(ctx, &post_mw_count, post_mw_len_val);
        JS_FreeValue(ctx, post_mw_len_val);

        for (int32_t i = 0; i < post_mw_count; i++) {
            JSValue entry = JS_GetPropertyUint32(ctx, post_mw, (uint32_t)i);
            if (JS_IsUndefined(entry))
                continue;

            JSValue method_val = JS_GetPropertyStr(ctx, entry, "method");
            JSValue pattern_val = JS_GetPropertyStr(ctx, entry, "pattern");
            JSValue id_val = JS_GetPropertyStr(ctx, entry, "handler_id");

            const char *method_str = JS_ToCString(ctx, method_val);
            const char *pattern = JS_ToCString(ctx, pattern_val);
            int32_t handler_id = 0;
            JS_ToInt32(ctx, &handler_id, id_val);

            if (method_str && pattern) {
                HlJSRoute *mw_ctx = hl_alloc_malloc(js->base.alloc,
                                                      sizeof(HlJSRoute));
                if (mw_ctx) {
                    mw_ctx->js = js;
                    mw_ctx->handler_id = handler_id;
                    hl_js_track_route(js, mw_ctx);
                    kl_server_use_post(server, method_str, pattern,
                                       hl_js_keel_middleware, mw_ctx);
                }
            }

            if (pattern) JS_FreeCString(ctx, pattern);
            if (method_str) JS_FreeCString(ctx, method_str);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, pattern_val);
            JS_FreeValue(ctx, method_val);
            JS_FreeValue(ctx, entry);
        }
    }
    JS_FreeValue(ctx, post_mw);

    /* Wire timers from __hull_timer_defs */
    JSValue timer_defs = JS_GetPropertyStr(ctx, global, "__hull_timer_defs");
    if (!JS_IsUndefined(timer_defs) && JS_IsArray(ctx, timer_defs)) {
        JSValue td_len_val = JS_GetPropertyStr(ctx, timer_defs, "length");
        int32_t td_count = 0;
        JS_ToInt32(ctx, &td_count, td_len_val);
        JS_FreeValue(ctx, td_len_val);

        for (int32_t i = 0; i < td_count; i++) {
            JSValue def = JS_GetPropertyUint32(ctx, timer_defs, (uint32_t)i);
            if (JS_IsUndefined(def))
                continue;

            JSValue type_val = JS_GetPropertyStr(ctx, def, "type");
            JSValue id_val = JS_GetPropertyStr(ctx, def, "handler_id");

            const char *type_str = JS_ToCString(ctx, type_val);
            int32_t handler_id = 0;
            JS_ToInt32(ctx, &handler_id, id_val);

            if (!type_str) {
                JS_FreeValue(ctx, id_val);
                JS_FreeValue(ctx, type_val);
                JS_FreeValue(ctx, def);
                continue;
            }

            HlJSTimer *t = hl_alloc_malloc(js->base.alloc,
                                             sizeof(HlJSTimer));
            if (!t) {
                JS_FreeCString(ctx, type_str);
                JS_FreeValue(ctx, id_val);
                JS_FreeValue(ctx, type_val);
                JS_FreeValue(ctx, def);
                continue;
            }

            memset(t, 0, sizeof(*t));
            t->js = js;
            t->handler_id = handler_id;

            int64_t delay_ms;
            if (strcmp(type_str, "daily") == 0) {
                JSValue hour_val = JS_GetPropertyStr(ctx, def, "hour");
                JSValue min_val = JS_GetPropertyStr(ctx, def, "minute");
                JSValue lt_val = JS_GetPropertyStr(ctx, def, "localtime");
                int32_t th = 0, tm = 0;
                JS_ToInt32(ctx, &th, hour_val);
                JS_ToInt32(ctx, &tm, min_val);
                t->hour = th;
                t->minute = tm;
                t->localtime = JS_ToBool(ctx, lt_val);
                JS_FreeValue(ctx, hour_val);
                JS_FreeValue(ctx, min_val);
                JS_FreeValue(ctx, lt_val);
                t->daily = 1;
                delay_ms = hl_js_compute_daily_delay_ms(t->hour, t->minute,
                                                         t->localtime);
                t->interval_ms = 0;
            } else {
                JSValue iv_val = JS_GetPropertyStr(ctx, def, "interval_ms");
                int64_t iv = 0;
                JS_ToInt64(ctx, &iv, iv_val);
                JS_FreeValue(ctx, iv_val);
                t->interval_ms = iv;
                t->daily = 0;
                delay_ms = iv;
            }

            const HlAsyncBackend *be = hl_async_backend();
            t->timer_id = (int64_t)be->timer_add(js->base.async_ctx,
                                                  (uint64_t)delay_ms,
                                                  hl_js_timer_trampoline, t);
            if (t->timer_id == 0) {
                hl_alloc_free(js->base.alloc, t, sizeof(HlJSTimer));
            } else {
                hl_js_track_timer(js, t);
            }

            JS_FreeCString(ctx, type_str);
            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, type_val);
            JS_FreeValue(ctx, def);
        }
    }
    JS_FreeValue(ctx, timer_defs);

    /* ── Wire WebSocket endpoints from __hull_ws_defs ──────────────── */
    JSValue ws_defs = JS_GetPropertyStr(ctx, global, "__hull_ws_defs");
    if (!JS_IsUndefined(ws_defs) && JS_IsArray(ctx, ws_defs)) {
        /* Initialize registry if needed */
        if (!js->base.ws_registry) {
            js->base.ws_registry = hl_alloc_malloc(js->base.alloc,
                                                      sizeof(HlWsRegistry));
            if (js->base.ws_registry)
                hl_ws_registry_init(js->base.ws_registry, js->base.alloc);
        }

        JSValue ws_len_val = JS_GetPropertyStr(ctx, ws_defs, "length");
        int32_t ws_count = 0;
        JS_ToInt32(ctx, &ws_count, ws_len_val);
        JS_FreeValue(ctx, ws_len_val);

        for (int32_t i = 0; i < ws_count; i++) {
            JSValue wd = JS_GetPropertyUint32(ctx, ws_defs, (uint32_t)i);
            if (JS_IsUndefined(wd)) continue;

            JSValue path_val = JS_GetPropertyStr(ctx, wd, "path");
            JSValue oo_val = JS_GetPropertyStr(ctx, wd, "on_open_id");
            JSValue om_val = JS_GetPropertyStr(ctx, wd, "on_message_id");
            JSValue oc_val = JS_GetPropertyStr(ctx, wd, "on_close_id");

            const char *path = JS_ToCString(ctx, path_val);
            int32_t on_open_id = -1, on_message_id = -1, on_close_id = -1;
            if (!JS_IsUndefined(oo_val)) JS_ToInt32(ctx, &on_open_id, oo_val);
            if (!JS_IsUndefined(om_val)) JS_ToInt32(ctx, &on_message_id, om_val);
            if (!JS_IsUndefined(oc_val)) JS_ToInt32(ctx, &on_close_id, oc_val);

            if (path) {
                HlJSWsRoute *ws_route = hl_alloc_malloc(js->base.alloc,
                                                           sizeof(HlJSWsRoute));
                if (ws_route) {
                    ws_route->js = js;
                    ws_route->on_open_id = on_open_id;
                    ws_route->on_message_id = on_message_id;
                    ws_route->on_close_id = on_close_id;
                    int wn = snprintf(ws_route->path, sizeof(ws_route->path),
                                      "%s", path);
                    if (wn < 0 || (size_t)wn >= sizeof(ws_route->path)) {
                        log_warn("[hull:js] app.ws: path too long (max 255 chars): %s",
                                 path);
                        hl_alloc_free(js->base.alloc, ws_route,
                                      sizeof(HlJSWsRoute));
                        ws_route = NULL;
                    }
                }
                if (ws_route) {
                    if (hl_js_track_alloc(js, &js->ws_routes,
                            &js->ws_route_count,
                            &js->ws_route_cap, ws_route) != 0) {
                        hl_alloc_free(js->base.alloc, ws_route,
                                      sizeof(HlJSWsRoute));
                    } else {
                        KlWsServerConfig *ws_cfg =
                            hl_alloc_malloc(js->base.alloc,
                                            sizeof(KlWsServerConfig));
                        if (ws_cfg) {
                            kl_ws_server_config_init(ws_cfg);
                            ws_cfg->callbacks.on_open = hl_js_ws_on_open;
                            ws_cfg->callbacks.on_message = hl_js_ws_on_message;
                            ws_cfg->callbacks.on_close = hl_js_ws_on_close;
                            ws_cfg->user_data = ws_route;
                            hl_js_track_alloc(js, &js->ws_cfgs,
                                               &js->ws_cfg_count,
                                               &js->ws_cfg_cap, ws_cfg);
                            kl_server_ws(server, path, ws_cfg);
                        }
                    }
                }
                JS_FreeCString(ctx, path);
            }

            JS_FreeValue(ctx, oc_val);
            JS_FreeValue(ctx, om_val);
            JS_FreeValue(ctx, oo_val);
            JS_FreeValue(ctx, path_val);
            JS_FreeValue(ctx, wd);
        }
    }
    JS_FreeValue(ctx, ws_defs);

    /* ── Wire SSE endpoints from __hull_sse_defs ───────────────────── */
    JSValue sse_defs = JS_GetPropertyStr(ctx, global, "__hull_sse_defs");
    if (!JS_IsUndefined(sse_defs) && JS_IsArray(ctx, sse_defs)) {
        JSValue sse_len_val = JS_GetPropertyStr(ctx, sse_defs, "length");
        int32_t sse_count = 0;
        JS_ToInt32(ctx, &sse_count, sse_len_val);
        JS_FreeValue(ctx, sse_len_val);

        for (int32_t i = 0; i < sse_count; i++) {
            JSValue sd = JS_GetPropertyUint32(ctx, sse_defs, (uint32_t)i);
            if (JS_IsUndefined(sd)) continue;

            JSValue path_val = JS_GetPropertyStr(ctx, sd, "path");
            JSValue id_val = JS_GetPropertyStr(ctx, sd, "handler_id");

            const char *path = JS_ToCString(ctx, path_val);
            int32_t handler_id = 0;
            JS_ToInt32(ctx, &handler_id, id_val);

            if (path) {
                HlJSSseRoute *sse_route = hl_alloc_malloc(js->base.alloc,
                                                             sizeof(HlJSSseRoute));
                if (sse_route) {
                    sse_route->js = js;
                    sse_route->handler_id = handler_id;
                    if (hl_js_track_alloc(js, &js->sse_routes,
                            &js->sse_route_count,
                            &js->sse_route_cap, sse_route) != 0) {
                        hl_alloc_free(js->base.alloc, sse_route,
                                      sizeof(HlJSSseRoute));
                    } else {
                        kl_server_route(server, "GET", path,
                                        hl_js_sse_handler, sse_route, NULL);
                    }
                }
                JS_FreeCString(ctx, path);
            }

            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, path_val);
            JS_FreeValue(ctx, sd);
        }
    }
    JS_FreeValue(ctx, sse_defs);

    JS_FreeValue(ctx, global);
    return 0;
}
