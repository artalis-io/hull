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

#include "hull/runtime/quickjs_tag.h"  /* QJS_TAG (shared with bytecode cache) */

#define JTC_STORE_KIND  "js-templates"

static int compute_key(const char *name,
                       const char *code, size_t code_len,
                       char hex_out[HL_BLOB_STORE_ID_BUF_SIZE])
{
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);

    const char *tag  = QJS_TAG;
    const char *arch = hl_runtime_cache_arch_tag();
    const char *end  = hl_runtime_cache_endian_tag();
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

    hl_runtime_cache_hex_encode(digest, 32, hex_out);
    return 0;
}

/* ── Process-wide store singleton (via shared helper) ─────────── */

static HlBlobStore *jtc_store = NULL;
static int          jtc_store_failed = 0;

static HlBlobStore *get_store(void)
{
    return hl_runtime_cache_singleton(JTC_STORE_KIND,
                                      &jtc_store, &jtc_store_failed);
}

void hl_js_template_cache_reset(void)
{
    hl_runtime_cache_singleton_reset(&jtc_store, &jtc_store_failed);
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
