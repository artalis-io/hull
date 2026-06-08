/**
 * @file js/bytecode_cache.c
 * @brief QuickJS bytecode cache. See hull/runtime/js_bytecode_cache.h.
 *
 * Mirror of runtime/lua/bytecode_cache.c, adapted to QuickJS's
 * JS_WriteObject / JS_ReadObject pair instead of lua_dump /
 * luaL_loadbuffer.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js_bytecode_cache.h"
#include "hull/runtime/cache_common.h"
#include "hull/blob_store.h"
#include "hull/cache_dir.h"
#include "hull/cap/crypto.h"

#include "quickjs.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hull/runtime/quickjs_tag.h"  /* QJS_TAG */

#define JBC_STORE_KIND  "js-bytecode"

/* Cache-key digest. `module_name` is in the key because QuickJS
 * embeds it in the bytecode (debug/traceback) — two different
 * names with identical source produce different bytecode.
 * arch/endian tags via the shared helper. */
static int compute_key(const char *module_name,
                       const char *src, size_t src_len,
                       char hex_out[HL_BLOB_STORE_ID_BUF_SIZE])
{
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);

    const char *tag  = QJS_TAG;
    const char *arch = hl_runtime_cache_arch_tag();
    const char *end  = hl_runtime_cache_endian_tag();
    const char *name = module_name ? module_name : "";

    if (hl_cap_crypto_sha256_update(&ctx, tag, strlen(tag))    != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, arch, strlen(arch))  != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, end, strlen(end))    != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, name, strlen(name))  != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, src, src_len)        != 0) return -1;

    uint8_t digest[32];
    if (hl_cap_crypto_sha256_final(&ctx, digest) != 0) return -1;

    hl_runtime_cache_hex_encode(digest, 32, hex_out);
    return 0;
}

/* ── Process-wide store singleton (via shared helper) ─────────── */

static HlRuntimeCacheSlot jbc_slot;

static void atexit_close_store(void)
{
    /* Free the open handle + close the underlying file descriptors
     * on process exit. The kernel reclaims fds either way; the
     * value is cleanly-zero leak reports under ASan/valgrind. */
    hl_runtime_cache_singleton_reset(&jbc_slot);
}

static HlBlobStore *get_store(void)
{
    return hl_runtime_cache_singleton(JBC_STORE_KIND, &jbc_slot,
                                      atexit_close_store);
}

void hl_js_bytecode_cache_reset(void)
{
    hl_runtime_cache_singleton_reset(&jbc_slot);
}

/* Fresh compile path. Returns whatever JS_Eval returns. Leaves
 * QuickJS in the same state JS_Eval would (either a valid module
 * function or an exception value). */
static JSValue fresh_compile(JSContext *ctx,
                             const char *src, size_t src_len,
                             const char *module_name)
{
    return JS_Eval(ctx, src, src_len, module_name,
                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
}

JSValue hl_js_compile_module_cached(JSContext *ctx,
                                    const char *src, size_t src_len,
                                    const char *module_name)
{
    /* Fast bail: cache disabled OR source too small to bother with
     * the round-trip. Same 256-byte floor as the Lua cache; for
     * smaller modules the cache machinery costs more than the
     * parse it would save. */
    if (!src || src_len < 256 ||
        hl_hull_cache_disabled("js_bytecode")) {
        return fresh_compile(ctx, src, src_len, module_name);
    }

    HlBlobStore *store = get_store();
    if (!store) return fresh_compile(ctx, src, src_len, module_name);

    char key[HL_BLOB_STORE_ID_BUF_SIZE];
    if (compute_key(module_name, src, src_len, key) != 0) {
        return fresh_compile(ctx, src, src_len, module_name);
    }

    /* ── Cache hit: deserialize via JS_ReadObject. ─────────────── */
    uint8_t *bc     = NULL;
    size_t   bc_len = 0;
    if (hl_blob_store_get(store, key, /*track_access=*/1, &bc, &bc_len) == 0) {
        /* Defensive: an empty or NULL blob slips past JS_ReadObject's
         * version-check (it dereferences buf to read BC_VERSION).
         * blob_store doesn't currently produce zero-byte entries on
         * the keyed-put path, but a truncated file from a crashed
         * writer or a hostile planted file in HULL_CACHE_DIR could.
         * Evict + fall through to source compile rather than crash. */
        if (!bc || bc_len == 0) {
            free(bc);
            (void)hl_blob_store_delete(store, key);
        } else {
            JSValue rv = JS_ReadObject(ctx, bc, bc_len,
                                       JS_READ_OBJ_BYTECODE);
            free(bc);
            if (!JS_IsException(rv)) return rv;
            /* Stale / corrupt bytecode — drop the exception, evict,
             * fall through to source compile + repersist. */
            JS_FreeValue(ctx, JS_GetException(ctx));
            (void)hl_blob_store_delete(store, key);
        }
    }

    /* ── Cache miss: compile, then persist. ────────────────────── */
    JSValue func = fresh_compile(ctx, src, src_len, module_name);
    if (JS_IsException(func)) return func;     /* propagate as-is */

    /* JS_WriteObject serializes the compiled module function so
     * JS_ReadObject can later reconstruct it without a parser
     * pass. JS_WRITE_OBJ_BYTECODE is required to permit
     * function/module values (otherwise QuickJS rejects them as
     * "cannot serialize"). */
    size_t out_len = 0;
    uint8_t *bytecode = JS_WriteObject(ctx, &out_len, func,
                                       JS_WRITE_OBJ_BYTECODE);
    if (bytecode) {
        /* Best-effort persist. Failures (disk full, race) are
         * silent — the compiled module function is already in
         * hand and the runtime keeps moving. */
        (void)hl_blob_store_put_keyed(store, key, bytecode, out_len);
        js_free(ctx, bytecode);
    }
    return func;
}
