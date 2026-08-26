/*
 * bundled_objs.c - accessor for the xxd-embedded compiler-free build objects.
 *
 * The byte arrays live in the generated embedded_bundled_objs.h (produced by
 * the Makefile from templates/app_main.c + templates/app_feature_registry_*.c
 * compiled for this build's native format + arch). When HL_BUNDLE_OBJS is off
 * the header is absent and every lookup fails closed. See
 * docs/compiler_free_build.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/bundled_objs.h"

#include <string.h>

#ifdef HL_BUNDLE_OBJS
#include "embedded_bundled_objs.h"
#endif

int hl_bundled_object(const char *kind, const unsigned char **data, size_t *len)
{
    if (!kind || !data || !len) return -1;
#ifdef HL_BUNDLE_OBJS
    if (strcmp(kind, "app_main") == 0) {
        *data = bundled_app_main; *len = bundled_app_main_len; return 0;
    }
    if (strcmp(kind, "app_feature_registry_lua") == 0) {
        *data = bundled_afr_lua; *len = bundled_afr_lua_len; return 0;
    }
    if (strcmp(kind, "app_feature_registry_js") == 0) {
        *data = bundled_afr_js; *len = bundled_afr_js_len; return 0;
    }
#else
    (void)kind; (void)data; (void)len;
#endif
    return -1;
}
