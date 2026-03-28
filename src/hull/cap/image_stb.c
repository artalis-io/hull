/*
 * image_stb.c — stb_image codec implementations for PNG, JPEG, BMP
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/image.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <stdlib.h>
#include <string.h>

/* ── Write callback accumulator ────────────────────────────────────── */

typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} WriteCtx;

static void write_cb(void *context, void *data, int size)
{
    WriteCtx *wc = (WriteCtx *)context;
    if (size <= 0) return;
    size_t needed = wc->len + (size_t)size;
    if (needed > wc->cap) {
        size_t newcap = wc->cap * 2;
        if (newcap < needed) newcap = needed;
        unsigned char *p = realloc(wc->data, newcap);
        if (!p) return; /* silent failure — stbi_write returns 0 */
        wc->data = p;
        wc->cap = newcap;
    }
    memcpy(wc->data + wc->len, data, (size_t)size);
    wc->len += (size_t)size;
}

/* ── PNG codec ─────────────────────────────────────────────────────── */

static int stb_can_decode_png(const void *header, size_t len)
{
    if (len < 4) return 0;
    const unsigned char *h = (const unsigned char *)header;
    return h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G';
}

static int stb_decode(const void *src, size_t src_len,
                      void **pixels, uint32_t *w, uint32_t *h,
                      int requested_channels, HlAllocator *alloc)
{
    (void)alloc;
    int iw, ih, channels_in_file;
    unsigned char *data = stbi_load_from_memory(
        (const unsigned char *)src, (int)src_len,
        &iw, &ih, &channels_in_file, requested_channels);
    if (!data) return -1;
    if (iw <= 0 || ih <= 0 || (uint32_t)iw > HL_IMAGE_MAX_DIM ||
        (uint32_t)ih > HL_IMAGE_MAX_DIM) {
        stbi_image_free(data);
        return -1;
    }
    *pixels = data;
    *w = (uint32_t)iw;
    *h = (uint32_t)ih;
    return 0;
}

static int stb_encode_png(const void *pixels, uint32_t w, uint32_t h,
                          int channels, int quality,
                          void **dst, size_t *dst_len, HlAllocator *alloc)
{
    (void)quality;
    (void)alloc;
    WriteCtx wc = { .data = NULL, .len = 0, .cap = 0 };
    /* Pre-allocate a reasonable buffer */
    wc.cap = (size_t)w * h * (size_t)channels / 2 + 256;
    wc.data = malloc(wc.cap);
    if (!wc.data) return -1;

    int stride = (int)((size_t)w * (size_t)channels);
    int rc = stbi_write_png_to_func(write_cb, &wc,
                                     (int)w, (int)h, channels,
                                     pixels, stride);
    if (rc == 0) {
        free(wc.data);
        return -1;
    }
    *dst = wc.data;
    *dst_len = wc.len;
    return 0;
}

static void stb_free_pixels(void *pixels)
{
    stbi_image_free(pixels);
}

static const HlImageCodec stb_png_codec = {
    .name        = "png",
    .can_decode  = stb_can_decode_png,
    .decode      = stb_decode,
    .encode      = stb_encode_png,
    .free_pixels = stb_free_pixels,
};

/* ── JPEG codec ────────────────────────────────────────────────────── */

static int stb_can_decode_jpeg(const void *header, size_t len)
{
    if (len < 3) return 0;
    const unsigned char *h = (const unsigned char *)header;
    return h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF;
}

static int stb_encode_jpeg(const void *pixels, uint32_t w, uint32_t h,
                           int channels, int quality,
                           void **dst, size_t *dst_len, HlAllocator *alloc)
{
    (void)alloc;
    if (quality <= 0) quality = 90;
    if (quality > 100) quality = 100;

    WriteCtx wc = { .data = NULL, .len = 0, .cap = 0 };
    wc.cap = (size_t)w * h * (size_t)channels / 4 + 256;
    wc.data = malloc(wc.cap);
    if (!wc.data) return -1;

    int rc = stbi_write_jpg_to_func(write_cb, &wc,
                                     (int)w, (int)h, channels,
                                     pixels, quality);
    if (rc == 0) {
        free(wc.data);
        return -1;
    }
    *dst = wc.data;
    *dst_len = wc.len;
    return 0;
}

static const HlImageCodec stb_jpeg_codec = {
    .name        = "jpeg",
    .can_decode  = stb_can_decode_jpeg,
    .decode      = stb_decode,
    .encode      = stb_encode_jpeg,
    .free_pixels = stb_free_pixels,
};

/* ── BMP codec ─────────────────────────────────────────────────────── */

static int stb_can_decode_bmp(const void *header, size_t len)
{
    if (len < 2) return 0;
    const unsigned char *h = (const unsigned char *)header;
    return h[0] == 'B' && h[1] == 'M';
}

static const HlImageCodec stb_bmp_codec = {
    .name        = "bmp",
    .can_decode  = stb_can_decode_bmp,
    .decode      = stb_decode,
    .encode      = NULL,   /* BMP encoding not supported */
    .free_pixels = stb_free_pixels,
};

/* ── Init ──────────────────────────────────────────────────────────── */

void hl_image_stb_init(void)
{
    hl_image_register_codec(&stb_png_codec);
    hl_image_register_codec(&stb_jpeg_codec);
    hl_image_register_codec(&stb_bmp_codec);
}
