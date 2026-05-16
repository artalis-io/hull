/**
 * @file buffer.h
 * @brief Unified buffer protocol: zero-copy bytes from any runtime value.
 *
 * Cap functions that accept "bytes as input" should not branch on the
 * runtime-side container type (string vs `ArrayBuffer` vs `WasmBuffer` vs
 * `MappedBuffer`). Instead, they consume an #HlBufferView — a non-owning
 * `(ptr, len)` pair — produced by runtime helpers:
 *   - `lua_get_buffer(L, idx, &view)`
 *   - `js_get_buffer(ctx, val, &view, &str, &needs_free)`
 *
 * This lets the same C function back `compute.call()`, `gpu.buffer()`, and
 * `gpu.dispatch()` for all four input types without copying.
 *
 * @par Lifetime:
 *   The view is valid only while the source value is alive. Cap functions
 *   typically consume the bytes synchronously (copy to GPU/WASM memory)
 *   so the source can be released right after the call.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_BUFFER_H
#define HL_BUFFER_H

#include <stddef.h>

/**
 * @brief Non-owning byte view: `(data, len)`.
 *
 * The caller must NOT free `data`. The view is valid only while its source
 * (Lua string, JS `ArrayBuffer`, #HlMappedBuffer, `WasmBuffer`) remains
 * alive. Typical pattern: extract → consume synchronously → return.
 */
typedef struct HlBufferView {
    const void *data;  /**< Borrowed pointer to bytes (no free). */
    size_t      len;   /**< Length in bytes. */
} HlBufferView;

#endif /* HL_BUFFER_H */
