/*
 * fs_resolve.h — descriptor-relative, virtual-root path resolver.
 *
 * Checkpoint 1 of the hull.fs design (docs/hull_fs_design.md). Replaces the
 * realpath->check->open TOCTOU (docs §1.4a) with a resolution that opens a path
 * under a base directory FILE DESCRIPTOR, following in-sandbox symlinks but
 * confining every resolution to that root (virtual-root, RESOLVE_IN_ROOT
 * semantics, docs §3/§5): absolute symlink targets re-root at the base, excess
 * ".." clamps at the base, nothing escapes. There is no resolve-then-open window
 * because every step is relative to a held fd (or one openat2 syscall).
 *
 * This is checkpoint 1: it is BASE_DIR-anchored (the no-granular-grants case).
 * The compiled-entry authorization policy (SUBTREE/EXACT/CREATE) lands at
 * checkpoint 3; here the root is always base_dir, preserving today's
 * authorization model (base_dir confinement + kernel sandbox).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_FS_RESOLVE_H
#define HULL_CAP_FS_RESOLVE_H

#include <sys/types.h>

typedef enum {
    HL_FS_OPEN_READ,  /* open an existing leaf O_RDONLY (symlinks followed, contained) */
    HL_FS_OPEN_WRITE, /* mkdir-p parents (contained) + open leaf O_WRONLY|O_CREAT|O_TRUNC */
} HlFsOpenMode;

/*
 * Open `base_dir` as a directory fd suitable for hl_fs_open_at(). Returns the fd
 * (caller closes with close()) or -1 with *err set. base_dir is the trusted app
 * root; it is opened O_DIRECTORY|O_CLOEXEC (a symlinked app root is followed once
 * here - it is the ceiling, not a traversal step).
 */
int hl_fs_open_base(const char *base_dir, const char **err);

/*
 * Resolve+open `relpath` under `root_fd` with virtual-root semantics.
 *
 *   HL_FS_OPEN_READ  -> returns an O_RDONLY fd to the leaf.
 *   HL_FS_OPEN_WRITE -> creates missing parent dirs (contained, mkdirat) and
 *                       returns an O_WRONLY|O_CREAT|O_TRUNC fd to the leaf.
 *
 * `relpath` MUST be relative and MUST NOT contain ".." components (the caller
 * performs that lexical pre-check; a violation returns "invalid_path"). Symlink
 * targets ENCOUNTERED on disk MAY be absolute or contain "..": those are
 * virtual-rooted (re-root / clamp at `root_fd`), never an escape.
 *
 * Returns an open fd (caller closes) or -1 with *err set to a stable token:
 * "invalid_path", "not_found", "permission", "not_a_directory",
 * "is_a_directory", "symlink_loop", "io_error".
 */
int hl_fs_open_at(int root_fd, const char *relpath, HlFsOpenMode mode,
                  mode_t create_mode, const char **err);

#endif /* HULL_CAP_FS_RESOLVE_H */
