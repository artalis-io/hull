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

#include <stddef.h>     /* size_t */
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

/**
 * @brief Check the IMPLICIT default database file (`<app_dir>/data.db`, no
 *        `-d`) and its SQLite sidecars (`-wal`, `-shm`, `-journal`) before
 *        anything opens it.
 *
 * Each must be absent or a regular file, judged by lstat: SQLite follows a
 * symbolic link, so a cloned repository that commits `data.db ->
 * ../../other.sqlite` had `hull migrate` run the repo's migrations against
 * that database (audit 25). An explicit `-d` path is the operator's choice
 * and is not checked here.
 *
 * @param path   the default database path.
 * @param err    receives a one-line reason on refusal (may be NULL).
 * @returns 0 when the path may be opened, -1 when it must be refused.
 */
int hl_default_db_path_check(const char *path, char *err, size_t err_sz);

#endif /* HL_SHARED_FS_UTIL_H */
