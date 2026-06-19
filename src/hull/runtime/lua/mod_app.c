/* mod_app.c — hull.app module: route registration, middleware, timers
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/module_registry.h"

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

/* Phase gate.  Raises a structured Lua error if the runtime has moved
 * past the boot phase (top-level + app.main).  Called at the top of
 * every app.X registration binding so a route added from inside a
 * request handler or timer callback produces a clear actionable
 * message instead of silently disappearing into a registry table no
 * consumer reads post wire_routes.  Function never returns when it
 * rejects (luaL_error long-jumps). */
static int lua_app_reject_if_serving(lua_State *L, const char *call)
{
    HlLua *lua = get_hl_lua(L);
    if (lua && lua->base.registration_closed) {
        luaL_error(L,
            "%s can only be called at app startup (top-level code or "
            "inside app.main). Hull seals the router after wire-up so "
            "dynamic registration from request handlers / timer "
            "callbacks is intentionally not supported. Move the "
            "registration to top level, or to an app.main(fn) that "
            "runs before the serve loop starts.",
            call);
    }
    return 0;
}

/* Helper: register a route with given method string.
 *
 * Signature: app.<verb>(pattern, handler [, opts])
 *
 *   opts.multipart = { max_part_size?, max_total_size?, max_parts?,
 *                      max_headers_size?, max_input_buffer? }
 *     When present, this route uses Keel's streaming-multipart body
 *     reader instead of the default buffered reader. The handler can
 *     iterate parts via req:multipart() (see §1.5.b-2 iterator
 *     bindings). All caps are integers; 0 or missing = unlimited.
 *     The whole opts.multipart subtable is stashed on the route def
 *     and re-read in routes.c (which allocates the KlMultipartConfig
 *     and registers the route via kl_server_route_streaming).
 */
static int lua_app_route(lua_State *L, const char *method)
{
    lua_app_reject_if_serving(L, "app.get/post/put/delete/patch/options");
    const char *pattern = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    /* Arg 3 is the optional opts table — type-check only if present. */
    int has_opts = !lua_isnoneornil(L, 3);
    if (has_opts) luaL_checktype(L, 3, LUA_TTABLE);

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

    /* Stash opts.multipart (if a table) verbatim on the def. routes.c
     * reads the integer caps off this subtable when materializing the
     * KlMultipartConfig. */
    if (has_opts) {
        lua_getfield(L, 3, "multipart");
        if (lua_istable(L, -1)) {
            lua_setfield(L, -2, "multipart");  /* def.multipart = opts.multipart */
        } else {
            lua_pop(L, 1);
        }
    }

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
    lua_app_reject_if_serving(L, "app.use");
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
    lua_app_reject_if_serving(L, "app.use_post");
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
    lua_app_reject_if_serving(L, "app.every");
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
    lua_app_reject_if_serving(L, "app.daily");
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
    lua_app_reject_if_serving(L, "app.ws");
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
    lua_app_reject_if_serving(L, "app.sse");
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

/* Forward decls for the conditionally-installed timer methods. */
static int lua_app_every(lua_State *L);
static int lua_app_daily(lua_State *L);

/* Scan an `app.manifest({modules = {...}})` table for a literal
 * "hull/<name>@<major>" entry. Returns 1 if found. The check is
 * loose: it matches any entry whose string starts with the prefix
 * so "hull/timers" matches "hull/timers@1" or "hull/timers@2".
 *
 * Stack on entry: table at index `manifest_idx`. Stack unchanged
 * on return. */
/* True iff `prefix` matches `name` directly OR any of `name`'s
 * registry-declared transitive deps. Mirrors the resolver's auto-admit
 * walk (module_resolver.c) but runs inline at decoration time, before
 * the resolver has populated the bitset. Bounded by registry size. */
static int spec_chain_matches(const HlModuleSpec *spec, const char *prefix,
                              size_t plen, int depth)
{
    if (!spec || depth > 8) return 0;
    if (strncmp(spec->name, prefix, plen) == 0) return 1;
    for (int i = 0; i < HL_MODULE_MAX_DEPS && spec->deps[i]; i++) {
        const HlModuleSpec *dep = hl_module_registry_find(spec->deps[i]);
        if (spec_chain_matches(dep, prefix, plen, depth + 1)) return 1;
    }
    return 0;
}

static int manifest_declares_module(lua_State *L, int manifest_idx,
                                    const char *prefix)
{
    int found = 0;
    lua_getfield(L, manifest_idx, "modules");
    if (lua_istable(L, -1)) {
        size_t plen = strlen(prefix);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isstring(L, -1)) {
                const char *s = lua_tostring(L, -1);
                /* Fast path: direct prefix match against the declared
                 * string (e.g. "hull/http-server@1"). */
                if (s && strncmp(s, prefix, plen) == 0) {
                    found = 1;
                } else if (s) {
                    /* Slow path: walk registry deps in case the user
                     * declared something that transitively pulls in
                     * `prefix`. Mirrors the resolver's auto-admit so
                     * decoration is in lockstep with module gating.
                     * Strip the "@N" version suffix before looking up
                     * the registry entry. */
                    char nameonly[HL_MODULE_NAME_MAX];
                    const char *at = strchr(s, '@');
                    size_t nlen = at ? (size_t)(at - s) : strlen(s);
                    if (nlen + 1 <= sizeof(nameonly)) {
                        memcpy(nameonly, s, nlen);
                        nameonly[nlen] = '\0';
                        const HlModuleSpec *spec =
                            hl_module_registry_find(nameonly);
                        if (spec_chain_matches(spec, prefix, plen, 0))
                            found = 1;
                    }
                }
            }
            lua_pop(L, 1); /* drop value, keep key */
            if (found) {
                lua_pop(L, 1); /* drop key */
                break;
            }
        }
    }
    lua_pop(L, 1); /* drop modules (or nil) */
    return found;
}

/* Install app.every / app.daily on the `app` global. Called from
 * lua_app_manifest after the manifest is captured, only when
 * "hull/timers@..." is declared. Mirrors the C# partial-class
 * pattern: the methods literally don't exist on `app` unless the
 * module is declared. */
static void install_app_timers(lua_State *L)
{
    lua_getglobal(L, "app");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, lua_app_every);
        lua_setfield(L, -2, "every");
        lua_pushcfunction(L, lua_app_daily);
        lua_setfield(L, -2, "daily");
    }
    lua_pop(L, 1); /* drop app */
}

/* Install the REST + middleware + router surface on the `app`
 * global. Called when "hull/http-server@1" is declared in the
 * manifest. The router (defined in router_src below as embedded
 * Lua) gets evaluated as part of the install so app.router(...)
 * comes along with the verbs. */
static void install_app_http_server(lua_State *L)
{
    lua_getglobal(L, "app");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, lua_app_get);      lua_setfield(L, -2, "get");
        lua_pushcfunction(L, lua_app_post);     lua_setfield(L, -2, "post");
        lua_pushcfunction(L, lua_app_put);      lua_setfield(L, -2, "put");
        lua_pushcfunction(L, lua_app_del);      lua_setfield(L, -2, "delete");
        lua_pushcfunction(L, lua_app_del);      lua_setfield(L, -2, "del");
        lua_pushcfunction(L, lua_app_patch);    lua_setfield(L, -2, "patch");
        lua_pushcfunction(L, lua_app_options);  lua_setfield(L, -2, "options");
        lua_pushcfunction(L, lua_app_use);      lua_setfield(L, -2, "use");
        lua_pushcfunction(L, lua_app_use_post); lua_setfield(L, -2, "use_post");
    }
    lua_pop(L, 1);

    /* Router is bundled with hull/http-server — install it now too. */
    hl_lua_install_app_router(L);
}

/* Install app.sse on the `app` global. Called when "hull/web/sse@1"
 * is declared. */
static void install_app_sse(lua_State *L)
{
    lua_getglobal(L, "app");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, lua_app_sse);
        lua_setfield(L, -2, "sse");
    }
    lua_pop(L, 1);
}

/* Install app.ws on the `app` global. Called when "hull/web/ws-server@1"
 * is declared. The ws.broadcast/ws.connections helpers come from
 * the require("hull.web.ws-server") module (see mod_ws.c). */
static void install_app_ws_server(lua_State *L)
{
    lua_getglobal(L, "app");
    if (lua_istable(L, -1)) {
        lua_pushcfunction(L, lua_app_ws);
        lua_setfield(L, -2, "ws");
    }
    lua_pop(L, 1);
}

/* app.manifest(tbl) — declare application capabilities (one-shot).
 *
 * Conditionally decorates the `app` intrinsic with methods provided
 * by declared modules. Today: hull/timers adds app.every/app.daily.
 * The decoration happens here, not at module-init time, because
 * the runtime only knows what modules an app declared once
 * app.manifest is called. */
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

    /* Module-conditional method installation. Each declared module
     * may decorate the `app` intrinsic with additional methods. */
    if (manifest_declares_module(L, 1, "hull/http-server")) {
        install_app_http_server(L);
    }
    if (manifest_declares_module(L, 1, "hull/web/ws-server")) {
        install_app_ws_server(L);
    }
    if (manifest_declares_module(L, 1, "hull/web/sse")) {
        install_app_sse(L);
    }
    if (manifest_declares_module(L, 1, "hull/timers")) {
        install_app_timers(L);
    }

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

/* The default `app` table contains only the bootstrap registration
 * surface: manifest, main, get_manifest. Every other method is
 * conditionally installed by lua_app_manifest based on the modules
 * an app declares:
 *
 *   hull/http-server@1 → get/post/put/delete/del/patch/options
 *                         use/use_post/router
 *   hull/web/ws-server@1   → ws
 *   hull/web/sse@1         → sse
 *   hull/timers@1      → every/daily (existing)
 *
 * Without the declaration, those methods literally don't exist on
 * `app` — accessing them yields nil (Lua) / undefined (JS) and
 * calling them raises a clear error.
 */
static const luaL_Reg app_funcs[] = {
    /* every + daily are installed conditionally by lua_app_manifest
     * when the manifest declares "hull/timers@1" — they're absent
     * from the default app table. */
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

/* ─────────────────────────────────────────────────────────────────────
 * app.router(prefix, opts) — prefix-mounted route group.
 *
 * Composes on top of app.get/app.post/app.use rather than duplicating
 * route registration logic in C. The Router class is pure Lua;
 * methods translate `r:get(path, h)` into `app.get(prefix .. path, h)`.
 *
 * Why this lives in C as an embedded string instead of a stdlib .lua:
 * the Router patches the intrinsic `app` global, so it has to load at
 * runtime init (after `lua_setglobal(L, "app")`) before any user code
 * runs. A regular stdlib .lua would require an explicit manifest
 * declaration, which defeats the "method on app" ergonomics.
 *
 * Call hl_lua_install_app_router(L) from modules.c after the app
 * global is set. The eval runs once per Lua state.
 * ──────────────────────────────────────────────────────────────────── */

static const char router_src[] =
"do\n"
"  local Router = {}\n"
"  Router.__index = Router\n"
"\n"
"  local function route_method(m)\n"
"    return function(self, path, h)\n"
"      app[m](self.prefix .. path, h)\n"
"      return self\n"
"    end\n"
"  end\n"
"\n"
"  Router.get     = route_method('get')\n"
"  Router.post    = route_method('post')\n"
"  Router.put     = route_method('put')\n"
"  Router.delete  = route_method('delete')\n"
"  Router.patch   = route_method('patch')\n"
"  Router.options = route_method('options')\n"
"\n"
"  function Router:use(a, b, c)\n"
"    if type(a) == 'function' then\n"
"      app.use('*', self.prefix .. '/*', a)\n"
"    else\n"
"      app.use(a, self.prefix .. b, c)\n"
"    end\n"
"    return self\n"
"  end\n"
"\n"
"  function Router:use_post(a, b, c)\n"
"    if type(a) == 'function' then\n"
"      app.use_post('*', self.prefix .. '/*', a)\n"
"    else\n"
"      app.use_post(a, self.prefix .. b, c)\n"
"    end\n"
"    return self\n"
"  end\n"
"\n"
"  function Router:ws(path, h)  app.ws(self.prefix .. path, h);  return self end\n"
"  function Router:sse(path, h) app.sse(self.prefix .. path, h); return self end\n"
"\n"
"  function Router:router(sub, opts)\n"
"    return setmetatable({prefix = self.prefix .. (sub or ''), opts = opts}, Router)\n"
"  end\n"
"\n"
"  app.router = function(prefix, opts)\n"
"    return setmetatable({prefix = prefix or '', opts = opts}, Router)\n"
"  end\n"
"end\n";

int hl_lua_install_app_router(lua_State *L)
{
    if (luaL_dostring(L, router_src) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        return luaL_error(L, "app.router init failed: %s", err ? err : "?");
    }
    return 0;
}
