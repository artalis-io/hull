/*
 * build_assets.h — Embedded platform library for hull build
 *
 * The hull binary carries a pre-compiled libhull_platform.a inside
 * itself. hull build extracts this to a temp dir, then links app code
 * against it. This module provides the extraction API.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_BUILD_ASSETS_H
#define HL_BUILD_ASSETS_H

#include <stddef.h>

/*
 * Extract the embedded platform library to `dir/libhull_platform.a`.
 * Returns 0 on success, -1 on error.
 */
int hl_build_extract_platform(const char *dir);

/*
 * Get the embedded app_main.c template (thin main() → hull_main() trampoline).
 * Sets *data and *len. Returns 0 on success.
 */
int hl_build_get_template(const char **data, size_t *len);

/*
 * Get the embedded HlStdlibEntry type definition header.
 * Sets *data and *len. Returns 0 on success.
 */
int hl_build_get_entry_header(const char **data, size_t *len);

#endif /* HL_BUILD_ASSETS_H */
