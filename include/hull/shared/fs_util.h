/**
 * @file fs_util.h
 * @brief Tiny, zero-dependency filesystem helpers shared across Hull.
 *
 * Just directory creation for now. These were copy-pasted (three times) into
 * blob_store.c, cache_dir.c, and tools_install.c; this is the single home. No
 * Hull dependencies (pure libc), so it links into any target for free.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_FS_UTIL_H
#define HL_SHARED_FS_UTIL_H

#include <sys/stat.h>   /* mode_t */

/**
 * @brief mkdir @p path if absent; success if it already exists as a directory.
 * @returns 0 on success, -1 (errno set) if it exists as a non-directory or
 *          mkdir failed.
 */
int hl_ensure_dir(const char *path, mode_t mode);

/**
 * @brief mkdir every component of @p path (parents included), @p mode each.
 *        An existing directory component is not an error.
 * @returns 0 on success, -1 on failure (errno set; ENAMETOOLONG if @p path
 *          is empty or exceeds PATH_MAX).
 */
int hl_mkdir_p(const char *path, mode_t mode);

#endif /* HL_SHARED_FS_UTIL_H */
