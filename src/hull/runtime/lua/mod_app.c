/* mod_app.c — hull.app module: route registration, middleware, timers
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.app module
 *
 * Provides route registration: app.get(), app.post(), app.use(), etc.
 * Routes are stored in the Lua registry:
 *   registry["__hull_routes"]     = { [1]=fn, [2]=fn, ... }
 *   registry["__hull_route_defs"] = { [1]={method,pattern,handler_id}, ... }
 * ════════════════════════════════════════════════════════════════════ */

/* Returns 1 if app.main has been registered. */
static int lua_app_main_registered(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_main");
    int has = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return has;
}

/* No-op: kept for source compatibility with the call sites in this
 * file. app.main and route registration are no longer mutually
 * exclusive — see the lifecycle docs in CLAUDE.md and serve.c. */
static int lua_app_reject_if_main(lua_State *L, const char *call)
{
    (void)L; (void)call;
    return 0;
}

/* Helper: register a route with given method string */
static int lua_app_route(lua_State *L, const char *method)
{
    lua_app_reject_if_main(L, "app.get/post/put/delete/patch/options");
    const char *pattern = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    /* Ensure __hull_routes table exists in registry */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    }

    /* Ensure __hull_route_defs table exists in registry */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_route_defs");
    }

    /* Get next handler index from __hull_routes (may have gaps from middleware) */
    lua_Integer handler_id = (lua_Integer)luaL_len(L, -2) + 1;

    /* Store handler function in __hull_routes[handler_id] */
    /* Stack: routes_table, defs_table */
    lua_pushvalue(L, 2); /* push handler function */
    lua_rawseti(L, -3, handler_id); /* routes[handler_id] = handler */

    /* Store route definition in __hull_route_defs — use contiguous index
     * so that luaL_len always returns the correct count even when
     * middleware was registered before routes. */
    lua_Integer def_idx = (lua_Integer)luaL_len(L, -1) + 1;
    lua_newtable(L);
    lua_pushstring(L, method);
    lua_setfield(L, -2, "method");
    lua_pushstring(L, pattern);
    lua_setfield(L, -2, "pattern");
    lua_pushinteger(L, handler_id);
    lua_setfield(L, -2, "handler_id");
    lua_rawseti(L, -2, def_idx); /* defs[def_idx] = def */

    lua_pop(L, 2); /* pop routes_table, defs_table */
    return 0;
}

static int lua_app_get(lua_State *L)     { return lua_app_route(L, "GET"); }
static int lua_app_post(lua_State *L)    { return lua_app_route(L, "POST"); }
static int lua_app_put(lua_State *L)     { return lua_app_route(L, "PUT"); }
static int lua_app_del(lua_State *L)     { return lua_app_route(L, "DELETE"); }
static int lua_app_patch(lua_State *L)   { return lua_app_route(L, "PATCH"); }
static int lua_app_options(lua_State *L) { return lua_app_route(L, "OPTIONS"); }

/* app.use(method, pattern, handler) — middleware registration */
static int lua_app_use(lua_State *L)
{
    lua_app_reject_if_main(L, "app.use");
    const char *method = luaL_checkstring(L, 1);
    const char *pattern = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    /* Store handler in __hull_routes (same array as route handlers) */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    }

    lua_Integer handler_id = (lua_Integer)luaL_len(L, -1) + 1;
    lua_pushvalue(L, 3);
    lua_rawseti(L, -2, handler_id);
    lua_pop(L, 1); /* pop routes table */

    /* Store middleware entry with handler_id */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_middleware");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_middleware");
    }

    lua_Integer idx = (lua_Integer)luaL_len(L, -1) + 1;

    lua_newtable(L);
    lua_pushstring(L, method);
    lua_setfield(L, -2, "method");
    lua_pushstring(L, pattern);
    lua_setfield(L, -2, "pattern");
    lua_pushinteger(L, handler_id);
    lua_setfield(L, -2, "handler_id");
    lua_rawseti(L, -2, idx);

    lua_pop(L, 1); /* pop middleware table */
    return 0;
}

/* app.use_post(method, pattern, fn) — register post-body middleware */
static int lua_app_use_post(lua_State *L)
{
    lua_app_reject_if_main(L, "app.use_post");
    const char *method = luaL_checkstring(L, 1);
    const char *pattern = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    /* Store handler in __hull_routes (same array as route handlers) */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_routes");
    }

    lua_Integer handler_id = (lua_Integer)luaL_len(L, -1) + 1;
    lua_pushvalue(L, 3);
    lua_rawseti(L, -2, handler_id);
    lua_pop(L, 1); /* pop routes table */

    /* Store middleware entry with handler_id in __hull_post_middleware */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_post_middleware");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_post_middleware");
    }

    lua_Integer idx = (lua_Integer)luaL_len(L, -1) + 1;

    lua_newtable(L);
    lua_pushstring(L, method);
    lua_setfield(L, -2, "method");
    lua_pushstring(L, pattern);
    lua_setfield(L, -2, "pattern");
    lua_pushinteger(L, handler_id);
    lua_setfield(L, -2, "handler_id");
    lua_rawseti(L, -2, idx);

    lua_pop(L, 1); /* pop post_middleware table */
    return 0;
}

/* app.every(interval_ms, handler) — repeating timer */
static int lua_app_every(lua_State *L)
{
    lua_app_reject_if_main(L, "app.every");
    lua_Integer interval_ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (interval_ms < 100)
        return luaL_error(L, "app.every() minimum interval is 100ms");

    /* Store handler in __hull_timers registry table */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_timers");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_timers");
    }

    lua_Integer handler_id = (lua_Integer)luaL_len(L, -1) + 1;
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, handler_id);
    lua_pop(L, 1); /* pop timers table */

    /* Store timer def in __hull_timer_defs */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_timer_defs");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_timer_defs");
    }

    lua_Integer idx = (lua_Integer)luaL_len(L, -1) + 1;

    lua_newtable(L);
    lua_pushstring(L, "every");
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, interval_ms);
    lua_setfield(L, -2, "interval_ms");
    lua_pushinteger(L, handler_id);
    lua_setfield(L, -2, "handler_id");
    lua_rawseti(L, -2, idx);

    lua_pop(L, 1); /* pop timer_defs table */
    return 0;
}

/* app.daily(time_str, handler [, opts]) — daily timer at HH:MM */
static int lua_app_daily(lua_State *L)
{
    lua_app_reject_if_main(L, "app.daily");
    const char *time_str = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    /* Parse "HH:MM" — character-level validation, no sscanf */
    if (strlen(time_str) != 5 || time_str[2] != ':' ||
        time_str[0] < '0' || time_str[0] > '9' ||
        time_str[1] < '0' || time_str[1] > '9' ||
        time_str[3] < '0' || time_str[3] > '9' ||
        time_str[4] < '0' || time_str[4] > '9')
        return luaL_error(L, "app.daily() requires time in HH:MM format");
    int hour = (time_str[0] - '0') * 10 + (time_str[1] - '0');
    int minute = (time_str[3] - '0') * 10 + (time_str[4] - '0');
    if (hour > 23 || minute > 59)
        return luaL_error(L, "app.daily() requires time in HH:MM format");

    /* Check opts table for localtime */
    int use_localtime = 0;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "localtime");
        if (lua_isboolean(L, -1))
            use_localtime = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    /* Store handler in __hull_timers registry table */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_timers");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_timers");
    }

    lua_Integer handler_id = (lua_Integer)luaL_len(L, -1) + 1;
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, handler_id);
    lua_pop(L, 1); /* pop timers table */

    /* Store timer def in __hull_timer_defs */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_timer_defs");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_timer_defs");
    }

    lua_Integer idx = (lua_Integer)luaL_len(L, -1) + 1;

    lua_newtable(L);
    lua_pushstring(L, "daily");
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, hour);
    lua_setfield(L, -2, "hour");
    lua_pushinteger(L, minute);
    lua_setfield(L, -2, "minute");
    lua_pushboolean(L, use_localtime);
    lua_setfield(L, -2, "localtime");
    lua_pushinteger(L, handler_id);
    lua_setfield(L, -2, "handler_id");
    lua_rawseti(L, -2, idx);

    lua_pop(L, 1); /* pop timer_defs table */
    return 0;
}

/* ── Helper: store handler in __hull_routes, return handler_id ──── */

static lua_Integer store_handler(lua_State *L, int func_idx,
                                  const char *table_name)
{
    lua_getfield(L, LUA_REGISTRYINDEX, table_name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, table_name);
    }

    lua_Integer handler_id = (lua_Integer)luaL_len(L, -1) + 1;
    lua_pushvalue(L, func_idx);
    lua_rawseti(L, -2, handler_id);
    lua_pop(L, 1);
    return handler_id;
}

/* ── app.ws(path, callbacks) — WebSocket endpoint registration ───── */

static int lua_app_ws(lua_State *L)
{
    lua_app_reject_if_main(L, "app.ws");
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    /* Ensure __hull_ws_defs exists */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_ws_defs");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_ws_defs");
    }

    lua_Integer idx = (lua_Integer)luaL_len(L, -1) + 1;

    lua_newtable(L); /* ws def entry */
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");

    /* Extract and store each callback */
    lua_getfield(L, 2, "on_open");
    if (lua_isfunction(L, -1)) {
        lua_Integer hid = store_handler(L, lua_gettop(L), "__hull_routes");
        lua_pushinteger(L, hid);
        lua_setfield(L, -3, "on_open_id");
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "on_message");
    if (lua_isfunction(L, -1)) {
        lua_Integer hid = store_handler(L, lua_gettop(L), "__hull_routes");
        lua_pushinteger(L, hid);
        lua_setfield(L, -3, "on_message_id");
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "on_close");
    if (lua_isfunction(L, -1)) {
        lua_Integer hid = store_handler(L, lua_gettop(L), "__hull_routes");
        lua_pushinteger(L, hid);
        lua_setfield(L, -3, "on_close_id");
    }
    lua_pop(L, 1);

    lua_rawseti(L, -2, idx); /* ws_defs[idx] = def */
    lua_pop(L, 1); /* pop __hull_ws_defs */
    return 0;
}

/* ── app.sse(path, handler) — SSE endpoint registration ────────────── */

static int lua_app_sse(lua_State *L)
{
    lua_app_reject_if_main(L, "app.sse");
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_Integer handler_id = store_handler(L, 2, "__hull_routes");

    /* Store SSE def in __hull_sse_defs */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_sse_defs");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_sse_defs");
    }

    lua_Integer idx = (lua_Integer)luaL_len(L, -1) + 1;

    lua_newtable(L);
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, handler_id);
    lua_setfield(L, -2, "handler_id");
    lua_rawseti(L, -2, idx);

    lua_pop(L, 1); /* pop __hull_sse_defs */
    return 0;
}

/* app.manifest(tbl) — declare application capabilities (one-shot) */
static int lua_app_manifest(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Reject second call — manifest is immutable once declared */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    if (!lua_isnil(L, -1))
        return luaL_error(L, "app.manifest() can only be called once");
    lua_pop(L, 1);

    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    return 0;
}

/* app.get_manifest() — retrieve manifest table (for build tools) */
static int lua_app_get_manifest(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    if (lua_isnil(L, -1))
        return 1; /* returns nil */
    return 1;
}

/* app.main(fn) — register a startup hook.
 *
 * Lifecycle: serve.c invokes the function once after manifest extraction
 * + sandbox + migrations, on the event loop thread. If the app also
 * registered routes / middleware / timers / WebSocket / SSE handlers,
 * the HTTP listener auto-starts after main returns nil/0. Returning a
 * non-zero exit code from main short-circuits — the process exits with
 * that code even if routes are registered. Apps with main only and no
 * routes exit when main returns. */
static int lua_app_main(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);

    if (lua_app_main_registered(L))
        return luaL_error(L, "app.main() can only be called once");

    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_main");
    return 0;
}

static const luaL_Reg app_funcs[] = {
    {"get",          lua_app_get},
    {"post",         lua_app_post},
    {"put",          lua_app_put},
    {"delete",       lua_app_del},
    {"del",          lua_app_del},   /* deprecated alias — use `app.delete` */
    {"patch",        lua_app_patch},
    {"options",      lua_app_options},
    {"use",          lua_app_use},
    {"use_post",     lua_app_use_post},
    {"ws",           lua_app_ws},
    {"sse",          lua_app_sse},
    {"every",        lua_app_every},
    {"daily",        lua_app_daily},
    {"main",         lua_app_main},
    {"manifest",     lua_app_manifest},
    {"get_manifest", lua_app_get_manifest},
    {NULL, NULL}
};

int luaopen_hull_app(lua_State *L)
{
    luaL_newlib(L, app_funcs);
    return 1;
}
