/*
 * hull_cap_fs.c - Shared filesystem capability
 *
 * All file I/O goes through these functions with path validation.
 * Rejects "..", absolute paths, and paths outside the declared base_dir.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/fs.h"
#include "hull/shared/host.h"
#include "hull/cap/fs_resolve.h"
#include "hull/utils/alloc.h"
#include "hull/cap/audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

/* ── Path validation ────────────────────────────────────────────────── */

int hl_cap_fs_validate(const HlFsConfig *cfg, const char *path,
                       const char **err_msg)
{
    if (!cfg || !path || !cfg->base_dir) {
        if (err_msg) *err_msg = "invalid_args";
        return -1;
    }

    /* Reject empty path */
    if (path[0] == '\0') {
        if (err_msg) *err_msg = "empty_path";
        return -1;
    }

    /* Reject absolute paths */
    if (path[0] == '/') {
        if (err_msg) *err_msg = "absolute_path";
        return -1;
    }

    /* Reject ".." components - walk the path */
    const char *p = path;
    while (*p) {
        /* Check for ".." at start or after "/" */
        if (p[0] == '.' && p[1] == '.') {
            /* Must be followed by '/' or '\0' to be a component */
            if (p[2] == '/' || p[2] == '\0') {
                if (err_msg) *err_msg = "path_traversal";
                return -1;
            }
        }
        /* Advance to next component */
        const char *slash = strchr(p, '/');
        if (!slash)
            break;
        p = slash + 1;
    }

    /* Resolve the base directory (must exist) */
    char resolved_base[PATH_MAX];
    if (realpath(cfg->base_dir, resolved_base) == NULL) {
        if (err_msg) *err_msg = "validate_failed";
        return -1; /* base dir must exist */
    }

    /* Build full path */
    char full[PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", resolved_base, path);
    if (n < 0 || (size_t)n >= sizeof(full)) {
        if (err_msg) *err_msg = "validate_failed";
        return -1;
    }

    /* Walk up the path to find the deepest existing ancestor,
     * resolve it, and verify it's under base_dir. */
    char probe[PATH_MAX];
    strncpy(probe, full, sizeof(probe) - 1);
    probe[sizeof(probe) - 1] = '\0';

    char resolved[PATH_MAX];
    while (realpath(probe, resolved) == NULL) {
        char *slash = strrchr(probe, '/');
        if (!slash || slash == probe) {
            if (err_msg) *err_msg = "validate_failed";
            return -1; /* exhausted all ancestors */
        }
        *slash = '\0';
    }

    /* Verify the resolved ancestor starts with resolved base */
    size_t base_len = strlen(resolved_base);
    if (strncmp(resolved, resolved_base, base_len) != 0) {
        if (err_msg) *err_msg = "symlink_escape";
        return -1;
    }

    /* Must be followed by '/' or be exactly the base dir */
    if (resolved[base_len] != '/' && resolved[base_len] != '\0') {
        if (err_msg) *err_msg = "symlink_escape";
        return -1;
    }

    return 0;
}

/* ── Internal: build full path ──────────────────────────────────────── */

static int build_path(const HlFsConfig *cfg, const char *path,
                      char *out, size_t out_size, const char **err_msg)
{
    if (hl_cap_fs_validate(cfg, path, err_msg) != 0)
        return -1;

    /* Use resolved base_dir to avoid TOCTOU with symlinks.
     * hl_cap_fs_validate already verified base_dir resolves. */
    char resolved_base[PATH_MAX];
    if (realpath(cfg->base_dir, resolved_base) == NULL) {
        if (err_msg) *err_msg = "validate_failed";
        return -1;
    }

    int n = snprintf(out, out_size, "%s/%s", resolved_base, path);
    if (n < 0 || (size_t)n >= out_size) {
        if (err_msg) *err_msg = "validate_failed";
        return -1;
    }

    return 0;
}

#ifndef HL_FS_PATH_MAX
#define HL_FS_PATH_MAX 4096   /* residual scratch for policy selection */
#endif

/* ── Descriptor-relative resolution + path authorization ──
 * read/write/mmap resolve through the virtual-root resolver (fs_resolve.c) AND
 * the compiled path-authorization policy (fs_policy.c, docs/hull_fs_design.md
 * sec. 6). The op SELECTS an authorization entry from the policy for (path, mode)
 * - read/mmap from the READ set, write from the WRITE set - then opens the
 * LITERAL residual under the selected entry's HELD anchor fd (never a
 * reconstructed host path), with a per-kind symlink policy: a SUBTREE follows
 * in-root symlinks (contained within its anchor), while EXACT/CREATE/PATTERN
 * REFUSE any symlink so a symlink cannot alias a non-authorized target. No
 * matching grant -> "permission" (fail closed).
 *
 * SCOPE: read/write/mmap route through the policy.
 * hl_cap_fs_exists / hl_cap_fs_delete and any direct hl_cap_fs_validate consumer
 * still use the OLD build_path()/realpath path and are NOT policy-gated (they
 * remain base_dir-confined + sandbox-gated). Tracked follow-up. */
/* The authorization for `path` in `mode`: the selected grant's held anchor fd,
 * the residual under it (written into `scratch`), and the grant's symlink
 * policy. Returns 0, or -1 with *err_msg set. */
typedef struct {
    int          anchor_fd;
    const char  *residual;
    HlFsSymlink  sym;
} FsTarget;

static int fs_select(const HlFsConfig *cfg, const char *path, HlFsOpenMode mode,
                     char *scratch, size_t scratch_sz, FsTarget *out,
                     const char **err_msg)
{
    if (!cfg || !path || !cfg->base_dir) {
        if (err_msg) *err_msg = "invalid_args";
        return -1;
    }
    /* No policy -> deny (fail closed). The config is only wired when the app
     * declares fs grants, so a no-grant app never gets here with a live cfg. */
    if (!cfg->policy) {
        if (err_msg) *err_msg = "permission";
        return -1;
    }

    HlFsSelection sel = hl_fs_policy_select(cfg->policy, path, mode,
                                            scratch, scratch_sz);
    if (!sel.entry) {
        if (err_msg) *err_msg = sel.err ? sel.err : "permission";
        return -1;
    }

    /* SUBTREE follows in-root symlinks (contained within its anchor);
     * EXACT/CREATE/PATTERN refuse every symlink. */
    out->anchor_fd = sel.entry->anchor_fd;
    out->residual  = sel.residual;
    out->sym = (sel.entry->kind == HL_FS_ENTRY_SUBTREE)
                   ? HL_FS_SYMLINK_FOLLOW : HL_FS_SYMLINK_REFUSE;
    return 0;
}

static int fs_resolve_fd(const HlFsConfig *cfg, const char *path,
                         HlFsOpenMode mode, mode_t cmode, const char **err_msg)
{
    char scratch[HL_FS_PATH_MAX];
    FsTarget t;
    if (fs_select(cfg, path, mode, scratch, sizeof(scratch), &t, err_msg) != 0)
        return -1;
    const char *e = NULL;
    int fd = hl_fs_open_at_ex(t.anchor_fd, t.residual, mode, t.sym, cmode, &e);
    if (fd < 0 && err_msg) *err_msg = e ? e : "open_failed";
    return fd;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int hl_cap_fs_open_read_fd(const HlFsConfig *cfg, const char *path,
                           const char **err_msg)
{
    return fs_resolve_fd(cfg, path, HL_FS_OPEN_READ, 0, err_msg);
}

int hl_cap_fs_open_write_fd(const HlFsConfig *cfg, const char *path,
                            const char **err_msg)
{
    return fs_resolve_fd(cfg, path, HL_FS_OPEN_WRITE, 0644, err_msg);
}

int64_t hl_cap_fs_read(const HlFsConfig *cfg, const char *path,
                         char *buf, size_t buf_size,
                         const char **err_msg)
{
    int64_t result = -1;

    int fd = fs_resolve_fd(cfg, path, HL_FS_OPEN_READ, 0, err_msg);
    if (fd < 0)
        goto audit;

    FILE *f = fdopen(fd, "rb");
    if (!f) {
        close(fd);
        if (err_msg) *err_msg = "open_failed";
        goto audit;
    }

    /* Get file size */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (err_msg) *err_msg = "read_failed";
        goto audit;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        if (err_msg) *err_msg = "read_failed";
        goto audit;
    }

    /* If buf is NULL, just return the size */
    if (!buf) {
        fclose(f);
        result = (int64_t)size;
        goto audit;
    }

    if ((size_t)size > buf_size) {
        fclose(f);
        if (err_msg) *err_msg = "read_failed";
        goto audit; /* buffer too small */
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        if (err_msg) *err_msg = "read_failed";
        goto audit;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    int read_err = ferror(f);
    fclose(f);

    if (read_err || nread != (size_t)size) {
        if (err_msg) *err_msg = "read_failed";
        goto audit;
    }

    result = (int64_t)nread;

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.read");
        sh_json_write_kv_string(&w, "path", path);
        if (result >= 0)
            sh_json_write_kv_int(&w, "bytes", result);
        sh_json_write_kv_int(&w, "result", result >= 0 ? 0 : -1);
        hl_audit_end(&w);
    }
    return result;
}

/* Write all of `len` bytes to `fd`, retrying short writes and EINTR. */
static int fs_write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

/* The old write: open the leaf O_TRUNC through the resolver and rewrite it in
 * place. Kept for the one case rename cannot serve - a target that is an
 * in-root symlink under a SUBTREE grant, where the write has always gone
 * THROUGH the link to its target, and a rename would replace the link. */
static int fs_write_in_place(const HlFsConfig *cfg, const char *path,
                             const char *data, size_t len, unsigned flags,
                             const char **err_msg)
{
    int fd = fs_resolve_fd(cfg, path, HL_FS_OPEN_WRITE, 0644, err_msg);
    if (fd < 0)
        return -1;
    int rc = (len > 0 && data) ? fs_write_all(fd, data, len) : 0;
    if (rc == 0 && !(flags & HL_FS_WRITE_NO_SYNC) && fsync(fd) != 0)
        rc = -1;
    if (close(fd) != 0)
        rc = -1;
    if (rc != 0 && err_msg) *err_msg = "write_failed";
    return rc;
}

/* Remove a temp file left by a failed atomic write, best effort. */
static void fs_unlink_temp(int parent_fd, const char *name)
{
    if (parent_fd >= 0) (void)unlinkat(parent_fd, name, 0);
}

enum { FS_ATOMIC_OK = 0, FS_ATOMIC_FAILED = -1, FS_ATOMIC_IN_PLACE = 1 };

/* Write to a temp file beside the target, then rename it over the target, so a
 * reader - or the file after a crash - sees the old contents or the new, never
 * a mix. Both the temp and the rename go through the same grant, anchor and
 * symlink policy as the target: the temp is created by the descriptor-relative
 * resolver (which also makes missing parents), and the rename is relative to
 * the target's held parent directory, after checking that the temp IS in that
 * directory. */
static int fs_write_atomic(const HlFsConfig *cfg, const char *path,
                           const char *data, size_t len, unsigned flags,
                           const char **err_msg)
{
    char scratch[HL_FS_PATH_MAX];
    FsTarget t;
    if (fs_select(cfg, path, HL_FS_OPEN_WRITE, scratch, sizeof(scratch), &t,
                  err_msg) != 0)
        return FS_ATOMIC_FAILED;
    if (strcmp(t.residual, ".") == 0) {        /* the grant root: a directory */
        if (err_msg) *err_msg = "not_a_regular_file";
        return FS_ATOMIC_FAILED;
    }

    /* A name unique on this host: pid, a per-process counter and the clock. It
     * starts with a dot and names nothing a caller asked for, and is gone
     * again - renamed or removed - before this returns. */
    static unsigned long counter;
    unsigned long seq = __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    char tmpname[80];
    snprintf(tmpname, sizeof(tmpname), ".hull-tmp-%lx-%lx-%lx-%lx",
             (unsigned long)getpid(), seq, (unsigned long)now.tv_sec,
             (unsigned long)now.tv_nsec);

    char tmprel[HL_FS_PATH_MAX];
    const char *slash = strrchr(t.residual, '/');
    int w = slash
        ? snprintf(tmprel, sizeof(tmprel), "%.*s/%s",
                   (int)(slash - t.residual), t.residual, tmpname)
        : snprintf(tmprel, sizeof(tmprel), "%s", tmpname);
    if (w < 0 || (size_t)w >= sizeof(tmprel)) {
        if (err_msg) *err_msg = "invalid_path";
        return FS_ATOMIC_FAILED;
    }

    const char *e = NULL;
    int tfd = hl_fs_open_at_ex(t.anchor_fd, tmprel, HL_FS_OPEN_WRITE, t.sym,
                               0644, &e);
    if (tfd < 0) {
        if (err_msg) *err_msg = e ? e : "open_failed";
        return FS_ATOMIC_FAILED;
    }

    /* The target's directory, held. Its parents exist now (the temp's open
     * made them), so this cannot fail for a missing directory. */
    HlFsParent par;
    if (hl_fs_resolve_parent(t.anchor_fd, t.residual, t.sym, &par, &e) != 0) {
        close(tfd);
        HlFsParent tp;
        if (hl_fs_resolve_parent(t.anchor_fd, tmprel, t.sym, &tp, &e) == 0) {
            fs_unlink_temp(tp.parent_fd, tmpname);
            close(tp.parent_fd);
        }
        if (err_msg) *err_msg = e ? e : "open_failed";
        return FS_ATOMIC_FAILED;
    }

    const char *fail = NULL;
    int status = FS_ATOMIC_FAILED;
    struct stat target, tmp_fd_st, tmp_dir_st;
    int have_target = fstatat(par.parent_fd, par.leaf, &target,
                              AT_SYMLINK_NOFOLLOW) == 0;
    if (have_target && S_ISLNK(target.st_mode)) {
        if (t.sym == HL_FS_SYMLINK_REFUSE) {
            fail = "symlink_denied";
        } else {
            status = FS_ATOMIC_IN_PLACE;       /* write through it, as always */
        }
        goto out;
    }
    if (have_target && !S_ISREG(target.st_mode)) {
        fail = "not_a_regular_file";
        goto out;
    }
    /* The temp must be the file of that name in the target's directory, or the
     * rename below would move something else. */
    if (fstat(tfd, &tmp_fd_st) != 0
        || fstatat(par.parent_fd, tmpname, &tmp_dir_st, AT_SYMLINK_NOFOLLOW) != 0
        || tmp_fd_st.st_dev != tmp_dir_st.st_dev
        || tmp_fd_st.st_ino != tmp_dir_st.st_ino) {
        fail = "io_error";
        goto out;
    }
    /* A replaced file keeps its permission bits (a 0600 secret stays 0600). */
    if (have_target)
        (void)fchmod(tfd, target.st_mode & 07777);

    if ((len > 0 && data && fs_write_all(tfd, data, len) != 0)
        || (!(flags & HL_FS_WRITE_NO_SYNC) && fsync(tfd) != 0)) {
        fail = "write_failed";
        goto out;
    }
    if (close(tfd) != 0) {
        tfd = -1;
        fail = "write_failed";
        goto out;
    }
    tfd = -1;
    if (renameat(par.parent_fd, tmpname, par.parent_fd, par.leaf) != 0) {
        fail = "write_failed";
        goto out;
    }
    /* Make the rename itself durable. Not every platform can fsync a
     * directory; the replace has happened either way. */
    if (!(flags & HL_FS_WRITE_NO_SYNC))
        (void)fsync(par.parent_fd);
    status = FS_ATOMIC_OK;

out:
    if (tfd >= 0) close(tfd);
    if (status != FS_ATOMIC_OK) fs_unlink_temp(par.parent_fd, tmpname);
    close(par.parent_fd);
    if (fail && err_msg) *err_msg = fail;
    return status;
}

int hl_cap_fs_write_ex(const HlFsConfig *cfg, const char *path,
                       const char *data, size_t len, unsigned flags,
                       const char **err_msg)
{
    int result = -1;
    int status = fs_write_atomic(cfg, path, data, len, flags, err_msg);
    if (status == FS_ATOMIC_OK)
        result = 0;
    else if (status == FS_ATOMIC_IN_PLACE)
        result = fs_write_in_place(cfg, path, data, len, flags, err_msg);

    {
        ShJsonWriter w = hl_audit_begin("fs.write");
        sh_json_write_kv_string(&w, "path", path);
        sh_json_write_kv_int(&w, "len", (int64_t)len);
        sh_json_write_kv_int(&w, "result", result);
        hl_audit_end(&w);
    }
    return result;
}

int hl_cap_fs_write(const HlFsConfig *cfg, const char *path,
                      const char *data, size_t len,
                      const char **err_msg)
{
    return hl_cap_fs_write_ex(cfg, path, data, len, 0, err_msg);
}

int hl_cap_fs_exists(const HlFsConfig *cfg, const char *path,
                     const char **err_msg)
{
    char full[PATH_MAX];
    if (build_path(cfg, path, full, sizeof(full), err_msg) != 0)
        return -1;

    return access(full, F_OK) == 0 ? 1 : 0;
}

int hl_cap_fs_delete(const HlFsConfig *cfg, const char *path,
                     const char **err_msg)
{
    int result = -1;

    char full[PATH_MAX];
    if (build_path(cfg, path, full, sizeof(full), err_msg) != 0)
        goto audit;

    if (unlink(full) != 0) {
        if (err_msg) *err_msg = "delete_failed";
        goto audit;
    }

    result = 0;

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.delete");
        sh_json_write_kv_string(&w, "path", path);
        sh_json_write_kv_int(&w, "result", result);
        hl_audit_end(&w);
    }
    return result;
}

/* ── Metadata (stat) + enumeration (list) ─────── */

/* Map an lstat'd mode to the public node type (symlink reported as a link). */
static HlFsNodeType fs_node_type(mode_t m)
{
    if (S_ISREG(m))  return HL_FS_NODE_FILE;
    if (S_ISDIR(m))  return HL_FS_NODE_DIR;
    if (S_ISLNK(m))  return HL_FS_NODE_SYMLINK;
    return HL_FS_NODE_OTHER;   /* FIFO, socket, device, ... */
}

int hl_cap_fs_stat(const HlFsConfig *cfg, const char *path,
                   HlFsStatInfo *out, const char **err_msg)
{
    int result = -1;   /* -1 error, 0 present, 1 absent */
    /* Clear the caller's error slot up front so an ABSENT result (return 1) leaves
     * *err_msg == NULL even when the caller reuses a pointer that held a prior
     * token (contract: absent -> no error). */
    if (err_msg) *err_msg = NULL;
    if (!cfg || !path || !cfg->base_dir || !out) {
        if (err_msg) *err_msg = "invalid_args";
        return -1;
    }
    if (!cfg->policy) { if (err_msg) *err_msg = "permission"; return -1; }

    char scratch[HL_FS_PATH_MAX];
    /* stat selection also authorizes the app root itself ("."). */
    HlFsSelection sel = hl_fs_policy_select_stat(cfg->policy, path,
                                                 scratch, sizeof(scratch));
    if (!sel.entry) { if (err_msg) *err_msg = sel.err ? sel.err : "permission"; goto audit; }

    {
        /* SUBTREE follows in-root symlinks (intermediates only); EXACT/CREATE/PATTERN
         * refuse them. The terminal is NEVER followed - lstat reports a link. */
        HlFsSymlink sym = (sel.entry->kind == HL_FS_ENTRY_SUBTREE)
                              ? HL_FS_SYMLINK_FOLLOW : HL_FS_SYMLINK_REFUSE;
        HlFsParent pr;
        const char *e = NULL;
        if (hl_fs_resolve_parent(sel.entry->anchor_fd, sel.residual, sym, &pr, &e) != 0) {
            /* A "not_found" from resolution means an intermediate (e.g. a read-set
             * CREATE's missing ancestor) does not exist -> the authorized path is
             * ABSENT, not an error. Any other token is a genuine failure. */
            if (e && strcmp(e, "not_found") == 0) { result = 1; goto audit; }
            if (err_msg) *err_msg = e ? e : "io_error";
            goto audit;
        }

        struct stat st;
        int rc = (pr.leaf[0] == '\0')
                     ? fstat(pr.parent_fd, &st)
                     : fstatat(pr.parent_fd, pr.leaf, &st, AT_SYMLINK_NOFOLLOW);
        int saved = errno;
        close(pr.parent_fd);
        if (rc != 0) {
            if (saved == ENOENT) { result = 1; goto audit; }   /* ABSENT (not an error) */
            if (err_msg)
                *err_msg = (saved == EACCES || saved == EPERM) ? "permission" : "io_error";
            goto audit;
        }
        if (st.st_size < 0 || (uint64_t)st.st_size > HL_FS_SIZE_MAX) {
            if (err_msg) *err_msg = "size_unrepresentable";
            goto audit;
        }
        out->type  = fs_node_type(st.st_mode);
        out->size  = (uint64_t)st.st_size;
        out->mode  = (uint32_t)(st.st_mode & 0777);
        out->mtime = (int64_t)st.st_mtime;
        result = 0;
    }

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.stat");
        sh_json_write_kv_string(&w, "path", path);
        sh_json_write_kv_int(&w, "result", result);
        hl_audit_end(&w);
    }
    return result;
}

/* Sort comparator: unsigned-byte lexicographic, shorter-prefix-first. */
static int fs_direntry_cmp(const void *pa, const void *pb)
{
    const HlFsDirEntry *a = (const HlFsDirEntry *)pa;
    const HlFsDirEntry *b = (const HlFsDirEntry *)pb;
    size_t la = strlen(a->name), lb = strlen(b->name);
    size_t m = la < lb ? la : lb;
    int c = memcmp(a->name, b->name, m);   /* memcmp = unsigned-byte comparison */
    if (c) return c;
    if (la != lb) return la < lb ? -1 : 1; /* shorter prefix precedes */
    return 0;
}

int hl_cap_fs_list(const HlFsConfig *cfg, const char *path,
                   HlFsDirEntry **out_entries, size_t *out_count,
                   HlAllocator *alloc, const char **err_msg)
{
    if (out_entries) *out_entries = NULL;
    if (out_count)   *out_count = 0;
    if (!cfg || !path || !cfg->base_dir || !out_entries || !out_count) {
        if (err_msg) *err_msg = "invalid_args";
        return -1;
    }
    if (!cfg->policy) { if (err_msg) *err_msg = "permission"; return -1; }

    int result = -1;
    HlFsDirEntry *arr = NULL;
    size_t count = 0, cap = 0, total_bytes = 0;
    DIR *dirp = NULL;

    char scratch[HL_FS_PATH_MAX];
    HlFsListSelection sel = hl_fs_policy_select_list(cfg->policy, path,
                                                     scratch, sizeof(scratch));
    if (!sel.entry) { if (err_msg) *err_msg = sel.err ? sel.err : "permission"; goto audit; }

    {
        const char *e = NULL;
        int dfd = hl_fs_open_at_ex(sel.entry->anchor_fd, sel.residual, HL_FS_OPEN_DIR,
                                   sel.sympol, 0, &e);
        if (dfd < 0) { if (err_msg) *err_msg = e ? e : "io_error"; goto audit; }

        dirp = fdopendir(dfd);   /* takes ownership of dfd; closedir closes it */
        if (!dirp) { close(dfd); if (err_msg) *err_msg = "io_error"; goto audit; }
    }

    {
        struct dirent *de;
        errno = 0;
        while ((de = readdir(dirp)) != NULL) {
            const char *name = de->d_name;
            if (name[0] == '.' &&
                (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
                continue;   /* skip "." and ".." */

            /* BOUNDED name-length check: scan at most HL_FS_LIST_MAX_NAME_BYTES + 1
             * bytes (<= the struct-dirent d_name field on every platform) for the
             * terminator. A name with no NUL in that window is malformed / over-long
             * and FAILS the op - never a silent skip, and never an unbounded strlen
             * that could read past the entry's name storage. */
            const void *nul = memchr(name, '\0', HL_FS_LIST_MAX_NAME_BYTES + 1);
            if (!nul) { if (err_msg) *err_msg = "name_too_long"; goto fail_mid; }
            size_t nlen = (size_t)((const char *)nul - name);
            if (nlen == 0) { if (err_msg) *err_msg = "io_error"; goto fail_mid; }

            /* PATTERN filter: only the terminal-pattern matches are exposed. A
             * non-match is filtered out (NOT a failure). */
            if (sel.filter &&
                !hl_fs_pattern_match(sel.filter, strlen(sel.filter), name, nlen))
                continue;

            if (count >= HL_FS_LIST_MAX_ENTRIES) {
                if (err_msg) *err_msg = "too_many_entries";
                goto fail_mid;
            }
            if (nlen > HL_FS_LIST_MAX_TOTAL_BYTES - total_bytes) {
                if (err_msg) *err_msg = "listing_too_large";
                goto fail_mid;
            }

            struct stat st;
            if (fstatat(dirfd(dirp), name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                /* An entry that vanished mid-enumeration is a hard failure, not a
                 * silent skip - the whole list rolls back. */
                if (err_msg) *err_msg = "io_error";
                goto fail_mid;
            }
            if (st.st_size < 0 || (uint64_t)st.st_size > HL_FS_SIZE_MAX) {
                if (err_msg) *err_msg = "size_unrepresentable";
                goto fail_mid;
            }

            if (count == cap) {
                size_t ncap = cap ? cap * 2 : 16;
                HlFsDirEntry *na =
                    (HlFsDirEntry *)hl_alloc_calloc(alloc, ncap, sizeof(HlFsDirEntry));
                if (!na) { if (err_msg) *err_msg = "io_error"; goto fail_mid; }
                if (arr) {
                    memcpy(na, arr, cap * sizeof(HlFsDirEntry));
                    hl_alloc_free(alloc, arr, cap * sizeof(HlFsDirEntry));
                }
                arr = na;
                cap = ncap;
            }

            char *ncopy = (char *)hl_alloc_malloc(alloc, nlen + 1);
            if (!ncopy) { if (err_msg) *err_msg = "io_error"; goto fail_mid; }
            memcpy(ncopy, name, nlen);
            ncopy[nlen] = '\0';
            arr[count].name = ncopy;
            arr[count].type = fs_node_type(st.st_mode);
            arr[count].size = (uint64_t)st.st_size;
            count++;
            total_bytes += nlen;
            errno = 0;
        }
        if (errno != 0) {   /* readdir() failure (not end-of-stream) */
            if (err_msg) *err_msg = "io_error";
            goto fail_mid;
        }
    }

    closedir(dirp);   /* closes the held dir fd */
    dirp = NULL;

    if (count > 1)
        qsort(arr, count, sizeof(HlFsDirEntry), fs_direntry_cmp);

    /* Shrink the (doubling-grown) buffer to EXACTLY `count` so the caller's
     * hl_cap_fs_list_free(entries, count, alloc) frees the exact allocated size -
     * the tracked allocator requires matching sizes. Past this point `cap` is no
     * longer read (success falls straight to `audit`), so it is not re-tracked. */
    if (count > 0 && cap != count) {
        HlFsDirEntry *shrunk =
            (HlFsDirEntry *)hl_alloc_calloc(alloc, count, sizeof(HlFsDirEntry));
        if (!shrunk) { if (err_msg) *err_msg = "io_error"; goto fail_mid; }
        memcpy(shrunk, arr, count * sizeof(HlFsDirEntry));
        hl_alloc_free(alloc, arr, cap * sizeof(HlFsDirEntry));
        arr = shrunk;
    } else if (count == 0 && arr) {
        hl_alloc_free(alloc, arr, cap * sizeof(HlFsDirEntry));
        arr = NULL;
    }

    *out_entries = arr;
    *out_count = count;
    result = 0;
    goto audit;

fail_mid:
    /* Complete-result rollback: NO partial result is ever observable. */
    for (size_t k = 0; k < count; k++)
        hl_alloc_free(alloc, arr[k].name, strlen(arr[k].name) + 1);
    if (arr) hl_alloc_free(alloc, arr, cap * sizeof(HlFsDirEntry));
    if (dirp) closedir(dirp);
    *out_entries = NULL;
    *out_count = 0;
    result = -1;

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.list");
        sh_json_write_kv_string(&w, "path", path);
        sh_json_write_kv_int(&w, "entries", result == 0 ? (int64_t)count : -1);
        sh_json_write_kv_int(&w, "result", result);
        hl_audit_end(&w);
    }
    return result;
}

void hl_cap_fs_list_free(HlFsDirEntry *entries, size_t count, HlAllocator *alloc)
{
    if (!entries) return;
    for (size_t i = 0; i < count; i++)
        if (entries[i].name)
            hl_alloc_free(alloc, entries[i].name, strlen(entries[i].name) + 1);
    hl_alloc_free(alloc, entries, count * sizeof(HlFsDirEntry));
}

/* ── Memory-mapped file ────────────────────────────────────────────── */

HlMappedBuffer *hl_cap_fs_mmap(const HlFsConfig *cfg, const char *path,
                                HlAllocator *alloc, const char **err_msg)
{
    HlMappedBuffer *buf = NULL;

    int fd = fs_resolve_fd(cfg, path, HL_FS_OPEN_READ, 0, err_msg);
    if (fd < 0)
        goto audit;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        /* Do NOT read st here - it is uninitialized when fstat fails. */
        close(fd);
        if (err_msg) *err_msg = "mmap_failed";
        goto audit;
    }
    if (st.st_size <= 0) {
        close(fd);
        if (err_msg) *err_msg = st.st_size == 0 ? "empty_file" : "mmap_failed";
        goto audit;
    }

    void *addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* mapping survives close */

    if (addr == MAP_FAILED) {
        if (err_msg) *err_msg = "mmap_failed";
        goto audit;
    }

    buf = hl_alloc_malloc(alloc, sizeof(HlMappedBuffer));
    if (!buf) {
        munmap(addr, (size_t)st.st_size);
        if (err_msg) *err_msg = "mmap_failed";
        goto audit;
    }

    buf->addr = addr;
    buf->len = (size_t)st.st_size;
    buf->closed = 0;
    buf->alloc = alloc;
    buf->borrow_count = 0;
    buf->pending_free = 0;
    /* Whole-file mmap: the window IS the whole mapping. */
    buf->map_base = addr;
    buf->map_len = (size_t)st.st_size;
    buf->foffset = 0;

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.mmap");
        sh_json_write_kv_string(&w, "path", path);
        sh_json_write_kv_int(&w, "size", buf ? (int64_t)buf->len : -1);
        hl_audit_end(&w);
    }
    return buf;
}

int hl_cap_fs_mmap_window_geometry(uint64_t offset, uint64_t length,
                                   uint64_t file_size, uint64_t page_size,
                                   uint64_t *out_map_off, uint64_t *out_map_len,
                                   uint64_t *out_slop, uint64_t *out_eff_len,
                                   const char **err)
{
    /* page_size need only be > 0. The modulo/division arithmetic below makes NO
     * power-of-two assumption (POSIX page sizes are powers of two, but we do not
     * rely on it -- so an exotic page size neither misbehaves nor is rejected). */
    if (page_size == 0) {
        if (err) *err = "bad_page_size";
        return -1;
    }
    if (length == 0) {
        if (err) *err = "empty_window";
        return -1;
    }
    /* The cap is on the LOGICAL requested window length; see the header note. */
    if (length > HL_FS_MMAP_MAX_WINDOW_BYTES) {
        if (err) *err = "window_too_large";
        return -1;
    }
    /* offset must be strictly inside the file; offset == file_size maps nothing. */
    if (offset >= file_size) {
        if (err) *err = "offset_past_eof";
        return -1;
    }

    /* Clamp the window to end-of-file (a short final window is normal when
     * scanning a large file in fixed windows). offset < file_size, so avail >= 1
     * and eff_len >= 1: the EOF clamp can NEVER produce a zero-length mapping. */
    uint64_t avail = file_size - offset;
    uint64_t eff_len = length < avail ? length : avail;

    uint64_t slop = offset % page_size;   /* 0 <= slop < page_size */
    uint64_t map_off = offset - slop;     /* page-aligned; no underflow (slop<=offset) */

    /* need = slop + eff_len, then round UP to a whole number of pages, with fully
     * overflow-safe arithmetic (never `a + b <= LIMIT`, never an a*b that wraps). */
    if (slop > UINT64_MAX - eff_len) {
        if (err) *err = "align_overflow";
        return -1;
    }
    uint64_t need = slop + eff_len;
    uint64_t pages = need / page_size;
    if (need % page_size != 0) {
        if (pages == UINT64_MAX) { if (err) *err = "align_overflow"; return -1; }
        pages += 1;
    }
    if (pages > UINT64_MAX / page_size) {
        if (err) *err = "align_overflow";
        return -1;
    }
    uint64_t map_len = pages * page_size;

    /* The mmap region must not wrap the address space and its LENGTH must fit
     * size_t (the mmap/munmap length type) on this host. map_off's off_t
     * representability is enforced by the caller (see hl_cap_fs_mmap_window). */
    if (map_off > UINT64_MAX - map_len || map_len > (uint64_t)SIZE_MAX) {
        if (err) *err = "align_overflow";
        return -1;
    }

    if (out_map_off) *out_map_off = map_off;
    if (out_map_len) *out_map_len = map_len;
    if (out_slop) *out_slop = slop;
    if (out_eff_len) *out_eff_len = eff_len;
    return 0;
}

uint64_t hl_cap_fs_mmap_granularity(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    uint64_t gran = (pg > 0) ? (uint64_t)pg : 4096u;
    /* Windows MapViewOfFile takes the file offset in multiples of the
     * allocation granularity, which is 64 KiB and independent of the 4 KiB
     * page size sysconf reports. Measured on Windows 11 + cosmocc 4.0.2:
     *   mmap(off=4096)  -> EINVAL
     *   mmap(off=16384) -> EINVAL
     * A larger granularity is always a valid page alignment too, so this
     * only ever widens the mapping. */
    if (hl_host_is_windows() && gran < 65536u) gran = 65536u;
    return gran;
}

HlMappedBuffer *hl_cap_fs_mmap_window(const HlFsConfig *cfg, const char *path,
                                      uint64_t offset, uint64_t length,
                                      HlAllocator *alloc, const char **err_msg)
{
    HlMappedBuffer *buf = NULL;
    uint64_t map_off = 0, map_len = 0, slop = 0, eff_len = 0;

    int fd = fs_resolve_fd(cfg, path, HL_FS_OPEN_READ, 0, err_msg);
    if (fd < 0)
        goto audit;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        if (err_msg) *err_msg = "mmap_failed";
        goto audit;
    }
    if (st.st_size <= 0) {
        close(fd);
        if (err_msg) *err_msg = st.st_size == 0 ? "empty_file" : "mmap_failed";
        goto audit;
    }

    uint64_t gran = hl_cap_fs_mmap_granularity();

    if (hl_cap_fs_mmap_window_geometry(offset, length, (uint64_t)st.st_size,
                                       gran, &map_off, &map_len, &slop,
                                       &eff_len, err_msg) != 0) {
        close(fd);
        goto audit;
    }

    /* Reject before the narrowing (off_t)map_off cast. map_off <= offset <
     * file_size == st.st_size, itself a valid non-negative off_t, so this cannot
     * fire on a file-derived size; the explicit guard documents the invariant and
     * fails closed should a future change ever break it. (map_len's size_t fit is
     * enforced inside the geometry helper.) */
    off_t off_t_max =
        (off_t)((((uintmax_t)1) << (sizeof(off_t) * CHAR_BIT - 1)) - 1);
    if (map_off > (uint64_t)off_t_max) {
        close(fd);
        if (err_msg) *err_msg = "window_too_large";
        goto audit;
    }

    /* The geometry helper rounds map_len UP to a whole granule. Windows
     * refuses a view that extends past end-of-file (measured: mapping
     * 40960 bytes of a 40000-byte file fails, win32 error 8), so clamp to
     * what the file actually holds. Safe everywhere: mmap does not require
     * a page-multiple length, and the bytes past EOF were never readable
     * anyway. The window itself still fits - slop + eff_len never exceeds
     * file_size - map_off.
     *
     * Windows-only ON PURPOSE. The rounded tail past EOF is unusable
     * everywhere (SIGBUS on POSIX), so clamping would arguably be more
     * correct in general - but map_len is what the WASM span layer reserves
     * in guest address space and asserts on (tests/hull/cap/test_wasm_spans.c),
     * and that path is compiled out on the host this was fixed on. Not
     * changing memory geometry that cannot be re-validated here. */
    if (hl_host_is_windows() && map_len > (uint64_t)st.st_size - map_off)
        map_len = (uint64_t)st.st_size - map_off;

    void *base = mmap(NULL, (size_t)map_len, PROT_READ, MAP_PRIVATE, fd,
                      (off_t)map_off);
    close(fd); /* mapping survives close */
    if (base == MAP_FAILED) {
        if (err_msg) *err_msg = "mmap_failed";
        goto audit;
    }

    buf = hl_alloc_malloc(alloc, sizeof(HlMappedBuffer));
    if (!buf) {
        munmap(base, (size_t)map_len);
        if (err_msg) *err_msg = "mmap_failed";
        goto audit;
    }

    buf->map_base = base;
    buf->map_len = (size_t)map_len;
    buf->addr = (char *)base + slop;
    buf->len = (size_t)eff_len;
    buf->foffset = offset;
    buf->closed = 0;
    buf->alloc = alloc;
    buf->borrow_count = 0;
    buf->pending_free = 0;

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.mmap_window");
        sh_json_write_kv_string(&w, "path", path);
        sh_json_write_kv_int(&w, "offset", (int64_t)offset);
        sh_json_write_kv_int(&w, "size", buf ? (int64_t)buf->len : -1);
        hl_audit_end(&w);
    }
    return buf;
}

void hl_cap_fs_munmap(HlMappedBuffer *buf)
{
    if (!buf) return;
    /* Defer the real teardown while a zero-copy borrower (e.g. an image
     * created via image.from_buffer) still points into the mapping. The
     * last hl_cap_fs_mmap_release completes it. The owning userdata detaches
     * (sets its pointer to NULL) regardless, so no new borrows can start. */
    if (buf->borrow_count > 0) {
        buf->pending_free = 1;
        return;
    }
    /* Unmap the PAGE-ALIGNED mapping (map_base/map_len), never the caller window
     * (addr/len) -- for a windowed buffer addr is offset into map_base. */
    if (!buf->closed && buf->map_base) {
        munmap(buf->map_base, buf->map_len);
        buf->closed = 1;
    }
    hl_alloc_free(buf->alloc, buf, sizeof(HlMappedBuffer));
}

void hl_cap_fs_mmap_borrow(HlMappedBuffer *buf)
{
    if (buf) buf->borrow_count++;
}

void hl_cap_fs_mmap_release(void *p)
{
    HlMappedBuffer *buf = (HlMappedBuffer *)p;
    if (!buf) return;
    if (buf->borrow_count > 0) buf->borrow_count--;
    if (buf->borrow_count == 0 && buf->pending_free) {
        /* Unmap the page-aligned mapping, not the caller window (see munmap). */
        if (!buf->closed && buf->map_base) {
            munmap(buf->map_base, buf->map_len);
            buf->closed = 1;
        }
        hl_alloc_free(buf->alloc, buf, sizeof(HlMappedBuffer));
    }
}
