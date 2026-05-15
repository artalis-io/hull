/*
 * manifest_lua.c — Extract HlManifest from Lua registry __hull_manifest
 *
 * Split from manifest.c as part of architectural roadmap item G.
 * Compiles to an empty translation unit when HL_ENABLE_LUA is not set.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest.h"
#include "hull/alloc.h"
#include "manifest_internal.h"
#include "log.h"

#include <string.h>

#ifdef HL_ENABLE_LUA

#include "lua.h"
#include "lauxlib.h"

/* Read a string array from a Lua table field into a C array.
 * Strings are copied via hl_manifest_strdup.
 * Returns number of strings read (capped at max). */
static int read_string_array(lua_State *L, int table_idx,
                              const char *field,
                              const char **out, int max,
                              HlAllocator *alloc)
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
        if (lua_isstring(L, -1)) {
            const char *copy = hl_manifest_strdup(alloc, lua_tostring(L, -1));
            if (copy)
                out[count++] = copy;
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 1); /* pop array table */
    return count;
}

int hl_manifest_extract_lua(lua_State *L, HlManifest *out, HlAllocator *alloc)
{
    if (!L || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    out->alloc = alloc;

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
                                                 HL_MANIFEST_MAX_PATHS, alloc);
        out->fs_write_count = read_string_array(L, fs_idx, "write",
                                                  out->fs_write,
                                                  HL_MANIFEST_MAX_PATHS, alloc);
    }
    lua_pop(L, 1); /* pop fs */

    /* env = {"PORT", "DATABASE_URL", ...} */
    out->env_count = read_string_array(L, manifest_idx, "env",
                                         out->env,
                                         HL_MANIFEST_MAX_ENVS, alloc);

    /* hosts = {"api.stripe.com", ...} */
    out->hosts_count = read_string_array(L, manifest_idx, "hosts",
                                           out->hosts,
                                           HL_MANIFEST_MAX_HOSTS, alloc);

    /* csp = "policy-string" or false */
    lua_getfield(L, manifest_idx, "csp");
    if (lua_isstring(L, -1)) {
        const char *csp_str = lua_tostring(L, -1);
        if (hl_manifest_csp_is_valid(csp_str)) {
            out->csp = hl_manifest_strdup(alloc, csp_str);
            out->csp_set = 1;
        }
        /* invalid → csp_set stays 0, falls back to default */
    } else if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
        out->csp = NULL;
        out->csp_set = 1;  /* explicitly disabled */
    }
    lua_pop(L, 1);

    /* cors = { origins = {...}, methods = "...", headers = "...",
     *          credentials = true, max_age = 86400 } */
    lua_getfield(L, manifest_idx, "cors");
    if (lua_istable(L, -1)) {
        int cors_idx = lua_gettop(L);
        out->cors_set = 1;
        out->cors_origin_count = read_string_array(L, cors_idx, "origins",
                                                     out->cors_origins,
                                                     HL_MANIFEST_MAX_CORS_ORIGINS,
                                                     alloc);
        lua_getfield(L, cors_idx, "methods");
        if (lua_isstring(L, -1))
            out->cors_methods = hl_manifest_strdup(alloc, lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, cors_idx, "headers");
        if (lua_isstring(L, -1))
            out->cors_headers = hl_manifest_strdup(alloc, lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, cors_idx, "credentials");
        if (lua_isboolean(L, -1))
            out->cors_credentials = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, cors_idx, "max_age");
        if (lua_isinteger(L, -1))
            out->cors_max_age = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* pop cors */

    /* wasm = { heap = N, stack = N, gas = N, max_input = N, max_output = N } */
    lua_getfield(L, manifest_idx, "wasm");
    if (lua_istable(L, -1)) {
        int wasm_idx = lua_gettop(L);
        lua_getfield(L, wasm_idx, "heap");
        if (lua_isinteger(L, -1)) out->wasm_heap = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, wasm_idx, "stack");
        if (lua_isinteger(L, -1)) out->wasm_stack = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, wasm_idx, "gas");
        if (lua_isinteger(L, -1)) out->wasm_gas = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, wasm_idx, "max_input");
        if (lua_isinteger(L, -1)) out->wasm_max_input = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, wasm_idx, "max_output");
        if (lua_isinteger(L, -1)) out->wasm_max_output = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* pop wasm */

    /* gpu = true  OR  gpu = { devices = {0, 1} } */
    lua_getfield(L, manifest_idx, "gpu");
    if (lua_istable(L, -1)) {
        out->gpu = 1;
        lua_getfield(L, -1, "devices");
        if (lua_istable(L, -1)) {
            int len = (int)luaL_len(L, -1);
            for (int i = 1; i <= len && out->gpu_device_count < HL_GPU_MAX_DEVICES; i++) {
                lua_rawgeti(L, -1, i);
                if (lua_isinteger(L, -1))
                    out->gpu_devices[out->gpu_device_count++] = (int)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1); /* pop devices */
    } else {
        out->gpu = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    /* compute = true */
    lua_getfield(L, manifest_idx, "compute");
    out->compute = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_pop(L, 1); /* pop manifest table */
    return 0;
}

#endif /* HL_ENABLE_LUA */
