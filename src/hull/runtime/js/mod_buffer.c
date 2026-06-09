/*
 * mod_buffer.c — Unified buffer protocol + shared class ID definitions
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
     * every mismatch — for a multi-type probe like this one we'd
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
    /* WasmBuffer — js_wasm_buf_class_id is defined in this file */
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
