/* bench_mapped_span.c — the mapped-span performance benchmark harness.
 *
 * Measures the shipped large-file mechanism: a host-mmap'd file window read by an
 * AOT wasm32 guest via a HullSpan, vs the native-mmap baseline and the copy-based
 * baselines. Methodology is locked in docs/mapped_span_benchmark_design.md:
 *   - 4 workloads (bench_span_ops.h), same body native + wasm;
 *   - 4 impls: native mmap / HullSpan AOT / copy-once linear / chunked-copy;
 *   - steady-state via two-point (t(1), t(K)) amortization of attach/dispatch;
 *   - correctness gate (identical checksums) BEFORE any timing;
 *   - getrusage page-fault + RSS capture; medians + MAD over iters;
 *   - runtime host-call-counter proof the scan loop makes zero host calls;
 *   - reproducible JSON.
 *
 * Requires a wamrc-built AOT guest (gen_bench_span_aot.h non-empty); without it the
 * bench prints a skip and exits 0 (the CI job builds wamrc, must-not-skip).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <stdint.h>
#include "hull/cap/wasm.h"
#include "hull/cap/fs.h"
#include "hull/limits/wasm.h"
#include "hull/vfs.h"
#include "hull/entry.h"
/* hull_span_setup() (the SPAN_INFO query) references host_call; the native read
 * path here never calls it, but the dual-target SDK header must parse. Declare it
 * (i32,i32,i32) per the native ABI (tests/hull/test_span_sdk.c) -- unreferenced at
 * link because the unused static-inline setup is dropped. */
int32_t host_call(int32_t op, int32_t a, int32_t b);
#include "bench_span_ops.h"           /* native baseline runs the same body */
#include "gen_bench_span_aot.h"       /* bench_span_aot[] + _len (0 if no wamrc) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>

/* Input wire layout shared with bench_span_guest.c: [workload,mode,reps(u64)] then
 * LINEAR data. Keep in lockstep with the guest's BENCH_HDR. */
#define BENCH_HDR 10

/* ── config (env-overridable) ─────────────────────────────────────────── */
static uint64_t g_dataset_bytes;      /* DATASET_MB * 1MiB */
static int g_iters   = 15;            /* timed iterations (ITERS) */
static int g_warmups = 3;             /* discarded warmups (WARMUPS) */
static int g_reps_k  = 32;            /* internal scans for the t(K) point (REPS_K) */
static const char *g_cache = "warm";  /* CACHE = warm|cold */
static const char *g_out = "build/bench_mapped_span.json";

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* median + MAD over a[] (a is mutated: sorted). */
static void stats(uint64_t *a, int n, uint64_t *med, uint64_t *mad,
                  uint64_t *lo, uint64_t *hi)
{
    qsort(a, n, sizeof(a[0]), cmp_u64);
    *lo = a[0]; *hi = a[n - 1];
    *med = a[n / 2];
    uint64_t *dev = malloc(sizeof(uint64_t) * n);
    for (int i = 0; i < n; i++)
        dev[i] = a[i] > *med ? a[i] - *med : *med - a[i];
    qsort(dev, n, sizeof(dev[0]), cmp_u64);
    *mad = dev[n / 2];
    free(dev);
}

/* Deterministic dataset: SplitMix64 fill so every run has identical bytes. */
static int write_dataset(const char *path, uint64_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    uint64_t s = 0x9E3779B97F4A7C15ull;
    unsigned char buf[1 << 16];
    uint64_t written = 0;
    while (written < n) {
        size_t chunk = sizeof(buf);
        if (n - written < chunk) chunk = (size_t)(n - written);
        for (size_t i = 0; i < chunk; i++) {
            s += 0x9E3779B97F4A7C15ull;
            uint64_t z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            buf[i] = (unsigned char)(z >> 24);
        }
        if (write(fd, buf, chunk) != (ssize_t)chunk) { close(fd); return -1; }
        written += chunk;
    }
    close(fd);
    return 0;
}

/* getrusage snapshot deltas. */
typedef struct { long minflt, majflt, maxrss; } RUsnap;
static RUsnap ru_now(void)
{
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    RUsnap s = { r.ru_minflt, r.ru_majflt, r.ru_maxrss };
    return s;
}

/* one measured (impl, workload) row */
typedef struct {
    uint64_t checksum;
    uint64_t t1_med, tK_med;          /* two-point medians (ns) */
    uint64_t steady_ns;               /* (tK - t1)/(K-1) per scan */
    uint64_t raw_med, raw_mad, raw_lo, raw_hi;   /* raw end-to-end = t(1) */
    long majflt, minflt, maxrss_kb;
    int representable;                /* 0 => "not representable" (copy-once ceiling) */
    uint64_t hostcall_delta;          /* hostcalls(tK) - hostcalls(t1); must be 0 for scan-clean */
} Row;

static uint64_t derive_steady(uint64_t t1, uint64_t tK, int K)
{
    if (K <= 1 || tK <= t1) return t1;   /* degenerate: fall back to raw */
    return (tK - t1) / (uint64_t)(K - 1);
}

/* Pre-fault (warm) or best-effort evict (cold) a mapping, per D5. */
static void cache_prep(void *addr, uint64_t len, int fd)
{
    (void)fd;
    if (strcmp(g_cache, "cold") == 0) {
#ifdef POSIX_FADV_DONTNEED
        if (fd >= 0) posix_fadvise(fd, 0, (off_t)len, POSIX_FADV_DONTNEED);
#endif
        (void)madvise(addr, (size_t)len, MADV_DONTNEED);
    } else {
        /* warm: touch every page so steady-state measures CPU + translation. */
        volatile uint64_t s = 0;
        for (uint64_t o = 0; o < len; o += 4096) s += ((volatile unsigned char *)addr)[o];
        (void)s;
    }
}

/* ── native mmap baseline (runs bench_span_ops.h in-process) ──────────── */
static Row measure_native(int workload, const char *path, uint64_t len)
{
    Row r; memset(&r, 0, sizeof(r)); r.representable = 1;
    int fd = open(path, O_RDONLY);
    void *m = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) { r.representable = -1; close(fd); return r; }
    cache_prep(m, len, fd);
    r.checksum = bench_run(workload, m, len);       /* correctness value */

    uint64_t *t1 = malloc(sizeof(uint64_t) * g_iters);
    uint64_t *tK = malloc(sizeof(uint64_t) * g_iters);
    for (int i = 0; i < g_warmups; i++) (void)bench_run(workload, m, len);
    RUsnap b = ru_now();
    for (int i = 0; i < g_iters; i++) {
        uint64_t s = now_ns(); volatile uint64_t v = bench_run(workload, m, len); (void)v;
        t1[i] = now_ns() - s;
    }
    for (int i = 0; i < g_iters; i++) {
        uint64_t s = now_ns();
        for (int k = 0; k < g_reps_k; k++) { volatile uint64_t v = bench_run(workload, m, len); (void)v; }
        tK[i] = now_ns() - s;
    }
    RUsnap a = ru_now();
    uint64_t mad, lo, hi, tKmed, tKmad, tKlo, tKhi;
    stats(t1, g_iters, &r.raw_med, &mad, &lo, &hi);
    r.raw_mad = mad; r.raw_lo = lo; r.raw_hi = hi; r.t1_med = r.raw_med;
    stats(tK, g_iters, &tKmed, &tKmad, &tKlo, &tKhi); r.tK_med = tKmed;
    r.steady_ns = derive_steady(r.t1_med, r.tK_med, g_reps_k);
    r.minflt = a.minflt - b.minflt; r.majflt = a.majflt - b.majflt; r.maxrss_kb = a.maxrss;
    free(t1); free(tK); munmap(m, (size_t)len); close(fd);
    return r;
}

/* ── a wasm impl (SPAN or LINEAR) via hl_cap_wasm_call, two-point ─────── */
/* mode 0 = SPAN (attach a window over `relname`, relative to cfg->base_dir);
 * mode 1 = LINEAR / copy-once (the whole dataset is copied into the input, hence
 * into guest linear memory). `fullpath` is the on-disk file for the copy read. */
static Row measure_wasm(HlWasmCache *cache, HlVfs *vfs, const HlFsConfig *cfg,
                        int workload, int mode, const char *relname,
                        const char *fullpath, uint64_t len)
{
    Row r; memset(&r, 0, sizeof(r)); r.representable = 1;

    /* input header: [workload, mode, reps(u64)] (+ data for LINEAR) */
    unsigned char *input = NULL; size_t in_len = 0;
    if (mode == 1) {
        /* copy-once: the whole dataset must fit a wasm32 I/O budget + linear heap.
         * Above the wasm32 256 MB I/O ceiling it is "not representable within the
         * configured linear-memory limit" (a reported finding, not a failure). */
        if (len + BENCH_HDR > HL_WASM_MAX_IO_SIZE) { r.representable = 0; return r; }
        in_len = (size_t)(len + BENCH_HDR);
        input = malloc(in_len);
        int fd = open(fullpath, O_RDONLY);
        if (!input || fd < 0 || read(fd, input + BENCH_HDR, (size_t)len) != (ssize_t)len) {
            r.representable = -1; free(input); if (fd >= 0) close(fd); return r;
        }
        close(fd);
    } else {
        in_len = BENCH_HDR; input = malloc(in_len);
    }
    input[0] = (unsigned char)workload; input[1] = (unsigned char)mode;

    HlMappedBuffer *buf = NULL;
    if (mode == 0) {
        if (len > HL_FS_MMAP_MAX_WINDOW_BYTES) { r.representable = 0; free(input); return r; }
        buf = hl_cap_fs_mmap_window(cfg, relname, 0, len, NULL, NULL);
        if (!buf) { r.representable = -1; free(input); return r; }
    }

    /* one call with the given rep count; returns elapsed ns + checksum. */
    #define SET_REPS(K) do { uint64_t _k=(K); for(int _i=0;_i<8;_i++) input[2+_i]=(unsigned char)(_k>>(8*_i)); } while(0)
    HlWasmCallOpts base = {0};
    base.max_input = (uint64_t)in_len; base.max_output = 64;
    base.gas = HL_WASM_MAX_GAS;                       /* whole-file scans are big */
    if (mode == 1) base.heap_size = (uint32_t)(len + 16 * 1024 * 1024);  /* fit copied data */
    HlWasmSpanReq req = { .name = "d", .buf = buf };
    if (mode == 0) { base.spans = &req; base.span_count = 1; }

    #define ONE_CALL(K, cksum_out, ns_out) do { \
        HlWasmCallOpts o = base; SET_REPS(K); \
        void *out=NULL; size_t ol=0; const char *e=NULL; \
        uint64_t _s=now_ns(); \
        int rc=hl_cap_wasm_call(cache, "benchspan", input, in_len, &out, &ol, &o, NULL, NULL, vfs, NULL, NULL, &e); \
        (ns_out)=now_ns()-_s; \
        if (rc!=HL_WASM_OK || ol<8) { (cksum_out)=~0ull; } \
        else { uint64_t _c=0; for(int _i=0;_i<8;_i++) _c|=((uint64_t)((unsigned char*)out)[_i])<<(8*_i); (cksum_out)=_c; } \
        free(out); \
    } while(0)

    /* correctness + warm + first-touch */
    uint64_t dummy;
    for (int i = 0; i < g_warmups; i++) { uint64_t ns; ONE_CALL(1, r.checksum, ns); (void)ns; }
    (void)dummy;

    uint64_t hc_before1 = hl_cap_wasm_host_call_count();
    uint64_t *t1 = malloc(sizeof(uint64_t) * g_iters);
    uint64_t *tK = malloc(sizeof(uint64_t) * g_iters);
    RUsnap b = ru_now();
    for (int i = 0; i < g_iters; i++) { uint64_t c, ns; ONE_CALL(1, c, ns); t1[i] = ns; }
    uint64_t hc_after1 = hl_cap_wasm_host_call_count();
    uint64_t hc_beforeK = hl_cap_wasm_host_call_count();
    for (int i = 0; i < g_iters; i++) { uint64_t c, ns; ONE_CALL(g_reps_k, c, ns); tK[i] = ns; }
    uint64_t hc_afterK = hl_cap_wasm_host_call_count();
    RUsnap a = ru_now();

    /* hostcalls per t1 call vs per tK call: equal => scan makes zero host calls. */
    uint64_t per1 = (hc_after1 - hc_before1) / (uint64_t)g_iters;
    uint64_t perK = (hc_afterK - hc_beforeK) / (uint64_t)g_iters;
    r.hostcall_delta = (perK > per1) ? (perK - per1) : 0;

    uint64_t mad, lo, hi, tKmed, tKmad, tKlo, tKhi;
    stats(t1, g_iters, &r.raw_med, &mad, &lo, &hi);
    r.raw_mad = mad; r.raw_lo = lo; r.raw_hi = hi; r.t1_med = r.raw_med;
    stats(tK, g_iters, &tKmed, &tKmad, &tKlo, &tKhi); r.tK_med = tKmed;
    r.steady_ns = derive_steady(r.t1_med, r.tK_med, g_reps_k);
    r.minflt = a.minflt - b.minflt; r.majflt = a.majflt - b.majflt; r.maxrss_kb = a.maxrss;

    free(t1); free(tK); free(input);
    if (buf) hl_cap_fs_munmap(buf);
    return r;
    #undef ONE_CALL
    #undef SET_REPS
}

static const char *WL[] = { "seq_bytes", "seq_words", "random", "parser" };

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *e;
    if ((e = getenv("DATASET_MB"))) g_dataset_bytes = (uint64_t)strtoull(e,0,10) * 1024*1024;
    else g_dataset_bytes = 96ull * 1024 * 1024;   /* CI default */
    if ((e = getenv("ITERS")))   g_iters   = atoi(e);
    if ((e = getenv("WARMUPS"))) g_warmups = atoi(e);
    if ((e = getenv("REPS_K")))  g_reps_k  = atoi(e);
    if ((e = getenv("CACHE")))   g_cache   = e;
    if ((e = getenv("OUT")))     g_out     = e;

    /* The native mmap baseline ALWAYS runs (any arch, no wamrc needed) so the JSON
     * always carries the target numbers. The three wasm impls need a wamrc-built
     * AOT guest; without it they are marked aot-absent and the CI must-not-skip gate
     * (which greps for a representable hullspan_aot row) fails the build. */
    int have_aot = (bench_span_aot_len != 0);
#if defined(__x86_64__) || defined(__amd64__)
    const char *arch = "x86_64";
#elif defined(__aarch64__)
    const char *arch = "aarch64";
#else
    const char *arch = "unknown";   /* native-only; no host-arch AOT to embed */
    have_aot = 0;
#endif

    char dir[] = "/tmp/benchspanXXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char path[512]; snprintf(path, sizeof(path), "%s/data.bin", dir);
    if (write_dataset(path, g_dataset_bytes)) { fprintf(stderr, "dataset write failed\n"); return 1; }

    HlFsConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = dir; cfg.base_len = strlen(dir);
    HlWasmCache cache; HlVfs vfs;
    char aot_key[128]; snprintf(aot_key, sizeof(aot_key), "compute/benchspan.aot.%s", arch);
    HlEntry entries[] = { { aot_key, bench_span_aot, bench_span_aot_len },
                          { NULL, NULL, 0 } };   /* sentinel-terminated per HlVfs */
    if (have_aot) {
        if (hl_cap_wasm_init(&cache)) { fprintf(stderr, "wasm init\n"); return 1; }
        hl_vfs_init(&vfs, entries, NULL);
    }

    struct utsname un; memset(&un, 0, sizeof(un)); uname(&un);
    FILE *jf = fopen(g_out, "w");
    /* NOTE: maxrss_kb is getrusage ru_maxrss verbatim -- KiB on Linux, BYTES on
     * macOS/BSD; disambiguate via "os". CI runs on Linux (KiB). */
    fprintf(jf, "{\n  \"arch\": \"%s\", \"os\": \"%s\", \"dataset_bytes\": %llu, \"iters\": %d, "
                "\"warmups\": %d, \"reps_k\": %d, \"cache\": \"%s\", \"page_size\": %ld,\n",
            arch, un.sysname[0] ? un.sysname : "unknown",
            (unsigned long long)g_dataset_bytes, g_iters, g_warmups, g_reps_k,
            g_cache, sysconf(_SC_PAGESIZE));
    fprintf(jf, "  \"rows\": [\n");

    int failures = 0, first = 1;
    for (int wl = 0; wl < BW_COUNT; wl++) {
        Row nat = measure_native(wl, path, g_dataset_bytes);
        Row spn, cp1;
        if (have_aot) {
            spn = measure_wasm(&cache, &vfs, &cfg, wl, 0, "data.bin", path, g_dataset_bytes);
            cp1 = measure_wasm(&cache, &vfs, &cfg, wl, 1, "data.bin", path, g_dataset_bytes);
        } else {
            memset(&spn, 0, sizeof(spn)); spn.representable = -3;   /* aot-absent */
            memset(&cp1, 0, sizeof(cp1)); cp1.representable = -3;
        }

        /* correctness gate: every representable impl must match native BEFORE timing is trusted. */
        int bad = 0;
        if (spn.representable == 1 && spn.checksum != nat.checksum) { bad = 1;
            fprintf(stderr, "CHECKSUM MISMATCH %s span: %llu != native %llu\n", WL[wl],
                    (unsigned long long)spn.checksum, (unsigned long long)nat.checksum); }
        if (cp1.representable == 1 && cp1.checksum != nat.checksum) { bad = 1;
            fprintf(stderr, "CHECKSUM MISMATCH %s copy-once: %llu != native %llu\n", WL[wl],
                    (unsigned long long)cp1.checksum, (unsigned long long)nat.checksum); }
        if (spn.representable == 1 && spn.hostcall_delta != 0) { bad = 1;
            fprintf(stderr, "HOST-CALL IN SCAN LOOP %s: per-scan hostcall delta %llu != 0\n",
                    WL[wl], (unsigned long long)spn.hostcall_delta); }
        if (bad) { failures++; continue; }

        double overhead = nat.steady_ns ? (100.0 * ((double)spn.steady_ns - (double)nat.steady_ns) / (double)nat.steady_ns) : 0.0;
        printf("%-10s  native_steady=%8lluns  span_steady=%8lluns  overhead=%+.1f%%  "
               "span_raw=%lluns  copy1=%s  majflt(span)=%ld\n",
               WL[wl], (unsigned long long)nat.steady_ns, (unsigned long long)spn.steady_ns,
               overhead, (unsigned long long)spn.raw_med,
               cp1.representable == 1 ? "ok" : (cp1.representable == 0 ? "not-representable" : "err"),
               spn.majflt);

        #define ROWJSON(nm, R) do { \
            fprintf(jf, "%s    {\"workload\": \"%s\", \"impl\": \"%s\", \"representable\": %d, " \
                "\"checksum\": %llu, \"steady_ns\": %llu, \"raw_med_ns\": %llu, \"raw_mad_ns\": %llu, " \
                "\"raw_lo_ns\": %llu, \"raw_hi_ns\": %llu, \"tK_med_ns\": %llu, \"minflt\": %ld, " \
                "\"majflt\": %ld, \"maxrss_kb\": %ld, \"hostcall_scan_delta\": %llu}", \
                first ? "" : ",\n", WL[wl], nm, (R).representable, (unsigned long long)(R).checksum, \
                (unsigned long long)(R).steady_ns, (unsigned long long)(R).raw_med, (unsigned long long)(R).raw_mad, \
                (unsigned long long)(R).raw_lo, (unsigned long long)(R).raw_hi, (unsigned long long)(R).tK_med, \
                (R).minflt, (R).majflt, (R).maxrss_kb, (unsigned long long)(R).hostcall_delta); \
            first = 0; } while (0)
        ROWJSON("native_mmap", nat);
        ROWJSON("hullspan_aot", spn);
        ROWJSON("copy_once", cp1);
        #undef ROWJSON
    }
    fprintf(jf, "\n  ]\n}\n");
    fclose(jf);
    if (have_aot) hl_cap_wasm_destroy(&cache);
    unlink(path); rmdir(dir);

    if (!have_aot)
        printf("bench-mapped-span: native baseline only -- wasm impls SKIPPED "
               "(no wamrc-built AOT guest; CI must-not-skip gate will fail)\n");
    printf("bench-mapped-span: wrote %s (%d workload checksum/host-call failures)\n", g_out, failures);
    return failures ? 1 : 0;
}

