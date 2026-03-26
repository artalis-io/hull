/* mod_fs.c — hull.fs module + custom require() and stdlib registration
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/fs.h"
#include "hull/limits.h"
#include "hull/vfs.h"

#include "log.h"

#include <sh_arena.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.fs module — filesystem capabilities
 *
 * fs.mmap(path) -> MappedBuffer userdata
 * ════════════════════════════════════════════════════════════════════ */

static int lua_fs_mmap(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.fs_cfg)
        return luaL_error(L, "fs.mmap: not available (declare fs_read in manifest)");

    const char *path = luaL_checkstring(L, 1);

    const char *err_msg = NULL;
    HlMappedBuffer *buf = hl_cap_fs_mmap(lua->base.fs_cfg, path,
                                          lua->base.alloc, &err_msg);
    if (!buf) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "mmap failed");
        return 2;
    }

    HlMappedBuffer **pp = lua_newuserdata(L, sizeof(HlMappedBuffer *));
    *pp = buf;
    luaL_setmetatable(L, HL_MMAP_MT);
    return 1;
}

static HlMappedBuffer *check_mmap(lua_State *L, int idx)
{
    HlMappedBuffer **pp = luaL_checkudata(L, idx, HL_MMAP_MT);
    if (!pp || !*pp) {
        luaL_error(L, "invalid mapped buffer");
        return NULL; /* unreachable — satisfies static analysis */
    }
    return *pp;
}

static int lua_mmap_len(lua_State *L)
{
    HlMappedBuffer *buf = check_mmap(L, 1);
    lua_pushinteger(L, buf->closed ? 0 : (lua_Integer)buf->len);
    return 1;
}

static int lua_mmap_close(lua_State *L)
{
    HlMappedBuffer **pp = luaL_checkudata(L, 1, HL_MMAP_MT);
    if (pp && *pp) {
        hl_cap_fs_munmap(*pp);
        *pp = NULL;
    }
    return 0;
}

static int lua_mmap_gc(lua_State *L)
{
    return lua_mmap_close(L);
}

static int lua_mmap_tostring(lua_State *L)
{
    HlMappedBuffer **pp = luaL_checkudata(L, 1, HL_MMAP_MT);
    if (pp && *pp && !(*pp)->closed)
        lua_pushfstring(L, "MappedBuffer(%d bytes)", (int)(*pp)->len);
    else
        lua_pushliteral(L, "MappedBuffer(closed)");
    return 1;
}

static void lua_register_mmap_metatable(lua_State *L)
{
    static const luaL_Reg mmap_methods[] = {
        {"len",   lua_mmap_len},
        {"close", lua_mmap_close},
        {NULL, NULL}
    };

    luaL_newmetatable(L, HL_MMAP_MT);

    /* __index = methods table */
    luaL_newlib(L, mmap_methods);
    lua_setfield(L, -2, "__index");

    /* __gc for automatic cleanup */
    lua_pushcfunction(L, lua_mmap_gc);
    lua_setfield(L, -2, "__gc");

    /* __close for Lua 5.4 to-be-closed variables */
    lua_pushcfunction(L, lua_mmap_close);
    lua_setfield(L, -2, "__close");

    /* __tostring */
    lua_pushcfunction(L, lua_mmap_tostring);
    lua_setfield(L, -2, "__tostring");

    /* __len */
    lua_pushcfunction(L, lua_mmap_len);
    lua_setfield(L, -2, "__len");

    lua_pop(L, 1); /* pop metatable */
}

static const luaL_Reg fs_funcs[] = {
    {"mmap", lua_fs_mmap},
    {NULL, NULL}
};

int luaopen_hull_fs(lua_State *L)
{
    lua_register_mmap_metatable(L);
    luaL_newlib(L, fs_funcs);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * Custom require() — module loader with embedded + filesystem fallback
 *
 * Replaces Lua's package.require with a minimal custom version.
 * Search order:
 *   1. Cache (registry "__hull_loaded")
 *   2. Embedded modules (registry "__hull_modules")
 *   3. Filesystem (dev mode — relative requires from app_dir)
 *   4. Error
 *
 * Module namespaces:
 *   hull.*   — Hull stdlib wrappers (e.g. require('hull.json'))
 *   vendor.* — Vendored third-party libs (e.g. require('vendor.json'))
 *   ./path   — Relative to requiring module (filesystem or embedded app)
 *   ../path  — Relative to requiring module (parent traversal)
 * ════════════════════════════════════════════════════════════════════ */

/* ── Path normalization helper ────────────────────────────────────── */

/*
 * Normalize a path in-place by collapsing `.` and `..` segments.
 * Input:  "routes/../utils/./helper"
 * Output: "utils/helper"
 * Returns 0 on success, -1 if `..` escapes past root.
 */
static int normalize_path(char *path)
{
    /* Split into segments, process left-to-right */
    char *segments[128];
    int depth = 0;
    int absolute = (path[0] == '/');

    char *p = path;
    while (*p) {
        /* Skip slashes */
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        /* Find end of segment */
        char *seg = p;
        while (*p && *p != '/')
            p++;
        if (*p == '/') {
            *p = '\0';
            p++;
        }

        if (strcmp(seg, ".") == 0) {
            continue; /* skip */
        } else if (strcmp(seg, "..") == 0) {
            if (depth > 0)
                depth--;
            else
                return -1; /* escapes past root */
        } else {
            if (depth >= 128)
                return -1;
            segments[depth++] = seg;
        }
    }

    /* Rebuild path */
    char *out = path;
    if (absolute)
        *out++ = '/';
    for (int i = 0; i < depth; i++) {
        if (i > 0)
            *out++ = '/';
        size_t len = strlen(segments[i]);
        memmove(out, segments[i], len);
        out += len;
    }
    *out = '\0';

    return 0;
}

/* ── Resolve relative module path ─────────────────────────────────── */

/*
 * Resolve a relative require path (starting with ./ or ../) against
 * the caller's module path and app_dir.
 *
 * Returns 0 on success with `out` filled with the filesystem path.
 * Returns -1 on error (path too long, escapes app_dir, etc.).
 */
static int resolve_module_path(lua_State *L, const char *name,
                               const char *app_dir,
                               char *out, size_t out_size)
{
    /* Get the caller's module path from registry */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_current_module");
    const char *caller = lua_tostring(L, -1);

    char caller_dir[HL_MODULE_PATH_MAX];
    if (caller) {
        /* Extract directory from caller path */
        const char *last_slash = strrchr(caller, '/');
        if (last_slash) {
            size_t dir_len = (size_t)(last_slash - caller);
            if (dir_len >= sizeof(caller_dir)) {
                lua_pop(L, 1);
                return -1;
            }
            memcpy(caller_dir, caller, dir_len);
            caller_dir[dir_len] = '\0';
        } else {
            /* No slash — caller is in the root */
            caller_dir[0] = '.';
            caller_dir[1] = '\0';
        }
    } else {
        /* No caller context — use app_dir as base */
        if (strlen(app_dir) >= sizeof(caller_dir)) {
            lua_pop(L, 1);
            return -1;
        }
        strncpy(caller_dir, app_dir, sizeof(caller_dir) - 1);
        caller_dir[sizeof(caller_dir) - 1] = '\0';
    }
    lua_pop(L, 1); /* pop __hull_current_module */

    /* Build joined path: caller_dir / name [.lua] */
    const char *ext = "";
    size_t name_len = strlen(name);
    int has_lua = (name_len >= 4 && strcmp(name + name_len - 4, ".lua") == 0);
    int has_json = (name_len >= 5 && strcmp(name + name_len - 5, ".json") == 0);
    if (!has_lua && !has_json)
        ext = ".lua";

    char joined[HL_MODULE_PATH_MAX];
    int n = snprintf(joined, sizeof(joined), "%s/%s%s",
                     caller_dir, name, ext);
    if (n < 0 || (size_t)n >= sizeof(joined))
        return -1;

    /* Normalize (collapse . and .. segments) */
    if (normalize_path(joined) != 0)
        return -1;

    /* Security: verify the resolved path starts with app_dir.
     * Build canonical: app_dir prefix must match. */
    size_t app_dir_len = strlen(app_dir);
    /* Strip trailing slash from app_dir for comparison */
    while (app_dir_len > 0 && app_dir[app_dir_len - 1] == '/')
        app_dir_len--;

    /* For "." app_dir, any path without leading .. is valid
     * (normalize_path already rejects escaping past root) */
    if (!(app_dir_len == 1 && app_dir[0] == '.')) {
        if (strncmp(joined, app_dir, app_dir_len) != 0 ||
            (joined[app_dir_len] != '/' && joined[app_dir_len] != '\0'))
            return -1; /* escapes above app_dir */
    }

    if (strlen(joined) >= out_size)
        return -1;
    memcpy(out, joined, strlen(joined) + 1);

    return 0;
}

/* ── Execute and cache a loaded module chunk ──────────────────────── */

/*
 * Execute a loaded chunk, save/restore __hull_current_module context,
 * cache the result, and leave the module value on the stack.
 * `module_path` is the canonical path used for context and cache key.
 * Returns 1 (number of Lua return values) on success.
 * On error, calls lua_error (does not return).
 */
static int execute_and_cache_module(lua_State *L, const char *module_path)
{
    /* Save current module context */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_current_module");
    /* Stack: ... chunk, saved_module */

    /* Set new module context */
    lua_pushstring(L, module_path);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_current_module");

    /* Execute chunk (it's below saved_module on the stack) */
    lua_pushvalue(L, -2); /* copy chunk to top */
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        /* Restore context before propagating error */
        lua_pushvalue(L, -2); /* push saved_module (now at -3) */
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_current_module");
        lua_remove(L, -2); /* remove saved_module */
        lua_remove(L, -2); /* remove original chunk */
        return lua_error(L);
    }
    /* Stack: ... chunk, saved_module, result */

    /* Restore previous module context */
    lua_pushvalue(L, -2); /* push saved_module */
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_current_module");
    lua_remove(L, -2); /* remove saved_module */
    /* Stack: ... chunk, result */

    /* If chunk returned nil, store true as sentinel */
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }

    /* Cache the result in __hull_loaded */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
    lua_pushvalue(L, -2);  /* push module result */
    lua_setfield(L, -2, module_path);
    lua_pop(L, 1); /* pop __hull_loaded */

    /* Remove original chunk, leaving just the result */
    lua_remove(L, -2);
    return 1;
}

/* ── Main require() implementation ────────────────────────────────── */

static int hl_lua_require(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    /* 1. Check cache (registry "__hull_loaded") */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2); /* remove __hull_loaded table */
        return 1;          /* return cached module */
    }
    lua_pop(L, 2); /* pop nil + __hull_loaded */

    /* 2. Look up in embedded modules table (registry "__hull_modules") */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_modules");
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2); /* remove __hull_modules table */

        /* JSON embedded module → decode raw string with json.decode()
         * Only for relative paths (./) — not stdlib like "hull.json" */
        size_t nlen = strlen(name);
        if (name[0] == '.' && name[1] == '/' &&
            nlen >= 5 && strcmp(name + nlen - 5, ".json") == 0) {
            lua_getglobal(L, "json");
            lua_getfield(L, -1, "decode");
            lua_remove(L, -2); /* remove json table */
            lua_pushvalue(L, -2); /* push the JSON string */
            lua_remove(L, -3); /* remove original string */
            if (lua_pcall(L, 1, 1, 0) != LUA_OK)
                return lua_error(L);

            /* Cache in __hull_loaded */
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
            lua_pushvalue(L, -2);
            lua_setfield(L, -2, name);
            lua_pop(L, 1); /* pop __hull_loaded */
            return 1;
        }

        return execute_and_cache_module(L, name);
    }
    lua_pop(L, 2); /* pop nil + __hull_modules */

    /* 3. Filesystem fallback (dev mode — relative requires) */
    HlLua *lua = get_hl_lua(L);
    if (lua && lua->app_dir &&
        (name[0] == '.' || strchr(name, '/') != NULL)) {

        char path[HL_MODULE_PATH_MAX];
        if (resolve_module_path(L, name, lua->app_dir,
                                path, sizeof(path)) == 0) {

            /* Check cache by resolved canonical path */
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
            lua_getfield(L, -1, path);
            if (!lua_isnil(L, -1)) {
                lua_remove(L, -2); /* remove __hull_loaded */
                return 1;
            }
            lua_pop(L, 2); /* pop nil + __hull_loaded */

            /* Read file from disk */
            FILE *f = fopen(path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                if (size < 0 || size > HL_MODULE_MAX_SIZE) {
                    fclose(f);
                    return luaL_error(L, "module too large: %s", path);
                }
                if (fseek(f, 0, SEEK_SET) != 0) {
                    fclose(f);
                    return luaL_error(L, "seek failed: %s", path);
                }

                /* Save arena position — buffer is only needed until
                 * luaL_loadbuffer copies it into Lua bytecode. */
                size_t arena_saved = lua->scratch->used;

                char *buf = sh_arena_alloc(lua->scratch, (size_t)size);
                if (!buf) {
                    fclose(f);
                    return luaL_error(L, "out of memory loading: %s", path);
                }

                size_t nread = fread(buf, 1, (size_t)size, f);
                int read_err = ferror(f);
                fclose(f);

                if (read_err || nread != (size_t)size) {
                    lua->scratch->used = arena_saved;
                    return luaL_error(L, "read error: %s", path);
                }

                /* JSON file → decode with json.decode() instead of
                 * compiling as Lua bytecode */
                size_t path_len = strlen(path);
                if (path_len >= 5 &&
                    strcmp(path + path_len - 5, ".json") == 0) {
                    lua_getglobal(L, "json");
                    lua_getfield(L, -1, "decode");
                    lua_remove(L, -2); /* remove json table */
                    lua_pushlstring(L, buf, nread);
                    lua->scratch->used = arena_saved;
                    if (lua_pcall(L, 1, 1, 0) != LUA_OK)
                        return lua_error(L);

                    /* Cache in __hull_loaded */
                    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
                    lua_pushvalue(L, -2);
                    lua_setfield(L, -2, path);
                    lua_pop(L, 1); /* pop __hull_loaded */
                    return 1;
                }

                /* Compile the chunk — copies data into Lua bytecode */
                int load_ok = luaL_loadbuffer(L, buf, nread, path) == LUA_OK;

                /* Reclaim file buffer — Lua owns the bytecode now */
                lua->scratch->used = arena_saved;

                if (!load_ok)
                    return lua_error(L); /* propagate compile error */

                return execute_and_cache_module(L, path);
            }
        }
    }

    return luaL_error(L, "module not found: %s", name);
}

int hl_lua_register_stdlib(HlLua *lua)
{
    if (!lua || !lua->L)
        return -1;

    lua_State *L = lua->L;

    /* Create __hull_loaded cache table */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_loaded");

    /* Create __hull_modules table and populate with compiled chunks.
     * Iterates the platform VFS entries, skipping JS modules
     * (colon-separated names) — adding a new .lua file requires no C changes. */
    lua_newtable(L);

    if (lua->base.platform_vfs) {
        for (size_t i = 0; i < lua->base.platform_vfs->count; i++) {
            const HlEntry *e = &lua->base.platform_vfs->entries[i];
            if (strchr(e->name, ':')) continue; /* skip JS modules */
            if (luaL_loadbuffer(L, (const char *)e->data, e->len, e->name) != LUA_OK) {
                log_error("[hull:c] failed to load stdlib module '%s': %s",
                          e->name, lua_tostring(L, -1));
                lua_pop(L, 2); /* pop error + modules table */
                return -1;
            }
            lua_setfield(L, -2, e->name);
        }
    }

    /* Load embedded app modules (if any — skip non-Lua entries) */
    if (lua->base.app_vfs) {
        for (size_t i = 0; i < lua->base.app_vfs->count; i++) {
            const HlEntry *e = &lua->base.app_vfs->entries[i];
            size_t nlen = strlen(e->name);
            /* Skip JS modules (.js) and non-module entries (templates/, static/, migrations/) */
            if (nlen >= 3 && strcmp(e->name + nlen - 3, ".js") == 0)
                continue;
            if (e->name[0] != '.')
                continue;  /* module entries start with "./" */
            if (nlen >= 5 && strcmp(e->name + nlen - 5, ".json") == 0) {
                /* JSON data — store as raw string, decoded on first require() */
                lua_pushlstring(L, (const char *)e->data, e->len);
            } else {
                if (luaL_loadbuffer(L, (const char *)e->data, e->len, e->name) != LUA_OK) {
                    log_error("[hull:c] failed to load app module '%s': %s",
                              e->name, lua_tostring(L, -1));
                    lua_pop(L, 2); /* pop error + modules table */
                    return -1;
                }
            }
            lua_setfield(L, -2, e->name);
        }
    }

    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_modules");

    /* Register require as a global function */
    lua_pushcfunction(L, hl_lua_require);
    lua_setglobal(L, "require");

    /* Pre-load json as a global: call require('hull.json') internally.
     * Skip if no platform VFS is available (bare init without stdlib). */
    if (lua->base.platform_vfs) {
        lua_getglobal(L, "require");
        lua_pushstring(L, "hull.json");
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            log_error("[hull:c] failed to pre-load json: %s",
                      lua_tostring(L, -1));
            lua_pop(L, 1);
            return -1;
        }
        lua_setglobal(L, "json");
    }

    return 0;
}
