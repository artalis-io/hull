/*
 * hull/cap/exec_child.h - a declared command's child manifest (`exec.<name>.
 * manifest`, docs/design/exec_broker.md §3).
 *
 * `manifest` is a strict subset of the app manifest: today only `fs`, with
 * the app's own meaning - each entry is a path under the app directory, or
 * under "@state/", parsed by the same hl_fs_grant_parse. What a command may
 * NOT be granted, though valid for an app, is refused here:
 *
 *   - the whole app or state directory ("." / "@state"): the kernel grant
 *     would include its .hull, which no app code reaches (audit 37), and
 *     Landlock can only add access, never take a subdirectory back out;
 *   - a pattern ("logs/<star>.log"): Landlock and Seatbelt grant files and
 *     directories, not patterns.
 *
 * An app's own kernel grant may be wider than its fs grant (a file write
 * grant is its parent directory, a pattern its literal directory) because
 * the capability layer narrows it again in process. A command has no
 * capability layer: its kernel grant IS the boundary, so the broker grants
 * exactly what the entry names (hl_exec_child_grant_resolve), never more.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_EXEC_CHILD_H
#define HL_CAP_EXEC_CHILD_H

#include <stddef.h>

/* The fields a child manifest takes (one row each, NULL-terminated). */
extern const char *const HL_EXEC_CHILD_MANIFEST_KEYS[];

/* Every other top-level app manifest field, in both spellings: valid for an
 * app, refused for a command (named as such, not as an unknown key). */
extern const char *const HL_EXEC_APP_ONLY_KEYS[];

/* Command-entry fields (path, pin, argv, host, ...): refused inside a child
 * manifest with a pointer to where they belong. */
extern const char *const HL_EXEC_ENTRY_ONLY_KEYS[];

typedef struct HlExecBuf HlExecBuf;

/* A key of exec.<cmd>.manifest: 0 when the child manifest takes it;
 * otherwise -1 after hl_exec_b_fail with the reason - an entry field
 * (path, sha256, argv, host, ...) "belongs on the command entry", an app
 * field a command cannot have "is an app capability a command cannot be
 * given", anything else is an unknown key. Both extractors call it, so the
 * messages match. */
int hl_exec_child_key(HlExecBuf *b, const char *cmd, const char *key);

/* One manifest.fs.<read|write>[i] entry (1-based @p idx): 0, or -1 after
 * hl_exec_b_fail naming the command, field and entry. */
int hl_exec_child_grant_entry(HlExecBuf *b, const char *cmd, const char *list,
                              unsigned idx, const char *raw, size_t n);

/* 0 when @p raw (n bytes) is an app fs grant a command may take; -1 with
 * the reason in @p why. Pure string check (no filesystem access). */
int hl_exec_child_grant_check(const char *raw, size_t n, char *why, size_t why_size);

#include <limits.h>

/* What a child grant is resolved against. @p protect: database files (the
 * app's default; relative = under @p app_dir) a WRITE grant may not reach,
 * with their -wal / -shm / -journal. */
typedef struct HlExecChildRoots {
    const char        *app_dir;
    const char        *state_dir;     /* NULL = none declared */
    const char *const *protect;
    int                n_protect;
} HlExecChildRoots;

/* One opened grant: the descriptor of the exact file or directory (Linux
 * O_PATH; the Landlock rule is made from it, never from the path again) and
 * its real path (macOS: from F_GETPATH on that descriptor). */
typedef struct HlExecChildGrant {
    int  fd;
    char path[PATH_MAX];
} HlExecChildGrant;

/* Open one child grant for the broker, at policy load, from the root's
 * descriptor component by component (fstatat NOFOLLOW, openat NOFOLLOW, the
 * opened object compared with the one checked): no symbolic link, no
 * component that is either root's .hull (by inode), not a directory that is
 * or holds either root, and for @p write not a protected database. "/" or
 * the home as the app root is refused (a built tool run from there). A
 * missing path is refused, except a trailing-'/' directory, which is made
 * (mkdirat) and checked again. 0 (caller closes out->fd) / -1 with @p why. */
int hl_exec_child_grant_open(const HlExecChildRoots *r, const char *raw, int write,
                             HlExecChildGrant *out, char *why, size_t why_size);

/* hl_exec_child_grant_open for read, the path only (tests, inspection).
 * Resolve one child grant for the broker, at policy load: the app's root
 * (@p app_dir) or its state root (@p state_dir, NULL = none declared), then
 * each component without following a symbolic link. The path must exist,
 * except a grant ending in '/' (a directory), which is created as the app's
 * sandbox creates it. Writes the real path to @p out. 0 / -1 with @p why. */
int hl_exec_child_grant_resolve(const char *app_dir, const char *state_dir,
                                const char *raw, char *out, size_t out_size,
                                char *why, size_t why_size);

#endif /* HL_CAP_EXEC_CHILD_H */
