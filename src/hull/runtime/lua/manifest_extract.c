/*
 * manifest_extract.c - read a Lua app's app.manifest({...}) in a runtime of
 * its own.
 *
 * The manifest is a call, so reading it means running the app's top level.
 * The build tools (hull build / manifest / modules / deploy / check) used to
 * run it inside the tool VM itself, with `tool` and the loaders removed for
 * the window and put back after. App code still shared that VM: a metatable
 * on _G saw `tool` come back and kept it, and replaced functions the tool
 * called next - tool.spawn and tool.write_file from an app you were only
 * building. Here the app runs in a fresh, sandboxed Lua runtime, the same
 * kind it runs in when served (no io / os / load, the heap and instruction
 * limits), which shares nothing with the tool VM; the manifest comes back as
 * JSON, as the JS extraction's does.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua.h"
#include "hull/module_registry.h"
#include "hull/utils/alloc.h"

#include "lua.h"
#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

/* ── Stubs for capabilities with no backing at build time ──────────────── */

static int noop_fn(lua_State *L) { (void)L; return 0; }
static int noop_index(lua_State *L)
{
    lua_pushcfunction(L, noop_fn);
    return 1;
}

void hl_lua_stub_unbacked_modules(lua_State *L)
{
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);

    /* One shared table whose every field is a function doing nothing. */
    lua_newtable(L);                 /* nop_table */
    lua_newtable(L);                 /* metatable */
    lua_pushcfunction(L, noop_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    int nop_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_modules");

    for (size_t i = 0; i < total; i++) {
        const char *cname = all[i].name;
        if (strncmp(cname, "hull/", 5) != 0) continue;
        char lua_name[HL_MODULE_NAME_MAX];
        size_t cname_len = strlen(cname);
        if (cname_len + 1 > sizeof(lua_name)) continue;
        memcpy(lua_name, cname, cname_len + 1);
        for (char *p = lua_name; *p; p++)
            if (*p == '/') *p = '.';

        lua_getfield(L, -2, lua_name);           /* native-registered? */
        int in_loaded = !lua_isnil(L, -1);
        lua_pop(L, 1);

        int in_hull_mods = 0;                    /* a stdlib .lua file? */
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, lua_name);
            in_hull_mods = !lua_isnil(L, -1);
            lua_pop(L, 1);
        }

        if (!in_loaded && !in_hull_mods) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, nop_ref);
            lua_setfield(L, -3, lua_name);       /* _LOADED[name] = nop_table */
        }
    }

    /* hull.db._internal_conn is not a registry module (stdlib-only, never
     * declared), so the loop skips it; the stdlib modules that keep _hull_*
     * tables require it at load through hull.db._internal. */
    lua_getfield(L, -2, "hull.db._internal_conn");
    int has_internal = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (!has_internal) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, nop_ref);
        lua_setfield(L, -3, "hull.db._internal_conn");
    }

    lua_pop(L, 2);  /* __hull_modules + _LOADED; nop_ref lives with the VM */
}

/* ── Extraction ──────────────────────────────────────────────────────── */

static char *dup_str(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* (args: manifest) -> json */
static const char ENCODE_CHUNK[] =
    "local m = ...\n"
    "return require('hull.json').encode(m)\n";

int hl_lua_extract_manifest_json(const char *path, const HlVfs *platform_vfs,
                                 char **out_json, size_t *out_len, char **out_err)
{
    if (out_json) *out_json = NULL;
    if (out_len) *out_len = 0;
    if (out_err) *out_err = NULL;
    if (!path || !out_json) return -1;

    HlLua *lua = calloc(1, sizeof *lua);
    if (!lua) {
        if (out_err) *out_err = dup_str("out of memory", 13);
        return -1;
    }
    lua->base.platform_vfs = platform_vfs;
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    if (hl_lua_init(lua, &cfg) != 0) {
        free(lua);
        if (out_err) *out_err = dup_str("Lua runtime init failed", 23);
        return -1;
    }
    lua_State *L = lua->L;
    hl_lua_stub_unbacked_modules(L);

    /* The app root, for the entry's relative requires - as hl_lua_load_app
     * sets it (freed by hl_lua_free). */
    size_t plen = strlen(path);
    const char *slash = strrchr(path, '/');
    size_t dlen = slash ? (size_t)(slash - path) : 1;
    char *app_dir = hl_alloc_malloc(lua->base.alloc, dlen + 1);
    if (app_dir) {
        if (slash) memcpy(app_dir, path, dlen);
        else app_dir[0] = '.';
        app_dir[dlen] = '\0';
        lua->app_dir = app_dir;
        lua->app_dir_size = dlen + 1;
    }
    lua_pushstring(L, path);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_current_module");

    /* Text only, under a name that cannot pass for the stdlib's ("@hull.*"). */
    char entry[4096];
    const char *load_name = path;
    if (strncmp(path, "hull.", 5) == 0 && plen + 3 <= sizeof entry) {
        entry[0] = '.'; entry[1] = '/';
        memcpy(entry + 2, path, plen + 1);
        load_name = entry;
    }

    char *run_err = NULL;
    int status = luaL_loadfilex(L, load_name, "t");
    if (status == LUA_OK)
        status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        size_t elen = 0;
        const char *e = lua_tolstring(L, -1, &elen);
        run_err = e ? dup_str(e, elen) : dup_str("app failed to load", 18);
        lua_pop(L, 1);
    }

    /* Whatever app.manifest() captured counts, even when the top level raised
     * later (a later line touching a capability that has no backing here). */
    int rc = 0;
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    if (lua_istable(L, -1)) {
        int mi = lua_gettop(L);
        if (luaL_loadbuffer(L, ENCODE_CHUNK, sizeof ENCODE_CHUNK - 1,
                            "=hull.extract") == LUA_OK) {
            lua_pushvalue(L, mi);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_type(L, -1) == LUA_TSTRING) {
                size_t jlen = 0;
                const char *j = lua_tolstring(L, -1, &jlen);
                *out_json = dup_str(j, jlen);
                if (*out_json && out_len) *out_len = jlen;
            } else {
                free(run_err);
                run_err = dup_str("app.manifest() is not serialisable", 34);
            }
        }
        lua_settop(L, mi - 1);
    } else {
        lua_pop(L, 1);
    }
    if (!*out_json && run_err)
        rc = -1;            /* no manifest, and the app did not load */

    if (out_err) *out_err = run_err;
    else free(run_err);

    hl_lua_free(lua);
    free(lua);
    return rc;
}
