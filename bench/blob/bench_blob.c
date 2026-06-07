/*
 * bench_blob.c — Throughput + latency benchmark for hull/blob@1.
 *
 * Measures the cap-layer directly (bypasses Lua/JS binding overhead)
 * across the workload shapes that matter for the real consumers:
 *
 *   - Small-blob put (compute AOT cache: lots of small artifacts)
 *   - Large-blob put (attachment uploads: file-shaped writes)
 *   - Streaming put with small chunks (multipart uploads)
 *   - Buffer-mode get
 *   - Streaming get
 *   - Idempotent-put speedup (second put of same content is dedup'd)
 *   - iter() walk cost at varying N
 *   - cleanup() throughput
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/blob.h"
#include "hull/cap/fs.h"
#include "hull/cap/crypto.h"
#include "hull/alloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ── Wall-clock + reporting helpers ──────────────────────────────── */

static double now_sec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void report(const char *name, size_t ops, size_t bytes, double elapsed)
{
    double ops_per_s = (double)ops / elapsed;
    double mb_per_s  = ((double)bytes / (1024.0 * 1024.0)) / elapsed;
    double us_per_op = (elapsed * 1e6) / (double)ops;
    printf("  %-38s  %7zu ops  %8.1f ms  "
           "%9.0f ops/s  %8.1f MB/s  %7.2f µs/op\n",
           name, ops, elapsed * 1e3, ops_per_s, mb_per_s, us_per_op);
}

/* ── rm -rf helper (best-effort tmpdir cleanup) ─────────────────── */

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

/* ── Benchmark scaffolding ──────────────────────────────────────── */

typedef struct {
    char         base_dir[256];
    HlFsConfig   fs_cfg;
    HlAllocator  alloc;
    HlBlob      *b;
} BenchEnv;

static int env_open(BenchEnv *e)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    snprintf(e->base_dir, sizeof(e->base_dir),
             "%s/hull-bench-blob-XXXXXX", tmp);
    if (!mkdtemp(e->base_dir)) return -1;
    e->fs_cfg.base_dir = e->base_dir;
    e->fs_cfg.base_len = strlen(e->base_dir);
    hl_alloc_init(&e->alloc, 0);
    if (hl_cap_blob_init(&e->b, &e->fs_cfg, &e->alloc, "blobs", 1, 0) != 0)
        return -1;
    return 0;
}

static void env_close(BenchEnv *e)
{
    if (e->b) hl_cap_blob_free(e->b);
    rm_rf(e->base_dir);
}

/* Fill `buf` with deterministic pseudo-random bytes that vary per
 * `seed` — so different put() calls produce distinct SHAs (idempotent
 * put would otherwise inflate the apparent dedup rate and slash
 * iter() counts in half). xorshift32 with a non-collapsing seed
 * mixer: bare `seed | 1` would map 2k and 2k+1 to the same state. */
static void fill_random(uint8_t *buf, size_t len, uint32_t seed)
{
    /* SplitMix-style avalanche so each seed produces a distinct state. */
    uint32_t s = seed * 0x9E3779B1u + 0xBF58476Du;
    s ^= s >> 16;
    s *= 0x85EBCA77u;
    s |= 1u;            /* xorshift requires non-zero */
    for (size_t i = 0; i < len; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        buf[i] = (uint8_t)s;
    }
}

/* ── Bench: buffer-mode put ─────────────────────────────────────── */

static void bench_put(BenchEnv *e, size_t n_blobs, size_t blob_size,
                        char (*ids_out)[HL_BLOB_ID_BUF_SIZE])
{
    uint8_t *buf = malloc(blob_size);
    if (!buf) { fprintf(stderr, "oom\n"); exit(1); }

    double t0 = now_sec();
    for (size_t i = 0; i < n_blobs; i++) {
        fill_random(buf, blob_size, (uint32_t)(i + 1));
        if (hl_cap_blob_put(e->b, buf, blob_size, NULL, ids_out[i]) != 0) {
            fprintf(stderr, "put failed at %zu\n", i);
            exit(1);
        }
    }
    double dt = now_sec() - t0;

    char label[80];
    snprintf(label, sizeof(label), "put buffer (%zu B)", blob_size);
    report(label, n_blobs, n_blobs * blob_size, dt);

    free(buf);
}

/* ── Bench: streaming put with N small chunks ───────────────────── */

static void bench_stream_put(BenchEnv *e, size_t n_blobs, size_t blob_size,
                               size_t chunk_size)
{
    uint8_t *buf = malloc(blob_size);
    if (!buf) { fprintf(stderr, "oom\n"); exit(1); }

    double t0 = now_sec();
    for (size_t i = 0; i < n_blobs; i++) {
        fill_random(buf, blob_size, (uint32_t)(0x80000000u + i));
        HlBlobWriter *w = NULL;
        if (hl_cap_blob_writer_open(e->b, NULL, &w) != 0) exit(1);
        size_t off = 0;
        while (off < blob_size) {
            size_t n = (blob_size - off < chunk_size)
                     ? (blob_size - off) : chunk_size;
            if (hl_cap_blob_writer_write(w, buf + off, n) != 0) exit(1);
            off += n;
        }
        char id[HL_BLOB_ID_BUF_SIZE];
        if (hl_cap_blob_writer_finalize(w, id, NULL) != 0) exit(1);
    }
    double dt = now_sec() - t0;

    char label[80];
    snprintf(label, sizeof(label), "put stream (%zu B / %zu chunks)",
             blob_size, blob_size / chunk_size);
    report(label, n_blobs, n_blobs * blob_size, dt);

    free(buf);
}

/* ── Bench: buffer-mode get ─────────────────────────────────────── */

static void bench_get(BenchEnv *e, size_t n_blobs, size_t blob_size,
                        char (*ids)[HL_BLOB_ID_BUF_SIZE], int track_access)
{
    double t0 = now_sec();
    size_t bytes_read = 0;
    for (size_t i = 0; i < n_blobs; i++) {
        uint8_t *out = NULL; size_t out_len = 0;
        if (hl_cap_blob_get(e->b, ids[i], track_access, &out, &out_len) != 0) {
            fprintf(stderr, "get failed at %zu\n", i);
            exit(1);
        }
        bytes_read += out_len;
        if (out) hl_alloc_free(&e->alloc, out, out_len);
    }
    double dt = now_sec() - t0;

    char label[80];
    snprintf(label, sizeof(label), "get buffer (track_access=%d)", track_access);
    report(label, n_blobs, bytes_read, dt);
    (void)blob_size;
}

/* ── Bench: streaming get ───────────────────────────────────────── */

static void bench_stream_get(BenchEnv *e, size_t n_blobs, size_t blob_size,
                                char (*ids)[HL_BLOB_ID_BUF_SIZE],
                                size_t chunk_size)
{
    uint8_t *buf = malloc(chunk_size);
    if (!buf) { fprintf(stderr, "oom\n"); exit(1); }

    double t0 = now_sec();
    size_t bytes_read = 0;
    for (size_t i = 0; i < n_blobs; i++) {
        HlBlobReader *r = NULL;
        if (hl_cap_blob_reader_open(e->b, ids[i], 0, &r) != 0) exit(1);
        size_t got = 1;
        while (got > 0) {
            if (hl_cap_blob_reader_read(r, buf, chunk_size, &got) != 0) exit(1);
            bytes_read += got;
        }
        hl_cap_blob_reader_close(r);
    }
    double dt = now_sec() - t0;

    char label[80];
    snprintf(label, sizeof(label), "get stream (%zu B / %zu chunks)",
             blob_size, blob_size / chunk_size);
    report(label, n_blobs, bytes_read, dt);

    free(buf);
}

/* ── Bench: idempotent-put speedup ──────────────────────────────── */

static void bench_idempotent_put(BenchEnv *e, size_t n_puts, size_t blob_size)
{
    uint8_t *buf = malloc(blob_size);
    if (!buf) exit(1);
    fill_random(buf, blob_size, 0xC0FFEEu);

    /* First put: creates the blob. */
    char id0[HL_BLOB_ID_BUF_SIZE];
    if (hl_cap_blob_put(e->b, buf, blob_size, NULL, id0) != 0) exit(1);

    /* Subsequent puts: same content → idempotent (tmp written, hash
     * matches, target exists, tmp dropped without rename). */
    double t0 = now_sec();
    for (size_t i = 0; i < n_puts; i++) {
        char id[HL_BLOB_ID_BUF_SIZE];
        if (hl_cap_blob_put(e->b, buf, blob_size, NULL, id) != 0) exit(1);
    }
    double dt = now_sec() - t0;

    char label[80];
    snprintf(label, sizeof(label), "put idempotent same content (%zu B)", blob_size);
    report(label, n_puts, n_puts * blob_size, dt);

    /* put_verified short-circuit: when caller supplies `expected`
     * AND the blob already exists, skip tmp+hash entirely.
     * Two-syscall fast path (validate_id + stat) vs the full write+
     * hash+rename. Should be vastly faster than the idempotent path
     * above. */
    double t1 = now_sec();
    for (size_t i = 0; i < n_puts; i++) {
        char id[HL_BLOB_ID_BUF_SIZE];
        if (hl_cap_blob_put(e->b, buf, blob_size, id0, id) != 0) exit(1);
    }
    double dt1 = now_sec() - t1;

    snprintf(label, sizeof(label), "put_verified short-circuit (%zu B)", blob_size);
    report(label, n_puts, n_puts * blob_size, dt1);

    free(buf);
}

/* ── Bench: durable put (fsync fd + dirfd) ──────────────────────── */

static void bench_durable_put(BenchEnv *e, size_t n_blobs, size_t blob_size)
{
    uint8_t *buf = malloc(blob_size);
    if (!buf) exit(1);

    double t0 = now_sec();
    for (size_t i = 0; i < n_blobs; i++) {
        fill_random(buf, blob_size, (uint32_t)(0x40000000u + i));
        char id[HL_BLOB_ID_BUF_SIZE];
        if (hl_cap_blob_put_durable(e->b, buf, blob_size, NULL, id) != 0)
            exit(1);
    }
    double dt = now_sec() - t0;

    char label[80];
    snprintf(label, sizeof(label),
             "put DURABLE (fsync fd+dir) (%zu B)", blob_size);
    report(label, n_blobs, n_blobs * blob_size, dt);

    free(buf);
}

/* ── Bench: iter walk ───────────────────────────────────────────── */

static int count_cb(const char *id, size_t size, void *user)
{
    (void)id; (void)size;
    (*(size_t *)user)++;
    return 0;
}

static void bench_iter(BenchEnv *e, size_t expected_count)
{
    double t0 = now_sec();
    size_t seen = 0;
    if (hl_cap_blob_iter(e->b, count_cb, &seen) != 0) exit(1);
    double dt = now_sec() - t0;
    if (seen != expected_count) {
        fprintf(stderr, "iter mismatch: expected %zu, got %zu\n",
                expected_count, seen);
    }

    char label[80];
    snprintf(label, sizeof(label), "iter walk (%zu entries)", seen);
    report(label, seen, 0, dt);
}

/* ── Bench: cleanup ─────────────────────────────────────────────── */

static void bench_cleanup(BenchEnv *e, uint64_t max_total_size)
{
    HlBlobCleanupOpts opts = {
        .max_total_size = max_total_size,
        .max_age_sec    = 0,
        .strategy       = HL_BLOB_LRU,
        .dry_run        = 0,
    };
    uint64_t removed = 0, freed = 0;
    double t0 = now_sec();
    if (hl_cap_blob_cleanup(e->b, &opts, &removed, &freed) != 0) exit(1);
    double dt = now_sec() - t0;

    char label[80];
    snprintf(label, sizeof(label), "cleanup LRU (removed %llu, freed %llu B)",
             (unsigned long long)removed, (unsigned long long)freed);
    report(label, removed > 0 ? removed : 1, freed, dt);
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    size_t n_small  = 10000;     /* small-blob workload */
    size_t small_sz = 4096;      /* 4 KiB */
    size_t n_med    = 1000;      /* medium-blob workload */
    size_t med_sz   = 64 * 1024; /* 64 KiB */
    size_t n_large  = 100;       /* large-blob workload */
    size_t large_sz = 4 * 1024 * 1024; /* 4 MiB */

    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--n-small"))       n_small  = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--small-sz")) small_sz = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--n-med"))    n_med    = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--med-sz"))   med_sz   = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--n-large"))  n_large  = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--large-sz")) large_sz = (size_t)atol(argv[++i]);
    }

    printf("\nhull/blob@1 benchmark\n");
    printf("=====================\n\n");

    /* ── Small blobs (compute AOT cache shape) ──────────────────── */
    {
        BenchEnv e = {0};
        if (env_open(&e) != 0) { fprintf(stderr, "env open\n"); return 1; }

        printf("Small blobs (n=%zu, size=%zu B):\n", n_small, small_sz);
        char (*ids)[HL_BLOB_ID_BUF_SIZE] =
            malloc(n_small * sizeof(*ids));
        if (!ids) { env_close(&e); return 1; }

        bench_put(&e, n_small, small_sz, ids);
        bench_get(&e, n_small, small_sz, ids, /*track_access=*/0);
        bench_get(&e, n_small, small_sz, ids, /*track_access=*/1);
        bench_iter(&e, n_small);
        bench_cleanup(&e, 0);  /* no eviction; just snapshot cost */
        printf("\n");

        free(ids);
        env_close(&e);
    }

    /* ── Medium blobs (attachment upload shape, mid-size) ───────── */
    {
        BenchEnv e = {0};
        if (env_open(&e) != 0) return 1;

        printf("Medium blobs (n=%zu, size=%zu B):\n", n_med, med_sz);
        char (*ids)[HL_BLOB_ID_BUF_SIZE] =
            malloc(n_med * sizeof(*ids));
        if (!ids) { env_close(&e); return 1; }

        bench_put(&e, n_med, med_sz, ids);
        bench_stream_put(&e, n_med, med_sz, 4096);
        bench_get(&e, n_med, med_sz, ids, 0);
        bench_stream_get(&e, n_med, med_sz, ids, 4096);
        bench_idempotent_put(&e, n_med, med_sz);
        bench_durable_put(&e, n_med, med_sz);
        printf("\n");

        free(ids);
        env_close(&e);
    }

    /* ── Large blobs (file-upload shape) ─────────────────────────── */
    {
        BenchEnv e = {0};
        if (env_open(&e) != 0) return 1;

        printf("Large blobs (n=%zu, size=%zu B):\n", n_large, large_sz);
        char (*ids)[HL_BLOB_ID_BUF_SIZE] =
            malloc(n_large * sizeof(*ids));
        if (!ids) { env_close(&e); return 1; }

        bench_put(&e, n_large, large_sz, ids);
        bench_stream_put(&e, n_large, large_sz, 65536);
        bench_get(&e, n_large, large_sz, ids, 0);
        bench_stream_get(&e, n_large, large_sz, ids, 65536);
        printf("\n");

        free(ids);
        env_close(&e);
    }

    /* ── Iter scaling: how does it grow with N? ──────────────────── */
    {
        BenchEnv e = {0};
        if (env_open(&e) != 0) return 1;

        printf("Iter scaling (varying N at 1 KiB each):\n");
        size_t scales[] = { 100, 1000, 10000, 100000 };
        size_t blob_sz = 1024;
        char (*ids)[HL_BLOB_ID_BUF_SIZE] =
            malloc(scales[3] * sizeof(*ids));
        if (!ids) { env_close(&e); return 1; }

        size_t current = 0;
        for (size_t s = 0; s < sizeof(scales)/sizeof(scales[0]); s++) {
            size_t target = scales[s];
            uint8_t *buf = malloc(blob_sz);
            for (; current < target; current++) {
                fill_random(buf, blob_sz, (uint32_t)(0xDEAD0000u + current));
                if (hl_cap_blob_put(e.b, buf, blob_sz, NULL,
                                       ids[current]) != 0) exit(1);
            }
            free(buf);
            bench_iter(&e, current);
        }

        free(ids);
        env_close(&e);
    }

    return 0;
}
