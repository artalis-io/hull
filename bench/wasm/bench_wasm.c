/*
 * bench_wasm.c — WASM compute benchmark: WAMR interpreter vs AOT vs native C
 *
 * Compares hull_process execution via:
 *   1. Native C (direct function call)
 *   2. WAMR classic interpreter (.wasm)
 *   3. WAMR AOT (.aot pre-compiled, if available)
 *
 * Four workloads:
 *   - compute_hash: compute-intensive hash compression
 *   - mem_histogram: memory-intensive counting sort
 *   - simd_dot_product: SIMD128 f32 dot product (scalar vs SIMD)
 *   - simd_matmul: SIMD128 f32 matrix multiply (scalar vs SIMD)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"
#include "hull/cap/wasm_buffer.h"
#include "hull/limits.h"
#include "hull/vfs.h"
#include "hull/entry.h"
#include "compute_hash_native.h"
#include "mem_histogram_native.h"
#include "simd_dot_product_native.h"
#include "simd_matmul_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ── Configuration ─────────────────────────────────────────────────── */

#define DEFAULT_ITERS   1000
#define WARMUP_ITERS    10
#define MAX_OUTPUT      (16 * 1024 * 1024)

/* ── Timing ────────────────────────────────────────────────────────── */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Statistics ────────────────────────────────────────────────────── */

typedef struct {
    double mean;
    double median;
    double min;
    double max;
    double stddev;
} Stats;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static Stats compute_stats(uint64_t *samples, int n)
{
    Stats s = {0};
    if (n == 0) return s;

    qsort(samples, (size_t)n, sizeof(uint64_t), cmp_u64);

    s.min = (double)samples[0];
    s.max = (double)samples[n - 1];
    s.median = (double)samples[n / 2];

    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += (double)samples[i];
    s.mean = sum / n;

    double var = 0;
    for (int i = 0; i < n; i++) {
        double d = (double)samples[i] - s.mean;
        var += d * d;
    }
    s.stddev = sqrt(var / n);

    return s;
}

static void print_stats(const char *label, Stats *s)
{
    printf("  %-12s mean=%.1fus  median=%.1fus  min=%.1fus  max=%.1fus  stddev=%.1fus\n",
           label,
           s->mean / 1000.0, s->median / 1000.0,
           s->min / 1000.0, s->max / 1000.0, s->stddev / 1000.0);
}

/* ── Input generation (deterministic xorshift32) ──────────────────── */

static void generate_input(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t x = seed;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)(x & 0xFF);
    }
}

/* ── Benchmark runners ─────────────────────────────────────────────── */

typedef int32_t (*NativeFn)(const void *, size_t, void *, size_t);

static void bench_native(const char *name, NativeFn fn,
                         const uint8_t *input, size_t input_len,
                         int iters, uint64_t *samples,
                         uint8_t *ref_output, size_t *ref_len)
{
    uint8_t *out = malloc(MAX_OUTPUT);

    /* Warm up */
    for (int i = 0; i < WARMUP_ITERS; i++) {
        int32_t rc = fn(input, input_len, out, MAX_OUTPUT);
        (void)rc;
    }

    /* Capture reference output for correctness check */
    int32_t rc = fn(input, input_len, out, MAX_OUTPUT);
    if (rc > 0) {
        memcpy(ref_output, out, (size_t)rc);
        *ref_len = (size_t)rc;
    }

    /* Timed iterations */
    for (int i = 0; i < iters; i++) {
        uint64_t t0 = now_ns();
        fn(input, input_len, out, MAX_OUTPUT);
        uint64_t t1 = now_ns();
        samples[i] = t1 - t0;
    }

    free(out);
    (void)name;
}

static void bench_wasm(const char *label, HlWasmCache *cache,
                       const char *module_name,
                       const uint8_t *input, size_t input_len,
                       int iters, uint64_t *samples,
                       const uint8_t *ref_output, size_t ref_len,
                       int64_t gas,
                       const HlVfs *vfs)
{
    HlWasmCallOpts opts = {0};
    opts.max_input = MAX_OUTPUT;
    opts.max_output = MAX_OUTPUT;
    opts.heap_size = 32 * 1024 * 1024;
    opts.gas = gas;

    /* Warm up */
    for (int i = 0; i < WARMUP_ITERS; i++) {
        void *out = NULL;
        size_t out_len = 0;
        const char *err = NULL;
        hl_cap_wasm_call(cache, module_name,
                         input, input_len, &out, &out_len,
                         &opts, NULL, NULL, vfs, NULL, NULL, &err);
        free(out);
    }

    /* Verify correctness against native reference */
    {
        void *out = NULL;
        size_t out_len = 0;
        const char *err = NULL;
        int rc = hl_cap_wasm_call(cache, module_name,
                                  input, input_len, &out, &out_len,
                                  &opts, NULL, NULL, vfs, NULL, NULL, &err);
        if (rc != 0) {
            fprintf(stderr, "ERROR: %s call failed: %s\n",
                    label, err ? err : "unknown");
            free(out);
            return;
        }
        if (out_len != ref_len || memcmp(out, ref_output, ref_len) != 0) {
            fprintf(stderr, "ERROR: %s output mismatch! "
                    "native=%zu bytes, wasm=%zu bytes\n",
                    label, ref_len, out_len);
            free(out);
            return;
        }
        free(out);
    }

    /* Timed iterations */
    for (int i = 0; i < iters; i++) {
        void *out = NULL;
        size_t out_len = 0;
        const char *err = NULL;
        uint64_t t0 = now_ns();
        hl_cap_wasm_call(cache, module_name,
                         input, input_len, &out, &out_len,
                         &opts, NULL, NULL, vfs, NULL, NULL, &err);
        uint64_t t1 = now_ns();
        samples[i] = t1 - t0;
        free(out);
    }
}

/* ── Workload runner ───────────────────────────────────────────────── */

static void run_workload(const char *workload_name,
                         NativeFn native_fn,
                         const char *interp_module,
                         const char *aot_module,
                         HlWasmCache *cache,
                         const HlVfs *vfs,
                         int iters)
{
    static const struct { const char *label; size_t size; } sizes[] = {
        { "64 B",    64 },
        { "4 KB",    4096 },
        { "64 KB",   65536 },
        { "256 KB",  262144 },
        { "1 MB",    1048576 },
        { "4 MB",    4194304 },
    };

    int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));
    for (int si = 0; si < nsizes; si++) {
        size_t input_len = sizes[si].size;
        uint8_t *input = malloc(input_len);
        generate_input(input, input_len, 42);

        uint64_t *samples = malloc((size_t)iters * sizeof(uint64_t));
        uint8_t *ref_output = malloc(MAX_OUTPUT);
        size_t ref_len = 0;

        printf("\n--- %s (%s) ---\n", workload_name, sizes[si].label);

        /* Native */
        bench_native("Native", native_fn, input, input_len,
                     iters, samples, ref_output, &ref_len);
        Stats s_native = compute_stats(samples, iters);
        print_stats("Native:", &s_native);

        /* WASM interpreter (gas metering always active in this build) */
        bench_wasm("WASM/interp", cache, interp_module,
                   input, input_len, iters, samples,
                   ref_output, ref_len, HL_WASM_MAX_GAS, vfs);
        Stats s_interp = compute_stats(samples, iters);
        print_stats("Interpreter:", &s_interp);

        /* AOT (if available) */
        Stats s_aot = {0};
        int have_aot = 0;
        if (aot_module) {
            /* Pre-load AOT module (idempotent — skips if already cached) */
            int rc = hl_cap_wasm_load(cache, aot_module, vfs, NULL);
            if (rc == 0) {
                bench_wasm("WASM/AOT", cache, aot_module,
                           input, input_len, iters, samples,
                           ref_output, ref_len, HL_WASM_MAX_GAS, vfs);
                s_aot = compute_stats(samples, iters);
                print_stats("AOT:", &s_aot);
                have_aot = 1;
            }
        }

        /* Use mean for overhead ratio (median can be 0 for sub-us native) */
        double native_ref = s_native.mean > 0.1 ? s_native.mean : 0.1;

        if (have_aot) {
            printf("  Overhead:  %.1fx interp  %.1fx AOT  (vs native mean)\n",
                   s_interp.mean / native_ref,
                   s_aot.mean / native_ref);
        } else {
            printf("  WASM/AOT:  (not available — build wamrc and run: wamrc -o file.aot file.wasm)\n");
            printf("  Overhead:  %.1fx interp  (vs native mean)\n",
                   s_interp.mean / native_ref);
        }

        free(ref_output);
        free(samples);
        free(input);
    }
}

/* ── SIMD workload runner ──────────────────────────────────────────── */

/* Generate dot product input: [n:u32] [a: n×f32] [b: n×f32] */
static uint8_t *generate_dot_input(uint32_t n, size_t *out_len)
{
    size_t len = 4 + (size_t)n * 4 * 2;
    uint8_t *buf = malloc(len);
    *(uint32_t *)buf = n;

    float *a = (float *)(buf + 4);
    float *b = (float *)(buf + 4 + n * 4);

    /* Deterministic pseudo-random floats in [-1, 1] */
    uint32_t x = 12345;
    for (uint32_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        a[i] = (float)(int32_t)x / (float)INT32_MAX;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        b[i] = (float)(int32_t)x / (float)INT32_MAX;
    }
    *out_len = len;
    return buf;
}

/* Generate matmul input: [dim:u32] [A: dim×dim×f32] [B: dim×dim×f32] */
static uint8_t *generate_matmul_input(uint32_t dim, size_t *out_len)
{
    size_t elems = (size_t)dim * dim;
    size_t len = 4 + elems * 4 * 2;
    uint8_t *buf = malloc(len);
    *(uint32_t *)buf = dim;

    float *A = (float *)(buf + 4);
    float *B = (float *)(buf + 4 + elems * 4);

    uint32_t x = 67890;
    for (size_t i = 0; i < elems; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        A[i] = (float)(int32_t)x / (float)INT32_MAX;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        B[i] = (float)(int32_t)x / (float)INT32_MAX;
    }
    *out_len = len;
    return buf;
}

/* Approximate float comparison for outputs (native vs WASM rounding diffs).
 * Uses byte-level comparison: tries as f64 first (for dot product output),
 * then falls back to f32 element comparison (for matmul output).
 * Tolerances are generous because SIMD f32x4 accumulation order differs
 * from scalar f64 accumulation, causing measurable drift on large vectors. */
static int approx_equal_float(const void *a, const void *b, size_t len)
{
    /* Try exact match first */
    if (memcmp(a, b, len) == 0)
        return 1;

    /* Try as f64 array (dot product: 8 bytes = 1 f64) */
    if (len % sizeof(double) == 0) {
        const double *da = (const double *)a;
        const double *db = (const double *)b;
        size_t n = len / sizeof(double);
        int ok = 1;
        for (size_t i = 0; i < n; i++) {
            double diff = da[i] - db[i];
            if (diff < 0) diff = -diff;
            double mag = da[i] < 0 ? -da[i] : da[i];
            /* Relative tolerance 1e-3, absolute tolerance 1e-4 */
            if (diff > 1e-4 && diff > mag * 1e-3) {
                ok = 0;
                break;
            }
        }
        if (ok) return 1;
    }

    /* Try as f32 array (matmul output) */
    if (len % sizeof(float) == 0) {
        const float *fa = (const float *)a;
        const float *fb = (const float *)b;
        size_t n = len / sizeof(float);
        for (size_t i = 0; i < n; i++) {
            float diff = fa[i] - fb[i];
            if (diff < 0) diff = -diff;
            float mag = fa[i] < 0 ? -fa[i] : fa[i];
            /* Relative tolerance 1e-3, absolute tolerance 1e-4 */
            if (diff > 1e-4f && diff > mag * 1e-3f)
                return 0;
        }
        return 1;
    }

    return 0;
}

/* bench_wasm variant that uses approximate float comparison */
static void bench_wasm_approx(const char *label, HlWasmCache *cache,
                               const char *module_name,
                               const uint8_t *input, size_t input_len,
                               int iters, uint64_t *samples,
                               const uint8_t *ref_output, size_t ref_len,
                               int64_t gas,
                               const HlVfs *vfs)
{
    HlWasmCallOpts opts = {0};
    opts.max_input = MAX_OUTPUT;
    opts.max_output = MAX_OUTPUT;
    opts.heap_size = 32 * 1024 * 1024;
    opts.gas = gas;

    /* Warm up */
    for (int i = 0; i < WARMUP_ITERS; i++) {
        void *out = NULL;
        size_t out_len = 0;
        const char *err = NULL;
        hl_cap_wasm_call(cache, module_name,
                         input, input_len, &out, &out_len,
                         &opts, NULL, NULL, vfs, NULL, NULL, &err);
        free(out);
    }

    /* Verify correctness against native reference (approximate) */
    {
        void *out = NULL;
        size_t out_len = 0;
        const char *err = NULL;
        int rc = hl_cap_wasm_call(cache, module_name,
                                  input, input_len, &out, &out_len,
                                  &opts, NULL, NULL, vfs, NULL, NULL, &err);
        if (rc != 0) {
            fprintf(stderr, "ERROR: %s call failed: %s\n",
                    label, err ? err : "unknown");
            free(out);
            return;
        }
        if (out_len != ref_len || !approx_equal_float(out, ref_output, ref_len)) {
            fprintf(stderr, "ERROR: %s output mismatch! "
                    "native=%zu bytes, wasm=%zu bytes\n",
                    label, ref_len, out_len);
            free(out);
            return;
        }
        free(out);
    }

    /* Timed iterations */
    for (int i = 0; i < iters; i++) {
        void *out = NULL;
        size_t out_len = 0;
        const char *err = NULL;
        uint64_t t0 = now_ns();
        hl_cap_wasm_call(cache, module_name,
                         input, input_len, &out, &out_len,
                         &opts, NULL, NULL, vfs, NULL, NULL, &err);
        uint64_t t1 = now_ns();
        samples[i] = t1 - t0;
        free(out);
    }
}

/*
 * Run SIMD benchmark: native vs scalar WASM (interp+AOT) vs SIMD WASM (AOT only).
 *
 * SIMD .wasm modules use v128 types which the WAMR interpreter rejects
 * (needs SIMDe, not vendored). SIMD is only benchmarked via AOT where
 * WAMR compiles SIMD opcodes to native SSE4.1/NEON instructions.
 */
static void run_simd_workload(const char *workload_name,
                              NativeFn native_fn,
                              const char *scalar_module,
                              const char *scalar_aot_module,
                              const char *simd_aot_module,
                              HlWasmCache *cache,
                              const HlVfs *vfs,
                              const uint8_t *input, size_t input_len,
                              const char *size_label,
                              int iters)
{
    uint64_t *samples = malloc((size_t)iters * sizeof(uint64_t));
    uint8_t *ref_output = malloc(MAX_OUTPUT);
    size_t ref_len = 0;

    printf("\n--- %s (%s) ---\n", workload_name, size_label);

    /* Native */
    bench_native("Native", native_fn, input, input_len,
                 iters, samples, ref_output, &ref_len);
    Stats s_native = compute_stats(samples, iters);
    print_stats("Native:", &s_native);
    double native_ref = s_native.mean > 0.1 ? s_native.mean : 0.1;

    /* Scalar WASM interpreter (no SIMD opcodes — works in interpreter) */
    bench_wasm_approx("Scalar/interp", cache, scalar_module,
                      input, input_len, iters, samples,
                      ref_output, ref_len, HL_WASM_MAX_GAS, vfs);
    Stats s_scalar = compute_stats(samples, iters);
    print_stats("Scalar:", &s_scalar);

    /* Scalar AOT */
    Stats s_scalar_aot = {0};
    int have_scalar_aot = 0;
    if (scalar_aot_module) {
        int rc = hl_cap_wasm_load(cache, scalar_aot_module, vfs, NULL);
        if (rc == 0) {
            bench_wasm_approx("Scalar/AOT", cache, scalar_aot_module,
                              input, input_len, iters, samples,
                              ref_output, ref_len, HL_WASM_MAX_GAS, vfs);
            s_scalar_aot = compute_stats(samples, iters);
            print_stats("Scalar AOT:", &s_scalar_aot);
            have_scalar_aot = 1;
        }
    }

    /* SIMD AOT only (interpreter can't load v128 types without SIMDe) */
    Stats s_simd_aot = {0};
    int have_simd_aot = 0;
    if (simd_aot_module) {
        int rc = hl_cap_wasm_load(cache, simd_aot_module, vfs, NULL);
        if (rc == 0) {
            bench_wasm_approx("SIMD/AOT", cache, simd_aot_module,
                              input, input_len, iters, samples,
                              ref_output, ref_len, HL_WASM_MAX_GAS, vfs);
            s_simd_aot = compute_stats(samples, iters);
            print_stats("SIMD AOT:", &s_simd_aot);
            have_simd_aot = 1;
        }
    }

    /* Summary */
    printf("  Overhead vs native:  %.1fx scalar-interp", s_scalar.mean / native_ref);
    if (have_scalar_aot)
        printf("  %.1fx scalar-AOT", s_scalar_aot.mean / native_ref);
    if (have_simd_aot)
        printf("  %.1fx simd-AOT", s_simd_aot.mean / native_ref);
    else
        printf("  (SIMD AOT not available — build with: make -C bench/wasm/workloads aot)");
    printf("\n");

    if (have_scalar_aot && have_simd_aot && s_scalar_aot.mean > 0.1)
        printf("  SIMD speedup vs scalar AOT:  %.2fx\n",
               s_scalar_aot.mean / s_simd_aot.mean);

    free(ref_output);
    free(samples);
}

/* ── Embedded WASM modules ─────────────────────────────────────────── */

/* These are #include'd as byte arrays from the compiled .wasm files */

#define INCBIN(name, path) \
    __asm__(".section .rodata\n" \
            ".global " #name "\n" \
            ".balign 16\n" \
            #name ":\n" \
            ".incbin \"" path "\"\n" \
            #name "_end:\n" \
            ".global " #name "_len\n" \
            ".balign 4\n" \
            #name "_len:\n" \
            ".int " #name "_end - " #name "\n" \
            ".previous\n"); \
    extern const unsigned char name[]; \
    extern const unsigned int name##_len;

/* We'll load from VFS entries instead — simpler and portable */

/* ── Main ──────────────────────────────────────────────────────────── */

static const char *arch_suffix(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#else
    return "unknown";
#endif
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int iters = DEFAULT_ITERS;
    const char *env_iters = getenv("BENCH_ITERS");
    if (env_iters) {
        int n = atoi(env_iters);
        if (n > 0) iters = n;
    }

    printf("=== Hull WASM Compute Benchmark ===\n");
    printf("Platform: %s %s\n",
#ifdef __APPLE__
           "Darwin",
#else
           "Linux",
#endif
           arch_suffix());
    printf("WAMR modes: fast interpreter (gas metered) + AOT (if available)\n");
    printf("Iterations: %d (warmup: %d)\n", iters, WARMUP_ITERS);

    /* ── Load WASM modules from files ─────────────────────────────── */

    /* Module descriptor: tracks loaded bytes + length */
    typedef struct { uint8_t *data; uint32_t len; } WasmBuf;

    /* Helper: load a file into a WasmBuf. Tries two path prefixes. */
    #define LOAD_FILE(buf, basename) do { \
        FILE *_f = fopen("bench/wasm/workloads/" basename, "rb"); \
        if (!_f) _f = fopen("workloads/" basename, "rb"); \
        if (_f) { \
            fseek(_f, 0, SEEK_END); \
            (buf).len = (uint32_t)ftell(_f); \
            fseek(_f, 0, SEEK_SET); \
            (buf).data = malloc((buf).len); \
            if (fread((buf).data, 1, (buf).len, _f) != (buf).len) { \
                free((buf).data); (buf).data = NULL; (buf).len = 0; \
            } \
            fclose(_f); \
        } \
    } while (0)

    /* Helper: load AOT file with arch suffix */
    #define LOAD_AOT(buf, basename) do { \
        char _p[512]; \
        snprintf(_p, sizeof(_p), "bench/wasm/workloads/" basename ".%s", arch_suffix()); \
        FILE *_f = fopen(_p, "rb"); \
        if (!_f) { \
            snprintf(_p, sizeof(_p), "workloads/" basename ".%s", arch_suffix()); \
            _f = fopen(_p, "rb"); \
        } \
        if (_f) { \
            fseek(_f, 0, SEEK_END); \
            (buf).len = (uint32_t)ftell(_f); \
            fseek(_f, 0, SEEK_SET); \
            (buf).data = malloc((buf).len); \
            if (fread((buf).data, 1, (buf).len, _f) != (buf).len) { \
                free((buf).data); (buf).data = NULL; (buf).len = 0; \
            } \
            fclose(_f); \
        } \
    } while (0)

    /* Original workloads */
    WasmBuf hash_wasm = {0}, hist_wasm = {0};
    WasmBuf hash_aot = {0}, hist_aot = {0};

    LOAD_FILE(hash_wasm, "compute_hash.wasm");
    LOAD_FILE(hist_wasm, "mem_histogram.wasm");
    LOAD_AOT(hash_aot, "compute_hash.aot");
    LOAD_AOT(hist_aot, "mem_histogram.aot");

    if (!hash_wasm.data || !hist_wasm.data) {
        fprintf(stderr, "ERROR: cannot load .wasm files from bench/wasm/workloads/\n");
        free(hash_wasm.data);
        free(hist_wasm.data);
        return 1;
    }

    /* SIMD workloads: scalar .wasm for interpreter, SIMD only via AOT */
    WasmBuf dot_scalar = {0}, mat_scalar = {0};
    WasmBuf dot_scalar_aot = {0}, dot_simd_aot = {0};
    WasmBuf mat_scalar_aot = {0}, mat_simd_aot = {0};

    LOAD_FILE(dot_scalar, "simd_dot_product.wasm");
    LOAD_FILE(mat_scalar, "simd_matmul.wasm");
    LOAD_AOT(dot_scalar_aot, "simd_dot_product.aot");
    LOAD_AOT(dot_simd_aot, "simd_dot_product_simd.aot");
    LOAD_AOT(mat_scalar_aot, "simd_matmul.aot");
    LOAD_AOT(mat_simd_aot, "simd_matmul_simd.aot");

    int have_simd = (dot_scalar.data && mat_scalar.data);
    if (!have_simd)
        printf("Note: SIMD workloads not found — run 'make' in bench/wasm/workloads/\n");

    /* ── Build VFS entries ─────────────────────────────────────────── */

    /* Max entries: 2 original + 4 SIMD interp + up to 6 AOT = 12, +1 sentinel */
    HlEntry entries[16];
    int ei = 0;

    #define ADD_VFS(vfs_name, buf) do { \
        if ((buf).data) \
            entries[ei++] = (HlEntry){ (vfs_name), (buf).data, (buf).len }; \
    } while (0)

    /* VFS names for AOT need arch suffix — use static buffers */
    static char aot_names[8][80];
    #define ADD_AOT_VFS(base, buf, idx) do { \
        if ((buf).data) { \
            snprintf(aot_names[idx], sizeof(aot_names[idx]), \
                     "compute/" base ".aot.%s", arch_suffix()); \
            entries[ei++] = (HlEntry){ aot_names[idx], (buf).data, (buf).len }; \
        } \
    } while (0)

    /* Original workloads */
    ADD_AOT_VFS("compute_hash_aot", hash_aot, 0);
    ADD_VFS("compute/compute_hash_interp.wasm", hash_wasm);
    ADD_AOT_VFS("mem_histogram_aot", hist_aot, 1);
    ADD_VFS("compute/mem_histogram_interp.wasm", hist_wasm);

    /* SIMD workloads (scalar .wasm for interpreter, SIMD .aot only) */
    ADD_AOT_VFS("simd_dot_product_aot", dot_scalar_aot, 2);
    ADD_VFS("compute/simd_dot_product_interp.wasm", dot_scalar);
    ADD_AOT_VFS("simd_dot_product_simd_aot", dot_simd_aot, 3);
    ADD_AOT_VFS("simd_matmul_aot", mat_scalar_aot, 4);
    ADD_VFS("compute/simd_matmul_interp.wasm", mat_scalar);
    ADD_AOT_VFS("simd_matmul_simd_aot", mat_simd_aot, 5);

    entries[ei] = (HlEntry){ 0, 0, 0 };

    /* Sort entries by name for VFS binary search */
    for (int i = 0; i < ei - 1; i++) {
        for (int j = i + 1; j < ei; j++) {
            if (strcmp(entries[i].name, entries[j].name) > 0) {
                HlEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    HlVfs vfs;
    hl_vfs_init(&vfs, entries, NULL);

    /* Initialize WAMR */
    HlWasmCache cache;
    if (hl_cap_wasm_init(&cache) != 0) {
        fprintf(stderr, "ERROR: WAMR init failed\n");
        return 1;
    }

    /* Pre-load interpreter modules */
    hl_cap_wasm_load(&cache, "compute_hash_interp", &vfs, NULL);
    hl_cap_wasm_load(&cache, "mem_histogram_interp", &vfs, NULL);

    if (hash_aot.data) printf("AOT: compute_hash loaded (%u bytes)\n", hash_aot.len);
    else               printf("AOT: compute_hash not available\n");
    if (hist_aot.data) printf("AOT: mem_histogram loaded (%u bytes)\n", hist_aot.len);
    else               printf("AOT: mem_histogram not available\n");

    /* ── Run original workloads ────────────────────────────────────── */

    run_workload("compute_hash", compute_hash_native,
                 "compute_hash_interp",
                 hash_aot.data ? "compute_hash_aot" : NULL,
                 &cache, &vfs, iters);

    run_workload("mem_histogram", mem_histogram_native,
                 "mem_histogram_interp",
                 hist_aot.data ? "mem_histogram_aot" : NULL,
                 &cache, &vfs, iters);

    /* ── Run SIMD workloads ────────────────────────────────────────── */

    if (have_simd) {
        /* Pre-load scalar interpreter modules */
        hl_cap_wasm_load(&cache, "simd_dot_product_interp", &vfs, NULL);
        hl_cap_wasm_load(&cache, "simd_matmul_interp", &vfs, NULL);

        if (dot_scalar_aot.data) printf("AOT: dot_product scalar loaded\n");
        if (dot_simd_aot.data)   printf("AOT: dot_product SIMD loaded\n");
        if (mat_scalar_aot.data) printf("AOT: matmul scalar loaded\n");
        if (mat_simd_aot.data)   printf("AOT: matmul SIMD loaded\n");

        printf("\n=== SIMD128 Benchmarks ===\n");
        printf("(SIMD modules require AOT — interpreter only runs scalar variant)\n");

        /* Dot product: vector sizes from 1K to 1M elements.
         * Small sizes are overhead-dominated; large sizes show SIMD benefit. */
        static const struct { const char *label; uint32_t n; } dot_sizes[] = {
            { "1K elems (8 KB)",       1024 },
            { "16K elems (128 KB)",    16384 },
            { "256K elems (2 MB)",     262144 },
            { "1M elems (8 MB)",       1048576 },
        };
        for (int si = 0; si < (int)(sizeof(dot_sizes)/sizeof(dot_sizes[0])); si++) {
            size_t input_len;
            uint8_t *input = generate_dot_input(dot_sizes[si].n, &input_len);
            run_simd_workload("simd_dot_product",
                              simd_dot_product_native,
                              "simd_dot_product_interp",
                              dot_scalar_aot.data ? "simd_dot_product_aot" : NULL,
                              dot_simd_aot.data ? "simd_dot_product_simd_aot" : NULL,
                              &cache, &vfs,
                              input, input_len, dot_sizes[si].label, iters);
            free(input);
        }

        /* Matmul: matrix sizes up to 256×256.
         * Small sizes are overhead-dominated; ≥128 shows SIMD benefit. */
        static const struct { const char *label; uint32_t dim; } mat_sizes[] = {
            { "16x16 (2 KB)",     16 },
            { "64x64 (32 KB)",    64 },
            { "128x128 (128 KB)", 128 },
            { "256x256 (512 KB)", 256 },
        };
        for (int si = 0; si < (int)(sizeof(mat_sizes)/sizeof(mat_sizes[0])); si++) {
            size_t input_len;
            uint8_t *input = generate_matmul_input(mat_sizes[si].dim, &input_len);
            run_simd_workload("simd_matmul",
                              simd_matmul_native,
                              "simd_matmul_interp",
                              mat_scalar_aot.data ? "simd_matmul_aot" : NULL,
                              mat_simd_aot.data ? "simd_matmul_simd_aot" : NULL,
                              &cache, &vfs,
                              input, input_len, mat_sizes[si].label, iters);
            free(input);
        }
    }

    /* ── Native C vs WASM: total execution time across input sizes ── */

    printf("\n=== Native C vs WASM: Total Execution Time (compute_hash) ===\n");
    printf("All times include computation + per-call overhead.\n");
    printf("Native C   = direct function call (no isolation).\n");
    printf("Unpooled   = fresh WASM instance per call (heap > pool threshold).\n");
    printf("Pooled     = WASM instance reuse (interpreter, gas metered).\n");
    printf("Pooled AOT = WASM instance reuse (AOT compiled, gas metered).\n\n");

    {
        HlWasmCache *bc = &cache;
        const char *interp_mod = "compute_hash_interp";
        const char *aot_mod = hash_aot.data ? "compute_hash_aot" : NULL;

        static const struct { const char *label; size_t size; } pool_sizes[] = {
            { "64 B",    64 },
            { "1 KB",    1024 },
            { "4 KB",    4096 },
            { "64 KB",   65536 },
            { "256 KB",  262144 },
        };
        int n_pool_sizes = (int)(sizeof(pool_sizes) / sizeof(pool_sizes[0]));

        int pool_iters = 100;
        uint64_t *s_nat = malloc((size_t)pool_iters * sizeof(uint64_t));
        uint64_t *s_unp = malloc((size_t)pool_iters * sizeof(uint64_t));
        uint64_t *s_pol = malloc((size_t)pool_iters * sizeof(uint64_t));
        uint64_t *s_aot = malloc((size_t)pool_iters * sizeof(uint64_t));
        uint8_t *nat_out = malloc(MAX_OUTPUT);

        for (int si = 0; si < n_pool_sizes; si++) {
            size_t sz = pool_sizes[si].size;
            uint8_t *inp = malloc(sz);
            generate_input(inp, sz, 42);

            printf("--- %s ---\n", pool_sizes[si].label);

            /* Native C */
            for (int i = 0; i < WARMUP_ITERS; i++)
                compute_hash_native(inp, sz, nat_out, MAX_OUTPUT);
            for (int i = 0; i < pool_iters; i++) {
                uint64_t t0 = now_ns();
                compute_hash_native(inp, sz, nat_out, MAX_OUTPUT);
                s_nat[i] = now_ns() - t0;
            }
            Stats st_nat = compute_stats(s_nat, pool_iters);
            print_stats("Native C:", &st_nat);

            /* Unpooled: 32 MB heap > threshold → no pooling */
            {
                HlWasmCallOpts opts = {0};
                opts.max_input  = (uint32_t)(sz + 1024);
                opts.max_output = (uint32_t)(sz + 1024);
                opts.heap_size  = 32 * 1024 * 1024;
                opts.gas        = HL_WASM_MAX_GAS;
                for (int i = 0; i < WARMUP_ITERS; i++) {
                    void *out = NULL; size_t ol = 0; const char *err = NULL;
                    hl_cap_wasm_call(bc, interp_mod, inp, sz, &out, &ol,
                                     &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                    free(out);
                }
                for (int i = 0; i < pool_iters; i++) {
                    void *out = NULL; size_t ol = 0; const char *err = NULL;
                    uint64_t t0 = now_ns();
                    hl_cap_wasm_call(bc, interp_mod, inp, sz, &out, &ol,
                                     &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                    s_unp[i] = now_ns() - t0;
                    free(out);
                }
                Stats st_unp = compute_stats(s_unp, pool_iters);
                print_stats("Unpooled:", &st_unp);
            }

            /* Pooled interpreter: heap must fit input + output + overhead.
             * Stay under pool threshold (4 MB) so pooling kicks in. */
            uint32_t pooled_heap = (uint32_t)(sz * 4 + 64 * 1024);
            if (pooled_heap < HL_WASM_DEFAULT_HEAP) pooled_heap = HL_WASM_DEFAULT_HEAP;
            if (pooled_heap > HL_WASM_POOL_HEAP_THRESHOLD)
                pooled_heap = HL_WASM_POOL_HEAP_THRESHOLD;
            {
                HlWasmCallOpts opts = {0};
                opts.max_input  = (uint32_t)(sz + 1024);
                opts.max_output = (uint32_t)(sz + 1024);
                opts.heap_size  = pooled_heap;
                opts.gas        = HL_WASM_MAX_GAS;
                /* Prime */
                { void *out = NULL; size_t ol = 0; const char *err = NULL;
                  hl_cap_wasm_call(bc, interp_mod, inp, sz, &out, &ol,
                                   &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                  free(out); }
                for (int i = 0; i < pool_iters; i++) {
                    void *out = NULL; size_t ol = 0; const char *err = NULL;
                    uint64_t t0 = now_ns();
                    hl_cap_wasm_call(bc, interp_mod, inp, sz, &out, &ol,
                                     &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                    s_pol[i] = now_ns() - t0;
                    free(out);
                }
                Stats st_pol = compute_stats(s_pol, pool_iters);
                print_stats("Pooled:", &st_pol);
            }

            /* Pooled AOT */
            Stats st_aot = {0};
            int got_aot = 0;
            if (aot_mod) {
                HlWasmCallOpts opts = {0};
                opts.max_input  = (uint32_t)(sz + 1024);
                opts.max_output = (uint32_t)(sz + 1024);
                opts.heap_size  = pooled_heap;
                opts.gas        = HL_WASM_MAX_GAS;
                /* Prime */
                { void *out = NULL; size_t ol = 0; const char *err = NULL;
                  hl_cap_wasm_call(bc, aot_mod, inp, sz, &out, &ol,
                                   &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                  free(out); }
                for (int i = 0; i < pool_iters; i++) {
                    void *out = NULL; size_t ol = 0; const char *err = NULL;
                    uint64_t t0 = now_ns();
                    hl_cap_wasm_call(bc, aot_mod, inp, sz, &out, &ol,
                                     &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                    s_aot[i] = now_ns() - t0;
                    free(out);
                }
                st_aot = compute_stats(s_aot, pool_iters);
                print_stats("Pooled AOT:", &st_aot);
                got_aot = 1;
            }

            double nb = st_nat.mean > 0.1 ? st_nat.mean : 0.1;
            Stats st_unp2 = compute_stats(s_unp, pool_iters);
            Stats st_pol2 = compute_stats(s_pol, pool_iters);
            printf("  vs native:  %.1fx unpooled  %.1fx pooled",
                   st_unp2.mean / nb, st_pol2.mean / nb);
            if (got_aot)
                printf("  %.1fx pooled-AOT", st_aot.mean / nb);
            printf("\n\n");

            free(inp);
        }

        free(nat_out);
        free(s_nat);
        free(s_unp);
        free(s_pol);
        free(s_aot);
    }

    /* ── Buffer mode: string vs buffer single call ─────────────────── */

    printf("\n=== Buffer Mode: String vs Zero-Copy (mem_histogram) ===\n");
    printf("Compares output handling: string (malloc+memcpy+free) vs HlWasmBuffer.\n");
    printf("Both use pooled instances. Buffer defers instance return to destroy.\n\n");

    {
        const char *mod = "mem_histogram_interp";
        int buf_iters = 100;
        uint64_t *s_str = malloc((size_t)buf_iters * sizeof(uint64_t));
        uint64_t *s_buf = malloc((size_t)buf_iters * sizeof(uint64_t));

        static const struct { const char *label; size_t size; } buf_sizes[] = {
            { "4 KB",    4096 },
            { "64 KB",   65536 },
            { "256 KB",  262144 },
            { "1 MB",    1048576 },
        };
        int n_buf_sizes = (int)(sizeof(buf_sizes) / sizeof(buf_sizes[0]));

        for (int si = 0; si < n_buf_sizes; si++) {
            size_t sz = buf_sizes[si].size;
            uint8_t *inp = malloc(sz);
            generate_input(inp, sz, 42);

            uint32_t pooled_heap = (uint32_t)(sz * 4 + 64 * 1024);
            if (pooled_heap < HL_WASM_DEFAULT_HEAP) pooled_heap = HL_WASM_DEFAULT_HEAP;
            if (pooled_heap > HL_WASM_POOL_HEAP_THRESHOLD)
                pooled_heap = HL_WASM_POOL_HEAP_THRESHOLD;

            HlWasmCallOpts opts = {0};
            opts.max_input  = (uint32_t)(sz + 1024);
            opts.max_output = (uint32_t)(sz + 1024);
            opts.heap_size  = pooled_heap;
            opts.gas        = HL_WASM_MAX_GAS;

            printf("--- %s ---\n", buf_sizes[si].label);

            /* Prime pool */
            { void *out = NULL; size_t ol = 0; const char *err = NULL;
              hl_cap_wasm_call(&cache, mod, inp, sz, &out, &ol,
                               &opts, NULL, NULL, &vfs, NULL, NULL, &err);
              free(out); }

            /* String mode (original) */
            for (int i = 0; i < buf_iters; i++) {
                void *out = NULL; size_t ol = 0; const char *err = NULL;
                uint64_t t0 = now_ns();
                hl_cap_wasm_call(&cache, mod, inp, sz, &out, &ol,
                                 &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                s_str[i] = now_ns() - t0;
                free(out);
            }
            Stats st_str = compute_stats(s_str, buf_iters);
            print_stats("String:", &st_str);

            /* Buffer mode */
            for (int i = 0; i < buf_iters; i++) {
                HlWasmBuffer *out_buf = NULL; const char *err = NULL;
                uint64_t t0 = now_ns();
                hl_cap_wasm_call_buf(&cache, mod, inp, sz, &out_buf,
                                     &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                s_buf[i] = now_ns() - t0;
                hl_wasm_buffer_destroy(out_buf);
                free(out_buf);
            }
            Stats st_buf = compute_stats(s_buf, buf_iters);
            print_stats("Buffer:", &st_buf);

            double ratio = st_str.mean > 0.1 ? st_buf.mean / st_str.mean : 0;
            printf("  Buffer/String ratio: %.2fx (%.1fus saved per call)\n\n",
                   ratio, (st_str.mean - st_buf.mean) / 1000.0);

            free(inp);
        }

        free(s_str);
        free(s_buf);
    }

    /* ── Chain benchmark: 3-step pipeline string vs buffer ────────── */

    printf("\n=== Chain Benchmark: 3-Step Pipeline (mem_histogram) ===\n");
    printf("Pipeline: call→call→call with output of each feeding the next.\n");
    printf("String mode: materialize to malloc'd bytes between each call.\n");
    printf("Buffer mode: pass HlWasmBuffer data pointer directly (zero-copy read).\n\n");

    {
        const char *mod = "mem_histogram_interp";
        int chain_iters = 100;
        uint64_t *s_str = malloc((size_t)chain_iters * sizeof(uint64_t));
        uint64_t *s_buf = malloc((size_t)chain_iters * sizeof(uint64_t));

        static const struct { const char *label; size_t size; } chain_sizes[] = {
            { "4 KB",    4096 },
            { "64 KB",   65536 },
            { "256 KB",  262144 },
        };
        int n_chain_sizes = (int)(sizeof(chain_sizes) / sizeof(chain_sizes[0]));

        for (int si = 0; si < n_chain_sizes; si++) {
            size_t sz = chain_sizes[si].size;
            uint8_t *inp = malloc(sz);
            generate_input(inp, sz, 42);

            uint32_t pooled_heap = (uint32_t)(sz * 4 + 64 * 1024);
            if (pooled_heap < HL_WASM_DEFAULT_HEAP) pooled_heap = HL_WASM_DEFAULT_HEAP;
            if (pooled_heap > HL_WASM_POOL_HEAP_THRESHOLD)
                pooled_heap = HL_WASM_POOL_HEAP_THRESHOLD;

            HlWasmCallOpts opts = {0};
            opts.max_input  = (uint32_t)(sz + 1024);
            opts.max_output = (uint32_t)(sz + 1024);
            opts.heap_size  = pooled_heap;
            opts.gas        = HL_WASM_MAX_GAS;

            printf("--- %s (×3 chain) ---\n", chain_sizes[si].label);

            /* Prime pool with enough instances for chaining */
            for (int p = 0; p < 3; p++) {
                void *out = NULL; size_t ol = 0; const char *err = NULL;
                hl_cap_wasm_call(&cache, mod, inp, sz, &out, &ol,
                                 &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                free(out);
            }

            /* String mode chain: call→malloc+memcpy→call→malloc+memcpy→call */
            for (int i = 0; i < chain_iters; i++) {
                uint64_t t0 = now_ns();

                void *out1 = NULL; size_t ol1 = 0; const char *err = NULL;
                hl_cap_wasm_call(&cache, mod, inp, sz, &out1, &ol1,
                                 &opts, NULL, NULL, &vfs, NULL, NULL, &err);

                void *out2 = NULL; size_t ol2 = 0; err = NULL;
                hl_cap_wasm_call(&cache, mod, out1, ol1, &out2, &ol2,
                                 &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                free(out1);

                void *out3 = NULL; size_t ol3 = 0; err = NULL;
                hl_cap_wasm_call(&cache, mod, out2, ol2, &out3, &ol3,
                                 &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                free(out2);

                s_str[i] = now_ns() - t0;
                free(out3);
            }
            Stats st_str = compute_stats(s_str, chain_iters);
            print_stats("String:", &st_str);

            /* Buffer mode chain: call_buf→read data ptr→call_buf→read data ptr→call_buf */
            for (int i = 0; i < chain_iters; i++) {
                uint64_t t0 = now_ns();

                HlWasmBuffer *buf1 = NULL; const char *err = NULL;
                hl_cap_wasm_call_buf(&cache, mod, inp, sz, &buf1,
                                     &opts, NULL, NULL, &vfs, NULL, NULL, &err);

                HlWasmBuffer *buf2 = NULL; err = NULL;
                hl_cap_wasm_call_buf(&cache, mod,
                                     hl_wasm_buffer_data(buf1),
                                     hl_wasm_buffer_len(buf1),
                                     &buf2, &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                hl_wasm_buffer_destroy(buf1); free(buf1);

                HlWasmBuffer *buf3 = NULL; err = NULL;
                hl_cap_wasm_call_buf(&cache, mod,
                                     hl_wasm_buffer_data(buf2),
                                     hl_wasm_buffer_len(buf2),
                                     &buf3, &opts, NULL, NULL, &vfs, NULL, NULL, &err);
                hl_wasm_buffer_destroy(buf2); free(buf2);

                s_buf[i] = now_ns() - t0;
                hl_wasm_buffer_destroy(buf3); free(buf3);
            }
            Stats st_buf = compute_stats(s_buf, chain_iters);
            print_stats("Buffer:", &st_buf);

            double ratio = st_str.mean > 0.1 ? st_buf.mean / st_str.mean : 0;
            double saved = (st_str.mean - st_buf.mean) / 1000.0;
            printf("  Buffer/String ratio: %.2fx (%.1fus saved per 3-step chain)\n\n",
                   ratio, saved);

            free(inp);
        }

        free(s_str);
        free(s_buf);
    }

    /* Cleanup */
    hl_cap_wasm_destroy(&cache);
    free(hash_wasm.data);
    free(hist_wasm.data);
    free(hash_aot.data);
    free(hist_aot.data);
    free(dot_scalar.data);
    free(mat_scalar.data);
    free(dot_scalar_aot.data);
    free(dot_simd_aot.data);
    free(mat_scalar_aot.data);
    free(mat_simd_aot.data);

    printf("\nDone.\n");
    return 0;
}

#else /* !HL_ENABLE_WASM */

#include <stdio.h>

int main(void)
{
    fprintf(stderr, "WASM support not compiled — rebuild with HL_ENABLE_WASM=1\n");
    return 1;
}

#endif
