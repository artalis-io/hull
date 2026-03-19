/*
 * bench_wasm.c — WASM compute benchmark: WAMR interpreter vs AOT vs native C
 *
 * Compares hull_process execution via:
 *   1. Native C (direct function call)
 *   2. WAMR classic interpreter (.wasm)
 *   3. WAMR AOT (.aot pre-compiled, if available)
 *
 * Two workloads: compute-intensive hash compression, memory-intensive counting sort.
 * Three input sizes: 64 B, 4 KB, 64 KB.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"
#include "hull/limits.h"
#include "hull/vfs.h"
#include "hull/entry.h"
#include "compute_hash_native.h"
#include "mem_histogram_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ── Configuration ─────────────────────────────────────────────────── */

#define DEFAULT_ITERS   1000
#define WARMUP_ITERS    10
#define MAX_OUTPUT      (8 * 1024 * 1024)

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
                         &opts, NULL, NULL, vfs, NULL, &err);
        free(out);
    }

    /* Verify correctness against native reference */
    {
        void *out = NULL;
        size_t out_len = 0;
        const char *err = NULL;
        int rc = hl_cap_wasm_call(cache, module_name,
                                  input, input_len, &out, &out_len,
                                  &opts, NULL, NULL, vfs, NULL, &err);
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
                         &opts, NULL, NULL, vfs, NULL, &err);
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

    /* Load WASM modules from files */
    FILE *f;
    uint8_t *hash_wasm = NULL, *hist_wasm = NULL;
    uint32_t hash_wasm_len = 0, hist_wasm_len = 0;
    uint8_t *hash_aot = NULL, *hist_aot = NULL;
    uint32_t hash_aot_len = 0, hist_aot_len = 0;

    /* Read compute_hash.wasm */
    f = fopen("bench/wasm/workloads/compute_hash.wasm", "rb");
    if (!f) f = fopen("workloads/compute_hash.wasm", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        hash_wasm_len = (uint32_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        hash_wasm = malloc(hash_wasm_len);
        if (fread(hash_wasm, 1, hash_wasm_len, f) != hash_wasm_len) {
            free(hash_wasm); hash_wasm = NULL; hash_wasm_len = 0;
        }
        fclose(f);
    }

    /* Read mem_histogram.wasm */
    f = fopen("bench/wasm/workloads/mem_histogram.wasm", "rb");
    if (!f) f = fopen("workloads/mem_histogram.wasm", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        hist_wasm_len = (uint32_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        hist_wasm = malloc(hist_wasm_len);
        if (fread(hist_wasm, 1, hist_wasm_len, f) != hist_wasm_len) {
            free(hist_wasm); hist_wasm = NULL; hist_wasm_len = 0;
        }
        fclose(f);
    }

    if (!hash_wasm || !hist_wasm) {
        fprintf(stderr, "ERROR: cannot load .wasm files from bench/wasm/workloads/\n");
        free(hash_wasm);
        free(hist_wasm);
        return 1;
    }

    /* Try loading AOT files: compute_hash.aot.<arch>, mem_histogram.aot.<arch> */
    char aot_path[512];

    snprintf(aot_path, sizeof(aot_path),
             "bench/wasm/workloads/compute_hash.aot.%s", arch_suffix());
    f = fopen(aot_path, "rb");
    if (!f) {
        snprintf(aot_path, sizeof(aot_path),
                 "workloads/compute_hash.aot.%s", arch_suffix());
        f = fopen(aot_path, "rb");
    }
    if (f) {
        fseek(f, 0, SEEK_END);
        hash_aot_len = (uint32_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        hash_aot = malloc(hash_aot_len);
        if (fread(hash_aot, 1, hash_aot_len, f) != hash_aot_len) {
            free(hash_aot); hash_aot = NULL; hash_aot_len = 0;
        }
        fclose(f);
    }

    snprintf(aot_path, sizeof(aot_path),
             "bench/wasm/workloads/mem_histogram.aot.%s", arch_suffix());
    f = fopen(aot_path, "rb");
    if (!f) {
        snprintf(aot_path, sizeof(aot_path),
                 "workloads/mem_histogram.aot.%s", arch_suffix());
        f = fopen(aot_path, "rb");
    }
    if (f) {
        fseek(f, 0, SEEK_END);
        hist_aot_len = (uint32_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        hist_aot = malloc(hist_aot_len);
        if (fread(hist_aot, 1, hist_aot_len, f) != hist_aot_len) {
            free(hist_aot); hist_aot = NULL; hist_aot_len = 0;
        }
        fclose(f);
    }

    /* Build VFS entries */
    int entry_count = 2; /* interp modules */
    if (hash_aot) entry_count++;
    if (hist_aot) entry_count++;

    HlEntry *entries = calloc((size_t)(entry_count + 1), sizeof(HlEntry));
    int ei = 0;

    /* Entries must be sorted by name (strcmp order) */
    if (hash_aot) {
        static char hash_aot_name[64];
        snprintf(hash_aot_name, sizeof(hash_aot_name),
                 "compute/compute_hash_aot.aot.%s", arch_suffix());
        entries[ei++] = (HlEntry){ hash_aot_name, hash_aot, hash_aot_len };
    }
    entries[ei++] = (HlEntry){ "compute/compute_hash_interp.wasm", hash_wasm, hash_wasm_len };
    if (hist_aot) {
        static char hist_aot_name[64];
        snprintf(hist_aot_name, sizeof(hist_aot_name),
                 "compute/mem_histogram_aot.aot.%s", arch_suffix());
        entries[ei++] = (HlEntry){ hist_aot_name, hist_aot, hist_aot_len };
    }
    entries[ei++] = (HlEntry){ "compute/mem_histogram_interp.wasm", hist_wasm, hist_wasm_len };
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

    if (hash_aot) printf("AOT: compute_hash loaded (%u bytes)\n", hash_aot_len);
    else          printf("AOT: compute_hash not available\n");
    if (hist_aot) printf("AOT: mem_histogram loaded (%u bytes)\n", hist_aot_len);
    else          printf("AOT: mem_histogram not available\n");

    /* Run workloads */
    run_workload("compute_hash", compute_hash_native,
                 "compute_hash_interp",
                 hash_aot ? "compute_hash_aot" : NULL,
                 &cache, &vfs, iters);

    run_workload("mem_histogram", mem_histogram_native,
                 "mem_histogram_interp",
                 hist_aot ? "mem_histogram_aot" : NULL,
                 &cache, &vfs, iters);

    /* Cleanup */
    hl_cap_wasm_destroy(&cache);
    free(entries);
    free(hash_wasm);
    free(hist_wasm);
    free(hash_aot);
    free(hist_aot);

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
