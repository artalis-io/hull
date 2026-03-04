/*
 * vfs.h — Unified Virtual Filesystem for embedded entry lookup
 *
 * Provides O(log n) sorted lookups and prefix queries over HlEntry arrays,
 * with centralized filesystem path construction. Two instances at runtime:
 *   - platform_vfs: stdlib entries (always embedded, root_dir = NULL)
 *   - app_vfs:      app entries (embedded in build mode, filesystem fallback in dev)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_VFS_H
#define HL_VFS_H

#include "hull/entry.h"
#include <stddef.h>

typedef struct HlVfs {
    const HlEntry *entries;   /* sorted by name, sentinel-terminated */
    size_t         count;     /* number of entries (excl sentinel) */
    const char    *root_dir;  /* filesystem root for dev mode, or NULL */
} HlVfs;

/*
 * Count entries and debug-assert sort order.
 * `entries` must be a sentinel-terminated array (last entry has name == NULL).
 * `root_dir` may be NULL (no filesystem fallback).
 */
void hl_vfs_init(HlVfs *vfs, const HlEntry *entries, const char *root_dir);

/*
 * O(log n) exact lookup by name.
 * Returns pointer to the matching entry, or NULL if not found.
 */
const HlEntry *hl_vfs_find(const HlVfs *vfs, const char *name);

/*
 * O(log n) prefix query.
 * Sets *first to the first matching entry and returns the count of
 * contiguous entries whose names start with `prefix`.
 * If no matches, sets *first to NULL and returns 0.
 */
size_t hl_vfs_prefix(const HlVfs *vfs, const char *prefix,
                     const HlEntry **first);

/*
 * O(log n) prefix existence check.
 * Returns 1 if any entry name starts with `prefix`, 0 otherwise.
 */
int hl_vfs_has_prefix(const HlVfs *vfs, const char *prefix);

/*
 * Build filesystem path: root_dir/name → buf.
 * Returns the number of characters written (excluding NUL), or -1 on error
 * (NULL root_dir, buffer overflow, or invalid arguments).
 */
int hl_vfs_path(const HlVfs *vfs, const char *name,
                char *buf, size_t buf_size);

#endif /* HL_VFS_H */
