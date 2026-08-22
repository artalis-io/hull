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
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#ifndef HL_FS_PATH_MAX
#define HL_FS_PATH_MAX 4096
#endif
#define HL_FS_SYMLINK_MAX 40   /* matches the kernel ELOOP budget */
#define HL_FS_MAX_DEPTH   256  /* directory-fd stack bound */

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

/* ── caller-path lexical pre-check (docs §3/§5): relative, no ".." ─────────── */
static int caller_path_ok(const char *p)
{
    if (!p || p[0] == '\0' || p[0] == '/') return 0;   /* empty or absolute */
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
            /* interior: descend into a directory, following a symlink if present */
            int fd = openat(stack[depth - 1], comp,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) {
                if (depth >= HL_FS_MAX_DEPTH) { close(fd); *err = "invalid_path"; goto fail; }
                stack[depth++] = fd;
                cur = rest;
                continue;
            }
            if (errno == ELOOP || errno == EMLINK) goto symlink;
            if (errno == ENOENT && mode == HL_FS_OPEN_WRITE) {
                if (mkdirat(stack[depth - 1], comp, 0755) < 0 && errno != EEXIST) {
                    map_errno(errno, err); goto fail;
                }
                fd = openat(stack[depth - 1], comp,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0) { map_errno(errno, err); goto fail; }
                if (depth >= HL_FS_MAX_DEPTH) { close(fd); *err = "invalid_path"; goto fail; }
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

#if defined(__linux__) && !defined(__COSMOPOLITAN__)
    /* openat2 does not create intermediate dirs, so WRITE always uses the manual
     * walk (which does the contained mkdir-p). READ takes the one-call fast path. */
    if (mode == HL_FS_OPEN_READ) {
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
