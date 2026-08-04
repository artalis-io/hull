/*
 * runtime/lua/mod_tool.c — Lua bindings for the `tool` global
 *
 * Thin Lua surface over the pure-C tool capability (src/hull/cap/tool.c):
 * spawn / spawn_read / find_files / copy / mkdir / rmdir / tmpdir /
 * exit / read_file / write_file / file_exists / file_mtime / stderr /
 * loadfile / extract_platform[_cosmo] / platform_archs, plus the
 * compiler vtable exposure (tool.compiler.*).
 *
 * Orchestration entries that pull cross-layer dependencies — the
 * resolver, doctor JSON renderer, agent JSON producers, dev-state
 * accessors, migration introspection — used to live here too. They
 * moved to src/hull/tool_orchestration.c per architectural audit A-1
 * so this file stays a binding layer and runtime/lua/ doesn't pull
 * commands/, dev_state, agent_lib, etc. into its include set.
 * `hl_lua_tool_register_orchestration` splices them onto the same
 * `tool` global after `hl_lua_tool_register` has installed the base
 * table; both calls happen in runtime/lua/runtime.c during init.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/tool.h"
#include "hull/shared/cache_dir.h"
#include "hull/shared/cache_registry.h"
#include "hull/shared/blob_store.h"
#include "hull/runtime/tool.h"
#include "hull/build_assets.h"
#include "hull/compiler.h"
#include "hull/obj_emit.h"
#include "hull/linker.h"
#include "hull/bundled_objs.h"
#include "hull/tools_install.h"
#include "hull/embedded_platform_sig.h"
#include "hull/platform_sig.h"
#include "hull/release_io.h"
#include "hull/cap/crypto.h"
#include "hull/signature.h"  /* HL_PLATFORM_PUBKEY_HEX */
#include "hull/manifest_extract_file.h"  /* extract_manifest_js helper */

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

    /* Optional arg 2: an env table { KEY = VALUE, ... } applied in the child
     * before exec (e.g. ZIG_GLOBAL_CACHE_DIR for a sandbox-writable zig cache).
     * Built into a NULL-terminated array of malloc'd "KEY=VALUE" strings. */
    const char **envadd = NULL;
    int envn = 0;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        int cap = 0;
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) { cap++; lua_pop(L, 1); }
        envadd = malloc(((size_t)cap + 1) * sizeof(const char *));
        if (!envadd) { free(argv); return luaL_error(L, "tool.spawn: oom"); }
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            /* Only string KEYS: never call lua_tostring on the key during a
             * lua_next walk (it converts a numeric key in place and can then
             * corrupt the traversal - a documented Lua footgun). An env-var name
             * is always a string anyway; a non-string key is skipped. */
            const char *k = (lua_type(L, -2) == LUA_TSTRING) ? lua_tostring(L, -2) : NULL;
            const char *v = lua_tostring(L, -1);
            if (k && v) {
                size_t len = strlen(k) + 1 + strlen(v) + 1;
                char *kv = malloc(len);
                if (kv) { snprintf(kv, len, "%s=%s", k, v); envadd[envn++] = kv; }
            }
            lua_pop(L, 1);
        }
        envadd[envn] = NULL;
    }

    int rc = envadd ? hl_tool_spawn_env(argv, envadd) : hl_tool_spawn(argv);
    free(argv);
    if (envadd) {
        for (int i = 0; i < envn; i++) free((void *)(uintptr_t)envadd[i]);
        free(envadd);
    }

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

/* ── tool.find_files(dir, pattern, opts?) → table ─────────────────
 * opts.include_vendor (bool, default false): walk into directories
 * named "vendor". Useful for `static/vendor/*` asset enumeration;
 * source walks should leave it at the default. node_modules and
 * dotfiles are still always skipped. */

static int l_tool_find_files(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    const char *pattern = luaL_checkstring(L, 2);
    int include_vendor = 0;
    if (lua_type(L, 3) == LUA_TTABLE) {
        lua_getfield(L, 3, "include_vendor");
        include_vendor = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    char **files = hl_tool_find_files_ex(dir, pattern, ctx, include_vendor);
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

/* ── tool.rename(src, dst) → bool ──────────────────────────────────
 *
 * POSIX-atomic rename. Used by the AOT cache (and any other
 * content-addressed writer) to publish a tmp file under the final
 * cache name without observers ever seeing a partially-written
 * artifact. Both paths get unveil-checked: 'r' on the source, 'w'
 * on the destination, so the call respects the tool-mode sandbox.
 */
static int l_tool_rename(lua_State *L)
{
    const char *src = luaL_checkstring(L, 1);
    const char *dst = luaL_checkstring(L, 2);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);
    if (ctx) {
        if (hl_tool_unveil_check(ctx, src, 'r') != 0 ||
            hl_tool_unveil_check(ctx, dst, 'w') != 0) {
            lua_pushboolean(L, 0);
            return 1;
        }
    }
    lua_pushboolean(L, rename(src, dst) == 0);
    return 1;
}

/* ── tool.remove_file(path) → bool ─────────────────────────────────
 *
 * Best-effort unlink, gated by tool-mode unveil ('w'). Returns true
 * if the file is gone after the call (either we unlinked it or it
 * was already absent), false otherwise. Used to clean up tmp files
 * left over from cache writes.
 */
static int l_tool_remove_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);
    if (ctx && hl_tool_unveil_check(ctx, path, 'w') != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    int rc = unlink(path);
    if (rc == 0 || errno == ENOENT) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
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
    /* On a cosmo hull on Windows, ensure the shared TMPDIR is set BEFORE
     * mkdtemp, and create the build tempdir under it - so hull's inputs
     * (app_registry.c, ...) land where cosmocc, driven later through busybox,
     * resolves them (cosmo maps `/tmp` via $TMPDIR). No-op / `/tmp` elsewhere. */
    const char *base = "/tmp";
#ifdef __COSMOPOLITAN__
    char cosmo_td[512];
    hl_tool_cosmo_prepare_tmpdir();
    if (hl_tool_cosmo_tmpdir(cosmo_td, sizeof(cosmo_td)) == 0)
        base = cosmo_td;   /* forward-slash shared dir on cosmo/Windows */
#endif
    char tmpl[600];
    int n = snprintf(tmpl, sizeof(tmpl), "%s/hull_XXXXXX", base);
    if (n < 0 || (size_t)n >= sizeof(tmpl)) {
        lua_pushnil(L);
        return 1;
    }
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

/* ── tool.extract_manifest_js(path) → JSON string | nil ────────────── *
 *
 * Used by build.lua / manifest.lua / inspect.lua to read
 * app.manifest({...}) out of a JS entry point. Thin Lua wrapper around
 * hl_manifest_extract_js_from_file (which owns the transient HlJS
 * lifecycle so runtime/lua doesn't need to include runtime/js.h).
 *
 * Returns nil if the app didn't declare a manifest. Raises a Lua error
 * if hull was built without HL_ENABLE_JS, or if the runtime / load
 * fails. */
static int l_tool_extract_manifest_js(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    char  *json = NULL;
    size_t json_len = 0;
    char  *err  = NULL;

    int rc = hl_manifest_extract_js_from_file(path, &json, &json_len, &err);
    if (rc != 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "tool.extract_manifest_js: %s",
                 err ? err : "unknown error");
        free(err);
        return luaL_error(L, "%s", buf);
    }
    free(err); /* defensive — should be NULL on success */

    if (!json) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, json, json_len);
    free(json);
    return 1;
}

/* ── tool.extract_platform(dir) → bool ─────────────────────────────── */

static int l_tool_extract_platform(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    int rc = hl_build_extract_platform(dir);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.extract_feature_runtime(dir, rt) → bool ──────────────────── */

static int l_tool_extract_feature_runtime(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    const char *rt  = luaL_checkstring(L, 2);
    int rc = hl_build_extract_feature_runtime(dir, rt);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.extract_feature_http(dir) → bool ─────────────────────────── */

static int l_tool_extract_feature_http(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    int rc = hl_build_extract_feature_http(dir);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.extract_feature_http_rt(dir, rt) → bool ──────────────────── */

static int l_tool_extract_feature_http_rt(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    const char *rt  = luaL_checkstring(L, 2);
    int rc = hl_build_extract_feature_http_rt(dir, rt);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.extract_feature_tui_rt(dir, rt) → bool ───────────────────── */

static int l_tool_extract_feature_tui_rt(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    const char *rt  = luaL_checkstring(L, 2);
    int rc = hl_build_extract_feature_tui_rt(dir, rt);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.extract_feature_wasm(dir) / _wasm_rt(dir, rt) → bool ──────── */

static int l_tool_extract_feature_wasm(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    int rc = hl_build_extract_feature_wasm(dir);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int l_tool_extract_feature_wasm_rt(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    const char *rt  = luaL_checkstring(L, 2);
    int rc = hl_build_extract_feature_wasm_rt(dir, rt);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int l_tool_extract_feature_sqlite_rt(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    const char *rt  = luaL_checkstring(L, 2);
    int rc = hl_build_extract_feature_sqlite_rt(dir, rt);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int l_tool_extract_feature_sqlite(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    int rc = hl_build_extract_feature_sqlite(dir);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int l_tool_extract_feature_tls(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    int rc = hl_build_extract_feature_tls(dir);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int l_tool_extract_feature_keel(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    int rc = hl_build_extract_feature_keel(dir);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.extract_feature_image(dir) / _image_rt(dir, rt) → bool ────── */

static int l_tool_extract_feature_image(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    int rc = hl_build_extract_feature_image(dir);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int l_tool_extract_feature_image_rt(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    const char *rt  = luaL_checkstring(L, 2);
    int rc = hl_build_extract_feature_image_rt(dir, rt);
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

/*
 * tool.emit_app_registry(entries, format, arch [, elf_osabi, elf_flags]) → string|nil
 *
 * Serialize hl_app_entries[] directly as a relocatable object (the
 * compiler-free build path). entries is an array of { name = "...",
 * data = "<bytes>" }; format ∈ "elf"|"macho"|"coff"; arch ∈
 * "x86_64"|"aarch64". Returns the object bytes as a Lua string (caller
 * writes them out), or nil + message on error. See docs/compiler_free_build.md.
 */
static int l_emit_app_registry(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *fmt  = luaL_checkstring(L, 2);
    const char *arch = luaL_checkstring(L, 3);
    int osabi = (int)luaL_optinteger(L, 4, 0);
    int flags = (int)luaL_optinteger(L, 5, 0);

    HlObjTarget tgt;
    if      (strcmp(fmt, "elf")   == 0) tgt.format = HL_OBJ_ELF;
    else if (strcmp(fmt, "macho") == 0) tgt.format = HL_OBJ_MACHO;
    else if (strcmp(fmt, "coff")  == 0) tgt.format = HL_OBJ_COFF;
    else { lua_pushnil(L); lua_pushfstring(L, "unknown object format '%s'", fmt); return 2; }
    if      (strcmp(arch, "x86_64")  == 0) tgt.arch = HL_OBJ_X86_64;
    else if (strcmp(arch, "aarch64") == 0) tgt.arch = HL_OBJ_AARCH64;
    else { lua_pushnil(L); lua_pushfstring(L, "unknown object arch '%s'", arch); return 2; }
    tgt.elf_osabi = (unsigned char)osabi;
    tgt.elf_flags = (unsigned int)flags;

    size_t n = (size_t)luaL_len(L, 1);
    HlEmitEntry *ents = NULL;
    if (n) {
        ents = (HlEmitEntry *)calloc(n, sizeof(*ents));
        if (!ents) { lua_pushnil(L); lua_pushstring(L, "out of memory"); return 2; }
    }
    /* Borrow name/data pointers from the Lua strings on the stack; they stay
     * valid until we pop, which is after the emit call completes. */
    for (size_t i = 0; i < n; i++) {
        lua_rawgeti(L, 1, (lua_Integer)(i + 1));      /* entry table */
        lua_getfield(L, -1, "name");
        lua_getfield(L, -2, "data");
        size_t dlen = 0;
        const char *name = lua_tostring(L, -2);
        const char *data = lua_tolstring(L, -1, &dlen);
        if (!name) {
            free(ents);
            lua_pushnil(L); lua_pushfstring(L, "entry %d missing name", (int)(i + 1));
            return 2;
        }
        ents[i].name = name;
        ents[i].data = (const unsigned char *)(data ? data : "");
        ents[i].len  = (unsigned int)dlen;
        /* Keep name+data+entry on the stack so the pointers stay alive. */
    }

    unsigned char *obj = NULL; size_t objlen = 0;
    int rc = hl_obj_emit_app_registry(&tgt, ents, n, &obj, &objlen);
    free(ents);
    if (rc != 0 || !obj) {
        lua_pushnil(L); lua_pushstring(L, "object emit failed"); return 2;
    }
    lua_pushlstring(L, (const char *)obj, objlen);
    free(obj);
    return 1;
}

/*
 * tool.bundled_object(kind) → string|nil
 *
 * Return the bytes of a pre-compiled bundled object embedded in this hull
 * (kind ∈ "app_main" | "app_feature_registry_lua" | "app_feature_registry_js"),
 * or nil when this build embedded none. Used by the --no-compiler build path.
 */
static int l_tool_bundled_object(lua_State *L) {
    const char *kind = luaL_checkstring(L, 1);
    const unsigned char *data = NULL; size_t len = 0;
    if (hl_bundled_object(kind, &data, &len) != 0) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char *)data, len);
    return 1;
}

/* ── tool.linker.* (the link-only vtable) ──────────────────────────── */
#define TOOL_LINKER_KEY "__hull_linker"

static int l_linker_name(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_LINKER_KEY);
    HlLinker *l = (HlLinker *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!l) { lua_pushnil(L); return 1; }
    lua_pushstring(L, hl_linker_name(l));
    return 1;
}

static int l_linker_is_available(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_LINKER_KEY);
    HlLinker *l = (HlLinker *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_pushboolean(L, l && hl_linker_is_available(l));
    return 1;
}

static int l_linker_link(lua_State *L) {
    const char *output = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_LINKER_KEY);
    HlLinker *l = (HlLinker *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!l) { lua_pushboolean(L, 0); return 1; }

    int nobj = (int)luaL_len(L, 2);
    int nlib = (int)luaL_len(L, 3);
    const char **objs = (const char **)malloc(((size_t)nobj + 1) * sizeof(char *));
    const char **libs = (const char **)malloc(((size_t)nlib + 1) * sizeof(char *));
    if (!objs || !libs) { free(objs); free(libs); lua_pushboolean(L, 0); return 1; }
    for (int i = 1; i <= nobj; i++) { lua_rawgeti(L, 2, i); objs[i - 1] = lua_tostring(L, -1); lua_pop(L, 1); }
    for (int i = 1; i <= nlib; i++) { lua_rawgeti(L, 3, i); libs[i - 1] = lua_tostring(L, -1); lua_pop(L, 1); }
    objs[nobj] = NULL; libs[nlib] = NULL;

    /* Optional 4th arg: cross target triple ("x86_64-linux-gnu"); 5th: target
     * format ("elf"/"macho"/"coff"). Both feed the zig backend (--target= +
     * the format-correct GC flag). Strings borrowed from the Lua stack, valid
     * across the link call. A NULL tgt (no triple + no fmt) = native default. */
    const char *triple = lua_tostring(L, 4);
    const char *fmts   = lua_tostring(L, 5);
    HlObjFormat fmt = HL_OBJ_ELF;
    if (fmts) {
        if      (strcmp(fmts, "macho") == 0) fmt = HL_OBJ_MACHO;
        else if (strcmp(fmts, "coff")  == 0) fmt = HL_OBJ_COFF;
    }
    HlLinkTarget tgt = { fmt, triple };
    int rc = hl_linker_link(l, output, objs, libs,
                            ((triple && *triple) || fmts) ? &tgt : NULL);
    free(objs); free(libs);
    lua_pushboolean(L, rc == 0);
    return 1;
}

void hl_lua_tool_expose_linker(lua_State *L, HlLinker *linker)
{
    if (!linker || !L) return;

    lua_pushlightuserdata(L, linker);
    lua_setfield(L, LUA_REGISTRYINDEX, TOOL_LINKER_KEY);

    lua_getglobal(L, "tool");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    lua_newtable(L);
    lua_pushcfunction(L, l_linker_name);          lua_setfield(L, -2, "name");
    lua_pushcfunction(L, l_linker_is_available);  lua_setfield(L, -2, "is_available");
    lua_pushcfunction(L, l_linker_link);          lua_setfield(L, -2, "link");
    lua_setfield(L, -2, "linker");
    lua_pop(L, 1);
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
    lua_setfield(L, -2, "compiler");
    lua_pop(L, 1);
}


/* ── tool.find_tool(name) → path|nil ──────────────────────────────
 *
 * Resolve a Hull-managed tool's executable path using the same
 * 4-step lookup order as `hl_tools_lookup_path()`:
 *   1. $HOME/.hull/tools/<name>
 *   2. dirname(hull_exe)/<name>
 *   3. $PATH lookup
 * Returns nil if not found. Used by build.lua to locate `wamrc`.
 */
static int l_tool_find_tool(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    /* Pull the hull binary path from the global `__hull_exe`, which
     * runtime/lua/runtime.c sets at tool-mode init. */
    const char *hull_exe = NULL;
    lua_getglobal(L, "__hull_exe");
    if (lua_isstring(L, -1)) hull_exe = lua_tostring(L, -1);
    char out[PATH_MAX];
    int rc = hl_tools_lookup_path(name, hull_exe, out, sizeof(out));
    lua_pop(L, 1);
    if (rc != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, out);
    return 1;
}

/* ── tool.hull_cache_dir([kind]) → "<path>/" | nil, err ──
 *
 * Resolve `$HOME/.hull/cache/[<kind>/]`, creating the directories on
 * demand. With no argument returns the cache root; with a name like
 * "compute-aot" returns the subdir for that cache kind. Returns the
 * path WITH a trailing slash, ready for concatenation.
 *
 * Used by the AOT cache (build.lua → aot_cache.lua), the bytecode
 * cache (C-side), and future cache consumers. Honors the same
 * HULL_NO_CACHE / HULL_NO_<KIND>_CACHE env vars as
 * `hl_hull_cache_disabled` — but the disable check is the caller's
 * responsibility (we just resolve the path).
 *
 * Returns nil + error string on failure (no $HOME / mkdir failed /
 * invalid kind).
 */
static int l_tool_hull_cache_dir(lua_State *L)
{
    const char *kind = luaL_optstring(L, 1, NULL);
    char out[PATH_MAX];
    int rc = kind
        ? hl_hull_cache_subdir(kind, out, sizeof(out))
        : hl_hull_cache_dir(out, sizeof(out));
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L,
            "cache dir unavailable (HOME unset or mkdir failed)");
        return 2;
    }
    lua_pushstring(L, out);
    return 1;
}

/* ── tool.platform_cache_dir() → "$HOME/.hull/platform" | nil ──
 *
 * Where `hull flavor install` stores fetched per-flavor platform libs.
 * Returns the path (not guaranteed to exist) so `hull build --flavor` can
 * look for a cached lib there; nil when HOME is unset. */
static int l_tool_platform_cache_dir(lua_State *L)
{
    const char *home = getenv("HOME");
    if (!home || !*home) { lua_pushnil(L); return 1; }
    char out[PATH_MAX];
    int n = snprintf(out, sizeof(out), "%s/.hull/platform", home);
    if (n <= 0 || (size_t)n >= (int)sizeof(out)) { lua_pushnil(L); return 1; }
    lua_pushstring(L, out);
    return 1;
}

/* ── tool.feature_cache_dir() → "$HOME/.hull/feature" | nil ──
 *
 * Where `hull feature install` stores fetched per-feature libs
 * (libhull_feature-<name>-<arch>.a). Returns the path (not guaranteed to exist)
 * so `hull build` can look for a composed feature lib there; nil when HOME is
 * unset. Re-verify a cache-sourced lib with tool.platform_verify(dir, asset),
 * which is generic over the cache dir. */
static int l_tool_feature_cache_dir(lua_State *L)
{
    const char *home = getenv("HOME");
    if (!home || !*home) { lua_pushnil(L); return 1; }
    char out[PATH_MAX];
    int n = snprintf(out, sizeof(out), "%s/.hull/feature", home);
    if (n <= 0 || (size_t)n >= (int)sizeof(out)) { lua_pushnil(L); return 1; }
    lua_pushstring(L, out);
    return 1;
}

/* ── tool.platform_verify(dir, asset) → boolean ──
 *
 * Offline re-verify of an installed platform lib against the signed manifest
 * `hull flavor install` cached in `dir`, anchored on the EMBEDDED release
 * pubkey (not the writable dir). `hull build --flavor` calls this before
 * linking a lib pulled from ~/.hull/platform, closing the install->build
 * TOCTOU. Returns true iff the lib matches its signed digest. */
static int l_tool_platform_verify(lua_State *L)
{
    const char *dir   = luaL_checkstring(L, 1);
    const char *asset = luaL_checkstring(L, 2);
    lua_pushboolean(L, hl_release_io_verify_local_asset(dir, asset) == 0);
    return 1;
}

/* ── tool.hull_cache_disabled([kind]) → boolean ──
 *
 * Mirrors the C-side `hl_hull_cache_disabled` so Lua callers don't
 * have to re-implement env-name composition. Pass nil for `kind` to
 * check only the global HULL_NO_CACHE switch.
 */
static int l_tool_hull_cache_disabled(lua_State *L)
{
    const char *kind = luaL_optstring(L, 1, NULL);
    lua_pushboolean(L, hl_hull_cache_disabled(kind) != 0);
    return 1;
}

/* ── tool.cache_override() → string | nil
 *
 * Returns the current `HULL_CACHE_DIR` env value, or nil when
 * unset / empty. Tool mode doesn't expose `os.getenv`, so disclosure
 * surfaces (hull inspect, future hull doctor lua tooling) need
 * this dedicated binding to surface the override status. */
static int l_tool_cache_override(lua_State *L)
{
    const char *v = getenv("HULL_CACHE_DIR");
    if (v && *v) lua_pushstring(L, v);
    else         lua_pushnil(L);
    return 1;
}

/* ── tool.cache_kinds() → list of cache-kind tables.
 *
 * Each entry: { name, description, is_runtime, path, env_var,
 *               disabled }.
 *
 *   env_var   — the granular opt-out env var ("HULL_NO_<X>_CACHE")
 *               or empty string for system stores (tools — no
 *               opt-out because it isn't a cache).
 *   disabled  — true if either HULL_NO_CACHE or env_var is set
 *               truthy in the current process.
 *
 * Single source of truth: adding a new entry to REGISTRY[] in
 * cache_registry.c automatically picks up disclosure here. */
static int l_tool_cache_kinds(lua_State *L)
{
    int global_off = hl_hull_cache_disabled(NULL);

    lua_newtable(L);
    int idx = 1;
    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        lua_newtable(L);
        lua_pushstring(L, k->name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, k->description);
        lua_setfield(L, -2, "description");
        lua_pushboolean(L, k->is_runtime);
        lua_setfield(L, -2, "is_runtime");

        char path[PATH_MAX];
        if (hl_cache_resolve_path(k, path, sizeof(path)) == 0)
            lua_pushstring(L, path);
        else
            lua_pushstring(L, "");
        lua_setfield(L, -2, "path");

        /* Surface the env-var name via the canonical composer in
         * cache_dir.h. NULL env_kind → system store (no per-cache
         * opt-out applies; emit empty string). */
        if (k->env_kind) {
            char env_name[64];
            if (hl_hull_cache_env_name(k->env_kind,
                                       env_name, sizeof(env_name)) == 0) {
                lua_pushstring(L, env_name);
            } else {
                lua_pushstring(L, "");
            }
        } else {
            lua_pushstring(L, "");
        }
        lua_setfield(L, -2, "env_var");

        int per_kind_off = k->env_kind
            ? hl_hull_cache_disabled(k->env_kind) : 0;
        lua_pushboolean(L, global_off || per_kind_off);
        lua_setfield(L, -2, "disabled");

        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

/* ── tool.blob_store_* — keyed CAS-style cache for tool-mode ──────
 *
 * Backed by hull/blob_store in keyed mode (see
 * hl_blob_store_put_keyed). The compute AOT cache uses this from
 * stdlib/cli/lua/hull/aot_cache.lua; future template-AST cache and
 * any other tool-side cache should reuse it.
 *
 * Stores are memoized per-process by kind so repeat lookups don't
 * re-resolve $HOME/.hull/blobs/runtime/<kind>/ on every call. */

#define HL_TOOL_MAX_STORES 8

typedef struct {
    char         kind[32];
    HlBlobStore *store;
} ToolStoreEntry;

static ToolStoreEntry g_tool_stores[HL_TOOL_MAX_STORES];
static int            g_tool_store_count = 0;

/* Resolve (and memoize) the blob store for `kind`. Returns NULL on
 * error (HOME unset, mkdir failed, invalid kind). */
static HlBlobStore *tool_store_for(const char *kind)
{
    if (!kind) return NULL;
    for (int i = 0; i < g_tool_store_count; i++) {
        if (strcmp(g_tool_stores[i].kind, kind) == 0)
            return g_tool_stores[i].store;
    }
    if (g_tool_store_count >= HL_TOOL_MAX_STORES) return NULL;

    char root[PATH_MAX];
    if (hl_hull_cache_subdir(kind, root, sizeof(root)) != 0) return NULL;
    size_t rl = strlen(root);
    if (rl > 1 && root[rl - 1] == '/') root[rl - 1] = '\0';

    HlBlobStore *s = NULL;
    if (hl_blob_store_open(&s, NULL, root, /*shard_depth=*/1, 0) != 0)
        return NULL;

    int slot = g_tool_store_count++;
    snprintf(g_tool_stores[slot].kind, sizeof(g_tool_stores[slot].kind),
             "%s", kind);
    g_tool_stores[slot].store = s;
    return s;
}

/* tool.blob_store_exists(kind, key) → boolean */
static int l_tool_blob_store_exists(lua_State *L)
{
    const char *kind = luaL_checkstring(L, 1);
    const char *key  = luaL_checkstring(L, 2);
    HlBlobStore *s = tool_store_for(kind);
    if (!s) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, hl_blob_store_exists(s, key) == 1);
    return 1;
}

/* tool.blob_store_get_to(kind, key, dest_path) → bool
 *
 * If a cache entry exists for `key`, copy its bytes to `dest_path`
 * (creating it). Returns true on success, false on miss or copy
 * failure. The destination is unveil-checked via the tool sandbox
 * ('w'); source path is internal so the check is skipped. */
static int l_tool_blob_store_get_to(lua_State *L)
{
    const char *kind     = luaL_checkstring(L, 1);
    const char *key      = luaL_checkstring(L, 2);
    const char *dest     = luaL_checkstring(L, 3);
    HlBlobStore *s = tool_store_for(kind);
    if (!s) { lua_pushboolean(L, 0); return 1; }

    uint8_t *bytes = NULL;
    size_t   len   = 0;
    if (hl_blob_store_get(s, key, /*track_access=*/1, &bytes, &len) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    HlToolUnveilCtx *ctx = get_unveil_ctx(L);
    if (ctx && hl_tool_unveil_check(ctx, dest, 'w') != 0) {
        free(bytes);
        lua_pushboolean(L, 0);
        return 1;
    }

    FILE *out = fopen(dest, "wb");
    if (!out) {
        free(bytes);
        lua_pushboolean(L, 0);
        return 1;
    }
    size_t wrote = (len > 0) ? fwrite(bytes, 1, len, out) : 0;
    int ok = (fclose(out) == 0) && (wrote == len);
    free(bytes);
    if (!ok) unlink(dest);
    lua_pushboolean(L, ok);
    return 1;
}

/* tool.blob_store_put_from(kind, key, src_path) → bool
 *
 * Read `src_path` and store its bytes under `key`. Source path is
 * unveil-checked ('r'). Returns true on success. */
static int l_tool_blob_store_put_from(lua_State *L)
{
    const char *kind = luaL_checkstring(L, 1);
    const char *key  = luaL_checkstring(L, 2);
    const char *src  = luaL_checkstring(L, 3);
    HlBlobStore *s = tool_store_for(kind);
    if (!s) { lua_pushboolean(L, 0); return 1; }

    HlToolUnveilCtx *ctx = get_unveil_ctx(L);
    if (ctx && hl_tool_unveil_check(ctx, src, 'r') != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    FILE *in = fopen(src, "rb");
    if (!in) { lua_pushboolean(L, 0); return 1; }
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in); lua_pushboolean(L, 0); return 1;
    }
    long sz = ftell(in);
    if (sz < 0 || sz > 256 * 1024 * 1024) {  /* 256 MB sanity cap */
        fclose(in); lua_pushboolean(L, 0); return 1;
    }
    rewind(in);

    uint8_t *bytes = (uint8_t *)malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (!bytes) { fclose(in); lua_pushboolean(L, 0); return 1; }
    size_t got = (sz > 0) ? fread(bytes, 1, (size_t)sz, in) : 0;
    fclose(in);
    if (got != (size_t)sz) {
        free(bytes); lua_pushboolean(L, 0); return 1;
    }

    int rc = hl_blob_store_put_keyed(s, key, bytes, (size_t)sz);
    free(bytes);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.platform_name() → "darwin-arm64" | "linux-x86_64" | ... ──
 *
 * Returns the running hull's platform identifier, matching the
 * arch-name format used in the embedded signed platform manifest
 * and in `hl_release_io_platform()`. Used by build.lua to look up
 * the expected SHA-256 for THIS arch when cross-checking the
 * libhull_platform.a it's about to embed in an app.
 *
 * Returns "cosmo" for cosmocc binaries (the manifest stores per-arch
 * entries under "cosmo-x86_64" / "cosmo-aarch64"; cosmo build.lua
 * code path checks BOTH explicitly, doesn't use this generic name).
 */
static int l_tool_platform_name(lua_State *L)
{
    lua_pushstring(L, hl_release_io_platform());
    return 1;
}

/* ── tool.platform_sig_get() → table | nil ────────────────────────
 *
 * Returns the embedded signed platform manifest as a Lua table:
 *
 *   { manifest = "<text bytes>", signature = "<hex+nl bytes>" }
 *
 * or nil when no signed blob is embedded (local dev builds, opt-out
 * builds, anything that hasn't run sign-platform-manifest in CI).
 *
 * Used by build.lua to:
 *   - Detect whether the running hull was built with platform-sig
 *     wired through, so it can decide whether to enforce or
 *     opt-out at app-build time.
 *   - Pass the raw bytes verbatim into package.sig.platform's new
 *     embedded fields (additive — doesn't replace today's developer-
 *     signed JSON shape).
 */
static int l_tool_platform_sig_get(lua_State *L)
{
    const unsigned char *manifest = NULL, *signature = NULL;
    size_t manifest_len = 0, sig_len = 0;
    if (hl_embedded_platform_sig(&manifest, &manifest_len,
                                 &signature, &sig_len) != 0 ||
        manifest_len == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 2);
    lua_pushlstring(L, (const char *)manifest, manifest_len);
    lua_setfield(L, -2, "manifest");
    lua_pushlstring(L, (const char *)signature, sig_len);
    lua_setfield(L, -2, "signature");
    return 1;
}

/* ── tool.platform_sig_arch_hash(arch_name) → hex_string | nil ───
 *
 * Look up the expected SHA-256 hex of the libhull_platform.a for
 * a given arch in the embedded signed manifest. Returns nil if the
 * arch isn't in the manifest (older release manifest predates the
 * arch) OR no embedded blob is present.
 *
 * build.lua uses this to cross-check the actual SHA-256 of the .a
 * it's about to embed in an app against what the release pipeline
 * signed for that arch. Mismatch → hard reject (unless
 * --no-verify-platform).
 */
static int l_tool_platform_sig_arch_hash(lua_State *L)
{
    const char *arch = luaL_checkstring(L, 1);
    const unsigned char *manifest = NULL;
    size_t manifest_len = 0;
    if (hl_embedded_platform_sig(&manifest, &manifest_len,
                                 NULL, NULL) != 0 ||
        manifest_len == 0) {
        lua_pushnil(L);
        return 1;
    }
    char hex[65];
    if (hl_platform_sig_extract_for_arch((const char *)manifest,
                                         manifest_len, arch, hex) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, hex);
    return 1;
}

/* ── tool.platform_pubkey() → hex_string | nil ────────────────────
 *
 * Returns the build-time-pinned gethull.dev platform Ed25519 public
 * key as 64 lowercase hex chars, or nil when the running hull was
 * compiled with the all-zeros placeholder (dev hull / fork without
 * platform-sig wired). verify.lua uses this to verify the
 * package.sig.platform.gethull layer signed by gethull.dev at
 * release time.
 *
 * Placeholder detection delegates to
 * `hl_platform_pubkey_is_placeholder()` so this binding and
 * signature.c §5b always agree on what "no pinned key" means.
 */
static int l_tool_platform_pubkey(lua_State *L)
{
    if (hl_platform_pubkey_is_placeholder()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, HL_PLATFORM_PUBKEY_HEX);
    return 1;
}

/* ── tool.sha256_file(path) → hex_string | nil, err ───────────────
 *
 * SHA-256 a file by STREAMING it through the incremental crypto API.
 * Hashing a large archive (e.g. the ~127 MB DuckDB feature lib, or the
 * wgpu GPU lib) must never materialize the file as a Lua string: that
 * would blow the tool VM's 64 MB sandbox allocator ("not enough
 * memory"). build.lua uses this to record composed feature archives in
 * package.sig.gethull.composed (issue #114). Only the 64-char hex
 * result crosses into Lua.
 */
static int l_tool_sha256_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "rb");
    if (!f) {
        lua_pushnil(L);
        lua_pushstring(L, "cannot open file");
        return 2;
    }
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);
    unsigned char buf[16384];   /* modest stack chunk; hashing is I/O-bound */
    size_t n;
    int err = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (hl_cap_crypto_sha256_update(&ctx, buf, n) != 0) { err = 1; break; }
    }
    if (!err && ferror(f)) err = 1;
    fclose(f);
    uint8_t digest[32];
    if (err || hl_cap_crypto_sha256_final(&ctx, digest) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "hash failed");
        return 2;
    }
    static const char hexd[] = "0123456789abcdef";
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[i * 2]     = hexd[digest[i] >> 4];
        hex[i * 2 + 1] = hexd[digest[i] & 0x0f];
    }
    hex[64] = '\0';
    lua_pushstring(L, hex);
    return 1;
}

static const luaL_Reg tool_funcs[] = {
    { "spawn",                       l_tool_spawn },
    { "sha256_file",                 l_tool_sha256_file },
    { "emit_app_registry",           l_emit_app_registry },
    { "bundled_object",              l_tool_bundled_object },
    { "spawn_read",                  l_tool_spawn_read },
    { "find_files",                  l_tool_find_files },
    { "find_tool",                   l_tool_find_tool },
    { "hull_cache_dir",              l_tool_hull_cache_dir },
    { "platform_cache_dir",          l_tool_platform_cache_dir },
    { "feature_cache_dir",           l_tool_feature_cache_dir },
    { "platform_verify",             l_tool_platform_verify },
    { "hull_cache_disabled",         l_tool_hull_cache_disabled },
    { "cache_kinds",                 l_tool_cache_kinds },
    { "cache_override",              l_tool_cache_override },
    { "blob_store_exists",           l_tool_blob_store_exists },
    { "blob_store_get_to",           l_tool_blob_store_get_to },
    { "blob_store_put_from",         l_tool_blob_store_put_from },
    { "platform_name",               l_tool_platform_name },
    { "platform_sig_get",            l_tool_platform_sig_get },
    { "platform_sig_arch_hash",      l_tool_platform_sig_arch_hash },
    { "platform_pubkey",             l_tool_platform_pubkey },
    { "copy",                   l_tool_copy },
    { "rename",                 l_tool_rename },
    { "remove_file",            l_tool_remove_file },
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
    { "extract_manifest_js",    l_tool_extract_manifest_js },
    { "extract_platform",       l_tool_extract_platform },
    { "extract_feature_runtime", l_tool_extract_feature_runtime },
    { "extract_feature_http",   l_tool_extract_feature_http },
    { "extract_feature_http_rt", l_tool_extract_feature_http_rt },
    { "extract_feature_tui_rt", l_tool_extract_feature_tui_rt },
    { "extract_feature_wasm",   l_tool_extract_feature_wasm },
    { "extract_feature_wasm_rt", l_tool_extract_feature_wasm_rt },
    { "extract_feature_sqlite_rt", l_tool_extract_feature_sqlite_rt },
    { "extract_feature_sqlite", l_tool_extract_feature_sqlite },
    { "extract_feature_tls",    l_tool_extract_feature_tls },
    { "extract_feature_keel",   l_tool_extract_feature_keel },
    { "extract_feature_image",  l_tool_extract_feature_image },
    { "extract_feature_image_rt", l_tool_extract_feature_image_rt },
    { "extract_platform_cosmo", l_tool_extract_platform_cosmo },
    { "platform_archs",         l_tool_platform_archs },
    /* Orchestration entries (modules_resolve, doctor_json, agent_*,
     * dev_*, migrate_status, modules_available) are added by
     * hl_lua_tool_register_orchestration after this base table is
     * installed — see src/hull/tool_orchestration.c. */
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
