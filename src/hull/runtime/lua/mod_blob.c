/* mod_blob.c - hull.blob bindings (content-addressed blob storage)
 *
 * Public API mirrors docs/blob.md:
 *
 *   blob.init({ dir, shard_depth?, tmp_max_age? })
 *
 *   blob.put(bytes)               -> id, size
 *   blob.put_verified(bytes, sha) -> id, size  (raises on mismatch)
 *   blob.writer({ expected? })    -> Writer
 *     w:write(chunk)              -> w (chainable)
 *     w:finalize()                -> id, size
 *     w:abort()
 *
 *   blob.get(id, { track_access? })  -> bytes
 *   blob.reader(id, { track_access? }) -> Reader
 *     r:read(n)                   -> chunk or nil at EOF
 *     r:close()
 *
 *   blob.exists(id)               -> bool
 *   blob.size(id)                 -> int or nil
 *   blob.atime(id)                -> int or nil
 *   blob.delete(id)               -> bool
 *
 *   for id, size in blob.iter() do ... end
 *   blob.total_size()             -> int
 *   blob.count()                  -> int
 *
 *   blob.cleanup({ max_total_size?, max_age?, strategy?, dry_run? })
 *                                  -> { removed, freed_bytes }
 *
 * Singleton: blob.init() stashes the HlBlob handle in registry under
 * "__hull_blob"; subsequent calls retrieve it. Re-init is allowed
 * (re-points to a new directory).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "refusal.h"
#include "mod_buffer.h"
#include "hull/cap/blob.h"
#include "hull/cap/fs.h"
#include "hull/utils/alloc.h"
#include "hull/limits/runtime.h"       /* HL_BLOB_READ_CHUNK_MAX */
#include "hull/shared/work_charge.h"   /* hl_work_bytes_units */
#include "log.h"
#include "protected.h"   /* pushes that cannot leak the C buffer */

#include <lauxlib.h>
#include <lua.h>
#include <stdlib.h>
#include <string.h>

#define HL_BLOB_MT        "HlBlobStore"
#define HL_BLOB_WRITER_MT "HlBlobWriter"
#define HL_BLOB_READER_MT "HlBlobReader"
#define HL_BLOB_REG_KEY   "__hull_blob"

/* ── Singleton handle ────────────────────────────────────────────── */

/* Stash for the HlBlob* - wrapped in a userdata so __gc frees it
 * when the runtime tears down. */
typedef struct {
    HlBlob *b;
} LuaBlobUd;

/* Fetch the singleton; raise if blob.init() hasn't been called. The
 * store's userdata is LEFT ON THE STACK, anchoring the store for the rest of
 * the call: popped, an __index on an options table could run blob.init
 * (replacing the registry's store) and collectgarbage() (finalizing the old
 * one), and the HlBlob in hand was then used after it was freed. */
static HlBlob *get_store(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, HL_BLOB_REG_KEY);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "blob: call blob.init({dir=...}) first");
        return NULL;  /* unreachable */
    }
    LuaBlobUd *s = (LuaBlobUd *)luaL_checkudata(L, -1, HL_BLOB_MT);
    HlBlob *b = s ? s->b : NULL;
    if (!b) {
        luaL_error(L, "blob: store unavailable (was freed)");
        return NULL;
    }
    return b;
}

static int lua_blob_store_gc(lua_State *L)
{
    LuaBlobUd *s = (LuaBlobUd *)luaL_checkudata(L, 1, HL_BLOB_MT);
    if (s && s->b) {
        hl_cap_blob_free(s->b);
        s->b = NULL;
    }
    return 0;
}

/* ── blob.init ───────────────────────────────────────────────────── */

static int lua_blob_init(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.fs_cfg)
        return luaL_error(L, "blob.init: fs config unavailable");

    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "dir");
    const char *dir = luaL_checkstring(L, -1);

    lua_getfield(L, 1, "shard_depth");
    int shard_depth = lua_isnil(L, -1) ? 1 : (int)luaL_checkinteger(L, -1);

    lua_getfield(L, 1, "tmp_max_age");
    uint64_t tmp_max_age = lua_isnil(L, -1) ? 0
                                            : (uint64_t)luaL_checkinteger(L, -1);

    /* The userdata first, the store after: made the other way round, a
     * memory error creating the userdata leaked the HlBlob. */
    /* Three mkdir_p and the tmp/ sweep every call (audit 37). */
    lua_hlwork(L, HL_WORK_BLOB_INIT_UNITS, 0);
    LuaBlobUd *s = (LuaBlobUd *)lua_newuserdatauv(L, sizeof(*s), 0);
    s->b = NULL;
    luaL_setmetatable(L, HL_BLOB_MT);
    uint64_t swept = 0;
    int rc = hl_cap_blob_init_ex(&s->b, lua->base.fs_cfg, lua->base.alloc,
                                 dir, shard_depth, tmp_max_age, &swept);
    if (rc != 0)
        return luaL_error(L, "blob.init: failed (check fs.write declares '%s')", dir);
    /* Each tmp/ entry the sweep read, charged after it as the blob walk is
     * (audit 21); the userdata owns the store, so a raise here frees it and
     * the store is not installed. */
    lua_hlwork(L, (size_t)swept, 0);

    /* Drop any previous singleton (re-init is allowed). The old
     * userdata's __gc will fire on GC, freeing the old HlBlob. */
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, HL_BLOB_REG_KEY);
    lua_pop(L, 1);                /* pop our local ref */

    lua_pop(L, 3);                /* dir, shard_depth, tmp_max_age */
    return 0;
}

/* ── Validate an id passed from Lua: 64 lowercase hex chars ──────── */

static const char *check_id(lua_State *L, int idx)
{
    size_t n;
    const char *s = luaL_checklstring(L, idx, &n);
    if (n != HL_BLOB_ID_HEX_LEN) {
        luaL_error(L, "blob: invalid id (expected 64 hex chars)");
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            luaL_error(L, "blob: id must be lowercase hex");
            return NULL;
        }
    }
    return s;
}

/* ── blob.put / blob.put_verified ────────────────────────────────── */

/* Charge hashing + writing @p len bytes to the instruction budget BEFORE the
 * work (audit 10): one put SHA-256s and writes up to the whole heap, and
 * counted as one instruction, so a loop of them was not bounded in time.
 * One unit per 8 bytes, crypto_charge's rate; lua_hlwork raises at once
 * when the charge takes the run over, with nothing written. */
static void blob_charge(lua_State *L, size_t len)
{
    lua_hlwork(L, len / 8, 0);
}

/* A whole put: the bytes, plus a floor for the temp file, fsync and rename
 * every put does whatever its size (audit 37: an empty put was free). */
static void blob_put_charge(lua_State *L, size_t len)
{
    lua_hlwork(L, (size_t)(HL_WORK_FS_WRITE_UNITS + hl_work_bytes_units(len)), 0);
}

/* Read `durable` from an optional opts table at `idx` (1 = true, 0 = false). */
static int read_durable_opt(lua_State *L, int idx)
{
    if (!lua_istable(L, idx)) return 0;
    lua_getfield(L, idx, "durable");
    int durable = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return durable;
}

static int lua_blob_put(lua_State *L)
{
    HlBlob *b = get_store(L);
    size_t len = 0;
    const char *bytes = luaL_checklstring(L, 1, &len);
    int durable = read_durable_opt(L, 2);
    blob_put_charge(L, len);

    char id[HL_BLOB_ID_BUF_SIZE];
    int rc = durable
        ? hl_cap_blob_put_durable(b, (const uint8_t *)bytes, len, NULL, id)
        : hl_cap_blob_put        (b, (const uint8_t *)bytes, len, NULL, id);
    if (rc != 0)
        return luaL_error(L, "blob.put: failed");

    lua_pushlstring(L, id, HL_BLOB_ID_HEX_LEN);
    lua_pushinteger(L, (lua_Integer)len);
    return 2;
}

static int lua_blob_put_verified(lua_State *L)
{
    HlBlob *b = get_store(L);
    size_t len = 0;
    const char *bytes = luaL_checklstring(L, 1, &len);
    const char *expected = check_id(L, 2);
    int durable = read_durable_opt(L, 3);
    blob_put_charge(L, len);

    char id[HL_BLOB_ID_BUF_SIZE];
    int rc = durable
        ? hl_cap_blob_put_durable(b, (const uint8_t *)bytes, len, expected, id)
        : hl_cap_blob_put        (b, (const uint8_t *)bytes, len, expected, id);
    if (rc != 0)
        return luaL_error(L, "blob.put_verified: SHA mismatch or I/O failure");

    lua_pushlstring(L, id, HL_BLOB_ID_HEX_LEN);
    lua_pushinteger(L, (lua_Integer)len);
    return 2;
}

/* ── Streaming writer ────────────────────────────────────────────── */

typedef struct {
    HlBlobWriter *w;     /* NULL after finalize/abort */
} HlBlobWriterLua;

static HlBlobWriterLua *check_writer_ud(lua_State *L, int idx)
{
    return (HlBlobWriterLua *)luaL_checkudata(L, idx, HL_BLOB_WRITER_MT);
}

static int lua_blob_writer_new(lua_State *L)
{
    HlBlob *b = get_store(L);
    int store_idx = lua_gettop(L);
    const char *expected = NULL;
    int durable = 0;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "expected");
        if (!lua_isnil(L, -1)) expected = check_id(L, -1);
        /* leave expected on stack - won't be popped because length-check
         * runs after; harmless. */
        lua_getfield(L, 1, "durable");
        durable = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    /* One user value: the store, kept alive while the writer is - the one
     * this call holds (store_idx), not the registry's, which an options
     * __index above may have replaced. The userdata is made before the
     * writer, so a memory error cannot leak an open writer. */
    HlBlobWriterLua *ud = (HlBlobWriterLua *)
        lua_newuserdatauv(L, sizeof(*ud), 1);
    ud->w = NULL;
    luaL_setmetatable(L, HL_BLOB_WRITER_MT);
    lua_pushvalue(L, store_idx);
    lua_setiuservalue(L, -2, 1);

    /* Opening a handle is charged (audit 34): it costs descriptors and
     * syscalls a plain call never sees. */
    lua_hlwork(L, HL_BLOB_OPEN_UNITS, 0);
    int open_rc = durable
        ? hl_cap_blob_writer_open_durable(b, expected, &ud->w)
        : hl_cap_blob_writer_open(b, expected, &ud->w);
    if (open_rc == HL_BLOB_ERR_TOO_MANY) {
        /* Live handles are capped (HL_BLOB_MAX_LIVE_HANDLES, audit 34), and
         * one the app dropped without finalize / abort holds its slot until
         * collected: collect once and retry, as fs.mmap does. The store is
         * anchored at store_idx, so a __gc that re-runs blob.init cannot free
         * it under us; this writer's userdata is on the stack too. */
        lua_gc(L, LUA_GCCOLLECT);
        open_rc = durable
            ? hl_cap_blob_writer_open_durable(b, expected, &ud->w)
            : hl_cap_blob_writer_open(b, expected, &ud->w);
    }
    if (open_rc == HL_BLOB_ERR_TOO_MANY)
        return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "blob.writer: too many open blob writers / "
                          "readers (at most %d)", HL_BLOB_MAX_LIVE_HANDLES);
    if (open_rc != 0)
        return luaL_error(L, "blob.writer: open failed");
    return 1;
}

static int lua_writer_write(lua_State *L)
{
    HlBlobWriterLua *ud = check_writer_ud(L, 1);
    if (!ud->w) return luaL_error(L, "blob.writer: write after finalize/abort");
    size_t len = 0;
    const char *bytes = luaL_checklstring(L, 2, &len);
    blob_charge(L, len);
    if (hl_cap_blob_writer_write(ud->w, (const uint8_t *)bytes, len) != 0) {
        hl_cap_blob_writer_abort(ud->w);
        ud->w = NULL;
        return luaL_error(L, "blob.writer: write failed (aborted)");
    }
    /* Return self for chaining. */
    lua_settop(L, 1);
    return 1;
}

static int lua_writer_finalize(lua_State *L)
{
    HlBlobWriterLua *ud = check_writer_ud(L, 1);
    if (!ud->w) return luaL_error(L, "blob.writer: already finalized/aborted");
    /* The fsync and rename (audit 37); a refused finalize leaves the
     * writer to its __gc, which aborts it. */
    lua_hlwork(L, HL_WORK_FS_WRITE_UNITS, 0);
    char id[HL_BLOB_ID_BUF_SIZE]; size_t size = 0;
    int rc = hl_cap_blob_writer_finalize(ud->w, id, &size);
    ud->w = NULL;          /* finalize always consumes the writer */
    if (rc != 0)
        return luaL_error(L, "blob.writer: finalize failed");
    lua_pushlstring(L, id, HL_BLOB_ID_HEX_LEN);
    lua_pushinteger(L, (lua_Integer)size);
    return 2;
}

static int lua_writer_abort(lua_State *L)
{
    HlBlobWriterLua *ud = check_writer_ud(L, 1);
    if (ud->w) { hl_cap_blob_writer_abort(ud->w); ud->w = NULL; }
    return 0;
}

static int lua_writer_gc(lua_State *L)
{
    HlBlobWriterLua *ud = (HlBlobWriterLua *)
        luaL_testudata(L, 1, HL_BLOB_WRITER_MT);
    /* Silent abort: cleans up the tmp file if caller forgot. */
    if (ud && ud->w) { hl_cap_blob_writer_abort(ud->w); ud->w = NULL; }
    return 0;
}

/* ── Streaming reader ────────────────────────────────────────────── */

typedef struct {
    HlBlobReader *r;     /* NULL after close */
} HlBlobReaderLua;

static HlBlobReaderLua *check_reader_ud(lua_State *L, int idx)
{
    return (HlBlobReaderLua *)luaL_checkudata(L, idx, HL_BLOB_READER_MT);
}

/* Helper: read `track_access` from an optional table at stack idx. */
static int read_track_access(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx)) return 1;
    if (!lua_istable(L, idx))    return 1;
    lua_getfield(L, idx, "track_access");
    int track = lua_isnil(L, -1) ? 1 : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return track;
}

static int lua_blob_reader_new(lua_State *L)
{
    HlBlob *b = get_store(L);
    int store_idx = lua_gettop(L);
    const char *id = check_id(L, 1);
    int track = read_track_access(L, 2);

    /* One user value: the store this call holds (see the writer). The
     * userdata first, so a memory error cannot leak an open reader. */
    HlBlobReaderLua *ud = (HlBlobReaderLua *)
        lua_newuserdatauv(L, sizeof(*ud), 1);
    ud->r = NULL;
    luaL_setmetatable(L, HL_BLOB_READER_MT);
    lua_pushvalue(L, store_idx);
    lua_setiuservalue(L, -2, 1);
    lua_hlwork(L, HL_BLOB_OPEN_UNITS, 0);   /* audit 34: see the writer */
    int rc = hl_cap_blob_reader_open(b, id, track, &ud->r);
    if (rc == HL_BLOB_ERR_TOO_MANY) {
        lua_gc(L, LUA_GCCOLLECT);
        rc = hl_cap_blob_reader_open(b, id, track, &ud->r);
    }
    if (rc == HL_BLOB_ERR_TOO_MANY)
        return hl_lua_refuse(L, HL_LUA_REFUSAL_BUSY "blob.reader: too many open blob writers / "
                          "readers (at most %d)", HL_BLOB_MAX_LIVE_HANDLES);
    if (rc != 0)
        return luaL_error(L, "blob.reader: blob not found");
    return 1;
}

static int lua_reader_read(lua_State *L)
{
    HlBlobReaderLua *ud = check_reader_ud(L, 1);
    if (!ud->r) return luaL_error(L, "blob.reader: read after close");
    lua_Integer cap = luaL_optinteger(L, 2, 65536);
    if (cap <= 0) {
        lua_pushlstring(L, "", 0);
        return 1;
    }
    /* n is the app's. The buffer was allocated at n, uncharged, from
     * lua->base.alloc - the server's allocator, which no VM limit covers
     * (audit 15). n is clamped now (a reader returns up to n bytes a call),
     * the read is charged before it runs, and it reads into a luaL_Buffer
     * box, which is VM memory. */
    if ((lua_Unsigned)cap > HL_BLOB_READ_CHUNK_MAX)
        cap = HL_BLOB_READ_CHUNK_MAX;
    lua_hlwork(L, (size_t)hl_work_bytes_units((uint64_t)cap), 0);
    luaL_Buffer lb;
    char *buf = luaL_buffinitsize(L, &lb, (size_t)cap);
    /* The box allocation can run a GC step, and an app __gc can close
     * this reader: read its state again, through the userdata. */
    ud = check_reader_ud(L, 1);
    if (!ud->r) return luaL_error(L, "blob.reader: read after close");
    size_t got = 0;
    if (hl_cap_blob_reader_read(ud->r, (uint8_t *)buf, (size_t)cap, &got) != 0)
        return luaL_error(L, "blob.reader: read failed");
    if (got == 0) {
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresultsize(&lb, got);
    return 1;
}

static int lua_reader_close(lua_State *L)
{
    HlBlobReaderLua *ud = check_reader_ud(L, 1);
    if (ud->r) { hl_cap_blob_reader_close(ud->r); ud->r = NULL; }
    return 0;
}

static int lua_reader_gc(lua_State *L)
{
    HlBlobReaderLua *ud = (HlBlobReaderLua *)
        luaL_testudata(L, 1, HL_BLOB_READER_MT);
    if (ud && ud->r) { hl_cap_blob_reader_close(ud->r); ud->r = NULL; }
    return 0;
}

/* ── blob.get (buffer-mode read) ─────────────────────────────────── */

static int lua_blob_get(lua_State *L)
{
    HlBlob *b = get_store(L);
    const char *id = check_id(L, 1);
    int track = read_track_access(L, 2);

    HlLua *lua = get_hl_lua(L);
    /* The cap reads the whole blob (up to 256 MB) into an off-heap buffer
     * and SHA-256s it before the copy into the heap: a blob the heap could
     * never hold is refused before that, and the read + hash is charged
     * before it runs (audit 15). An unknown id is still nil. The stat is
     * charged, found or not (audit 37: a get of a missing id was free). */
    lua_hlwork(L, HL_WORK_BLOB_STAT_UNITS, 0);
    size_t want = 0;
    if (hl_cap_blob_stat(b, id, &want, NULL) != 0) {
        lua_pushnil(L);
        return 1;
    }
    if (lua->mem_limit && want > lua->mem_limit)
        return luaL_error(L, "blob.get: blob larger than the heap limit");
    lua_hlwork(L, (size_t)hl_work_bytes_units((uint64_t)want), 0);

    uint8_t *buf = NULL;
    size_t len = 0;
    if (hl_cap_blob_get(b, id, track, &buf, &len) != 0) {
        lua_pushnil(L);
        return 1;
    }
    /* Sized from the opened file: a blob replaced since the stat (a shared
     * store) is held to the same limit. */
    if (lua->mem_limit && len > lua->mem_limit) {
        hl_alloc_free(lua->base.alloc, buf, len);
        return luaL_error(L, "blob.get: blob larger than the heap limit");
    }
    /* Empty-blob contract: (NULL, 0) pushes an empty string - no allocation
     * to free. Protected: a plain push that ran out of memory raised past the
     * free below and leaked the blob. */
    int pushed = hl_lua_pushlstring_safe(L, buf ? (const char *)buf : "", buf ? len : 0);
    if (buf) hl_alloc_free(lua->base.alloc, buf, len);
    if (pushed != 0) return luaL_error(L, "not enough memory for the blob");
    return 1;
}

/* ── Metadata ────────────────────────────────────────────────────── */

static int lua_blob_exists(lua_State *L)
{
    HlBlob *b = get_store(L);
    const char *id = check_id(L, 1);
    lua_hlwork(L, HL_WORK_BLOB_STAT_UNITS, 0);   /* a stat / unlink (audit 37) */
    int rc = hl_cap_blob_exists(b, id);
    if (rc < 0) return luaL_error(L, "blob.exists: stat failed");
    lua_pushboolean(L, rc);
    return 1;
}

static int lua_blob_size(lua_State *L)
{
    HlBlob *b = get_store(L);
    const char *id = check_id(L, 1);
    lua_hlwork(L, HL_WORK_BLOB_STAT_UNITS, 0);   /* a stat / unlink (audit 37) */
    size_t size = 0;
    if (hl_cap_blob_stat(b, id, &size, NULL) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)size);
    return 1;
}

static int lua_blob_atime(lua_State *L)
{
    HlBlob *b = get_store(L);
    const char *id = check_id(L, 1);
    lua_hlwork(L, HL_WORK_BLOB_STAT_UNITS, 0);   /* a stat / unlink (audit 37) */
    int64_t atime = 0;
    if (hl_cap_blob_stat(b, id, NULL, &atime) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)atime);
    return 1;
}

static int lua_blob_delete(lua_State *L)
{
    HlBlob *b = get_store(L);
    const char *id = check_id(L, 1);
    lua_hlwork(L, HL_WORK_BLOB_STAT_UNITS, 0);   /* a stat / unlink (audit 37) */
    int rc = hl_cap_blob_delete(b, id);
    if (rc < 0) return luaL_error(L, "blob.delete: unlink failed");
    lua_pushboolean(L, rc);
    return 1;
}

/* ── Snapshot enumeration ────────────────────────────────────────── */

typedef struct {
    char    id[HL_BLOB_ID_BUF_SIZE];
    size_t  size;
} IterItem;

/* Iter accumulator and the persisted iterator state both use
 * `lua->base.alloc`. That is the server's allocator, NOT the Lua heap
 * tracker (hl_lua_alloc): the VM limit does not cover the snapshot array,
 * which is bounded by the store's entry count (~72 bytes a blob). The
 * matching free in lua_iter_state_gc needs the same allocator and the
 * same capacity it was grown to - stash both on the state. */

typedef struct {
    HlAllocator *alloc;
    IterItem    *items;
    size_t       count;
    size_t       capacity;
} IterAcc;

static int iter_collect_cb(const char *id, size_t size, void *user)
{
    IterAcc *a = user;
    if (a->count == a->capacity) {
        size_t old_cap = a->capacity;
        size_t new_cap = old_cap == 0 ? 64 : old_cap * 2;
        if (new_cap > SIZE_MAX / sizeof(IterItem)) return -1;  /* L1 */
        IterItem *grown = hl_alloc_realloc(a->alloc, a->items,
                                            old_cap * sizeof(IterItem),
                                            new_cap * sizeof(IterItem));
        if (!grown) return -1;
        a->items = grown;
        a->capacity = new_cap;
    }
    memcpy(a->items[a->count].id, id, HL_BLOB_ID_BUF_SIZE);
    a->items[a->count].size = size;
    a->count++;
    return 0;
}

/* Iterator state stored as a single userdata that holds the items
 * array. __gc frees it. Closure upvalue holds (state, position).
 * Carries `alloc` and `capacity` so the free matches the original
 * tracked allocation exactly. */
typedef struct {
    HlAllocator *alloc;
    IterItem    *items;
    size_t       count;
    size_t       capacity;
    size_t       pos;
} IterState;

static int lua_iter_state_gc(lua_State *L)
{
    IterState *st = (IterState *)lua_touserdata(L, 1);
    if (st && st->items) {
        hl_alloc_free(st->alloc, st->items,
                      st->capacity * sizeof(IterItem));
        st->items = NULL;
    }
    return 0;
}

static int lua_iter_step(lua_State *L)
{
    IterState *st = (IterState *)lua_touserdata(L, lua_upvalueindex(1));
    if (st->pos >= st->count) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, st->items[st->pos].id, HL_BLOB_ID_HEX_LEN);
    lua_pushinteger(L, (lua_Integer)st->items[st->pos].size);
    st->pos++;
    return 2;
}

static int lua_blob_iter(lua_State *L)
{
    HlBlob *b = get_store(L);
    HlLua *lua = get_hl_lua(L);

    /* The state's userdata first, owning nothing yet: made after the walk,
     * a memory error creating it leaked the collected array. */
    IterState *st = (IterState *)lua_newuserdatauv(L, sizeof(*st), 0);
    memset(st, 0, sizeof *st);
    st->alloc = lua->base.alloc;
    if (luaL_newmetatable(L, "HlBlobIterState")) {
        lua_pushcfunction(L, lua_iter_state_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);

    /* The walk is charged: 1 unit per shard directory before it, 1 per
     * entry after (audit 21). lua_hlwork raises once the run is over. */
    lua_hlwork(L, (size_t)hl_cap_blob_walk_units_before(b), 0);
    IterAcc acc = { lua->base.alloc, NULL, 0, 0 };
    if (hl_cap_blob_iter(b, iter_collect_cb, &acc) != 0) {
        if (acc.items)
            hl_alloc_free(acc.alloc, acc.items,
                          acc.capacity * sizeof(IterItem));
        return luaL_error(L, "blob.iter: walk failed");
    }

    /* Hand the collected array to the state, whose __gc cleans it. */
    st->items    = acc.items;
    st->count    = acc.count;
    st->capacity = acc.capacity;
    st->pos      = 0;
    lua_hlwork(L, (size_t)hl_cap_blob_walk_units_after(b), 0);

    /* Push the iterator function with the state as upvalue. */
    lua_pushcclosure(L, lua_iter_step, 1);
    return 1;
}

static int lua_blob_total_size(lua_State *L)
{
    HlBlob *b = get_store(L);
    lua_hlwork(L, (size_t)hl_cap_blob_walk_units_before(b), 0);
    uint64_t total = hl_cap_blob_total_size(b);
    lua_hlwork(L, (size_t)hl_cap_blob_walk_units_after(b), 0);
    lua_pushinteger(L, (lua_Integer)total);
    return 1;
}

static int lua_blob_count(lua_State *L)
{
    HlBlob *b = get_store(L);
    lua_hlwork(L, (size_t)hl_cap_blob_walk_units_before(b), 0);
    uint64_t n = hl_cap_blob_count(b);
    lua_hlwork(L, (size_t)hl_cap_blob_walk_units_after(b), 0);
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

static int lua_blob_cleanup(lua_State *L)
{
    HlBlob *b = get_store(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    HlBlobCleanupOpts opts = {0};

    lua_getfield(L, 1, "max_total_size");
    opts.max_total_size = lua_isnil(L, -1) ? 0
                                           : (uint64_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "max_age");
    opts.max_age_sec = lua_isnil(L, -1) ? 0
                                        : (uint64_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "strategy");
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        opts.strategy = (strcmp(s, "fifo") == 0) ? HL_BLOB_FIFO : HL_BLOB_LRU;
    } else {
        opts.strategy = HL_BLOB_LRU;
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "dry_run");
    opts.dry_run = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_hlwork(L, (size_t)hl_cap_blob_walk_units_before(b), 0);
    uint64_t removed = 0, freed = 0;
    if (hl_cap_blob_cleanup(b, &opts, &removed, &freed) != 0)
        return luaL_error(L, "blob.cleanup: failed");
    lua_hlwork(L, (size_t)(hl_cap_blob_walk_units_after(b) + removed), 0);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)removed);
    lua_setfield(L, -2, "removed");
    lua_pushinteger(L, (lua_Integer)freed);
    lua_setfield(L, -2, "freed_bytes");
    return 1;
}

/* ── Module registration ─────────────────────────────────────────── */

static const luaL_Reg writer_methods[] = {
    {"write",    lua_writer_write},
    {"finalize", lua_writer_finalize},
    {"abort",    lua_writer_abort},
    {NULL, NULL}
};

static const luaL_Reg reader_methods[] = {
    {"read",  lua_reader_read},
    {"close", lua_reader_close},
    {NULL, NULL}
};

/* Methods live in a table of their own and the metatables are locked: the
 * metatable used to be its own __index, so app code could reach it through
 * getmetatable(w) and replace `finalize` / `write` - which the stdlib calls
 * (attachment.lua records the id finalize returns in _hull_* metadata). */
static void register_writer_mt(lua_State *L)
{
    luaL_newmetatable(L, HL_BLOB_WRITER_MT);
    luaL_newlib(L, writer_methods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lua_writer_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

static void register_reader_mt(lua_State *L)
{
    luaL_newmetatable(L, HL_BLOB_READER_MT);
    luaL_newlib(L, reader_methods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lua_reader_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

static void register_store_mt(lua_State *L)
{
    luaL_newmetatable(L, HL_BLOB_MT);
    lua_pushcfunction(L, lua_blob_store_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

static const luaL_Reg blob_funcs[] = {
    {"init",         lua_blob_init},
    {"put",          lua_blob_put},
    {"put_verified", lua_blob_put_verified},
    {"writer",       lua_blob_writer_new},
    {"get",          lua_blob_get},
    {"reader",       lua_blob_reader_new},
    {"exists",       lua_blob_exists},
    {"size",         lua_blob_size},
    {"atime",        lua_blob_atime},
    {"delete",       lua_blob_delete},
    {"iter",         lua_blob_iter},
    {"total_size",   lua_blob_total_size},
    {"count",        lua_blob_count},
    {"cleanup",      lua_blob_cleanup},
    {NULL, NULL}
};

int luaopen_hull_blob(lua_State *L)
{
    register_store_mt(L);
    register_writer_mt(L);
    register_reader_mt(L);
    luaL_newlib(L, blob_funcs);
    return 1;
}
