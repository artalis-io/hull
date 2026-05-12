/*
 * manifest.c — Extract app manifest from runtime state
 *
 * Reads the __hull_manifest key (Lua registry or JS globalThis)
 * and populates an HlManifest struct with capability declarations.
 *
 * All strings are copied into Hull-owned allocations so cleanup
 * is uniform: hl_manifest_free() frees everything regardless of
 * which runtime extracted the manifest.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest.h"
#include "hull/alloc.h"
#include "log.h"
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Reject CSP strings containing CR/LF to prevent CRLF header injection. */
static int csp_is_valid(const char *s)
{
    if (!s) return 1;
    for (const char *p = s; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            log_warn("[manifest] CSP string contains CR/LF — rejected");
            return 0;
        }
    }
    return 1;
}

/* Copy a string into the manifest's allocator.  Returns NULL on failure. */
static const char *manifest_strdup(HlAllocator *alloc, const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *copy = hl_alloc_malloc(alloc, len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

/* Free a single manifest-owned string and NULL-out the pointer. */
static void manifest_str_free(HlAllocator *alloc, const char **sp)
{
    if (*sp) {
        hl_alloc_free(alloc, (void *)*sp, strlen(*sp) + 1);
        *sp = NULL;
    }
}

/* ── hl_manifest_free ──────────────────────────────────────────────── */

void hl_manifest_free(HlManifest *m)
{
    if (!m) return;
    HlAllocator *a = m->alloc;

    for (int i = 0; i < m->fs_read_count; i++)
        manifest_str_free(a, &m->fs_read[i]);
    for (int i = 0; i < m->fs_write_count; i++)
        manifest_str_free(a, &m->fs_write[i]);
    for (int i = 0; i < m->env_count; i++)
        manifest_str_free(a, &m->env[i]);
    for (int i = 0; i < m->hosts_count; i++)
        manifest_str_free(a, &m->hosts[i]);
    manifest_str_free(a, &m->csp);
    for (int i = 0; i < m->cors_origin_count; i++)
        manifest_str_free(a, &m->cors_origins[i]);
    manifest_str_free(a, &m->cors_methods);
    manifest_str_free(a, &m->cors_headers);

    memset(m, 0, sizeof(*m));
}

/* ── Lua manifest extraction ───────────────────────────────────────── */

#ifdef HL_ENABLE_LUA

#include "lua.h"
#include "lauxlib.h"

/* Read a string array from a Lua table field into a C array.
 * Strings are copied via manifest_strdup.
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
            const char *copy = manifest_strdup(alloc, lua_tostring(L, -1));
            if (copy)
                out[count++] = copy;
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 1); /* pop array table */
    return count;
}

int hl_manifest_extract(lua_State *L, HlManifest *out, HlAllocator *alloc)
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
        if (csp_is_valid(csp_str)) {
            out->csp = manifest_strdup(alloc, csp_str);
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
            out->cors_methods = manifest_strdup(alloc, lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, cors_idx, "headers");
        if (lua_isstring(L, -1))
            out->cors_headers = manifest_strdup(alloc, lua_tostring(L, -1));
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

/* ── QuickJS manifest extraction ──────────────────────────────────── */

#ifdef HL_ENABLE_JS

#include "quickjs.h"

/* Read a string array from a JS object property into a C array.
 * Strings are copied via manifest_strdup; JS strings are freed immediately.
 * Returns number of strings read (capped at max). */
static int read_js_string_array(JSContext *ctx, JSValueConst obj,
                                 const char *field,
                                 const char **out, int max,
                                 HlAllocator *alloc)
{
    int count = 0;
    JSValue arr = JS_GetPropertyStr(ctx, obj, field);
    if (JS_IsUndefined(arr) || !JS_IsArray(ctx, arr)) {
        JS_FreeValue(ctx, arr);
        return 0;
    }

    JSValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < len && count < max; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
        if (JS_IsString(elem)) {
            const char *s = JS_ToCString(ctx, elem);
            if (s) {
                const char *copy = manifest_strdup(alloc, s);
                JS_FreeCString(ctx, s);
                if (copy)
                    out[count++] = copy;
            }
        }
        JS_FreeValue(ctx, elem);
    }

    JS_FreeValue(ctx, arr);
    return count;
}

int hl_manifest_extract_js(JSContext *ctx, HlManifest *out, HlAllocator *alloc)
{
    if (!ctx || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    out->alloc = alloc;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue manifest = JS_GetPropertyStr(ctx, global, "__hull_manifest");
    JS_FreeValue(ctx, global);

    if (JS_IsUndefined(manifest) || JS_IsNull(manifest)) {
        JS_FreeValue(ctx, manifest);
        return -1; /* no manifest declared */
    }

    out->present = 1;

    /* fs = { read: [...], write: [...] } */
    JSValue fs = JS_GetPropertyStr(ctx, manifest, "fs");
    if (!JS_IsUndefined(fs) && !JS_IsNull(fs)) {
        out->fs_read_count = read_js_string_array(ctx, fs, "read",
                                                    out->fs_read,
                                                    HL_MANIFEST_MAX_PATHS, alloc);
        out->fs_write_count = read_js_string_array(ctx, fs, "write",
                                                     out->fs_write,
                                                     HL_MANIFEST_MAX_PATHS, alloc);
    }
    JS_FreeValue(ctx, fs);

    /* env = [...] */
    out->env_count = read_js_string_array(ctx, manifest, "env",
                                            out->env,
                                            HL_MANIFEST_MAX_ENVS, alloc);

    /* hosts = [...] */
    out->hosts_count = read_js_string_array(ctx, manifest, "hosts",
                                              out->hosts,
                                              HL_MANIFEST_MAX_HOSTS, alloc);

    /* csp = "policy-string" or false */
    JSValue csp_val = JS_GetPropertyStr(ctx, manifest, "csp");
    if (JS_IsString(csp_val)) {
        const char *csp_str = JS_ToCString(ctx, csp_val);
        if (csp_str && csp_is_valid(csp_str)) {
            out->csp = manifest_strdup(alloc, csp_str);
            out->csp_set = 1;
        }
        if (csp_str) JS_FreeCString(ctx, csp_str);
    } else if (JS_IsBool(csp_val) && !JS_ToBool(ctx, csp_val)) {
        out->csp = NULL;
        out->csp_set = 1;  /* explicitly disabled */
    }
    JS_FreeValue(ctx, csp_val);

    /* cors = { origins: [...], methods: "...", headers: "...",
     *          credentials: true, maxAge: 86400 } */
    JSValue cors_val = JS_GetPropertyStr(ctx, manifest, "cors");
    if (!JS_IsUndefined(cors_val) && !JS_IsNull(cors_val)) {
        out->cors_set = 1;
        out->cors_origin_count = read_js_string_array(ctx, cors_val, "origins",
                                                        out->cors_origins,
                                                        HL_MANIFEST_MAX_CORS_ORIGINS,
                                                        alloc);

        JSValue methods_val = JS_GetPropertyStr(ctx, cors_val, "methods");
        if (JS_IsString(methods_val)) {
            const char *s = JS_ToCString(ctx, methods_val);
            if (s) {
                out->cors_methods = manifest_strdup(alloc, s);
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, methods_val);

        JSValue headers_val = JS_GetPropertyStr(ctx, cors_val, "headers");
        if (JS_IsString(headers_val)) {
            const char *s = JS_ToCString(ctx, headers_val);
            if (s) {
                out->cors_headers = manifest_strdup(alloc, s);
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, headers_val);

        JSValue creds_val = JS_GetPropertyStr(ctx, cors_val, "credentials");
        if (JS_IsBool(creds_val))
            out->cors_credentials = JS_ToBool(ctx, creds_val);
        JS_FreeValue(ctx, creds_val);

        JSValue age_val = JS_GetPropertyStr(ctx, cors_val, "maxAge");
        if (JS_IsNumber(age_val)) {
            int32_t age = 0;
            JS_ToInt32(ctx, &age, age_val);
            out->cors_max_age = age;
        }
        JS_FreeValue(ctx, age_val);
    }
    JS_FreeValue(ctx, cors_val);

    /* wasm: { heap, stack, gas, maxInput, maxOutput } */
    JSValue wasm_val = JS_GetPropertyStr(ctx, manifest, "wasm");
    if (!JS_IsUndefined(wasm_val) && !JS_IsNull(wasm_val)) {
        JSValue v;
        int64_t iv;
        v = JS_GetPropertyStr(ctx, wasm_val, "heap");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_heap = (uint32_t)iv; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, wasm_val, "stack");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_stack = (uint32_t)iv; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, wasm_val, "gas");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_gas = iv; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, wasm_val, "maxInput");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_max_input = (uint32_t)iv; }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, wasm_val, "maxOutput");
        if (!JS_IsUndefined(v)) { JS_ToInt64(ctx, &iv, v); out->wasm_max_output = (uint32_t)iv; }
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, wasm_val);

    /* gpu: true  OR  gpu: { devices: [0, 1] } */
    JSValue gpu_val = JS_GetPropertyStr(ctx, manifest, "gpu");
    if (JS_IsObject(gpu_val) && !JS_IsNull(gpu_val)) {
        out->gpu = 1;
        JSValue devs = JS_GetPropertyStr(ctx, gpu_val, "devices");
        if (JS_IsArray(ctx, devs)) {
            JSValue len_val = JS_GetPropertyStr(ctx, devs, "length");
            int32_t len = 0;
            JS_ToInt32(ctx, &len, len_val);
            JS_FreeValue(ctx, len_val);
            for (int32_t i = 0; i < len && out->gpu_device_count < HL_GPU_MAX_DEVICES; i++) {
                JSValue elem = JS_GetPropertyUint32(ctx, devs, (uint32_t)i);
                if (JS_IsNumber(elem)) {
                    int32_t d = 0;
                    JS_ToInt32(ctx, &d, elem);
                    out->gpu_devices[out->gpu_device_count++] = d;
                }
                JS_FreeValue(ctx, elem);
            }
        }
        JS_FreeValue(ctx, devs);
    } else if (JS_IsBool(gpu_val)) {
        out->gpu = JS_ToBool(ctx, gpu_val);
    }
    JS_FreeValue(ctx, gpu_val);

    /* compute: true */
    JSValue compute_val = JS_GetPropertyStr(ctx, manifest, "compute");
    if (JS_IsBool(compute_val))
        out->compute = JS_ToBool(ctx, compute_val);
    JS_FreeValue(ctx, compute_val);

    JS_FreeValue(ctx, manifest);
    return 0;
}

#endif /* HL_ENABLE_JS */
