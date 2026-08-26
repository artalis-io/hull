/*
 * test_hull_cap_image.c - Tests for image capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/image.h"

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
                                       pixels, sizeof(pixels), NULL);
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
                                       pixels, sizeof(pixels), NULL);
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

UTEST_MAIN();
