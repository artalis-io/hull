/*
 * test_blob.c — Content-addressed blob storage tests.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/blob.h"
#include "hull/cap/fs.h"
#include "hull/cap/crypto.h"
#include "hull/alloc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── Test fixture: per-test isolated temp directory + fs_cfg ─────── */

typedef struct {
    char         base_dir[256];
    HlFsConfig   fs_cfg;
    HlAllocator  alloc;
} TestEnv;

static int env_init(TestEnv *e)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    snprintf(e->base_dir, sizeof(e->base_dir),
             "%s/hull-blob-test-XXXXXX", tmp);
    if (!mkdtemp(e->base_dir)) return -1;
    e->fs_cfg.base_dir = e->base_dir;
    e->fs_cfg.base_len = strlen(e->base_dir);
    hl_alloc_init(&e->alloc, 0);
    return 0;
}

/* Recursive rm -rf for cleanup. Best-effort. */
static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
                    continue;
                char child[1024];
                snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                rm_rf(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void env_free(TestEnv *e)
{
    rm_rf(e->base_dir);
}

/* Compute reference SHA-256 hex for a buffer for cross-check. */
static void ref_sha256_hex(const uint8_t *buf, size_t len, char out[65])
{
    static const char HEX[] = "0123456789abcdef";
    uint8_t d[32];
    hl_cap_crypto_sha256(buf, len, d);
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = HEX[(d[i] >> 4) & 0xF];
        out[i * 2 + 1] = HEX[d[i] & 0xF];
    }
    out[64] = '\0';
}

/* ── init / free ─────────────────────────────────────────────────── */

UTEST(hl_cap_blob, init_creates_layout)
{
    TestEnv e;
    ASSERT_EQ(env_init(&e), 0);

    HlBlob *b = NULL;
    ASSERT_EQ(hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "data/blobs", 1, 0), 0);
    ASSERT_TRUE(b != NULL);

    /* blobs/ and tmp/ must exist under <base>/data/blobs/ */
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/data/blobs/blobs", e.base_dir);
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    snprintf(path, sizeof(path), "%s/data/blobs/tmp", e.base_dir);
    ASSERT_EQ(stat(path, &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, init_rejects_traversal)
{
    TestEnv e;
    ASSERT_EQ(env_init(&e), 0);
    HlBlob *b = NULL;
    /* ".." path traversal should be rejected by hl_cap_fs_validate. */
    ASSERT_EQ(hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc,
                                 "../escape", 1, 0), -1);
    ASSERT_TRUE(b == NULL);
    env_free(&e);
}

UTEST(hl_cap_blob, init_sweeps_stale_tmps)
{
    TestEnv e;
    ASSERT_EQ(env_init(&e), 0);

    HlBlob *b = NULL;
    ASSERT_EQ(hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 60), 0);
    hl_cap_blob_free(b);

    /* Drop a stale .tmp file with mtime > 60s ago. */
    char path[512];
    snprintf(path, sizeof(path), "%s/blobs/tmp/.blob-stale.tmp", e.base_dir);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    ASSERT_TRUE(fd >= 0);
    write(fd, "x", 1);
    close(fd);
    struct timeval tv[2] = {{ time(NULL) - 3600, 0 }, { time(NULL) - 3600, 0 }};
    utimes(path, tv);

    /* And a fresh one. */
    char fresh[512];
    snprintf(fresh, sizeof(fresh), "%s/blobs/tmp/.blob-fresh.tmp", e.base_dir);
    fd = open(fresh, O_WRONLY | O_CREAT, 0644);
    ASSERT_TRUE(fd >= 0);
    write(fd, "y", 1);
    close(fd);

    /* Re-init triggers the sweep. */
    ASSERT_EQ(hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 60), 0);
    hl_cap_blob_free(b);

    /* Stale one is gone; fresh one survives. */
    struct stat st;
    ASSERT_TRUE(stat(path, &st) != 0);     /* removed */
    ASSERT_EQ(stat(fresh, &st), 0);        /* preserved */

    env_free(&e);
}

/* ── put / get round-trip ────────────────────────────────────────── */

UTEST(hl_cap_blob, put_get_roundtrip)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    const char *msg = "hello, blob world";
    char id[HL_BLOB_ID_BUF_SIZE];
    ASSERT_EQ(hl_cap_blob_put(b, (const uint8_t *)msg, strlen(msg), NULL, id), 0);

    char expected[65];
    ref_sha256_hex((const uint8_t *)msg, strlen(msg), expected);
    ASSERT_STREQ(id, expected);

    uint8_t *got = NULL;
    size_t got_len = 0;
    ASSERT_EQ(hl_cap_blob_get(b, id, 1, &got, &got_len), 0);
    ASSERT_EQ(got_len, strlen(msg));
    ASSERT_TRUE(memcmp(got, msg, got_len) == 0);
    hl_alloc_free(&e.alloc, got, got_len);

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, put_empty_buffer_is_valid)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    char id[HL_BLOB_ID_BUF_SIZE];
    ASSERT_EQ(hl_cap_blob_put(b, NULL, 0, NULL, id), 0);

    /* SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    ASSERT_STREQ(id,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, put_verified_short_circuits_when_present)
{
    /* When `expected` is supplied and the blob already exists on
     * disk, put skips the tmp-write + hash entirely. Verify by:
     *  (a) writing a blob once
     *  (b) re-putting different bytes claiming the SAME id — the
     *      short-circuit accepts (existing file's SHA is trusted)
     *  (c) reading back: bytes are the original, not the "lie" */
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    const char *original = "the original bytes";
    char id[HL_BLOB_ID_BUF_SIZE];
    ASSERT_EQ(hl_cap_blob_put(b, (const uint8_t *)original,
                                strlen(original), NULL, id), 0);

    /* Now put_verified with DIFFERENT bytes but claim the same id.
     * Short-circuit returns 0 immediately because the id exists. */
    const char *liar = "totally different bytes";
    char id2[HL_BLOB_ID_BUF_SIZE];
    ASSERT_EQ(hl_cap_blob_put(b, (const uint8_t *)liar,
                                strlen(liar), id, id2), 0);
    ASSERT_STREQ(id2, id);

    /* Confirm only ONE blob landed and the original bytes survived. */
    ASSERT_EQ(hl_cap_blob_count(b), (uint64_t)1);
    uint8_t *got = NULL; size_t got_len = 0;
    ASSERT_EQ(hl_cap_blob_get(b, id, 0, &got, &got_len), 0);
    ASSERT_EQ(got_len, strlen(original));
    ASSERT_TRUE(memcmp(got, original, got_len) == 0);
    if (got) hl_alloc_free(&e.alloc, got, got_len);

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, put_durable_round_trips)
{
    /* Durable put hits fsync(fd) + fsync(dirfd). Hard to assert
     * syscalls directly; this just verifies the durable path
     * produces correct output (same SHA + read-back match). */
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    const char *msg = "durable bytes";
    char id[HL_BLOB_ID_BUF_SIZE];
    ASSERT_EQ(hl_cap_blob_put_durable(b, (const uint8_t *)msg,
                                         strlen(msg), NULL, id), 0);

    char expected[65];
    ref_sha256_hex((const uint8_t *)msg, strlen(msg), expected);
    ASSERT_STREQ(id, expected);

    uint8_t *got = NULL; size_t got_len = 0;
    ASSERT_EQ(hl_cap_blob_get(b, id, 0, &got, &got_len), 0);
    ASSERT_EQ(got_len, strlen(msg));
    ASSERT_TRUE(memcmp(got, msg, got_len) == 0);
    if (got) hl_alloc_free(&e.alloc, got, got_len);

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, writer_durable_round_trips)
{
    /* Same correctness check via the streaming writer. */
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    HlBlobWriter *w = NULL;
    ASSERT_EQ(hl_cap_blob_writer_open_durable(b, NULL, &w), 0);
    hl_cap_blob_writer_write(w, (const uint8_t *)"foo", 3);
    hl_cap_blob_writer_write(w, (const uint8_t *)"bar", 3);
    char id[HL_BLOB_ID_BUF_SIZE]; size_t size = 0;
    ASSERT_EQ(hl_cap_blob_writer_finalize(w, id, &size), 0);
    ASSERT_EQ(size, (size_t)6);

    char expected[65];
    ref_sha256_hex((const uint8_t *)"foobar", 6, expected);
    ASSERT_STREQ(id, expected);

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, get_empty_returns_null_buffer)
{
    /* M2 contract: an empty blob is fetched with out_buf=NULL,
     * out_len=0, and no allocation is made — caller can skip the
     * free entirely. */
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    char id[HL_BLOB_ID_BUF_SIZE];
    ASSERT_EQ(hl_cap_blob_put(b, NULL, 0, NULL, id), 0);

    uint8_t *buf = (uint8_t *)0xDEADBEEF;   /* sentinel — must be cleared */
    size_t   len = 999;                      /* sentinel — must be 0 */
    ASSERT_EQ(hl_cap_blob_get(b, id, 0, &buf, &len), 0);
    ASSERT_TRUE(buf == NULL);
    ASSERT_EQ(len, (size_t)0);
    /* Deliberately no hl_alloc_free here — the contract is "no free
     * needed for empty blobs". If this leaks, ASan in `make debug`
     * would catch it. */

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, put_verified_accepts_correct_sha)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    const char *msg = "verify-me";
    char expected[65];
    ref_sha256_hex((const uint8_t *)msg, strlen(msg), expected);

    char id[HL_BLOB_ID_BUF_SIZE];
    ASSERT_EQ(hl_cap_blob_put(b, (const uint8_t *)msg, strlen(msg),
                                expected, id), 0);
    ASSERT_STREQ(id, expected);

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, put_verified_rejects_wrong_sha)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    const char *msg = "verify-me";
    /* A valid hex string but the wrong SHA for these bytes. */
    const char *wrong = "0000000000000000000000000000000000000000000000000000000000000000";

    char id[HL_BLOB_ID_BUF_SIZE];
    ASSERT_EQ(hl_cap_blob_put(b, (const uint8_t *)msg, strlen(msg),
                                wrong, id), -1);

    /* No blob file should remain — tmp was unlinked. */
    char tmp_dir[512];
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/blobs/tmp", e.base_dir);
    DIR *d = opendir(tmp_dir);
    ASSERT_TRUE(d != NULL);
    struct dirent *ent;
    int leaked = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, ".blob-", 6) == 0) leaked = 1;
    }
    closedir(d);
    ASSERT_EQ(leaked, 0);

    hl_cap_blob_free(b);
    env_free(&e);
}

/* ── Streaming write — on-the-fly SHA correctness ────────────────── */

UTEST(hl_cap_blob, streaming_write_hashes_on_the_fly)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    /* Build a 256 KiB payload with varied bytes. */
    size_t total = 256 * 1024;
    uint8_t *payload = malloc(total);
    ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < total; i++) payload[i] = (uint8_t)(i * 7919 + 13);

    HlBlobWriter *w = NULL;
    ASSERT_EQ(hl_cap_blob_writer_open(b, NULL, &w), 0);

    /* Feed in irregular chunk sizes — exercises the streaming loop. */
    size_t off = 0;
    size_t chunks[] = { 1, 64, 65536, 100, 4096, 1, 0, 13, 8192 };
    for (size_t i = 0; off < total; i++) {
        size_t want = chunks[i % (sizeof(chunks)/sizeof(chunks[0]))];
        if (want > total - off) want = total - off;
        ASSERT_EQ(hl_cap_blob_writer_write(w, payload + off, want), 0);
        off += want;
    }

    char id[HL_BLOB_ID_BUF_SIZE]; size_t size = 0;
    ASSERT_EQ(hl_cap_blob_writer_finalize(w, id, &size), 0);
    ASSERT_EQ(size, total);

    /* Cross-check against ref_sha256_hex on the whole payload. */
    char expected[65];
    ref_sha256_hex(payload, total, expected);
    ASSERT_STREQ(id, expected);

    /* Read it back. */
    uint8_t *got = NULL; size_t got_len = 0;
    ASSERT_EQ(hl_cap_blob_get(b, id, 0, &got, &got_len), 0);
    ASSERT_EQ(got_len, total);
    ASSERT_TRUE(memcmp(got, payload, total) == 0);
    hl_alloc_free(&e.alloc, got, got_len);

    free(payload);
    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, writer_abort_removes_tmp)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    HlBlobWriter *w = NULL;
    ASSERT_EQ(hl_cap_blob_writer_open(b, NULL, &w), 0);
    hl_cap_blob_writer_write(w, (const uint8_t *)"abc", 3);
    hl_cap_blob_writer_abort(w);

    /* tmp/ should be empty. */
    char tmp_dir[512];
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/blobs/tmp", e.base_dir);
    DIR *d = opendir(tmp_dir);
    ASSERT_TRUE(d != NULL);
    struct dirent *ent;
    int leaked = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, ".blob-", 6) == 0) leaked = 1;
    }
    closedir(d);
    ASSERT_EQ(leaked, 0);

    hl_cap_blob_free(b);
    env_free(&e);
}

/* ── Idempotent put — concurrent / repeated identical writes ─────── */

UTEST(hl_cap_blob, put_twice_same_bytes_is_idempotent)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    const char *msg = "duplicate";
    char id1[HL_BLOB_ID_BUF_SIZE], id2[HL_BLOB_ID_BUF_SIZE];
    ASSERT_EQ(hl_cap_blob_put(b, (const uint8_t *)msg, 9, NULL, id1), 0);
    ASSERT_EQ(hl_cap_blob_put(b, (const uint8_t *)msg, 9, NULL, id2), 0);
    ASSERT_STREQ(id1, id2);

    /* Only one count, one total_size. */
    ASSERT_EQ(hl_cap_blob_count(b), (uint64_t)1);
    ASSERT_EQ(hl_cap_blob_total_size(b), (uint64_t)9);

    hl_cap_blob_free(b);
    env_free(&e);
}

/* ── Metadata: exists / stat / delete ────────────────────────────── */

UTEST(hl_cap_blob, exists_stat_delete)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    const char *msg = "stat me";
    char id[HL_BLOB_ID_BUF_SIZE];
    hl_cap_blob_put(b, (const uint8_t *)msg, strlen(msg), NULL, id);

    ASSERT_EQ(hl_cap_blob_exists(b, id), 1);
    size_t size = 0; int64_t at = 0;
    ASSERT_EQ(hl_cap_blob_stat(b, id, &size, &at), 0);
    ASSERT_EQ(size, strlen(msg));
    ASSERT_TRUE(at > 0);

    ASSERT_EQ(hl_cap_blob_delete(b, id), 1);
    ASSERT_EQ(hl_cap_blob_exists(b, id), 0);
    ASSERT_EQ(hl_cap_blob_delete(b, id), 0);    /* already gone */

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, rejects_invalid_id)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    /* Too short. */
    ASSERT_EQ(hl_cap_blob_exists(b, "abc"), -1);
    /* Wrong charset. */
    char bad[65];
    memset(bad, 'g', 64); bad[64] = '\0';     /* 'g' is not hex */
    ASSERT_EQ(hl_cap_blob_exists(b, bad), -1);
    /* Uppercase rejected (canonical-lowercase requirement). */
    memset(bad, 'A', 64); bad[64] = '\0';
    ASSERT_EQ(hl_cap_blob_exists(b, bad), -1);

    hl_cap_blob_free(b);
    env_free(&e);
}

/* ── track_access opt-out ────────────────────────────────────────── */

UTEST(hl_cap_blob, track_access_false_preserves_atime)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    const char *msg = "atime test";
    char id[HL_BLOB_ID_BUF_SIZE];
    hl_cap_blob_put(b, (const uint8_t *)msg, strlen(msg), NULL, id);

    /* Backdate the file's atime via utimes(). */
    char path[512];
    snprintf(path, sizeof(path), "%s/blobs/blobs/%c%c/%s",
             e.base_dir, id[0], id[1], id);
    struct timeval old_tv[2] = {{ time(NULL) - 3600, 0 },
                                  { time(NULL) - 3600, 0 }};
    ASSERT_EQ(utimes(path, old_tv), 0);

    int64_t before = 0;
    ASSERT_EQ(hl_cap_blob_stat(b, id, NULL, &before), 0);

    /* Read with track_access=0 — atime must NOT be bumped. */
    uint8_t *got = NULL; size_t got_len = 0;
    ASSERT_EQ(hl_cap_blob_get(b, id, 0, &got, &got_len), 0);
    hl_alloc_free(&e.alloc, got, got_len);

    int64_t after = 0;
    ASSERT_EQ(hl_cap_blob_stat(b, id, NULL, &after), 0);
    /* Some filesystems (relatime, noatime) may already not bump on read.
     * The contract is: track_access=0 must NEVER bump. Tolerate equal,
     * fail if after > before. */
    ASSERT_TRUE(after <= before + 1);

    hl_cap_blob_free(b);
    env_free(&e);
}

/* ── Enumeration (snapshot) ──────────────────────────────────────── */

typedef struct { uint64_t total; int n; } IterAcc;
static int iter_count_cb(const char *id, size_t size, void *user)
{
    (void)id;
    IterAcc *a = user;
    a->n++;
    a->total += size;
    return 0;
}

UTEST(hl_cap_blob, iter_walks_all)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    char id[HL_BLOB_ID_BUF_SIZE];
    hl_cap_blob_put(b, (const uint8_t *)"a", 1, NULL, id);
    hl_cap_blob_put(b, (const uint8_t *)"bb", 2, NULL, id);
    hl_cap_blob_put(b, (const uint8_t *)"ccc", 3, NULL, id);

    IterAcc acc = {0, 0};
    ASSERT_EQ(hl_cap_blob_iter(b, iter_count_cb, &acc), 0);
    ASSERT_EQ(acc.n, 3);
    ASSERT_EQ(acc.total, (uint64_t)6);

    ASSERT_EQ(hl_cap_blob_count(b), (uint64_t)3);
    ASSERT_EQ(hl_cap_blob_total_size(b), (uint64_t)6);

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, iter_handles_empty_store)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    IterAcc acc = {0, 0};
    ASSERT_EQ(hl_cap_blob_iter(b, iter_count_cb, &acc), 0);
    ASSERT_EQ(acc.n, 0);
    ASSERT_EQ(acc.total, (uint64_t)0);

    hl_cap_blob_free(b);
    env_free(&e);
}

/* ── shard_depth = 2 ─────────────────────────────────────────────── */

UTEST(hl_cap_blob, shard_depth_2_layout)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 2, 0);

    const char *msg = "shard depth two test";
    char id[HL_BLOB_ID_BUF_SIZE];
    hl_cap_blob_put(b, (const uint8_t *)msg, strlen(msg), NULL, id);

    /* File should live at blobs/<XX>/<YY>/<id> */
    char path[512];
    snprintf(path, sizeof(path), "%s/blobs/blobs/%c%c/%c%c/%s",
             e.base_dir, id[0], id[1], id[2], id[3], id);
    struct stat st;
    ASSERT_EQ(stat(path, &st), 0);

    /* Round-trip still works. */
    uint8_t *got = NULL; size_t got_len = 0;
    ASSERT_EQ(hl_cap_blob_get(b, id, 0, &got, &got_len), 0);
    ASSERT_EQ(got_len, strlen(msg));
    ASSERT_TRUE(memcmp(got, msg, got_len) == 0);
    hl_alloc_free(&e.alloc, got, got_len);

    hl_cap_blob_free(b);
    env_free(&e);
}

/* ── Cleanup: LRU eviction by max_total_size ─────────────────────── */

UTEST(hl_cap_blob, cleanup_lru_by_total_size)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    /* Put three blobs, backdate their atimes to differ. */
    char id_old[HL_BLOB_ID_BUF_SIZE], id_mid[HL_BLOB_ID_BUF_SIZE], id_new[HL_BLOB_ID_BUF_SIZE];
    hl_cap_blob_put(b, (const uint8_t *)"oldoldold", 9, NULL, id_old);
    hl_cap_blob_put(b, (const uint8_t *)"midmidmid", 9, NULL, id_mid);
    hl_cap_blob_put(b, (const uint8_t *)"newnewnew", 9, NULL, id_new);

    /* Make 'old' atime oldest, 'mid' middle. 'new' stays fresh. */
    char path[512];
    snprintf(path, sizeof(path), "%s/blobs/blobs/%c%c/%s",
             e.base_dir, id_old[0], id_old[1], id_old);
    struct timeval tv_old[2] = {{ time(NULL) - 7200, 0 },
                                  { time(NULL) - 7200, 0 }};
    utimes(path, tv_old);
    snprintf(path, sizeof(path), "%s/blobs/blobs/%c%c/%s",
             e.base_dir, id_mid[0], id_mid[1], id_mid);
    struct timeval tv_mid[2] = {{ time(NULL) - 3600, 0 },
                                  { time(NULL) - 3600, 0 }};
    utimes(path, tv_mid);

    /* Cap to 18 bytes — must evict the 9-byte oldest blob. */
    HlBlobCleanupOpts opts = {
        .max_total_size = 18, .max_age_sec = 0,
        .strategy = HL_BLOB_LRU, .dry_run = 0,
    };
    uint64_t removed = 0, freed = 0;
    ASSERT_EQ(hl_cap_blob_cleanup(b, &opts, &removed, &freed), 0);
    ASSERT_EQ(removed, (uint64_t)1);
    ASSERT_EQ(freed, (uint64_t)9);

    ASSERT_EQ(hl_cap_blob_exists(b, id_old), 0);
    ASSERT_EQ(hl_cap_blob_exists(b, id_mid), 1);
    ASSERT_EQ(hl_cap_blob_exists(b, id_new), 1);

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, cleanup_dry_run_reports_but_does_not_remove)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    char id[HL_BLOB_ID_BUF_SIZE];
    hl_cap_blob_put(b, (const uint8_t *)"aged", 4, NULL, id);

    char path[512];
    snprintf(path, sizeof(path), "%s/blobs/blobs/%c%c/%s",
             e.base_dir, id[0], id[1], id);
    struct timeval old_tv[2] = {{ time(NULL) - 7200, 0 },
                                  { time(NULL) - 7200, 0 }};
    utimes(path, old_tv);

    HlBlobCleanupOpts opts = {
        .max_total_size = 0, .max_age_sec = 3600,
        .strategy = HL_BLOB_LRU, .dry_run = 1,
    };
    uint64_t removed = 0, freed = 0;
    ASSERT_EQ(hl_cap_blob_cleanup(b, &opts, &removed, &freed), 0);
    ASSERT_EQ(removed, (uint64_t)1);
    ASSERT_EQ(freed, (uint64_t)4);

    /* Blob still on disk because dry_run was set. */
    ASSERT_EQ(hl_cap_blob_exists(b, id), 1);

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST(hl_cap_blob, cleanup_age_only)
{
    TestEnv e; env_init(&e);
    HlBlob *b = NULL;
    hl_cap_blob_init(&b, &e.fs_cfg, &e.alloc, "blobs", 1, 0);

    char id_old[HL_BLOB_ID_BUF_SIZE], id_new[HL_BLOB_ID_BUF_SIZE];
    hl_cap_blob_put(b, (const uint8_t *)"old", 3, NULL, id_old);
    hl_cap_blob_put(b, (const uint8_t *)"new", 3, NULL, id_new);

    char path[512];
    snprintf(path, sizeof(path), "%s/blobs/blobs/%c%c/%s",
             e.base_dir, id_old[0], id_old[1], id_old);
    struct timeval old_tv[2] = {{ time(NULL) - 7200, 0 },
                                  { time(NULL) - 7200, 0 }};
    utimes(path, old_tv);

    HlBlobCleanupOpts opts = {
        .max_total_size = 0, .max_age_sec = 3600,
        .strategy = HL_BLOB_LRU, .dry_run = 0,
    };
    uint64_t removed = 0;
    ASSERT_EQ(hl_cap_blob_cleanup(b, &opts, &removed, NULL), 0);
    ASSERT_EQ(removed, (uint64_t)1);
    ASSERT_EQ(hl_cap_blob_exists(b, id_old), 0);
    ASSERT_EQ(hl_cap_blob_exists(b, id_new), 1);

    hl_cap_blob_free(b);
    env_free(&e);
}

UTEST_MAIN()
