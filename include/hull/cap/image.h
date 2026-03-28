/*
 * cap/image.h — Image capability with pluggable codec vtable
 *
 * First-class image type with four pixel formats and stb_image
 * as the default decoder/encoder. Integrates with the unified
 * buffer protocol (HlBufferView).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_IMAGE_H
#define HL_CAP_IMAGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct HlAllocator HlAllocator;

/* ── Pixel formats ─────────────────────────────────────────────────── */

typedef enum {
    HL_IMAGE_RGBA8    = 0,  /* 4 bytes/pixel */
    HL_IMAGE_R8       = 1,  /* 1 byte/pixel  */
    HL_IMAGE_RGBA16F  = 2,  /* 8 bytes/pixel */
    HL_IMAGE_R32F     = 3,  /* 4 bytes/pixel */
} HlImageFormat;

/* ── Image handle ──────────────────────────────────────────────────── */

typedef struct HlImage {
    uint32_t      width;
    uint32_t      height;
    HlImageFormat format;
    void         *pixels;
    size_t        pixel_len;
    int           owned;      /* 1 = we allocated pixels */
    HlAllocator  *alloc;
} HlImage;

/* ── Codec vtable ──────────────────────────────────────────────────── */

typedef struct HlImageCodec {
    const char *name;
    int (*can_decode)(const void *header, size_t len);
    int (*decode)(const void *src, size_t src_len,
                  void **pixels, uint32_t *w, uint32_t *h,
                  int requested_channels, HlAllocator *alloc);
    int (*encode)(const void *pixels, uint32_t w, uint32_t h,
                  int channels, int quality,
                  void **dst, size_t *dst_len, HlAllocator *alloc);
    void (*free_pixels)(void *pixels);
} HlImageCodec;

/* ── Utility ───────────────────────────────────────────────────────── */

/* Bytes per pixel for a given format */
int hl_image_bpp(HlImageFormat fmt);

/* Format name -> enum (-1 if unknown) */
int hl_image_format_from_name(const char *name);

/* Enum -> format name string */
const char *hl_image_format_name(HlImageFormat fmt);

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Create from raw pixels (copies data) */
HlImage *hl_image_new(uint32_t w, uint32_t h, HlImageFormat fmt,
                       const void *pixels, size_t pixel_len,
                       HlAllocator *alloc);

/* Create from buffer view (borrows — caller keeps source alive) */
HlImage *hl_image_from_view(uint32_t w, uint32_t h, HlImageFormat fmt,
                             const void *data, size_t len,
                             HlAllocator *alloc);

/* Decode from encoded bytes (auto-detects format if fmt_name is NULL) */
HlImage *hl_image_decode(const void *data, size_t len,
                          const char *fmt_name,
                          HlAllocator *alloc, const char **err_msg);

/* Encode to bytes */
int hl_image_encode(const HlImage *img, const char *fmt_name,
                     int quality,
                     void **out, size_t *out_len,
                     HlAllocator *alloc, const char **err_msg);

/* Free image (respects owned flag) */
void hl_image_free(HlImage *img);

/* ── Codec registry ────────────────────────────────────────────────── */

/* Register a codec (max HL_IMAGE_MAX_CODECS) */
void hl_image_register_codec(const HlImageCodec *codec);

/* Initialize stb codecs (call once at startup) */
void hl_image_stb_init(void);

/* ── Limits ────────────────────────────────────────────────────────── */

#define HL_IMAGE_MAX_CODECS 8
#define HL_IMAGE_MAX_DIM    65536

#endif /* HL_CAP_IMAGE_H */
