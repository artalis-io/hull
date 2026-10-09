/*
 * test_hull_cap_image.c - Tests for image capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/image.h"
#include "hull/utils/alloc.h"

#include <stdlib.h>
#include <string.h>

/* ── Minimal 1x1 red PNG (70 bytes) ───────────────────────────────── */

static const unsigned char minimal_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, /* PNG signature */
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, /* IHDR chunk */
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
    0xde,
    0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, /* IDAT chunk */
    0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0x00, 0x00,
    0x00, 0x03, 0x00, 0x01, 0x36, 0x28, 0x19, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, /* IEND chunk */
    0xae, 0x42, 0x60, 0x82,
};

/* ── Tests ─────────────────────────────────────────────────────────── */

UTEST(hull_cap_image, bpp_values)
{
    ASSERT_EQ(4, hl_image_bpp(HL_IMAGE_RGBA8));
    ASSERT_EQ(1, hl_image_bpp(HL_IMAGE_R8));
    ASSERT_EQ(8, hl_image_bpp(HL_IMAGE_RGBA16F));
    ASSERT_EQ(4, hl_image_bpp(HL_IMAGE_R32F));
}

UTEST(hull_cap_image, format_names)
{
    ASSERT_EQ(HL_IMAGE_RGBA8,   hl_image_format_from_name("rgba8"));
    ASSERT_EQ(HL_IMAGE_R8,      hl_image_format_from_name("r8"));
    ASSERT_EQ(HL_IMAGE_RGBA16F, hl_image_format_from_name("rgba16float"));
    ASSERT_EQ(HL_IMAGE_R32F,    hl_image_format_from_name("r32float"));
    ASSERT_EQ(-1,               hl_image_format_from_name("bogus"));
    ASSERT_EQ(-1,               hl_image_format_from_name(NULL));

    ASSERT_STREQ("rgba8",       hl_image_format_name(HL_IMAGE_RGBA8));
    ASSERT_STREQ("r8",          hl_image_format_name(HL_IMAGE_R8));
    ASSERT_STREQ("rgba16float", hl_image_format_name(HL_IMAGE_RGBA16F));
    ASSERT_STREQ("r32float",    hl_image_format_name(HL_IMAGE_R32F));
}

UTEST(hull_cap_image, new_rgba8)
{
    /* 2x2 RGBA8 = 16 bytes */
    uint8_t pixels[16];
    memset(pixels, 0xAB, sizeof(pixels));

    HlImage *img = hl_image_new(2, 2, HL_IMAGE_RGBA8,
                                 pixels, sizeof(pixels), NULL);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ(2u, img->width);
    ASSERT_EQ(2u, img->height);
    ASSERT_EQ(HL_IMAGE_RGBA8, img->format);
    ASSERT_EQ(16u, img->pixel_len);
    ASSERT_EQ(1, img->owned);

    /* Verify data was copied (not aliased) */
    ASSERT_NE(pixels, (uint8_t *)img->pixels);
    ASSERT_EQ(0, memcmp(img->pixels, pixels, 16));

    hl_image_free(img);
}

UTEST(hull_cap_image, new_r8)
{
    /* 4x4 grayscale = 16 bytes */
    uint8_t pixels[16];
    memset(pixels, 0x55, sizeof(pixels));

    HlImage *img = hl_image_new(4, 4, HL_IMAGE_R8,
                                 pixels, sizeof(pixels), NULL);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ(4u, img->width);
    ASSERT_EQ(4u, img->height);
    ASSERT_EQ(HL_IMAGE_R8, img->format);
    ASSERT_EQ(16u, img->pixel_len);

    hl_image_free(img);
}

UTEST(hull_cap_image, new_r32f)
{
    /* 2x2 R32F = 16 bytes */
    float pixels[4] = { 1.0f, 0.5f, 0.25f, 0.0f };

    HlImage *img = hl_image_new(2, 2, HL_IMAGE_R32F,
                                 pixels, sizeof(pixels), NULL);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ(2u, img->width);
    ASSERT_EQ(2u, img->height);
    ASSERT_EQ(HL_IMAGE_R32F, img->format);
    ASSERT_EQ(16u, img->pixel_len);

    hl_image_free(img);
}

UTEST(hull_cap_image, from_view)
{
    uint8_t pixels[8];
    memset(pixels, 0xCD, sizeof(pixels));

    HlImage *img = hl_image_from_view(2, 1, HL_IMAGE_RGBA8,
                                       pixels, sizeof(pixels));
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ(2u, img->width);
    ASSERT_EQ(1u, img->height);
    ASSERT_EQ(0, img->owned);
    /* Borrowed pointer - same address */
    ASSERT_EQ(pixels, (uint8_t *)img->pixels);

    hl_image_free(img);  /* should NOT free pixels */
}

/* Regression (audit F1): hl_image_free must invoke the on_free borrow-release
 * hook exactly once, before dropping the (borrowed) pixels. This is the seam
 * image.from_buffer uses to release a refcounted mmap/WASM source. */
static void test_image_on_free(void *ctx) { (*(int *)ctx)++; }

UTEST(hull_cap_image, on_free_hook_runs_once)
{
    uint8_t pixels[8];
    memset(pixels, 0xAB, sizeof(pixels));

    HlImage *img = hl_image_from_view(2, 1, HL_IMAGE_RGBA8,
                                       pixels, sizeof(pixels));
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ(0, img->owned);

    int released = 0;
    img->on_free = test_image_on_free;
    img->on_free_ctx = &released;

    hl_image_free(img);
    ASSERT_EQ(1, released);  /* hook ran exactly once */
}

UTEST(hull_cap_image, free_null)
{
    /* Should not crash */
    hl_image_free(NULL);
}

UTEST(hull_cap_image, overflow_rejected)
{
    uint8_t pixel = 0;
    /* Huge dimensions should be rejected */
    HlImage *img = hl_image_new(100000, 100000, HL_IMAGE_RGBA8,
                                 &pixel, 1, NULL);
    ASSERT_TRUE(img == NULL);

    /* Zero dimensions should be rejected */
    img = hl_image_new(0, 10, HL_IMAGE_RGBA8, &pixel, 1, NULL);
    ASSERT_TRUE(img == NULL);
}

UTEST(hull_cap_image, data_too_small)
{
    uint8_t pixels[4]; /* only 4 bytes, but 2x2 RGBA8 needs 16 */
    HlImage *img = hl_image_new(2, 2, HL_IMAGE_RGBA8,
                                 pixels, sizeof(pixels), NULL);
    ASSERT_TRUE(img == NULL);
}

/* The 1x1 PNG with its IHDR rewritten to w x h (stb does not check CRCs). */
static void png_with_size(unsigned char *out, uint32_t w, uint32_t h)
{
    memcpy(out, minimal_png, sizeof(minimal_png));
    for (int i = 0; i < 4; i++) {
        out[16 + i] = (unsigned char)(w >> (24 - 8 * i));
        out[20 + i] = (unsigned char)(h >> (24 - 8 * i));
    }
}

/* A header's declared size is checked before decoding: stb allocated the
 * whole buffer first, so a 70-byte file claiming 23000 x 23000 cost 2 GB.
 * 8193 x 8193 is under the per-side limit but over the pixel budget. */
UTEST(hull_cap_image, decode_refuses_a_header_past_the_pixel_budget)
{
    unsigned char png[sizeof(minimal_png)];
    const char *err = NULL;

    png_with_size(png, 23000, 23000);
    EXPECT_TRUE(hl_image_decode(png, sizeof png, NULL, NULL, &err) == NULL);

    png_with_size(png, 8193, 8193);
    EXPECT_TRUE(hl_image_decode(png, sizeof png, NULL, NULL, &err) == NULL);

    png_with_size(png, 1, 1);
    HlImage *img = hl_image_decode(png, sizeof png, NULL, NULL, &err);
    ASSERT_TRUE(img != NULL);
    hl_image_free(img);
}

UTEST(hull_cap_image, decode_png)
{
    const char *err = NULL;
    HlImage *img = hl_image_decode(minimal_png, sizeof(minimal_png),
                                    NULL, NULL, &err);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ(1u, img->width);
    ASSERT_EQ(1u, img->height);
    ASSERT_EQ(HL_IMAGE_RGBA8, img->format);
    /* 1x1 RGBA8 = 4 bytes */
    ASSERT_EQ(4u, img->pixel_len);
    ASSERT_TRUE(img->pixels != NULL);

    hl_image_free(img);
}

UTEST(hull_cap_image, encode_png)
{
    /* Create a 2x2 RGBA8 image */
    uint8_t pixels[16];
    memset(pixels, 0xFF, sizeof(pixels));

    HlImage *img = hl_image_new(2, 2, HL_IMAGE_RGBA8,
                                 pixels, sizeof(pixels), NULL);
    ASSERT_TRUE(img != NULL);

    void *out = NULL;
    size_t out_len = 0;
    const char *err = NULL;

    int rc = hl_image_encode(img, "png", 0, &out, &out_len, NULL, &err);
    ASSERT_EQ(0, rc);
    ASSERT_TRUE(out != NULL);
    ASSERT_TRUE(out_len > 8);

    /* Verify PNG magic */
    const uint8_t *p = (const uint8_t *)out;
    ASSERT_EQ(0x89, p[0]);
    ASSERT_EQ('P',  p[1]);
    ASSERT_EQ('N',  p[2]);
    ASSERT_EQ('G',  p[3]);

    free(out);
    hl_image_free(img);
}

UTEST(hull_cap_image, decode_invalid)
{
    const char *err = NULL;
    uint8_t junk[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
    HlImage *img = hl_image_decode(junk, sizeof(junk), NULL, NULL, &err);
    ASSERT_TRUE(img == NULL);
    ASSERT_TRUE(err != NULL);
}

UTEST(hull_cap_image, decode_empty)
{
    const char *err = NULL;
    HlImage *img = hl_image_decode(NULL, 0, NULL, NULL, &err);
    ASSERT_TRUE(img == NULL);
    ASSERT_STREQ("empty input", err);
}

UTEST(hull_cap_image, encode_unsupported_format)
{
    /* R32F cannot be encoded - only RGBA8 and R8 */
    float pixels[4] = { 1.0f, 0.5f, 0.25f, 0.0f };
    HlImage *img = hl_image_new(2, 2, HL_IMAGE_R32F,
                                 pixels, sizeof(pixels), NULL);
    ASSERT_TRUE(img != NULL);

    void *out = NULL;
    size_t out_len = 0;
    const char *err = NULL;

    int rc = hl_image_encode(img, "png", 0, &out, &out_len, NULL, &err);
    ASSERT_NE(0, rc);
    ASSERT_STREQ("unsupported_pixel_format", err);

    hl_image_free(img);
}

UTEST(hull_cap_image, roundtrip_png)
{
    /* Encode then decode, verify pixel data survives */
    uint8_t pixels[16];
    for (int i = 0; i < 16; i++) pixels[i] = (uint8_t)(i * 17);

    HlImage *img = hl_image_new(2, 2, HL_IMAGE_RGBA8,
                                 pixels, sizeof(pixels), NULL);
    ASSERT_TRUE(img != NULL);

    void *encoded = NULL;
    size_t enc_len = 0;
    ASSERT_EQ(0, hl_image_encode(img, "png", 0, &encoded, &enc_len, NULL, NULL));

    HlImage *decoded = hl_image_decode(encoded, enc_len, NULL, NULL, NULL);
    ASSERT_TRUE(decoded != NULL);
    ASSERT_EQ(2u, decoded->width);
    ASSERT_EQ(2u, decoded->height);
    ASSERT_EQ(16u, decoded->pixel_len);
    ASSERT_EQ(0, memcmp(decoded->pixels, pixels, 16));

    free(encoded);
    hl_image_free(img);
    hl_image_free(decoded);
}

/* Audit 12: stb_impl.c caps the SUM of stb_image's live blocks, not each
 * one. stb itself, driven directly: a cap every block fits under alone but
 * the decode's blocks together do not is refused; the live count returns to
 * zero after every decode; a real decode fits the cap image_stb.c sets. */
extern _Thread_local size_t hl_stb_alloc_cap;
extern _Thread_local size_t hl_stb_live;
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len,
                                     int *x, int *y, int *channels_in_file,
                                     int desired_channels);
void stbi_image_free(void *retval_from_stbi_load);

static int encode_test_png(uint32_t w, uint32_t h, void **out, size_t *len)
{
    size_t n = (size_t)w * h * 4u;
    uint8_t *px = malloc(n);
    if (!px) return -1;
    for (size_t i = 0; i < n; i++) px[i] = (uint8_t)((i * 2654435761u) >> 13);
    HlImage *img = hl_image_new(w, h, HL_IMAGE_RGBA8, px, n, NULL);
    free(px);
    if (!img) return -1;
    int rc = hl_image_encode(img, "png", 0, out, len, NULL, NULL);
    hl_image_free(img);
    return rc;
}

UTEST(hull_cap_image, stb_cap_bounds_the_sum_of_live_blocks)
{
    void *enc = NULL;
    size_t enc_len = 0;
    ASSERT_EQ(0, encode_test_png(64, 64, &enc, &enc_len));

    /* Uncapped: decodes, and every block is handed back. */
    size_t before = hl_stb_live;
    int w = 0, h = 0, ch = 0;
    hl_stb_alloc_cap = 0;
    unsigned char *px = stbi_load_from_memory(enc, (int)enc_len, &w, &h, &ch, 4);
    ASSERT_TRUE(px != NULL);
    ASSERT_TRUE(hl_stb_live > before);
    stbi_image_free(px);
    ASSERT_EQ(before, hl_stb_live);

    /* The largest block of this decode is the inflated raw image,
     * 64 x (1 + 64 x 4) = 16448 bytes (the output is 16384). A cap above
     * that holds each block alone - which the old per-block cap allowed -
     * but the raw and the output are live together, so the sum is refused. */
    hl_stb_alloc_cap = 24u * 1024u;
    px = stbi_load_from_memory(enc, (int)enc_len, &w, &h, &ch, 4);
    hl_stb_alloc_cap = 0;
    if (px) stbi_image_free(px);
    ASSERT_TRUE(px == NULL);
    ASSERT_EQ(before, hl_stb_live);   /* the failed decode freed all it held */

    free(enc);
}

UTEST(hull_cap_image, stb_total_cap_admits_a_real_decode)
{
    /* Through hl_image_decode: image_stb.c sets the total cap from the
     * header; an ordinary 512x384 RGBA PNG decodes under it and leaves
     * nothing counted live. */
    void *enc = NULL;
    size_t enc_len = 0;
    ASSERT_EQ(0, encode_test_png(512, 384, &enc, &enc_len));
    size_t before = hl_stb_live;
    const char *err = NULL;
    HlImage *img = hl_image_decode(enc, enc_len, NULL, NULL, &err);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ(512u, img->width);
    ASSERT_EQ(384u, img->height);
    ASSERT_EQ(0u, (unsigned)hl_stb_alloc_cap);
    ASSERT_EQ(before, hl_stb_live);
    hl_image_free(img);
    free(enc);
}

/* Audit 9 M2: owned pixels come from the caller's allocator (the VM's, in
 * the runtimes), so they count against its limit and are handed back on
 * free. They were plain malloc - and decoded ones stb's - outside the app's
 * 64 MB heap limit. A tracking HlAllocator stands in for the VM here. */
static void *test_px_malloc(void *ctx, size_t size)
{
    return hl_alloc_malloc((HlAllocator *)ctx, size);
}

static void test_px_free(void *ctx, void *ptr, size_t size)
{
    hl_alloc_free((HlAllocator *)ctx, ptr, size);
}

UTEST(hull_cap_image, new_pixels_counted_against_allocator)
{
    HlAllocator a;
    hl_alloc_init(&a, 64);
    HlImageAlloc ia = { test_px_malloc, test_px_free, &a };

    uint8_t pixels[256];
    memset(pixels, 0x5A, sizeof pixels);

    /* 4x4 RGBA8 = 64 bytes: fits, and is charged. */
    HlImage *img = hl_image_new(4, 4, HL_IMAGE_RGBA8, pixels, sizeof pixels, &ia);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ((size_t)64, hl_alloc_used(&a));
    ASSERT_EQ(0, memcmp(img->pixels, pixels, 64));

    /* A second one would pass the limit: refused, nothing leaks. */
    HlImage *over = hl_image_new(1, 1, HL_IMAGE_RGBA8, pixels, 4, &ia);
    ASSERT_TRUE(over == NULL);
    ASSERT_EQ((size_t)64, hl_alloc_used(&a));

    hl_image_free(img);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
}

UTEST(hull_cap_image, decoded_pixels_counted_against_allocator)
{
    HlAllocator a;
    hl_alloc_init(&a, 0);
    HlImageAlloc ia = { test_px_malloc, test_px_free, &a };
    const char *err = NULL;

    HlImage *img = hl_image_decode(minimal_png, sizeof(minimal_png),
                                    NULL, &ia, &err);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ((size_t)4, hl_alloc_used(&a));   /* 1x1 RGBA8 */
    hl_image_free(img);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));

    /* Over the limit: a clean failure, not an image outside it. */
    hl_alloc_init(&a, 3);
    err = NULL;
    img = hl_image_decode(minimal_png, sizeof(minimal_png), NULL, &ia, &err);
    ASSERT_TRUE(img == NULL);
    ASSERT_STREQ("out_of_memory", err);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
}

/* Audit 10: the pixels are reserved in the caller's allocator from the
 * header BEFORE the codec decodes (stb's buffers are outside the heap
 * limit). A recording allocator shows the one request is the reservation. */
typedef struct {
    HlAllocator *a;
    int          calls;
    size_t       last;
} RecAlloc;

static void *rec_px_malloc(void *ctx, size_t size)
{
    RecAlloc *r = (RecAlloc *)ctx;
    r->calls++;
    r->last = size;
    return hl_alloc_malloc(r->a, size);
}

static void rec_px_free(void *ctx, void *ptr, size_t size)
{
    hl_alloc_free(((RecAlloc *)ctx)->a, ptr, size);
}

UTEST(hull_cap_image, decode_reserves_pixels_before_decoding)
{
    HlAllocator a;
    hl_alloc_init(&a, (size_t)1 << 20);   /* 1 MB "heap" */
    RecAlloc r = { &a, 0, 0 };
    HlImageAlloc ia = { rec_px_malloc, rec_px_free, &r };
    unsigned char png[sizeof(minimal_png)];
    const char *err = NULL;

    /* 4000 x 4000 RGBA = 64 MB: refused from the header, before stb ran
     * (the truncated body would otherwise fail as decode_failed). */
    png_with_size(png, 4000, 4000);
    ASSERT_TRUE(hl_image_decode(png, sizeof png, NULL, &ia, &err) == NULL);
    ASSERT_STREQ("out_of_memory", err);
    ASSERT_EQ(1, r.calls);
    ASSERT_EQ((size_t)4000 * 4000 * 4, r.last);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));

    /* Fits: reserved, the decode then fails on the truncated body, and the
     * reservation is handed back. */
    r.calls = 0;
    err = NULL;
    png_with_size(png, 100, 100);
    ASSERT_TRUE(hl_image_decode(png, sizeof png, NULL, &ia, &err) == NULL);
    ASSERT_STREQ("decode_failed", err);
    ASSERT_EQ(1, r.calls);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
}

UTEST(hull_cap_image, info_reads_header_only)
{
    unsigned char png[sizeof(minimal_png)];
    uint32_t w = 0, h = 0;
    png_with_size(png, 640, 480);
    ASSERT_EQ(0, hl_image_info(png, sizeof png, NULL, &w, &h));
    ASSERT_EQ((uint32_t)640, w);
    ASSERT_EQ((uint32_t)480, h);
    /* over the pixel budget: refused, as decode does */
    png_with_size(png, 8193, 8193);
    ASSERT_EQ(-1, hl_image_info(png, sizeof png, NULL, &w, &h));
    /* not an image */
    ASSERT_EQ(-1, hl_image_info("nope", 4, NULL, &w, &h));
    ASSERT_EQ(-1, hl_image_info(NULL, 0, NULL, &w, &h));
}

UTEST(hull_cap_image, codec_units)
{
    /* one unit per 8 bytes of RGBA pixels plus one per 8 encoded bytes */
    ASSERT_EQ((uint64_t)0, hl_image_codec_units(0, 0, 0));
    ASSERT_EQ((uint64_t)2, hl_image_codec_units(2, 2, 0));
    ASSERT_EQ((uint64_t)3, hl_image_codec_units(2, 2, 8));
    ASSERT_EQ((uint64_t)8192 * 8192 / 2,
              hl_image_codec_units(8192, 8192, 0));
    /* saturates */
    ASSERT_TRUE(hl_image_codec_units(UINT32_MAX, UINT32_MAX, SIZE_MAX) > 0);
}

UTEST_MAIN();
