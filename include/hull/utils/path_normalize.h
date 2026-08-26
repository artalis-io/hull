/*
 * path_normalize.h - collapse `.` and `..` segments in a path, in place.
 *
 * Originally lived in src/hull/runtime/lua/mod_fs.c. Lifted out so the
 * JS module loader can use the same logic (the loader previously
 * rejected `..` *anywhere* in the resolved string, which broke any
 * cross-directory relative import like `./../models/user.js` even
 * though the normalized form is well-behaved). One implementation,
 * one set of failure modes.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_PATH_NORMALIZE_H
#define HL_PATH_NORMALIZE_H

/*
 * Normalize `path` in place by collapsing `.` and `..` segments.
 * Examples:
 *   "routes/../models/user"  → "models/user"
 *   "a/./b"                  → "a/b"
 *   "/foo/../bar"            → "/bar"
 *
 * Returns 0 on success, -1 if `..` would escape past the root (the
 * resulting path would have started above the original starting
 * directory). Capacity for up to 128 path segments; longer paths
 * also return -1.
 *
 * Absolute paths (leading `/`) stay absolute. Trailing slashes are
 * collapsed.
 */
int hl_path_normalize(char *path);

#endif /* HL_PATH_NORMALIZE_H */
