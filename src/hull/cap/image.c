/*
 * image.c — Image capability: core types, codec dispatch, lifecycle
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/image.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Static codec registry ─────────────────────────────────────────── */

static const HlImageCodec *g_codecs[HL_IMAGE_MAX_CODECS];
static int g_codec_count;
static int g_stb_inited;

void hl_image_register_codec(const HlImageCodec *codec)
{
    if (!codec || g_codec_count >= HL_IMAGE_MAX_CODECS)
        return;
    g_codecs[g_codec_count++] = codec;
}

static void ensure_stb_init(void)
{
    if (!g_stb_inited) {
        hl_image_stb_init();
        g_stb_inited = 1;
    }
}

/* ── Utility ───────────────────────────────────────────────────────── */

int hl_image_bpp(HlImageFormat fmt)
{
    switch (fmt) {
    case HL_IMAGE_RGBA8:   return 4;
    case HL_IMAGE_R8:      return 1;
    case HL_IMAGE_RGBA16F: return 8;
    case HL_IMAGE_R32F:    return 4;
    default:               return 0;
    }
}

int hl_image_format_from_name(const char *name)
{
    if (!name) return -1;
    if (strcmp(name, "rgba8") == 0)       return HL_IMAGE_RGBA8;
    if (strcmp(name, "r8") == 0)          return HL_IMAGE_R8;
    if (strcmp(name, "rgba16float") == 0) return HL_IMAGE_RGBA16F;
    if (strcmp(name, "r32float") == 0)    return HL_IMAGE_R32F;
    return -1;
}

const char *hl_image_format_name(HlImageFormat fmt)
{
    switch (fmt) {
    case HL_IMAGE_RGBA8:   return "rgba8";
    case HL_IMAGE_R8:      return "r8";
    case HL_IMAGE_RGBA16F: return "rgba16float";
    case HL_IMAGE_R32F:    return "r32float";
    default:               return "unknown";
    }
}

/* ── Overflow-safe size computation ────────────────────────────────── */

static int image_byte_size(uint32_t w, uint32_t h, int bpp, size_t *out)
{
    if (w == 0 || h == 0 || bpp <= 0)
        return -1;
    if (w > HL_IMAGE_MAX_DIM || h > HL_IMAGE_MAX_DIM)
        return -1;
    uint64_t total = (uint64_t)w * (uint64_t)h * (uint64_t)bpp;
    if (total > SIZE_MAX / 2)
        return -1;
    *out = (size_t)total;
    return 0;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

HlImage *hl_image_new(uint32_t w, uint32_t h, HlImageFormat fmt,
                       const void *pixels, size_t pixel_len,
                       HlAllocator *alloc)
{
    (void)alloc;
    int bpp = hl_image_bpp(fmt);
    size_t expected;
    if (image_byte_size(w, h, bpp, &expected) != 0)
        return NULL;
    if (pixel_len < expected)
        return NULL;

    HlImage *img = calloc(1, sizeof(*img));
    if (!img) return NULL;

    img->pixels = malloc(expected);
    if (!img->pixels) {
        free(img);
        return NULL;
    }
    memcpy(img->pixels, pixels, expected);
    img->width     = w;
    img->height    = h;
    img->format    = fmt;
    img->pixel_len = expected;
    img->owned     = 1;
    img->alloc     = alloc;
    return img;
}

HlImage *hl_image_from_view(uint32_t w, uint32_t h, HlImageFormat fmt,
                             const void *data, size_t len,
                             HlAllocator *alloc)
{
    (void)alloc;
    int bpp = hl_image_bpp(fmt);
    size_t expected;
    if (image_byte_size(w, h, bpp, &expected) != 0)
        return NULL;
    if (len < expected)
        return NULL;

    HlImage *img = calloc(1, sizeof(*img));
    if (!img) return NULL;

    img->pixels    = (void *)(uintptr_t)data; /* borrowed — NOT owned, never written when owned=0 */
    img->width     = w;
    img->height    = h;
    img->format    = fmt;
    img->pixel_len = expected;
    img->owned     = 0;
    img->alloc     = alloc;
    return img;
}

/* Channels for a format (for stb encode/decode) */
static int format_channels(HlImageFormat fmt)
{
    switch (fmt) {
    case HL_IMAGE_RGBA8:   return 4;
    case HL_IMAGE_R8:      return 1;
    case HL_IMAGE_RGBA16F: return 4;
    case HL_IMAGE_R32F:    return 1;
    default:               return 4;
    }
}

HlImage *hl_image_decode(const void *data, size_t len,
                          const char *fmt_name,
                          HlAllocator *alloc, const char **err_msg)
{
    ensure_stb_init();

    if (!data || len == 0) {
        if (err_msg) *err_msg = "empty input";
        return NULL;
    }
    if (len < 4) {
        if (err_msg) *err_msg = "input too small";
        return NULL;
    }

    /* Find a codec that can decode this data */
    const HlImageCodec *codec = NULL;
    for (int i = 0; i < g_codec_count; i++) {
        if (fmt_name) {
            if (strcmp(g_codecs[i]->name, fmt_name) == 0) {
                codec = g_codecs[i];
                break;
            }
        } else if (g_codecs[i]->can_decode(data, len)) {
            codec = g_codecs[i];
            break;
        }
    }
    if (!codec) {
        if (err_msg) *err_msg = "unsupported_format";
        return NULL;
    }

    void *pixels = NULL;
    uint32_t w = 0, h = 0;
    /* Decode as RGBA8 (4 channels) by default */
    int channels = 4;
    if (codec->decode(data, len, &pixels, &w, &h, channels, alloc) != 0) {
        if (err_msg) *err_msg = "decode_failed";
        return NULL;
    }

    size_t expected;
    if (image_byte_size(w, h, channels, &expected) != 0) {
        codec->free_pixels(pixels);
        if (err_msg) *err_msg = "decoded_dimensions_overflow";
        return NULL;
    }

    HlImage *img = calloc(1, sizeof(*img));
    if (!img) {
        codec->free_pixels(pixels);
        if (err_msg) *err_msg = "out_of_memory";
        return NULL;
    }

    img->width     = w;
    img->height    = h;
    img->format    = HL_IMAGE_RGBA8;
    img->pixels    = pixels;
    img->pixel_len = expected;
    img->owned     = 1;
    img->alloc     = alloc;
    /* Decoded pixels were allocated by the codec's allocator (stb uses its
     * own), so free them through the codec, not plain free(). */
    img->free_pixels = codec->free_pixels;

    return img;
}

int hl_image_encode(const HlImage *img, const char *fmt_name,
                     int quality,
                     void **out, size_t *out_len,
                     HlAllocator *alloc, const char **err_msg)
{
    ensure_stb_init();

    if (!img || !img->pixels || !out || !out_len) {
        if (err_msg) *err_msg = "invalid_args";
        return -1;
    }
    if (!fmt_name) {
        if (err_msg) *err_msg = "format_required";
        return -1;
    }

    /* Only support encoding RGBA8 and R8 (8-bit integer formats) */
    if (img->format != HL_IMAGE_RGBA8 && img->format != HL_IMAGE_R8) {
        if (err_msg) *err_msg = "unsupported_pixel_format";
        return -1;
    }

    /* Find codec by name */
    const HlImageCodec *codec = NULL;
    for (int i = 0; i < g_codec_count; i++) {
        if (strcmp(g_codecs[i]->name, fmt_name) == 0) {
            codec = g_codecs[i];
            break;
        }
    }
    if (!codec || !codec->encode) {
        if (err_msg) *err_msg = "unsupported_format";
        return -1;
    }

    int channels = format_channels(img->format);
    if (codec->encode(img->pixels, img->width, img->height,
                      channels, quality, out, out_len, alloc) != 0) {
        if (err_msg) *err_msg = "encode_failed";
        return -1;
    }

    return 0;
}

void hl_image_free(HlImage *img)
{
    if (!img) return;
    /* Release a borrowed zero-copy source first (may complete the source's
     * deferred munmap/free). Must run before we drop our borrowed `pixels`
     * pointer; for owned images this is NULL. */
    if (img->on_free)
        img->on_free(img->on_free_ctx);
    if (img->owned && img->pixels) {
        /* Decoded pixels are freed through the codec's allocator;
         * image.new() pixels (free_pixels == NULL) use plain free(). */
        if (img->free_pixels)
            img->free_pixels(img->pixels);
        else
            free(img->pixels);
    }
    free(img);
}
