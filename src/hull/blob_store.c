/**
 * @file blob_store.c
 * @brief Low-level content-addressed blob store implementation.
 *
 * Moved out of cap/blob.c so the cap layer (manifest gate + future
 * audit emission) and runtime infrastructure (Lua bytecode cache,
 * compute AOT cache, future template cache) share a single CAS
 * implementation. cap/blob.c is now a thin wrapper that adds the
 * manifest fs.write validation; everything else forwards here.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/blob_store.h"
#include "hull/cap/crypto.h"
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

#define HL_BLOB_STORE_TMP_PREFIX      ".blob-"
#define HL_BLOB_STORE_TMP_RAND_BYTES  8                                  /* 16 hex chars */
#define HL_BLOB_STORE_TMP_RAND_HEX    (HL_BLOB_STORE_TMP_RAND_BYTES * 2) /* 16 */
#define HL_BLOB_STORE_TMP_SUFFIX      ".tmp"
/* Full tmp basename: ".blob-" + <16-hex> + ".tmp" + NUL. */
#define HL_BLOB_STORE_TMP_NAME_SIZE   (sizeof(HL_BLOB_STORE_TMP_PREFIX) - 1 + \
                                       HL_BLOB_STORE_TMP_RAND_HEX + \
                                       sizeof(HL_BLOB_STORE_TMP_SUFFIX))
#define HL_BLOB_STORE_DEFAULT_TMP_AGE 3600

/* ── Internal types ──────────────────────────────────────────────── */

struct HlBlobStore {
    HlAllocator *alloc;
    char        *root;        /* absolute path, no trailing slash */
    size_t       root_len;
    int          shard_depth; /* 1 or 2 */
};

struct HlBlobStoreWriter {
    HlBlobStore *store;
    int          fd;          /* tmp file fd; -1 once finalized/aborted */
    char        *tmp_path;    /* absolute path of tmp file */
    size_t       written;
    HlSha256Ctx  hash;
    char         expected[HL_BLOB_STORE_ID_BUF_SIZE];  /* "" if no expected */
    int          durable;
};

struct HlBlobStoreReader {
    HlBlobStore *store;
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

/* Validate that `id` is exactly HL_BLOB_STORE_ID_HEX_LEN lowercase
 * hex characters. */
static int validate_id(const char *id)
{
    if (!id) return -1;
    for (size_t i = 0; i < HL_BLOB_STORE_ID_HEX_LEN; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return -1;
    }
    return id[HL_BLOB_STORE_ID_HEX_LEN] == '\0' ? 0 : -1;
}

/* ── Path builders ───────────────────────────────────────────────── */

static int build_blob_path(HlBlobStore *s, const char *id,
                           char *out, size_t out_cap)
{
    size_t needed = s->root_len + strlen("/blobs/") + 64 + 1;
    if (s->shard_depth >= 1) needed += 3;
    if (s->shard_depth >= 2) needed += 3;
    if (needed > out_cap) return -1;

    int n;
    if (s->shard_depth >= 2) {
        n = snprintf(out, out_cap, "%s/blobs/%c%c/%c%c/%s",
                     s->root, id[0], id[1], id[2], id[3], id);
    } else {
        n = snprintf(out, out_cap, "%s/blobs/%c%c/%s",
                     s->root, id[0], id[1], id);
    }
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

static int build_shard_dir(HlBlobStore *s, const char *id,
                           char *out, size_t out_cap)
{
    int n;
    if (s->shard_depth >= 2) {
        n = snprintf(out, out_cap, "%s/blobs/%c%c/%c%c",
                     s->root, id[0], id[1], id[2], id[3]);
    } else {
        n = snprintf(out, out_cap, "%s/blobs/%c%c",
                     s->root, id[0], id[1]);
    }
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

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

static int make_tmp_name(char *out, size_t out_cap)
{
    if (out_cap < HL_BLOB_STORE_TMP_NAME_SIZE) return -1;
    uint8_t rand_bytes[HL_BLOB_STORE_TMP_RAND_BYTES];
    if (hl_cap_crypto_random(rand_bytes, sizeof(rand_bytes)) != 0) return -1;
    char hex[HL_BLOB_STORE_TMP_RAND_HEX + 1];
    hex_encode(rand_bytes, sizeof(rand_bytes), hex);
    int n = snprintf(out, out_cap, "%s%s%s",
                     HL_BLOB_STORE_TMP_PREFIX, hex,
                     HL_BLOB_STORE_TMP_SUFFIX);
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void sweep_stale_tmps(const char *root, uint64_t max_age_sec)
{
    if (max_age_sec == UINT64_MAX) return;
    char tmp_dir[PATH_MAX];
    if (snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", root) >=
        (int)sizeof(tmp_dir)) return;

    DIR *d = opendir(tmp_dir);
    if (!d) return;

    time_t now = time(NULL);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, HL_BLOB_STORE_TMP_PREFIX,
                    sizeof(HL_BLOB_STORE_TMP_PREFIX) - 1) != 0) continue;

        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", tmp_dir, ent->d_name) >=
            (int)sizeof(path)) continue;

        struct stat st;
        if (stat(path, &st) < 0) continue;
        if ((uint64_t)(now - st.st_mtime) >= max_age_sec) unlink(path);
    }
    closedir(d);
}

int hl_blob_store_open(HlBlobStore **out,
                       HlAllocator *alloc,
                       const char *abs_root,
                       int shard_depth,
                       uint64_t tmp_max_age_sec)
{
    if (!out || !abs_root || abs_root[0] != '/') return -1;

    /* Trim trailing slashes. */
    size_t root_len = strlen(abs_root);
    while (root_len > 0 && abs_root[root_len - 1] == '/') root_len--;
    if (root_len == 0) return -1;

    if (shard_depth < 1 || shard_depth > 2) shard_depth = 1;
    if (tmp_max_age_sec == 0) tmp_max_age_sec = HL_BLOB_STORE_DEFAULT_TMP_AGE;

    char *root = hl_alloc_malloc(alloc, root_len + 1);
    if (!root) return -1;
    memcpy(root, abs_root, root_len);
    root[root_len] = '\0';

    char path[PATH_MAX];
    if (mkdir_p(root, 0755) < 0) goto fail_root;

    if (snprintf(path, sizeof(path), "%s/blobs", root) >=
        (int)sizeof(path)) goto fail_root;
    if (mkdir_p(path, 0755) < 0) goto fail_root;

    if (snprintf(path, sizeof(path), "%s/tmp", root) >=
        (int)sizeof(path)) goto fail_root;
    if (mkdir_p(path, 0755) < 0) goto fail_root;

    sweep_stale_tmps(root, tmp_max_age_sec);

    HlBlobStore *s = hl_alloc_malloc(alloc, sizeof(*s));
    if (!s) goto fail_root;
    s->alloc       = alloc;
    s->root        = root;
    s->root_len    = root_len;
    s->shard_depth = shard_depth;

    *out = s;
    return 0;

fail_root:
    hl_alloc_free(alloc, root, root_len + 1);
    return -1;
}

void hl_blob_store_close(HlBlobStore *s)
{
    if (!s) return;
    HlAllocator *alloc = s->alloc;
    hl_alloc_free(alloc, s->root, s->root_len + 1);
    hl_alloc_free(alloc, s, sizeof(*s));
}

/* ── Writer ──────────────────────────────────────────────────────── */

static int writer_open_full(HlBlobStore *s, const char *expected, int durable,
                            HlBlobStoreWriter **out)
{
    if (!s || !out) return -1;
    if (expected && validate_id(expected) != 0) return -1;

    char tmp_name[HL_BLOB_STORE_TMP_NAME_SIZE];
    if (make_tmp_name(tmp_name, sizeof(tmp_name)) != 0) return -1;

    size_t tmp_path_len = s->root_len + strlen("/tmp/") + strlen(tmp_name) + 1;
    char  *tmp_path     = hl_alloc_malloc(s->alloc, tmp_path_len);
    if (!tmp_path) return -1;
    snprintf(tmp_path, tmp_path_len, "%s/tmp/%s", s->root, tmp_name);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        hl_alloc_free(s->alloc, tmp_path, tmp_path_len);
        return -1;
    }

    HlBlobStoreWriter *w = hl_alloc_malloc(s->alloc, sizeof(*w));
    if (!w) {
        close(fd);
        unlink(tmp_path);
        hl_alloc_free(s->alloc, tmp_path, tmp_path_len);
        return -1;
    }
    w->store    = s;
    w->fd       = fd;
    w->tmp_path = tmp_path;
    w->written  = 0;
    w->durable  = durable;
    hl_cap_crypto_sha256_init(&w->hash);
    if (expected)
        memcpy(w->expected, expected, HL_BLOB_STORE_ID_BUF_SIZE);
    else
        w->expected[0] = '\0';

    *out = w;
    return 0;
}

int hl_blob_store_writer_open(HlBlobStore *s, const char *expected,
                              HlBlobStoreWriter **out)
{
    return writer_open_full(s, expected, /*durable=*/0, out);
}

int hl_blob_store_writer_open_durable(HlBlobStore *s, const char *expected,
                                      HlBlobStoreWriter **out)
{
    return writer_open_full(s, expected, /*durable=*/1, out);
}

int hl_blob_store_writer_write(HlBlobStoreWriter *w,
                               const uint8_t *buf, size_t len)
{
    if (!w || w->fd < 0) return -1;
    if (len == 0) return 0;
    if (!buf) return -1;

    size_t remaining = len;
    const uint8_t *p = buf;
    while (remaining > 0) {
        ssize_t n = write(w->fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        if (hl_cap_crypto_sha256_update(&w->hash, p, (size_t)n) != 0)
            return -1;
        w->written += (size_t)n;
        p          += n;
        remaining  -= (size_t)n;
    }
    return 0;
}

static void writer_release(HlBlobStoreWriter *w)
{
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    HlAllocator *alloc = w->store->alloc;
    size_t tmp_len = strlen(w->tmp_path) + 1;
    hl_alloc_free(alloc, w->tmp_path, tmp_len);
    hl_alloc_free(alloc, w, sizeof(*w));
}

int hl_blob_store_writer_finalize(HlBlobStoreWriter *w,
                                  char *out_id, size_t *out_size)
{
    if (!w) return -1;
    if (w->fd < 0) { writer_release(w); return -1; }

    uint8_t digest[32];
    if (hl_cap_crypto_sha256_final(&w->hash, digest) != 0) {
        close(w->fd); w->fd = -1;
        unlink(w->tmp_path);
        writer_release(w);
        return -1;
    }
    char id[HL_BLOB_STORE_ID_BUF_SIZE];
    hex_encode(digest, 32, id);

    if (w->durable && fsync(w->fd) < 0) {
        close(w->fd); w->fd = -1;
        unlink(w->tmp_path);
        writer_release(w);
        return -1;
    }

    if (close(w->fd) < 0) {
        w->fd = -1;
        unlink(w->tmp_path);
        writer_release(w);
        return -1;
    }
    w->fd = -1;

    if (w->expected[0] != '\0' &&
        memcmp(w->expected, id, HL_BLOB_STORE_ID_HEX_LEN) != 0) {
        unlink(w->tmp_path);
        writer_release(w);
        return -1;
    }

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

    struct stat st;
    int renamed_into_place = 0;
    if (stat(dest, &st) == 0) {
        unlink(w->tmp_path);
    } else if (rename(w->tmp_path, dest) < 0) {
        if (errno != EXDEV) {
            unlink(w->tmp_path);
            writer_release(w);
            return -1;
        }
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
        renamed_into_place = 1;
    } else {
        renamed_into_place = 1;
    }

    if (w->durable && renamed_into_place) {
        int dfd = open(shard, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            (void)fsync(dfd);
            close(dfd);
        }
    }

    if (out_id) memcpy(out_id, id, HL_BLOB_STORE_ID_BUF_SIZE);
    if (out_size) *out_size = w->written;
    writer_release(w);
    return 0;
}

void hl_blob_store_writer_abort(HlBlobStoreWriter *w)
{
    if (!w) return;
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    if (w->tmp_path) unlink(w->tmp_path);
    writer_release(w);
}

/* ── Buffer put ──────────────────────────────────────────────────── */

static int store_put_full(HlBlobStore *s, const uint8_t *buf, size_t len,
                          const char *expected, int durable, char *out_id)
{
    if (!s) return -1;
    if (len > 0 && !buf) return -1;

    if (expected && validate_id(expected) == 0) {
        int rc = hl_blob_store_exists(s, expected);
        if (rc == 1) {
            if (out_id) memcpy(out_id, expected, HL_BLOB_STORE_ID_BUF_SIZE);
            return 0;
        }
    }

    HlBlobStoreWriter *w = NULL;
    int open_rc = durable
        ? hl_blob_store_writer_open_durable(s, expected, &w)
        : hl_blob_store_writer_open(s, expected, &w);
    if (open_rc != 0) return -1;
    if (len > 0 && hl_blob_store_writer_write(w, buf, len) != 0) {
        hl_blob_store_writer_abort(w);
        return -1;
    }
    return hl_blob_store_writer_finalize(w, out_id, NULL);
}

int hl_blob_store_put(HlBlobStore *s, const uint8_t *buf, size_t len,
                      const char *expected, char *out_id)
{
    return store_put_full(s, buf, len, expected, /*durable=*/0, out_id);
}

int hl_blob_store_put_durable(HlBlobStore *s, const uint8_t *buf, size_t len,
                              const char *expected, char *out_id)
{
    return store_put_full(s, buf, len, expected, /*durable=*/1, out_id);
}

int hl_blob_store_put_keyed(HlBlobStore *s, const char *key,
                            const uint8_t *bytes, size_t len)
{
    if (!s || validate_id(key) != 0) return -1;
    if (len > 0 && !bytes) return -1;

    /* Fast-path: target already present — same key implies same
     * bytes by the cache's design contract. Skip the write. */
    if (hl_blob_store_exists(s, key) == 1) return 0;

    /* Write into the store's tmp/ then atomic-rename into the shard
     * dir under the caller-supplied key. Mirrors writer_open_full +
     * writer_finalize, but without the SHA computation. */
    char tmp_name[HL_BLOB_STORE_TMP_NAME_SIZE];
    if (make_tmp_name(tmp_name, sizeof(tmp_name)) != 0) return -1;

    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s/tmp/%s",
                 s->root, tmp_name) >= (int)sizeof(tmp_path))
        return -1;

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return -1;

    size_t put = 0;
    while (put < len) {
        ssize_t w = write(fd, bytes + put, len - put);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(tmp_path);
            return -1;
        }
        if (w == 0) { close(fd); unlink(tmp_path); return -1; }
        put += (size_t)w;
    }
    if (close(fd) < 0) { unlink(tmp_path); return -1; }

    /* We pass a non-empty `key` to validate_id which we already did
     * above, so build_*_path are safe. */
    HlBlobStore *mutable_s = (HlBlobStore *)s;
    char dest[PATH_MAX], shard[PATH_MAX];
    if (build_blob_path(mutable_s, key, dest, sizeof(dest)) != 0 ||
        build_shard_dir(mutable_s, key, shard, sizeof(shard)) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (mkdir_p(shard, 0755) < 0) {
        unlink(tmp_path);
        return -1;
    }

    /* If a race produced the target between exists() and rename(),
     * unlink our tmp and accept the winner. Same content by
     * contract. */
    struct stat st;
    if (stat(dest, &st) == 0) {
        unlink(tmp_path);
        return 0;
    }
    if (rename(tmp_path, dest) < 0) {
        if (errno == EXDEV) {
            /* Cross-fs fallback: copy + unlink. */
            FILE *src = fopen(tmp_path, "rb");
            FILE *dst = fopen(dest, "wb");
            if (!src || !dst) {
                if (src) fclose(src);
                if (dst) { fclose(dst); unlink(dest); }
                unlink(tmp_path);
                return -1;
            }
            char copy_buf[65536];
            size_t n; int err = 0;
            while ((n = fread(copy_buf, 1, sizeof(copy_buf), src)) > 0) {
                if (fwrite(copy_buf, 1, n, dst) != n) { err = 1; break; }
            }
            if (ferror(src)) err = 1;
            fclose(src);
            if (fclose(dst) != 0) err = 1;
            unlink(tmp_path);
            if (err) { unlink(dest); return -1; }
            return 0;
        }
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* ── Reader ──────────────────────────────────────────────────────── */

static void bump_atime_fd(int fd)
{
    struct timeval tv[2];
    if (gettimeofday(&tv[0], NULL) != 0) return;
    tv[1] = tv[0];
    (void)futimes(fd, tv);
}

int hl_blob_store_reader_open(HlBlobStore *s, const char *id, int track_access,
                              HlBlobStoreReader **out)
{
    if (!s || !id || !out) return -1;
    if (validate_id(id) != 0) return -1;

    char path[PATH_MAX];
    if (build_blob_path(s, id, path, sizeof(path)) != 0) return -1;

    /* O_NOFOLLOW: refuse to follow a symlink at the blob path.
     * Nothing in the legitimate write path ever creates a symlink
     * inside the store, so a symlink here means someone planted
     * one — refuse to read what's at the other end. Matters on
     * multi-user hosts or shared HULL_CACHE_DIR mounts where the
     * cache root might be writable by an unprivileged user.
     * Symlink → ELOOP (Linux) / EMLINK (BSD) → blob_store_get
     * returns failure, caller falls back to fresh compile. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;

    HlBlobStoreReader *r = hl_alloc_malloc(s->alloc, sizeof(*r));
    if (!r) { close(fd); return -1; }
    r->store = s;
    r->fd    = fd;

    if (track_access) bump_atime_fd(fd);

    *out = r;
    return 0;
}

int hl_blob_store_reader_read(HlBlobStoreReader *r,
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

void hl_blob_store_reader_close(HlBlobStoreReader *r)
{
    if (!r) return;
    if (r->fd >= 0) close(r->fd);
    hl_alloc_free(r->store->alloc, r, sizeof(*r));
}

/* Defensive ceiling on any single blob the in-memory `_get` path
 * will materialise. Mirrors the limit used by `cache verify` and
 * is comfortably above every legitimate cache entry today (the
 * largest stdlib bytecode is ~50 KB; the largest AOT module is
 * a few hundred KB). Hostile or corrupted blob entries — e.g. a
 * stat that reports billions of bytes due to filesystem state
 * corruption, or a planted file in a shared HULL_CACHE_DIR —
 * are rejected rather than crashing the allocator. Callers that
 * need to stream larger payloads use the reader_open / _read
 * API directly. */
#define HL_BLOB_STORE_GET_MAX_BYTES ((size_t)(256u * 1024u * 1024u))

int hl_blob_store_get(HlBlobStore *s, const char *id, int track_access,
                      uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;

    size_t size = 0;
    if (hl_blob_store_stat(s, id, &size, NULL) != 0) return -1;

    if (size > HL_BLOB_STORE_GET_MAX_BYTES) {
        errno = EFBIG;
        return -1;
    }

    if (size == 0) {
        HlBlobStoreReader *r = NULL;
        if (hl_blob_store_reader_open(s, id, track_access, &r) != 0)
            return -1;
        hl_blob_store_reader_close(r);
        return 0;
    }

    HlBlobStoreReader *r = NULL;
    if (hl_blob_store_reader_open(s, id, track_access, &r) != 0) return -1;

    uint8_t *buf = hl_alloc_malloc(s->alloc, size);
    if (!buf) { hl_blob_store_reader_close(r); return -1; }

    size_t got = 0;
    while (got < size) {
        size_t n = 0;
        if (hl_blob_store_reader_read(r, buf + got, size - got, &n) != 0) {
            hl_alloc_free(s->alloc, buf, size);
            hl_blob_store_reader_close(r);
            return -1;
        }
        if (n == 0) break;
        got += n;
    }
    hl_blob_store_reader_close(r);

    if (got != size) {
        hl_alloc_free(s->alloc, buf, size);
        return -1;
    }
    *out_buf = buf;
    *out_len = size;
    return 0;
}

/* ── Metadata ────────────────────────────────────────────────────── */

int hl_blob_store_exists(HlBlobStore *s, const char *id)
{
    if (!s || !id || validate_id(id) != 0) return -1;
    char path[PATH_MAX];
    if (build_blob_path(s, id, path, sizeof(path)) != 0) return -1;
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    if (errno == ENOENT) return 0;
    return -1;
}

int hl_blob_store_stat(HlBlobStore *s, const char *id,
                       size_t *size, int64_t *atime)
{
    if (!s || !id || validate_id(id) != 0) return -1;
    char path[PATH_MAX];
    if (build_blob_path(s, id, path, sizeof(path)) != 0) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (size)  *size  = (size_t)st.st_size;
    if (atime) *atime = (int64_t)st.st_atime;
    return 0;
}

int hl_blob_store_delete(HlBlobStore *s, const char *id)
{
    if (!s || !id || validate_id(id) != 0) return -1;
    char path[PATH_MAX];
    if (build_blob_path(s, id, path, sizeof(path)) != 0) return -1;
    if (unlink(path) == 0) return 1;
    if (errno == ENOENT) return 0;
    return -1;
}

int hl_blob_store_compose_path(HlBlobStore *s, const char *id,
                               char *out, size_t out_cap)
{
    if (!s || !id || !out || out_cap == 0) return -1;
    if (validate_id(id) != 0) return -1;
    return build_blob_path(s, id, out, out_cap);
}

/* ── Enumeration (snapshot semantics) ────────────────────────────── */

typedef struct {
    char    id[HL_BLOB_STORE_ID_BUF_SIZE];
    size_t  size;
    int64_t atime;
    int64_t mtime;
} StoreEntry;

typedef struct {
    StoreEntry  *items;
    size_t       count;
    size_t       capacity;
    HlAllocator *alloc;
} StoreEntries;

static int entries_push(StoreEntries *e, const StoreEntry *item)
{
    if (e->count == e->capacity) {
        size_t old_cap = e->capacity;
        size_t new_cap = old_cap == 0 ? 64 : old_cap * 2;
        if (new_cap > SIZE_MAX / sizeof(StoreEntry)) return -1;
        StoreEntry *grown = hl_alloc_realloc(e->alloc, e->items,
            old_cap * sizeof(StoreEntry),
            new_cap * sizeof(StoreEntry));
        if (!grown) return -1;
        e->items    = grown;
        e->capacity = new_cap;
    }
    e->items[e->count++] = *item;
    return 0;
}

static void entries_free(StoreEntries *e)
{
    if (e->items) hl_alloc_free(e->alloc, e->items,
        e->capacity * sizeof(StoreEntry));
    e->items = NULL;
    e->count = 0;
    e->capacity = 0;
}

static int walk_shard(HlBlobStore *s, const char *shard_path,
                      StoreEntries *e)
{
    (void)s;
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

        StoreEntry item;
        memcpy(item.id, ent->d_name, HL_BLOB_STORE_ID_BUF_SIZE);
        item.size  = (size_t)st.st_size;
        item.atime = (int64_t)st.st_atime;
        item.mtime = (int64_t)st.st_mtime;
        if (entries_push(e, &item) != 0) { rc = -1; break; }
    }
    closedir(d);
    return rc;
}

static int collect_entries(HlBlobStore *s, StoreEntries *e)
{
    e->items    = NULL;
    e->count    = 0;
    e->capacity = 0;
    e->alloc    = s->alloc;

    static const char HEX[] = "0123456789abcdef";
    char shard_path[PATH_MAX];

    if (s->shard_depth >= 2) {
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 16; j++) {
                for (int k = 0; k < 16; k++) {
                    for (int l = 0; l < 16; l++) {
                        if (snprintf(shard_path, sizeof(shard_path),
                                "%s/blobs/%c%c/%c%c",
                                s->root, HEX[i], HEX[j], HEX[k], HEX[l]) >=
                            (int)sizeof(shard_path)) continue;
                        if (walk_shard(s, shard_path, e) != 0) {
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
                        s->root, HEX[i], HEX[j]) >= (int)sizeof(shard_path))
                    continue;
                if (walk_shard(s, shard_path, e) != 0) {
                    entries_free(e);
                    return -1;
                }
            }
        }
    }
    return 0;
}

int hl_blob_store_iter(HlBlobStore *s, HlBlobStoreIterCb cb, void *user)
{
    if (!s || !cb) return -1;
    StoreEntries e;
    if (collect_entries(s, &e) != 0) return -1;
    for (size_t i = 0; i < e.count; i++) {
        if (cb(e.items[i].id, e.items[i].size, user) != 0) break;
    }
    entries_free(&e);
    return 0;
}

uint64_t hl_blob_store_total_size(HlBlobStore *s)
{
    if (!s) return 0;
    StoreEntries e;
    if (collect_entries(s, &e) != 0) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < e.count; i++) total += e.items[i].size;
    entries_free(&e);
    return total;
}

uint64_t hl_blob_store_count(HlBlobStore *s)
{
    if (!s) return 0;
    StoreEntries e;
    if (collect_entries(s, &e) != 0) return 0;
    uint64_t n = e.count;
    entries_free(&e);
    return n;
}

/* ── Cleanup / eviction ──────────────────────────────────────────── */

static int cmp_lru(const void *a, const void *b)
{
    int64_t ta = ((const StoreEntry *)a)->atime;
    int64_t tb = ((const StoreEntry *)b)->atime;
    if (ta < tb) return -1;
    if (ta > tb) return 1;
    return 0;
}

static int cmp_fifo(const void *a, const void *b)
{
    int64_t ta = ((const StoreEntry *)a)->mtime;
    int64_t tb = ((const StoreEntry *)b)->mtime;
    if (ta < tb) return -1;
    if (ta > tb) return 1;
    return 0;
}

int hl_blob_store_cleanup(HlBlobStore *s,
                          const HlBlobStoreCleanupOpts *opts,
                          uint64_t *removed_out, uint64_t *freed_out)
{
    if (removed_out) *removed_out = 0;
    if (freed_out)   *freed_out   = 0;
    if (!s || !opts) return -1;

    StoreEntries e;
    if (collect_entries(s, &e) != 0) return -1;

    qsort(e.items, e.count, sizeof(StoreEntry),
          opts->strategy == HL_BLOB_STORE_FIFO ? cmp_fifo : cmp_lru);

    uint64_t total = 0;
    for (size_t i = 0; i < e.count; i++) total += e.items[i].size;

    time_t now = time(NULL);
    uint64_t removed = 0;
    uint64_t freed   = 0;
    int rc = 0;

    for (size_t i = 0; i < e.count; i++) {
        const StoreEntry *it = &e.items[i];
        int evict = 0;

        if (opts->max_age_sec > 0) {
            int64_t age = (int64_t)now -
                (opts->strategy == HL_BLOB_STORE_FIFO ? it->mtime : it->atime);
            if (age < 0) age = 0;
            if ((uint64_t)age >= opts->max_age_sec) evict = 1;
        }
        if (!evict && opts->max_total_size > 0 && total > opts->max_total_size)
            evict = 1;

        if (!evict) continue;

        if (!opts->dry_run) {
            char path[PATH_MAX];
            if (build_blob_path(s, it->id, path, sizeof(path)) != 0) {
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
