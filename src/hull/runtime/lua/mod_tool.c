/*
 * runtime/lua/mod_tool.c — Lua bindings for the `tool` global
 *
 * Provides the `tool` global table used by `hull build`, `hull keygen`,
 * `hull migrate new`, `hull deploy`, and other Lua-implemented tooling.
 *
 * The pure-C tool capability (unveil table, allowlisted spawn, copy/mkdir/
 * rmdir/find_files) lives in src/hull/cap/tool.c. This file holds the
 * Lua-specific bindings and runtime-side helpers (loadfile, extract_platform,
 * compiler vtable exposure). Sources separated here as part of architectural
 * roadmap item F — restores the cap-layer "no runtime knowledge" invariant.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/tool.h"
#include "hull/runtime/tool.h"
#include "hull/agent_lib.h"
#include "hull/build_assets.h"
#include "hull/commands/doctor.h"
#include "hull/compiler.h"
#include "hull/dev_state.h"
#include "hull/manifest.h"
#include "hull/module_registry.h"
#include "hull/module_resolver.h"

#include "sh_json.h"

#include <signal.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HL_ENABLE_LUA

#include "lua.h"
#include "lauxlib.h"

/* Registry key for the unveil context pointer */
#define TOOL_UNVEIL_KEY "__hull_tool_unveil"


/* ── Lua helper: get unveil context from registry ──────────────────── */

static HlToolUnveilCtx *get_unveil_ctx(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_UNVEIL_KEY);
    HlToolUnveilCtx *ctx = (HlToolUnveilCtx *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

/* ── tool.spawn(argv_table) → (bool, int) ─────────────────────────── */

static int l_tool_spawn(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Count elements */
    int n = (int)luaL_len(L, 1);
    if (n <= 0) {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, -1);
        return 2;
    }

    /* Build argv array */
    const char **argv = malloc(((size_t)n + 1) * sizeof(const char *));
    if (!argv) {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, -1);
        return 2;
    }

    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv[i - 1] = lua_tostring(L, -1);
        if (!argv[i - 1]) {
            free(argv);
            return luaL_error(L, "tool.spawn: argument %d must be a string", i);
        }
        lua_pop(L, 1);
    }
    argv[n] = NULL;

    int rc = hl_tool_spawn(argv);
    free(argv);

    if (rc == 0) {
        lua_pushboolean(L, 1);
        lua_pushinteger(L, 0);
    } else {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, rc);
    }
    return 2;
}

/* ── tool.spawn_read(argv_table) → string | nil ──────────────────── */

static int l_tool_spawn_read(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    int n = (int)luaL_len(L, 1);
    if (n <= 0) {
        lua_pushnil(L);
        return 1;
    }

    const char **argv = malloc(((size_t)n + 1) * sizeof(const char *));
    if (!argv) {
        lua_pushnil(L);
        return 1;
    }

    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv[i - 1] = lua_tostring(L, -1);
        if (!argv[i - 1]) {
            free(argv);
            return luaL_error(L, "tool.spawn_read: argument %d must be a string", i);
        }
        lua_pop(L, 1);
    }
    argv[n] = NULL;

    size_t out_len = 0;
    char *output = hl_tool_spawn_read(argv, &out_len);
    free(argv);

    if (output) {
        lua_pushlstring(L, output, out_len);
        free(output);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* ── tool.find_files(dir, pattern) → table ────────────────────────── */

static int l_tool_find_files(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    const char *pattern = luaL_checkstring(L, 2);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    char **files = hl_tool_find_files(dir, pattern, ctx);
    if (!files) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);
    int idx = 1;
    for (char **p = files; *p; p++) {
        lua_pushstring(L, *p);
        lua_rawseti(L, -2, idx++);
        free(*p);
    }
    free(files);

    return 1;
}

/* ── tool.copy(src, dst) → bool ───────────────────────────────────── */

static int l_tool_copy(lua_State *L)
{
    const char *src = luaL_checkstring(L, 1);
    const char *dst = luaL_checkstring(L, 2);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    int rc = hl_tool_copy(src, dst, ctx);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.mkdir(path) → bool ──────────────────────────────────────── */

static int l_tool_mkdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    int rc = hl_tool_mkdir(path, ctx);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.rmdir(path) → bool ──────────────────────────────────────── */

static int l_tool_rmdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    int rc = hl_tool_rmdir(path, ctx);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.tmpdir() ─────────────────────────────────────────────────── */

static int l_tool_tmpdir(lua_State *L)
{
    char tmpl[] = "/tmp/hull_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, dir);
    return 1;
}

/* ── tool.exit(code) ───────────────────────────────────────────────── */

static int l_tool_exit(lua_State *L)
{
    int code = (int)luaL_checkinteger(L, 1);
    exit(code);
    return 0; /* unreachable */
}

/* ── tool.read_file(path) ──────────────────────────────────────────── */

static int l_tool_read_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    if (ctx && hl_tool_unveil_check(ctx, path, 'r') != 0) {
        lua_pushnil(L);
        return 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        lua_pushnil(L);
        return 1;
    }

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        luaL_addlstring(&b, buf, n);
    int read_err = ferror(f);
    fclose(f);
    if (read_err) {
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresult(&b);
    return 1;
}

/* ── tool.write_file(path, data) ───────────────────────────────────── */

static int l_tool_write_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    if (ctx && hl_tool_unveil_check(ctx, path, 'w') != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        lua_pushboolean(L, 0);
        return 1;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    lua_pushboolean(L, written == len);
    return 1;
}

/* ── tool.file_exists(path) ────────────────────────────────────────── */

static int l_tool_file_exists(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    if (ctx && hl_tool_unveil_check(ctx, path, 'r') != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, access(path, F_OK) == 0);
    return 1;
}

/* ── tool.file_mtime(path) → number|nil ────────────────────────────── */

static int l_tool_file_mtime(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    if (ctx && hl_tool_unveil_check(ctx, path, 'r') != 0) {
        lua_pushnil(L);
        return 1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        lua_pushnil(L);
        return 1;
    }
    /* Return seconds since epoch as a Lua number. */
    lua_pushnumber(L, (lua_Number)st.st_mtime);
    return 1;
}

/* ── tool.stderr(msg) ──────────────────────────────────────────────── */

static int l_tool_stderr(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    fprintf(stderr, "%s", msg);
    return 0;
}

/* ── tool.loadfile(path) ───────────────────────────────────────────── */

static int l_tool_loadfile(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int rc = luaL_loadfile(L, path);
    if (rc != LUA_OK) {
        /* Stack: error message. Return nil, errmsg. */
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1; /* chunk function on stack */
}

/* ── tool.extract_platform(dir) → bool ─────────────────────────────── */

static int l_tool_extract_platform(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    int rc = hl_build_extract_platform(dir);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.extract_platform_cosmo(dir) → bool ───────────────────────── */
/*
 * Extract both arch-specific archives and set up the .aarch64/ directory
 * layout that cosmocc expects:
 *   dir/libhull_platform.a          ← x86_64
 *   dir/.aarch64/libhull_platform.a ← aarch64
 */

static int l_tool_extract_platform_cosmo(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    const HlEmbeddedPlatform *platforms = NULL;
    int count = hl_build_get_platforms(&platforms);
    if (count < 2 || !platforms) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Extract x86_64 as dir/libhull_platform.a */
    char path[1024];
    snprintf(path, sizeof(path), "%s/libhull_platform.a", dir);
    const HlEmbeddedPlatform *x86 = NULL;
    const HlEmbeddedPlatform *arm = NULL;

    for (int i = 0; i < count; i++) {
        if (strstr(platforms[i].arch, "x86_64"))
            x86 = &platforms[i];
        else if (strstr(platforms[i].arch, "aarch64"))
            arm = &platforms[i];
    }

    if (!x86 || !arm) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Write x86_64 archive */
    FILE *f = fopen(path, "wb");
    if (!f) { lua_pushboolean(L, 0); return 1; }
    size_t w = fwrite(x86->data, 1, x86->len, f);
    fclose(f);
    if (w != x86->len) { lua_pushboolean(L, 0); return 1; }

    /* Create .aarch64/ subdir */
    char aarch64_dir[1024];
    snprintf(aarch64_dir, sizeof(aarch64_dir), "%s/.aarch64", dir);
    if (hl_tool_mkdir(aarch64_dir, ctx) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Write aarch64 archive */
    snprintf(path, sizeof(path), "%s/.aarch64/libhull_platform.a", dir);
    f = fopen(path, "wb");
    if (!f) { lua_pushboolean(L, 0); return 1; }
    w = fwrite(arm->data, 1, arm->len, f);
    fclose(f);
    if (w != arm->len) { lua_pushboolean(L, 0); return 1; }

    lua_pushboolean(L, 1);
    return 1;
}

/* ── tool.platform_archs() → table | nil ───────────────────────────── */

static int l_tool_platform_archs(lua_State *L)
{
    const HlEmbeddedPlatform *platforms = NULL;
    int count = hl_build_get_platforms(&platforms);
    if (count == 0 || !platforms) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        lua_pushstring(L, platforms[i].arch);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ── Compiler vtable Lua bindings ──────────────────────────────── */

#define TOOL_COMPILER_KEY "__hull_compiler"

static int l_compiler_name(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_COMPILER_KEY);
    HlCompiler *c = (HlCompiler *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!c) { lua_pushnil(L); return 1; }
    lua_pushstring(L, hl_compiler_name(c));
    return 1;
}

static int l_compiler_is_available(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_COMPILER_KEY);
    HlCompiler *c = (HlCompiler *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_pushboolean(L, c && hl_compiler_is_available(c));
    return 1;
}

static int l_compiler_version(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_COMPILER_KEY);
    HlCompiler *c = (HlCompiler *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!c) { lua_pushnil(L); return 1; }
    char *v = hl_compiler_version(c);
    if (v) { lua_pushstring(L, v); free(v); }
    else   { lua_pushnil(L); }
    return 1;
}

static int l_compiler_compile(lua_State *L) {
    const char *src = luaL_checkstring(L, 1);
    const char *obj = luaL_checkstring(L, 2);
    const char *inc = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_COMPILER_KEY);
    HlCompiler *c = (HlCompiler *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!c) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, hl_compiler_compile(c, src, obj, inc) == 0);
    return 1;
}

static int l_compiler_link(lua_State *L) {
    const char *output = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_COMPILER_KEY);
    HlCompiler *c = (HlCompiler *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!c) { lua_pushboolean(L, 0); return 1; }

    int nobj = (int)luaL_len(L, 2);
    int nlib = (int)luaL_len(L, 3);
    const char **objs = (const char **)malloc(((size_t)nobj + 1) * sizeof(char *));
    const char **libs = (const char **)malloc(((size_t)nlib + 1) * sizeof(char *));
    if (!objs || !libs) {
        free(objs); free(libs);
        lua_pushboolean(L, 0); return 1;
    }
    for (int i = 1; i <= nobj; i++) {
        lua_rawgeti(L, 2, i);
        objs[i - 1] = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    for (int i = 1; i <= nlib; i++) {
        lua_rawgeti(L, 3, i);
        libs[i - 1] = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    objs[nobj] = NULL;
    libs[nlib] = NULL;

    int rc = hl_compiler_link(c, output, objs, libs);
    free(objs); free(libs);
    lua_pushboolean(L, rc == 0);
    return 1;
}

void hl_lua_tool_expose_compiler(lua_State *L, HlCompiler *compiler)
{
    if (!compiler || !L) return;

    lua_pushlightuserdata(L, compiler);
    lua_setfield(L, LUA_REGISTRYINDEX, TOOL_COMPILER_KEY);

    lua_getglobal(L, "tool");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    lua_newtable(L);
    lua_pushcfunction(L, l_compiler_name);         lua_setfield(L, -2, "name");
    lua_pushcfunction(L, l_compiler_is_available);  lua_setfield(L, -2, "is_available");
    lua_pushcfunction(L, l_compiler_version);       lua_setfield(L, -2, "version");
    lua_pushcfunction(L, l_compiler_compile);       lua_setfield(L, -2, "compile");
    lua_pushcfunction(L, l_compiler_link);          lua_setfield(L, -2, "link");
    lua_setfield(L, -2, "compiler");
    lua_pop(L, 1);
}

/* ── Module-system resolver binding ────────────────────────────────── */
/*
 * tool.modules_resolve(manifest_table) → { ok = bool, modules = {...} }
 *                                       | { ok = false, error = "..." }
 *
 * Runs the canonical resolver in C against a Lua-side manifest table
 * (the same shape returned by app.get_manifest()). Returns the
 * admitted set as a sorted array of { name = "hull/foo", api_major = 1 }
 * entries — intrinsics included. Used by `hull build` to persist the
 * resolved set into package.sig, and by anything that needs to know
 * "given this manifest, what will the runtime actually admit".
 *
 * Mirrors HlManifest field-by-field by walking the Lua table:
 *   modules = { name = "ver" }
 *   fs      = { read = {...}, write = {...} }  (for cap presence)
 *   env     = {...}                              (for cap presence)
 *   hosts   = {...}                              (for cap presence)
 *
 * The manifest stub is allocated on the C stack with no Hull allocator
 * — names are read directly off the Lua stack and copied via strdup
 * only for the time of the resolver call, then freed.
 */
static int l_tool_modules_resolve(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    HlManifest m = {0};

    /* fs.read / fs.write — only the counts matter for the resolver. */
    lua_getfield(L, 1, "fs");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "read");
        if (lua_istable(L, -1)) {
            lua_Integer n = luaL_len(L, -1);
            m.fs_read_count = n > HL_MANIFEST_MAX_PATHS
                              ? HL_MANIFEST_MAX_PATHS : (int)n;
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "write");
        if (lua_istable(L, -1)) {
            lua_Integer n = luaL_len(L, -1);
            m.fs_write_count = n > HL_MANIFEST_MAX_PATHS
                               ? HL_MANIFEST_MAX_PATHS : (int)n;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    /* env */
    lua_getfield(L, 1, "env");
    if (lua_istable(L, -1)) {
        lua_Integer n = luaL_len(L, -1);
        m.env_count = n > HL_MANIFEST_MAX_ENVS ? HL_MANIFEST_MAX_ENVS : (int)n;
    }
    lua_pop(L, 1);

    /* hosts */
    lua_getfield(L, 1, "hosts");
    if (lua_istable(L, -1)) {
        lua_Integer n = luaL_len(L, -1);
        m.hosts_count = n > HL_MANIFEST_MAX_HOSTS ? HL_MANIFEST_MAX_HOSTS : (int)n;
    }
    lua_pop(L, 1);

    /* modules — copy names so the resolver sees stable pointers. */
    char *owned_names[HL_MANIFEST_MAX_MODULES] = {0};
    int owned_count = 0;
    lua_getfield(L, 1, "modules");
    if (lua_istable(L, -1)) {
        m.modules_declared = 1;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0 && m.modules_count < HL_MANIFEST_MAX_MODULES) {
            if (lua_type(L, -2) == LUA_TSTRING &&
                lua_type(L, -1) == LUA_TSTRING) {
                const char *name = lua_tostring(L, -2);
                const char *vstr = lua_tostring(L, -1);
                char *end = NULL;
                long v = strtol(vstr, &end, 10);
                if (end != vstr && v >= 1 && v <= 255) {
                    char *copy = strdup(name);
                    if (copy) {
                        owned_names[owned_count] = copy;
                        m.modules[m.modules_count].name = copy;
                        m.modules[m.modules_count].api_major = (uint8_t)v;
                        m.modules_count++;
                        owned_count++;
                    }
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    /* Resolve. */
    HlResolvedModuleSet set;
    char err[HL_MODULE_RESOLVER_ERR_MAX] = {0};
    int rc = hl_module_resolver_resolve(&m, &set, err, sizeof(err));

    /* Free owned name copies. */
    for (int i = 0; i < owned_count; i++) free(owned_names[i]);

    /* Build result table. */
    lua_newtable(L);
    if (rc != 0) {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        lua_pushstring(L, err);
        lua_setfield(L, -2, "error");
        return 1;
    }

    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "ok");

    lua_newtable(L);  /* modules array */
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);
    int idx = 1;
    for (size_t i = 0; i < total; i++) {
        if (!hl_module_set_contains_index(&set, (int)i)) continue;
        lua_newtable(L);
        lua_pushstring(L, all[i].name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, all[i].api_major);
        lua_setfield(L, -2, "api_major");
        lua_pushboolean(L, all[i].intrinsic ? 1 : 0);
        lua_setfield(L, -2, "intrinsic");
        lua_rawseti(L, -2, idx++);
    }
    lua_setfield(L, -2, "modules");

    return 1;
}

/* ── tool.doctor_json() ───────────────────────────────────────────── */

/* Render `hull doctor --json` payload into a heap buffer and return
 * it as a Lua string. Consumed by stdlib/lua/hull/doctor_tui.lua
 * which parses with json.decode and renders interactively via the
 * hull.tui module.
 *
 * Wrapping fmemopen avoids reimplementing all of doctor.c's print_json
 * in Lua; the JSON text is already the canonical machine-readable
 * shape. */
static int l_tool_doctor_json(lua_State *L)
{
    char *buf = NULL;
    size_t sz = 0;
    FILE *f = open_memstream(&buf, &sz);
    if (!f) return luaL_error(L, "doctor_json: open_memstream failed");
    hl_doctor_collect_json(f);
    fflush(f);
    fclose(f);
    if (!buf) return luaL_error(L, "doctor_json: empty");
    lua_pushlstring(L, buf, sz);
    free(buf);
    return 1;
}

/* ── tool.agent_errors(app_dir) / tool.agent_context(task, level) ── */

/* Helper: run an agent JSON-producing function into a Lua string.
 * The agent functions accept an app_dir + return JSON in a ShJsonBuf.
 * We push two values: the JSON string and the integer return code.
 *
 * Bindings live here (not in a dedicated mod_agent.c) because the
 * tool-mode VM is what hull's own TUI tool modules run in — same
 * shape as tool.doctor_json. Apps don't get this surface; the
 * resolver gates `hull/agent@*` separately. */
static int l_tool_agent_errors(lua_State *L)
{
    const char *app_dir = luaL_optstring(L, 1, ".");
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_errors(app_dir, &out);
    if (out.buf) lua_pushlstring(L, out.buf, out.len);
    else         lua_pushnil(L);
    sh_json_buf_free(&out);
    lua_pushinteger(L, rc);
    return 2;
}

static int l_tool_agent_context(lua_State *L)
{
    const char *task  = luaL_checkstring(L, 1);
    const char *level = luaL_optstring(L, 2, "compact");
    ShJsonBuf out;
    sh_json_buf_init(&out);
    int rc = hl_agent_context(task, level, &out);
    if (out.buf) lua_pushlstring(L, out.buf, out.len);
    else         lua_pushnil(L);
    sh_json_buf_free(&out);
    lua_pushinteger(L, rc);
    return 2;
}

#ifdef HL_ENABLE_DB
/* ── tool.migrate_status — applied/pending migration list ─────────
 *
 * Opens the database via the sqlite backend, queries
 * _hull_migrations, returns an array of {name, applied, applied_at}
 * entries (sorted by filename like hl_migrate_status does).
 *
 * Returns nil + an error message on open/query failure, so the
 * stdlib's migrate_status_tui can render the failure mode. */

#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/migrate.h"
#include "hull/vfs.h"

extern const HlEntry hl_app_entries[];
extern const HlDbBackend hl_db_backend_sqlite;

static int l_tool_migrate_status(lua_State *L)
{
    const char *app_dir = luaL_optstring(L, 1, ".");
    const char *db_path = luaL_optstring(L, 2, "data.db");

    HlDbHandle handle = { .backend = &hl_db_backend_sqlite, .ctx = NULL };
    if (hl_db_backend_sqlite.open(&handle.ctx, db_path, NULL) != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open database: %s", db_path);
        return 2;
    }

    HlVfs vfs;
    hl_vfs_init(&vfs, hl_app_entries, app_dir);

    HlMigrationStatus *entries = NULL;
    int count = 0;
    int rc = hl_migrate_status(&handle, &vfs, &entries, &count);
    hl_db_backend_sqlite.close(handle.ctx);

    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "migrate_status query failed");
        return 2;
    }

    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        lua_newtable(L);
        lua_pushstring(L, entries[i].name);    lua_setfield(L, -2, "name");
        lua_pushboolean(L, entries[i].applied); lua_setfield(L, -2, "applied");
        if (entries[i].applied_at) {
            lua_pushstring(L, entries[i].applied_at);
            lua_setfield(L, -2, "applied_at");
        }
        lua_rawseti(L, -2, i + 1);
    }
    hl_migrate_status_free(entries, count);
    lua_pushinteger(L, count);
    return 2;
}
#endif

/* ── tool.modules_available — every first-party registry entry ──── */

/* Returns a table { count = N, modules = { {name, api_major, intrinsic,
 * pure, deps = {...}, caps = {...} }, ... } }. The Lua dev_tui /
 * modules_available_tui modules render from it directly; no JSON
 * intermediate. */
static int l_tool_modules_available(lua_State *L)
{
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)total);
    lua_setfield(L, -2, "count");

    lua_newtable(L);  /* modules array */
    for (size_t i = 0; i < total; i++) {
        const HlModuleSpec *s = &all[i];
        lua_newtable(L);

        lua_pushstring(L, s->name);                 lua_setfield(L, -2, "name");
        lua_pushinteger(L, s->api_major);            lua_setfield(L, -2, "api_major");
        lua_pushboolean(L, s->intrinsic);            lua_setfield(L, -2, "intrinsic");
        lua_pushboolean(L, s->pure);                 lua_setfield(L, -2, "pure");

        /* deps array */
        lua_newtable(L);
        int di = 1;
        for (int j = 0; j < HL_MODULE_MAX_DEPS && s->deps[j]; j++) {
            lua_pushstring(L, s->deps[j]);
            lua_rawseti(L, -2, di++);
        }
        lua_setfield(L, -2, "deps");

        /* caps array — turn the bitmask back into stable names */
        lua_newtable(L);
        int ci = 1;
        uint32_t need = s->required_caps;
        struct { uint32_t bit; const char *name; } map[] = {
            { HL_MOD_CAP_FS,          "fs"          },
            { HL_MOD_CAP_HOSTS,       "hosts"       },
            { HL_MOD_CAP_ENV,         "env"         },
            { HL_MOD_CAP_DB,          "db"          },
            { HL_MOD_CAP_WASM,        "wasm"        },
            { HL_MOD_CAP_GPU,         "gpu"         },
            { HL_MOD_CAP_HTTP_CLIENT, "http_client" },
            { HL_MOD_CAP_HTTP_SERVER, "http_server" },
            { HL_MOD_CAP_TUI,         "tui"         },
        };
        for (size_t k = 0; k < sizeof map / sizeof map[0]; k++) {
            if (need & map[k].bit) {
                lua_pushstring(L, map[k].name);
                lua_rawseti(L, -2, ci++);
            }
        }
        lua_setfield(L, -2, "caps");

        lua_rawseti(L, -2, (int)(i + 1));
    }
    lua_setfield(L, -2, "modules");
    return 1;
}

/* ── tool.dev_* — hull dev --tui bindings ─────────────────────────── */

/* These accessors all consult hl_dev_state(); they're a no-op (returns
 * nil / safe defaults) when hull dev --tui isn't running. That lets
 * the stdlib module load cleanly outside the dev context (e.g. tests
 * that just require the file). */

static int l_tool_dev_status(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    if (!s) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)s->child_pid);  lua_setfield(L, -2, "pid");
    lua_pushinteger(L, s->reload_count);            lua_setfield(L, -2, "reload_count");
    lua_pushinteger(L, (lua_Integer)s->last_reload_ms); lua_setfield(L, -2, "last_reload_ms");
    lua_pushinteger(L, s->log_count);               lua_setfield(L, -2, "log_count");
    lua_pushstring(L,  s->app_dir);                 lua_setfield(L, -2, "app_dir");

    /* Liveness: non-blocking waitpid. */
    int alive = 0;
    if (s->child_pid > 0) {
        int status;
        pid_t r = waitpid(s->child_pid, &status, WNOHANG);
        if (r == 0) alive = 1;
        else if (r == s->child_pid) {
            /* Child exited — mark dead so the TUI shows it. */
            s->child_pid = 0;
        }
    }
    lua_pushboolean(L, alive); lua_setfield(L, -2, "alive");
    return 1;
}

/* tool.dev_drain() — pull whatever's currently on the pipe into the
 * ring buffer. Returns the number of new lines appended (informational). */
static int l_tool_dev_drain(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    if (!s) { lua_pushinteger(L, 0); return 1; }
    int before = s->log_count;
    hl_dev_state_drain(s);
    lua_pushinteger(L, s->log_count - before);
    return 1;
}

/* tool.dev_recent_lines(n) — newest-first array of the most recent
 * up-to-n lines from the ring buffer. */
static int l_tool_dev_recent_lines(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    int n = (int)luaL_optinteger(L, 1, 50);
    if (n < 1) n = 1;
    if (n > HL_DEV_LOG_LINES) n = HL_DEV_LOG_LINES;

    lua_newtable(L);
    if (!s || s->log_count == 0) return 1;

    int available = s->log_count < HL_DEV_LOG_LINES ? s->log_count : HL_DEV_LOG_LINES;
    if (n > available) n = available;

    /* Walk back from head-1 for n entries. */
    for (int i = 0; i < n; i++) {
        int idx = (s->log_head - 1 - i + HL_DEV_LOG_LINES) % HL_DEV_LOG_LINES;
        lua_pushstring(L, s->log_lines[idx]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* tool.dev_check_file_change() — returns true if app files changed
 * since the last reload baseline. */
static int l_tool_dev_check_file_change(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    lua_pushboolean(L, s ? hl_dev_state_check_file_change(s) : 0);
    return 1;
}

/* tool.dev_reload() — kill+respawn child. Returns true on success. */
static int l_tool_dev_reload(lua_State *L)
{
    HlDevState *s = hl_dev_state();
    if (!s) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, hl_dev_state_reload(s) == 0);
    return 1;
}

/* ── Registration ──────────────────────────────────────────────────── */

static const luaL_Reg tool_funcs[] = {
    { "spawn",                  l_tool_spawn },
    { "spawn_read",             l_tool_spawn_read },
    { "find_files",             l_tool_find_files },
    { "copy",                   l_tool_copy },
    { "mkdir",                  l_tool_mkdir },
    { "rmdir",                  l_tool_rmdir },
    { "tmpdir",                 l_tool_tmpdir },
    { "exit",                   l_tool_exit },
    { "read_file",              l_tool_read_file },
    { "write_file",             l_tool_write_file },
    { "file_exists",            l_tool_file_exists },
    { "file_mtime",             l_tool_file_mtime },
    { "stderr",                 l_tool_stderr },
    { "loadfile",               l_tool_loadfile },
    { "extract_platform",       l_tool_extract_platform },
    { "extract_platform_cosmo", l_tool_extract_platform_cosmo },
    { "platform_archs",         l_tool_platform_archs },
    { "modules_resolve",        l_tool_modules_resolve },
    { "doctor_json",            l_tool_doctor_json },
    { "agent_errors",           l_tool_agent_errors },
    { "agent_context",          l_tool_agent_context },
    { "dev_status",             l_tool_dev_status },
    { "dev_drain",              l_tool_dev_drain },
    { "dev_recent_lines",       l_tool_dev_recent_lines },
    { "dev_check_file_change",  l_tool_dev_check_file_change },
    { "dev_reload",             l_tool_dev_reload },
    { "modules_available",      l_tool_modules_available },
#ifdef HL_ENABLE_DB
    { "migrate_status",         l_tool_migrate_status },
#endif
    { NULL, NULL }
};

void hl_lua_tool_register(lua_State *L, HlToolUnveilCtx *ctx)
{
    /* Store unveil context in registry */
    if (ctx)
        lua_pushlightuserdata(L, ctx);
    else
        lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, TOOL_UNVEIL_KEY);

    /* Register tool table */
    luaL_newlib(L, tool_funcs);
    lua_setglobal(L, "tool");
}

#endif /* HL_ENABLE_LUA */
