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
#include <string.h>

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
}

#endif /* HL_ENABLE_JS */
