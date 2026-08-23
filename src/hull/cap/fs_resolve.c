/*
 * fs_resolve.c — descriptor-relative, virtual-root path resolver.
 *
 * See include/hull/cap/fs_resolve.h and docs/hull_fs_design.md §3/§5.
 *
 * Two implementations of the SAME virtual-root contract:
 *   - Linux >= 5.6: one openat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS) syscall.
 *   - Everywhere else (macOS, older Linux, cosmo): a manual held-fd-stack walk
 *     that reproduces RESOLVE_IN_ROOT exactly. Both are race-free (every step is
 *     relative to a held fd / a single kernel-confined call); never realpath.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/fs_resolve.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#ifndef HL_FS_PATH_MAX
#define HL_FS_PATH_MAX 4096
#endif
#define HL_FS_SYMLINK_MAX 40   /* matches the kernel ELOOP budget */
/* HL_FS_MAX_DEPTH is the public component-depth limit (fs_resolve.h); it also
 * bounds the manual walk's directory-fd stack. */

static void map_errno(int e, const char **err)
{
    switch (e) {
    case ENOENT:  *err = "not_found";       break;
    case EACCES:
    case EPERM:   *err = "permission";      break;
    case ENOTDIR: *err = "not_a_directory"; break;
    case EISDIR:  *err = "is_a_directory";  break;
    case ELOOP:   *err = "symlink_loop";    break;
    case ENAMETOOLONG: *err = "invalid_path"; break;
    default:      *err = "io_error";        break;
    }
}

/* ── caller-path lexical pre-check (docs §3/§5) ────────────────────────────────
 * Relative, no "..", and no TRAILING slash. The trailing-slash rejection is a
 * cross-platform PARITY guard: a directory-shaped path like "file/" must not open
 * a regular file. `openat2` rejects it (ENOTDIR for a file leaf); the manual walk,
 * left to itself, would strip the trailing slash and open "file" as a leaf (and
 * under WRITE could even create/truncate a regular file at a directory-shaped
 * path). Rejecting it here — BEFORE either implementation runs — makes both agree.
 * READ/WRITE are leaf-file modes, so a trailing slash is never valid for them;
 * directory modes (stat/list, a later checkpoint) will pre-check differently. */
static int caller_path_ok(const char *p)
{
    if (!p || p[0] == '\0' || p[0] == '/') return 0;   /* empty or absolute */
    size_t len = strlen(p);
    if (p[len - 1] == '/') return 0;                   /* trailing slash (dir-shaped) */
    const char *c = p;
    while (*c) {
        if (c[0] == '.' && c[1] == '.' && (c[2] == '/' || c[2] == '\0'))
            return 0;                                   /* ".." component */
        const char *s = strchr(c, '/');
        if (!s) break;
        c = s + 1;
    }
    return 1;
}

/* Count the resolvable components of a caller path (non-empty, non-"." segments;
 * "." adds no depth, "..") is already rejected). Used to enforce the public
 * HL_FS_MAX_DEPTH bound BEFORE both implementations so `openat2` (which has no
 * component-count limit of its own) and the manual walk (whose held-fd stack is
 * bounded) accept/reject the same caller paths. */
static size_t caller_component_count(const char *p)
{
    size_t n = 0;
    const char *c = p;
    while (*c) {
        while (*c == '/') c++;
        if (*c == '\0') break;
        const char *s = strchr(c, '/');
        size_t l = s ? (size_t)(s - c) : strlen(c);
        if (!(l == 1 && c[0] == '.')) n++;   /* skip "." segments (zero depth) */
        if (!s) break;
        c = s;
    }
    return n;
}

/* Test/diagnostic hook: HL_FS_FORCE_MANUAL forces the portable manual walk even on
 * Linux, so the parity tests can exercise both implementations on one platform.
 * NOT a security downgrade — both implement the same virtual-root contract. Read
 * per call (negligible next to the resolution syscalls) so a test can toggle it at
 * runtime. Only meaningful where the openat2 fast path exists. */
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
static int force_manual(void)
{
    return getenv("HL_FS_FORCE_MANUAL") != NULL;
}
#endif

/* ── Linux openat2 fast-path ──────────────────────────────────────────────── */
/* Cosmopolitan (fat APE) uses the portable manual walk, not a Linux raw syscall. */
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
#include <sys/syscall.h>
#include <stdint.h>

/* Kernel uapi may predate openat2; define the ABI locally if absent. */
#ifndef RESOLVE_IN_ROOT
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_IN_ROOT       0x10
struct open_how { uint64_t flags; uint64_t mode; uint64_t resolve; };
#endif
#ifndef __NR_openat2
#define __NR_openat2 437
#endif

/* Returns fd, or -1 with *err set, or -2 meaning "openat2 unavailable, fall
 * back to the manual walk". */
static int try_openat2(int root_fd, const char *relpath, int flags,
                       mode_t mode, const char **err)
{
    struct open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)flags;
    how.mode = (flags & O_CREAT) ? (uint64_t)mode : 0;
    how.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS;

    long r = syscall(__NR_openat2, root_fd, relpath, &how, sizeof(how));
    if (r >= 0) return (int)r;
    if (errno == ENOSYS || errno == EPERM /* seccomp may block it */)
        return -2;
    map_errno(errno, err);
    return -1;
}
#endif /* __linux__ */

/* ── portable manual walk (reproduces RESOLVE_IN_ROOT) ────────────────────── */

/* Prepend `link` (a symlink target) in front of `rest`, into `out`. An absolute
 * link resets the caller's dir stack to the root (signalled via *reset). */
static int splice_link(const char *link, const char *rest,
                       char *out, size_t out_sz, int *reset)
{
    *reset = (link[0] == '/');
    int n;
    if (rest && rest[0] != '\0')
        n = snprintf(out, out_sz, "%s/%s", link, rest);
    else
        n = snprintf(out, out_sz, "%s", link);
    return (n < 0 || (size_t)n >= out_sz) ? -1 : 0;
}

static int resolve_manual(int root_fd, const char *relpath, HlFsOpenMode mode,
                          mode_t cmode, const char **err)
{
    int stack[HL_FS_MAX_DEPTH];
    int depth = 0;
    stack[depth++] = dup(root_fd);            /* stack[0] = root; never popped */
    if (stack[0] < 0) { map_errno(errno, err); return -1; }

    char rem[HL_FS_PATH_MAX];
    if (strlen(relpath) >= sizeof(rem)) { *err = "invalid_path"; goto fail; }
    strcpy(rem, relpath);

    int symlinks = 0;
    int result_fd = -1;
    char *cur = rem;

    for (;;) {
        while (*cur == '/') cur++;
        if (*cur == '\0') {
            /* Consumed everything without a terminal leaf (path was "." etc.).
             * READ opens the current directory; WRITE has no leaf to create. */
            if (mode == HL_FS_OPEN_READ) {
                result_fd = openat(stack[depth - 1], ".", O_RDONLY | O_CLOEXEC);
                if (result_fd < 0) { map_errno(errno, err); goto fail; }
                goto done;
            }
            *err = "invalid_path";
            goto fail;
        }

        /* Extract the next component. */
        char comp[NAME_MAX + 1];
        char *slash = strchr(cur, '/');
        size_t clen = slash ? (size_t)(slash - cur) : strlen(cur);
        if (clen > NAME_MAX) { *err = "invalid_path"; goto fail; }
        memcpy(comp, cur, clen);
        comp[clen] = '\0';
        char *rest = slash ? slash + 1 : cur + clen;   /* what remains after comp */

        if (strcmp(comp, ".") == 0) { cur = rest; continue; }
        if (strcmp(comp, "..") == 0) {                 /* clamp at root */
            if (depth > 1) close(stack[--depth]);
            cur = rest;
            continue;
        }

        /* Is this the terminal component? (nothing but slashes/empty after it) */
        const char *tail = rest;
        while (*tail == '/') tail++;
        int last = (*tail == '\0');

        if (!last) {
            /* Interior: descend into a directory, following a symlink if present.
             * Open O_RDONLY|O_NOFOLLOW *without* O_DIRECTORY, then verify dir-ness
             * with fstat on the held fd. Using O_DIRECTORY|O_NOFOLLOW here is not
             * portable: on a SYMLINK component Linux returns ELOOP (detected below)
             * but macOS returns ENOTDIR, which would misclassify an INTERIOR symlink
             * as "not_a_directory" instead of following it. Dropping O_DIRECTORY
             * makes a symlink fail ELOOP uniformly, and the fstat is race-safe (it
             * inspects the already-opened inode). */
            int fd = openat(stack[depth - 1], comp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) != 0) { close(fd); map_errno(errno, err); goto fail; }
                if (!S_ISDIR(st.st_mode)) { close(fd); *err = "not_a_directory"; goto fail; }
                if (depth >= HL_FS_MAX_DEPTH) { close(fd); *err = "path_too_deep"; goto fail; }
                stack[depth++] = fd;
                cur = rest;
                continue;
            }
            if (errno == ELOOP || errno == EMLINK) goto symlink;
            if (errno == ENOENT && mode == HL_FS_OPEN_WRITE) {
                if (mkdirat(stack[depth - 1], comp, 0755) < 0 && errno != EEXIST) {
                    map_errno(errno, err); goto fail;
                }
                fd = openat(stack[depth - 1], comp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0) { map_errno(errno, err); goto fail; }
                struct stat st;
                if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    close(fd); *err = "not_a_directory"; goto fail;
                }
                if (depth >= HL_FS_MAX_DEPTH) { close(fd); *err = "path_too_deep"; goto fail; }
                stack[depth++] = fd;
                cur = rest;
                continue;
            }
            map_errno(errno, err);
            goto fail;
        } else {
            /* terminal component */
            int flags = (mode == HL_FS_OPEN_READ)
                          ? (O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
                          : (O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC);
            int fd = openat(stack[depth - 1], comp, flags, cmode);
            if (fd >= 0) { result_fd = fd; goto done; }
            if (errno == ELOOP || errno == EMLINK) goto symlink;
            map_errno(errno, err);
            goto fail;
        }

    symlink:
        if (++symlinks > HL_FS_SYMLINK_MAX) { *err = "symlink_loop"; goto fail; }
        {
            char link[HL_FS_PATH_MAX];
            ssize_t ln = readlinkat(stack[depth - 1], comp, link, sizeof(link) - 1);
            if (ln < 0) { map_errno(errno, err); goto fail; }
            link[ln] = '\0';
            char next[HL_FS_PATH_MAX];
            int reset = 0;
            if (splice_link(link, rest, next, sizeof(next), &reset) != 0) {
                *err = "invalid_path"; goto fail;
            }
            if (reset)                       /* absolute target: re-root */
                while (depth > 1) close(stack[--depth]);
            memcpy(rem, next, strlen(next) + 1);
            cur = rem;
            continue;
        }
    }

done:
    for (int i = 0; i < depth; i++) close(stack[i]);
    return result_fd;
fail:
    for (int i = 0; i < depth; i++) close(stack[i]);
    return -1;
}

/* ── public entry points ──────────────────────────────────────────────────── */

int hl_fs_open_base(const char *base_dir, const char **err)
{
    if (!base_dir || base_dir[0] == '\0') { if (err) *err = "invalid_args"; return -1; }
    int fd = open(base_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { if (err) map_errno(errno, err); return -1; }
    return fd;
}

int hl_fs_open_at(int root_fd, const char *relpath, HlFsOpenMode mode,
                  mode_t create_mode, const char **err)
{
    const char *e = "io_error";
    if (root_fd < 0 || !relpath) { if (err) *err = "invalid_args"; return -1; }
    if (!caller_path_ok(relpath)) { if (err) *err = "invalid_path"; return -1; }
    /* Public depth bound, enforced BEFORE either implementation so both accept /
     * reject the same caller paths (openat2 has no component-count limit; the
     * manual walk's held-fd stack does). See HL_FS_MAX_DEPTH in fs_resolve.h. */
    if (caller_component_count(relpath) > HL_FS_MAX_DEPTH) {
        if (err) *err = "path_too_deep";
        return -1;
    }

#if defined(__linux__) && !defined(__COSMOPOLITAN__)
    /* openat2 does not create intermediate dirs, so WRITE always uses the manual
     * walk (which does the contained mkdir-p). READ takes the one-call fast path,
     * unless HL_FS_FORCE_MANUAL is set (parity testing). */
    if (mode == HL_FS_OPEN_READ && !force_manual()) {
        int fd = try_openat2(root_fd, relpath, O_RDONLY | O_CLOEXEC, 0, &e);
        if (fd >= 0) return fd;
        if (fd == -1) { if (err) *err = e; return -1; }
        /* fd == -2: openat2 unavailable -> manual walk */
    }
#endif

    int fd = resolve_manual(root_fd, relpath, mode, create_mode, &e);
    if (fd < 0 && err) *err = e;
    return fd;
}
