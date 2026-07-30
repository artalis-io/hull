/*
 * image_stub.c — weak per-runtime image-binding default (JS).
 *
 * The image.* binding (mod_image) is a composable feature archive
 * (libhull_feature-image-js.a, docs/image_feature.md). The pure runtime archive
 * still references hl_js_init_image_module (modules.c's module register). When
 * the image bridge is composed its strong def wins; when it is NOT (an
 * image-free app, needs_image false) this weak no-op satisfies the link.
 *
 * Not reached in a way that matters: the JS module loader gates hull:image on
 * the resolved module set (optional-absent -> a synthesized null export;
 * non-optional -> hard error before load), so no real module is expected here.
 *
 * Mirrors wasm_stub.c (the compute seam).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"   /* hl_js_init_image_module decl, JSContext, HlJS */

__attribute__((weak)) int hl_js_init_image_module(JSContext *ctx, HlJS *js)
{
    (void)ctx; (void)js;
    return 0;   /* not registered meaningfully without the feature; satisfies link */
}
