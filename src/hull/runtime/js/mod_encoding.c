/*
 * mod_encoding.c: JS binding for the C codecs (utils/hex, utils/base64).
 *
 * Registers the internal module "hull:encoding:_native". The stdlib
 * hull:encoding is the sole caller: it takes this fast path and keeps its
 * pure-JS codecs for everything the fast path declines - reason-free refusals,
 * lenient decoding, and input shapes this file does not read.
 *
 *   hexEncode(bytes)               -> text | undefined
 *   hexDecode(text)                -> byte string | null
 *   base64Encode(bytes, url, pad)  -> text | undefined
 *   base64Decode(text, url)        -> byte string | null
 *
 * Bytes are what hull:encoding takes: a BYTE STRING (one character per byte,
 * 0..255), an ArrayBuffer or a typed array. Anything else - a string with a
 * character above 0xFF, a DataView, a non-buffer - comes back `undefined`, and
 * hull:encoding's own code then accepts it or throws its usual error. Decoded
 * bytes come back as a byte string.
 *
 * A JS string reaches C as UTF-8, so a byte string's characters 0x80..0xFF
 * arrive as two bytes each; they are folded back to one here, and the decoded
 * output is widened the same way on the way out (JS_NewStringLen reads UTF-8).
 *
 * No capability and no authority: pure computation, so no manifest gate.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "../../utils/base64.h"
#include "../../utils/hex.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The bytes of a byte string, ArrayBuffer or typed array. Returns 1 with
 * *data / *len set (and *owned = a malloc'd copy to free, or NULL), 0 when the
 * value is not one of those, -1 when out of memory. Never leaves an exception
 * pending on the 0 path. */
static int get_bytes(JSContext *ctx, JSValueConst v,
                     const uint8_t **data, size_t *len, uint8_t **owned)
{
    *owned = NULL;
    if (JS_IsString(v)) {
        size_t n;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s) return -1;
        uint8_t *out = malloc(n ? n : 1);
        if (!out) { JS_FreeCString(ctx, s); return -1; }
        size_t o = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x80) {
                out[o++] = c;
            } else if ((c == 0xC2 || c == 0xC3) && i + 1 < n
                       && ((unsigned char)s[i + 1] & 0xC0) == 0x80) {
                out[o++] = (uint8_t)(((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F));
                i++;
            } else {
                /* a character above 0xFF: not bytes */
                free(out);
                JS_FreeCString(ctx, s);
                return 0;
            }
        }
        JS_FreeCString(ctx, s);
        *data = out; *len = o; *owned = out;
        return 1;
    }
    if (!JS_IsObject(v)) return 0;

    size_t off = 0, blen = 0, per = 0;
    JSValue tab = JS_GetTypedArrayBuffer(ctx, v, &off, &blen, &per);
    if (!JS_IsException(tab)) {
        size_t ab_len = 0;
        uint8_t *raw = JS_GetArrayBuffer(ctx, &ab_len, tab);
        JS_FreeValue(ctx, tab);
        if (raw && off <= ab_len && blen <= ab_len - off) {
            *data = raw + off; *len = blen;
            return 1;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));   /* not a typed array */

    size_t ab_len = 0;
    uint8_t *raw = JS_GetArrayBuffer(ctx, &ab_len, v);
    if (raw) {
        *data = raw; *len = ab_len;
        return 1;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));   /* not an ArrayBuffer */
    return 0;
}

/* A byte string from raw bytes: each byte >= 0x80 is written as its two-byte
 * UTF-8 form, which JS_NewStringLen reads back as that one character. */
static JSValue new_byte_string(JSContext *ctx, const uint8_t *b, size_t n)
{
    if (n > (SIZE_MAX - 1) / 2)
        return JS_ThrowRangeError(ctx, "encoding: value too large");
    char *u = malloc(n * 2 + 1);
    if (!u) return JS_ThrowOutOfMemory(ctx);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i] < 0x80) {
            u[o++] = (char)b[i];
        } else {
            u[o++] = (char)(0xC0 | (b[i] >> 6));
            u[o++] = (char)(0x80 | (b[i] & 0x3F));
        }
    }
    JSValue r = JS_NewStringLen(ctx, u, o);
    free(u);
    return r;
}

static JSValue js_hex_encode(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const uint8_t *in; size_t n; uint8_t *owned;
    int got = get_bytes(ctx, argv[0], &in, &n, &owned);
    if (got < 0) return JS_ThrowOutOfMemory(ctx);
    if (got == 0) return JS_UNDEFINED;
    if (n > (SIZE_MAX - 1) / 2) { free(owned); return JS_ThrowRangeError(ctx, "encoding: value too large"); }
    char *out = malloc(n * 2 + 1);
    if (!out) { free(owned); return JS_ThrowOutOfMemory(ctx); }
    int rc = hl_hex_encode(in, n, out, n * 2 + 1);
    free(owned);
    JSValue r = rc == 0 ? JS_NewStringLen(ctx, out, n * 2) : JS_UNDEFINED;
    free(out);
    return r;
}

static JSValue js_hex_decode(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    size_t n;
    const char *text = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!text) return JS_EXCEPTION;
    uint8_t *out = malloc(n / 2 + 1);
    if (!out) { JS_FreeCString(ctx, text); return JS_ThrowOutOfMemory(ctx); }
    int got = hl_hex_decode(text, n, out, n / 2 + 1);
    JS_FreeCString(ctx, text);
    JSValue r = got < 0 ? JS_NULL : new_byte_string(ctx, out, (size_t)got);
    free(out);
    return r;
}

static JSValue js_base64_encode(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    unsigned flags = 0;
    if (argc > 1 && JS_ToBool(ctx, argv[1])) flags |= HL_BASE64_URL;
    if (!(argc > 2 && JS_ToBool(ctx, argv[2]))) flags |= HL_BASE64_NOPAD;
    const uint8_t *in; size_t n; uint8_t *owned;
    int got = get_bytes(ctx, argv[0], &in, &n, &owned);
    if (got < 0) return JS_ThrowOutOfMemory(ctx);
    if (got == 0) return JS_UNDEFINED;
    size_t len = hl_base64_encoded_len(n, flags);
    if (len == 0 && n != 0) { free(owned); return JS_ThrowRangeError(ctx, "encoding: value too large"); }
    char *out = malloc(len + 1);
    if (!out) { free(owned); return JS_ThrowOutOfMemory(ctx); }
    int rc = hl_base64_encode(in, n, out, len + 1, flags);
    free(owned);
    JSValue r = rc >= 0 ? JS_NewStringLen(ctx, out, len) : JS_UNDEFINED;
    free(out);
    return r;
}

static JSValue js_base64_decode(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NULL;
    /* The url alphabet is unpadded, so any '=' is refused there. */
    unsigned flags = (argc > 1 && JS_ToBool(ctx, argv[1]))
                         ? (HL_BASE64_URL | HL_BASE64_NOPAD) : 0;
    size_t n;
    const char *text = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!text) return JS_EXCEPTION;
    size_t cap = n / 4 * 3 + 3, got = 0;
    uint8_t *out = malloc(cap);
    if (!out) { JS_FreeCString(ctx, text); return JS_ThrowOutOfMemory(ctx); }
    int rc = hl_base64_decode(text, n, out, cap, &got, flags);
    JS_FreeCString(ctx, text);
    JSValue r = rc != 0 ? JS_NULL : new_byte_string(ctx, out, got);
    free(out);
    return r;
}

static int js_encoding_module_init(JSContext *ctx, JSModuleDef *m)
{
    JS_SetModuleExport(ctx, m, "hexEncode",
                       JS_NewCFunction(ctx, js_hex_encode, "hexEncode", 1));
    JS_SetModuleExport(ctx, m, "hexDecode",
                       JS_NewCFunction(ctx, js_hex_decode, "hexDecode", 1));
    JS_SetModuleExport(ctx, m, "base64Encode",
                       JS_NewCFunction(ctx, js_base64_encode, "base64Encode", 3));
    JS_SetModuleExport(ctx, m, "base64Decode",
                       JS_NewCFunction(ctx, js_base64_decode, "base64Decode", 2));
    return 0;
}

int hl_js_init_encoding_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:encoding:_native", js_encoding_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "hexEncode");
    JS_AddModuleExport(ctx, m, "hexDecode");
    JS_AddModuleExport(ctx, m, "base64Encode");
    JS_AddModuleExport(ctx, m, "base64Decode");
    return 0;
}
