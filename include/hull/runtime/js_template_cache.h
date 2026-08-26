/**
 * @file js_template_cache.h
 * @brief On-disk cache for JS template render functions.
 *
 * JS-side parity with the Lua template cache (§1.5.b-X-4).
 * Memoizes the parse + IIFE-execute pass that turns generated JS
 * template code into a render function. Backed by hull/blob_store
 * in keyed mode at `$HOME/.hull/blobs/runtime/js-templates/`.
 *
 * Cache key = SHA-256(QJS_TAG || arch || endian || name ||
 *                     generated_code). Hashing the GENERATED code
 * (post-resolve of extends + includes by stdlib/js/hull/template.js)
 * gives automatic invalidation when any referenced template
 * changes - no dependency tracking needed.
 *
 * Cached value = `JS_WriteObject(render_fn, JS_WRITE_OBJ_BYTECODE)`
 * for the post-eval render function. On hit we skip both the
 * parse pass AND the IIFE execute that creates the render
 * closure.
 *
 * Honors `HULL_NO_CACHE=1` and `HULL_NO_JS_TEMPLATE_CACHE=1`.
 * Falls back transparently to a fresh `JS_Eval` on any cache I/O
 * failure (corrupt file → unlink + recompile + repersist).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_JS_TEMPLATE_CACHE_H
#define HL_JS_TEMPLATE_CACHE_H

#include "quickjs.h"
#include <stddef.h>

/**
 * @brief Compile a JS template into a render function, with
 *        on-disk memoization.
 *
 * Drop-in replacement for the existing JS_Eval call in
 * `runtime/js/mod_template.c::js_template_compile`:
 *
 *   JS_Eval(ctx, code, len, name,
 *           JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
 *
 * Returns the render function value (or an exception). Caller
 * treats the result identically to a direct JS_Eval.
 *
 * @param ctx          QuickJS context.
 * @param code         Generated template JS source (the IIFE-wrapped
 *                     `compileSource` output).
 * @param code_len     Length of `code`.
 * @param name         Chunk name for tracebacks (e.g.
 *                     `"=template:pages/home.html"`).
 * @return Render function value on success; exception on parse /
 *         runtime failure.
 */
JSValue hl_js_template_compile_cached(JSContext *ctx,
                                      const char *code, size_t code_len,
                                      const char *name);

/**
 * @brief Tear down the process-wide JS template store handle.
 *
 * Tests that redirect `$HOME` need to drop the cached store. Not
 * for production use.
 */
void hl_js_template_cache_reset(void);

#endif /* HL_JS_TEMPLATE_CACHE_H */
