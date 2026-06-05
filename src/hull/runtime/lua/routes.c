/*
 * routes.c — Lua route + middleware + timer + WS/SSE wiring
 *
 * Reads the route/middleware/timer/ws/sse definition tables that
 * `app.<verb>()` builds in the Lua registry and registers them with
 * Keel (KlRouter or KlServer). Also hosts the tracked-allocation
 * helpers used by every wire step so we can free per-route contexts
 * on shutdown without leaking.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/alloc.h"
#include "hull/async_backend.h"
#include "hull/cap/body.h"
#include "hull/cap/ws.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>
#include <keel/body_reader_multipart.h>
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

int hl_lua_wire_routes(HlLua *lua, KlRouter *router)
{
    lua_State *L = lua->L;

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    int count = (int)luaL_len(L, -1);
    if (count <= 0) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "method");
        lua_getfield(L, -2, "pattern");
        lua_getfield(L, -3, "handler_id");

        const char *method_str = lua_tostring(L, -3);
        const char *pattern = lua_tostring(L, -2);
        int handler_id = (int)lua_tointeger(L, -1);

        if (method_str && pattern) {
            HlLuaRoute *route = hl_alloc_malloc(lua->base.alloc,
                                                  sizeof(HlLuaRoute));
            if (route) {
                route->lua = lua;
                route->handler_id = handler_id;
                route->multipart_config = NULL;
                if (hl_lua_track_route(lua, route) != 0) {
                    hl_alloc_free(lua->base.alloc, route, sizeof(HlLuaRoute));
                } else {
                    kl_router_add(router, method_str, pattern,
                                  hl_lua_keel_handler, route, NULL);
                }
            }
        }

        lua_pop(L, 3); /* method_str, pattern, handler_id */
        lua_pop(L, 1); /* route def table */
    }

    lua_pop(L, 1); /* __hull_route_defs table */

    /* Wire pre-body middleware from __hull_middleware */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_middleware");
    if (lua_istable(L, -1)) {
        int mw_count = (int)luaL_len(L, -1);
        for (int i = 1; i <= mw_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

            lua_getfield(L, -1, "method");
            lua_getfield(L, -2, "pattern");
            lua_getfield(L, -3, "handler_id");

            const char *mw_method = lua_tostring(L, -3);
            const char *mw_pattern = lua_tostring(L, -2);
            int mw_handler_id = (int)lua_tointeger(L, -1);

            if (mw_method && mw_pattern) {
                HlLuaRoute *ctx = hl_alloc_malloc(lua->base.alloc,
                                                    sizeof(HlLuaRoute));
                if (ctx) {
                    ctx->lua = lua;
                    ctx->handler_id = mw_handler_id;
                    if (hl_lua_track_route(lua, ctx) != 0)
                        hl_alloc_free(lua->base.alloc, ctx, sizeof(HlLuaRoute));
                    else
                        kl_router_use(router, mw_method, mw_pattern,
                                      hl_lua_keel_middleware, ctx);
                }
            }

            lua_pop(L, 3);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    /* Wire post-body middleware from __hull_post_middleware */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_post_middleware");
    if (lua_istable(L, -1)) {
        int post_mw_count = (int)luaL_len(L, -1);
        for (int i = 1; i <= post_mw_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

            lua_getfield(L, -1, "method");
            lua_getfield(L, -2, "pattern");
            lua_getfield(L, -3, "handler_id");

            const char *mw_method = lua_tostring(L, -3);
            const char *mw_pattern = lua_tostring(L, -2);
            int mw_handler_id = (int)lua_tointeger(L, -1);

            if (mw_method && mw_pattern) {
                HlLuaRoute *ctx = hl_alloc_malloc(lua->base.alloc,
                                                    sizeof(HlLuaRoute));
                if (ctx) {
                    ctx->lua = lua;
                    ctx->handler_id = mw_handler_id;
                    if (hl_lua_track_route(lua, ctx) != 0)
                        hl_alloc_free(lua->base.alloc, ctx, sizeof(HlLuaRoute));
                    else
                        kl_router_use_post(router, mw_method, mw_pattern,
                                           hl_lua_keel_middleware, ctx);
                }
            }

            lua_pop(L, 3);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    return 0;
}

/* ── Server route wiring (with body reader factory) ────────────────── */

/* Read a non-negative integer field from a Lua table at stack idx -1,
 * default to 0 if missing or non-numeric. Caps are size_t in Keel's
 * config; we round-trip through lua_Integer to reject negatives. */
static size_t lua_read_size_field(lua_State *L, const char *key)
{
    lua_getfield(L, -1, key);
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
    lua_getfield(L, -1, key);
    int v = 0;
    if (lua_isnumber(L, -1)) {
        lua_Integer i = lua_tointeger(L, -1);
        if (i > 0 && i <= INT_MAX) v = (int)i;
    }
    lua_pop(L, 1);
    return v;
}

/* Build a heap-allocated KlMultipartConfig from a Lua subtable at -1.
 * Caller frees with hl_alloc_free(...,sizeof(KlMultipartConfig)).
 * Returns NULL on allocation failure. */
static KlMultipartConfig *lua_build_multipart_config(HlLua *lua)
{
    KlMultipartConfig *cfg = hl_alloc_malloc(lua->base.alloc,
                                              sizeof(KlMultipartConfig));
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
 * kl_body_reader_multipart) so the Lua iterator can hl_cap_multipart_park
 * on NEED_DATA. The wrapper forwards our per-route config to the inner
 * Keel reader. */
static KlBodyReader *hl_lua_multipart_factory(KlAllocator *alloc,
                                               const KlRequest *req,
                                               void *user_data)
{
    HlLuaRoute *route = (HlLuaRoute *)user_data;
    return hl_cap_multipart_factory(alloc, req, route->multipart_config);
}

int hl_lua_wire_routes_server(HlLua *lua, KlServer *server,
                               void *(*alloc_fn)(size_t))
{
    (void)alloc_fn; /* routes always use Hull allocator */
    lua_State *L = lua->L;

    /* Store server for async operations (hull.sleep, http.get, etc.) */
    lua->server = server;

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    int count = (int)luaL_len(L, -1);
    if (count <= 0) {
        lua_pop(L, 1);
        log_error("[hull:c] no routes registered");
        return -1;
    }

    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "method");
        lua_getfield(L, -2, "pattern");
        lua_getfield(L, -3, "handler_id");

        const char *method_str = lua_tostring(L, -3);
        const char *pattern = lua_tostring(L, -2);
        int handler_id = (int)lua_tointeger(L, -1);

        if (method_str && pattern) {
            HlLuaRoute *route = hl_alloc_malloc(lua->base.alloc,
                                                  sizeof(HlLuaRoute));
            if (route) {
                route->lua = lua;
                route->handler_id = handler_id;
                route->multipart_config = NULL;

                /* Peek def.multipart — present → streaming route. */
                lua_getfield(L, -4, "multipart");
                int is_streaming = lua_istable(L, -1);
                if (is_streaming) {
                    route->multipart_config = lua_build_multipart_config(lua);
                    if (!route->multipart_config) is_streaming = 0;
                }
                lua_pop(L, 1); /* multipart subtable */

                if (hl_lua_track_route(lua, route) != 0) {
                    if (route->multipart_config) {
                        hl_alloc_free(lua->base.alloc, route->multipart_config,
                                      sizeof(KlMultipartConfig));
                    }
                    hl_alloc_free(lua->base.alloc, route, sizeof(HlLuaRoute));
                } else if (is_streaming) {
                    /* streaming-async (v2.2.0+) — handler is invoked
                     * BEFORE leftover body bytes are fed via on_data.
                     * Closes the single-read leftover-cap UX gap so
                     * parser caps fire structured 4xx responses even
                     * when the body fits in the first kernel read. */
                    kl_server_route_streaming_async(server, method_str, pattern,
                                                     hl_lua_keel_handler, route,
                                                     hl_lua_multipart_factory);
                } else {
                    kl_server_route(server, method_str, pattern,
                                    hl_lua_keel_handler, route,
                                    hl_cap_body_factory);
                }
            }
        }

        lua_pop(L, 3); /* method_str, pattern, handler_id */
        lua_pop(L, 1); /* route def table */
    }

    lua_pop(L, 1); /* __hull_route_defs table */

    /* Wire middleware from __hull_middleware */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_middleware");
    if (lua_istable(L, -1)) {
        int mw_count = (int)luaL_len(L, -1);
        for (int i = 1; i <= mw_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }

            lua_getfield(L, -1, "method");
            lua_getfield(L, -2, "pattern");
            lua_getfield(L, -3, "handler_id");

            const char *method_str = lua_tostring(L, -3);
            const char *pattern = lua_tostring(L, -2);
            int handler_id = (int)lua_tointeger(L, -1);

            if (method_str && pattern) {
                HlLuaRoute *ctx = hl_alloc_malloc(lua->base.alloc,
                                                    sizeof(HlLuaRoute));
                if (ctx) {
                    ctx->lua = lua;
                    ctx->handler_id = handler_id;
                    if (hl_lua_track_route(lua, ctx) != 0) {
                        hl_alloc_free(lua->base.alloc, ctx, sizeof(HlLuaRoute));
                    } else {
                        kl_server_use(server, method_str, pattern,
                                      hl_lua_keel_middleware, ctx);
                    }
                }
            }

            lua_pop(L, 3); /* method_str, pattern, handler_id */
            lua_pop(L, 1); /* middleware entry table */
        }
    }
    lua_pop(L, 1); /* __hull_middleware table */

    /* Wire post-body middleware from __hull_post_middleware */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_post_middleware");
    if (lua_istable(L, -1)) {
        int post_mw_count = (int)luaL_len(L, -1);
        for (int i = 1; i <= post_mw_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }

            lua_getfield(L, -1, "method");
            lua_getfield(L, -2, "pattern");
            lua_getfield(L, -3, "handler_id");

            const char *method_str = lua_tostring(L, -3);
            const char *pattern = lua_tostring(L, -2);
            int handler_id = (int)lua_tointeger(L, -1);

            if (method_str && pattern) {
                HlLuaRoute *ctx = hl_alloc_malloc(lua->base.alloc,
                                                    sizeof(HlLuaRoute));
                if (ctx) {
                    ctx->lua = lua;
                    ctx->handler_id = handler_id;
                    if (hl_lua_track_route(lua, ctx) != 0) {
                        hl_alloc_free(lua->base.alloc, ctx, sizeof(HlLuaRoute));
                    } else {
                        kl_server_use_post(server, method_str, pattern,
                                           hl_lua_keel_middleware, ctx);
                    }
                }
            }

            lua_pop(L, 3); /* method_str, pattern, handler_id */
            lua_pop(L, 1); /* middleware entry table */
        }
    }
    lua_pop(L, 1); /* __hull_post_middleware table */

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
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_ws_defs");
    if (lua_istable(L, -1)) {
        /* Initialize registry if needed */
        if (!lua->base.ws_registry) {
            lua->base.ws_registry = hl_alloc_malloc(lua->base.alloc,
                                                      sizeof(HlWsRegistry));
            if (lua->base.ws_registry)
                hl_ws_registry_init(lua->base.ws_registry, lua->base.alloc);
        }

        int ws_count = (int)luaL_len(L, -1);
        for (int i = 1; i <= ws_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }

            lua_getfield(L, -1, "path");
            const char *path = lua_tostring(L, -1);
            lua_pop(L, 1);

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

            if (path) {
                HlLuaWsRoute *ws_route = hl_alloc_malloc(lua->base.alloc,
                                                           sizeof(HlLuaWsRoute));
                if (ws_route) {
                    ws_route->lua = lua;
                    ws_route->on_open_id = on_open_id;
                    ws_route->on_message_id = on_message_id;
                    ws_route->on_close_id = on_close_id;
                    int wn = snprintf(ws_route->path, sizeof(ws_route->path),
                                      "%s", path);
                    if (wn < 0 || (size_t)wn >= sizeof(ws_route->path)) {
                        hl_alloc_free(lua->base.alloc, ws_route,
                                      sizeof(HlLuaWsRoute));
                        return luaL_error(L, "app.ws: path too long (max 255 chars)");
                    }

                    if (hl_lua_track_alloc(lua, &lua->ws_routes,
                            &lua->ws_route_count,
                            &lua->ws_route_cap, ws_route) != 0) {
                        hl_alloc_free(lua->base.alloc, ws_route,
                                      sizeof(HlLuaWsRoute));
                    } else {
                        KlWsServerConfig *ws_cfg =
                            hl_alloc_malloc(lua->base.alloc,
                                            sizeof(KlWsServerConfig));
                        if (ws_cfg) {
                            kl_ws_server_config_init(ws_cfg);
                            ws_cfg->callbacks.on_open = hl_lua_ws_on_open;
                            ws_cfg->callbacks.on_message = hl_lua_ws_on_message;
                            ws_cfg->callbacks.on_close = hl_lua_ws_on_close;
                            ws_cfg->user_data = ws_route;
                            hl_lua_track_alloc(lua, &lua->ws_cfgs,
                                               &lua->ws_cfg_count,
                                               &lua->ws_cfg_cap, ws_cfg);
                            kl_server_ws(server, path, ws_cfg);
                        }
                    }
                }
            }

            lua_pop(L, 1); /* pop ws def entry */
        }
    }
    lua_pop(L, 1); /* pop __hull_ws_defs */

    /* ── Wire SSE endpoints from __hull_sse_defs ───────────────────── */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_sse_defs");
    if (lua_istable(L, -1)) {
        int sse_count = (int)luaL_len(L, -1);
        for (int i = 1; i <= sse_count; i++) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }

            lua_getfield(L, -1, "path");
            lua_getfield(L, -2, "handler_id");

            const char *path = lua_tostring(L, -2);
            int handler_id = (int)lua_tointeger(L, -1);
            lua_pop(L, 2);

            if (path) {
                HlLuaSseRoute *sse_route = hl_alloc_malloc(lua->base.alloc,
                                                             sizeof(HlLuaSseRoute));
                if (sse_route) {
                    sse_route->lua = lua;
                    sse_route->handler_id = handler_id;
                    if (hl_lua_track_alloc(lua, &lua->sse_routes,
                            &lua->sse_route_count,
                            &lua->sse_route_cap, sse_route) != 0) {
                        hl_alloc_free(lua->base.alloc, sse_route,
                                      sizeof(HlLuaSseRoute));
                    } else {
                        kl_server_route(server, "GET", path,
                                        hl_lua_sse_handler, sse_route, NULL);
                    }
                }
            }

            lua_pop(L, 1); /* pop sse def entry */
        }
    }
    lua_pop(L, 1); /* pop __hull_sse_defs */

    return 0;
}
