/* readonly_view.c - read-only views of stdlib module tables (audit 43)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * `require` caches one table per module, and that one table used to go to
 * app code and to stdlib chunks alike. Stdlib code calls through it at call
 * time (totp: `sealbox.seal(keyring(), secret, ctx)`), so an app that
 * declared hull/crypto/sealbox and assigned its own `sealbox.seal` had totp
 * hand it the keyring. The stdlib keeps the real tables; app code gets a VIEW
 * of each:
 *
 *   - an empty table whose metatable reads through to the real one
 *     (`__index`), refuses every write (`__newindex`), and is locked
 *     (`__metatable` = "locked", so setmetatable / getmetatable cannot reach
 *     it - and `debug` is not loaded);
 *   - a table read from a view is itself a view (`encoding.hex`,
 *     `compute.async`, the jobs sentinels), made on first read;
 *   - `rawset` on a view writes the view table only: nothing reads it but the
 *     app, since the stdlib holds the real table;
 *   - `pairs` enumerates the real table (`__pairs`), `#` measures it, `==`
 *     compares the real tables (`__eq`: a sentinel the stdlib hands back, such
 *     as a real jobs.RETRY, equals the app's view of it), and `tostring` /
 *     a call go to the real table's own metamethods when it has them;
 *   - one view per real table per VM (a weak-keyed cache), so
 *     `require(x) == require(x)` holds.
 *
 * A Lua `next` / `rawget` / `rawlen` / `rawequal` on a view sees the empty
 * view table: use `pairs`, field reads, `#` and `==`.
 *
 * Every metamethod is registered with the binding dispatcher
 * (hl_lua_pushcclosure, audit convergence section 6.1), like any other
 * binding, and a field read hands back the real table's value - for a
 * function, the dispatched closure the module registered - so a view never
 * reaches a binding the dispatcher does not meter.
 */

#include "internal.h"

#include <lauxlib.h>
#include <lua.h>

#define RO_CACHE_KEY "__hull_ro_views"

/* Its address marks a view's metatable and keys the real table in it. A
 * lightuserdata key no Lua code can make, behind a locked metatable. */
static char ro_real_key;

/* Pushes the real table behind the view at @p idx, or nothing and 0 when the
 * value there is not a view. */
static int ro_push_real(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TTABLE || !lua_getmetatable(L, idx))
        return 0;
    lua_pushlightuserdata(L, &ro_real_key);
    lua_rawget(L, -2);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 2);
        return 0;
    }
    lua_remove(L, -2);                         /* the metatable */
    return 1;
}

static int ro_index(lua_State *L)
{
    lua_pushvalue(L, 2);
    lua_gettable(L, lua_upvalueindex(1));      /* real[k], its metamethods */
    hl_lua_push_readonly(L, -1);
    return 1;
}

static int ro_newindex(lua_State *L)
{
    return luaL_error(L, "a Hull stdlib module is read-only to app code "
                         "(cannot set '%s')",
                      lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2)
                                                    : luaL_typename(L, 2));
}

/* The iterator __pairs hands out: lua_next over the real table, table
 * values as views. */
static int ro_next(lua_State *L)
{
    lua_settop(L, 2);
    if (!lua_next(L, lua_upvalueindex(1)))
        return 0;
    hl_lua_push_readonly(L, -1);
    lua_replace(L, -2);
    return 2;
}

static int ro_pairs(lua_State *L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    hl_lua_pushcclosure(L, ro_next, 1);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

static int ro_len(lua_State *L)
{
    lua_len(L, lua_upvalueindex(1));
    return 1;
}

static int ro_eq(lua_State *L)
{
    if (!ro_push_real(L, 1)) lua_pushvalue(L, 1);
    if (!ro_push_real(L, 2)) lua_pushvalue(L, 2);
    lua_pushboolean(L, lua_rawequal(L, -1, -2));
    return 1;
}

static int ro_tostring(lua_State *L)
{
    if (luaL_getmetafield(L, lua_upvalueindex(1), "__tostring") != LUA_TNIL) {
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_call(L, 1, 1);
        return 1;
    }
    lua_pushfstring(L, "table: %p", lua_topointer(L, 1));
    return 1;
}

static int ro_call(lua_State *L)
{
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_replace(L, 1);                         /* the real table as callee */
    lua_call(L, n - 1, LUA_MULTRET);
    return lua_gettop(L);
}

void hl_lua_push_readonly(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    if (lua_type(L, idx) != LUA_TTABLE) {
        lua_pushvalue(L, idx);
        return;
    }
    /* A view stays as it is. */
    if (ro_push_real(L, idx)) {
        lua_pop(L, 1);
        lua_pushvalue(L, idx);
        return;
    }

    if (lua_getfield(L, LUA_REGISTRYINDEX, RO_CACHE_KEY) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushliteral(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, RO_CACHE_KEY);
    }
    int cache = lua_gettop(L);
    lua_pushvalue(L, idx);
    if (lua_rawget(L, cache) == LUA_TTABLE) {
        lua_remove(L, cache);
        return;
    }
    lua_pop(L, 1);

    lua_newtable(L);                           /* the view */
    int view = lua_gettop(L);
    lua_createtable(L, 0, 10);                 /* its metatable */
    int mt = lua_gettop(L);

    lua_pushlightuserdata(L, &ro_real_key);
    lua_pushvalue(L, idx);
    lua_rawset(L, mt);

    /* Each closes over the real table. Spelled out, not table-driven, so
     * every registration names its binding (INV-1). */
    lua_pushvalue(L, idx);
    hl_lua_pushcclosure(L, ro_index, 1);
    lua_setfield(L, mt, "__index");
    lua_pushvalue(L, idx);
    hl_lua_pushcclosure(L, ro_pairs, 1);
    lua_setfield(L, mt, "__pairs");
    lua_pushvalue(L, idx);
    hl_lua_pushcclosure(L, ro_len, 1);
    lua_setfield(L, mt, "__len");
    lua_pushvalue(L, idx);
    hl_lua_pushcclosure(L, ro_tostring, 1);
    lua_setfield(L, mt, "__tostring");
    hl_lua_pushcfunction(L, ro_newindex);
    lua_setfield(L, mt, "__newindex");
    hl_lua_pushcfunction(L, ro_eq);
    lua_setfield(L, mt, "__eq");
    /* A call only when the real table answers one. */
    if (luaL_getmetafield(L, idx, "__call") != LUA_TNIL) {
        lua_pop(L, 1);
        lua_pushvalue(L, idx);
        hl_lua_pushcclosure(L, ro_call, 1);
        lua_setfield(L, mt, "__call");
    }
    lua_pushliteral(L, "locked");
    lua_setfield(L, mt, "__metatable");
    lua_setmetatable(L, view);

    lua_pushvalue(L, idx);
    lua_pushvalue(L, view);
    lua_rawset(L, cache);
    lua_remove(L, cache);
}

/* hull._readonly.view(t): the app's view of a stdlib table, for a stdlib
 * function that returns its own module (jobs' chainable calls). */
static int lua_ro_view(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    hl_lua_push_readonly(L, 1);
    return 1;
}

int luaopen_hull_readonly(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "view", lua_ro_view },
        { NULL, NULL }
    };
    hl_lua_newlib(L, fns);
    return 1;
}
