/*
 * routes.c - Lua route + middleware + timer + WS/SSE wiring
 *
 * Reads the route/middleware/timer/ws/sse definition tables that
 * `app.<verb>()` builds in the Lua registry and registers them with
 * Keel (KlHttpRouter or KlHttpServer). Also hosts the tracked-allocation
 * helpers used by every wire step so we can free per-route contexts
 * on shutdown without leaking.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/utils/alloc.h"
#include "hull/shared/async_backend.h"
#include "hull/cap/body.h"
#include "hull/cap/ws.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>
#include <keel/http_body_reader_multipart.h>
#include <keel/websocket_server.h>

#include "log.h"

#include <limits.h>
#include <string.h>
#include <stdio.h>

/* ── Route tracking ────────────────────────────────────────────────── */

int hl_lua_track_route(HlLua *lua, void *route)
{
    if (lua->route_count >= lua->route_cap) {
        size_t new_cap = lua->route_cap ? lua->route_cap * 2 : 8;
        if (new_cap < lua->route_cap || new_cap > SIZE_MAX / sizeof(void *))
            return -1; /* overflow */
        size_t old_sz = lua->route_cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(lua->base.alloc,
                                           lua->routes, old_sz, new_sz);
        if (!new_arr)
            return -1;
        lua->routes = new_arr;
        lua->route_cap = new_cap;
    }
    lua->routes[lua->route_count++] = route;
    return 0;
}

/* ── Generic tracked-allocation helper ──────────────────────────────── */

int hl_lua_track_alloc(HlLua *lua, void ***arr, size_t *count,
                                size_t *cap, void *ptr)
{
    if (*count >= *cap) {
        size_t new_cap = *cap ? *cap * 2 : 4;
        if (new_cap < *cap || new_cap > SIZE_MAX / sizeof(void *))
            return -1;
        size_t old_sz = *cap * sizeof(void *);
        size_t new_sz = new_cap * sizeof(void *);
        void **new_arr = hl_alloc_realloc(lua->base.alloc,
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
static KlHttpMultipartConfig *lua_build_multipart_config(HlLua *lua);
static KlHttpBodyReader *hl_lua_multipart_factory(KlAllocator *alloc,
                                              const KlHttpRequest *req,
                                              void *user_data);

/*
 * Pointer lifetime (audit 11 H1, mirrors runtime/js/routes.c). Keel's router
 * stores the method and pattern pointers it is handed without copying them.
 * The Lua strings were kept alive only by the registry's def tables; every
 * string handed to Keel is now a Hull-owned copy that lives as long as the
 * route context (freed in hl_lua_free), so nothing about Keel's lifetime
 * depends on what the Lua VM still references.
 *
 * Every registration failure (allocation, tracking, a Keel refusal) fails the
 * wiring, so the app refuses to start: a dropped middleware (auth, CSRF)
 * must not fail open.
 */

static char *lua_route_strdup(HlLua *lua, const char *s)
{
    size_t n = strlen(s);
    char *p = hl_alloc_malloc(lua->base.alloc, n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* Build a tracked HlLuaRoute from the def table at the top of the stack
 * ({method, pattern, handler_id [, multipart]}). Stack-neutral. On success
 * the route is tracked (hl_lua_free releases it) and owns copies of method +
 * pattern. NULL on any failure (logged). */
static HlLuaRoute *lua_route_from_def(HlLua *lua, int want_multipart,
                                      const char *what)
{
    lua_State *L = lua->L;
    lua_getfield(L, -1, "method");
    lua_getfield(L, -2, "pattern");
    lua_getfield(L, -3, "handler_id");
    const char *m = lua_tostring(L, -3);
    const char *p = lua_tostring(L, -2);
    int handler_id = (int)lua_tointeger(L, -1);

    if (!m || !p) {
        log_error("[hull] could not read a %s definition; refusing to start", what);
        lua_pop(L, 3);
        return NULL;
    }

    HlLuaRoute *route = hl_alloc_malloc(lua->base.alloc, sizeof(HlLuaRoute));
    int ok = route != NULL;
    if (route) {
        memset(route, 0, sizeof(*route));
        route->lua = lua;
        route->handler_id = handler_id;
        route->method = lua_route_strdup(lua, m);
        route->pattern = lua_route_strdup(lua, p);
        ok = route->method && route->pattern;

        /* def.multipart present → streaming route (both the server wiring
         * and the in-process test router; the harness pre-feeds the whole
         * body to the factory's wrapper, hl_cap_test_dispatch). */
        if (ok && want_multipart) {
            lua_pushliteral(L, "multipart");   /* raw: the app's opts table */
            lua_rawget(L, -5);
            if (lua_istable(L, -1)) {
                route->multipart_config = lua_build_multipart_config(lua);
                if (!route->multipart_config) ok = 0;
            }
            lua_pop(L, 1); /* multipart subtable */
        }
        if (ok && hl_lua_track_route(lua, route) != 0) ok = 0;
    }
    if (!ok) {
        log_error("[hull] out of memory registering %s %s %s; refusing to start",
                  what, m, p);
        hl_lua_route_destroy(lua, route);
        route = NULL;
    }
    lua_pop(L, 3); /* method, pattern, handler_id */
    return route;
}

typedef enum { LUA_WIRE_ROUTE, LUA_WIRE_PRE_MW, LUA_WIRE_POST_MW } LuaWireKind;

/* Register one route / middleware with the router (test harness) or the
 * server. Exactly one of router / server is non-NULL. Returns Keel's rc. */
static int lua_wire_register(KlHttpRouter *router, KlHttpServer *server,
                             LuaWireKind kind, HlLuaRoute *r)
{
    switch (kind) {
    case LUA_WIRE_ROUTE:
        if (r->multipart_config) {
            /* streaming-async (v2.2.0+) - handler is invoked BEFORE leftover
             * body bytes are fed via on_data, so parser caps fire structured
             * 4xx responses even when the body fits in the first read. */
            return router
                ? kl_http_router_add_streaming_async(router, r->method, r->pattern,
                                                     hl_lua_keel_handler, r,
                                                     hl_lua_multipart_factory)
                : kl_http_server_route_streaming_async(server, r->method, r->pattern,
                                                       hl_lua_keel_handler, r,
                                                       hl_lua_multipart_factory);
        }
        return router
            ? kl_http_router_add(router, r->method, r->pattern,
                                 hl_lua_keel_handler, r, NULL)
            : kl_http_server_route(server, r->method, r->pattern,
                                   hl_lua_keel_handler, r, hl_cap_body_factory);
    case LUA_WIRE_PRE_MW:
        return router
            ? kl_http_router_use(router, r->method, r->pattern,
                                 hl_lua_keel_middleware, r)
            : kl_http_server_use(server, r->method, r->pattern,
                                 hl_lua_keel_middleware, r);
    case LUA_WIRE_POST_MW:
        return router
            ? kl_http_router_use_post(router, r->method, r->pattern,
                                      hl_lua_keel_middleware, r)
            : kl_http_server_use_post(server, r->method, r->pattern,
                                      hl_lua_keel_middleware, r);
    }
    return -1;
}

/* Wire every def in registry[key]. A missing key / empty table is "nothing
 * to wire" (0) unless `required`. Stack-neutral. Returns -1 when anything
 * failed. */
static int lua_wire_defs(HlLua *lua, const char *key, LuaWireKind kind,
                         int required, KlHttpRouter *router, KlHttpServer *server)
{
    lua_State *L = lua->L;
    static const char *const what_names[] = {
        [LUA_WIRE_ROUTE] = "route",
        [LUA_WIRE_PRE_MW] = "middleware",
        [LUA_WIRE_POST_MW] = "post-body middleware",
    };
    const char *what = what_names[kind];

    lua_getfield(L, LUA_REGISTRYINDEX, key);
    int count = lua_istable(L, -1) ? (int)luaL_len(L, -1) : 0;
    if (count <= 0) {
        lua_pop(L, 1);
        if (required) {
            log_error("[hull:c] no routes registered");
            return -1;
        }
        return 0;
    }

    int rc = 0;
    for (int i = 1; rc == 0 && i <= count; i++) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        HlLuaRoute *r = lua_route_from_def(lua, kind == LUA_WIRE_ROUTE, what);
        lua_pop(L, 1); /* def table */
        if (!r) {
            rc = -1;
            break;
        }
        /* r is tracked: hl_lua_free releases it even if Keel refuses it. */
        if (lua_wire_register(router, server, kind, r) != 0) {
            if (kind == LUA_WIRE_POST_MW) {
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
    lua_pop(L, 1); /* defs table */
    return rc;
}

int hl_lua_wire_routes(HlLua *lua, KlHttpRouter *router)
{
    int rc = lua_wire_defs(lua, "__hull_route_defs", LUA_WIRE_ROUTE, 1,
                           router, NULL);
    if (rc == 0)
        rc = lua_wire_defs(lua, "__hull_middleware", LUA_WIRE_PRE_MW, 0,
                           router, NULL);
    if (rc == 0)
        rc = lua_wire_defs(lua, "__hull_post_middleware", LUA_WIRE_POST_MW, 0,
                           router, NULL);
    return rc;
}

/* ── Server route wiring (with body reader factory) ────────────────── */

/* Read a non-negative integer field from a Lua table at stack idx -1,
 * default to 0 if missing or non-numeric. Caps are size_t in Keel's
 * config; we round-trip through lua_Integer to reject negatives. */
static size_t lua_read_size_field(lua_State *L, const char *key)
{
    /* Raw: this table is the app's own opts.multipart, read at wire time
     * outside any pcall - an __index there ran app code at boot, and one
     * that raised aborted the process. */
    lua_pushstring(L, key);
    lua_rawget(L, -2);
    size_t v = 0;
    if (lua_isnumber(L, -1)) {
        lua_Integer i = lua_tointeger(L, -1);
        if (i > 0) v = (size_t)i;
    }
    lua_pop(L, 1);
    return v;
}

/* Same as lua_read_size_field but returns int (for max_parts). */
static int lua_read_int_field(lua_State *L, const char *key)
{
    lua_pushstring(L, key);   /* raw, as lua_read_size_field */
    lua_rawget(L, -2);
    int v = 0;
    if (lua_isnumber(L, -1)) {
        lua_Integer i = lua_tointeger(L, -1);
        if (i > 0 && i <= INT_MAX) v = (int)i;
    }
    lua_pop(L, 1);
    return v;
}

/* Build a heap-allocated KlHttpMultipartConfig from a Lua subtable at -1.
 * Caller frees with hl_alloc_free(...,sizeof(KlHttpMultipartConfig)).
 * Returns NULL on allocation failure. */
static KlHttpMultipartConfig *lua_build_multipart_config(HlLua *lua)
{
    KlHttpMultipartConfig *cfg = hl_alloc_malloc(lua->base.alloc,
                                              sizeof(KlHttpMultipartConfig));
    if (!cfg) return NULL;
    cfg->max_part_size    = lua_read_size_field(lua->L, "max_part_size");
    cfg->max_total_size   = lua_read_size_field(lua->L, "max_total_size");
    cfg->max_parts        = lua_read_int_field (lua->L, "max_parts");
    cfg->max_headers_size = lua_read_size_field(lua->L, "max_headers_size");
    cfg->max_input_buffer = lua_read_size_field(lua->L, "max_input_buffer");
    return cfg;
}

/* Body factory shim for streaming-multipart routes: routes the request
 * through hl_cap_multipart_factory (the parkable wrapper around Keel's
 * kl_http_body_reader_multipart) so the Lua iterator can hl_cap_multipart_park
 * on NEED_DATA. The wrapper forwards our per-route config to the inner
 * Keel reader. */
static KlHttpBodyReader *hl_lua_multipart_factory(KlAllocator *alloc,
                                               const KlHttpRequest *req,
                                               void *user_data)
{
    HlLuaRoute *route = (HlLuaRoute *)user_data;
    return hl_cap_multipart_factory(alloc, req, route->multipart_config);
}

/* Cosmo HTTP-bridge force-link anchor (0.13.1 PR#1) - see hull/http_feature.h.
 * A unique strong symbol with no weak twin, co-resident in this object with the
 * strong hl_lua_wire_routes_server below; a produced cosmo app references it to
 * force-pull this member over the weak http_weakstub.o stub. */
int hl_lua_http_bridge_anchor = 0;

int hl_lua_wire_routes_server(HlLua *lua, KlHttpServer *server,
                               void *(*alloc_fn)(size_t))
{
    (void)alloc_fn; /* routes always use Hull allocator */
    lua_State *L = lua->L;

    /* Store server for async operations (hull.sleep, http.get, etc.) */
    lua->server = server;

    /* Routes, then pre-body and post-body middleware: any failure refuses
     * the wiring (see "Pointer lifetime" above). */
    int rc = lua_wire_defs(lua, "__hull_route_defs", LUA_WIRE_ROUTE, 1,
                           NULL, server);
    if (rc == 0)
        rc = lua_wire_defs(lua, "__hull_middleware", LUA_WIRE_PRE_MW, 0,
                           NULL, server);
    if (rc == 0)
        rc = lua_wire_defs(lua, "__hull_post_middleware", LUA_WIRE_POST_MW, 0,
                           NULL, server);
    if (rc != 0)
        return -1;

    /* Wire timers from __hull_timer_defs */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_timer_defs");
    if (lua_istable(L, -1)) {
        int timer_count = (int)luaL_len(L, -1);
        for (int i = 1; i <= timer_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }

            lua_getfield(L, -1, "type");
            lua_getfield(L, -2, "handler_id");

            const char *type_str = lua_tostring(L, -2);
            int handler_id = (int)lua_tointeger(L, -1);
            lua_pop(L, 2); /* type, handler_id */

            if (!type_str) {
                lua_pop(L, 1);
                continue;
            }

            HlLuaTimer *t = hl_alloc_malloc(lua->base.alloc,
                                              sizeof(HlLuaTimer));
            if (!t) {
                lua_pop(L, 1);
                continue;
            }

            memset(t, 0, sizeof(*t));
            t->lua = lua;
            t->handler_id = handler_id;

            int64_t delay_ms;
            if (strcmp(type_str, "daily") == 0) {
                lua_getfield(L, -1, "hour");
                lua_getfield(L, -2, "minute");
                lua_getfield(L, -3, "localtime");
                t->hour = (int)lua_tointeger(L, -3);
                t->minute = (int)lua_tointeger(L, -2);
                t->localtime = lua_toboolean(L, -1);
                t->daily = 1;
                lua_pop(L, 3);

                delay_ms = hl_compute_daily_delay_ms(t->hour, t->minute,
                                                      t->localtime);
                t->interval_ms = 0; /* recomputed each time */
            } else {
                lua_getfield(L, -1, "interval_ms");
                t->interval_ms = (int64_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
                t->daily = 0;
                delay_ms = t->interval_ms;
            }

            {
                const HlAsyncBackend *be = hl_async_backend();
                t->timer_id = (int64_t)be->timer_add(lua->base.async_ctx,
                                                      (uint64_t)delay_ms,
                                                      hl_lua_timer_trampoline, t);
            }
            if (t->timer_id == 0) {
                hl_alloc_free(lua->base.alloc, t, sizeof(HlLuaTimer));
                lua_pop(L, 1);
                continue;
            }

            hl_lua_track_timer(lua, t);
            lua_pop(L, 1); /* timer def table */
        }
    }
    lua_pop(L, 1); /* __hull_timer_defs table */

    /* ── Wire WebSocket endpoints from __hull_ws_defs ──────────────── */
    /* Keel stores the upgrade pattern pointer: it is ws_route->path, a
     * Hull-owned copy, never the Lua string (audit 11 H1). */
    rc = 0;
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_ws_defs");
    if (lua_istable(L, -1)) {
        /* Initialize registry if needed */
        if (!lua->base.ws_registry) {
            lua->base.ws_registry = hl_alloc_malloc(lua->base.alloc,
                                                      sizeof(HlWsRegistry));
            if (lua->base.ws_registry)
                hl_ws_registry_init(lua->base.ws_registry, lua->base.alloc);
            else
                rc = -1;
        }

        int ws_count = (int)luaL_len(L, -1);
        for (int i = 1; rc == 0 && i <= ws_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }

            lua_getfield(L, -1, "on_open_id");
            int on_open_id = lua_isinteger(L, -1)
                                 ? (int)lua_tointeger(L, -1) : -1;
            lua_pop(L, 1);

            lua_getfield(L, -1, "on_message_id");
            int on_message_id = lua_isinteger(L, -1)
                                    ? (int)lua_tointeger(L, -1) : -1;
            lua_pop(L, 1);

            lua_getfield(L, -1, "on_close_id");
            int on_close_id = lua_isinteger(L, -1)
                                  ? (int)lua_tointeger(L, -1) : -1;
            lua_pop(L, 1);

            /* path stays on the stack (anchored) while it is copied */
            lua_getfield(L, -1, "path");
            const char *path = lua_tostring(L, -1);

            if (!path) {
                log_error("[hull] could not read an app.ws definition; refusing to start");
                rc = -1;
            } else {
                HlLuaWsRoute *ws_route = hl_alloc_malloc(lua->base.alloc,
                                                         sizeof(HlLuaWsRoute));
                if (!ws_route) {
                    rc = -1;
                } else {
                    ws_route->lua = lua;
                    ws_route->on_open_id = on_open_id;
                    ws_route->on_message_id = on_message_id;
                    ws_route->on_close_id = on_close_id;
                    int wn = snprintf(ws_route->path, sizeof(ws_route->path),
                                      "%s", path);
                    if (wn < 0 || (size_t)wn >= sizeof(ws_route->path)) {
                        /* Was luaL_error - outside any pcall, a panic. */
                        log_error("[hull:lua] app.ws: path too long (max 255 chars): %s",
                                  path);
                        hl_alloc_free(lua->base.alloc, ws_route, sizeof(HlLuaWsRoute));
                        rc = -1;
                    } else if (hl_lua_track_alloc(lua, &lua->ws_routes,
                                   &lua->ws_route_count,
                                   &lua->ws_route_cap, ws_route) != 0) {
                        hl_alloc_free(lua->base.alloc, ws_route, sizeof(HlLuaWsRoute));
                        rc = -1;
                    } else {
                        KlWsServerConfig *ws_cfg =
                            hl_alloc_malloc(lua->base.alloc, sizeof(KlWsServerConfig));
                        if (!ws_cfg) {
                            rc = -1;
                        } else if (hl_lua_track_alloc(lua, &lua->ws_cfgs,
                                       &lua->ws_cfg_count,
                                       &lua->ws_cfg_cap, ws_cfg) != 0) {
                            hl_alloc_free(lua->base.alloc, ws_cfg,
                                          sizeof(KlWsServerConfig));
                            rc = -1;
                        } else {
                            kl_ws_server_config_init(ws_cfg);
                            ws_cfg->callbacks.on_open = hl_lua_ws_on_open;
                            ws_cfg->callbacks.on_message = hl_lua_ws_on_message;
                            ws_cfg->callbacks.on_close = hl_lua_ws_on_close;
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
            }

            lua_pop(L, 2); /* path, ws def entry */
        }
    }
    lua_pop(L, 1); /* pop __hull_ws_defs */
    if (rc != 0)
        return -1;

    /* ── Wire SSE endpoints from __hull_sse_defs ───────────────────── */
    /* As for WebSocket: Keel gets sse_route->path, a Hull-owned copy. */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_sse_defs");
    if (lua_istable(L, -1)) {
        int sse_count = (int)luaL_len(L, -1);
        for (int i = 1; rc == 0 && i <= sse_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }

            lua_getfield(L, -1, "path");
            lua_getfield(L, -2, "handler_id");

            const char *path = lua_tostring(L, -2);
            int handler_id = (int)lua_tointeger(L, -1);

            if (!path) {
                log_error("[hull] could not read an app.sse definition; refusing to start");
                rc = -1;
            } else {
                HlLuaSseRoute *sse_route = hl_alloc_malloc(lua->base.alloc,
                                                           sizeof(HlLuaSseRoute));
                if (!sse_route) {
                    rc = -1;
                } else {
                    sse_route->lua = lua;
                    sse_route->handler_id = handler_id;
                    int sn = snprintf(sse_route->path, sizeof(sse_route->path),
                                      "%s", path);
                    if (sn < 0 || (size_t)sn >= sizeof(sse_route->path)) {
                        log_error("[hull:lua] app.sse: path too long (max 255 chars): %s",
                                  path);
                        hl_alloc_free(lua->base.alloc, sse_route,
                                      sizeof(HlLuaSseRoute));
                        rc = -1;
                    } else if (hl_lua_track_alloc(lua, &lua->sse_routes,
                                   &lua->sse_route_count,
                                   &lua->sse_route_cap, sse_route) != 0) {
                        hl_alloc_free(lua->base.alloc, sse_route,
                                      sizeof(HlLuaSseRoute));
                        rc = -1;
                    } else if (kl_http_server_route(server, "GET", sse_route->path,
                                                    hl_lua_sse_handler, sse_route,
                                                    NULL) != 0) {
                        rc = -1;
                    }
                }
                if (rc != 0)
                    log_error("[hull] could not register app.sse %s; refusing to start",
                              path);
            }

            lua_pop(L, 3); /* path, handler_id, sse def entry */
        }
    }
    lua_pop(L, 1); /* pop __hull_sse_defs */

    return rc;
}
