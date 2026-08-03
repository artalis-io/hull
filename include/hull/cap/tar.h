/**
 * @file cap/tar.h
 * @brief ustar (.tar) parse / extract / create - the shared C core.
 *
 * Pure format handling (bytes <-> entries) with one trusted extract helper. Used
 * by BOTH `hull tools install` (C-side extraction of a SHA-256-verified bundle)
 * and the `hull.tar` stdlib module (Lua/JS bindings over parse/create; the
 * stdlib's `extract` composes parse + the fs capability so app extraction stays
 * inside the fs sandbox). No new authority: parse/create are pure; hl_tar_extract
 * is for hull's own trusted install path only.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TAR_H
#define HL_CAP_TAR_H

#include <stddef.h>

/**
 * @brief One archive member, as seen by hl_tar_parse.
 *
 * @c name / @c data borrow into the input archive bytes and are valid only for
 * the duration of the callback. @c name is normalized (a leading "./" and any
 * trailing "/" stripped) and validated (never absolute, never containing a
 * ".." component); the archive root ("./") is skipped, not reported.
 */
typedef struct {
    const char          *name;   /**< member path (normalized, NUL-terminated) */
    const unsigned char *data;   /**< file contents (NULL for a directory) */
    size_t               size;   /**< content length (0 for a directory) */
    unsigned             mode;   /**< permission bits, e.g. 0755 */
    int                  is_dir; /**< 1 = directory, 0 = regular file */
} HlTarEntry;

/**
 * @brief Iterate a ustar archive, invoking @p cb per regular-file / directory
 *        member. Other typeflags (symlinks, hardlinks, ...) are skipped.
 *
 * @p cb returns 0 to continue or non-0 to stop early (that value is returned).
 * PURE - no I/O. Rejects an archive with an absolute or ".."-bearing member
 * name (returns -1) as defense in depth.
 *
 * @returns 0 on clean end, the callback's non-zero value on early stop, -1 on a
 *          malformed / unsafe archive.
 */
int hl_tar_parse(const unsigned char *tar, size_t tar_len,
                 int (*cb)(const HlTarEntry *e, void *ctx), void *ctx);

/**
 * @brief Extract the archive into @p dest_dir (creating parent dirs, preserving
 *        the exec bit). TRUSTED: writes files directly, for hull's own install
 *        of a verified bundle. NOT for untrusted archives - apps extract via the
 *        hull.tar stdlib, which writes through the sandboxed fs capability.
 * @returns 0 on success, -1 on a malformed archive / write error.
 */
int hl_tar_extract(const unsigned char *tar, size_t tar_len,
                   const char *dest_dir);

/**
 * @brief Build a ustar archive from @p n entries into a malloc'd buffer.
 *
 * @c name (bare or nested-relative), @c data / @c size, @c mode (0 -> 0644),
 * @c is_dir per entry. Writes @p *out (caller frees) and @p *out_len.
 * @returns 0 on success, -1 on a bad name / overflow / OOM.
 */
int hl_tar_create(const HlTarEntry *entries, size_t n,
                  unsigned char **out, size_t *out_len);

#endif /* HL_CAP_TAR_H */
