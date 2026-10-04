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

#include "hull/shared/blob_store.h"
#include "hull/shared/fs_util.h"
#include "hull/cap/crypto.h"
#include "hull/utils/alloc.h"
#include "../utils/hex.h"

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

/* O_NOATIME: Linux-only flag that suppresses atime updates on read.
 * glibc only exposes the symbol when _GNU_SOURCE is defined; the
 * rest of the codebase compiles with _DEFAULT_SOURCE only, so on
 * Linux we hardcode the kernel constant directly rather than
 * conditionally widening the feature-test surface for the whole
 * translation unit. Value verified stable across all Linux archs
 * (asm-generic/fcntl.h: 01000000 octal = 0x40000 hex). On macOS,
 * BSDs, and Cosmo there is no equivalent - define to 0 so it's a
 * no-op in the open() flags. */
#if defined(__linux__)
#  ifndef O_NOATIME
#    define O_NOATIME 01000000
#  endif
#else
#  ifndef O_NOATIME
#    define O_NOATIME 0
#  endif
#endif

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
    int          tmp_dirfd;   /* the store's tmp/ (opened without following) */
    char         tmp_name[HL_BLOB_STORE_TMP_NAME_SIZE]; /* "" once unlinked */
    size_t       written;
    HlSha256Ctx  hash;
    char         expected[HL_BLOB_STORE_ID_BUF_SIZE];  /* "" if no expected */
    int          durable;
};

struct HlBlobStoreReader {
    HlBlobStore *store;
    int          fd;
};

/* Hex encoding goes through the shared byte->hex leaf hl_hex_encode
 * (utils/hex.h). The nibble->shard-directory-name table further down is a
 * different shape (path construction, not buffer encoding) and stays local. */

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

/* Every write below the root goes through directory descriptors, each
 * component opened O_NOFOLLOW from the one above it, and the final step is a
 * renameat between the held tmp/ and shard descriptors. The store's threat
 * model is a shared (other-writable) cache root: a symlink planted at tmp/,
 * blobs/ or a shard redirected every write below it, and a path-string check
 * followed by a later rename(path) was check-then-use - a component swapped
 * for a symlink in between was still followed. The root itself is the
 * configured path and may be reached through a symlink (a linked $HOME). */

/* Directory @p name below @p dirfd, made if missing, opened without following
 * a symlink there. -1 (ELOOP / ENOTDIR) when it is not a real directory. */
static int open_subdir(int dirfd, const char *name)
{
    if (mkdirat(dirfd, name, 0755) < 0 && errno != EEXIST) return -1;
    return openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/* <root>/<name>, opened as above. */
static int open_root_subdir(const char *root, const char *name)
{
    int r = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (r < 0) return -1;
    int d = open_subdir(r, name);
    int e = errno;
    close(r);
    errno = e;
    return d;
}

/* The shard directory blob @p id goes into (blobs/ab, or blobs/ab/cd at
 * shard depth 2): each level made if missing and opened without following. */
static int open_shard_dir(const HlBlobStore *s, const char *id)
{
    int fd = open_root_subdir(s->root, "blobs");
    for (int level = 0; fd >= 0 && level < s->shard_depth; level++) {
        char part[3] = { id[level * 2], id[level * 2 + 1], '\0' };
        int next = open_subdir(fd, part);
        int e = errno;
        close(fd);
        errno = e;
        fd = next;
    }
    return fd;
}

/* Cross-filesystem fallback for renameat: copy into a file this call CREATES
 * (O_EXCL, O_NOFOLLOW) in the held shard directory. 0 = copied, 1 = something
 * was already there (the race's winner, same content by contract), -1 =
 * error. */
static int copy_into_place(int tmp_dirfd, const char *tmp_name,
                           int shard_fd, const char *id)
{
    int ofd = openat(shard_fd, id,
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (ofd < 0) return errno == EEXIST ? 1 : -1;
    FILE *dst = fdopen(ofd, "wb");
    if (!dst) { close(ofd); unlinkat(shard_fd, id, 0); return -1; }
    int ifd = openat(tmp_dirfd, tmp_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    FILE *src = ifd >= 0 ? fdopen(ifd, "rb") : NULL;
    if (!src) {
        if (ifd >= 0) close(ifd);
        fclose(dst);
        unlinkat(shard_fd, id, 0);
        return -1;
    }
    char copy_buf[65536];
    size_t n;
    int err = 0;
    while ((n = fread(copy_buf, 1, sizeof(copy_buf), src)) > 0) {
        if (fwrite(copy_buf, 1, n, dst) != n) { err = 1; break; }
    }
    if (ferror(src)) err = 1;
    fclose(src);
    if (fclose(dst) != 0) err = 1;
    if (err) { unlinkat(shard_fd, id, 0); return -1; }
    return 0;
}

/* Move tmp/<tmp_name> into the shard as <id>, relative to the held
 * descriptors, and unlink the temp file whatever happens. 1 = this call put
 * the blob in place, 0 = something was already there (same content by
 * contract), -1 = error. */
static int place_blob(int tmp_dirfd, const char *tmp_name,
                      int shard_fd, const char *id)
{
    struct stat st;
    int rc;
    if (fstatat(shard_fd, id, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        rc = 0;
    } else if (renameat(tmp_dirfd, tmp_name, shard_fd, id) == 0) {
        return 1;
    } else if (errno != EXDEV) {
        rc = -1;
    } else {
        int c = copy_into_place(tmp_dirfd, tmp_name, shard_fd, id);
        rc = c < 0 ? -1 : (c == 0 ? 1 : 0);
    }
    unlinkat(tmp_dirfd, tmp_name, 0);
    return rc;
}

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

static int make_tmp_name(char *out, size_t out_cap)
{
    if (out_cap < HL_BLOB_STORE_TMP_NAME_SIZE) return -1;
    uint8_t rand_bytes[HL_BLOB_STORE_TMP_RAND_BYTES];
    if (hl_cap_crypto_random(rand_bytes, sizeof(rand_bytes)) != 0) return -1;
    char hex[HL_BLOB_STORE_TMP_RAND_HEX + 1];
    hl_hex_encode(rand_bytes, sizeof(rand_bytes), hex, sizeof(hex));
    int n = snprintf(out, out_cap, "%s%s%s",
                     HL_BLOB_STORE_TMP_PREFIX, hex,
                     HL_BLOB_STORE_TMP_SUFFIX);
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void sweep_stale_tmps(const char *root, uint64_t max_age_sec)
{
    if (max_age_sec == UINT64_MAX) return;
    /* tmp/ through a descriptor opened without following, and every stat /
     * unlink relative to it: a symlink at tmp/ pointed this unlink sweep at
     * whatever directory it named. */
    int tfd = open_root_subdir(root, "tmp");
    if (tfd < 0) return;
    DIR *d = fdopendir(tfd);
    if (!d) { close(tfd); return; }

    time_t now = time(NULL);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, HL_BLOB_STORE_TMP_PREFIX,
                    sizeof(HL_BLOB_STORE_TMP_PREFIX) - 1) != 0) continue;

        struct stat st;
        if (fstatat(dirfd(d), ent->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
            continue;

        /* A future mtime is never stale. (now - st_mtime) is signed, and
         * casting a NEGATIVE difference to uint64_t wraps to a huge value
         * that clears any max_age - so a temp file whose mtime sits even
         * one second ahead of time(NULL) was unlinked as ancient. That is
         * the opposite of what this sweep is for: the file it deletes in
         * that case is the freshest one, potentially a concurrent writer's
         * in-flight blob.
         *
         * mtime can lead the clock after an NTP step, or where the
         * filesystem and time(NULL) resolve slightly differently - which is
         * how this surfaced: an intermittent CI failure where the sweep ate
         * the file the test had just created. */
        if (st.st_mtime > now) continue;
        if ((uint64_t)(now - st.st_mtime) >= max_age_sec)
            unlinkat(dirfd(d), ent->d_name, 0);
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
    if (hl_mkdir_p(root, 0755) < 0) goto fail_root;

    if (snprintf(path, sizeof(path), "%s/blobs", root) >=
        (int)sizeof(path)) goto fail_root;
    if (hl_mkdir_p(path, 0755) < 0) goto fail_root;

    if (snprintf(path, sizeof(path), "%s/tmp", root) >=
        (int)sizeof(path)) goto fail_root;
    if (hl_mkdir_p(path, 0755) < 0) goto fail_root;

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

    int tdir = open_root_subdir(s->root, "tmp");
    if (tdir < 0) return -1;
    int fd = openat(tdir, tmp_name,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        close(tdir);
        return -1;
    }

    HlBlobStoreWriter *w = hl_alloc_malloc(s->alloc, sizeof(*w));
    if (!w) {
        close(fd);
        unlinkat(tdir, tmp_name, 0);
        close(tdir);
        return -1;
    }
    w->store     = s;
    w->fd        = fd;
    w->tmp_dirfd = tdir;
    memcpy(w->tmp_name, tmp_name, sizeof(w->tmp_name));
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

/* Unlink the writer's temp file (once). */
static void writer_unlink_tmp(HlBlobStoreWriter *w)
{
    if (w->tmp_name[0]) {
        unlinkat(w->tmp_dirfd, w->tmp_name, 0);
        w->tmp_name[0] = '\0';
    }
}

static void writer_release(HlBlobStoreWriter *w)
{
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    if (w->tmp_dirfd >= 0) close(w->tmp_dirfd);
    hl_alloc_free(w->store->alloc, w, sizeof(*w));
}

int hl_blob_store_writer_finalize(HlBlobStoreWriter *w,
                                  char *out_id, size_t *out_size)
{
    if (!w) return -1;
    if (w->fd < 0) { writer_release(w); return -1; }

    uint8_t digest[32];
    if (hl_cap_crypto_sha256_final(&w->hash, digest) != 0) {
        close(w->fd); w->fd = -1;
        writer_unlink_tmp(w);
        writer_release(w);
        return -1;
    }
    char id[HL_BLOB_STORE_ID_BUF_SIZE];
    hl_hex_encode(digest, 32, id, sizeof(id));

    if (w->durable && fsync(w->fd) < 0) {
        close(w->fd); w->fd = -1;
        writer_unlink_tmp(w);
        writer_release(w);
        return -1;
    }

    if (close(w->fd) < 0) {
        w->fd = -1;
        writer_unlink_tmp(w);
        writer_release(w);
        return -1;
    }
    w->fd = -1;

    if (w->expected[0] != '\0' &&
        memcmp(w->expected, id, HL_BLOB_STORE_ID_HEX_LEN) != 0) {
        writer_unlink_tmp(w);
        writer_release(w);
        return -1;
    }

    int shard_fd = open_shard_dir(w->store, id);
    if (shard_fd < 0) {
        writer_unlink_tmp(w);
        writer_release(w);
        return -1;
    }
    int placed = place_blob(w->tmp_dirfd, w->tmp_name, shard_fd, id);
    w->tmp_name[0] = '\0';   /* place_blob unlinked it */
    if (placed < 0) {
        close(shard_fd);
        writer_release(w);
        return -1;
    }
    if (w->durable && placed == 1)
        (void)fsync(shard_fd);
    close(shard_fd);

    if (out_id) memcpy(out_id, id, HL_BLOB_STORE_ID_BUF_SIZE);
    if (out_size) *out_size = w->written;
    writer_release(w);
    return 0;
}

void hl_blob_store_writer_abort(HlBlobStoreWriter *w)
{
    if (!w) return;
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    writer_unlink_tmp(w);
    writer_release(w);
}

/* ── Buffer put ──────────────────────────────────────────────────── */

/* 1 when the stored blob @p id hashes to @p id, 0 when it does not (or
 * cannot be read). */
static int stored_blob_matches(HlBlobStore *s, const char *id)
{
    HlBlobStoreReader *r = NULL;
    if (hl_blob_store_reader_open(s, id, /*track_access=*/0, &r) != 0)
        return 0;
    HlSha256Ctx h;
    hl_cap_crypto_sha256_init(&h);
    uint8_t chunk[65536];
    int ok = 1;
    for (;;) {
        size_t got = 0;
        if (hl_blob_store_reader_read(r, chunk, sizeof chunk, &got) != 0) { ok = 0; break; }
        if (got == 0) break;
        if (hl_cap_crypto_sha256_update(&h, chunk, got) != 0) { ok = 0; break; }
    }
    hl_blob_store_reader_close(r);
    uint8_t digest[32];
    if (!ok || hl_cap_crypto_sha256_final(&h, digest) != 0) return 0;
    char hex[HL_BLOB_STORE_ID_BUF_SIZE];
    hl_hex_encode(digest, 32, hex, sizeof hex);
    unsigned char diff = 0;
    for (size_t i = 0; i < HL_BLOB_STORE_ID_HEX_LEN; i++)
        diff |= (unsigned char)(hex[i] ^ id[i]);
    return diff == 0;
}

static int store_put_full(HlBlobStore *s, const uint8_t *buf, size_t len,
                          const char *expected, int durable, char *out_id)
{
    if (!s) return -1;
    if (len > 0 && !buf) return -1;

    if (expected && validate_id(expected) == 0) {
        int rc = hl_blob_store_exists(s, expected);
        if (rc == 1) {
            /* Already stored - but the CALLER's bytes were never hashed on
             * this path, and it goes on to use them (hull tools install
             * extracted the downloaded bundle): a swapped download passed
             * as "verified" whenever an earlier install had stored the
             * genuine one. And the stored file itself was trusted on its
             * name alone - a corrupt or planted blob is replaced. */
            uint8_t digest[32];
            char hex[HL_BLOB_STORE_ID_BUF_SIZE];
            if (hl_cap_crypto_sha256(buf ? buf : (const uint8_t *)"", len, digest) != 0)
                return -1;
            hl_hex_encode(digest, 32, hex, sizeof hex);
            unsigned char diff = 0;
            for (size_t i = 0; i < HL_BLOB_STORE_ID_HEX_LEN; i++)
                diff |= (unsigned char)(hex[i] ^ expected[i]);
            if (diff != 0) return -1;
            if (stored_blob_matches(s, expected)) {
                if (out_id) memcpy(out_id, expected, HL_BLOB_STORE_ID_BUF_SIZE);
                return 0;
            }
            (void)hl_blob_store_delete(s, expected);   /* rewritten below */
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

    /* Fast-path: target already present - same key implies same
     * bytes by the cache's design contract. Skip the write. */
    if (hl_blob_store_exists(s, key) == 1) return 0;

    /* Write into the store's tmp/ then atomic-rename into the shard
     * dir under the caller-supplied key. Mirrors writer_open_full +
     * writer_finalize, but without the SHA computation. */
    char tmp_name[HL_BLOB_STORE_TMP_NAME_SIZE];
    if (make_tmp_name(tmp_name, sizeof(tmp_name)) != 0) return -1;

    int tdir = open_root_subdir(s->root, "tmp");
    if (tdir < 0) return -1;
    int fd = openat(tdir, tmp_name,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) { close(tdir); return -1; }

    int rc = 0;
    size_t put = 0;
    while (put < len) {
        ssize_t w = write(fd, bytes + put, len - put);
        if (w < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        if (w == 0) { rc = -1; break; }
        put += (size_t)w;
    }
    if (close(fd) < 0) rc = -1;
    if (rc < 0) {
        unlinkat(tdir, tmp_name, 0);
        close(tdir);
        return -1;
    }

    /* A race that produced the target between exists() and here is
     * accepted (place_blob leaves the winner): same content by contract. */
    int shard_fd = open_shard_dir(s, key);
    if (shard_fd < 0) {
        unlinkat(tdir, tmp_name, 0);
        close(tdir);
        return -1;
    }
    int placed = place_blob(tdir, tmp_name, shard_fd, key);
    close(shard_fd);
    close(tdir);
    return placed < 0 ? -1 : 0;
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
     * one - refuse to read what's at the other end. Matters on
     * multi-user hosts or shared HULL_CACHE_DIR mounts where the
     * cache root might be writable by an unprivileged user.
     * Symlink → ELOOP (Linux) / EMLINK (BSD) → blob_store_get
     * returns failure, caller falls back to fresh compile.
     *
     * O_NOATIME (track_access=0 path only): suppress kernel atime
     * updates so the caller's track_access contract is honored
     * even on filesystems mounted with strictatime, or with
     * relatime when atime happens to be older than ctime/mtime.
     * Without it, the open() itself bumps atime before we get a
     * chance to apply the policy. Linux-only; defined as 0 below
     * for portability so this is a no-op elsewhere. Falls back to
     * a plain open() on EPERM (O_NOATIME requires owner-or-CAP),
     * since lacking permission to suppress atime updates isn't
     * fatal - worst case the policy degrades to "best-effort". */
    /* O_NONBLOCK, then a regular-file check: a FIFO planted at the blob
     * path otherwise blocked this open() - and the event loop - until a
     * writer turned up. Blocking mode is restored for the reads. */
    int open_flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (!track_access) open_flags |= O_NOATIME;
    int fd = open(path, open_flags);
    if (fd < 0 && errno == EPERM && !track_access) {
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    }
    if (fd < 0) return -1;
    struct stat fst;
    if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode)) { close(fd); return -1; }
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) { close(fd); return -1; }

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
 * will materialise. Hostile or corrupted blob entries - e.g. a
 * stat that reports billions of bytes due to filesystem state
 * corruption, or a planted file in a shared HULL_CACHE_DIR -
 * are rejected rather than crashing the allocator. Constant lives
 * in the header (HL_BLOB_STORE_MAX_IN_MEMORY_BYTES); shared with
 * cache verify so both paths bump together. */

int hl_blob_store_get(HlBlobStore *s, const char *id, int track_access,
                      uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;

    /* Sized from the OPENED file: a stat() of the path, before the
     * O_NOFOLLOW open, could see a different file than the one read (a
     * replace in between, in a shared store), and the first `size` bytes of
     * the other file came back as this blob. */
    HlBlobStoreReader *r = NULL;
    if (hl_blob_store_reader_open(s, id, track_access, &r) != 0) return -1;
    struct stat fst;
    if (fstat(r->fd, &fst) != 0 || fst.st_size < 0) {
        hl_blob_store_reader_close(r);
        return -1;
    }
    size_t size = (size_t)fst.st_size;

    if (size > HL_BLOB_STORE_MAX_IN_MEMORY_BYTES) {
        hl_blob_store_reader_close(r);
        errno = EFBIG;
        return -1;
    }

    if (size == 0) {
        hl_blob_store_reader_close(r);
        return 0;
    }

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
    /* And nothing past it (a file that grew while read). */
    uint8_t extra;
    size_t more = 0;
    if (got == size && hl_blob_store_reader_read(r, &extra, 1, &more) == 0 && more)
        got = size + 1;
    hl_blob_store_reader_close(r);

    if (got != size) {
        hl_alloc_free(s->alloc, buf, size);
        return -1;
    }
    *out_buf = buf;
    *out_len = size;
    return 0;
}

int hl_blob_store_get_verified(HlBlobStore *s, const char *id, int track_access,
                               uint8_t **out_buf, size_t *out_len)
{
    if (hl_blob_store_get(s, id, track_access, out_buf, out_len) != 0)
        return -1;
    uint8_t digest[32];
    char hex[HL_BLOB_STORE_ID_BUF_SIZE];
    int ok = hl_cap_crypto_sha256(*out_buf ? *out_buf : (const uint8_t *)"",
                                  *out_len, digest) == 0;
    if (ok) {
        hl_hex_encode(digest, 32, hex, sizeof hex);
        unsigned char diff = 0;
        for (size_t i = 0; i < HL_BLOB_STORE_ID_HEX_LEN; i++)
            diff |= (unsigned char)(hex[i] ^ id[i]);
        ok = diff == 0;
    }
    if (!ok) {
        if (*out_buf) hl_alloc_free(s->alloc, *out_buf, *out_len);
        *out_buf = NULL;
        *out_len = 0;
        return -1;
    }
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
