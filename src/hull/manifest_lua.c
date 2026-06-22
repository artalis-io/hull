/*
 * manifest_lua.c — Extract HlManifest from Lua registry __hull_manifest
 *
 * Split from manifest.c as part of architectural roadmap item G.
 * Compiles to an empty translation unit when HL_ENABLE_LUA is not set.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest.h"
#include "hull/utils/alloc.h"
#include "manifest_internal.h"
#include "log.h"

#include <stdlib.h>
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

    /* tui = true */
    lua_getfield(L, manifest_idx, "tui");
    out->tui = lua_toboolean(L, -1);
    lua_pop(L, 1);

    /* modules = { crypto = "1", fs = "1", ["hull/http"] = "1" }
     *
     * Strict format: key is the module name (short alias or full
     * canonical), value is a version-string parsed as the API major.
     * Other shapes (array, boolean, number) are rejected silently here
     * and surface as resolver errors later. The presence of the key —
     * even with an empty table — sets `modules_declared = 1`. */
    lua_getfield(L, manifest_idx, "modules");
    if (lua_istable(L, -1)) {
        int modules_idx = lua_gettop(L);
        out->modules_declared = 1;

        /* Syntax: `modules = { "<vendor>/<name>@<major>", ... }` — a
         * Lua sequence of canonical spec strings. The local-variable
         * name in `require()` is the user-facing alias (a plain Lua
         * convention); the manifest only declares which canonical
         * modules are in scope.
         *
         * We iterate as a sequence first (the supported form). For
         * back-compat we also accept the legacy table-keyed form
         * `{ crypto = "hull/crypto@1" }` if the sequence is empty —
         * keys are then ignored as cosmetic labels. */
        lua_Integer seq_len = luaL_len(L, modules_idx);
        if (seq_len > 0) {
            for (lua_Integer i = 1; i <= seq_len; i++) {
                if (out->modules_count >= HL_MANIFEST_MAX_MODULES) {
                    log_warn("[manifest] modules array exceeds "
                             "HL_MANIFEST_MAX_MODULES (%d), truncated",
                             HL_MANIFEST_MAX_MODULES);
                    break;
                }
                lua_rawgeti(L, modules_idx, i);
                if (lua_type(L, -1) == LUA_TSTRING) {
                    const char *spec = lua_tostring(L, -1);
                    const char *at   = strchr(spec, '@');
                    if (!at || at == spec) {
                        log_warn("[manifest] modules[%lld] = %s — expected "
                                 "\"vendor/name@version\", ignored",
                                 (long long)i, spec);
                    } else {
                        char *end = NULL;
                        long v = strtol(at + 1, &end, 10);
                        if (end == at + 1 || *end != '\0' ||
                            v < 1 || v > 255) {
                            log_warn("[manifest] modules[%lld] = %s — "
                                     "invalid major version, ignored",
                                     (long long)i, spec);
                        } else {
                            size_t nlen = (size_t)(at - spec);
                            char *namebuf = hl_alloc_malloc(alloc, nlen + 1);
                            if (namebuf) {
                                memcpy(namebuf, spec, nlen);
                                namebuf[nlen] = '\0';
                                out->modules[out->modules_count].name      = namebuf;
                                out->modules[out->modules_count].api_major = (uint8_t)v;
                                out->modules_count++;
                            }
                        }
                    }
                }
                lua_pop(L, 1);
            }
        } else {
            /* Fall back to keyed form (legacy). Iterate string keys. */
            int truncated_warned = 0;
            lua_pushnil(L);
            while (lua_next(L, modules_idx) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING &&
                    lua_type(L, -1) == LUA_TSTRING) {
                    if (out->modules_count >= HL_MANIFEST_MAX_MODULES) {
                        if (!truncated_warned) {
                            log_warn("[manifest] modules object exceeds "
                                     "HL_MANIFEST_MAX_MODULES (%d), truncated",
                                     HL_MANIFEST_MAX_MODULES);
                            truncated_warned = 1;
                        }
                        lua_pop(L, 1);
                        continue;
                    }
                    const char *alias = lua_tostring(L, -2);
                    const char *spec  = lua_tostring(L, -1);
                    const char *at    = strchr(spec, '@');
                    if (!at || at == spec) {
                        log_warn("[manifest] modules.%s = %s — expected "
                                 "\"vendor/name@version\", ignored",
                                 alias, spec);
                    } else {
                        char *end = NULL;
                        long v = strtol(at + 1, &end, 10);
                        if (end == at + 1 || *end != '\0' ||
                            v < 1 || v > 255) {
                            log_warn("[manifest] modules.%s = %s — invalid "
                                     "major version, ignored", alias, spec);
                        } else {
                            size_t nlen = (size_t)(at - spec);
                            char *namebuf = hl_alloc_malloc(alloc, nlen + 1);
                            if (namebuf) {
                                memcpy(namebuf, spec, nlen);
                                namebuf[nlen] = '\0';
                                out->modules[out->modules_count].name      = namebuf;
                                out->modules[out->modules_count].api_major = (uint8_t)v;
                                out->modules_count++;
                            }
                        }
                    }
                }
                lua_pop(L, 1);
            }
        }
    }
    lua_pop(L, 1); /* pop modules */

    /* allow_dynamic_code = true — opt-in to JIT / runtime codegen.
     * Rejected by hl_sandbox_apply unless --no-sandbox. */
    lua_getfield(L, manifest_idx, "allow_dynamic_code");
    out->allow_dynamic_code = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (out->allow_dynamic_code)
        log_warn("[manifest] allow_dynamic_code=true — kernel sandbox "
                 "will fail closed unless --no-sandbox is set");

    /* allow_dynamic_libraries = true — opt-in to dlopen() of native libs.
     * Rejected by hl_sandbox_apply unless --no-sandbox. */
    lua_getfield(L, manifest_idx, "allow_dynamic_libraries");
    out->allow_dynamic_libraries = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (out->allow_dynamic_libraries)
        log_warn("[manifest] allow_dynamic_libraries=true — kernel sandbox "
                 "will fail closed unless --no-sandbox is set");

    lua_pop(L, 1); /* pop manifest table */
    return 0;
}

#endif /* HL_ENABLE_LUA */
