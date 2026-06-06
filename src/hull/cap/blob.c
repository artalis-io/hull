/**
 * @file cap/blob.c
 * @brief Content-addressed blob storage implementation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/blob.h"
#include "hull/cap/crypto.h"
#include "hull/cap/fs.h"
#include "hull/alloc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define HL_BLOB_TMP_PREFIX      ".blob-"
#define HL_BLOB_TMP_RAND_BYTES  8                          /* 16 hex chars */
#define HL_BLOB_TMP_RAND_HEX    (HL_BLOB_TMP_RAND_BYTES*2) /* 16 */
#define HL_BLOB_TMP_SUFFIX      ".tmp"
/* Full tmp-file basename: ".blob-" + <16-hex> + ".tmp" + NUL */
#define HL_BLOB_TMP_NAME_SIZE   (sizeof(HL_BLOB_TMP_PREFIX) - 1 + \
                                 HL_BLOB_TMP_RAND_HEX + \
                                 sizeof(HL_BLOB_TMP_SUFFIX))
#define HL_BLOB_DEFAULT_TMP_AGE 3600

/* ── Internal types ──────────────────────────────────────────────── */

struct HlBlob {
    HlAllocator *alloc;
    char        *root;        /* absolute path, no trailing slash */
    size_t       root_len;
    int          shard_depth; /* 1 or 2 */
};

struct HlBlobWriter {
    HlBlob      *store;
    int          fd;          /* tmp file fd; -1 once finalized/aborted */
    char        *tmp_path;    /* absolute path of tmp file */
    size_t       written;
    HlSha256Ctx  hash;
    char         expected[HL_BLOB_ID_BUF_SIZE];  /* "" if no expected */
};

struct HlBlobReader {
    HlBlob      *store;
    int          fd;
};

/* ── Hex helpers ─────────────────────────────────────────────────── */

static void hex_encode(const uint8_t *bytes, size_t len, char *out)
{
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = HEX[(bytes[i] >> 4) & 0xF];
        out[i * 2 + 1] = HEX[bytes[i] & 0xF];
    }
    out[len * 2] = '\0';
}

/* Validate that `id` is exactly HL_BLOB_ID_HEX_LEN lowercase hex
 * characters. Returns 0 on success, -1 on rejection. The lowercase
 * requirement keeps filenames canonical: we never write 'A'..'F' so
 * never need a case-insensitive lookup. Callers passing user input
 * should lower-case first. */
static int validate_id(const char *id)
{
    if (!id) return -1;
    for (size_t i = 0; i < HL_BLOB_ID_HEX_LEN; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return -1;
    }
    return id[HL_BLOB_ID_HEX_LEN] == '\0' ? 0 : -1;
}

/* ── Path builders ───────────────────────────────────────────────── */

/* Build the final blob path: <root>/blobs/<id[0:2]>[/<id[2:4]>]/<id>
 *
 * out must hold at least root_len + 1 + 6 + (depth*3) + 64 + 1 bytes.
 * Returns 0 on success, -1 on path overflow. */
static int build_blob_path(HlBlob *b, const char *id,
                             char *out, size_t out_cap)
{
    size_t needed = b->root_len + strlen("/blobs/") + 64 + 1;
    if (b->shard_depth >= 1) needed += 3;   /* "XX/" */
    if (b->shard_depth >= 2) needed += 3;   /* "YY/" */
    if (needed > out_cap) return -1;

    int n;
    if (b->shard_depth >= 2) {
        n = snprintf(out, out_cap, "%s/blobs/%c%c/%c%c/%s",
                     b->root, id[0], id[1], id[2], id[3], id);
    } else {
        n = snprintf(out, out_cap, "%s/blobs/%c%c/%s",
                     b->root, id[0], id[1], id);
    }
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

/* Build the shard-directory path so we can mkdir it before rename. */
static int build_shard_dir(HlBlob *b, const char *id,
                             char *out, size_t out_cap)
{
    int n;
    if (b->shard_depth >= 2) {
        n = snprintf(out, out_cap, "%s/blobs/%c%c/%c%c",
                     b->root, id[0], id[1], id[2], id[3]);
    } else {
        n = snprintf(out, out_cap, "%s/blobs/%c%c",
                     b->root, id[0], id[1]);
    }
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

/* mkdir -p: create `path` and any missing parents. Existing dirs are
 * fine. Returns 0 on success, -1 on failure (errno set). */
static int mkdir_p(const char *path, mode_t mode)
{
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, len + 1);

    for (size_t i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
            buf[i] = saved;
        }
    }
    return 0;
}

/* Random tmp-file basename using crypto.random for the entropy part.
 * Format: ".blob-<16hex>.tmp". Caller-supplied buffer must be at
 * least HL_BLOB_TMP_NAME_SIZE bytes. */
static int make_tmp_name(char *out, size_t out_cap)
{
    if (out_cap < HL_BLOB_TMP_NAME_SIZE) return -1;
    uint8_t rand_bytes[HL_BLOB_TMP_RAND_BYTES];
    if (hl_cap_crypto_random(rand_bytes, sizeof(rand_bytes)) != 0) return -1;
    char hex[HL_BLOB_TMP_RAND_HEX + 1];
    hex_encode(rand_bytes, sizeof(rand_bytes), hex);
    int n = snprintf(out, out_cap, "%s%s%s",
                     HL_BLOB_TMP_PREFIX, hex, HL_BLOB_TMP_SUFFIX);
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

/* Resolve `dir` (relative to fs_cfg->base_dir) to an absolute path.
 * Allocates via `alloc`; caller frees with hl_alloc_free(alloc, p, len+1). */
static char *resolve_root(const HlFsConfig *fs_cfg, HlAllocator *alloc,
                            const char *dir, size_t *out_len)
{
    if (!fs_cfg || !fs_cfg->base_dir || !dir) return NULL;
    size_t base_len = fs_cfg->base_len;
    size_t dir_len  = strlen(dir);

    /* Strip trailing slashes from dir. */
    while (dir_len > 0 && dir[dir_len - 1] == '/') dir_len--;

    size_t total = base_len + 1 + dir_len + 1;  /* base + '/' + dir + NUL */
    char *p = hl_alloc_malloc(alloc, total);
    if (!p) return NULL;

    memcpy(p, fs_cfg->base_dir, base_len);
    p[base_len] = '/';
    memcpy(p + base_len + 1, dir, dir_len);
    p[base_len + 1 + dir_len] = '\0';

    if (out_len) *out_len = base_len + 1 + dir_len;
    return p;
}

/* Sweep stale ".tmp" files under tmp/ older than `max_age_sec`. */
static void sweep_stale_tmps(const char *root, uint64_t max_age_sec)
{
    if (max_age_sec == UINT64_MAX) return;     /* sweep disabled */
    char tmp_dir[PATH_MAX];
    if (snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", root) >=
        (int)sizeof(tmp_dir)) return;

    DIR *d = opendir(tmp_dir);
    if (!d) return;

    time_t now = time(NULL);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, HL_BLOB_TMP_PREFIX,
                    sizeof(HL_BLOB_TMP_PREFIX) - 1) != 0) continue;

        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", tmp_dir, ent->d_name) >=
            (int)sizeof(path)) continue;

        struct stat st;
        if (stat(path, &st) < 0) continue;
        if ((uint64_t)(now - st.st_mtime) >= max_age_sec) unlink(path);
    }
    closedir(d);
}

int hl_cap_blob_init(HlBlob **out,
                       const HlFsConfig *fs_cfg,
                       HlAllocator *alloc,
                       const char *dir,
                       int shard_depth,
                       uint64_t tmp_max_age_sec)
{
    if (!out || !fs_cfg || !dir) return -1;

    /* Trim trailing slashes so "data/blobs/" works the same as
     * "data/blobs". hl_cap_fs_validate rejects paths with internal
     * "//" but accepts both forms here after the trim. */
    char trimmed[PATH_MAX];
    size_t dir_len = strlen(dir);
    while (dir_len > 0 && dir[dir_len - 1] == '/') dir_len--;
    if (dir_len == 0 || dir_len >= sizeof(trimmed)) return -1;
    memcpy(trimmed, dir, dir_len);
    trimmed[dir_len] = '\0';

    const char *err = NULL;
    if (hl_cap_fs_validate(fs_cfg, trimmed, &err) != 0) return -1;

    if (shard_depth < 1 || shard_depth > 2) shard_depth = 1;
    if (tmp_max_age_sec == 0) tmp_max_age_sec = HL_BLOB_DEFAULT_TMP_AGE;

    size_t root_len;
    char *root = resolve_root(fs_cfg, alloc, trimmed, &root_len);
    if (!root) return -1;

    /* Create root, blobs/, tmp/. */
    char path[PATH_MAX];
    if (mkdir_p(root, 0755) < 0) goto fail_root;

    if (snprintf(path, sizeof(path), "%s/blobs", root) >= (int)sizeof(path)) goto fail_root;
    if (mkdir_p(path, 0755) < 0) goto fail_root;

    if (snprintf(path, sizeof(path), "%s/tmp", root) >= (int)sizeof(path)) goto fail_root;
    if (mkdir_p(path, 0755) < 0) goto fail_root;

    sweep_stale_tmps(root, tmp_max_age_sec);

    HlBlob *b = hl_alloc_malloc(alloc, sizeof(*b));
    if (!b) goto fail_root;
    b->alloc       = alloc;
    b->root        = root;
    b->root_len    = root_len;
    b->shard_depth = shard_depth;

    *out = b;
    return 0;

fail_root:
    hl_alloc_free(alloc, root, root_len + 1);
    return -1;
}

void hl_cap_blob_free(HlBlob *b)
{
    if (!b) return;
    HlAllocator *alloc = b->alloc;
    hl_alloc_free(alloc, b->root, b->root_len + 1);
    hl_alloc_free(alloc, b, sizeof(*b));
}

/* ── Writer ──────────────────────────────────────────────────────── */

int hl_cap_blob_writer_open(HlBlob *b, const char *expected,
                              HlBlobWriter **out)
{
    if (!b || !out) return -1;
    if (expected && validate_id(expected) != 0) return -1;

    char tmp_name[HL_BLOB_TMP_NAME_SIZE];
    if (make_tmp_name(tmp_name, sizeof(tmp_name)) != 0) return -1;

    size_t tmp_path_len = b->root_len + strlen("/tmp/") + strlen(tmp_name) + 1;
    char  *tmp_path     = hl_alloc_malloc(b->alloc, tmp_path_len);
    if (!tmp_path) return -1;
    snprintf(tmp_path, tmp_path_len, "%s/tmp/%s", b->root, tmp_name);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        hl_alloc_free(b->alloc, tmp_path, tmp_path_len);
        return -1;
    }

    HlBlobWriter *w = hl_alloc_malloc(b->alloc, sizeof(*w));
    if (!w) {
        close(fd);
        unlink(tmp_path);
        hl_alloc_free(b->alloc, tmp_path, tmp_path_len);
        return -1;
    }
    w->store    = b;
    w->fd       = fd;
    w->tmp_path = tmp_path;
    w->written  = 0;
    hl_cap_crypto_sha256_init(&w->hash);
    if (expected)
        memcpy(w->expected, expected, HL_BLOB_ID_BUF_SIZE);
    else
        w->expected[0] = '\0';

    *out = w;
    return 0;
}

int hl_cap_blob_writer_write(HlBlobWriter *w,
                               const uint8_t *buf, size_t len)
{
    if (!w || w->fd < 0) return -1;
    if (len == 0) return 0;
    if (!buf) return -1;

    /* Write + hash in lockstep — bytes flow through both in one pass. */
    size_t remaining = len;
    const uint8_t *p = buf;
    while (remaining > 0) {
        ssize_t n = write(w->fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;       /* shouldn't happen for regular fd */
        if (hl_cap_crypto_sha256_update(&w->hash, p, (size_t)n) != 0)
            return -1;
        w->written += (size_t)n;
        p          += n;
        remaining  -= (size_t)n;
    }
    return 0;
}

/* Free a writer's resources (path + struct). Closes fd if still open. */
static void writer_release(HlBlobWriter *w)
{
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    HlAllocator *alloc = w->store->alloc;
    size_t tmp_len = strlen(w->tmp_path) + 1;
    hl_alloc_free(alloc, w->tmp_path, tmp_len);
    hl_alloc_free(alloc, w, sizeof(*w));
}

int hl_cap_blob_writer_finalize(HlBlobWriter *w,
                                  char *out_id, size_t *out_size)
{
    if (!w) return -1;
    if (w->fd < 0) { writer_release(w); return -1; }

    /* Finalize the hash and compute the destination id. */
    uint8_t digest[32];
    if (hl_cap_crypto_sha256_final(&w->hash, digest) != 0) {
        close(w->fd); w->fd = -1;
        unlink(w->tmp_path);
        writer_release(w);
        return -1;
    }
    char id[HL_BLOB_ID_BUF_SIZE];
    hex_encode(digest, 32, id);

    /* Close the tmp fd before rename. */
    if (close(w->fd) < 0) {
        w->fd = -1;
        unlink(w->tmp_path);
        writer_release(w);
        return -1;
    }
    w->fd = -1;

    /* Expected-SHA check. */
    if (w->expected[0] != '\0' && memcmp(w->expected, id, HL_BLOB_ID_HEX_LEN) != 0) {
        unlink(w->tmp_path);
        writer_release(w);
        return -1;
    }

    /* Build destination + ensure shard dir exists. */
    char dest[PATH_MAX], shard[PATH_MAX];
    if (build_blob_path(w->store, id, dest, sizeof(dest)) != 0 ||
        build_shard_dir(w->store, id, shard, sizeof(shard)) != 0) {
        unlink(w->tmp_path);
        writer_release(w);
        return -1;
    }
    if (mkdir_p(shard, 0755) < 0) {
        unlink(w->tmp_path);
        writer_release(w);
        return -1;
    }

    /* If destination already exists, blob is already stored — drop tmp.
     * (Content is identical by SHA so we don't need to overwrite.) */
    struct stat st;
    if (stat(dest, &st) == 0) {
        unlink(w->tmp_path);
    } else if (rename(w->tmp_path, dest) < 0) {
        /* EXDEV fallback (rare: tmp and blobs on different FS). */
        if (errno != EXDEV) {
            unlink(w->tmp_path);
            writer_release(w);
            return -1;
        }
        /* Copy + unlink. */
        FILE *src = fopen(w->tmp_path, "rb");
        FILE *dst = fopen(dest, "wb");
        if (!src || !dst) {
            if (src) fclose(src);
            if (dst) { fclose(dst); unlink(dest); }
            unlink(w->tmp_path);
            writer_release(w);
            return -1;
        }
        char copy_buf[65536];
        size_t n;
        int copy_err = 0;
        while ((n = fread(copy_buf, 1, sizeof(copy_buf), src)) > 0) {
            if (fwrite(copy_buf, 1, n, dst) != n) { copy_err = 1; break; }
        }
        if (ferror(src)) copy_err = 1;
        fclose(src);
        if (fclose(dst) != 0) copy_err = 1;
        if (copy_err) {
            unlink(dest);
            unlink(w->tmp_path);
            writer_release(w);
            return -1;
        }
        unlink(w->tmp_path);
    }

    if (out_id) memcpy(out_id, id, HL_BLOB_ID_BUF_SIZE);
    if (out_size) *out_size = w->written;
    writer_release(w);
    return 0;
}

void hl_cap_blob_writer_abort(HlBlobWriter *w)
{
    if (!w) return;
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    if (w->tmp_path) unlink(w->tmp_path);
    writer_release(w);
}

/* ── Buffer-mode put ─────────────────────────────────────────────── */

int hl_cap_blob_put(HlBlob *b, const uint8_t *buf, size_t len,
                      const char *expected, char *out_id)
{
    if (!b) return -1;
    /* Empty buffer is valid: it's the sha256("") blob. */
    if (len > 0 && !buf) return -1;

    HlBlobWriter *w = NULL;
    if (hl_cap_blob_writer_open(b, expected, &w) != 0) return -1;
    if (len > 0 && hl_cap_blob_writer_write(w, buf, len) != 0) {
        hl_cap_blob_writer_abort(w);
        return -1;
    }
    return hl_cap_blob_writer_finalize(w, out_id, NULL);
}

/* ── Reader ──────────────────────────────────────────────────────── */

static void bump_atime(const char *path)
{
    struct timeval tv[2];
    if (gettimeofday(&tv[0], NULL) != 0) return;
    tv[1] = tv[0];                       /* atime + mtime */
    utimes(path, tv);
}

int hl_cap_blob_reader_open(HlBlob *b, const char *id, int track_access,
                              HlBlobReader **out)
{
    if (!b || !id || !out) return -1;
    if (validate_id(id) != 0) return -1;

    char path[PATH_MAX];
    if (build_blob_path(b, id, path, sizeof(path)) != 0) return -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    HlBlobReader *r = hl_alloc_malloc(b->alloc, sizeof(*r));
    if (!r) { close(fd); return -1; }
    r->store = b;
    r->fd    = fd;

    if (track_access) bump_atime(path);

    *out = r;
    return 0;
}

int hl_cap_blob_reader_read(HlBlobReader *r,
                              uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!r || r->fd < 0 || !buf || !out_len) return -1;
    if (cap == 0) { *out_len = 0; return 0; }

    while (1) {
        ssize_t n = read(r->fd, buf, cap);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        *out_len = (size_t)n;
        return 0;
    }
}

void hl_cap_blob_reader_close(HlBlobReader *r)
{
    if (!r) return;
    if (r->fd >= 0) close(r->fd);
    hl_alloc_free(r->store->alloc, r, sizeof(*r));
}

int hl_cap_blob_get(HlBlob *b, const char *id, int track_access,
                      uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;

    size_t size = 0;
    if (hl_cap_blob_stat(b, id, &size, NULL) != 0) return -1;

    HlBlobReader *r = NULL;
    if (hl_cap_blob_reader_open(b, id, track_access, &r) != 0) return -1;

    uint8_t *buf = (size == 0) ? hl_alloc_malloc(b->alloc, 1)
                               : hl_alloc_malloc(b->alloc, size);
    if (!buf) { hl_cap_blob_reader_close(r); return -1; }

    size_t got = 0;
    while (got < size) {
        size_t n = 0;
        if (hl_cap_blob_reader_read(r, buf + got, size - got, &n) != 0) {
            hl_alloc_free(b->alloc, buf, size == 0 ? 1 : size);
            hl_cap_blob_reader_close(r);
            return -1;
        }
        if (n == 0) break;
        got += n;
    }
    hl_cap_blob_reader_close(r);

    if (got != size) {
        hl_alloc_free(b->alloc, buf, size == 0 ? 1 : size);
        return -1;
    }
    *out_buf = buf;
    *out_len = size;
    return 0;
}

/* ── Metadata ────────────────────────────────────────────────────── */

int hl_cap_blob_exists(HlBlob *b, const char *id)
{
    if (!b || !id || validate_id(id) != 0) return -1;
    char path[PATH_MAX];
    if (build_blob_path(b, id, path, sizeof(path)) != 0) return -1;
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    if (errno == ENOENT) return 0;
    return -1;
}

int hl_cap_blob_stat(HlBlob *b, const char *id,
                       size_t *size, int64_t *atime)
{
    if (!b || !id || validate_id(id) != 0) return -1;
    char path[PATH_MAX];
    if (build_blob_path(b, id, path, sizeof(path)) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (size)  *size  = (size_t)st.st_size;
    if (atime) *atime = (int64_t)st.st_atime;
    return 0;
}

int hl_cap_blob_delete(HlBlob *b, const char *id)
{
    if (!b || !id || validate_id(id) != 0) return -1;
    char path[PATH_MAX];
    if (build_blob_path(b, id, path, sizeof(path)) != 0) return -1;
    if (unlink(path) == 0) return 1;
    if (errno == ENOENT) return 0;
    return -1;
}

/* ── Enumeration (snapshot semantics) ────────────────────────────── */

typedef struct {
    char    id[HL_BLOB_ID_BUF_SIZE];
    size_t  size;
    int64_t atime;
    int64_t mtime;
} HlBlobEntry;

typedef struct {
    HlBlobEntry *items;
    size_t       count;
    size_t       capacity;
    HlAllocator *alloc;
} HlBlobEntries;

static int entries_push(HlBlobEntries *e, const HlBlobEntry *item)
{
    if (e->count == e->capacity) {
        size_t old_cap = e->capacity;
        size_t new_cap = old_cap == 0 ? 64 : old_cap * 2;
        if (new_cap > SIZE_MAX / sizeof(HlBlobEntry)) return -1;
        HlBlobEntry *grown = hl_alloc_realloc(e->alloc, e->items,
            old_cap * sizeof(HlBlobEntry),
            new_cap * sizeof(HlBlobEntry));
        if (!grown) return -1;
        e->items    = grown;
        e->capacity = new_cap;
    }
    e->items[e->count++] = *item;
    return 0;
}

static void entries_free(HlBlobEntries *e)
{
    if (e->items) hl_alloc_free(e->alloc, e->items,
        e->capacity * sizeof(HlBlobEntry));
    e->items = NULL;
    e->count = 0;
    e->capacity = 0;
}

/* Walk one shard directory, push entries. Returns 0 even when shard
 * doesn't exist (empty store legitimately has no shards yet). */
static int walk_shard(HlBlob *b, const char *shard_path, HlBlobEntries *e)
{
    (void)b;
    DIR *d = opendir(shard_path);
    if (!d) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    int rc = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (validate_id(ent->d_name) != 0) continue;

        char fpath[PATH_MAX];
        if (snprintf(fpath, sizeof(fpath), "%s/%s", shard_path, ent->d_name) >=
            (int)sizeof(fpath)) continue;
        struct stat st;
        if (stat(fpath, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        HlBlobEntry item;
        memcpy(item.id, ent->d_name, HL_BLOB_ID_BUF_SIZE);
        item.size  = (size_t)st.st_size;
        item.atime = (int64_t)st.st_atime;
        item.mtime = (int64_t)st.st_mtime;
        if (entries_push(e, &item) != 0) { rc = -1; break; }
    }
    closedir(d);
    return rc;
}

/* Walk every shard. Snapshot semantics: collect first, then caller
 * iterates. */
static int collect_entries(HlBlob *b, HlBlobEntries *e)
{
    e->items    = NULL;
    e->count    = 0;
    e->capacity = 0;
    e->alloc    = b->alloc;

    static const char HEX[] = "0123456789abcdef";
    char shard_path[PATH_MAX];

    if (b->shard_depth >= 2) {
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 16; j++) {
                for (int k = 0; k < 16; k++) {
                    for (int l = 0; l < 16; l++) {
                        if (snprintf(shard_path, sizeof(shard_path),
                                "%s/blobs/%c%c/%c%c",
                                b->root, HEX[i], HEX[j], HEX[k], HEX[l]) >=
                            (int)sizeof(shard_path)) continue;
                        if (walk_shard(b, shard_path, e) != 0) {
                            entries_free(e);
                            return -1;
                        }
                    }
                }
            }
        }
    } else {
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 16; j++) {
                if (snprintf(shard_path, sizeof(shard_path),
                        "%s/blobs/%c%c",
                        b->root, HEX[i], HEX[j]) >= (int)sizeof(shard_path))
                    continue;
                if (walk_shard(b, shard_path, e) != 0) {
                    entries_free(e);
                    return -1;
                }
            }
        }
    }
    return 0;
}

int hl_cap_blob_iter(HlBlob *b, HlBlobIterCb cb, void *user)
{
    if (!b || !cb) return -1;
    HlBlobEntries e;
    if (collect_entries(b, &e) != 0) return -1;
    for (size_t i = 0; i < e.count; i++) {
        if (cb(e.items[i].id, e.items[i].size, user) != 0) break;
    }
    entries_free(&e);
    return 0;
}

uint64_t hl_cap_blob_total_size(HlBlob *b)
{
    if (!b) return 0;
    HlBlobEntries e;
    if (collect_entries(b, &e) != 0) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < e.count; i++) total += e.items[i].size;
    entries_free(&e);
    return total;
}

uint64_t hl_cap_blob_count(HlBlob *b)
{
    if (!b) return 0;
    HlBlobEntries e;
    if (collect_entries(b, &e) != 0) return 0;
    uint64_t n = e.count;
    entries_free(&e);
    return n;
}

/* ── Cleanup / eviction ──────────────────────────────────────────── */

static int cmp_lru(const void *a, const void *b)
{
    int64_t ta = ((const HlBlobEntry *)a)->atime;
    int64_t tb = ((const HlBlobEntry *)b)->atime;
    if (ta < tb) return -1;
    if (ta > tb) return 1;
    return 0;
}

static int cmp_fifo(const void *a, const void *b)
{
    int64_t ta = ((const HlBlobEntry *)a)->mtime;
    int64_t tb = ((const HlBlobEntry *)b)->mtime;
    if (ta < tb) return -1;
    if (ta > tb) return 1;
    return 0;
}

int hl_cap_blob_cleanup(HlBlob *b, const HlBlobCleanupOpts *opts,
                          uint64_t *removed_out, uint64_t *freed_out)
{
    if (removed_out) *removed_out = 0;
    if (freed_out)   *freed_out   = 0;
    if (!b || !opts) return -1;

    HlBlobEntries e;
    if (collect_entries(b, &e) != 0) return -1;

    /* Sort by policy. Oldest first. */
    qsort(e.items, e.count, sizeof(HlBlobEntry),
          opts->strategy == HL_BLOB_FIFO ? cmp_fifo : cmp_lru);

    uint64_t total = 0;
    for (size_t i = 0; i < e.count; i++) total += e.items[i].size;

    time_t now = time(NULL);
    uint64_t removed = 0;
    uint64_t freed   = 0;
    int rc = 0;

    for (size_t i = 0; i < e.count; i++) {
        const HlBlobEntry *it = &e.items[i];
        int evict = 0;

        if (opts->max_age_sec > 0) {
            int64_t age = (int64_t)now - (opts->strategy == HL_BLOB_FIFO
                                            ? it->mtime : it->atime);
            if (age < 0) age = 0;
            if ((uint64_t)age >= opts->max_age_sec) evict = 1;
        }
        if (!evict && opts->max_total_size > 0 && total > opts->max_total_size)
            evict = 1;

        if (!evict) continue;

        if (!opts->dry_run) {
            char path[PATH_MAX];
            if (build_blob_path(b, it->id, path, sizeof(path)) != 0) {
                rc = -1; continue;
            }
            if (unlink(path) != 0 && errno != ENOENT) { rc = -1; continue; }
        }
        removed++;
        freed += it->size;
        if (total >= it->size) total -= it->size; else total = 0;
    }

    entries_free(&e);
    if (removed_out) *removed_out = removed;
    if (freed_out)   *freed_out   = freed;
    return rc;
}
