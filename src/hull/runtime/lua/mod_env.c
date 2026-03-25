/* mod_env.c — hull.env module: environment variable access
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/env.h"

/* ════════════════════════════════════════════════════════════════════
 * hull.env module
 *
 * env.get(name) → string or nil
 * ════════════════════════════════════════════════════════════════════ */

static int lua_env_get(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.env_cfg)
        return luaL_error(L, "env not configured");

    const char *name = luaL_checkstring(L, 1);
    const char *val = hl_cap_env_get(lua->base.env_cfg, name);

    if (val)
        lua_pushstring(L, val);
    else
        lua_pushnil(L);
    return 1;
}

static const luaL_Reg env_funcs[] = {
    {"get", lua_env_get},
    {NULL, NULL}
};

int luaopen_hull_env(lua_State *L)
{
    luaL_newlib(L, env_funcs);
    return 1;
}
