/*
 * mod_buildctx.c - the native pieces of a BuildAction (docs/buildcontext_design.md
 * §13.1, §13.2); the action itself (state machine, observations, outputs,
 * manifests, revalidation) is Lua, hull.buildctx.action.
 *
 * hull.buildctx._fs: the declared workspace inputs, read through the shipped
 * fs grant policy (§4.1).
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
#include "mod_buildctx.h"

#include "hull/cap/crypto.h"
#include "hull/runtime/cache_common.h"
#include "hull/shared/blob_store.h"
#include "hull/cap/fs.h"
#include "hull/cap/fs_policy.h"
#include "hull/cap/fs_resolve.h"
#include "hull/manifest.h"
#include "hull/shared/build_plugin.h"
#include "hull/utils/alloc.h"
#include "../../utils/hex.h"
#include "plain_value.h"

#include "lauxlib.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HL_CHAN_SUPPORTED
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#endif

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

/* ── hull.buildctx._store ──────────────────────────────────────────────
 *
 * The result store (§2.2, §13.2): one blob store at an absolute root, every
 * entry sealed under the tool key ($HOME/.hull/tool-cache.key, which no app
 * process loads), so an app that can write the cache pool cannot forge one.
 *
 *   local st, tok = store.open(root)
 *   st:put_payload(bytes)  -> sha256_hex | nil, token   (keyed by its own hash)
 *   st:get_payload(sha)    -> bytes      | nil, token   ("miss", "corrupt")
 *   st:put_result(key, b)  -> true       | nil, token   (key: 64 lowercase hex)
 *   st:get_result(key)     -> bytes      | nil, "miss"
 *   st:close()
 *
 * Payloads and results are sealed in separate domains, so a payload never
 * verifies as a result. A put is atomic (temp + rename). A payload's key is
 * its hash, so a second put of it is a no-op; a result's key is the action's
 * pre-run key, which a later run with other dependencies shares (a stale
 * result), so put_result replaces what is there.
 */

#define BCST_MT          "hull.buildctx._store"
#define BCST_PAYLOAD     "build-payload"
#define BCST_RESULT      "build-result"

typedef struct {
    HlBlobStore *store;
    uint8_t     *pending;   /* a get in progress, freed by the next call or the finalizer */
} BcStore;

static void st_drop_pending(BcStore *st)
{
    free(st->pending);
    st->pending = NULL;
}

static BcStore *st_check(lua_State *L)
{
    BcStore *st = luaL_checkudata(L, 1, BCST_MT);
    if (!st->store) luaL_error(L, "hull.buildctx._store: closed");
    st_drop_pending(st);
    return st;
}

static int hex_key_ok(const char *k, size_t n)
{
    if (n != 64) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((k[i] >= '0' && k[i] <= '9') || (k[i] >= 'a' && k[i] <= 'f'))) return 0;
    return 1;
}

static int st_open(lua_State *L)
{
    size_t rlen;
    const char *root = luaL_checklstring(L, 1, &rlen);
    if (rlen == 0 || root[0] != '/') return fail(L, "invalid_args");
    BcStore *st = lua_newuserdatauv(L, sizeof(*st), 0);
    st->store = NULL;
    st->pending = NULL;
    luaL_setmetatable(L, BCST_MT);
    if (hl_blob_store_open(&st->store, NULL, root, /*shard_depth=*/1, 0) != 0) {
        st->store = NULL;
        return fail(L, "io_error");
    }
    return 1;
}

static int st_put_payload(lua_State *L)
{
    BcStore *st = st_check(L);
    size_t len;
    const char *bytes = luaL_checklstring(L, 2, &len);
    uint8_t sha[32];
    char hex[65];
    hl_cap_crypto_sha256(bytes, len, sha);
    hl_hex_encode(sha, 32, hex, sizeof(hex));
    if (hl_tool_cache_put_sealed(st->store, BCST_PAYLOAD, hex, (const uint8_t *)bytes, len) != 0)
        return fail(L, "io_error");
    lua_pushlstring(L, hex, 64);
    return 1;
}

static int st_get_payload(lua_State *L)
{
    BcStore *st = st_check(L);
    size_t klen;
    const char *key = luaL_checklstring(L, 2, &klen);
    if (!hex_key_ok(key, klen)) return fail(L, "invalid_args");
    size_t len = 0;
    if (hl_tool_cache_get_sealed(st->store, BCST_PAYLOAD, key, &st->pending, &len) != 0) {
        st->pending = NULL;
        return fail(L, "miss");
    }
    uint8_t sha[32];
    char hex[65];
    hl_cap_crypto_sha256(st->pending, len, sha);
    hl_hex_encode(sha, 32, hex, sizeof(hex));
    if (memcmp(hex, key, 64) != 0) { st_drop_pending(st); return fail(L, "corrupt"); }
    lua_pushlstring(L, (const char *)st->pending, len);
    st_drop_pending(st);
    return 1;
}

static int st_put_result(lua_State *L)
{
    BcStore *st = st_check(L);
    size_t klen, len;
    const char *key = luaL_checklstring(L, 2, &klen);
    const char *bytes = luaL_checklstring(L, 3, &len);
    if (!hex_key_ok(key, klen)) return fail(L, "invalid_args");
    /* Replace: the keyed put skips a present key. A lookup between the two
     * finds no result and runs the action itself. */
    if (hl_blob_store_delete(st->store, key) < 0) return fail(L, "io_error");
    if (hl_tool_cache_put_sealed(st->store, BCST_RESULT, key, (const uint8_t *)bytes, len) != 0)
        return fail(L, "io_error");
    lua_pushboolean(L, 1);
    return 1;
}

static int st_get_result(lua_State *L)
{
    BcStore *st = st_check(L);
    size_t klen;
    const char *key = luaL_checklstring(L, 2, &klen);
    if (!hex_key_ok(key, klen)) return fail(L, "invalid_args");
    size_t len = 0;
    if (hl_tool_cache_get_sealed(st->store, BCST_RESULT, key, &st->pending, &len) != 0) {
        st->pending = NULL;
        return fail(L, "miss");
    }
    lua_pushlstring(L, (const char *)st->pending, len);
    st_drop_pending(st);
    return 1;
}

static int st_close(lua_State *L)
{
    BcStore *st = luaL_checkudata(L, 1, BCST_MT);
    st_drop_pending(st);
    if (st->store) { hl_blob_store_close(st->store); st->store = NULL; }
    return 0;
}

static const luaL_Reg STORE_METHODS[] = {
    { "put_payload", st_put_payload },
    { "get_payload", st_get_payload },
    { "put_result",  st_put_result  },
    { "get_result",  st_get_result  },
    { "close",       st_close       },
    { NULL, NULL },
};

int luaopen_hull_buildctx_store(lua_State *L)
{
    if (luaL_newmetatable(L, BCST_MT)) {
        luaL_newlib(L, STORE_METHODS);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, st_close);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, st_close);
        lua_setfield(L, -2, "__close");
    }
    lua_pop(L, 1);
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, st_open);
    lua_setfield(L, -2, "open");
    return 1;
}

/* ── hull.buildctx._plugin ─────────────────────────────────────────────
 *
 * A build plugin's process and its channel (§9.1, shared/build_plugin.h),
 * seen from the host. The supervisor - one process per plugin per run, the
 * action loop, the host operations - is Lua (hull.buildctx.plugin); this is
 * the part that holds a socket and a pid.
 *
 *   local P = require("hull.buildctx._plugin")
 *   local p, tok = P.spawn(exe, entry, args)  -- `exe [args...] entry`, the
 *                                             -- channel on its fd 3
 *   p:send(type, id, value?, timeout_ms?)  -> true        | nil, token
 *   p:recv(timeout_ms)                     -> type, id, v | nil, token
 *   p:close(grace_ms)                      -> exit code | "signal" | "killed"
 *   p:kill()                               -- SIGKILL now, reaped
 *   P.bytes(s)                             -- a bytes value (a string is text)
 *   P.now()                                -- the monotonic clock, in ms
 *   P.empty_array                          -- encodes as []
 *   P.preset_check(manifest)               -> true | nil, reason
 *
 * Tokens: "unsupported" (no channel on this host), "spawn_failed" (and the
 * system's reason),
 * "timeout", "eof" (the plugin closed its end or exited), "protocol" (a
 * frame with another magic or past the payload limit), "malformed" (a value
 * outside the value model), "too_big", "unsupported_value", "closed".
 * Every error but "timeout" on recv leaves the process unusable: the caller
 * kills it.
 */

#define BCPL_MT "hull.buildctx._plugin"

typedef struct {
    int   fd;
    pid_t pid;      /* 0 once reaped */
} BcProc;

#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#    define HL_BUILDCTX_SANITIZED 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define HL_BUILDCTX_SANITIZED 1
#endif

#ifdef HL_CHAN_SUPPORTED
/* What a plugin inherits: its home (the code caches and their key) and the
 * cache knobs. Never a secret, a database URL or a path list. A sanitizer
 * build also passes the sanitizer options, so its tests can keep the leak
 * checker's exit-time work out of the plugin's kernel sandbox. */
static int plugin_env_ok(const char *kv)
{
    static const char *const keep[] = { "HOME=", "TMPDIR=", "HULL_CACHE_DIR=",
                                        "HULL_NO_",
#ifdef HL_BUILDCTX_SANITIZED
                                        "ASAN_OPTIONS=", "LSAN_OPTIONS=", "UBSAN_OPTIONS=",
                                        "MSAN_OPTIONS=",
#endif
                                        NULL };
    for (int i = 0; keep[i]; i++)
        if (strncmp(kv, keep[i], strlen(keep[i])) == 0) return 1;
    return 0;
}

static void proc_kill(BcProc *p)
{
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
    if (p->pid > 0) {
        int st;
        (void)kill(p->pid, SIGKILL);
        (void)hl_chan_reap(p->pid, &st, 5000, NULL);
        p->pid = 0;
    }
}
#else
static void proc_kill(BcProc *p) { (void)p; }
#endif

static BcProc *proc_check(lua_State *L)
{
    BcProc *p = luaL_checkudata(L, 1, BCPL_MT);
    if (p->fd < 0) luaL_error(L, "hull.buildctx._plugin: closed");
    return p;
}

/* P.spawn(exe, entry, args) */
static int pl_spawn(lua_State *L)
{
    const char *exe = luaL_checkstring(L, 1);
    const char *entry = luaL_checkstring(L, 2);
    lua_Integer n = lua_istable(L, 3) ? luaL_len(L, 3) : 0;
    if (n < 0 || n > 32) return fail(L, "invalid_args");
#ifdef HL_CHAN_SUPPORTED
    const char *argv[32 + 3];
    int k = 0;
    argv[k++] = exe;
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 3, i);
        if (lua_type(L, -1) != LUA_TSTRING) return fail(L, "invalid_args");
        argv[k++] = lua_tostring(L, -1);   /* kept alive by the args table */
        lua_pop(L, 1);
    }
    argv[k++] = entry;
    argv[k] = NULL;

    BcProc *p = lua_newuserdatauv(L, sizeof *p, 0);
    p->fd = -1;
    p->pid = 0;
    luaL_setmetatable(L, BCPL_MT);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return fail(L, "spawn_failed");
    (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(sv[1], F_SETFD, FD_CLOEXEC);   /* dup2'd onto the child's 3 */
    hl_chan_set_nonblock(sv[0]);
    hl_chan_set_nosigpipe(sv[0]);
    static const char *const extra[] = { HL_BUILD_PLUGIN_ENV "=3", NULL };
    pid_t pid = hl_chan_spawn_env(exe, (char *const *)(uintptr_t)argv, sv[1], plugin_env_ok, extra);
    int err = errno;
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        lua_pushnil(L);
        lua_pushliteral(L, "spawn_failed");
        lua_pushstring(L, strerror(err));
        return 3;
    }
    p->fd = sv[0];
    p->pid = pid;
    return 1;
#else
    (void)exe; (void)entry;
    return fail(L, "unsupported");
#endif
}

/* p:send(type, id, value?, timeout_ms?) */
static int pl_send(lua_State *L)
{
    BcProc *p = proc_check(L);
    lua_Integer type = luaL_checkinteger(L, 2), id = luaL_checkinteger(L, 3);
    lua_Integer ms = luaL_optinteger(L, 5, HL_BP_REPLY_TIMEOUT_MS);
    if (type < 0 || type > UINT32_MAX || id < 0 || id > UINT32_MAX || ms <= 0)
        return fail(L, "invalid_args");
    HlPvWriter w;
    hl_pv_writer_init(&w, HL_BP_MAX_PAYLOAD);
    if (!lua_isnone(L, 4)) {
        int err = hl_lua_pv_encode(L, 4, &w);
        if (err != HL_PV_OK) {
            hl_pv_writer_free(&w);
            return fail(L, hl_pv_error_name(err));
        }
    }
#ifdef HL_CHAN_SUPPORTED
    int rc = hl_chan_write_frame(p->fd, HL_BUILD_PLUGIN_MAGIC, (uint32_t)type, (uint32_t)id,
                                 w.buf, w.len, NULL, 0, hl_chan_mono_ms() + (uint64_t)ms);
#else
    int rc = -1;
    (void)p;
#endif
    hl_pv_writer_free(&w);
    if (rc != 0) return fail(L, "eof");
    lua_pushboolean(L, 1);
    return 1;
}

/* p:recv(timeout_ms) */
static int pl_recv(lua_State *L)
{
    BcProc *p = proc_check(L);
    lua_Integer ms = luaL_checkinteger(L, 2);
    if (ms <= 0) return fail(L, "invalid_args");
#ifdef HL_CHAN_SUPPORTED
    HlChanFrame f = { .magic = HL_BUILD_PLUGIN_MAGIC, .max = HL_BP_MAX_PAYLOAD };
    int rc = hl_chan_frame_read(p->fd, &f, hl_chan_mono_ms() + (uint64_t)ms);
    if (rc != HL_CHAN_DONE) {
        hl_chan_frame_reset(&f);
        return fail(L, rc == HL_CHAN_MORE ? "timeout" : rc == HL_CHAN_EOF ? "eof" : "protocol");
    }
    lua_pushinteger(L, (lua_Integer)f.type);
    lua_pushinteger(L, (lua_Integer)f.id);
    int err = f.len ? hl_lua_pv_decode_protected(L, f.buf, f.len) : (lua_pushnil(L), HL_PV_OK);
    hl_chan_frame_reset(&f);
    if (err == HL_LUA_PV_RAISED) return lua_error(L);   /* the frame is freed first */
    if (err != HL_PV_OK) {
        lua_pop(L, 2);
        return fail(L, "malformed");
    }
    return 3;
#else
    (void)p;
    return fail(L, "unsupported");
#endif
}

/* p:close(grace_ms): the host is done with it. Closing the socket ends its
 * loop; a process still there after @p grace_ms is killed. */
static int pl_close(lua_State *L)
{
    BcProc *p = luaL_checkudata(L, 1, BCPL_MT);
    lua_Integer ms = luaL_optinteger(L, 2, 0);
#ifdef HL_CHAN_SUPPORTED
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
    if (p->pid > 0) {
        int st = 0;
        if (hl_chan_reap(p->pid, &st, ms > 0 ? (uint32_t)ms : 0, NULL)) {
            p->pid = 0;
            if (WIFEXITED(st)) lua_pushinteger(L, WEXITSTATUS(st));
            else lua_pushliteral(L, "signal");
            return 1;
        }
        proc_kill(p);
        lua_pushliteral(L, "killed");
        return 1;
    }
#else
    (void)ms;
    proc_kill(p);
#endif
    lua_pushnil(L);
    return 1;
}

static int pl_kill(lua_State *L)
{
    proc_kill(luaL_checkudata(L, 1, BCPL_MT));
    return 0;
}

static int pl_gc(lua_State *L)
{
    proc_kill(luaL_checkudata(L, 1, BCPL_MT));
    return 0;
}

/* P.now(): the monotonic clock the deadlines are on, in milliseconds. */
static int pl_now(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)hl_chan_mono_ms());
    return 1;
}

static int pl_bytes(lua_State *L)
{
    hl_lua_pv_push_bytes(L, 1);
    return 1;
}

/* P.preset_check(manifest): the manifest (a table, as the extraction child's
 * JSON decodes) against the plugin preset - the runner's own check
 * (hl_build_plugin_preset_check), run before the plugin is registered. */
static int pl_preset_check(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_manifest");   /* saved */
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_manifest");
    HlAllocator a;
    hl_alloc_init(&a, 0);
    HlManifest m;
    int rc = hl_manifest_extract_lua(L, &m, &a);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_manifest");   /* restored */
    char err[256] = "a build plugin must declare a manifest";
    int ok = rc == 0 && hl_build_plugin_preset_check(&m, err, sizeof err) == 0;
    if (rc == 0) hl_manifest_free(&m);
    if (ok) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, err);
    return 2;
}

static const luaL_Reg PROC_METHODS[] = {
    { "send",  pl_send  },
    { "recv",  pl_recv  },
    { "close", pl_close },
    { "kill",  pl_kill  },
    { NULL, NULL },
};

int luaopen_hull_buildctx_plugin(lua_State *L)
{
    if (luaL_newmetatable(L, BCPL_MT)) {
        luaL_newlib(L, PROC_METHODS);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, pl_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    lua_createtable(L, 0, 10);
    lua_pushcfunction(L, pl_spawn);
    lua_setfield(L, -2, "spawn");
    lua_pushcfunction(L, pl_bytes);
    lua_setfield(L, -2, "bytes");
    lua_pushcfunction(L, pl_now);
    lua_setfield(L, -2, "now");
    lua_pushcfunction(L, pl_preset_check);
    lua_setfield(L, -2, "preset_check");
    hl_lua_pv_push_empty_array(L);
    lua_setfield(L, -2, "empty_array");
    static const struct { const char *name; lua_Integer v; } F[] = {
        { "ACTION", HL_BP_ACTION }, { "CALL", HL_BP_CALL }, { "REPLY", HL_BP_REPLY },
        { "DONE", HL_BP_DONE }, { "STOP", HL_BP_STOP },
    };
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
        lua_pushinteger(L, F[i].v);
        lua_setfield(L, -2, F[i].name);
    }
    lua_pushinteger(L, HL_BP_MAX_PAYLOAD);
    lua_setfield(L, -2, "MAX_PAYLOAD");
    return 1;
}
