/*
 * buffer.h — Unified buffer protocol for Hull
 *
 * Provides a common interface for extracting (ptr, len) from any
 * buffer-like value: Lua strings, JS ArrayBuffers, WasmBuffers,
 * MappedBuffers. Functions that accept "bytes" should use
 * hl_lua_get_buffer() / hl_js_get_buffer() instead of checking
 * each type individually.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_BUFFER_H
#define HL_BUFFER_H

#include <stddef.h>

/*
 * Buffer view — non-owning pointer + length.
 * The caller must NOT free the data pointer.
 * Valid only while the source (string, MappedBuffer, WasmBuffer) is alive.
 */
typedef struct HlBufferView {
    const void *data;
    size_t      len;
} HlBufferView;

#endif /* HL_BUFFER_H */
