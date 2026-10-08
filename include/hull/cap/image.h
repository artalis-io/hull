/**
 * @file cap/image.h
 * @brief Image decode / encode / pixel buffers (stb_image-backed).
 *
 * First-class image type with four pixel formats (`rgba8`, `r8`,
 * `rgba16float`, `r32float`) and a pluggable codec vtable. `stb_image`
 * is the default decoder (PNG, JPEG, BMP) and encoder.
 *
 * Integrates with the unified buffer protocol (#HlBufferView) so a
 * decoded image can be passed to `gpu.texture()` or `compute.call()`
 * without copying.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_IMAGE_H
#define HL_CAP_IMAGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct HlAllocator HlAllocator;

/* Where an image's OWNED pixels are allocated: the runtime passes its VM
 * allocator (Lua's lua_Alloc, QuickJS's js_malloc_rt), so pixel buffers count
 * against the app's heap limit like any other VM memory. `free` gets the size
 * `malloc` was asked for. NULL = plain malloc/free (tools, tests). */
typedef struct HlImageAlloc {
    void *(*malloc)(void *ctx, size_t size);
    void  (*free)(void *ctx, void *ptr, size_t size);
    void  *ctx;
} HlImageAlloc;

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
    /* Owned pixels came from here (pixel_len bytes); a zero `malloc` means
     * plain malloc. Decoded pixels are copied out of the codec's buffer into
     * it, so every owned buffer is freed the same way. */
    HlImageAlloc  pixel_alloc;
    /* Optional borrow-release hook. Called by hl_image_free regardless of
     * `owned`, BEFORE the pixels/struct are freed. image.from_buffer uses it
     * to release a refcounted zero-copy source (HlMappedBuffer / HlWasmBuffer)
     * whose teardown was deferred while this image borrowed its pixels. NULL
     * for owned (copied) images. */
    void        (*on_free)(void *ctx);
    void         *on_free_ctx;
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

/* Create from raw pixels (copies data into @p alloc's memory). NULL on bad
 * dimensions / short data, or when @p alloc refuses the pixel buffer. */
HlImage *hl_image_new(uint32_t w, uint32_t h, HlImageFormat fmt,
                       const void *pixels, size_t pixel_len,
                       const HlImageAlloc *alloc);

/* Create from buffer view (borrows - caller keeps source alive) */
HlImage *hl_image_from_view(uint32_t w, uint32_t h, HlImageFormat fmt,
                             const void *data, size_t len);

/* Decode from encoded bytes (auto-detects format if fmt_name is NULL). The
 * pixels end up in @p alloc's memory ("out_of_memory" when it refuses). */
HlImage *hl_image_decode(const void *data, size_t len,
                          const char *fmt_name,
                          const HlImageAlloc *alloc, const char **err_msg);

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
/* Pixels in one image (8192 x 8192; ~268 MB at RGBA). A side may still reach
 * HL_IMAGE_MAX_DIM, but not both at once: a 200-byte header declaring
 * 23000 x 23000 asked the decoder for 2 GB. */
#define HL_IMAGE_MAX_PIXELS ((uint64_t)1 << 26)

#endif /* HL_CAP_IMAGE_H */
