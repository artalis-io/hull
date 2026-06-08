/**
 * @file js/template_cache.c
 * @brief QuickJS template render-function cache. See
 *        hull/runtime/js_template_cache.h.
 *
 * Mirror of runtime/lua/template_cache.c on the JS side. The
 * generated template code is an IIFE that returns the render
 * function; we cache the post-eval function value so a hit skips
 * BOTH the parse pass and the IIFE execute that creates the
 * render closure.
 *
 * Same QJS_TAG, arch, endian, name, code key composition as the
 * JS bytecode cache (X-9) — but a separate store kind so the
 * two surfaces don't share blob keys (different eval flag set:
 * GLOBAL+STRICT here, MODULE+COMPILE_ONLY there).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js_template_cache.h"
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

#define JTC_STORE_KIND  "js-templates"

/* Same QJS_TAG as the bytecode cache. Bump together with any
 * vendor/quickjs/ change — incompatible bytecode formats would
 * make JS_ReadObject reject stale entries (caught below; we
 * unlink + recompile), but invalidating proactively at the key
 * level is cleaner. */
#define QJS_TAG "qjs-2024-01-13"

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

static int compute_key(const char *name,
                       const char *code, size_t code_len,
                       char hex_out[HL_BLOB_STORE_ID_BUF_SIZE])
{
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);

    const char *tag  = QJS_TAG;
    const char *arch = arch_tag();
    const char *end  = endian_tag();
    const char *nm   = name ? name : "";

    if (hl_cap_crypto_sha256_update(&ctx, tag, strlen(tag))    != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, arch, strlen(arch))  != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, end, strlen(end))    != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, nm, strlen(nm))      != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)              != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, code, code_len)      != 0) return -1;

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

static HlBlobStore *jtc_store = NULL;
static int          jtc_store_failed = 0;

static HlBlobStore *get_store(void)
{
    if (jtc_store) return jtc_store;
    if (jtc_store_failed) return NULL;

    char root[PATH_MAX];
    if (hl_hull_cache_subdir(JTC_STORE_KIND, root, sizeof(root)) != 0) {
        jtc_store_failed = 1;
        return NULL;
    }
    size_t rl = strlen(root);
    if (rl > 1 && root[rl - 1] == '/') root[rl - 1] = '\0';

    HlBlobStore *s = NULL;
    if (hl_blob_store_open(&s, NULL, root, /*shard_depth=*/1, 0) != 0) {
        jtc_store_failed = 1;
        return NULL;
    }
    jtc_store = s;
    return jtc_store;
}

void hl_js_template_cache_reset(void)
{
    if (jtc_store) {
        hl_blob_store_close(jtc_store);
        jtc_store = NULL;
    }
    jtc_store_failed = 0;
}

/* Fresh compile + execute, matching what the original
 * js_template_compile did. Used for cache-disabled / too-small /
 * resolver-failure fast paths. */
static JSValue fresh_eval(JSContext *ctx,
                          const char *code, size_t code_len,
                          const char *name)
{
    return JS_Eval(ctx, code, code_len, name,
                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
}

JSValue hl_js_template_compile_cached(JSContext *ctx,
                                      const char *code, size_t code_len,
                                      const char *name)
{
    if (!code || code_len < 256 ||
        hl_hull_cache_disabled("js_template")) {
        return fresh_eval(ctx, code, code_len, name);
    }

    HlBlobStore *store = get_store();
    if (!store) return fresh_eval(ctx, code, code_len, name);

    char key[HL_BLOB_STORE_ID_BUF_SIZE];
    if (compute_key(name, code, code_len, key) != 0) {
        return fresh_eval(ctx, code, code_len, name);
    }

    /* ── Cache hit: read compiled chunk + run it. ──────────────
     *
     * JS_WriteObject can only serialize COMPILE_ONLY results
     * (the compiled chunk as a function value), NOT closures
     * created at runtime by an IIFE invocation. So the cached
     * blob is the unrun chunk; we JS_EvalFunction it on hit to
     * produce the render closure.
     *
     * The win is still substantial — parse + bytecode emission
     * is by far the expensive part. Running the IIFE itself is
     * a single-frame function call that returns its inner
     * `function` literal. */
    uint8_t *bc     = NULL;
    size_t   bc_len = 0;
    if (hl_blob_store_get(store, key, /*track_access=*/1, &bc, &bc_len) == 0) {
        JSValue chunk = JS_ReadObject(ctx, bc, bc_len,
                                      JS_READ_OBJ_BYTECODE);
        free(bc);
        if (!JS_IsException(chunk)) {
            /* JS_EvalFunction takes ownership of `chunk` (consumes
             * the reference). On success it returns the script's
             * result value — for our IIFE that's the render fn. */
            JSValue rv = JS_EvalFunction(ctx, chunk);
            if (!JS_IsException(rv)) return rv;
            /* Runtime error from a cached chunk shouldn't normally
             * happen — code that compiled and ran successfully
             * before should keep doing so. Drop the exception and
             * fall through to a fresh compile + eval. */
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, rv);
        } else {
            /* Stale / corrupt entry — evict and recompile. */
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        (void)hl_blob_store_delete(store, key);
    }

    /* ── Cache miss: compile-only, persist the chunk, then run. ─
     *
     * Compile twice? No — JS_Eval with COMPILE_ONLY produces the
     * chunk, then JS_EvalFunction runs it. Same total work as
     * the direct JS_Eval the old code path did, plus the
     * JS_WriteObject + store-put. The next boot pays only the
     * JS_ReadObject + JS_EvalFunction cost, which is what we're
     * trying to amortize. */
    JSValue chunk = JS_Eval(ctx, code, code_len, name,
                            JS_EVAL_TYPE_GLOBAL |
                            JS_EVAL_FLAG_STRICT |
                            JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(chunk)) return chunk;

    size_t out_len = 0;
    uint8_t *bytecode = JS_WriteObject(ctx, &out_len, chunk,
                                       JS_WRITE_OBJ_BYTECODE);
    if (bytecode) {
        (void)hl_blob_store_put_keyed(store, key, bytecode, out_len);
        js_free(ctx, bytecode);
    }

    /* JS_EvalFunction consumes `chunk` — no JS_FreeValue afterward. */
    return JS_EvalFunction(ctx, chunk);
}
