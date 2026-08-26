/**
 * @file bytecode_cache.c
 * @brief On-disk Lua bytecode cache. See hull/runtime/lua_bytecode_cache.h.
 *
 * Backed by hull/blob_store in keyed mode (see
 * hl_blob_store_put_keyed). Store root lives at
 * $HOME/.hull/blobs/runtime/lua-bytecode/. The store handle is
 * process-wide and lazily opened on first cache hit / miss.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua_bytecode_cache.h"
#include "hull/runtime/cache_common.h"
#include "hull/shared/blob_store.h"
#include "hull/shared/cache_dir.h"
#include "hull/cap/crypto.h"

#include "lauxlib.h"
#include "lua.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BC_STORE_KIND  "lua-bytecode"

/* ── Arch / endian tags folded into the cache key ─────────────────
 *
 * The Lua precompiled chunk header already encodes these and
 * luaL_loadbuffer validates them - this is a belt-and-suspenders
 * early reject so we don't even open the file under a mismatched
 * $HOME (NFS / dotfile syncing across architectures). */

/* Cache-key digest = sha256(LUA_VERSION || "|" || arch || "|" ||
 * endian || "|" || source). LUA_VERSION moves on every Lua upgrade
 * (header constant), so stale entries from an old vendor bump never
 * collide. arch/endian tags come from the shared helper. */
static int compute_key(const char *src, size_t src_len,
                       char hex_out[HL_BLOB_STORE_ID_BUF_SIZE])
{
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);

    const char *ver  = LUA_VERSION;
    const char *arch = hl_runtime_cache_arch_tag();
    const char *end  = hl_runtime_cache_endian_tag();

    if (hl_cap_crypto_sha256_update(&ctx, ver, strlen(ver))   != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)             != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, arch, strlen(arch)) != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)             != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, end, strlen(end))   != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)             != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, src, src_len)       != 0) return -1;

    uint8_t digest[32];
    if (hl_cap_crypto_sha256_final(&ctx, digest) != 0) return -1;

    hl_runtime_cache_hex_encode(digest, 32, hex_out);
    return 0;
}

/* ── Process-wide store singleton ─────────────────────────────────
 *
 * The bytecode store is opened lazily on first use, kept alive for
 * the lifetime of the process. NULL allocator → blob_store falls
 * back to libc malloc/free (we don't want to charge cache I/O to
 * any HlRuntime's memory limit). */

/* Per-cache slot. Zero-initialised by C static storage rules; the
 * helper handles thread-safe open + atexit registration internally. */
static HlRuntimeCacheSlot bc_slot;

static void atexit_close_store(void)
{
    /* Free the open handle + close the underlying file descriptors
     * on process exit. The kernel reclaims fds either way; the
     * value is cleanly-zero leak reports under ASan/valgrind. */
    hl_runtime_cache_singleton_reset(&bc_slot);
}

static HlBlobStore *get_store(void)
{
    return hl_runtime_cache_singleton(BC_STORE_KIND, &bc_slot,
                                      atexit_close_store);
}

void hl_lua_bytecode_cache_reset(void)
{
    hl_runtime_cache_singleton_reset(&bc_slot);
}

/* ── lua_dump accumulator ─────────────────────────────────────────
 *
 * lua_dump writer that appends to a malloc'd buffer. Returns 0
 * (success) to lua_dump per its contract; allocation failure flips
 * an error flag we check after. */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      failed;
} DumpAcc;

static int dump_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    (void)L;
    DumpAcc *a = (DumpAcc *)ud;
    if (a->failed) return 1;
    if (a->len + sz > a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 1024;
        while (new_cap < a->len + sz) new_cap *= 2;
        uint8_t *nb = (uint8_t *)realloc(a->buf, new_cap);
        if (!nb) { a->failed = 1; return 1; }
        a->buf = nb;
        a->cap = new_cap;
    }
    memcpy(a->buf + a->len, p, sz);
    a->len += sz;
    return 0;
}

int hl_lua_load_cached(lua_State *L,
                       const char *src, size_t src_len,
                       const char *chunkname)
{
    /* Fast bail: cache disabled, source too tiny to bother.
     * Kind string is "lua_bytecode" so the env-var builder
     * produces HULL_NO_LUA_BYTECODE_CACHE - symmetric with
     * HULL_NO_JS_BYTECODE_CACHE on the JS side. */
    if (!src || src_len < 256 ||
        hl_hull_cache_disabled("lua_bytecode")) {
        return luaL_loadbuffer(L, src, src_len, chunkname);
    }

    HlBlobStore *store = get_store();
    if (!store) return luaL_loadbuffer(L, src, src_len, chunkname);

    char key[HL_BLOB_STORE_ID_BUF_SIZE];
    if (compute_key(src, src_len, key) != 0) {
        return luaL_loadbuffer(L, src, src_len, chunkname);
    }

    /* ── Cache hit. ─────────────────────────────────────────────── */
    uint8_t *bc      = NULL;
    size_t   bc_len  = 0;
    if (hl_blob_store_get(store, key, /*track_access=*/1, &bc, &bc_len) == 0) {
        int rc = luaL_loadbuffer(L, (const char *)bc, bc_len, chunkname);
        free(bc);  /* allocator was NULL → libc malloc */
        if (rc == LUA_OK) return LUA_OK;
        /* Stale / corrupt entry - pop error, evict, fall through. */
        lua_pop(L, 1);
        (void)hl_blob_store_delete(store, key);
    }

    /* ── Cache miss: compile + persist. ─────────────────────────── */
    int rc = luaL_loadbuffer(L, src, src_len, chunkname);
    if (rc != LUA_OK) return rc;     /* parse error on stack */

    /* lua_dump with strip=0 - keep source-name + line numbers so
     * mod_db.c::lua_is_stdlib_caller can see `ar.source` and let
     * stdlib code touch _hull_* tables. Stripping breaks the gate. */
    DumpAcc acc = { NULL, 0, 0, 0 };
    int dump_rc = lua_dump(L, dump_writer, &acc, 0);
    if (dump_rc != 0 || acc.failed || !acc.buf) {
        free(acc.buf);
        return LUA_OK;               /* keep compiled fn; skip caching */
    }

    /* Best-effort persist. Failure (disk full, race) is silent -
     * the compiled function is already on the stack. */
    (void)hl_blob_store_put_keyed(store, key, acc.buf, acc.len);
    free(acc.buf);
    return LUA_OK;
}
