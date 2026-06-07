/**
 * @file js_bytecode_cache.h
 * @brief On-disk QuickJS bytecode cache (JS-side parity with the
 *        Lua bytecode cache).
 *
 * Memoizes the parse pass that turns ES module source into a QuickJS
 * bytecode module. Backed by hull/blob_store in keyed mode at
 * `$HOME/.hull/blobs/runtime/js-bytecode/`. Shared infrastructure
 * with the Lua bytecode cache (§1.5.b-X-1), AOT cache (§1.5.b-X-2),
 * and template cache (§1.5.b-X-4).
 *
 * Cache key = SHA-256(QJS_TAG || arch || endian || module_name ||
 *                     source). `module_name` belongs in the key
 * because QuickJS bakes it into the bytecode (debug / traceback);
 * two different names with identical source produce different
 * bytecode.
 *
 * Cached value = `JS_WriteObject(value, JS_WRITE_OBJ_BYTECODE)`
 * output for the compiled module function. On hit we skip the
 * parse pass entirely and `JS_ReadObject` returns the function
 * directly.
 *
 * Honors `HULL_NO_CACHE=1` and `HULL_NO_JS_BYTECODE_CACHE=1`.
 * Falls back transparently to a fresh `JS_Eval` on any cache I/O
 * failure (corrupt file → unlink + recompile + repersist).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_JS_BYTECODE_CACHE_H
#define HL_JS_BYTECODE_CACHE_H

#include "quickjs.h"
#include <stddef.h>

/**
 * @brief Compile a JS module from source, with on-disk memoization.
 *
 * Drop-in replacement for:
 *   JS_Eval(ctx, src, src_len, module_name,
 *           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
 *
 * Caller checks `JS_IsException(rv)` and, on success, extracts the
 * `JSModuleDef *` via `JS_VALUE_GET_PTR(rv)` per QuickJS convention,
 * then `JS_FreeValue(ctx, rv)`.
 *
 * @param ctx          QuickJS context.
 * @param src          UTF-8 source bytes (no NUL required).
 * @param src_len      Length of `src`.
 * @param module_name  Module name (e.g. `"hull:cookie"`, an absolute
 *                     filesystem path, or the lookup name used by
 *                     the module loader).
 * @return Module function value on success; QuickJS exception value
 *         on parse failure. Either way the caller treats the result
 *         identically to a direct `JS_Eval` call.
 */
JSValue hl_js_compile_module_cached(JSContext *ctx,
                                    const char *src, size_t src_len,
                                    const char *module_name);

/**
 * @brief Tear down the process-wide JS bytecode store handle.
 *
 * Tests that redirect `$HOME` need to drop the cached store. Not
 * for production use.
 */
void hl_js_bytecode_cache_reset(void);

#endif /* HL_JS_BYTECODE_CACHE_H */
