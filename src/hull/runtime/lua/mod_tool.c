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
#include "hull/build_assets.h"
#include "hull/compiler.h"

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
