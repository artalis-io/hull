/*
 * mod_buffer.c - Unified buffer protocol + shared class ID definitions
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/image.h"

#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm_buffer.h"
#endif

/* ── Shared class ID definitions ─────────────────────────────────── */

JSClassID js_mmap_class_id;
JSClassID js_image_class_id;

#ifdef HL_ENABLE_WASM
JSClassID js_wasm_buf_class_id;
#endif

/* ── Unified buffer protocol (JS) ─────────────────────────────────── */

/*
 * Extract a buffer view from a JS value. Tries (in order):
 * MappedBuffer -> WasmBuffer -> HlImage -> ArrayBuffer -> string.
 * Sets *str_needs_free = 1 if the data came from JS_ToCStringLen
 * (caller must JS_FreeCString). Returns 1 on success, 0 on failure.
 */
int js_get_buffer(JSContext *ctx, JSValueConst val,
                  HlBufferView *out, const char **str_out,
                  int *str_needs_free)
{
    *str_needs_free = 0;
    *str_out = NULL;

    /* Probe each opaque type via JS_GetOpaque (non-throwing). The
     * throwing variant JS_GetOpaque2 sets a TypeError in the ctx on
     * every mismatch - for a multi-type probe like this one we'd
     * leak a pending exception on every successful "second-choice"
     * match, which can later confuse callers that test JSValues
     * against exceptions. JS_GetOpaque just returns NULL on
     * class-id mismatch. (Caught in c-audit on §1.5.b-4 PR 1.) */

    /* MappedBuffer */
    HlMappedBuffer *mb = JS_GetOpaque(val, js_mmap_class_id);
    if (mb && !mb->closed) {
        out->data = mb->addr;
        out->len = mb->len;
        return 1;
    }
#ifdef HL_ENABLE_WASM
    /* WasmBuffer - js_wasm_buf_class_id is defined in this file */
    {
        HlWasmBuffer *wb = JS_GetOpaque(val, js_wasm_buf_class_id);
        if (wb && !wb->closed) {
            out->data = hl_wasm_buffer_data(wb);
            out->len = hl_wasm_buffer_len(wb);
            return 1;
        }
    }
#endif
    /* HlImage (pixel data) */
    if (js_image_class_id) {
        HlImage *img = JS_GetOpaque(val, js_image_class_id);
        if (img) {
            out->data = img->pixels;
            out->len = img->pixel_len;
            return 1;
        }
    }
    /* ArrayBuffer */
    size_t ab_len;
    uint8_t *ab = JS_GetArrayBuffer(ctx, &ab_len, val);
    if (ab) {
        out->data = ab;
        out->len = ab_len;
        return 1;
    }
    /* TypedArray (Uint8Array, Int8Array, Uint16Array, ...). Without
     * this branch a Uint8Array would fall through to the string path
     * and base64url-encode literally "65,66,67" (the toString output)
     * instead of the underlying bytes. Resolve the backing buffer +
     * byte offset and slice the view to the typed array's window. */
    {
        size_t byte_offset = 0, byte_length = 0, bytes_per_element = 0;
        JSValue tab = JS_GetTypedArrayBuffer(ctx, val,
                                              &byte_offset,
                                              &byte_length,
                                              &bytes_per_element);
        if (!JS_IsException(tab)) {
            uint8_t *raw = JS_GetArrayBuffer(ctx, &ab_len, tab);
            JS_FreeValue(ctx, tab);
            if (raw && byte_offset + byte_length <= ab_len) {
                out->data = raw + byte_offset;
                out->len = byte_length;
                return 1;
            }
        } else {
            /* Drain the pending exception so the string fallback
             * doesn't see it. JS_GetTypedArrayBuffer raises on a
             * non-TypedArray which is fine here - we're probing. */
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    /* String (caller must free) */
    size_t slen;
    const char *s = JS_ToCStringLen(ctx, &slen, val);
    if (s) {
        out->data = s;
        out->len = slen;
        *str_out = s;
        *str_needs_free = 1;
        return 1;
    }
    return 0;
}
