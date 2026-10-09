/*
 * routes.c - JS route + middleware + timer + WS/SSE wiring
 *
 * Reads the route/middleware/timer/ws/sse definition arrays that
 * `app.<verb>()` builds on globalThis and registers them with Keel
 * (KlHttpRouter or KlHttpServer). Also hosts the tracked-allocation helpers
 * used by every wire step so we can free per-route contexts on
 * shutdown without leaking.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/utils/alloc.h"
#include "hull/shared/async_backend.h"
#include "hull/cap/body.h"
#include "hull/cap/ws.h"

#include <keel/keel.h>
#include <keel/http_body_reader_multipart.h>
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

/* Defined below; forward-declared so the router (test-harness) wiring can
 * register streaming-multipart routes the same way the server wiring does. */
static KlHttpMultipartConfig *js_build_multipart_config(HlJS *js, JSValueConst mp);
static KlHttpBodyReader *hl_js_multipart_factory(KlAllocator *alloc,
                                             const KlHttpRequest *req,
                                             void *user_data);

/*
 * Pointer lifetime (audit 11 H1). Keel's router stores the method and
 * pattern pointers it is handed WITHOUT copying them (kl_http_router_add,
 * kl_http_router_use / _use_post, kl_http_server_ws_upgrade). Those used to
 * be JS_ToCString results, freed right after registration: for a non-ASCII
 * pattern QuickJS allocates a fresh UTF-8 buffer, so the router matched
 * every later request against freed memory; for an ASCII one the pointer
 * aliased the JS string, kept alive only by the app-writable
 * __hull_route_defs / __hull_middleware / ... globals. Every string handed
 * to Keel is now a Hull-owned copy that lives as long as the route context
 * (freed in hl_js_free, after the router / server stops dispatching).
 *
 * Every registration failure (allocation, string conversion, tracking, a
 * Keel refusal) fails the wiring, so the app refuses to start: a dropped
 * middleware (auth, CSRF, CORS) must not fail open.
 */

static char *js_route_strdup(HlJS *js, const char *s)
{
    size_t n = strlen(s);
    char *p = hl_alloc_malloc(js->base.alloc, n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* Drop a pending exception a failed property read / conversion left. */
static void js_wire_clear_exception(JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);
    JS_FreeValue(ctx, exc);
}

/* Build a tracked HlJSRoute from one def {method, pattern, handler_id
 * [, multipart]}. On success the route is tracked (hl_js_free releases it)
 * and owns copies of method + pattern. NULL on any failure (logged). */
static HlJSRoute *js_route_from_def(HlJS *js, JSValueConst def,
                                    int want_multipart, const char *what)
{
    JSContext *ctx = js->ctx;
    HlJSRoute *route = NULL;
    JSValue m_val = JS_GetPropertyStr(ctx, def, "method");
    JSValue p_val = JS_GetPropertyStr(ctx, def, "pattern");
    JSValue id_val = JS_GetPropertyStr(ctx, def, "handler_id");
    JSValue mp_val = want_multipart ? JS_GetPropertyStr(ctx, def, "multipart")
                                    : JS_UNDEFINED;
    const char *m = JS_ToCString(ctx, m_val);
    const char *p = JS_ToCString(ctx, p_val);
    int32_t hid = 0;

    if (!m || !p || JS_ToInt32(ctx, &hid, id_val) != 0) {
        js_wire_clear_exception(ctx);
        log_error("[hull] could not read a %s definition; refusing to start", what);
        goto out;
    }

    route = hl_alloc_malloc(js->base.alloc, sizeof(HlJSRoute));
    if (!route) {
        log_error("[hull] out of memory registering %s %s %s; refusing to start",
                  what, m, p);
        goto out;
    }
    memset(route, 0, sizeof(*route));
    route->js = js;
    route->handler_id = hid;
    route->method = js_route_strdup(js, m);
    route->pattern = js_route_strdup(js, p);
    int ok = route->method && route->pattern;

    /* def.multipart present → streaming route (both the server wiring and
     * the in-process test router; the harness pre-feeds the whole body to
     * the factory's wrapper, hl_cap_test_dispatch). */
    if (ok && JS_IsObject(mp_val) && !JS_IsFunction(ctx, mp_val) &&
        !JS_IsArray(ctx, mp_val)) {
        route->multipart_config = js_build_multipart_config(js, mp_val);
        if (!route->multipart_config) ok = 0;
    }
    if (ok && hl_js_track_route(js, route) != 0) ok = 0;
    if (!ok) {
        log_error("[hull] out of memory registering %s %s %s; refusing to start",
                  what, m, p);
        hl_js_route_destroy(js, route);
        route = NULL;
    }

out:
    if (p) JS_FreeCString(ctx, p);
    if (m) JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, mp_val);
    JS_FreeValue(ctx, id_val);
    JS_FreeValue(ctx, p_val);
    JS_FreeValue(ctx, m_val);
    return route;
}

typedef enum { JS_WIRE_ROUTE, JS_WIRE_PRE_MW, JS_WIRE_POST_MW } JsWireKind;

/* Register one route / middleware with the router (test harness) or the
 * server. Exactly one of router / server is non-NULL. Returns Keel's rc. */
static int js_wire_register(KlHttpRouter *router, KlHttpServer *server,
                            JsWireKind kind, HlJSRoute *r)
{
    switch (kind) {
    case JS_WIRE_ROUTE:
        if (r->multipart_config)
            return router
                ? kl_http_router_add_streaming_async(router, r->method, r->pattern,
                                                     hl_js_keel_handler, r,
                                                     hl_js_multipart_factory)
                /* streaming-async (v2.2.0+) - see Lua sibling. */
                : kl_http_server_route_streaming_async(server, r->method, r->pattern,
                                                       hl_js_keel_handler, r,
                                                       hl_js_multipart_factory);
        return router
            ? kl_http_router_add(router, r->method, r->pattern,
                                 hl_js_keel_handler, r, NULL)
            : kl_http_server_route(server, r->method, r->pattern,
                                   hl_js_keel_handler, r, hl_cap_body_factory);
    case JS_WIRE_PRE_MW:
        return router
            ? kl_http_router_use(router, r->method, r->pattern,
                                 hl_js_keel_middleware, r)
            : kl_http_server_use(server, r->method, r->pattern,
                                 hl_js_keel_middleware, r);
    case JS_WIRE_POST_MW:
        return router
            ? kl_http_router_use_post(router, r->method, r->pattern,
                                      hl_js_keel_middleware, r)
            : kl_http_server_use_post(server, r->method, r->pattern,
                                      hl_js_keel_middleware, r);
    }
    return -1;
}

/* Wire every def in globalThis[key]. A missing / non-array key is "nothing
 * to wire" (0) unless `required`. Returns -1 when anything failed. */
static int js_wire_defs(HlJS *js, JSValueConst global, const char *key,
                        JsWireKind kind, int required,
                        KlHttpRouter *router, KlHttpServer *server)
{
    JSContext *ctx = js->ctx;
    static const char *const what_names[] = {
        [JS_WIRE_ROUTE] = "route",
        [JS_WIRE_PRE_MW] = "middleware",
        [JS_WIRE_POST_MW] = "post-body middleware",
    };
    const char *what = what_names[kind];
    JSValue defs = JS_GetPropertyStr(ctx, global, key);
    if (JS_IsException(defs)) {
        js_wire_clear_exception(ctx);
        log_error("[hull] could not read %s; refusing to start", key);
        return -1;
    }
    if (!JS_IsArray(ctx, defs)) {
        JS_FreeValue(ctx, defs);
        if (required) {
            log_error("[hull:c] no routes registered");
            return -1;
        }
        return 0;
    }

    int rc = 0;
    JSValue len_val = JS_GetPropertyStr(ctx, defs, "length");
    int32_t count = 0;
    if (JS_ToInt32(ctx, &count, len_val) != 0) {
        js_wire_clear_exception(ctx);
        rc = -1;
    }
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; rc == 0 && i < count; i++) {
        JSValue def = JS_GetPropertyUint32(ctx, defs, (uint32_t)i);
        if (JS_IsException(def)) {
            js_wire_clear_exception(ctx);
            log_error("[hull] could not read a %s definition; refusing to start", what);
            rc = -1;
            break;
        }
        if (JS_IsUndefined(def))
            continue;

        HlJSRoute *r = js_route_from_def(js, def, kind == JS_WIRE_ROUTE, what);
        JS_FreeValue(ctx, def);
        if (!r) {
            rc = -1;
            break;
        }
        /* r is tracked: hl_js_free releases it even if Keel refuses it. */
        if (js_wire_register(router, server, kind, r) != 0) {
            if (kind == JS_WIRE_POST_MW) {
                /* Keel 3.3.0 caps post-body middleware per router;
                 * a dropped one (CSRF, auth) must not fail open. */
                log_error("[hull] could not register post-body middleware %s %s (Keel allows "
                          "at most %d per router); refusing to start",
                          r->method, r->pattern, KL_HTTP_ROUTER_MAX_POST_MIDDLEWARE);
            } else {
                log_error("[hull] could not register %s %s %s; refusing to start",
                          what, r->method, r->pattern);
            }
            rc = -1;
        }
    }
    JS_FreeValue(ctx, defs);
    return rc;
}

int hl_js_wire_routes(HlJS *js, KlHttpRouter *router)
{
    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    int rc = js_wire_defs(js, global, "__hull_route_defs", JS_WIRE_ROUTE, 1,
                          router, NULL);
    if (rc == 0)
        rc = js_wire_defs(js, global, "__hull_middleware", JS_WIRE_PRE_MW, 0,
                          router, NULL);
    if (rc == 0)
        rc = js_wire_defs(js, global, "__hull_post_middleware", JS_WIRE_POST_MW, 0,
                          router, NULL);
    JS_FreeValue(ctx, global);
    return rc;
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

/* Allocate KlHttpMultipartConfig from JS subobject; caller frees with
 * hl_alloc_free(...,sizeof(KlHttpMultipartConfig)). */
static KlHttpMultipartConfig *js_build_multipart_config(HlJS *js, JSValueConst mp)
{
    KlHttpMultipartConfig *cfg = hl_alloc_malloc(js->base.alloc,
                                              sizeof(KlHttpMultipartConfig));
    if (!cfg) return NULL;
    JSContext *ctx = js->ctx;
    /* Accept both snake_case and camelCase for cross-runtime ergonomics -
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
 * kl_http_body_reader_multipart) so the JS iterator can hl_cap_multipart_park
 * on NEED_DATA. The wrapper forwards our per-route config to the inner
 * Keel reader. */
static KlHttpBodyReader *hl_js_multipart_factory(KlAllocator *alloc,
                                              const KlHttpRequest *req,
                                              void *user_data)
{
    HlJSRoute *route = (HlJSRoute *)user_data;
    return hl_cap_multipart_factory(alloc, req, route->multipart_config);
}

/* Cosmo HTTP-bridge force-link anchor (0.13.1 PR#1) - see hull/http_feature.h.
 * Unique strong symbol, co-resident with the strong hl_js_wire_routes_server;
 * a produced cosmo app references it to force-pull this member over the weak
 * http_weakstub.o stub. */
int hl_js_http_bridge_anchor = 0;

int hl_js_wire_routes_server(HlJS *js, KlHttpServer *server,
                              void *(*alloc_fn)(size_t))
{
    (void)alloc_fn; /* routes always use Hull allocator */
    js->server = server; /* store for async operations (hull.sleep, etc.) */
    JSContext *ctx = js->ctx;
    JSValue global = JS_GetGlobalObject(ctx);

    /* Routes, then pre-body and post-body middleware: any failure refuses
     * the wiring (see "Pointer lifetime" above). */
    int rc = js_wire_defs(js, global, "__hull_route_defs", JS_WIRE_ROUTE, 1,
                          NULL, server);
    if (rc == 0)
        rc = js_wire_defs(js, global, "__hull_middleware", JS_WIRE_PRE_MW, 0,
                          NULL, server);
    if (rc == 0)
        rc = js_wire_defs(js, global, "__hull_post_middleware", JS_WIRE_POST_MW, 0,
                          NULL, server);
    if (rc != 0) {
        JS_FreeValue(ctx, global);
        return -1;
    }

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
    /* Keel stores the upgrade pattern pointer: it is ws_route->path, a
     * Hull-owned copy, never the JS string (audit 11 H1). */
    rc = 0;
    JSValue ws_defs = JS_GetPropertyStr(ctx, global, "__hull_ws_defs");
    if (JS_IsException(ws_defs)) {
        js_wire_clear_exception(ctx);
        rc = -1;
    } else if (JS_IsArray(ctx, ws_defs)) {
        /* Initialize registry if needed */
        if (!js->base.ws_registry) {
            js->base.ws_registry = hl_alloc_malloc(js->base.alloc,
                                                      sizeof(HlWsRegistry));
            if (js->base.ws_registry)
                hl_ws_registry_init(js->base.ws_registry, js->base.alloc);
            else
                rc = -1;
        }

        JSValue ws_len_val = JS_GetPropertyStr(ctx, ws_defs, "length");
        int32_t ws_count = 0;
        if (JS_ToInt32(ctx, &ws_count, ws_len_val) != 0) {
            js_wire_clear_exception(ctx);
            rc = -1;
        }
        JS_FreeValue(ctx, ws_len_val);

        for (int32_t i = 0; rc == 0 && i < ws_count; i++) {
            JSValue wd = JS_GetPropertyUint32(ctx, ws_defs, (uint32_t)i);
            if (JS_IsException(wd)) {
                js_wire_clear_exception(ctx);
                rc = -1;
                break;
            }
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

            HlJSWsRoute *ws_route = NULL;
            if (!path) {
                js_wire_clear_exception(ctx);
                log_error("[hull] could not read an app.ws definition; refusing to start");
                rc = -1;
            } else {
                ws_route = hl_alloc_malloc(js->base.alloc, sizeof(HlJSWsRoute));
                if (!ws_route) {
                    rc = -1;
                } else {
                    ws_route->js = js;
                    ws_route->on_open_id = on_open_id;
                    ws_route->on_message_id = on_message_id;
                    ws_route->on_close_id = on_close_id;
                    int wn = snprintf(ws_route->path, sizeof(ws_route->path),
                                      "%s", path);
                    if (wn < 0 || (size_t)wn >= sizeof(ws_route->path)) {
                        log_error("[hull:js] app.ws: path too long (max 255 chars): %s",
                                  path);
                        hl_alloc_free(js->base.alloc, ws_route, sizeof(HlJSWsRoute));
                        rc = -1;
                    } else if (hl_js_track_alloc(js, &js->ws_routes,
                                   &js->ws_route_count,
                                   &js->ws_route_cap, ws_route) != 0) {
                        hl_alloc_free(js->base.alloc, ws_route, sizeof(HlJSWsRoute));
                        rc = -1;
                    } else {
                        KlWsServerConfig *ws_cfg =
                            hl_alloc_malloc(js->base.alloc, sizeof(KlWsServerConfig));
                        if (!ws_cfg) {
                            rc = -1;
                        } else if (hl_js_track_alloc(js, &js->ws_cfgs,
                                       &js->ws_cfg_count,
                                       &js->ws_cfg_cap, ws_cfg) != 0) {
                            hl_alloc_free(js->base.alloc, ws_cfg,
                                          sizeof(KlWsServerConfig));
                            rc = -1;
                        } else {
                            kl_ws_server_config_init(ws_cfg);
                            ws_cfg->callbacks.on_open = hl_js_ws_on_open;
                            ws_cfg->callbacks.on_message = hl_js_ws_on_message;
                            ws_cfg->callbacks.on_close = hl_js_ws_on_close;
                            ws_cfg->user_data = ws_route;
                            if (kl_http_server_ws_upgrade(server, ws_route->path,
                                                          ws_cfg) != 0)
                                rc = -1;
                        }
                    }
                }
                if (rc != 0)
                    log_error("[hull] could not register app.ws %s; refusing to start",
                              path);
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
    if (rc != 0) {
        JS_FreeValue(ctx, global);
        return -1;
    }

    /* ── Wire SSE endpoints from __hull_sse_defs ───────────────────── */
    /* As for WebSocket: Keel gets sse_route->path, a Hull-owned copy. */
    JSValue sse_defs = JS_GetPropertyStr(ctx, global, "__hull_sse_defs");
    if (JS_IsException(sse_defs)) {
        js_wire_clear_exception(ctx);
        rc = -1;
    } else if (JS_IsArray(ctx, sse_defs)) {
        JSValue sse_len_val = JS_GetPropertyStr(ctx, sse_defs, "length");
        int32_t sse_count = 0;
        if (JS_ToInt32(ctx, &sse_count, sse_len_val) != 0) {
            js_wire_clear_exception(ctx);
            rc = -1;
        }
        JS_FreeValue(ctx, sse_len_val);

        for (int32_t i = 0; rc == 0 && i < sse_count; i++) {
            JSValue sd = JS_GetPropertyUint32(ctx, sse_defs, (uint32_t)i);
            if (JS_IsException(sd)) {
                js_wire_clear_exception(ctx);
                rc = -1;
                break;
            }
            if (JS_IsUndefined(sd)) continue;

            JSValue path_val = JS_GetPropertyStr(ctx, sd, "path");
            JSValue id_val = JS_GetPropertyStr(ctx, sd, "handler_id");

            const char *path = JS_ToCString(ctx, path_val);
            int32_t handler_id = 0;
            JS_ToInt32(ctx, &handler_id, id_val);

            if (!path) {
                js_wire_clear_exception(ctx);
                log_error("[hull] could not read an app.sse definition; refusing to start");
                rc = -1;
            } else {
                HlJSSseRoute *sse_route = hl_alloc_malloc(js->base.alloc,
                                                          sizeof(HlJSSseRoute));
                if (!sse_route) {
                    rc = -1;
                } else {
                    sse_route->js = js;
                    sse_route->handler_id = handler_id;
                    int sn = snprintf(sse_route->path, sizeof(sse_route->path),
                                      "%s", path);
                    if (sn < 0 || (size_t)sn >= sizeof(sse_route->path)) {
                        log_error("[hull:js] app.sse: path too long (max 255 chars): %s",
                                  path);
                        hl_alloc_free(js->base.alloc, sse_route, sizeof(HlJSSseRoute));
                        rc = -1;
                    } else if (hl_js_track_alloc(js, &js->sse_routes,
                                   &js->sse_route_count,
                                   &js->sse_route_cap, sse_route) != 0) {
                        hl_alloc_free(js->base.alloc, sse_route, sizeof(HlJSSseRoute));
                        rc = -1;
                    } else if (kl_http_server_route(server, "GET", sse_route->path,
                                                    hl_js_sse_handler, sse_route,
                                                    NULL) != 0) {
                        rc = -1;
                    }
                }
                if (rc != 0)
                    log_error("[hull] could not register app.sse %s; refusing to start",
                              path);
                JS_FreeCString(ctx, path);
            }

            JS_FreeValue(ctx, id_val);
            JS_FreeValue(ctx, path_val);
            JS_FreeValue(ctx, sd);
        }
    }
    JS_FreeValue(ctx, sse_defs);

    JS_FreeValue(ctx, global);
    return rc;
}
