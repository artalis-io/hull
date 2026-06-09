/*
 * mod_mime.c — hull:mime module (content-MIME sniffing)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/mime.h"

/* mime.sniff(bytes) -> string | null
 *
 * Thin binding around hl_cap_mime_sniff. `bytes` may be an
 * ArrayBuffer, a Uint8Array (or any TypedArray with a backing
 * buffer), or a string. Returns the sniffed MIME type, or null
 * if no signature matches.
 *
 * Pure capability — no fs/network/db access. */
static JSValue js_mime_sniff(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "mime.sniff requires (bytes)");

    HlBufferView view = {0};
    const char *str = NULL;
    int needs_free = 0;
    if (!js_get_buffer(ctx, argv[0], &view, &str, &needs_free))
        return JS_ThrowTypeError(ctx,
            "mime.sniff: bytes must be an ArrayBuffer, TypedArray, or string");

    const char *mime = hl_cap_mime_sniff((const uint8_t *)view.data, view.len);

    if (needs_free && str) JS_FreeCString(ctx, str);

    if (mime) return JS_NewString(ctx, mime);
    return JS_NULL;
}

static int js_mime_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/mime", "hull:mime") != 0) return -1;

    JSValue mime = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mime, "sniff",
                      JS_NewCFunction(ctx, js_mime_sniff, "sniff", 1));
    JS_SetModuleExport(ctx, m, "mime", mime);
    return 0;
}

int hl_js_init_mime_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:mime", js_mime_module_init);
    if (!m)
        return -1;
    JS_AddModuleExport(ctx, m, "mime");
    return 0;
}
