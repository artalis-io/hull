/*
 * image_stub.c - weak per-runtime image-binding default (Lua).
 *
 * The image.* binding (mod_image) is a composable feature archive
 * (libhull_feature-image-lua.a, docs/image_feature.md). The pure runtime archive
 * still references luaopen_hull_image (modules.c's module register). When the
 * image bridge is composed its strong def wins; when it is NOT (an image-free
 * app, needs_image false) this weak no-op satisfies the link.
 *
 * Not reached in a way that matters: hl_lua_require gates hull.image on the
 * resolved module set (optional-absent -> nil; non-optional -> hard error before
 * load), so the empty module this would register is never handed to app code.
 *
 * Mirrors wasm_stub.c (the compute seam).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"   /* luaopen_hull_image decl, lua_State */

__attribute__((weak)) int luaopen_hull_image(lua_State *L)
{
    (void)L;
    return 0;   /* not registered meaningfully without the feature; satisfies link */
}
