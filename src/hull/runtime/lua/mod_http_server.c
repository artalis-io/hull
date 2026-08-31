/* mod_server.c - hull.server module: server stats
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

#include <keel/http_server.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.server - Server stats
 * ════════════════════════════════════════════════════════════════════ */

/* server.stats() → { active_connections, max_connections, async_suspended,
 *                     listen_paused } */
static int lua_server_stats(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!lua || !lua->server)
        return luaL_error(L, "server.stats: server not available");

    KlHttpServerStats stats;
    kl_http_server_stats(lua->server, &stats);

    lua_newtable(L);
    lua_pushinteger(L, stats.active_connections);
    lua_setfield(L, -2, "active_connections");
    lua_pushinteger(L, stats.max_connections);
    lua_setfield(L, -2, "max_connections");
    lua_pushinteger(L, stats.async_suspended);
    lua_setfield(L, -2, "async_suspended");
    lua_pushboolean(L, stats.listen_paused);
    lua_setfield(L, -2, "listen_paused");
    return 1;
}

static const luaL_Reg server_funcs[] = {
    {"stats", lua_server_stats},
    {NULL, NULL}
};

int luaopen_hull_server(lua_State *L)
{
    luaL_newlib(L, server_funcs);
    return 1;
}
