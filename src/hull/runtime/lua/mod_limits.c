/*
 * mod_limits.c - hull._limits (stdlib-only): the input ceilings
 *
 * hull.json's decoder asks it, on every decode, for the JSON nesting depth
 * and key-count ceilings (hull/limits/input.h): the defaults, with the
 * manifest's `limits` over them once the manifest is applied. Read at call
 * time because hull.json is preloaded before app.manifest() has run.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"           /* get_hl_lua */
#include "hull/limits/input.h"

/* json() -> max_depth, max_keys, depth_ceil, keys_ceil. The ceilings bound
 * a per-call { max_depth, max_keys } as they bound the manifest's values (C
 * audit L13). */
static int lua_limits_json(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    HlInputLimits l = hl_input_limits_default();
    if (lua && lua->base.input_limits.json_max_depth)
        l = lua->base.input_limits;
    lua_pushinteger(L, (lua_Integer)l.json_max_depth);
    lua_pushinteger(L, (lua_Integer)l.json_max_keys);
    lua_pushinteger(L, (lua_Integer)HL_INPUT_JSON_MAX_DEPTH_CEIL);
    lua_pushinteger(L, (lua_Integer)HL_INPUT_JSON_MAX_KEYS_CEIL);
    return 4;
}

int luaopen_hull_limits_native(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "json", lua_limits_json },
        { NULL, NULL }
    };
    hl_lua_newlib(L, fns);
    return 1;
}
