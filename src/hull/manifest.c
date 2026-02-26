/*
 * manifest.c — Extract app manifest from Lua state
 *
 * Reads the __hull_manifest registry key (set by app.manifest())
 * and populates an HlManifest struct with capability declarations.
 *
 * String pointers reference Lua-owned memory — valid as long as
 * the Lua state is alive.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest.h"
#include "lua.h"
#include "lauxlib.h"
#include <string.h>

/* Read a string array from a Lua table field into a C array.
 * Returns number of strings read (capped at max). */
static int read_string_array(lua_State *L, int table_idx,
                              const char *field,
                              const char **out, int max)
{
    int count = 0;
    lua_getfield(L, table_idx, field);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }

    int arr_idx = lua_gettop(L);
    lua_Integer len = luaL_len(L, arr_idx);
    for (lua_Integer i = 1; i <= len && count < max; i++) {
        lua_rawgeti(L, arr_idx, i);
        if (lua_isstring(L, -1))
            out[count++] = lua_tostring(L, -1);
        lua_pop(L, 1);
    }

    lua_pop(L, 1); /* pop array table */
    return count;
}

int hl_manifest_extract(lua_State *L, HlManifest *out)
{
    if (!L || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return -1; /* no manifest declared */
    }

    int manifest_idx = lua_gettop(L);
    out->present = 1;

    /* fs = { read = {...}, write = {...} } */
    lua_getfield(L, manifest_idx, "fs");
    if (lua_istable(L, -1)) {
        int fs_idx = lua_gettop(L);
        out->fs_read_count = read_string_array(L, fs_idx, "read",
                                                 out->fs_read,
                                                 HL_MANIFEST_MAX_PATHS);
        out->fs_write_count = read_string_array(L, fs_idx, "write",
                                                  out->fs_write,
                                                  HL_MANIFEST_MAX_PATHS);
    }
    lua_pop(L, 1); /* pop fs */

    /* env = {"PORT", "DATABASE_URL", ...} */
    out->env_count = read_string_array(L, manifest_idx, "env",
                                         out->env,
                                         HL_MANIFEST_MAX_ENVS);

    /* hosts = {"api.stripe.com", ...} */
    out->hosts_count = read_string_array(L, manifest_idx, "hosts",
                                           out->hosts,
                                           HL_MANIFEST_MAX_HOSTS);

    lua_pop(L, 1); /* pop manifest table */
    return 0;
}
