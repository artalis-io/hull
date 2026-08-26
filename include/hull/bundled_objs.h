/*
 * bundled_objs.h - pre-compiled objects for the compiler-free `hull build`
 *
 * app_main.o and app_feature_registry-<rt>.o are invariant (they depend only
 * on the runtime, never on the app), so they are compiled once at Hull build
 * time (native format + arch) and xxd-embedded. `hull build --no-compiler`
 * extracts the matching blob and links it against an emitted app_registry.o
 * with no C compiler. See docs/compiler_free_build.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_BUNDLED_OBJS_H
#define HL_BUNDLED_OBJS_H

#include <stddef.h>

/*
 * Look up a bundled object by kind:
 *   "app_main"                  - the main() trampoline
 *   "app_feature_registry_lua"  - the Lua per-runtime feature registry
 *   "app_feature_registry_js"   - the JS per-runtime feature registry
 * On success returns 0 and sets *data / *len to the embedded object bytes
 * (not owned by the caller). Returns -1 on an unknown kind or when this build
 * embedded no bundled objects (HL_BUNDLE_OBJS off).
 */
int hl_bundled_object(const char *kind, const unsigned char **data, size_t *len);

#endif /* HL_BUNDLED_OBJS_H */
