/*
 * image_weakstub.c — weak image-cap defaults for an image-less base.
 *
 * The image codec (cap/image.c + cap/image_stb.c + vendored stb) is a composable
 * feature archive (libhull_feature-image.a, docs/image_feature.md). The base
 * runtime archive still references two of its cap symbols from mod_gpu
 * (gpu.texture_read builds an HlImage to return; the paired free), so these weak
 * no-ops satisfy the link when the image feature is NOT composed (an image-free
 * app). When it IS composed, the strong defs in cap/image.c win. Neither body is
 * reached in a way that matters: gpu.texture_read only produces an image when the
 * app declared hull/image (needs_image -> the image feature is composed), and
 * hl_image_free(NULL) is a no-op.
 *
 * Mirrors wasm_weakstub.c (the WASM seam). Runtime-agnostic — lives in the base
 * platform lib, linked into both Lua-only and JS-only apps.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/image.h"

__attribute__((weak)) HlImage *hl_image_new(uint32_t w, uint32_t h,
                                            HlImageFormat fmt,
                                            const void *pixels, size_t pixel_len,
                                            HlAllocator *alloc)
{
    (void)w; (void)h; (void)fmt; (void)pixels; (void)pixel_len; (void)alloc;
    return NULL;   /* image feature not composed; only satisfies the link */
}

__attribute__((weak)) void hl_image_free(HlImage *img)
{
    (void)img;   /* no HlImage exists without the feature; NULL-free is a no-op */
}
