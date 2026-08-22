/*
 * hull_cap_fs.c — Shared filesystem capability
 *
 * All file I/O goes through these functions with path validation.
 * Rejects "..", absolute paths, and paths outside the declared base_dir.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/fs.h"
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
#include <errno.h>
#include <limits.h>

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

    /* Reject ".." components — walk the path */
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

/* ── Descriptor-relative resolution (checkpoint 1) ──────────────────────
 * read/write/mmap resolve through the virtual-root resolver (fs_resolve.c)
 * instead of realpath->check->open (docs/hull_fs_design.md §3). base_dir-anchored:
 * open base_dir as a dirfd, resolve `path` under it (symlinks followed but
 * confined), and return the leaf fd. No resolve-then-open window. exists/delete
 * still use build_path() until they gain a resolver mode (a later checkpoint). */
static int fs_resolve_fd(const HlFsConfig *cfg, const char *path,
                         HlFsOpenMode mode, mode_t cmode, const char **err_msg)
{
    if (!cfg || !path || !cfg->base_dir) {
        if (err_msg) *err_msg = "invalid_args";
        return -1;
    }
    const char *e = NULL;
    int root = hl_fs_open_base(cfg->base_dir, &e);
    if (root < 0) {
        if (err_msg) *err_msg = e ? e : "open_failed";
        return -1;
    }
    int fd = hl_fs_open_at(root, path, mode, cmode, &e);
    close(root);
    if (fd < 0 && err_msg) *err_msg = e ? e : "open_failed";
    return fd;
}

/* ── Public API ─────────────────────────────────────────────────────── */

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

int hl_cap_fs_write(const HlFsConfig *cfg, const char *path,
                      const char *data, size_t len,
                      const char **err_msg)
{
    int result = -1;

    /* WRITE mode: the resolver creates missing parent dirs (contained mkdirat)
     * and opens the leaf O_WRONLY|O_CREAT|O_TRUNC — same implicit-parents +
     * truncate behavior as the previous mkdir-p + fopen("wb"), now race-free. */
    int fd = fs_resolve_fd(cfg, path, HL_FS_OPEN_WRITE, 0644, err_msg);
    if (fd < 0)
        goto audit;

    {
        FILE *f = fdopen(fd, "wb");
        if (!f) {
            close(fd);
            if (err_msg) *err_msg = "write_failed";
            goto audit;
        }

        if (len > 0 && data) {
            size_t written = fwrite(data, 1, len, f);
            if (written != len) {
                fclose(f);
                if (err_msg) *err_msg = "write_failed";
                goto audit;
            }
        }

        fclose(f);
        result = 0;
    }

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.write");
        sh_json_write_kv_string(&w, "path", path);
        sh_json_write_kv_int(&w, "len", (int64_t)len);
        sh_json_write_kv_int(&w, "result", result);
        hl_audit_end(&w);
    }
    return result;
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
        /* Do NOT read st here — it is uninitialized when fstat fails. */
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

    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;

    if (hl_cap_fs_mmap_window_geometry(offset, length, (uint64_t)st.st_size,
                                       (uint64_t)pg, &map_off, &map_len, &slop,
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
