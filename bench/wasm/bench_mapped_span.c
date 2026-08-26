/* bench_mapped_span.c - the mapped-span performance benchmark harness.
 *
 * Measures the shipped large-file mechanism: a host-mmap'd file window read by a
 * wasm32 guest via a HullSpan, vs the native-mmap baseline and the two copy-based
 * baselines. Methodology is locked in docs/mapped_span_benchmark_design.md:
 *   - 4 workloads (bench_span_ops.h), same body native + wasm;
 *   - 4 impls: native mmap / HullSpan / copy-once linear / chunked-copy. For
 *     random, chunked is a bounded ONE-PAGE-cache reader that (re)loads the page
 *     containing each scattered offset -- representable but thrashing (the useful
 *     comparison), with load/hit/bytes-copied metrics;
 *   - WARM steady-state via a setup-only control: t(1 scan) - t(0 scans), which
 *     stays under WAMR's INT_MAX instruction-gas ceiling at any dataset size;
 *   - correctness gate (identical checksums across all four impls) BEFORE timing;
 *   - getrusage page-fault + RSS capture; medians + MAD over iters;
 *   - runtime host-call-counter proof the scan loop makes zero host calls;
 *   - reproducible JSON. (#339 is warm-only; cold/RSS validation is a follow-up.)
 *
 * Two embedded guests: the committed .wasm (interpreter fallback, always present,
 * so the wasm impls + correctness gate run WITHOUT wamrc) and the wamrc-built .aot
 * (preferred; the perf comparand). The JSON "engine" field is "aot" or "interp";
 * CI builds wamrc and asserts engine=aot (must-not-skip).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <stdint.h>
#include "hull/cap/wasm.h"
#include "hull/cap/fs.h"
#include "hull/cap/fs_policy.h"
#include "hull/utils/alloc.h"
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
#include "gen_bench_span_wasm.h"      /* bench_span_wasm[] + _len (interpreter fallback) */

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
static const char *g_cache = "warm";  /* #339 is warm-only; cold is a tracked follow-up */
static const char *g_out = "build/bench_mapped_span.json";

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Host-call counter accessor: real when this bench's private cap_wasm object is
 * compiled with HL_WASM_HOST_CALL_COUNTER (always, for the bench target); a 0 stub
 * otherwise, so this source stays buildable without the flag. */
#ifdef HL_WASM_HOST_CALL_COUNTER
static inline uint64_t hl_wasm_hostcalls(void) { return hl_cap_wasm_host_call_count(); }
#else
static inline uint64_t hl_wasm_hostcalls(void) { return 0; }
#endif

/* Fail-fast allocation for this dev benchmark: an OOM NULL becomes a clean
 * diagnostic exit instead of a silent deref crash (and xrealloc doesn't leak the
 * old block on failure). Not production code; a benchmark that can't allocate its
 * timing arrays has nothing useful to measure. */
static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "bench-mapped-span: out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}
static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) { free(p); fprintf(stderr, "bench-mapped-span: out of memory (%zu bytes)\n", n); exit(1); }
    return q;
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
    uint64_t *dev = xmalloc(sizeof(uint64_t) * n);
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

/* one measured (impl, workload) row.
 *
 * Steady-state via a SETUP-ONLY CONTROL (D3's sanctioned alternative to the K-rep
 * two-point): steady = t(1 scan) - t(0 scans). Each timed call runs AT MOST ONE
 * whole-file scan, so it never approaches WAMR's INT_MAX instruction-gas ceiling
 * (a 64-128 MiB scan x a large internal rep count would). t(0 scans) captures the
 * fixed per-call cost (span attach + dispatch + teardown, or per-chunk dispatch);
 * subtracting it leaves the marginal scan. */
typedef struct {
    uint64_t checksum;
    uint64_t scan_med, setup_med;     /* median t(1 scan), median t(0 scans) */
    uint64_t steady_ns;               /* scan_med - setup_med = one marginal scan */
    uint64_t raw_med, raw_mad, raw_lo, raw_hi;   /* raw end-to-end = t(1 scan) */
    long majflt, minflt, maxrss_kb;
    int representable;                /* 0 => "not representable" (copy-once ceiling); -5 gas-limited */
    uint64_t hostcall_delta;          /* hostcalls(1 scan) - hostcalls(0 scans); must be 0 */
    /* chunked-random bounded-reader metrics (0 for other rows): */
    uint64_t chunk_loads, cache_hits, bytes_copied;
} Row;

static uint64_t derive_steady(uint64_t scan, uint64_t setup)
{
    return (scan > setup) ? (scan - setup) : scan;   /* degenerate: fall back to raw */
}

/* Pre-fault a mapping so the timed WARM scan measures CPU + address translation,
 * not first-touch page faults. #339 is scoped to WARM steady-state only: a real
 * per-iteration COLD protocol (evict both the native AND the span mapping every
 * iteration, with verified major-fault evidence) is a separate effort tracked in
 * the follow-up, since the previous "cold" path faulted pages back in before
 * timing and only touched the native mapping -- it never measured cold. */
static void prefault_warm(void *addr, uint64_t len)
{
    volatile uint64_t s = 0;
    for (uint64_t o = 0; o < len; o += 4096) s += ((volatile unsigned char *)addr)[o];
    (void)s;
}

/* ── native mmap baseline (runs bench_span_ops.h in-process) ──────────── */
static Row measure_native(int workload, const char *path, uint64_t len)
{
    Row r; memset(&r, 0, sizeof(r)); r.representable = 1;
    int fd = open(path, O_RDONLY);
    void *m = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) { r.representable = -1; close(fd); return r; }
    prefault_warm(m, len);
    r.checksum = bench_run(workload, m, len);       /* correctness value */

    /* native has no dispatch, so t(0 scans) is ~0; steady == one in-process scan. */
    uint64_t *scan = xmalloc(sizeof(uint64_t) * g_iters);
    for (int i = 0; i < g_warmups; i++) (void)bench_run(workload, m, len);
    RUsnap b = ru_now();
    for (int i = 0; i < g_iters; i++) {
        uint64_t s = now_ns(); volatile uint64_t v = bench_run(workload, m, len); (void)v;
        scan[i] = now_ns() - s;
    }
    RUsnap a = ru_now();
    uint64_t mad, lo, hi;
    stats(scan, g_iters, &r.raw_med, &mad, &lo, &hi);
    r.raw_mad = mad; r.raw_lo = lo; r.raw_hi = hi;
    r.scan_med = r.raw_med; r.setup_med = 0;
    r.steady_ns = r.scan_med;
    r.minflt = a.minflt - b.minflt; r.majflt = a.majflt - b.majflt; r.maxrss_kb = a.maxrss;
    free(scan); munmap(m, (size_t)len); close(fd);
    return r;
}

/* ── a wasm impl (SPAN or LINEAR) via hl_cap_wasm_call, setup-control ─── */
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
        input = xmalloc(in_len);          /* xmalloc never returns NULL */
        int fd = open(fullpath, O_RDONLY);
        if (fd < 0 || read(fd, input + BENCH_HDR, (size_t)len) != (ssize_t)len) {
            r.representable = -1; free(input); if (fd >= 0) close(fd); return r;
        }
        close(fd);
    } else {
        in_len = BENCH_HDR; input = xmalloc(in_len);
    }
    input[0] = (unsigned char)workload; input[1] = (unsigned char)mode;

    HlMappedBuffer *buf = NULL;
    if (mode == 0) {
        if (len > HL_FS_MMAP_MAX_WINDOW_BYTES) { r.representable = 0; free(input); return r; }
        buf = hl_cap_fs_mmap_window(cfg, relname, 0, len, NULL, NULL);
        if (!buf) { r.representable = -1; free(input); return r; }
    }

    /* one call with the given rep count; returns elapsed ns + checksum + rc. */
    #define SET_REPS(K) do { uint64_t _k=(K); for(int _i=0;_i<8;_i++) input[2+_i]=(unsigned char)(_k>>(8*_i)); } while(0)
    HlWasmCallOpts base = {0};
    base.max_input = (uint64_t)in_len; base.max_output = 64;
    /* Just under WAMR's INT_MAX instruction-gas ceiling (no clamp warning). One
     * whole-file scan of a 64-128 MiB dataset is well under this; a much larger
     * dataset (e.g. the 1 GiB manual job) can exceed it -- handled as a reported
     * finding (representable=-5), not a crash. */
    base.gas = 2000000000;
    if (mode == 1) base.heap_size = (uint32_t)(len + 16 * 1024 * 1024);  /* fit copied data */
    HlWasmSpanReq req = { .name = "d", .buf = buf };
    if (mode == 0) { base.spans = &req; base.span_count = 1; }

    int last_rc = HL_WASM_OK;
    #define ONE_CALL(K, cksum_out, ns_out) do { \
        HlWasmCallOpts o = base; SET_REPS(K); \
        void *out=NULL; size_t ol=0; const char *e=NULL; \
        uint64_t _s=now_ns(); \
        int rc=hl_cap_wasm_call(cache, "benchspan", input, in_len, &out, &ol, &o, NULL, NULL, vfs, NULL, NULL, &e); \
        (ns_out)=now_ns()-_s; last_rc=rc; \
        if (rc!=HL_WASM_OK || ol<8) { (cksum_out)=~0ull; } \
        else { uint64_t _c=0; for(int _i=0;_i<8;_i++) _c|=((uint64_t)((unsigned char*)out)[_i])<<(8*_i); (cksum_out)=_c; } \
        free(out); \
    } while(0)

    /* probe one full scan: a GAS failure here is the per-call instruction ceiling
     * (a whole-file scan too big to meter), reported as a finding, not a crash. */
    { uint64_t ns; ONE_CALL(1, r.checksum, ns); (void)ns; }
    if (last_rc == HL_WASM_ERR_GAS) { r.representable = -5; free(input); if (buf) hl_cap_fs_munmap(buf); return r; }
    if (last_rc != HL_WASM_OK)      { r.representable = -1; free(input); if (buf) hl_cap_fs_munmap(buf); return r; }

    /* warm + first-touch + re-capture the checksum (1 scan). */
    for (int i = 0; i < g_warmups; i++) { uint64_t ns; ONE_CALL(1, r.checksum, ns); (void)ns; }

    /* setup-only control: t(0 scans) = attach + dispatch + teardown, no scan;
     * t(1 scan) adds exactly one whole-file scan. steady = scan - setup. */
    uint64_t hc_b0 = hl_wasm_hostcalls();
    uint64_t *t0 = xmalloc(sizeof(uint64_t) * g_iters);
    uint64_t *t1 = xmalloc(sizeof(uint64_t) * g_iters);
    RUsnap b = ru_now();
    for (int i = 0; i < g_iters; i++) { uint64_t c, ns; ONE_CALL(0, c, ns); t0[i] = ns; }
    uint64_t hc_a0 = hl_wasm_hostcalls();
    uint64_t hc_b1 = hl_wasm_hostcalls();
    for (int i = 0; i < g_iters; i++) { uint64_t c, ns; ONE_CALL(1, c, ns); t1[i] = ns; }
    uint64_t hc_a1 = hl_wasm_hostcalls();
    RUsnap a = ru_now();

    /* hostcalls per 0-scan call vs per 1-scan call: equal => the scan makes zero
     * host calls (only the per-call SPAN_INFO setup does, in both). */
    uint64_t per0 = (hc_a0 - hc_b0) / (uint64_t)g_iters;
    uint64_t per1 = (hc_a1 - hc_b1) / (uint64_t)g_iters;
    r.hostcall_delta = (per1 > per0) ? (per1 - per0) : 0;

    uint64_t mad, lo, hi, s0med, s0mad, s0lo, s0hi;
    stats(t1, g_iters, &r.raw_med, &mad, &lo, &hi);
    r.raw_mad = mad; r.raw_lo = lo; r.raw_hi = hi; r.scan_med = r.raw_med;
    stats(t0, g_iters, &s0med, &s0mad, &s0lo, &s0hi); r.setup_med = s0med;
    r.steady_ns = derive_steady(r.scan_med, r.setup_med);
    r.minflt = a.minflt - b.minflt; r.majflt = a.majflt - b.majflt; r.maxrss_kb = a.maxrss;

    free(t0); free(t1); free(input);
    if (buf) hl_cap_fs_munmap(buf);
    return r;
    #undef ONE_CALL
    #undef SET_REPS
}

/* ── chunked-copy: process the file through a FIXED bounded linear-memory buffer,
 * the host re-filling native->wasm per chunk (guest LINEAR mode, one scan per
 * chunk). Genuinely bounded memory (heap == one chunk), so it is representable
 * even for files far above the copy-once wasm32 ceiling. Checksum-DECOMPOSABLE
 * (sum of per-chunk bench_run == whole-file bench_run) for:
 *   - seq_bytes: any cut (byte sum is associative);
 *   - seq_words: 8-aligned cuts (u32 stride 4 + u64 stride 8 never straddle);
 *   - parser:    record-aligned cuts (a chunk holds whole records only).
 * RANDOM is deliberately NOT chunk-decomposable: a random walk touches offsets
 * across the whole file, so it cannot be served from a chunk already discarded --
 * that impossibility is exactly the property a mapped span provides and a chunked
 * copy cannot. Reported representable=-4 (not-applicable), a finding, not a bug. */
#define BENCH_CHUNK (8u * 1024 * 1024)

typedef struct { uint64_t off, clen; } Chunk;

/* Fill ch[] with the workload's chunk cuts over [0,len); returns chunk count
 * (<= *cap; grows ch via realloc through the caller). */
static int chunk_plan(int workload, const unsigned char *src, uint64_t len,
                      Chunk **out)
{
    Chunk *ch = NULL; int n = 0, cap = 0;
    #define PUSH(o,l) do { if (n==cap){ int _nc = cap ? cap*2 : 64; \
                               if (_nc <= cap) { fprintf(stderr, "bench-mapped-span: chunk count overflow\n"); exit(1); } \
                               cap=_nc; ch=xrealloc(ch,(size_t)cap*sizeof(Chunk)); } \
                           ch[n].off=(o); ch[n].clen=(l); n++; } while(0)
    if (workload == BW_PARSER) {
        /* replicate bench_run's parser walk EXACTLY to find record boundaries */
        uint64_t o = 0, cstart = 0;
        while (o + 4 <= len) {
            uint32_t raw = (uint32_t)src[o] | ((uint32_t)src[o+1]<<8)
                         | ((uint32_t)src[o+2]<<16) | ((uint32_t)src[o+3]<<24);
            uint64_t rec_end = o + 4 + ((uint64_t)(raw & 63u) + 1u);
            if (rec_end > len || rec_end < o) break;          /* truncated tail: stop (as bench_run) */
            if (rec_end - cstart > BENCH_CHUNK && o > cstart) { /* cut BEFORE this record */
                PUSH(cstart, o - cstart); cstart = o;
            }
            o = rec_end;
        }
        if (o > cstart) PUSH(cstart, o - cstart);             /* final chunk of whole records */
    } else {
        uint64_t step = (workload == BW_SEQ_WORDS) ? (BENCH_CHUNK & ~7u) : BENCH_CHUNK;
        for (uint64_t o = 0; o < len; o += step) {
            uint64_t l = (len - o < step) ? (len - o) : step;
            PUSH(o, l);
        }
    }
    #undef PUSH
    *out = ch;
    return n;
}

static Row measure_chunked(HlWasmCache *cache, HlVfs *vfs,
                           int workload, const char *fullpath, uint64_t len)
{
    Row r; memset(&r, 0, sizeof(r));
    if (workload == BW_RANDOM) { r.representable = -4; return r; }  /* N/A: not decomposable */
    r.representable = 1;

    int fd = open(fullpath, O_RDONLY);
    if (fd < 0) { r.representable = -1; return r; }
    unsigned char *src = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (src == MAP_FAILED) { r.representable = -1; close(fd); return r; }

    Chunk *ch = NULL; int nch = chunk_plan(workload, src, len, &ch);

    /* input = header + up to one chunk (parser chunks <= BENCH_CHUNK + one record). */
    size_t cap = (size_t)BENCH_CHUNK + 128;
    unsigned char *input = xmalloc(cap + BENCH_HDR);
    input[0] = (unsigned char)workload; input[1] = 1;   /* LINEAR */

    HlWasmCallOpts o = {0};
    o.max_input = cap + BENCH_HDR; o.max_output = 64; o.gas = 2000000000;  /* < INT_MAX; 8 MiB chunk fits easily */
    o.heap_size = (uint32_t)(cap + 4 * 1024 * 1024);     /* BOUNDED: one chunk, not the file */

    /* ONE_PASS(reps): feed every chunk once (LINEAR, `reps` scans each), accumulate
     * the per-chunk checksums. reps=0 = the setup-only control (per-chunk dispatch,
     * no scan); reps=1 = one full-file pass. */
    #define ONE_PASS(reps, cksum_out) do { \
        for (int _r = 0; _r < 8; _r++) input[2+_r] = (unsigned char)((uint64_t)(reps) >> (8*_r)); \
        uint64_t _acc = 0; int _ok = 1; \
        for (int _c = 0; _c < nch; _c++) { \
            memcpy(input + BENCH_HDR, src + ch[_c].off, (size_t)ch[_c].clen); \
            size_t _il = BENCH_HDR + (size_t)ch[_c].clen; \
            void *_out=NULL; size_t _ol=0; const char *_e=NULL; \
            int _rc = hl_cap_wasm_call(cache, "benchspan", input, _il, &_out, &_ol, &o, NULL, NULL, vfs, NULL, NULL, &_e); \
            if (_rc != HL_WASM_OK || _ol < 8) { _ok = 0; free(_out); break; } \
            uint64_t _v=0; for (int _i=0;_i<8;_i++) _v |= ((uint64_t)((unsigned char*)_out)[_i])<<(8*_i); \
            _acc += _v; free(_out); \
        } \
        (cksum_out) = _ok ? _acc : ~0ull; \
    } while (0)

    for (int i = 0; i < g_warmups; i++) { uint64_t c; ONE_PASS(1, c); }
    ONE_PASS(1, r.checksum);                             /* correctness value */

    uint64_t hc_b0 = hl_wasm_hostcalls();
    uint64_t *t0 = xmalloc(sizeof(uint64_t) * g_iters);
    uint64_t *t1 = xmalloc(sizeof(uint64_t) * g_iters);
    RUsnap b = ru_now();
    for (int i = 0; i < g_iters; i++) { uint64_t c; uint64_t s = now_ns(); ONE_PASS(0, c); t0[i] = now_ns() - s; }
    uint64_t hc_a0 = hl_wasm_hostcalls();
    uint64_t hc_b1 = hl_wasm_hostcalls();
    for (int i = 0; i < g_iters; i++) { uint64_t c; uint64_t s = now_ns(); ONE_PASS(1, c); t1[i] = now_ns() - s; }
    uint64_t hc_a1 = hl_wasm_hostcalls();
    RUsnap a = ru_now();
    uint64_t per0 = (hc_a0 - hc_b0) / (uint64_t)g_iters;
    uint64_t per1 = (hc_a1 - hc_b1) / (uint64_t)g_iters;
    r.hostcall_delta = (per1 > per0) ? (per1 - per0) : 0;   /* LINEAR makes zero host calls */

    uint64_t mad, lo, hi, s0med, s0mad, s0lo, s0hi;
    stats(t1, g_iters, &r.raw_med, &mad, &lo, &hi);
    r.raw_mad = mad; r.raw_lo = lo; r.raw_hi = hi; r.scan_med = r.raw_med;
    stats(t0, g_iters, &s0med, &s0mad, &s0lo, &s0hi); r.setup_med = s0med;
    r.steady_ns = derive_steady(r.scan_med, r.setup_med);
    r.minflt = a.minflt - b.minflt; r.majflt = a.majflt - b.majflt; r.maxrss_kb = a.maxrss;

    free(t0); free(t1); free(input); free(ch);
    munmap(src, (size_t)len); close(fd);
    return r;
    #undef ONE_PASS
}

/* ── chunked-random: a bounded reader that serves the fixed-LCG random offsets
 * from a ONE-PAGE (4 KiB) cache -- the honest "page in the chunk containing each
 * scattered offset" model. It IS representable (a whole-file copy at scale is
 * not), it just THRASHES: a random walk almost never re-hits the cached page, so
 * it (re)loads ~4 KiB per 4-byte read (bytes_copied >> bytes_used) -- the useful
 * comparison vs a span that reads 4 bytes in place. No wasm dispatch: scattered
 * access is not a guest scan; the read is host-side and its checksum matches
 * native by construction (same seed/LCG/offsets/u32le assembly). */
#define BENCH_RPAGE 4096u

static uint64_t chunked_random_pass(const unsigned char *src, uint64_t len, uint64_t span,
                                    uint64_t n, unsigned char *page,
                                    uint64_t *loads, uint64_t *hits, uint64_t *bytes)
{
    int64_t cur = -1;
    uint64_t x = BENCH_RANDOM_SEED, sum = 0;
    *loads = *hits = *bytes = 0;
    for (uint64_t i = 0; i < n; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;   /* same LCG as bench_run */
        uint64_t off = (x >> 11) % span;
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {                              /* le u32 at off, byte by byte */
            uint64_t o = off + (uint64_t)k;
            int64_t pg = (int64_t)(o >> 12);                       /* page index (/4096) */
            if (pg != cur) {                                       /* miss: (re)load the page */
                uint64_t base = (uint64_t)pg << 12;
                uint64_t clen = (len - base < BENCH_RPAGE) ? (len - base) : BENCH_RPAGE;
                memcpy(page, src + base, (size_t)clen);
                cur = pg; (*loads)++; *bytes += clen;
            } else (*hits)++;
            v |= (uint32_t)page[o & 4095u] << (8 * k);
        }
        sum += v;
    }
    return sum;
}

static Row measure_chunked_random(const char *fullpath, uint64_t len)
{
    Row r; memset(&r, 0, sizeof(r)); r.representable = 1;
    int fd = open(fullpath, O_RDONLY);
    if (fd < 0) { r.representable = -1; return r; }
    unsigned char *src = mmap(NULL, (size_t)len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (src == MAP_FAILED) { r.representable = -1; close(fd); return r; }

    uint64_t span = (len >= 4) ? (len - 3) : 1;
    uint64_t n = len / 64; if (n == 0) n = 1;
    if (n > BENCH_RANDOM_MAX_READS) n = BENCH_RANDOM_MAX_READS;   /* == bench_random_n */
    unsigned char *page = xmalloc(BENCH_RPAGE);

    uint64_t loads, hits, bytes;
    for (int i = 0; i < g_warmups; i++) (void)chunked_random_pass(src, len, span, n, page, &loads, &hits, &bytes);
    r.checksum = chunked_random_pass(src, len, span, n, page, &loads, &hits, &bytes);
    r.chunk_loads = loads; r.cache_hits = hits; r.bytes_copied = bytes;

    uint64_t *t = xmalloc(sizeof(uint64_t) * g_iters);
    RUsnap b = ru_now();
    for (int i = 0; i < g_iters; i++) {
        uint64_t s = now_ns();
        (void)chunked_random_pass(src, len, span, n, page, &loads, &hits, &bytes);
        t[i] = now_ns() - s;
    }
    RUsnap a = ru_now();
    uint64_t mad, lo, hi;
    stats(t, g_iters, &r.raw_med, &mad, &lo, &hi);
    r.raw_mad = mad; r.raw_lo = lo; r.raw_hi = hi;
    r.scan_med = r.raw_med; r.setup_med = 0; r.steady_ns = r.scan_med;   /* host-side: no dispatch to subtract */
    r.minflt = a.minflt - b.minflt; r.majflt = a.majflt - b.majflt; r.maxrss_kb = a.maxrss;

    free(t); free(page); munmap(src, (size_t)len); close(fd);
    return r;
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
    if ((e = getenv("OUT")))     g_out     = e;
    /* Clamp to sane floors: a bad env value (ITERS=0/negative) would otherwise
     * make stats() read a[-1]/a[0] of an empty array and size the timing mallocs
     * from a negative int -> huge size_t. g_iters must be >= 1; warmups >= 0. */
    if (g_iters   < 1) g_iters   = 1;
    if (g_warmups < 0) g_warmups = 0;

    /* Two embedded guests. The committed .wasm (interpreter fallback) is ALWAYS
     * present, so the wasm impls + the correctness gate run without wamrc (e.g.
     * locally). The wamrc-built .aot is the PREFERRED engine and the perf
     * comparand; hl_cap_wasm_call picks it over the .wasm when the .aot VFS entry
     * exists. engine == "aot" iff the AOT was embedded. The native baseline always
     * runs. CI asserts engine == "aot" (must-not-skip). */
    int have_aot = (bench_span_aot_len != 0);
#if defined(__x86_64__) || defined(__amd64__)
    const char *arch = "x86_64";
#elif defined(__aarch64__)
    const char *arch = "aarch64";
#else
    const char *arch = "unknown";   /* native-only; no host-arch AOT to embed */
    have_aot = 0;
#endif
    const char *engine = have_aot ? "aot" : "interp";

    char dir[] = "/tmp/benchspanXXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char path[512]; snprintf(path, sizeof(path), "%s/data.bin", dir);
    if (write_dataset(path, g_dataset_bytes)) { fprintf(stderr, "dataset write failed\n"); return 1; }

    HlFsConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.base_dir = dir; cfg.base_len = strlen(dir);
    /* mmap is policy-gated. This bench mmaps data.bin (read) under
     * `dir`, so grant the whole dir with a base-root "." read grant. */
    HlAllocator fs_alloc; hl_alloc_init(&fs_alloc, 0);
    HlFsPolicy fs_policy = HL_FS_POLICY_INIT;
    const char *rd_grant[] = { "." };
    const char *fperr = NULL;
    if (hl_fs_policy_compile_manifest(dir, &fs_alloc, rd_grant, 1, NULL, 0,
                                      &fs_policy, &fperr) != 0) {
        fprintf(stderr, "fs policy compile failed: %s\n", fperr ? fperr : "?");
        return 1;
    }
    cfg.policy = &fs_policy;
    HlWasmCache cache; HlVfs vfs;
    /* VFS entries MUST be sorted by name (binary search). "...aot..." < "...wasm..."
     * (a < w), so the AOT entry precedes the .wasm entry when both are present. */
    char aot_key[128]; snprintf(aot_key, sizeof(aot_key), "compute/benchspan.aot.%s", arch);
    HlEntry with_aot[] = { { aot_key, bench_span_aot, bench_span_aot_len },
                           { "compute/benchspan.wasm", bench_span_wasm, bench_span_wasm_len },
                           { NULL, NULL, 0 } };
    HlEntry interp_only[] = { { "compute/benchspan.wasm", bench_span_wasm, bench_span_wasm_len },
                              { NULL, NULL, 0 } };
    if (hl_cap_wasm_init(&cache)) { fprintf(stderr, "wasm init\n"); return 1; }
    hl_vfs_init(&vfs, have_aot ? with_aot : interp_only, NULL);

    struct utsname un; memset(&un, 0, sizeof(un)); uname(&un);
    FILE *jf = fopen(g_out, "w");
    /* NOTE: maxrss_kb is getrusage ru_maxrss verbatim -- KiB on Linux, BYTES on
     * macOS/BSD; disambiguate via "os". CI runs on Linux (KiB). */
    fprintf(jf, "{\n  \"arch\": \"%s\", \"os\": \"%s\", \"engine\": \"%s\", \"method\": \"setup-control\", "
                "\"dataset_bytes\": %llu, \"iters\": %d, \"warmups\": %d, \"cache\": \"%s\", \"page_size\": %ld,\n",
            arch, un.sysname[0] ? un.sysname : "unknown", engine,
            (unsigned long long)g_dataset_bytes, g_iters, g_warmups,
            g_cache, sysconf(_SC_PAGESIZE));
    fprintf(jf, "  \"rows\": [\n");

    int failures = 0, first = 1;
    for (int wl = 0; wl < BW_COUNT; wl++) {
        Row nat = measure_native(wl, path, g_dataset_bytes);
        Row spn = measure_wasm(&cache, &vfs, &cfg, wl, 0, "data.bin", path, g_dataset_bytes);
        Row cp1 = measure_wasm(&cache, &vfs, &cfg, wl, 1, "data.bin", path, g_dataset_bytes);
        Row chk = (wl == BW_RANDOM) ? measure_chunked_random(path, g_dataset_bytes)
                                    : measure_chunked(&cache, &vfs, wl, path, g_dataset_bytes);

        /* correctness gate: every REPRESENTABLE impl must match native BEFORE any
         * timing is trusted. Non-representable rows (copy-once above the wasm32
         * ceiling; chunked-random N/A) are excluded, not compared. */
        int bad = 0;
        #define CKSUM_GATE(R, label) do { \
            if ((R).representable == 1 && (R).checksum != nat.checksum) { bad = 1; \
                fprintf(stderr, "CHECKSUM MISMATCH %s %s: %llu != native %llu\n", WL[wl], label, \
                        (unsigned long long)(R).checksum, (unsigned long long)nat.checksum); } \
        } while (0)
        CKSUM_GATE(spn, "span"); CKSUM_GATE(cp1, "copy-once"); CKSUM_GATE(chk, "chunked");
        #undef CKSUM_GATE
        if (spn.representable == 1 && spn.hostcall_delta != 0) { bad = 1;
            fprintf(stderr, "HOST-CALL IN SCAN LOOP %s: per-scan hostcall delta %llu != 0\n",
                    WL[wl], (unsigned long long)spn.hostcall_delta); }
        if (bad) { failures++; continue; }

        double overhead = (spn.representable == 1 && nat.steady_ns)
            ? (100.0 * ((double)spn.steady_ns - (double)nat.steady_ns) / (double)nat.steady_ns) : 0.0;
        const char *chunk_s = (chk.representable == 1)
            ? (wl == BW_RANDOM ? "ok(thrash)" : "ok")
            : (chk.representable == 0 ? "not-representable" : "err");
        const char *copy_s = cp1.representable == 1 ? "ok"
            : (cp1.representable == 0 ? "not-representable" : (cp1.representable == -5 ? "gas-limited" : "err"));
        if (spn.representable == 1)
            printf("%-10s  native=%8lluns  span=%8lluns  overhead=%+.1f%%  copy1=%-17s chunked=%s\n",
                   WL[wl], (unsigned long long)nat.steady_ns, (unsigned long long)spn.steady_ns, overhead,
                   copy_s, chunk_s);
        else
            printf("%-10s  native=%8lluns  span=%-14s copy1=%-17s chunked=%s\n",
                   WL[wl], (unsigned long long)nat.steady_ns,
                   spn.representable == -5 ? "gas-limited" : "err", copy_s, chunk_s);

        #define ROWJSON(nm, R) do { \
            fprintf(jf, "%s    {\"workload\": \"%s\", \"impl\": \"%s\", \"representable\": %d, " \
                "\"checksum\": %llu, \"steady_ns\": %llu, \"raw_med_ns\": %llu, \"raw_mad_ns\": %llu, " \
                "\"raw_lo_ns\": %llu, \"raw_hi_ns\": %llu, \"setup_ns\": %llu, \"minflt\": %ld, " \
                "\"majflt\": %ld, \"maxrss_kb\": %ld, \"hostcall_scan_delta\": %llu, " \
                "\"chunk_loads\": %llu, \"cache_hits\": %llu, \"bytes_copied\": %llu}", \
                first ? "" : ",\n", WL[wl], nm, (R).representable, (unsigned long long)(R).checksum, \
                (unsigned long long)(R).steady_ns, (unsigned long long)(R).raw_med, (unsigned long long)(R).raw_mad, \
                (unsigned long long)(R).raw_lo, (unsigned long long)(R).raw_hi, (unsigned long long)(R).setup_med, \
                (R).minflt, (R).majflt, (R).maxrss_kb, (unsigned long long)(R).hostcall_delta, \
                (unsigned long long)(R).chunk_loads, (unsigned long long)(R).cache_hits, (unsigned long long)(R).bytes_copied); \
            first = 0; } while (0)
        ROWJSON("native_mmap", nat);
        ROWJSON("hullspan_aot", spn);
        ROWJSON("copy_once", cp1);
        ROWJSON("chunked_copy", chk);
        #undef ROWJSON
    }
    fprintf(jf, "\n  ]\n}\n");
    fclose(jf);
    hl_cap_wasm_destroy(&cache);
    unlink(path); rmdir(dir);

    printf("bench-mapped-span: engine=%s, wrote %s (%d workload checksum/host-call failures)\n",
           engine, g_out, failures);
    if (!have_aot)
        printf("bench-mapped-span: NOTE engine=interp (no wamrc AOT) -- correctness validated, "
               "but the CI must-not-skip gate requires engine=aot for the perf comparand\n");
    return failures ? 1 : 0;
}

