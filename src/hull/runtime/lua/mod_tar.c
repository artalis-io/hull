/* mod_tar.c - hull.tar module: ustar archive parse / create / extract / pack
 *
 * Thin bindings over the shared C core (cap/tar.h). The format primitives
 * (parse/create) are pure byte<->table transforms and accept any buffer type
 * (string / MappedBuffer / WasmBuffer) via the unified buffer protocol. The
 * ergonomic helpers (extract/pack) COMPOSE the fs capability (hl_cap_fs_*) so
 * app-side archive I/O goes through the manifest fs allowlist + path validation
 * exactly like fs.read/fs.write - NOT the trusted hl_tar_extract install path.
 *
 *   tar.parse(bytes)          -> { {name,data,size,mode,is_dir}, ... } | nil,err
 *   tar.create(entries)       -> bytes | nil,err
 *   tar.extract(bytes, dir)   -> true | nil,err   (writes via fs.write)
 *   tar.pack(files [, opts])  -> bytes | nil,err   (reads via fs.read)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/tar.h"
#include "hull/cap/fs.h"
#include "hull/utils/alloc.h"

#include <stdlib.h>
#include <string.h>

/* ── tar.parse ─────────────────────────────────────────────────────── */

/* Collect each member into a Lua array-of-tables at a fixed stack slot. */
struct parse_ctx {
    lua_State *L;
    int        tbl;   /* stack index of the result table */
    int        n;     /* count so far */
};

static int parse_collect(const HlTarEntry *e, void *vctx)
{
    struct parse_ctx *c = (struct parse_ctx *)vctx;
    lua_State *L = c->L;

    lua_newtable(L);
    lua_pushstring(L, e->name);
    lua_setfield(L, -2, "name");
    /* data is a copy (parse borrows into the input; the Lua string owns bytes) */
    lua_pushlstring(L, e->data ? (const char *)e->data : "", e->size);
    lua_setfield(L, -2, "data");
    lua_pushinteger(L, (lua_Integer)e->size);
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, (lua_Integer)e->mode);
    lua_setfield(L, -2, "mode");
    lua_pushboolean(L, e->is_dir);
    lua_setfield(L, -2, "is_dir");

    lua_rawseti(L, c->tbl, ++c->n);
    return 0;
}

/* tar.parse(bytes) -> array of entries | nil, err */
static int l_tar_parse(lua_State *L)
{
    HlBufferView view;
    if (!lua_get_buffer(L, 1, &view)) {
        lua_pushnil(L);
        lua_pushstring(L, "tar.parse: arg 1 must be a buffer (string/mmap/wasm)");
        return 2;
    }

    lua_newtable(L);                 /* result table at the top */
    struct parse_ctx c = { L, lua_gettop(L), 0 };
    int rc = hl_tar_parse((const unsigned char *)view.data, view.len,
                          parse_collect, &c);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "tar.parse: malformed or unsafe archive");
        return 2;
    }
    /* result table already on top */
    return 1;
}

/* ── tar.create ────────────────────────────────────────────────────── */

/* Read the entries array at stack index 1 into a C HlTarEntry array. The
 * borrowed name/data pointers stay valid while arg 1 (and its nested strings)
 * remain on the stack. Returns a malloc'd array (caller frees) + count, or
 * NULL on a shape error (msg set). */
static HlTarEntry *read_entries(lua_State *L, int idx, size_t *out_n,
                                const char **msg)
{
    if (lua_type(L, idx) != LUA_TTABLE) {
        *msg = "tar.create: arg 1 must be a table of entries";
        return NULL;
    }
    lua_Integer n = luaL_len(L, idx);
    if (n < 0) { *msg = "tar.create: bad entry count"; return NULL; }
    if (n == 0) { *out_n = 0; return NULL; }   /* empty -> caller handles */

    HlTarEntry *ents = (HlTarEntry *)calloc((size_t)n, sizeof(HlTarEntry));
    if (!ents) { *msg = "out_of_memory"; return NULL; }

    for (lua_Integer i = 1; i <= n; i++) {
        lua_geti(L, idx, i);                    /* entry table */
        if (lua_type(L, -1) != LUA_TTABLE) {
            lua_pop(L, 1);
            free(ents);
            *msg = "tar.create: each entry must be a table";
            return NULL;
        }
        HlTarEntry *e = &ents[i - 1];

        lua_getfield(L, -1, "name");
        e->name = lua_tostring(L, -1);          /* borrowed (kept by entry tbl) */
        lua_pop(L, 1);
        if (!e->name) {
            lua_pop(L, 1);
            free(ents);
            *msg = "tar.create: entry missing 'name'";
            return NULL;
        }

        lua_getfield(L, -1, "is_dir");
        e->is_dir = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "mode");
        e->mode = (unsigned)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        if (!e->is_dir) {
            lua_getfield(L, -1, "data");
            size_t len = 0;
            const char *d = lua_tolstring(L, -1, &len);  /* borrowed */
            lua_pop(L, 1);
            e->data = (const unsigned char *)(d ? d : "");
            e->size = d ? len : 0;
        }
        lua_pop(L, 1);                           /* entry table */
    }
    *out_n = (size_t)n;
    return ents;
}

/* tar.create(entries) -> bytes | nil, err */
static int l_tar_create(lua_State *L)
{
    size_t n = 0;
    const char *msg = NULL;
    HlTarEntry *ents = read_entries(L, 1, &n, &msg);
    if (!ents && n != 0) {          /* real error (n==0 is the empty-array case) */
        lua_pushnil(L);
        lua_pushstring(L, msg ? msg : "tar.create: bad entries");
        return 2;
    }

    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc = hl_tar_create(ents, n, &out, &out_len);
    free(ents);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "tar.create: unsafe name or out of memory");
        return 2;
    }
    lua_pushlstring(L, (const char *)out, out_len);
    free(out);
    return 1;
}

/* ── tar.extract (fs-composed, sandboxed) ──────────────────────────── */

struct extract_ctx {
    const HlFsConfig *fs;
    const char       *dir;     /* destination prefix (relative, fs-validated) */
    const char       *err;     /* set on first failure */
};

static int extract_write(const HlTarEntry *e, void *vctx)
{
    struct extract_ctx *c = (struct extract_ctx *)vctx;
    if (e->is_dir) return 0;   /* fs.write creates parents; empty dirs dropped */

    /* Build "<dir>/<name>". Both halves are already traversal-checked (dir by
     * the fs cap on write, name by hl_tar_parse), but keep it bounded. */
    char path[4096];
    int pn;
    if (c->dir && c->dir[0])
        pn = snprintf(path, sizeof(path), "%s/%s", c->dir, e->name);
    else
        pn = snprintf(path, sizeof(path), "%s", e->name);
    if (pn < 0 || (size_t)pn >= sizeof(path)) { c->err = "path too long"; return -1; }

    int rc = hl_cap_fs_write(c->fs, path, (const char *)(e->data ? e->data : (const unsigned char *)""),
                             e->size, &c->err);
    return rc == 0 ? 0 : -1;
}

/* tar.extract(bytes, dest_dir) -> true | nil, err */
static int l_tar_extract(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.fs_cfg) {
        lua_pushnil(L);
        lua_pushstring(L, "tar.extract: not available (declare fs.write in manifest)");
        return 2;
    }
    HlBufferView view;
    if (!lua_get_buffer(L, 1, &view)) {
        lua_pushnil(L);
        lua_pushstring(L, "tar.extract: arg 1 must be a buffer");
        return 2;
    }
    const char *dir = luaL_optstring(L, 2, "");

    struct extract_ctx c = { lua->base.fs_cfg, dir, NULL };
    int rc = hl_tar_parse((const unsigned char *)view.data, view.len,
                          extract_write, &c);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, c.err ? c.err : "tar.extract: malformed or unsafe archive");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ── tar.pack (fs-composed, sandboxed) ─────────────────────────────── */

/* tar.pack(files) -> bytes | nil, err
 *
 * `files` is an array; each element is either a path string (the archive
 * member name is the path) or a table { path=, name?= }. Each file is read
 * through hl_cap_fs_read (manifest fs.read allowlist). */
static int l_tar_pack(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.fs_cfg) {
        lua_pushnil(L);
        lua_pushstring(L, "tar.pack: not available (declare fs.read in manifest)");
        return 2;
    }
    if (lua_type(L, 1) != LUA_TTABLE) {
        lua_pushnil(L);
        lua_pushstring(L, "tar.pack: arg 1 must be an array of files");
        return 2;
    }
    lua_Integer n = luaL_len(L, 1);
    if (n <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, "tar.pack: no files");
        return 2;
    }

    HlTarEntry *ents = (HlTarEntry *)calloc((size_t)n, sizeof(HlTarEntry));
    /* Parallel array of read buffers to free after create. */
    char **bufs = (char **)calloc((size_t)n, sizeof(char *));
    if (!ents || !bufs) {
        free(ents); free(bufs);
        lua_pushnil(L);
        lua_pushstring(L, "out_of_memory");
        return 2;
    }

    const char *err = NULL;
    lua_Integer built = 0;
    for (lua_Integer i = 1; i <= n; i++) {
        lua_geti(L, 1, i);
        const char *path = NULL, *name = NULL;
        if (lua_type(L, -1) == LUA_TSTRING) {
            path = lua_tostring(L, -1);
            name = path;
        } else if (lua_type(L, -1) == LUA_TTABLE) {
            lua_getfield(L, -1, "path"); path = lua_tostring(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "name"); name = lua_tostring(L, -1); lua_pop(L, 1);
            if (!name) name = path;
        }
        if (!path) { lua_pop(L, 1); err = "tar.pack: entry needs a path"; break; }

        int64_t size = hl_cap_fs_read(lua->base.fs_cfg, path, NULL, 0, &err);
        if (size < 0) { lua_pop(L, 1); break; }
        char *buf = (char *)malloc(size ? (size_t)size : 1);
        if (!buf) { lua_pop(L, 1); err = "out_of_memory"; break; }
        if (size > 0) {
            int64_t got = hl_cap_fs_read(lua->base.fs_cfg, path, buf, (size_t)size, &err);
            if (got < 0) { free(buf); lua_pop(L, 1); break; }
        }
        bufs[i - 1] = buf;
        HlTarEntry *e = &ents[i - 1];
        e->name = name;                 /* borrowed from arg-1 nested string */
        e->data = (const unsigned char *)buf;
        e->size = (size_t)size;   /* size >= 0 here (guarded above) */
        e->mode = 0644;
        e->is_dir = 0;
        built++;
        lua_pop(L, 1);
    }

    unsigned char *out = NULL;
    size_t out_len = 0;
    int rc = -1;
    if (!err && built == n)
        rc = hl_tar_create(ents, (size_t)n, &out, &out_len);

    for (lua_Integer i = 0; i < n; i++) free(bufs[i]);
    free(bufs);
    free(ents);

    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "tar.pack: failed");
        return 2;
    }
    lua_pushlstring(L, (const char *)out, out_len);
    free(out);
    return 1;
}

/* ── Registration ──────────────────────────────────────────────────── */

static const luaL_Reg tar_funcs[] = {
    { "parse",   l_tar_parse   },
    { "create",  l_tar_create  },
    { "extract", l_tar_extract },
    { "pack",    l_tar_pack    },
    { NULL, NULL }
};

int luaopen_hull_tar(lua_State *L)
{
    luaL_newlib(L, tar_funcs);
    return 1;
}
