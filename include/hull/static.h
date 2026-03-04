/*
 * static.h — Static file serving middleware for Hull
 *
 * Convention: files in static/ are served at /static/.
 * Dev mode reads from disk; build mode uses embedded entries.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_STATIC_H
#define HL_STATIC_H

#include "hull/vfs.h"

#include <keel/request.h>
#include <keel/response.h>
#include <stddef.h>

typedef struct {
    const HlVfs *vfs;             /* unified VFS: embedded + filesystem */
} HlStaticCtx;

/**
 * @brief Keel pre-body middleware that serves static files.
 *
 * Checks if path starts with /static/. If the file exists (embedded or
 * on disk), writes the response and returns 1 (short-circuit).
 * Otherwise returns 0 (continue to next middleware/handler).
 */
int hl_static_middleware(KlRequest *req, KlResponse *res, void *user_data);

/**
 * @brief Return MIME type string for a file path based on extension.
 * @param path     File path (need not be null-terminated).
 * @param path_len Length of path.
 * @return Static string with Content-Type value.
 */
const char *hl_static_mime_type(const char *path, size_t path_len);

#endif /* HL_STATIC_H */
