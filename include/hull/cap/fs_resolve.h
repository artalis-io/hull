/*
 * fs_resolve.h - descriptor-relative, virtual-root path resolver.
 *
 * Part of the hull.fs design (docs/hull_fs_design.md). Replaces the
 * realpath->check->open TOCTOU (docs §1.4a) with a resolution that opens a path
 * under a base directory FILE DESCRIPTOR, following in-sandbox symlinks but
 * confining every resolution to that root (virtual-root, RESOLVE_IN_ROOT
 * semantics, docs §3/§5): absolute symlink targets re-root at the base, excess
 * ".." clamps at the base, nothing escapes. There is no resolve-then-open window
 * because every step is relative to a held fd (or one openat2 syscall).
 *
 * The resolver is BASE_DIR-anchored (the no-granular-grants case). The
 * compiled-entry authorization policy (SUBTREE/EXACT/CREATE) layers separately
 * (fs_policy.h); here the root is always base_dir, preserving the base_dir
 * confinement + kernel sandbox authorization model.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_FS_RESOLVE_H
#define HULL_CAP_FS_RESOLVE_H

#include <limits.h>    /* NAME_MAX */
#include <sys/types.h>

/*
 * Public, deterministic path-component-depth limit. A caller path with more than
 * this many resolvable components is rejected ("path_too_deep") BEFORE either
 * implementation runs, so `openat2` (which has no component-count limit) and the
 * manual held-fd-stack walk (which is bounded by open-fd count) accept/reject the
 * same caller paths - a documented common limit, not platform-dependent behavior.
 * The manual walk additionally enforces the same bound on the fully-resolved path
 * (post-symlink-expansion); `openat2`'s in-kernel resolution stays within the
 * kernel's own PATH_MAX/ELOOP bounds. Kept well under RLIMIT_NOFILE so the manual
 * walk never exhausts descriptors.
 */
#define HL_FS_MAX_DEPTH 256

typedef enum {
    HL_FS_OPEN_READ,  /* open an existing REGULAR-FILE leaf O_RDONLY (symlinks
                       * followed, contained). The leaf is opened O_NONBLOCK so a
                       * special-file target can never block the open, then type-gated
                       * to a regular file: a FIFO / socket / character-or-block device
                       * / directory is rejected "not_a_regular_file" (§5a). */
    HL_FS_OPEN_WRITE, /* mkdir-p parents (contained) + open leaf O_WRONLY|O_CREAT|O_TRUNC,
                       * O_NONBLOCK + regular-file type gate as for READ (a special-file
                       * or directory target -> "not_a_regular_file"). */
    HL_FS_OPEN_DIR,   /* open an existing DIRECTORY O_RDONLY|O_DIRECTORY (symlinks
                       * followed, contained); a non-directory target -> not_a_directory.
                       * Never creates. Used to resolve a SUBTREE grant anchor
                       * (fs_policy) and, later, stat/list. */
} HlFsOpenMode;

/*
 * Symlink policy for a resolution. Independent of the open mode.
 *   HL_FS_SYMLINK_FOLLOW - follow in-root symlinks, virtual-root contained
 *     (re-root / clamp at root_fd). The default and back-compatible behavior;
 *     used for a SUBTREE grant (descendants follow symlinks within the anchor).
 *   HL_FS_SYMLINK_REFUSE - ANY symlink component (intermediate or terminal) is
 *     REFUSED with "symlink_denied", never followed. Used for EXACT/CREATE/PATTERN
 *     grants so a symlink cannot alias a non-authorized target.
 */
typedef enum {
    HL_FS_SYMLINK_FOLLOW = 0,
    HL_FS_SYMLINK_REFUSE,
} HlFsSymlink;

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
 *   HL_FS_OPEN_READ  -> returns an O_RDONLY fd to the leaf (a file).
 *   HL_FS_OPEN_WRITE -> creates missing parent dirs (contained, mkdirat) and
 *                       returns an O_WRONLY|O_CREAT|O_TRUNC fd to the leaf (a file).
 *   HL_FS_OPEN_DIR   -> returns an O_RDONLY|O_DIRECTORY fd to an existing
 *                       DIRECTORY; a non-directory target -> "not_a_directory".
 *                       Never creates. (Used to resolve a SUBTREE grant anchor and,
 *                       later, stat/list.)
 *
 * `relpath` MUST be relative, MUST NOT contain ".." components, and MUST have at
 * most HL_FS_MAX_DEPTH components (the caller-path lexical pre-check; violations
 * return "invalid_path" / "path_too_deep"). It MUST NOT end in a trailing slash for
 * ANY mode - READ/WRITE are leaf-file modes, and although DIR opens a directory it
 * ALSO rejects a trailing slash in v1 (the lexical pre-check is mode-independent);
 * future stat/list work must not assume a trailing slash is accepted here. Symlink
 * targets ENCOUNTERED on disk MAY be absolute or contain "..": those are
 * virtual-rooted (re-root / clamp at `root_fd`), never an escape.
 *
 * Returns an open fd (caller closes) or -1 with *err set to a stable token:
 * "invalid_path", "path_too_deep", "not_found", "permission", "not_a_directory",
 * "is_a_directory", "not_a_regular_file", "symlink_loop", "symlink_denied",
 * "io_error". "not_a_regular_file" is the single token for a READ/WRITE leaf that
 * resolves to a FIFO, socket, character/block device, or directory (§5a intentional
 * tightening). "symlink_denied" only under HL_FS_SYMLINK_REFUSE; see
 * hl_fs_open_at_ex.
 */
int hl_fs_open_at(int root_fd, const char *relpath, HlFsOpenMode mode,
                  mode_t create_mode, const char **err);

/*
 * As hl_fs_open_at, plus an explicit symlink policy. hl_fs_open_at is exactly
 * hl_fs_open_at_ex(..., HL_FS_SYMLINK_FOLLOW, ...). Under HL_FS_SYMLINK_REFUSE any
 * symlink component (intermediate or terminal) fails "symlink_denied" and is never
 * followed - used to open an EXACT/CREATE/PATTERN residual under its grant anchor.
 */
int hl_fs_open_at_ex(int root_fd, const char *relpath, HlFsOpenMode mode,
                     HlFsSymlink symlink, mode_t create_mode, const char **err);

/*
 * Result of resolving a path to its PARENT directory fd + leaf NAME - for a
 * METADATA op (stat) that must lstat the terminal WITHOUT opening or following it.
 *
 *   parent_fd - a HELD directory fd (the caller closes it with close()), or -1 on
 *               failure. The terminal is named RELATIVE to this fd, so the caller's
 *               fstatat(parent_fd, leaf, AT_SYMLINK_NOFOLLOW) is descriptor-relative
 *               and race-resistant (no host path reconstructed).
 *   leaf      - the terminal component, NUL-terminated (guaranteed <= NAME_MAX
 *               bytes; never contains '/'), OR the empty string "" when `relpath`
 *               is the GRANT ROOT (".") and there is no parent-plus-leaf. On "" the
 *               caller fstat(parent_fd)s the anchor directory itself.
 */
typedef struct {
    int  parent_fd;
    char leaf[NAME_MAX + 1];
} HlFsParent;

/*
 * Resolve `relpath` under `root_fd` to (parent_fd, leaf) for a NO-FOLLOW metadata
 * op. Every component EXCEPT the terminal is walked with the `symlink` policy
 * (HL_FS_SYMLINK_FOLLOW = in-root contained; HL_FS_SYMLINK_REFUSE = "symlink_denied"
 * on ANY symlink), holding the terminal's PARENT dir fd. The TERMINAL is NEVER
 * opened or followed - the caller lstat()s it via *out. The residual "." (grant
 * root) yields parent_fd = a fresh O_CLOEXEC dup of root_fd and leaf = "".
 *
 * Same lexical contract + stable error tokens as hl_fs_open_at (relative, no "..",
 * <= HL_FS_MAX_DEPTH components, no trailing slash). On success returns 0 with *out
 * populated (caller closes out->parent_fd); on failure returns -1 with *err set and
 * out->parent_fd == -1 (nothing to close).
 */
int hl_fs_resolve_parent(int root_fd, const char *relpath, HlFsSymlink symlink,
                         HlFsParent *out, const char **err);

#endif /* HULL_CAP_FS_RESOLVE_H */
