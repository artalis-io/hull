/*
 * manifest.c — Extract app manifest from runtime state
 *
 * Reads the __hull_manifest key (Lua registry or JS globalThis)
 * and populates an HlManifest struct with capability declarations.
 *
 * Lua: string pointers reference Lua-owned memory — valid as long
 *      as the Lua state is alive.
 * JS:  string pointers from JS_ToCString — must be freed with
 *      hl_manifest_free_js_strings() after use.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest.h"
#include "log.h"
#include <string.h>

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

#ifdef HL_ENABLE_LUA
#include "lua.h"
#include "lauxlib.h"
#endif

/* ── Lua manifest extraction ───────────────────────────────────────── */

#ifdef HL_ENABLE_LUA

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

    /* csp = "policy-string" or false */
    lua_getfield(L, manifest_idx, "csp");
    if (lua_isstring(L, -1)) {
        const char *csp_str = lua_tostring(L, -1);
        if (csp_is_valid(csp_str)) {
            out->csp = csp_str;
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
                                                     HL_MANIFEST_MAX_CORS_ORIGINS);
        lua_getfield(L, cors_idx, "methods");
        if (lua_isstring(L, -1))
            out->cors_methods = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, cors_idx, "headers");
        if (lua_isstring(L, -1))
            out->cors_headers = lua_tostring(L, -1);
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

    lua_pop(L, 1); /* pop manifest table */
    return 0;
}

#endif /* HL_ENABLE_LUA */

/* ── QuickJS manifest extraction ──────────────────────────────────── */

#ifdef HL_ENABLE_JS

#include "quickjs.h"

/* Read a string array from a JS object property into a C array.
 * Strings are allocated via JS_ToCString and must be freed later.
 * Returns number of strings read (capped at max). */
static int read_js_string_array(JSContext *ctx, JSValueConst obj,
                                 const char *field,
                                 const char **out, int max)
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
            if (s)
                out[count++] = s;
        }
        JS_FreeValue(ctx, elem);
    }

    JS_FreeValue(ctx, arr);
    return count;
}

int hl_manifest_extract_js(JSContext *ctx, HlManifest *out)
{
    if (!ctx || !out)
        return -1;

    memset(out, 0, sizeof(*out));

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
                                                    HL_MANIFEST_MAX_PATHS);
        out->fs_write_count = read_js_string_array(ctx, fs, "write",
                                                     out->fs_write,
                                                     HL_MANIFEST_MAX_PATHS);
    }
    JS_FreeValue(ctx, fs);

    /* env = [...] */
    out->env_count = read_js_string_array(ctx, manifest, "env",
                                            out->env,
                                            HL_MANIFEST_MAX_ENVS);

    /* hosts = [...] */
    out->hosts_count = read_js_string_array(ctx, manifest, "hosts",
                                              out->hosts,
                                              HL_MANIFEST_MAX_HOSTS);

    /* csp = "policy-string" or false */
    JSValue csp_val = JS_GetPropertyStr(ctx, manifest, "csp");
    if (JS_IsString(csp_val)) {
        const char *csp_str = JS_ToCString(ctx, csp_val);
        if (csp_str && csp_is_valid(csp_str)) {
            out->csp = csp_str;
            out->csp_set = 1;
        } else if (csp_str) {
            JS_FreeCString(ctx, csp_str);
            /* invalid → csp_set stays 0, falls back to default */
        }
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
                                                        HL_MANIFEST_MAX_CORS_ORIGINS);

        JSValue methods_val = JS_GetPropertyStr(ctx, cors_val, "methods");
        if (JS_IsString(methods_val))
            out->cors_methods = JS_ToCString(ctx, methods_val);
        JS_FreeValue(ctx, methods_val);

        JSValue headers_val = JS_GetPropertyStr(ctx, cors_val, "headers");
        if (JS_IsString(headers_val))
            out->cors_headers = JS_ToCString(ctx, headers_val);
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

    JS_FreeValue(ctx, manifest);
    return 0;
}

void hl_manifest_free_js_strings(JSContext *ctx, HlManifest *m)
{
    if (!ctx || !m)
        return;

    for (int i = 0; i < m->fs_read_count; i++)
        if (m->fs_read[i]) JS_FreeCString(ctx, m->fs_read[i]);
    for (int i = 0; i < m->fs_write_count; i++)
        if (m->fs_write[i]) JS_FreeCString(ctx, m->fs_write[i]);
    for (int i = 0; i < m->env_count; i++)
        if (m->env[i]) JS_FreeCString(ctx, m->env[i]);
    for (int i = 0; i < m->hosts_count; i++)
        if (m->hosts[i]) JS_FreeCString(ctx, m->hosts[i]);
    if (m->csp) JS_FreeCString(ctx, m->csp);
    for (int i = 0; i < m->cors_origin_count; i++)
        if (m->cors_origins[i]) JS_FreeCString(ctx, m->cors_origins[i]);
    if (m->cors_methods) JS_FreeCString(ctx, m->cors_methods);
    if (m->cors_headers) JS_FreeCString(ctx, m->cors_headers);
}

#endif /* HL_ENABLE_JS */
