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

#define JBC_STORE_KIND  "js-bytecode"

/* QuickJS doesn't expose a runtime version constant. The bytecode
 * format ("BC_VERSION" inside quickjs.c) changes whenever we
 * vendor a new snapshot. This string tracks the vendored release
 * we built against and MUST be bumped together with `make
 * fetch-quickjs` or any other QuickJS source change — otherwise
 * stale bytecode might be loaded into an incompatible runtime.
 *
 * The Makefile defines CONFIG_VERSION="2024-01-13" for the
 * vendored snapshot; we mirror that here so the two surfaces are
 * easy to keep in sync. */
#define QJS_TAG "qjs-2024-01-13"

/* ── Arch / endian tags (shared shape with the Lua cache) ──── */

static const char *arch_tag(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#elif defined(__arm__)
    return "arm";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#else
    return "unknown";
#endif
}

static const char *endian_tag(void)
{
    uint16_t probe = 0x0102;
    return (*(const uint8_t *)&probe == 0x01) ? "be" : "le";
}

/* Cache-key digest. `module_name` is in the key because QuickJS
 * embeds it in the bytecode (debug/traceback) — two different
 * names with identical source produce different bytecode. */
static int compute_key(const char *module_name,
                       const char *src, size_t src_len,
                       char hex_out[HL_BLOB_STORE_ID_BUF_SIZE])
{
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);

    const char *tag  = QJS_TAG;
    const char *arch = arch_tag();
    const char *end  = endian_tag();
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

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i * 2]     = hex[digest[i] >> 4];
        hex_out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    hex_out[64] = '\0';
    return 0;
}

/* ── Process-wide store singleton ─────────────────────────────── */

static HlBlobStore *jbc_store = NULL;
static int          jbc_store_failed = 0;

static HlBlobStore *get_store(void)
{
    if (jbc_store) return jbc_store;
    if (jbc_store_failed) return NULL;

    char root[PATH_MAX];
    if (hl_hull_cache_subdir(JBC_STORE_KIND, root, sizeof(root)) != 0) {
        jbc_store_failed = 1;
        return NULL;
    }
    size_t rl = strlen(root);
    if (rl > 1 && root[rl - 1] == '/') root[rl - 1] = '\0';

    HlBlobStore *s = NULL;
    if (hl_blob_store_open(&s, NULL, root, /*shard_depth=*/1, 0) != 0) {
        jbc_store_failed = 1;
        return NULL;
    }
    jbc_store = s;
    return jbc_store;
}

void hl_js_bytecode_cache_reset(void)
{
    if (jbc_store) {
        hl_blob_store_close(jbc_store);
        jbc_store = NULL;
    }
    jbc_store_failed = 0;
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
        JSValue rv = JS_ReadObject(ctx, bc, bc_len,
                                   JS_READ_OBJ_BYTECODE);
        free(bc);
        if (!JS_IsException(rv)) return rv;
        /* Stale / corrupt bytecode — drop the exception, evict,
         * fall through to source compile + repersist. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        (void)hl_blob_store_delete(store, key);
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
