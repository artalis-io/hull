/*
 * cap/tar.c — ustar (.tar) parse / extract / create. See cap/tar.h.
 *
 * A minimal read-only ustar reader plus a matching writer. Files + directories
 * only (producers dereference symlinks to copies). Nested relative paths are
 * allowed (zig's lib/ tree); every member is validated - not absolute, no ".."
 * component - as defense in depth.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/tar.h"
#include "hull/shared/fs_util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAR_BLOCK 512

/* Parse a NUL/space-padded octal field of `n` bytes. */
static unsigned long tar_octal(const unsigned char *p, size_t n)
{
    unsigned long v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++)
        v = (v << 3) + (unsigned long)(p[i] - '0');
    return v;
}

/* Normalize + validate a member name IN PLACE. Strips a leading "./" and any
 * trailing "/", then requires: relative (not absolute), every segment non-empty
 * and not "..". Returns the cleaned name, an EMPTY string for the archive root
 * ("./" or "."), or NULL to reject (absolute / ".."). */
static const char *tar_safe_path(char *name)
{
    if (name[0] == '/') return NULL;
    while (name[0] == '.' && name[1] == '/') name += 2;
    size_t len = strlen(name);
    while (len > 0 && name[len - 1] == '/') name[--len] = '\0';
    if (len == 0)                   return name;      /* root "./" -> "" */
    if (len == 1 && name[0] == '.') return name + 1;  /* root "."  -> "" */
    for (const char *p = name; *p; ) {
        const char *slash = strchr(p, '/');
        size_t seg = slash ? (size_t)(slash - p) : strlen(p);
        if (seg == 0) return NULL;
        if (seg == 2 && p[0] == '.' && p[1] == '.') return NULL;
        p += seg;
        if (*p == '/') p++;
    }
    return name;
}

/* A symlink target is safe if it is relative (not absolute) and has no ".."
 * segment, so it can't point outside its own directory subtree. cosmocc's
 * arch-cc symlinks are bare same-dir names (e.g. "cosmocc"). */
static int tar_safe_linkname(const char *ln)
{
    if (!ln || ln[0] == '\0' || ln[0] == '/') return 0;
    for (const char *p = ln; *p; ) {
        const char *slash = strchr(p, '/');
        size_t seg = slash ? (size_t)(slash - p) : strlen(p);
        if (seg == 2 && p[0] == '.' && p[1] == '.') return 0;
        p += seg;
        if (*p == '/') p++;
    }
    return 1;
}

/* The shared ustar iterator. @p want_files surfaces regular-file / directory
 * members; @p want_symlinks surfaces symlink members (typeflag '2', target in
 * HlTarEntry.linkname). hl_tar_parse selects files only (preserving the
 * app-facing contract); extraction runs it twice (files, then symlinks). */
static int tar_iter(const unsigned char *tar, size_t tar_len,
                    int (*cb)(const HlTarEntry *e, void *ctx), void *ctx,
                    int want_files, int want_symlinks)
{
    if (!tar || !cb) return -1;

    size_t off = 0;
    while (off + TAR_BLOCK <= tar_len) {
        const unsigned char *hdr = tar + off;

        int zero = 1;
        for (int i = 0; i < TAR_BLOCK; i++) if (hdr[i]) { zero = 0; break; }
        if (zero) break;                        /* end-of-archive marker */

        char name[101];
        memcpy(name, hdr, 100);
        name[100] = '\0';

        char typeflag = (char)hdr[156];
        unsigned long size = tar_octal(hdr + 124, 12);
        unsigned mode = (unsigned)(tar_octal(hdr + 100, 8) & 0777);

        off += TAR_BLOCK;
        if (off + size > tar_len) return -1;    /* truncated data */

        if (typeflag == '5' || typeflag == '0' || typeflag == '\0') {
            if (want_files) {
                const char *rel = tar_safe_path(name);
                if (!rel) return -1;            /* absolute / ".." */
                if (rel[0] != '\0') {           /* skip the archive root */
                    HlTarEntry e = {
                        .name   = rel,
                        .is_dir = (typeflag == '5'),
                        .mode   = mode ? mode : 0644,
                        .data   = (typeflag == '5') ? NULL : tar + off,
                        .size   = (typeflag == '5') ? 0 : size,
                    };
                    int rc = cb(&e, ctx);
                    if (rc) return rc;
                }
            }
        } else if (typeflag == '2') {           /* symlink */
            if (want_symlinks) {
                const char *rel = tar_safe_path(name);
                if (!rel) return -1;
                char linkname[101];
                memcpy(linkname, hdr + 157, 100);
                linkname[100] = '\0';
                if (!tar_safe_linkname(linkname)) return -1;
                if (rel[0] != '\0') {
                    HlTarEntry e = {
                        .name       = rel,
                        .mode       = mode ? mode : 0755,
                        .is_symlink = 1,
                        .linkname   = linkname,
                    };
                    int rc = cb(&e, ctx);
                    if (rc) return rc;
                }
            }
        }
        off += (size + (TAR_BLOCK - 1)) & ~(size_t)(TAR_BLOCK - 1);
    }
    return 0;
}

int hl_tar_parse(const unsigned char *tar, size_t tar_len,
                 int (*cb)(const HlTarEntry *e, void *ctx), void *ctx)
{
    /* Files + directories only; symlinks/hardlinks are skipped, preserving the
     * app-facing hull.tar contract. Extraction handles symlinks separately. */
    return tar_iter(tar, tar_len, cb, ctx, /*want_files=*/1, /*want_symlinks=*/0);
}

/* ── Extraction (trusted; hull's own install path) ─────────────────── */

struct extract_ctx { const char *dest; };

/* Create parent directories of @p path (which ends in the member name). */
static int extract_make_parents(char *path)
{
    char *last = strrchr(path, '/');
    if (last && last != path) {
        *last = '\0';
        int rc = hl_mkdir_p(path, 0755);
        *last = '/';
        return rc;
    }
    return 0;
}

static int extract_file_cb(const HlTarEntry *e, void *vctx)
{
    struct extract_ctx *c = (struct extract_ctx *)vctx;
    char path[PATH_MAX];
    int pn = snprintf(path, sizeof(path), "%s/%s", c->dest, e->name);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return -1;

    if (e->is_dir) return hl_mkdir_p(path, 0755);

    if (extract_make_parents(path) != 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (e->size && fwrite(e->data, 1, e->size, f) != e->size) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) return -1;
    if (e->mode) (void)chmod(path, (mode_t)e->mode);
    return 0;
}

/* Copy @p src -> @p dst (the symlink copy-fallback). */
static int extract_copy(const char *src, const char *dst, unsigned mode)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    }
    if (ferror(in)) rc = -1;
    if (fclose(out) != 0) rc = -1;
    fclose(in);
    if (rc == 0 && mode) (void)chmod(dst, (mode_t)mode);
    return rc;
}

static int extract_symlink_cb(const HlTarEntry *e, void *vctx)
{
    struct extract_ctx *c = (struct extract_ctx *)vctx;
    char path[PATH_MAX];
    int pn = snprintf(path, sizeof(path), "%s/%s", c->dest, e->name);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return -1;

    if (extract_make_parents(path) != 0) return -1;
    (void)unlink(path);                          /* idempotent re-extract */
    if (symlink(e->linkname, path) == 0) return 0;

    /* Fallback where symlinks are unavailable (e.g. Windows without the
     * privilege): copy the target file. The target is relative to the link's
     * OWN directory; pass 1 already wrote it, so it is on disk. */
    char target[PATH_MAX];
    char *last = strrchr(path, '/');
    int tn = last
        ? snprintf(target, sizeof(target), "%.*s/%s",
                   (int)(last - path), path, e->linkname)
        : snprintf(target, sizeof(target), "%s/%s", c->dest, e->linkname);
    if (tn < 0 || (size_t)tn >= sizeof(target)) return -1;
    return extract_copy(target, path, e->mode ? e->mode : 0755);
}

int hl_tar_extract(const unsigned char *tar, size_t tar_len, const char *dest_dir)
{
    if (!dest_dir) return -1;
    /* mkdir -p, not a single mkdir: dest_dir's parent may not exist yet. A
     * `hull tools install <bundle>` into a fresh $HOME extracts to
     * ~/.hull/tools/<name>/ before anything else created ~/.hull/tools. */
    if (hl_mkdir_p(dest_dir, 0755) != 0) return -1;
    struct extract_ctx c = { dest_dir };
    /* Two passes: files + dirs first, THEN symlinks, so a link's target is
     * already on disk for the copy-fallback (and symlink ordering is moot). */
    int rc = tar_iter(tar, tar_len, extract_file_cb, &c, 1, 0);
    if (rc) return rc;
    return tar_iter(tar, tar_len, extract_symlink_cb, &c, 0, 1);
}

/* ── Creation ──────────────────────────────────────────────────────── */

/* Append `len` bytes to a growing buffer. Returns 0 / -1 (OOM). */
static int buf_append(unsigned char **buf, size_t *len, size_t *cap,
                      const void *src, size_t n)
{
    if (*len + n > *cap) {
        size_t nc = *cap ? *cap * 2 : 8192;
        while (nc < *len + n) {
            if (nc > (size_t)-1 / 2) return -1;
            nc *= 2;
        }
        unsigned char *nb = (unsigned char *)realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb;
        *cap = nc;
    }
    if (n) memcpy(*buf + *len, src, n);
    *len += n;
    return 0;
}

/* Fill a 512-byte ustar header for one entry, with a valid checksum. */
static int tar_write_header(unsigned char hdr[TAR_BLOCK], const HlTarEntry *e)
{
    memset(hdr, 0, TAR_BLOCK);
    size_t nl = strlen(e->name);
    if (nl == 0 || nl > 99) { errno = ENAMETOOLONG; return -1; }
    memcpy(hdr, e->name, nl);
    snprintf((char *)(hdr + 100), 8, "%07o", (e->mode ? e->mode : 0644) & 0777);
    snprintf((char *)(hdr + 108), 8, "%07o", 0);          /* uid */
    snprintf((char *)(hdr + 116), 8, "%07o", 0);          /* gid */
    snprintf((char *)(hdr + 124), 12, "%011o", (unsigned)(e->is_dir ? 0 : e->size));
    snprintf((char *)(hdr + 136), 12, "%011o", 0);        /* mtime */
    hdr[156] = e->is_dir ? '5' : '0';                     /* typeflag */
    memcpy(hdr + 257, "ustar", 6);                        /* magic */
    hdr[263] = '0'; hdr[264] = '0';                       /* version "00" */

    /* Checksum: sum of all header bytes with the chksum field taken as spaces. */
    memset(hdr + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < TAR_BLOCK; i++) sum += hdr[i];
    snprintf((char *)(hdr + 148), 7, "%06o", sum);
    hdr[154] = '\0';
    hdr[155] = ' ';
    return 0;
}

int hl_tar_create(const HlTarEntry *entries, size_t n,
                  unsigned char **out, size_t *out_len)
{
    if (!out || !out_len || (n && !entries)) return -1;
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0;
    unsigned char hdr[TAR_BLOCK];
    static const unsigned char zeros[TAR_BLOCK] = {0};

    for (size_t i = 0; i < n; i++) {
        /* Reject an unsafe member name (absolute / "..") in the writer too. */
        char nm[101];
        snprintf(nm, sizeof(nm), "%s", entries[i].name ? entries[i].name : "");
        if (!tar_safe_path(nm) || nm[0] == '\0') { free(buf); return -1; }

        if (tar_write_header(hdr, &entries[i]) != 0) { free(buf); return -1; }
        if (buf_append(&buf, &len, &cap, hdr, TAR_BLOCK) != 0) { free(buf); return -1; }
        if (!entries[i].is_dir && entries[i].size) {
            if (buf_append(&buf, &len, &cap, entries[i].data, entries[i].size) != 0) {
                free(buf); return -1;
            }
            size_t pad = (TAR_BLOCK - (entries[i].size % TAR_BLOCK)) % TAR_BLOCK;
            if (pad && buf_append(&buf, &len, &cap, zeros, pad) != 0) { free(buf); return -1; }
        }
    }
    /* Two zero blocks terminate a ustar archive. */
    if (buf_append(&buf, &len, &cap, zeros, TAR_BLOCK) != 0 ||
        buf_append(&buf, &len, &cap, zeros, TAR_BLOCK) != 0) {
        free(buf); return -1;
    }
    *out = buf;
    *out_len = len;
    return 0;
}
