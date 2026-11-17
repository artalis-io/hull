/*
 * mod_buildctx_fs.c - hull.buildctx._fs: a BuildAction's declared workspace
 * inputs, read through the shipped fs grant policy (docs/buildcontext_design.md
 * §4.1, §13.1). The one native piece of checkpoint 1; the action itself (state
 * machine, observations, manifest, revalidation) is Lua, hull.buildctx.action.
 *
 * Tool VM only: never registered in an app runtime.
 *
 *   local fs = require("hull.buildctx._fs")
 *   local g, tok = fs.open(root, { "src/", "app.lua" })   -- compiled read grants
 *   g:read(path, max)  -> bytes, sha256_hex   | nil, token
 *   g:stat(path)       -> type, size, exec    | nil, token   (type "absent" when missing)
 *   g:list(path)       -> { {name, type}... }, projection_sha256_hex | nil, token
 *   g:close()
 *
 * Grants compile with hl_fs_policy_compile_manifest (read set only); reads
 * resolve through the selected entry's held anchor (a SUBTREE follows links
 * inside it, clamped at it; EXACT / PATTERN refuse links), stat and list
 * through hl_cap_fs_stat / hl_cap_fs_list. Tokens are the resolver's, plus
 * "limit" (a read over @p max). Types: "file", "dir", "symlink", "other".
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mod_buildctx_fs.h"

#include "hull/cap/crypto.h"
#include "hull/cap/fs.h"
#include "hull/cap/fs_policy.h"
#include "hull/cap/fs_resolve.h"
#include "hull/utils/alloc.h"
#include "../../utils/hex.h"

#include "lauxlib.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BCFS_MT       "hull.buildctx._fs"
#define BCFS_PATH_MAX 4096

typedef struct {
    HlAllocator alloc;
    HlFsPolicy  policy;
    HlFsConfig  cfg;
    char        root[BCFS_PATH_MAX];
    int         open;
    /* A read's descriptor and a listing in progress: when a Lua allocation
     * error unwinds past them, the next call or the finalizer releases them. */
    int           fd;
    HlFsDirEntry *ents;
    size_t        ents_n;
} BcFs;

static const char *const TYPE_NAMES[] = { "file", "dir", "symlink", "other" };

static const char *type_name(HlFsNodeType t)
{
    return (unsigned)t <= HL_FS_NODE_OTHER ? TYPE_NAMES[t] : "other";
}

static void drop_fd(BcFs *g)
{
    if (g->fd >= 0) { close(g->fd); g->fd = -1; }
}

static void drop_list(BcFs *g)
{
    if (g->ents) hl_cap_fs_list_free(g->ents, g->ents_n, &g->alloc);
    g->ents = NULL;
    g->ents_n = 0;
}

static void bcfs_close(BcFs *g)
{
    drop_fd(g);
    drop_list(g);
    if (g->open) { hl_fs_policy_free(&g->policy); g->open = 0; }
}

static BcFs *check_open(lua_State *L)
{
    BcFs *g = luaL_checkudata(L, 1, BCFS_MT);
    if (!g->open) luaL_error(L, "hull.buildctx._fs: closed");
    drop_fd(g);
    drop_list(g);
    return g;
}

static int fail(lua_State *L, const char *tok)
{
    lua_pushnil(L);
    lua_pushstring(L, tok ? tok : "io_error");
    return 2;
}

static void push_hex(lua_State *L, const uint8_t sha[32])
{
    char hex[65];
    hl_hex_encode(sha, 32, hex, sizeof(hex));
    lua_pushlstring(L, hex, 64);
}

/* fs.open(root, grants) */
static int l_open(lua_State *L)
{
    size_t rlen;
    const char *root = luaL_checklstring(L, 1, &rlen);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (rlen == 0 || rlen >= BCFS_PATH_MAX || root[0] != '/') return fail(L, "invalid_args");
    lua_Integer n = luaL_len(L, 2);
    if (n < 0 || n > 4096) return fail(L, "invalid_args");
    const char **grants = lua_newuserdatauv(L, (size_t)(n ? n : 1) * sizeof(char *), 0);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_type(L, -1) != LUA_TSTRING) return fail(L, "invalid_args");
        grants[i - 1] = lua_tostring(L, -1);   /* kept alive by the grants table */
        lua_pop(L, 1);
    }

    BcFs *g = lua_newuserdatauv(L, sizeof(*g), 0);
    memset(g, 0, sizeof(*g));
    g->fd = -1;     /* ents NULL */
    g->policy = HL_FS_POLICY_INIT;
    hl_alloc_init(&g->alloc, 0);
    memcpy(g->root, root, rlen + 1);
    luaL_setmetatable(L, BCFS_MT);
    const char *err = NULL;
    if (hl_fs_policy_compile_manifest(g->root, &g->alloc, grants, (size_t)n, NULL, 0,
                                      &g->policy, &err) != 0)
        return fail(L, err ? err : "invalid_path");
    g->open = 1;
    g->cfg.base_dir = g->root;
    g->cfg.base_len = rlen;
    g->cfg.policy = &g->policy;
    return 1;
}

/* g:read(path, max) */
static int l_read(lua_State *L)
{
    BcFs *g = check_open(L);
    const char *path = luaL_checkstring(L, 2);
    lua_Integer max = luaL_checkinteger(L, 3);
    if (max < 0) return fail(L, "invalid_args");

    char scratch[BCFS_PATH_MAX + 2];
    HlFsSelection sel = hl_fs_policy_select(&g->policy, path, HL_FS_OPEN_READ,
                                            scratch, sizeof(scratch));
    if (!sel.entry) return fail(L, sel.err ? sel.err : "permission");
    HlFsSymlink sym = sel.entry->kind == HL_FS_ENTRY_SUBTREE ? HL_FS_SYMLINK_FOLLOW
                                                             : HL_FS_SYMLINK_REFUSE;
    const char *err = NULL;
    g->fd = hl_fs_open_at_ex(sel.entry->anchor_fd, sel.residual, HL_FS_OPEN_READ, sym, 0, &err);
    if (g->fd < 0) return fail(L, err);

    struct stat st;
    if (fstat(g->fd, &st) != 0 || !S_ISREG(st.st_mode)) { drop_fd(g); return fail(L, "io_error"); }
    if ((uint64_t)st.st_size > (uint64_t)max) { drop_fd(g); return fail(L, "limit"); }

    /* One byte past the size: a file that grew while it was read is read to its
     * end (and refused past max). The buffer may raise; the fd is closed then
     * by the next call or the finalizer. */
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    HlSha256Ctx h;
    hl_cap_crypto_sha256_init(&h);
    size_t total = 0;
    for (;;) {
        char *p = luaL_prepbuffer(&b);
        ssize_t r = read(g->fd, p, LUAL_BUFFERSIZE);
        if (r < 0) {
            if (errno == EINTR) continue;
            drop_fd(g);
            return fail(L, "io_error");
        }
        if (r == 0) break;
        total += (size_t)r;
        if (total > (size_t)max) { drop_fd(g); return fail(L, "limit"); }
        hl_cap_crypto_sha256_update(&h, p, (size_t)r);
        luaL_addsize(&b, (size_t)r);
    }
    drop_fd(g);
    uint8_t sha[32];
    hl_cap_crypto_sha256_final(&h, sha);
    luaL_pushresult(&b);
    push_hex(L, sha);
    return 2;
}

/* g:stat(path) */
static int l_stat(lua_State *L)
{
    BcFs *g = check_open(L);
    const char *path = luaL_checkstring(L, 2);
    HlFsStatInfo info;
    const char *err = NULL;
    int r = hl_cap_fs_stat(&g->cfg, path, &info, &err);
    if (r == 1) { lua_pushliteral(L, "absent"); return 1; }
    if (r != 0) return fail(L, err);
    lua_pushstring(L, type_name(info.type));
    lua_pushinteger(L, (lua_Integer)info.size);
    lua_pushboolean(L, (info.mode & 0111) != 0);
    return 3;
}

/* g:list(path): names and types, and the hash of that projection (each name,
 * a NUL, its type, a newline) - sizes are not part of it. */
static int l_list(lua_State *L)
{
    BcFs *g = check_open(L);
    const char *path = luaL_checkstring(L, 2);
    const char *err = NULL;
    if (hl_cap_fs_list(&g->cfg, path, &g->ents, &g->ents_n, &g->alloc, &err) != 0) {
        g->ents = NULL;
        g->ents_n = 0;
        return fail(L, err);
    }
    HlSha256Ctx h;
    hl_cap_crypto_sha256_init(&h);
    lua_createtable(L, (int)g->ents_n, 0);
    for (size_t i = 0; i < g->ents_n; i++) {
        const char *name = g->ents[i].name, *tn = type_name(g->ents[i].type);
        hl_cap_crypto_sha256_update(&h, name, strlen(name) + 1);
        hl_cap_crypto_sha256_update(&h, tn, strlen(tn));
        hl_cap_crypto_sha256_update(&h, "\n", 1);
        lua_createtable(L, 0, 2);
        lua_pushstring(L, name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, tn);
        lua_setfield(L, -2, "type");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    drop_list(g);
    uint8_t sha[32];
    hl_cap_crypto_sha256_final(&h, sha);
    push_hex(L, sha);
    return 2;
}

static int l_close(lua_State *L)
{
    bcfs_close(luaL_checkudata(L, 1, BCFS_MT));
    return 0;
}

static const luaL_Reg METHODS[] = {
    { "read",  l_read  },
    { "stat",  l_stat  },
    { "list",  l_list  },
    { "close", l_close },
    { NULL, NULL },
};

int luaopen_hull_buildctx_fs(lua_State *L)
{
    if (luaL_newmetatable(L, BCFS_MT)) {
        luaL_newlib(L, METHODS);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_close);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, l_close);
        lua_setfield(L, -2, "__close");
    }
    lua_pop(L, 1);
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_open);
    lua_setfield(L, -2, "open");
    return 1;
}
