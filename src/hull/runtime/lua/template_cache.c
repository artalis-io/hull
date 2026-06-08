/**
 * @file template_cache.c
 * @brief On-disk template render-function cache.
 *
 * See hull/runtime/lua_template_cache.h for the architectural
 * write-up. Implementation mirrors bytecode_cache.c (sha-keyed
 * blob_store entry, lazy singleton, fall-through on any failure)
 * with two semantic differences:
 *
 *   1. The cache key is computed from the GENERATED template code,
 *      not the raw source — naturally invalidates when extends /
 *      include targets change.
 *   2. The cached value is the inner render function (lua_dump
 *      after pcall) — on hit we skip both the parse pass AND the
 *      pcall to unwrap the outer chunk.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua_template_cache.h"
#include "hull/runtime/cache_common.h"
#include "hull/blob_store.h"
#include "hull/cache_dir.h"
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

#define TC_STORE_KIND  "templates"

static int compute_key(const char *code, size_t code_len,
                       char hex_out[HL_BLOB_STORE_ID_BUF_SIZE])
{
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);

    const char *ver  = LUA_VERSION;
    const char *arch = hl_runtime_cache_arch_tag();
    const char *end  = hl_runtime_cache_endian_tag();

    if (hl_cap_crypto_sha256_update(&ctx, ver, strlen(ver))    != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, arch, strlen(arch))  != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, end, strlen(end))    != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, code, code_len)      != 0) return -1;

    uint8_t digest[32];
    if (hl_cap_crypto_sha256_final(&ctx, digest) != 0) return -1;

    hl_runtime_cache_hex_encode(digest, 32, hex_out);
    return 0;
}

/* ── Process-wide template store singleton ─────────────────────── */

static HlRuntimeCacheSlot tc_slot;

static void atexit_close_store(void)
{
    hl_runtime_cache_singleton_reset(&tc_slot);
}

static HlBlobStore *get_store(void)
{
    return hl_runtime_cache_singleton(TC_STORE_KIND, &tc_slot,
                                      atexit_close_store);
}

void hl_lua_template_cache_reset(void)
{
    hl_runtime_cache_singleton_reset(&tc_slot);
}

/* ── lua_dump accumulator ─────────────────────────────────────── */

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
        size_t new_cap = a->cap ? a->cap * 2 : 4096;
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

/* Fresh compile path: loadbuffer(code) → pcall(0,1,0) returns the
 * inner render function. Caller-shared by the cache-miss path and
 * the disabled-cache path. Leaves the render function (or the
 * propagated error) on top of the stack. */
static int fresh_compile(lua_State *L,
                         const char *code, size_t code_len,
                         const char *chunkname)
{
    int rc = luaL_loadbuffer(L, code, code_len, chunkname);
    if (rc != LUA_OK) return rc;
    rc = lua_pcall(L, 0, 1, 0);
    return rc;
}

int hl_lua_template_compile_cached(lua_State *L,
                                   const char *code, size_t code_len,
                                   const char *chunkname)
{
    if (!code || code_len < 256 ||
        hl_hull_cache_disabled("template")) {
        return fresh_compile(L, code, code_len, chunkname);
    }

    HlBlobStore *store = get_store();
    if (!store) return fresh_compile(L, code, code_len, chunkname);

    char key[HL_BLOB_STORE_ID_BUF_SIZE];
    if (compute_key(code, code_len, key) != 0) {
        return fresh_compile(L, code, code_len, chunkname);
    }

    /* ── Cache hit: load the dumped render function directly. ──── */
    uint8_t *bc     = NULL;
    size_t   bc_len = 0;
    if (hl_blob_store_get(store, key, /*track_access=*/1, &bc, &bc_len) == 0) {
        int rc = luaL_loadbuffer(L, (const char *)bc, bc_len, chunkname);
        free(bc);
        if (rc == LUA_OK) return LUA_OK;
        /* Stale / corrupt entry — pop error, evict, fall through. */
        lua_pop(L, 1);
        (void)hl_blob_store_delete(store, key);
    }

    /* ── Cache miss: compile + pcall + persist the render fn. ──── */
    int rc = fresh_compile(L, code, code_len, chunkname);
    if (rc != LUA_OK) return rc;     /* error string on stack */

    /* lua_dump with strip=0 keeps source-name + line numbers so
     * template tracebacks point at the original `=template:<name>`
     * chunkname rather than "[?]". Templates rarely call into the
     * SQL namespace gate so strip=1 would also work, but the size
     * win is small (~30%) and consistency with bytecode_cache is
     * worth more than the savings. */
    DumpAcc acc = { NULL, 0, 0, 0 };
    int dump_rc = lua_dump(L, dump_writer, &acc, 0);
    if (dump_rc != 0 || acc.failed || !acc.buf) {
        free(acc.buf);
        return LUA_OK;  /* keep render fn on stack; skip caching */
    }

    /* Best-effort persist; failures are silent. */
    (void)hl_blob_store_put_keyed(store, key, acc.buf, acc.len);
    free(acc.buf);
    return LUA_OK;
}
