/* mod_fs.c - hull.fs module + custom require() and stdlib registration
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/utils/alloc.h"
#include "hull/cap/fs.h"
#include "hull/cap/fs_resolve.h"  /* descriptor-relative virtual-root module read */
#include "hull/limits/core.h"
#include "hull/module_registry.h"
#include "hull/module_resolver.h"
#include "hull/utils/path_normalize.h"
#include "hull/runtime/lua_bytecode_cache.h"
#include "hull/vfs.h"

#include "log.h"

#include <sh_arena.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* close */

/* ════════════════════════════════════════════════════════════════════
 * hull.fs module - filesystem capabilities
 *
 * fs.read(path)        -> string | nil, err  (binary-safe)
 * fs.write(path, bytes) -> true | nil, err
 * fs.mmap(path)        -> MappedBuffer userdata
 *
 * All three go through hl_cap_fs_* which enforces manifest fs_read /
 * fs_write allowlists. Paths are relative to the app's base_dir;
 * absolute paths and `..` traversal are rejected at the cap layer.
 * ════════════════════════════════════════════════════════════════════ */

/* fs.read(path) - returns the whole file contents as a binary-safe
 * Lua string, or (nil, err). Two-pass: first call hl_cap_fs_read
 * with buf=NULL to learn the size, then allocate + read. */
static int lua_fs_read(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.fs_cfg) {
        lua_pushnil(L);
        lua_pushstring(L, "fs.read: not available (declare fs.read in manifest)");
        return 2;
    }
    const char *path = luaL_checkstring(L, 1);

    const char *err_msg = NULL;
    int64_t size = hl_cap_fs_read(lua->base.fs_cfg, path, NULL, 0, &err_msg);
    if (size < 0) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "read_failed");
        return 2;
    }
    if (size == 0) {
        lua_pushlstring(L, "", 0);
        return 1;
    }

    /* Route allocation through the Lua memory tracker so the runtime's
     * memory cap covers this transient buffer. */
    uint8_t *buf = hl_alloc_malloc(lua->base.alloc, (size_t)size);
    if (!buf) {
        lua_pushnil(L);
        lua_pushstring(L, "out_of_memory");
        return 2;
    }
    int64_t got = hl_cap_fs_read(lua->base.fs_cfg, path,
                                  (char *)buf, (size_t)size, &err_msg);
    if (got < 0) {
        hl_alloc_free(lua->base.alloc, buf, (size_t)size);
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "read_failed");
        return 2;
    }
    lua_pushlstring(L, (const char *)buf, (size_t)got);
    hl_alloc_free(lua->base.alloc, buf, (size_t)size);
    return 1;
}

/* fs.write(path, bytes) - writes bytes to path (binary-safe). Creates
 * parent dirs as needed. Returns true or (nil, err). */
static int lua_fs_write(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.fs_cfg) {
        lua_pushnil(L);
        lua_pushstring(L, "fs.write: not available (declare fs.write in manifest)");
        return 2;
    }
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *bytes = luaL_checklstring(L, 2, &len);

    const char *err_msg = NULL;
    int rc = hl_cap_fs_write(lua->base.fs_cfg, path, bytes, len, &err_msg);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err_msg ? err_msg : "write_failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_fs_mmap(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.fs_cfg)
        return luaL_error(L, "fs.mmap: not available (declare fs_read in manifest)");

    const char *path = luaL_checkstring(L, 1);

    /* Optional second arg { offset = N, length = M } selects a windowed,
     * page-aligned mapping (mapped-spans). A bare path stays whole-file. */
    const char *err_msg = NULL;
    HlMappedBuffer *buf;
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_getfield(L, 2, "offset");
        lua_Integer off = luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, 2, "length");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "fs.mmap: window requires a length");
        }
        lua_Integer length = lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (off < 0 || length <= 0)
            return luaL_error(L, "fs.mmap: offset must be >= 0 and length > 0");
        buf = hl_cap_fs_mmap_window(lua->base.fs_cfg, path,
                                    (uint64_t)off, (uint64_t)length,
                                    lua->base.alloc, &err_msg);
    } else {
        buf = hl_cap_fs_mmap(lua->base.fs_cfg, path,
                             lua->base.alloc, &err_msg);
    }
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

/* Map the node-type enum to its stable string label (Lua/JS parity). */
static const char *fs_node_type_name(HlFsNodeType t)
{
    switch (t) {
    case HL_FS_NODE_FILE:    return "file";
    case HL_FS_NODE_DIR:     return "dir";
    case HL_FS_NODE_SYMLINK: return "symlink";
    default:                 return "other";   /* FIFO, socket, device, ... */
    }
}

/* fs.stat(path) -> { type, size, mode, mtime } | nil | (nil, err).
 * Present -> a metadata table. Absent -> a single nil (subsumes `exists`).
 * Error   -> (nil, err) with a stable token. lstat semantics: a terminal
 * symlink is reported as type "symlink", never followed. */
static int lua_fs_stat(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.fs_cfg) {
        lua_pushnil(L);
        lua_pushstring(L, "fs.stat: not available (declare fs.read in manifest)");
        return 2;
    }
    const char *path = luaL_checkstring(L, 1);

    const char *err = NULL;
    HlFsStatInfo info;
    int rc = hl_cap_fs_stat(lua->base.fs_cfg, path, &info, &err);
    if (rc == 1) { lua_pushnil(L); return 1; }        /* absent (not an error) */
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "stat_failed");
        return 2;
    }
    lua_createtable(L, 0, 4);
    lua_pushstring(L, fs_node_type_name(info.type)); lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)info.size);      lua_setfield(L, -2, "size");
    lua_pushinteger(L, (lua_Integer)info.mode);      lua_setfield(L, -2, "mode");
    lua_pushinteger(L, (lua_Integer)info.mtime);     lua_setfield(L, -2, "mtime");
    return 1;
}

/* fs.list(dir) -> array of { name, type, size } | (nil, err). Deterministic
 * byte-order (unsigned-byte lexicographic, shorter first). An empty directory
 * yields an empty array; a missing/denied directory yields (nil, err). */
/* A __gc-guarded owner for the C list result, so the C-heap array + names are
 * freed even if a Lua allocator OOM longjmps out of the table-building loop below
 * (the array outlives several Lua allocations that can each raise). Mirrors how the
 * MappedBuffer userdata ties a C resource to Lua GC. */
#define HL_FS_LIST_GUARD_MT "hull.fs.list.guard"

typedef struct {
    HlFsDirEntry *entries;
    size_t        count;
    HlAllocator  *alloc;
} HlFsListGuard;

static int lua_fs_list_guard_gc(lua_State *L)
{
    HlFsListGuard *g = luaL_checkudata(L, 1, HL_FS_LIST_GUARD_MT);
    if (g && g->entries) {
        hl_cap_fs_list_free(g->entries, g->count, g->alloc);
        g->entries = NULL;   /* idempotent: a later collection is a no-op */
    }
    return 0;
}

static void lua_register_fs_list_guard(lua_State *L)
{
    luaL_newmetatable(L, HL_FS_LIST_GUARD_MT);
    lua_pushcfunction(L, lua_fs_list_guard_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

static int lua_fs_list(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.fs_cfg) {
        lua_pushnil(L);
        lua_pushstring(L, "fs.list: not available (declare fs.read in manifest)");
        return 2;
    }
    const char *path = luaL_checkstring(L, 1);

    /* Create the __gc guard FIRST, while it owns nothing: if this allocation OOMs,
     * there is no C result yet to leak. Only after the cap call fills `entries`
     * (with NO intervening Lua allocation) do we hand ownership to the guard, so
     * there is no window in which a Lua longjmp could strand the C array. */
    HlFsListGuard *g = (HlFsListGuard *)lua_newuserdatauv(L, sizeof(HlFsListGuard), 0);
    g->entries = NULL; g->count = 0; g->alloc = NULL;
    luaL_setmetatable(L, HL_FS_LIST_GUARD_MT);

    const char *err = NULL;
    HlFsDirEntry *entries = NULL;
    size_t count = 0;
    int rc = hl_cap_fs_list(lua->base.fs_cfg, path, &entries, &count,
                            lua->base.alloc, &err);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "list_failed");
        return 2;   /* guard owns nothing; its __gc is a no-op */
    }
    g->entries = entries; g->count = count; g->alloc = lua->base.alloc;  /* now owned */

    /* count is bounded by HL_FS_LIST_MAX_ENTRIES (fits int). Any raise below is now
     * covered by the guard's __gc. */
    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; i++) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, entries[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, fs_node_type_name(entries[i].type));
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, (lua_Integer)entries[i].size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }

    /* Success: free promptly and neutralize the guard (its __gc becomes a no-op).
     * The result table is on top; the guard sits below and is discarded on return. */
    hl_cap_fs_list_free(g->entries, g->count, g->alloc);
    g->entries = NULL;
    return 1;
}

static HlMappedBuffer *check_mmap(lua_State *L, int idx)
{
    HlMappedBuffer **pp = luaL_checkudata(L, idx, HL_MMAP_MT);
    if (!pp || !*pp) {
        luaL_error(L, "invalid mapped buffer");
        return NULL; /* unreachable - satisfies static analysis */
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
    {"read",  lua_fs_read},
    {"write", lua_fs_write},
    {"stat",  lua_fs_stat},
    {"list",  lua_fs_list},
    {"mmap",  lua_fs_mmap},
    {NULL, NULL}
};

int luaopen_hull_fs(lua_State *L)
{
    lua_register_mmap_metatable(L);
    lua_register_fs_list_guard(L);
    luaL_newlib(L, fs_funcs);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════
 * Custom require() - module loader with embedded + filesystem fallback
 *
 * Replaces Lua's package.require with a minimal custom version.
 * Search order:
 *   1. Cache (registry "__hull_loaded")
 *   2. Embedded modules (registry "__hull_modules")
 *   3. Filesystem (dev mode - relative requires from app_dir)
 *   4. Error
 *
 * Module namespaces:
 *   hull.*   - Hull stdlib wrappers (e.g. require('hull.json'))
 *   vendor.* - Vendored third-party libs (e.g. require('vendor.json'))
 *   ./path   - Relative to requiring module (filesystem or embedded app)
 *   ../path  - Relative to requiring module (parent traversal)
 * ════════════════════════════════════════════════════════════════════ */

/* ── Path normalization helper ────────────────────────────────────── */

/* normalize_path moved to src/hull/path_normalize.c so the JS module
 * loader can share the same implementation - see the path_normalize.h
 * header for the contract. Local alias kept for call-site readability. */
#define normalize_path(p)  hl_path_normalize(p)

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
            /* No slash - caller is in the root */
            caller_dir[0] = '.';
            caller_dir[1] = '\0';
        }
    } else {
        /* No caller context - use app_dir as base */
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

/*
 * Resolve a RELATIVE require ("./x", "../x") to its canonical embedded-VFS key:
 * the name joined to the requiring module's directory (__hull_current_module)
 * with `.`/`..` collapsed, no extension - e.g. "./../models/user" required from
 * "./routes/users" resolves to "./models/user". This is the SAME normalization
 * the filesystem fallback (resolve_module_path) applies, so a nested relative
 * require resolves in a BUILT binary (modules embedded in __hull_modules under
 * their canonical keys) exactly as it does in dev (modules on disk). Without it
 * the VFS lookup used the literal name and missed on any require needing `..`
 * collapse, so a built modular app failed at runtime with "module not found".
 * A non-relative name is copied through unchanged. Returns 0 on success (out
 * set), -1 on error (too long, or `..` escaping the app root - fail closed).
 */
static int resolve_module_key(lua_State *L, const char *name,
                              char *out, size_t out_size)
{
    if (name[0] != '.') {
        if (strlen(name) >= out_size) return -1;
        memcpy(out, name, strlen(name) + 1);
        return 0;
    }

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_current_module");
    const char *caller = lua_tostring(L, -1);
    char caller_dir[HL_MODULE_PATH_MAX];
    if (caller) {
        const char *last_slash = strrchr(caller, '/');
        if (last_slash) {
            size_t dir_len = (size_t)(last_slash - caller);
            if (dir_len >= sizeof(caller_dir)) { lua_pop(L, 1); return -1; }
            memcpy(caller_dir, caller, dir_len);
            caller_dir[dir_len] = '\0';
        } else {
            caller_dir[0] = '.'; caller_dir[1] = '\0';
        }
    } else {
        caller_dir[0] = '.'; caller_dir[1] = '\0';
    }
    lua_pop(L, 1); /* pop __hull_current_module */

    char joined[HL_MODULE_PATH_MAX];
    int n = snprintf(joined, sizeof(joined), "%s/%s", caller_dir, name);
    if (n < 0 || (size_t)n >= sizeof(joined)) return -1;
    if (normalize_path(joined) != 0) return -1; /* escapes root -> fail closed */
    /* Embedded module keys are app-root-relative with a leading "./"
     * (e.g. "./routes/users"); normalize_path drops the "." segments, so re-add
     * the prefix to match the __hull_modules keys exactly. */
    int m = snprintf(out, out_size, "./%s", joined);
    if (m < 0 || (size_t)m >= out_size) return -1;
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
    HlLua *lua = get_hl_lua(L);

    /* 1. Check cache (registry "__hull_loaded") */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2); /* remove __hull_loaded table */
        return 1;          /* return cached module */
    }
    lua_pop(L, 2); /* pop nil + __hull_loaded */

    /* 2. Capability-aware gate. If the runtime has a resolved module
     * set wired AND the requested name maps to a known first-party
     * module that is not admitted, refuse here with a clear message
     * rather than letting the lookup fall through to "module not
     * found". Names that aren't in the registry (user code, app
     * helpers) fall through unchanged.
     *
     * If module_set isn't wired yet (top-of-file require running
     * before app.manifest()), record the canonical name in the
     * runtime's import tracker. serve.c will validate the tracker
     * against the resolved set after manifest extraction so silent
     * bypass of undeclared imports can't slip through. */
    if (lua) {
        const HlModuleSpec *spec =
            hl_module_registry_find_runtime(name, '.');
        if (spec) {
            if (lua->base.module_set) {
                if (!hl_module_set_contains_spec(lua->base.module_set, spec)) {
                    /* Optional module the build lacks ("hull/gpu@1?"): return
                     * nil so the app can fall back, instead of erroring. */
                    if (hl_module_set_optional_absent_spec(lua->base.module_set,
                                                           spec)) {
                        lua_pushnil(L);
                        return 1;
                    }
                    char deps_buf[128];
                    hl_module_registry_format_deps(spec, deps_buf, sizeof(deps_buf));
                    char deps_part[160] = {0};
                    if (deps_buf[0])
                        snprintf(deps_part, sizeof(deps_part),
                                 " (also needs: %s)", deps_buf);

                    return luaL_error(L,
                        "module '%s' is not declared in app.manifest. "
                        "Add \"%s@%d\" to the modules array%s. "
                        "See `hull modules available` for the full list.",
                        name, spec->name, (int)spec->api_major, deps_part);
                }
            } else {
                /* Only track requires originating from user-level Lua
                 * code. Two non-user paths bypass tracking:
                 *
                 *   1) The C runtime pre-loads hull.json into the
                 *      registry stash before the app file runs (see
                 *      hl_lua_register_modules); there's no Lua frame
                 *      above this require, lua_getstack returns 0.
                 *   2) Stdlib modules that transitively require other
                 *      stdlib (middleware/session → hull.crypto): the
                 *      Lua caller's chunk source starts with "hull.".
                 *      The user's entry-point declaration is what
                 *      matters; auto-admit covers transitive deps.
                 *
                 * Tracking fires only when there IS a Lua caller AND
                 * that caller is NOT in the hull namespace. */
                lua_Debug ar;
                int caller_is_user = lua_getstack(L, 1, &ar) &&
                                     lua_getinfo(L, "S", &ar) &&
                                     ar.source &&
                                     strncmp(ar.source, "hull.", 5) != 0;
                if (caller_is_user)
                    hl_import_tracker_record(&lua->base, spec->name);
            }
        }
    }

    /* 3. Native modules: check Lua's standard loaded table.
     *
     * Each hull.X C module is registered at runtime init via
     * luaL_requiref, which populates LUA_LOADED_TABLE
     * (registry["_LOADED"][name]). The Hull-custom require() doesn't
     * normally consult that table, but for native modules it is the
     * right answer - they are not in __hull_modules (which only holds
     * VFS-embedded .lua files). The `package` global itself is
     * sandboxed out, but the registry table still backs it. */
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, name);
        if (!lua_isnil(L, -1)) {
            /* Stack: loaded, module - cache in __hull_loaded too. */
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
            lua_pushvalue(L, -2);
            lua_setfield(L, -2, name);
            lua_pop(L, 1);            /* pop __hull_loaded */
            lua_remove(L, -2);         /* remove loaded */
            return 1;                  /* module on top */
        }
        lua_pop(L, 1); /* pop nil */
    }
    lua_pop(L, 1); /* pop _LOADED */

    /* 4. Look up in embedded modules table (registry "__hull_modules").
     *
     * Embedded modules are keyed by their canonical app-root-relative path. A
     * relative require must be resolved against the requiring module FIRST (the
     * same normalization the filesystem fallback applies) so a nested
     * `require("./../models/user")` from "./routes/users" looks up the
     * canonical "./models/user" key, not the literal string. `mkey` holds the
     * resolved key for a relative name (falls back to the literal on error, so
     * a `..`-escaping name simply misses and fails closed). */
    char mkey[HL_MODULE_PATH_MAX];
    const char *vkey = name;
    if (name[0] == '.' && resolve_module_key(L, name, mkey, sizeof(mkey)) == 0)
        vkey = mkey;

    /* Canonical-key cache: the module may already be loaded under its canonical
     * key from a DIFFERENT literal require string (the top-of-function cache is
     * keyed by the literal name). Check here so a repeated relative require
     * returns the SAME instance and does not re-execute - matching normal
     * runtime loading and keeping require cycles well-behaved. */
    if (vkey != name) {
        lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
        lua_getfield(L, -1, vkey);
        if (!lua_isnil(L, -1)) {
            /* also cache under the literal name for a faster next hit */
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
            lua_pushvalue(L, -2);
            lua_setfield(L, -2, name);
            lua_pop(L, 1);            /* pop __hull_loaded (2nd) */
            lua_remove(L, -2);         /* remove __hull_loaded (1st) */
            return 1;                  /* cached module on top */
        }
        lua_pop(L, 2); /* pop nil + __hull_loaded */
    }

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_modules");
    lua_getfield(L, -1, vkey);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2); /* remove __hull_modules table */

        /* JSON embedded module → decode raw string with the runtime's
         * cached json.decode. Uses the __hull_json_internal stash so
         * the decode works even when the app doesn't declare
         * hull/json@1 (the user wrote `require("./locales/en.json")`,
         * which is data loading, not stdlib use). */
        size_t nlen = strlen(vkey);
        if (vkey[0] == '.' && vkey[1] == '/' &&
            nlen >= 5 && strcmp(vkey + nlen - 5, ".json") == 0) {
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_json_internal");
            lua_getfield(L, -1, "decode");
            lua_remove(L, -2); /* remove json table */
            lua_pushvalue(L, -2); /* push the JSON string */
            lua_remove(L, -3); /* remove original string */
            if (lua_pcall(L, 1, 1, 0) != LUA_OK)
                return lua_error(L);

            /* Cache in __hull_loaded under BOTH the canonical key and the
             * literal require string, so a later identical require hits the
             * top-of-function cache (which is keyed by the literal name). */
            lua_getfield(L, LUA_REGISTRYINDEX, "__hull_loaded");
            lua_pushvalue(L, -2);
            lua_setfield(L, -2, vkey);
            if (vkey != name) {
                lua_pushvalue(L, -2);
                lua_setfield(L, -2, name);
            }
            lua_pop(L, 1); /* pop __hull_loaded */
            return 1;
        }

        /* Execute under the CANONICAL key so this module's own relative
         * requires resolve against its true app-root-relative path. */
        return execute_and_cache_module(L, vkey);
    }
    lua_pop(L, 2); /* pop nil + __hull_modules */

    /* 5. Filesystem fallback (dev mode - relative requires) */
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

            /* Read the module through the descriptor-relative virtual-root
             * resolver instead of reopening a reconstructed host path (which
             * would follow a module symlink out of the app root, and reintroduce
             * a resolve->check->open TOCTOU). Open the app root as a directory
             * fd and resolve the app-relative module path beneath it with
             * contained-follow semantics: an in-root symlink is followed, a
             * symlink whose target is absolute or escapes the root is clamped to
             * the root (re-rooted), so it can never read a host object outside
             * the app. `path` is the app root + the normalized (no-"..")
             * relative module path; derive that relative tail. */
            const char *root_dir = lua->app_dir;
            const char *relpath = path;
            if (!(root_dir[0] == '.' && root_dir[1] == '\0')) {
                size_t adl = strlen(root_dir);
                while (adl > 0 && root_dir[adl - 1] == '/') adl--;
                relpath = path + adl;
                while (*relpath == '/') relpath++;
            }
            FILE *f = NULL;
            const char *fserr = NULL;
            int root_fd = hl_fs_open_base(root_dir, &fserr);
            if (root_fd >= 0) {
                int mfd = hl_fs_open_at(root_fd, relpath, HL_FS_OPEN_READ,
                                        0, &fserr);
                close(root_fd);
                if (mfd >= 0) {
                    f = fdopen(mfd, "rb");
                    if (!f) close(mfd);
                }
            }
            if (f) {
                if (fseek(f, 0, SEEK_END) != 0) {
                    fclose(f);
                    return luaL_error(L, "seek failed: %s", path);
                }
                long size = ftell(f);
                if (size < 0 || size > HL_MODULE_MAX_SIZE) {
                    fclose(f);
                    return luaL_error(L, "module too large: %s", path);
                }
                if (fseek(f, 0, SEEK_SET) != 0) {
                    fclose(f);
                    return luaL_error(L, "seek failed: %s", path);
                }

                /* Save arena position - buffer is only needed until
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

                /* JSON file → decode with the runtime's cached
                 * json.decode (registry stash, no manifest gate). */
                size_t path_len = strlen(path);
                if (path_len >= 5 &&
                    strcmp(path + path_len - 5, ".json") == 0) {
                    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_json_internal");
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

                /* Compile the chunk - copies data into Lua bytecode */
                int load_ok = luaL_loadbuffer(L, buf, nread, path) == LUA_OK;

                /* Reclaim file buffer - Lua owns the bytecode now */
                lua->scratch->used = arena_saved;

                if (!load_ok)
                    return lua_error(L); /* propagate compile error */

                return execute_and_cache_module(L, path);
            }
        }
    }

    /* Final fall-through. If the user wrote `require("hull.something")`
     * and we got here, it's a real typo (the known-module gate above
     * already short-circuited admitted modules). Probe the registry for
     * a near-match and surface "did you mean?" if anything is close. */
    if (strncmp(name, "hull.", 5) == 0) {
        char short_name[64];
        size_t i = 0;
        const char *src = name + 5;
        while (*src && i + 1 < sizeof(short_name)) {
            short_name[i++] = (*src == '.') ? '/' : *src;
            src++;
        }
        short_name[i] = '\0';
        const HlModuleSpec *guess = hl_module_registry_suggest(short_name);
        if (guess) {
            /* Convert the suggested canonical name back to require()
             * syntax: hull/web/middleware/session → hull.web.middleware.session */
            char hint[128];
            const char *g = guess->name;
            size_t hpos = 0;
            while (*g && hpos + 1 < sizeof(hint)) {
                hint[hpos++] = (*g == '/') ? '.' : *g;
                g++;
            }
            hint[hpos] = '\0';
            return luaL_error(L,
                "module not found: %s - did you mean require(\"%s\")?",
                name, hint);
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
     * Iterates the platform VFS entries, skipping non-Lua-module
     * entries - adding a new .lua file requires no C changes.
     *
     * Skip conditions:
     *   - colon in name => JS module (hull:foo) or context doc
     *     (context:bar). JS modules are loaded by the QuickJS
     *     loader; context docs are not Lua source.
     *   - "static/" prefix => stdlib-shipped static asset
     *     (CSS / JS / image bytes), not Lua source.
     *   - "templates/" prefix => stdlib-shipped template partial,
     *     not Lua source. The template engine resolves these via
     *     hl_vfs_find on the platform VFS at render time. */
    lua_newtable(L);

    if (lua->base.platform_vfs) {
        for (size_t i = 0; i < lua->base.platform_vfs->count; i++) {
            const HlEntry *e = &lua->base.platform_vfs->entries[i];
            if (strchr(e->name, ':')) continue;            /* JS / context */
            if (strncmp(e->name, "static/", 7) == 0) continue;
            if (strncmp(e->name, "templates/", 10) == 0) continue;
            if (hl_lua_load_cached(L, (const char *)e->data, e->len, e->name) != LUA_OK) {
                log_error("[hull:c] failed to load stdlib module '%s': %s",
                          e->name, lua_tostring(L, -1));
                lua_pop(L, 2); /* pop error + modules table */
                return -1;
            }
            lua_setfield(L, -2, e->name);
        }
    }

    /* Load embedded app modules (if any - skip non-Lua entries) */
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
                /* JSON data - store as raw string, decoded on first require() */
                lua_pushlstring(L, (const char *)e->data, e->len);
            } else {
                if (hl_lua_load_cached(L, (const char *)e->data, e->len, e->name) != LUA_OK) {
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

    /* Pre-load hull.json into the Lua registry under
     * __hull_json_internal for runtime-internal use (request-context
     * JSON decoding, embedded .json file decoding, test harness JSON
     * marshalling). User code must declare "hull/json@1" and call
     * require("hull.json") explicitly - log and json are no longer
     * intrinsic as of v0.1.0 release. The internal stash bypasses
     * the manifest gate because it's not user-callable. */
    if (lua->base.platform_vfs) {
        lua_getglobal(L, "require");
        lua_pushstring(L, "hull.json");
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            log_error("[hull:c] failed to pre-load json: %s",
                      lua_tostring(L, -1));
            lua_pop(L, 1);
            return -1;
        }
        lua_setfield(L, LUA_REGISTRYINDEX, "__hull_json_internal");
    }

    return 0;
}
