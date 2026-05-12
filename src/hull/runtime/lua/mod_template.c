/* mod_template.c — hull._template module: internal bridge for stdlib
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/limits.h"
#include "hull/vfs.h"

#include <sh_arena.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * hull._template module (internal — called only by stdlib hull.template)
 *
 * _template._compile(code)    → compiled Lua function
 * _template._load_raw(name)   → raw template string or nil
 * ════════════════════════════════════════════════════════════════════ */

/* _template._compile(code) — compile generated Lua source to a function */
static int lua_template_compile(lua_State *L)
{
    size_t len;
    const char *code = luaL_checklstring(L, 1, &len);
    const char *name = luaL_optstring(L, 2, "=template");

    if (luaL_loadbuffer(L, code, len, name) != LUA_OK)
        return lua_error(L); /* propagate compile error */

    /* loadbuffer pushes a function — call it to get the inner function */
    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        return lua_error(L);

    return 1; /* compiled function on stack */
}

/* _template._load_raw(name) — load raw template bytes from embedded
 * entries or filesystem fallback. Returns string or nil. */
static int lua_template_load_raw(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    /* Quick-reject for both VFS and filesystem paths */
    if (name[0] == '/' || name[0] == '\0')
        return luaL_error(L, "invalid template name: %s", name);

    /* 1. Search embedded template entries via VFS */
    HlLua *lua = get_hl_lua(L);
    if (lua && lua->base.app_vfs) {
        char tpl_name[HL_MODULE_PATH_MAX];
        int n = snprintf(tpl_name, sizeof(tpl_name), "templates/%s", name);
        if (n > 0 && (size_t)n < sizeof(tpl_name)) {
            const HlEntry *e = hl_vfs_find(lua->base.app_vfs, tpl_name);
            if (e) {
                lua_pushlstring(L, (const char *)e->data, e->len);
                return 1;
            }
        }
    }

    /* 2. Filesystem fallback (dev mode): app_dir/templates/<name> */
    if (lua && lua->app_dir) {
        /* Reject ".." components to prevent path traversal */
        const char *p = name;
        while (*p) {
            if (p[0] == '.' && p[1] == '.' &&
                (p[2] == '/' || p[2] == '\0'))
                return luaL_error(L, "invalid template name: %s", name);
            const char *slash = strchr(p, '/');
            if (!slash) break;
            p = slash + 1;
        }

        char path[HL_MODULE_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/templates/%s",
                         lua->app_dir, name);
        if (n > 0 && (size_t)n < sizeof(path)) {
            /* Verify resolved path stays within app_dir (symlink escape check).
             * Canonicalize app_dir too — it may be a relative path when
             * invoked as `hull test relative/path/`. */
            char resolved[PATH_MAX];
            if (realpath(path, resolved)) {
                char real_app_dir[PATH_MAX];
                if (!realpath(lua->app_dir, real_app_dir))
                    return luaL_error(L, "invalid template name: %s", name);
                size_t app_dir_len = strlen(real_app_dir);
                if (strncmp(resolved, real_app_dir, app_dir_len) != 0 ||
                    (resolved[app_dir_len] != '/' && resolved[app_dir_len] != '\0'))
                    return luaL_error(L, "invalid template name: %s", name);
            }

            FILE *f = fopen(path, "rb");
            if (f) {
                if (fseek(f, 0, SEEK_END) != 0) {
                    fclose(f);
                    return luaL_error(L, "seek failed: %s", name);
                }
                long size = ftell(f);
                if (size < 0 || size > HL_MODULE_MAX_SIZE) {
                    fclose(f);
                    return luaL_error(L, "template too large: %s", name);
                }
                if (fseek(f, 0, SEEK_SET) != 0) {
                    fclose(f);
                    return luaL_error(L, "seek failed: %s", name);
                }

                /* Use scratch arena — Lua copies the string */
                size_t arena_saved = lua->scratch->used;
                char *buf = sh_arena_alloc(lua->scratch, (size_t)size);
                if (!buf) {
                    fclose(f);
                    return luaL_error(L, "out of memory loading: %s", name);
                }
                size_t nread = fread(buf, 1, (size_t)size, f);
                int read_err = ferror(f);
                fclose(f);

                if (read_err || nread != (size_t)size) {
                    lua->scratch->used = arena_saved;
                    return luaL_error(L, "read error: %s", name);
                }

                lua_pushlstring(L, buf, nread);
                lua->scratch->used = arena_saved;
                return 1;
            }
        }
    }

    lua_pushnil(L);
    return 1;
}

static const luaL_Reg template_funcs[] = {
    {"_compile",  lua_template_compile},
    {"_load_raw", lua_template_load_raw},
    {NULL, NULL}
};

int luaopen_hull_template_bridge(lua_State *L)
{
    luaL_newlib(L, template_funcs);
    return 1;
}
