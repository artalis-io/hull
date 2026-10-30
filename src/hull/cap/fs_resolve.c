/*
 * fs_resolve.c - descriptor-relative, virtual-root path resolver.
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
#include "hull/shared/host.h"   /* hl_host_is_windows */
#include "hull/shared/work_charge.h"   /* HL_WORK_FS_* resolver units */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <strings.h>     /* strncasecmp */
#include <pthread.h>
#include <stdatomic.h>

#ifndef HL_FS_PATH_MAX
#define HL_FS_PATH_MAX 4096
#endif
#define HL_FS_SYMLINK_MAX 40   /* matches the kernel ELOOP budget */
#define HL_FS_RECLASSIFY_MAX 64 /* bound interior classify<->open TOCTOU retries */
/* HL_FS_MAX_DEPTH is the public component-depth limit (fs_resolve.h); it also
 * bounds the manual walk's directory-fd stack. */

/* ── charge scope (audit 38; fs_resolve.h) ───────────────────────────────── */

static _Thread_local HlFsChargeScope tl_charge;

HlFsChargeScope hl_fs_charge_begin(HlFsChargeFn fn, void *ud)
{
    HlFsChargeScope prev = tl_charge;
    tl_charge.fn = fn;
    tl_charge.ud = fn ? ud : NULL;
    tl_charge.tripped = 0;
    return prev;
}

int hl_fs_charge_end(HlFsChargeScope prev)
{
    int tripped = tl_charge.tripped;
    tl_charge = prev;
    return tripped;
}

int hl_fs_charge(uint64_t units)
{
    if (tl_charge.tripped) return 1;
    if (!tl_charge.fn || units == 0) return 0;
    if (tl_charge.fn(tl_charge.ud, units) != 0) {
        tl_charge.tripped = 1;
        return 1;
    }
    return 0;
}

#define HL_FS_BUDGET_ERR "budget_exhausted"

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

/* An open() errno from a terminal READ/WRITE leaf that means the target is a
 * non-regular special file (or, under WRITE, a directory) rather than a transient
 * failure: a socket special file fails ENXIO (Linux) / EOPNOTSUPP (macOS), a FIFO
 * opened for WRITE with no reader fails ENXIO, and a directory opened O_WRONLY
 * fails EISDIR. These collapse to the single stable "not_a_regular_file" token so
 * every special-file / directory rejection reads identically, whether it surfaces
 * at open() (this helper) or at the post-open type gate (finalize_regular_leaf). */
static int leaf_type_errno(int e)
{
    if (e == EISDIR || e == ENXIO) return 1;
#ifdef EOPNOTSUPP
    if (e == EOPNOTSUPP) return 1;
#endif
#if defined(ENOTSUP) && (!defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP)
    if (e == ENOTSUP) return 1;
#endif
    return 0;
}

/* Type gate for a terminal READ/WRITE LEAF: accept ONLY a regular file. A FIFO,
 * socket, character/block device, or directory is rejected with the single stable
 * "not_a_regular_file" token. The leaf is opened O_NONBLOCK (so a special-file leaf
 * can never BLOCK the open - a FIFO/device open returns immediately instead of
 * waiting on a peer); O_NONBLOCK is then cleared on the accepted regular fd so the
 * returned descriptor has ordinary blocking read/write semantics. On rejection or
 * fstat failure the fd is closed and -1 returned with *err set. INTENTIONAL
 * tightening: an authorized special-file leaf is no longer readable/writable/
 * mmap-able (docs/hull_fs_design.md §5a). Applied to READ and WRITE only; DIR keeps
 * its own S_ISDIR contract (a directory never blocks on open). */
static int finalize_regular_leaf(int fd, const char **err)
{
    struct stat st;
    if (fstat(fd, &st) != 0) { map_errno(errno, err); close(fd); return -1; }
    if (!S_ISREG(st.st_mode)) { *err = "not_a_regular_file"; close(fd); return -1; }
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    return fd;
}

/* ── caller-path lexical pre-check (docs §3/§5) ────────────────────────────────
 * Relative, no "..", and no TRAILING slash. The trailing-slash rejection is a
 * cross-platform PARITY guard: a directory-shaped path like "file/" must not open
 * a regular file. `openat2` rejects it (ENOTDIR for a file leaf); the manual walk,
 * left to itself, would strip the trailing slash and open "file" as a leaf (and
 * under WRITE could even create/truncate a regular file at a directory-shaped
 * path). Rejecting it here - BEFORE either implementation runs - makes both agree.
 * READ/WRITE are leaf-file modes, so a trailing slash is never valid for them;
 * directory modes (stat/list, a later checkpoint) will pre-check differently. */
static int caller_path_ok(const char *p)
{
    if (!p || p[0] == '\0' || p[0] == '/') return 0;   /* empty or absolute */
    /* Windows separators and drive letters (see split_caller, fs_policy.c). */
    if (hl_host_is_windows() && strpbrk(p, "\\:")) return 0;
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

/* ── reserved app-tree directory (audit 37) ───────────────────────────────── *
 *
 * <app>/.hull holds the `hull dev --agent` sidecars (dev.json, last_error.json,
 * discovery.json) and `hull build`'s cosmo debug sidecars; an agent reads them as
 * Hull's own output, so app code never touches it. The policy refuses a caller path
 * naming it lexically (fs_policy.c); this walk refuses STEPPING into it from an app
 * root by any route a lexical check cannot see - an in-root symlink (`x -> .hull`),
 * another letter case on a case-insensitive filesystem, or an HFS+ spelling with
 * ignorable code points (compared by inode when it is a directory). The roots are
 * registered by hl_fs_reserve_root (every policy compile). */
#define HL_FS_RESERVED_ROOTS_MAX 16

static pthread_mutex_t g_res_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { dev_t dev; ino_t ino; int fd; } g_res[HL_FS_RESERVED_ROOTS_MAX];
static atomic_int g_res_n;
static int g_res_evict;   /* next slot to reuse once full (under g_res_mu) */

int hl_fs_reserved_name(const char *name, size_t len)
{
    return name && len == 5 && strncasecmp(name, ".hull", 5) == 0;
}

void hl_fs_reserve_root(int base_fd)
{
    struct stat st;
    if (base_fd < 0 || fstat(base_fd, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    pthread_mutex_lock(&g_res_mu);
    int n = atomic_load(&g_res_n);
    int seen = 0;
    for (int i = 0; i < n; i++)
        if (g_res[i].dev == st.st_dev && g_res[i].ino == st.st_ino) { seen = 1; break; }
    if (!seen) {
        int slot = n < HL_FS_RESERVED_ROOTS_MAX ? n : -1;
        if (slot < 0) {
            /* Full: reuse the slot of a root that has been removed. Only a
             * process that compiles policies for many directories - a test
             * binary - gets here; a server compiles one. */
            for (int i = 0; i < n; i++) {
                struct stat rs;
                if (fstat(g_res[i].fd, &rs) == 0 && rs.st_nlink == 0) { slot = i; break; }
            }
            /* None known removed (some filesystems keep st_nlink): the oldest
             * goes, so the root compiled last is always registered. */
            if (slot < 0) {
                slot = g_res_evict;
                g_res_evict = (g_res_evict + 1) % HL_FS_RESERVED_ROOTS_MAX;
            }
        }
        int fd = slot >= 0 ? fcntl(base_fd, F_DUPFD_CLOEXEC, 0) : -1;
        if (fd >= 0) {
            int old = slot < n ? g_res[slot].fd : -1;
            g_res[slot].dev = st.st_dev;
            g_res[slot].ino = st.st_ino;
            g_res[slot].fd = fd;
            if (old >= 0) close(old);
            if (slot == n) atomic_store(&g_res_n, n + 1);   /* publish after the write */
        }
    }
    pthread_mutex_unlock(&g_res_mu);
}

/* Is @p dirfd one of the registered app roots? */
static int is_reserved_root(int dirfd)
{
    int n = atomic_load(&g_res_n);
    if (n == 0) return 0;
    struct stat st;
    if (fstat(dirfd, &st) != 0) return 0;
    for (int i = 0; i < n; i++)
        if (g_res[i].dev == st.st_dev && g_res[i].ino == st.st_ino) return 1;
    return 0;
}

int hl_fs_is_reserved_root(int dirfd) { return is_reserved_root(dirfd); }

/* Would stepping from @p parent to its entry @p comp (whose stat is @p st, when
 * known) reach the reserved directory? 1 = refuse. */
static int reserved_step(int parent, const char *comp, const struct stat *st)
{
    if (atomic_load(&g_res_n) == 0) return 0;
    int root = -1;   /* computed lazily: one fstat */
    if (hl_fs_reserved_name(comp, strlen(comp))) {
        root = is_reserved_root(parent);
        if (root) return 1;
    }
    if (st && S_ISDIR(st->st_mode)) {
        if (root < 0) root = is_reserved_root(parent);
        if (!root) return 0;
        struct stat hs;
        if (fstatat(parent, ".hull", &hs, AT_SYMLINK_NOFOLLOW) == 0 &&
            hs.st_dev == st->st_dev && hs.st_ino == st->st_ino)
            return 1;
    }
    return 0;
}

#if defined(__linux__) && !defined(__COSMOPOLITAN__)
/* Does any registered root currently hold a .hull entry? Then openat2's one-call
 * resolution, which reports nothing about the directories it passed, is not used. */
static int reserved_dir_present(void)
{
    int n = atomic_load(&g_res_n);
    for (int i = 0; i < n; i++) {
        struct stat hs;
        if (fstatat(g_res[i].fd, ".hull", &hs, AT_SYMLINK_NOFOLLOW) == 0) return 1;
        /* another spelling of the name on a case-insensitive filesystem is found by
         * the same lookup there; on a case-sensitive one it is another directory. */
    }
    return 0;
}
#endif

/* Test/diagnostic hook: HL_FS_FORCE_MANUAL forces the portable manual walk even on
 * Linux, so the parity tests can exercise both implementations on one platform.
 * NOT a security downgrade - both implement the same virtual-root contract. Read
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
#ifndef RESOLVE_NO_SYMLINKS
#define RESOLVE_NO_SYMLINKS   0x04
#endif
#ifndef __NR_openat2
#define __NR_openat2 437
#endif

#define OPENAT2_EAGAIN_TRIES 8

/* What the manual walk would charge for @p relpath (no symlinks). */
static uint64_t openat2_units(const char *relpath, HlFsOpenMode mode)
{
    uint64_t comps = caller_component_count(relpath);
    uint64_t u = comps * (HL_WORK_FS_NAME_UNITS + HL_WORK_FS_LOOKUP_UNITS);
    if (comps > 1) u += (comps - 1) * HL_WORK_FS_STEP_UNITS;
    u += mode == HL_FS_OPEN_DIR ? HL_WORK_FS_STEP_UNITS : HL_WORK_FS_LEAF_UNITS;
    return u;
}

/* Returns fd, or -1 with *err set, or -2 meaning "openat2 unavailable (or kept
 * losing to concurrent renames), fall back to the manual walk". Under HL_FS_SYMLINK_REFUSE the kernel refuses every
 * symlink (RESOLVE_NO_SYMLINKS) and an ELOOP is mapped to "symlink_denied". */
static int try_openat2(int root_fd, const char *relpath, int flags,
                       HlFsSymlink sympol, mode_t mode, const char **err)
{
    struct open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)flags;
    how.mode = (flags & O_CREAT) ? (uint64_t)mode : 0;
    how.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS
                | (sympol == HL_FS_SYMLINK_REFUSE ? RESOLVE_NO_SYMLINKS : 0);

    /* A scoped lookup that crosses ".." fails EAGAIN when a rename or mount
     * happens ANYWHERE on the system during the walk (the kernel cannot prove
     * the ".." stayed inside the root, so it gives up rather than guess). That
     * is a transient, not an answer about this path: retry, and if renames keep
     * winning, let the manual walk - which has no such failure - resolve it. */
    long r;
    int tries = 0;
    do {
        r = syscall(__NR_openat2, root_fd, relpath, &how, sizeof(how));
    } while (r < 0 && errno == EAGAIN && ++tries < OPENAT2_EAGAIN_TRIES);
    if (r >= 0) return (int)r;
    if (errno == EAGAIN) return -2;
    if (errno == ENOSYS || errno == EPERM /* seccomp may block it */)
        return -2;
    if (sympol == HL_FS_SYMLINK_REFUSE && errno == ELOOP) { *err = "symlink_denied"; return -1; }
    /* A READ leaf (no O_DIRECTORY) that is a socket special file fails ENXIO here;
     * map it to the same token the post-open type gate uses. O_DIRECTORY opens (DIR
     * mode) keep their ENOTDIR-> not_a_directory mapping. */
    if (!(flags & O_DIRECTORY) && leaf_type_errno(errno)) {
        *err = "not_a_regular_file"; return -1;
    }
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
                          HlFsSymlink sympol, mode_t cmode,
                          char *leaf, size_t leaf_sz, const char **err)
{
    int stack[HL_FS_MAX_DEPTH];
    int depth = 0;
    /* CLOEXEC: a plain dup() clears it, and a tool spawned while this walk
     * held the copy inherited a descriptor on the app root. */
    stack[depth++] = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);   /* stack[0] = root; never popped */
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
            /* Consumed everything without a terminal leaf: the path IS a directory
             * (a grant root reached via residual ".", or a path that clamped to root
             * via ".."). DIR opens it. READ resolves to a DIRECTORY, which is not a
             * regular file, so it is type-gated exactly like a special-file leaf and
             * rejected "not_a_regular_file" (the O_NONBLOCK is harmless on a dir but
             * kept for one uniform finalize path). WRITE has no leaf to create. */
            if (mode == HL_FS_OPEN_READ || mode == HL_FS_OPEN_DIR) {
                int of = O_RDONLY | O_CLOEXEC
                         | (mode == HL_FS_OPEN_DIR ? O_DIRECTORY : O_NONBLOCK);
                if (hl_fs_charge(HL_WORK_FS_STEP_UNITS)) {
                    *err = HL_FS_BUDGET_ERR; goto fail;
                }
                result_fd = openat(stack[depth - 1], ".", of);
                if (result_fd < 0) { map_errno(errno, err); goto fail; }
                if (mode == HL_FS_OPEN_READ) {
                    result_fd = finalize_regular_leaf(result_fd, err);
                    if (result_fd < 0) goto fail;   /* directory -> not_a_regular_file */
                }
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
        /* Every component token parsed - "." and ".." included: a symlink
         * target of 2000 "./" pieces is re-read on every expansion. Each
         * step's syscalls are charged below, before they run (audit 38). */
        if (hl_fs_charge(HL_WORK_FS_NAME_UNITS)) { *err = HL_FS_BUDGET_ERR; goto fail; }

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
            /* INTERIOR component: descend into a directory.
             *
             * CLASSIFY BEFORE OPENING. Opening an arbitrary interior inode merely
             * to learn its type is unsafe: an attacker-planted FIFO opened O_RDONLY
             * without O_NONBLOCK blocks resolution indefinitely, and device nodes
             * can carry open-time side effects. fstatat(AT_SYMLINK_NOFOLLOW) is an
             * lstat - it never blocks and never triggers device semantics:
             *   - a symlink     -> expand via readlinkat (below); never opened.
             *   - not a dir     -> "not_a_directory"; never opened (FIFO/dev/reg).
             *   - a directory   -> open it O_RDONLY|O_DIRECTORY|O_NOFOLLOW.
             * O_DIRECTORY keeps the FIFO-hang closed even against a TOCTOU swap (the
             * kernel rejects a non-dir with ENOTDIR before any blocking open) and
             * O_NOFOLLOW rejects a swap-to-symlink (ELOOP). A component that changed
             * between classify and open is re-classified (bounded by
             * HL_FS_RECLASSIFY_MAX); the flags are NEVER relaxed. This also removes
             * the old macOS ELOOP-vs-ENOTDIR ambiguity: an interior symlink is now
             * recognised by the lstat, not by interpreting an open error. */
            int reclass = 0;
            if (reserved_step(stack[depth - 1], comp, NULL)) {
                *err = "reserved_path"; goto fail;
            }
            for (;;) {
                struct stat lst;
                /* The classify (and each re-classify) is a lookup. */
                if (hl_fs_charge(HL_WORK_FS_LOOKUP_UNITS)) {
                    *err = HL_FS_BUDGET_ERR; goto fail;
                }
                if (fstatat(stack[depth - 1], comp, &lst, AT_SYMLINK_NOFOLLOW) != 0) {
                    if (errno == ENOENT && (mode == HL_FS_OPEN_WRITE ||
                                            mode == HL_FS_OPEN_WRITE_KEEP)) {
                        /* A directory created (mkdir-p): charged before it
                         * is made, so a tripped walk creates nothing more. */
                        if (hl_fs_charge(HL_WORK_FS_MKDIR_UNITS)) {
                            *err = HL_FS_BUDGET_ERR; goto fail;
                        }
                        if (mkdirat(stack[depth - 1], comp, 0755) != 0 && errno != EEXIST) {
                            map_errno(errno, err); goto fail;
                        }
                        if (++reclass > HL_FS_RECLASSIFY_MAX) { *err = "io_error"; goto fail; }
                        continue;                       /* re-classify the created dir */
                    }
                    map_errno(errno, err); goto fail;
                }
                if (S_ISLNK(lst.st_mode)) goto symlink;             /* expand; no open */
                if (!S_ISDIR(lst.st_mode)) { *err = "not_a_directory"; goto fail; }

                /* The directory open, fstat and (later) close. */
                if (hl_fs_charge(HL_WORK_FS_STEP_UNITS)) {
                    *err = HL_FS_BUDGET_ERR; goto fail;
                }
                int fd = openat(stack[depth - 1], comp,
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0) {
                    /* TOCTOU: the component changed between classify and open.
                     * Re-classify (bounded); never relax O_DIRECTORY/O_NOFOLLOW. */
                    if (errno == ELOOP || errno == EMLINK || errno == ENOTDIR ||
                        errno == ENOENT) {
                        /* Exhausting the bound is itself a (pathological) contained
                         * failure: report ONE stable token (io_error) on every path,
                         * not whichever transient errno happened to be last. Matches
                         * the WRITE mkdirat re-classification path above. */
                        if (++reclass > HL_FS_RECLASSIFY_MAX) { *err = "io_error"; goto fail; }
                        continue;
                    }
                    map_errno(errno, err); goto fail;
                }
                struct stat st;
                if (fstat(fd, &st) != 0) { close(fd); map_errno(errno, err); goto fail; }
                if (!S_ISDIR(st.st_mode)) { close(fd); *err = "not_a_directory"; goto fail; }
                if (reserved_step(stack[depth - 1], comp, &st)) {
                    close(fd); *err = "reserved_path"; goto fail;
                }
                if (depth >= HL_FS_MAX_DEPTH) { close(fd); *err = "path_too_deep"; goto fail; }
                stack[depth++] = fd;
                break;
            }
            cur = rest;
            continue;
        } else if (mode == HL_FS_OPEN_DIR) {
            /* Terminal in DIR mode: this component IS the result but must be a
             * directory, following a symlink (contained). CLASSIFY first (like an
             * interior component): a terminal FIFO opened O_RDONLY would block, and
             * O_DIRECTORY|O_NOFOLLOW on a symlink returns ENOTDIR on macOS (not
             * ELOOP), which would misread a symlinked dir as not_a_directory. */
            struct stat lst;
            if (hl_fs_charge(HL_WORK_FS_LOOKUP_UNITS)) { *err = HL_FS_BUDGET_ERR; goto fail; }
            if (reserved_step(stack[depth - 1], comp, NULL)) {
                *err = "reserved_path"; goto fail;
            }
            if (fstatat(stack[depth - 1], comp, &lst, AT_SYMLINK_NOFOLLOW) != 0) {
                map_errno(errno, err); goto fail;
            }
            if (S_ISLNK(lst.st_mode)) goto symlink;                 /* follow the symlink */
            if (!S_ISDIR(lst.st_mode)) { *err = "not_a_directory"; goto fail; }
            if (hl_fs_charge(HL_WORK_FS_STEP_UNITS)) { *err = HL_FS_BUDGET_ERR; goto fail; }
            int fd = openat(stack[depth - 1], comp,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                if (errno == ELOOP || errno == EMLINK) goto symlink;   /* swapped to a symlink */
                if (errno == ENOTDIR) { *err = "not_a_directory"; goto fail; }
                map_errno(errno, err); goto fail;
            }
            {
                struct stat dst;
                if (fstat(fd, &dst) != 0) { close(fd); map_errno(errno, err); goto fail; }
                if (reserved_step(stack[depth - 1], comp, &dst)) {
                    close(fd); *err = "reserved_path"; goto fail;
                }
            }
            result_fd = fd;
            goto done;
        } else {
            /* terminal component (READ / WRITE leaf). O_NONBLOCK guarantees a
             * special-file leaf (FIFO / device / socket) can never BLOCK the open;
             * the opened fd is then type-gated to a regular file
             * (finalize_regular_leaf). WRITE keeps O_CREAT|O_TRUNC (creation +
             * truncate) plus the contained mkdir-p already done for interior
             * components. A symlink leaf still returns ELOOP (O_NOFOLLOW) and is
             * handled by the symlink policy below, unchanged. */
            /* The leaf open: the lookup before it; a file created or truncated
             * (WRITE) costs a directory update, charged before it too; an
             * existing file opened for READ is charged once it opened (a
             * missing one costs the lookup only). */
            if (hl_fs_charge(HL_WORK_FS_LOOKUP_UNITS) ||
                (mode != HL_FS_OPEN_READ && hl_fs_charge(HL_WORK_FS_CREATE_UNITS))) {
                *err = HL_FS_BUDGET_ERR; goto fail;
            }
            if (reserved_step(stack[depth - 1], comp, NULL)) {
                *err = "reserved_path"; goto fail;
            }
            int flags = (mode == HL_FS_OPEN_READ)
                          ? (O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
                          : (O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC
                             | (mode == HL_FS_OPEN_WRITE ? O_TRUNC : 0));
            int fd = openat(stack[depth - 1], comp, flags, cmode);
            if (fd >= 0) {
                if (mode == HL_FS_OPEN_READ && hl_fs_charge(HL_WORK_FS_LEAF_UNITS)) {
                    close(fd); *err = HL_FS_BUDGET_ERR; goto fail;
                }
                result_fd = finalize_regular_leaf(fd, err);  /* closes fd + sets *err on reject */
                if (result_fd < 0) goto fail;                /* *err already set */
                if (leaf && leaf_sz > clen) memcpy(leaf, comp, clen + 1);
                goto done;
            }
            if (errno == ELOOP || errno == EMLINK) goto symlink;
            if (leaf_type_errno(errno)) { *err = "not_a_regular_file"; goto fail; }
            map_errno(errno, err);
            goto fail;
        }

    symlink:
        /* Every symlink-encounter site jumps here, so one gate enforces the policy:
         * under REFUSE, any symlink (intermediate or terminal) is denied outright. */
        if (sympol == HL_FS_SYMLINK_REFUSE) { *err = "symlink_denied"; goto fail; }
        if (++symlinks > HL_FS_SYMLINK_MAX) { *err = "symlink_loop"; goto fail; }
        /* An expansion: readlinkat and the splice; the components it puts
         * back are charged as they are walked again. */
        if (hl_fs_charge(HL_WORK_FS_SYMLINK_UNITS)) { *err = HL_FS_BUDGET_ERR; goto fail; }
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
    return hl_fs_open_at_ex(root_fd, relpath, mode, HL_FS_SYMLINK_FOLLOW,
                            create_mode, err);
}

int hl_fs_open_at_ex(int root_fd, const char *relpath, HlFsOpenMode mode,
                     HlFsSymlink sympol, mode_t create_mode, const char **err)
{
    return hl_fs_open_at_leaf(root_fd, relpath, mode, sympol, create_mode,
                              NULL, 0, err);
}

int hl_fs_open_at_leaf(int root_fd, const char *relpath, HlFsOpenMode mode,
                       HlFsSymlink sympol, mode_t create_mode,
                       char *leaf, size_t leaf_sz, const char **err)
{
    const char *e = "io_error";
    if (leaf && leaf_sz) leaf[0] = '\0';
    if (root_fd < 0 || !relpath) { if (err) *err = "invalid_args"; return -1; }
    if (!caller_path_ok(relpath)) { if (err) *err = "invalid_path"; return -1; }
    /* Public depth bound, enforced BEFORE either implementation so both accept /
     * reject the same caller paths (openat2 has no component-count limit; the
     * manual walk's held-fd stack does). See HL_FS_MAX_DEPTH in fs_resolve.h. */
    if (caller_component_count(relpath) > HL_FS_MAX_DEPTH) {
        if (err) *err = "path_too_deep";
        return -1;
    }
    /* The resolution itself: the checks above, the root's dup (audit 38). */
    if (hl_fs_charge(HL_WORK_FS_RESOLVE_UNITS)) {
        if (err) *err = HL_FS_BUDGET_ERR;
        return -1;
    }

#if defined(__linux__) && !defined(__COSMOPOLITAN__)
    /* openat2 does not create intermediate dirs, so WRITE always uses the manual
     * walk (which does the contained mkdir-p). READ and DIR take the one-call fast
     * path, unless HL_FS_FORCE_MANUAL is set (parity testing). */
    if ((mode == HL_FS_OPEN_READ || mode == HL_FS_OPEN_DIR) && !force_manual() &&
        !reserved_dir_present()) {
        /* A READ leaf opens O_NONBLOCK so a special-file target can never block the
         * open; it is then type-gated to a regular file. DIR keeps O_DIRECTORY (its
         * own S_ISDIR contract; a directory open never blocks). */
        int of = O_RDONLY | O_CLOEXEC
                 | (mode == HL_FS_OPEN_DIR ? O_DIRECTORY : O_NONBLOCK);
        int fd = try_openat2(root_fd, relpath, of, sympol, 0, &e);
        /* One syscall, charged as the walk it replaces (audit 38): a step per
         * interior component and the leaf (symlinks it follows inside the
         * kernel are bounded by its own ELOOP limit). Charged once it ran -
         * one bounded call - so a fallback to the manual walk (fd == -2:
         * no openat2 on this kernel) is charged by that walk alone. */
        if (fd != -2 && hl_fs_charge(openat2_units(relpath, mode))) {
            if (fd >= 0) close(fd);
            if (err) *err = HL_FS_BUDGET_ERR;
            return -1;
        }
        if (fd >= 0) {
            if (mode == HL_FS_OPEN_READ) {
                fd = finalize_regular_leaf(fd, &e);
                if (fd < 0) { if (err) *err = e; return -1; }
            }
            return fd;
        }
        if (fd == -1) { if (err) *err = e; return -1; }
        /* fd == -2: openat2 unavailable -> manual walk */
    }
#endif

    int fd = resolve_manual(root_fd, relpath, mode, sympol, create_mode,
                            leaf, leaf_sz, &e);
    if (fd < 0 && err) *err = e;
    return fd;
}

int hl_fs_resolve_parent(int root_fd, const char *relpath, HlFsSymlink sympol,
                         HlFsParent *out, const char **err)
{
    const char *e = "io_error";
    if (root_fd < 0 || !relpath || !out) { if (err) *err = "invalid_args"; return -1; }
    out->parent_fd = -1;
    out->leaf[0] = '\0';
    if (!caller_path_ok(relpath)) { if (err) *err = "invalid_path"; return -1; }
    if (caller_component_count(relpath) > HL_FS_MAX_DEPTH) {
        if (err) *err = "path_too_deep";
        return -1;
    }
    if (hl_fs_charge(HL_WORK_FS_RESOLVE_UNITS)) {   /* audit 38 */
        if (err) *err = HL_FS_BUDGET_ERR;
        return -1;
    }

    /* Grant root ".": there is no parent-plus-leaf. Hand back a CLOEXEC dup of the
     * anchor and an EMPTY leaf; the caller fstat()s the anchor directory itself.
     * (caller_path_ok accepted "." - a bare "." is a valid single "." segment.) */
    if (relpath[0] == '.' && relpath[1] == '\0') {
        int fd = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) { map_errno(errno, &e); if (err) *err = e; return -1; }
        out->parent_fd = fd;
        return 0;
    }

    /* Split the terminal component off lexically. `relpath` is a clean relative path
     * (no "..", no trailing slash), so the bytes after the final '/' are the leaf and
     * everything before it is the parent directory. The leaf is NEVER opened here -
     * the caller lstat()s it - so a terminal symlink is reported, not followed. */
    size_t len = strlen(relpath);
    const char *slash = NULL;
    for (size_t i = len; i-- > 0; ) {
        if (relpath[i] == '/') { slash = relpath + i; break; }
    }
    const char *leaf = slash ? slash + 1 : relpath;
    size_t leaf_len = strlen(leaf);
    if (leaf_len == 0 || leaf_len > NAME_MAX) { if (err) *err = "invalid_path"; return -1; }
    if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        if (err) *err = "invalid_path";              /* not a nameable leaf */
        return -1;
    }

    int parent_fd;
    if (!slash) {
        /* Single-component residual: the parent IS the anchor. */
        parent_fd = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
        if (parent_fd < 0) { map_errno(errno, &e); if (err) *err = e; return -1; }
    } else {
        char dir[HL_FS_PATH_MAX];
        size_t dlen = (size_t)(slash - relpath);
        if (dlen == 0 || dlen >= sizeof(dir)) { if (err) *err = "invalid_path"; return -1; }
        memcpy(dir, relpath, dlen);
        dir[dlen] = '\0';
        /* Walk the intermediates with the per-kind symlink policy; the parent must be
         * a directory (HL_FS_OPEN_DIR). The leaf stays unopened. */
        parent_fd = hl_fs_open_at_ex(root_fd, dir, HL_FS_OPEN_DIR, sympol, 0, &e);
        if (parent_fd < 0) { if (err) *err = e; return -1; }
    }

    if (reserved_step(parent_fd, leaf, NULL)) {
        close(parent_fd);
        if (err) *err = "reserved_path";
        return -1;
    }
    memcpy(out->leaf, leaf, leaf_len + 1);
    out->parent_fd = parent_fd;
    return 0;
}

FILE *hl_fs_fopen_read(const char *base_dir, const char *relpath)
{
    if (!base_dir || !relpath) return NULL;
    const char *err = NULL;
    int root_fd = hl_fs_open_base(base_dir, &err);
    if (root_fd < 0) return NULL;
    int fd = hl_fs_open_at(root_fd, relpath, HL_FS_OPEN_READ, 0, &err);
    close(root_fd);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "rb");
    if (!f) close(fd);
    return f;
}
