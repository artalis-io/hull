/*
 * bench_gpu.c — GPU vs WASM vs native benchmark
 *
 * Compares cosine similarity computation across:
 *   1. Native C (direct function call)
 *   2. WAMR interpreter (.wasm)
 *   3. GPU compute (wgpu-native, WGSL shader)
 *
 * Tests multiple vector counts to show where GPU parallelism wins.
 *
 * Build & run:
 *   make bench-gpu HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#if !defined(HL_ENABLE_GPU) || !defined(HL_ENABLE_WASM)
#include <stdio.h>
int main(void) {
    fprintf(stderr, "bench_gpu requires HL_ENABLE_GPU=1 and HL_ENABLE_WASM\n");
    return 1;
}
#else

#include "hull/cap/gpu.h"
#include "hull/cap/wasm.h"
#include "hull/limits.h"
#include "hull/vfs.h"
#include "hull/entry.h"
#include "cosine_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ── Configuration ─────────────────────────────────────────────────── */

#define DEFAULT_ITERS   100
#define WARMUP_ITERS    5
#define DIMENSIONS      128

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
    double p99;
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
    s.median = (double)samples[n / 2];
    s.p99 = (double)samples[(int)(n * 0.99)];
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)samples[i];
    s.mean = sum / n;
    return s;
}

/* ── Input generation (deterministic xorshift32) ──────────────────── */

static void generate_vectors(float *query, float *candidates,
                              uint32_t dims, uint32_t count)
{
    uint32_t x = 42;
    for (uint32_t d = 0; d < dims; d++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        query[d] = (float)(int32_t)x / (float)INT32_MAX;
    }
    for (uint32_t c = 0; c < count; c++) {
        for (uint32_t d = 0; d < dims; d++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            candidates[c * dims + d] = (float)(int32_t)x / (float)INT32_MAX;
        }
    }
}

/* ── WGSL shader (same as examples/gpu_search) ────────────────────── */

static const char COSINE_WGSL[] =
    "struct Params { dimensions: u32, count: u32 }\n"
    "@group(0) @binding(0) var<uniform> params: Params;\n"
    "@group(0) @binding(1) var<storage, read> query: array<f32>;\n"
    "@group(0) @binding(2) var<storage, read> candidates: array<f32>;\n"
    "@group(0) @binding(3) var<storage, read_write> results: array<f32>;\n"
    "@compute @workgroup_size(64)\n"
    "fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "    let idx = gid.x;\n"
    "    if (idx >= params.count) { return; }\n"
    "    let offset = idx * params.dimensions;\n"
    "    var dot_prod: f32 = 0.0;\n"
    "    var norm_q: f32 = 0.0;\n"
    "    var norm_c: f32 = 0.0;\n"
    "    for (var i: u32 = 0u; i < params.dimensions; i = i + 1u) {\n"
    "        let q = query[i];\n"
    "        let c = candidates[offset + i];\n"
    "        dot_prod = dot_prod + q * c;\n"
    "        norm_q = norm_q + q * q;\n"
    "        norm_c = norm_c + c * c;\n"
    "    }\n"
    "    let denom = sqrt(norm_q) * sqrt(norm_c);\n"
    "    if (denom > 0.0) { results[idx] = dot_prod / denom; }\n"
    "    else { results[idx] = 0.0; }\n"
    "}\n";

/* ── Benchmark: Native ─────────────────────────────────────────────── */

static void bench_native(uint32_t dims, uint32_t count, int iters,
                          uint64_t *samples, float *ref_results)
{
    float *query = malloc(dims * 4);
    float *candidates = malloc((size_t)count * dims * 4);
    generate_vectors(query, candidates, dims, count);

    /* Pack input: [dims:u32] [count:u32] [query] [candidates] */
    size_t in_len = 8 + (size_t)dims * 4 + (size_t)count * dims * 4;
    uint8_t *input = malloc(in_len);
    memcpy(input, &dims, 4);
    memcpy(input + 4, &count, 4);
    memcpy(input + 8, query, dims * 4);
    memcpy(input + 8 + dims * 4, candidates, (size_t)count * dims * 4);

    size_t out_max = (size_t)count * 4;
    float *output = malloc(out_max);

    /* Warmup */
    for (int i = 0; i < WARMUP_ITERS; i++)
        cosine_native(input, in_len, output, out_max);

    /* Reference output */
    cosine_native(input, in_len, output, out_max);
    memcpy(ref_results, output, count * 4);

    /* Timed */
    for (int i = 0; i < iters; i++) {
        uint64_t t0 = now_ns();
        cosine_native(input, in_len, output, out_max);
        uint64_t t1 = now_ns();
        samples[i] = t1 - t0;
    }

    free(output);
    free(input);
    free(candidates);
    free(query);
}

/* ── Benchmark: WASM ───────────────────────────────────────────────── */

static void bench_wasm(HlWasmCache *cache, const char *module_name,
                        uint32_t dims, uint32_t count, int iters,
                        uint64_t *samples, const float *ref_results)
{
    float *query = malloc(dims * 4);
    float *candidates = malloc((size_t)count * dims * 4);
    generate_vectors(query, candidates, dims, count);

    size_t in_len = 8 + (size_t)dims * 4 + (size_t)count * dims * 4;
    uint8_t *input = malloc(in_len);
    memcpy(input, &dims, 4);
    memcpy(input + 4, &count, 4);
    memcpy(input + 8, query, dims * 4);
    memcpy(input + 8 + dims * 4, candidates, (size_t)count * dims * 4);

    HlWasmCallOpts opts = {0};
    opts.max_input = in_len + 1024;
    opts.max_output = (size_t)count * 4 + 1024;
    /* Size heap to fit input+output. Pool threshold is 4MB — smaller workloads
     * get pooled, larger ones pay instance creation cost (realistic). */
    size_t heap_need = in_len + (size_t)count * 4 + 256 * 1024; /* data + 256K overhead */
    if (heap_need < 2 * 1024 * 1024) heap_need = 2 * 1024 * 1024;
    opts.heap_size = (uint32_t)heap_need;
    opts.gas = 2000000000LL; /* 2B instructions — enough for 64K × 128-dim */

    /* Use dummy VFS — module already cached in wasm_cache */
    HlEntry dummy[] = {{ NULL, NULL, 0 }};
    HlVfs dvfs;
    hl_vfs_init(&dvfs, dummy, NULL);

    /* Warmup */
    for (int i = 0; i < WARMUP_ITERS; i++) {
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        hl_cap_wasm_call(cache, module_name, input, in_len,
                         &out, &out_len, &opts, NULL, NULL, &dvfs, NULL, NULL, &err);
        free(out);
    }

    /* Verify */
    {
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        int rc = hl_cap_wasm_call(cache, module_name, input, in_len,
                                  &out, &out_len, &opts, NULL, NULL, &dvfs, NULL, NULL, &err);
        if (rc != 0) {
            fprintf(stderr, "  WASM %s call failed: %s\n", module_name, err ? err : "unknown");
            free(out); free(input); free(candidates); free(query);
            return;
        }
        const float *wasm_results = (const float *)out;
        int mismatch = 0;
        for (uint32_t c = 0; c < count && !mismatch; c++) {
            float diff = ref_results[c] - wasm_results[c];
            if (diff < 0) diff = -diff;
            if (diff > 1e-3f) mismatch = 1;
        }
        if (mismatch)
            fprintf(stderr, "  WARNING: WASM %s output differs from native\n", module_name);
        free(out);
    }

    /* Timed */
    for (int i = 0; i < iters; i++) {
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        uint64_t t0 = now_ns();
        hl_cap_wasm_call(cache, module_name, input, in_len,
                         &out, &out_len, &opts, NULL, NULL, &dvfs, NULL, NULL, &err);
        uint64_t t1 = now_ns();
        samples[i] = t1 - t0;
        free(out);
    }

    free(input);
    free(candidates);
    free(query);
}

/* ── Benchmark: GPU ────────────────────────────────────────────────── */

static void bench_gpu(HlGpuCtx *gpu_ctx, uint32_t dims, uint32_t count,
                       int iters, uint64_t *samples, const float *ref_results)
{
    float *query = malloc(dims * 4);
    float *candidates = malloc((size_t)count * dims * 4);
    generate_vectors(query, candidates, dims, count);

    /* Create persistent candidate buffer */
    hl_cap_gpu_buffer_create(gpu_ctx, -1, "bench_candidates",
                              (size_t)count * dims * 4, HL_GPU_USAGE_READ);
    hl_cap_gpu_buffer_write(gpu_ctx, -1, "bench_candidates",
                             candidates, (size_t)count * dims * 4, 0);

    /* Pack uniform: { dims, count } padded to 16 bytes */
    uint32_t uniforms[4] = { dims, count, 0, 0 };

    uint32_t wg_x = (count + 63) / 64;

    HlGpuBufferDesc bufs[3] = {
        { .data = query, .size = dims * 4, .usage = HL_GPU_USAGE_READ, .binding = -1 },
        { .name = "bench_candidates", .usage = HL_GPU_USAGE_READ, .binding = -1 },
        { .size = count * 4, .usage = HL_GPU_USAGE_READWRITE, .binding = -1 },
    };
    HlGpuDispatchOpts opts = {
        .uniforms = uniforms,
        .uniforms_len = 16,
        .buffers = bufs,
        .buffer_count = 3,
        .workgroups = { wg_x, 1, 1 },
        .output_buffer = 2,
        .device = -1,
    };

    /* Warmup */
    for (int i = 0; i < WARMUP_ITERS; i++) {
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        hl_cap_gpu_dispatch(gpu_ctx, "cosine_bench", &opts, &out, &out_len, &err);
        free(out);
    }

    /* Verify */
    {
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        int rc = hl_cap_gpu_dispatch(gpu_ctx, "cosine_bench", &opts,
                                      &out, &out_len, &err);
        if (rc != HL_GPU_OK) {
            fprintf(stderr, "  GPU dispatch failed: %s\n", err ? err : "unknown");
            free(out);
            hl_cap_gpu_buffer_destroy(gpu_ctx, -1, "bench_candidates");
            free(candidates); free(query);
            return;
        }
        const float *gpu_results = (const float *)out;
        int mismatch = 0;
        for (uint32_t c = 0; c < count && !mismatch; c++) {
            float diff = ref_results[c] - gpu_results[c];
            if (diff < 0) diff = -diff;
            /* GPU f32 accumulation order differs from scalar C — use generous tolerance */
            if (diff > 5e-2f) mismatch = 1;
        }
        if (mismatch)
            fprintf(stderr, "  WARNING: GPU output differs from native (>5e-2 tolerance)\n");
        free(out);
    }

    /* Timed */
    for (int i = 0; i < iters; i++) {
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        uint64_t t0 = now_ns();
        hl_cap_gpu_dispatch(gpu_ctx, "cosine_bench", &opts, &out, &out_len, &err);
        uint64_t t1 = now_ns();
        samples[i] = t1 - t0;
        free(out);
    }

    hl_cap_gpu_buffer_destroy(gpu_ctx, -1, "bench_candidates");
    free(candidates);
    free(query);
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int iters = DEFAULT_ITERS;
    const char *env = getenv("BENCH_ITERS");
    if (env) { int n = atoi(env); if (n > 0) iters = n; }

    printf("=== Hull GPU vs WASM Benchmark ===\n");
    printf("Workload: cosine similarity (%d-dim vectors)\n", DIMENSIONS);
    printf("Iterations: %d (warmup: %d)\n\n", iters, WARMUP_ITERS);

    /* ── Initialize GPU ────────────────────────────────────────────── */

    HlGpuCtx gpu_ctx;
    int gpu_ok = 0;
    if (hl_cap_gpu_init(&gpu_ctx, &hl_gpu_backend_wgpu) == HL_GPU_OK
        && hl_cap_gpu_available(&gpu_ctx)) {
        printf("GPU: %s\n", hl_cap_gpu_device_name(&gpu_ctx, 0));

        /* Compile shader */
        int rc = hl_cap_gpu_compile(&gpu_ctx, -1, "cosine_bench",
                                     COSINE_WGSL, strlen(COSINE_WGSL));
        if (rc == HL_GPU_OK) {
            gpu_ok = 1;
        } else {
            fprintf(stderr, "GPU shader compile failed\n");
        }
    } else {
        printf("GPU: not available (skipping GPU benchmarks)\n");
    }

    /* ── Initialize WASM ───────────────────────────────────────────── */

    HlWasmCache wasm_cache;
    int wasm_interp_ok = 0, wasm_aot_ok = 0;
    uint8_t *wasm_data = NULL, *aot_data = NULL;

    if (hl_cap_wasm_init(&wasm_cache) == 0) {
        /* Load cosine.wasm (interpreter) */
        FILE *f = fopen("bench/gpu/cosine.wasm", "rb");
        if (!f) f = fopen("cosine.wasm", "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            uint32_t len = (uint32_t)ftell(f);
            fseek(f, 0, SEEK_SET);
            wasm_data = malloc(len);
            if (fread(wasm_data, 1, len, f) == len) {
                HlEntry entries[] = {
                    { "compute/cosine_interp.wasm", wasm_data, len },
                    { NULL, NULL, 0 },
                };
                HlVfs vfs;
                hl_vfs_init(&vfs, entries, NULL);
                if (hl_cap_wasm_load(&wasm_cache, "cosine_interp", &vfs, NULL) == 0) {
                    wasm_interp_ok = 1;
                    printf("WASM interp: loaded cosine.wasm (%u bytes)\n", len);
                }
            }
            fclose(f);
        }

        /* Load AOT (.aot.ARCH) */
        {
            char aot_path[512];
            const char *arch =
#if defined(__aarch64__)
                "aarch64";
#elif defined(__x86_64__)
                "x86_64";
#else
                "unknown";
#endif
            snprintf(aot_path, sizeof(aot_path), "bench/gpu/cosine.aot.%s", arch);
            FILE *af = fopen(aot_path, "rb");
            if (!af) {
                snprintf(aot_path, sizeof(aot_path), "cosine.aot.%s", arch);
                af = fopen(aot_path, "rb");
            }
            if (af) {
                fseek(af, 0, SEEK_END);
                uint32_t alen = (uint32_t)ftell(af);
                fseek(af, 0, SEEK_SET);
                aot_data = malloc(alen);
                if (fread(aot_data, 1, alen, af) == alen) {
                    char vfs_name[256];
                    snprintf(vfs_name, sizeof(vfs_name),
                             "compute/cosine_aot.aot.%s", arch);
                    HlEntry entries[] = {
                        { vfs_name, aot_data, alen },
                        { NULL, NULL, 0 },
                    };
                    HlVfs vfs;
                    hl_vfs_init(&vfs, entries, NULL);
                    if (hl_cap_wasm_load(&wasm_cache, "cosine_aot", &vfs, NULL) == 0) {
                        wasm_aot_ok = 1;
                        printf("WASM AOT:    loaded %s (%u bytes)\n", aot_path, alen);
                    }
                }
                fclose(af);
            }
            if (!wasm_aot_ok)
                printf("WASM AOT:    not available (compile with: wamrc -o bench/gpu/cosine.aot.%s bench/gpu/cosine.wasm)\n", arch);
        }
    } else {
        printf("WASM: init failed\n");
    }

    printf("\n");

    /* ── Run benchmarks at different scales ─────────────────────────── */

    static const struct { const char *label; uint32_t count; } sizes[] = {
        { "64",      64 },
        { "256",     256 },
        { "1K",      1024 },
        { "4K",      4096 },
        { "16K",     16384 },
        { "64K",     65536 },
    };
    int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    uint64_t *samples = malloc((size_t)iters * sizeof(uint64_t));

    printf("%-8s  %12s  %12s  %12s  %12s  %12s\n",
           "Vectors", "Native (us)", "Interp (us)", "AOT (us)", "GPU (us)", "GPU vs AOT");
    printf("%-8s  %12s  %12s  %12s  %12s  %12s\n",
           "-------", "-----------", "-----------", "--------", "--------", "----------");

    for (int si = 0; si < nsizes; si++) {
        uint32_t count = sizes[si].count;
        float *ref = malloc(count * 4);

        /* Native */
        bench_native(DIMENSIONS, count, iters, samples, ref);
        Stats s_native = compute_stats(samples, iters);

        /* WASM interpreter */
        Stats s_interp = {0};
        if (wasm_interp_ok) {
            bench_wasm(&wasm_cache, "cosine_interp",
                       DIMENSIONS, count, iters, samples, ref);
            s_interp = compute_stats(samples, iters);
        }

        /* WASM AOT */
        Stats s_aot = {0};
        if (wasm_aot_ok) {
            bench_wasm(&wasm_cache, "cosine_aot",
                       DIMENSIONS, count, iters, samples, ref);
            s_aot = compute_stats(samples, iters);
        }

        /* GPU */
        Stats s_gpu = {0};
        if (gpu_ok) {
            bench_gpu(&gpu_ctx, DIMENSIONS, count, iters, samples, ref);
            s_gpu = compute_stats(samples, iters);
        }

        /* Print row */
        printf("%-8s  %9.0f   ", sizes[si].label, s_native.median / 1000.0);
        if (wasm_interp_ok)
            printf("  %9.0f   ", s_interp.median / 1000.0);
        else
            printf("  %12s", "n/a");
        if (wasm_aot_ok)
            printf("  %9.0f   ", s_aot.median / 1000.0);
        else
            printf("  %12s", "n/a");
        if (gpu_ok)
            printf("  %9.0f   ", s_gpu.median / 1000.0);
        else
            printf("  %12s", "n/a");

        /* Speedup: GPU vs AOT (or vs native if no AOT) */
        if (gpu_ok && s_gpu.median > 0) {
            double ref_time = wasm_aot_ok ? s_aot.median : s_native.median;
            printf("  %9.1fx", ref_time / s_gpu.median);
        }
        printf("\n");

        free(ref);
    }

    printf("\n");

    /* Cleanup */
    free(samples);
    if (gpu_ok) hl_cap_gpu_destroy(&gpu_ctx);
    if (wasm_interp_ok || wasm_aot_ok) hl_cap_wasm_destroy(&wasm_cache);
    free(wasm_data);
    free(aot_data);

    return 0;
}

#endif /* HL_ENABLE_GPU && HL_ENABLE_WASM */
