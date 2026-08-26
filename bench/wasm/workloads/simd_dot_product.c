/*
 * simd_dot_product.c - WASM SIMD128 dot product benchmark
 *
 * Computes dot product of two f32 vectors packed in the input buffer.
 * Input layout: [n:u32] [vec_a: n×f32] [vec_b: n×f32]
 * Output: [result: f64]
 *
 * Uses wasm_simd128.h intrinsics for 4-wide f32 multiply-add.
 * Compile with: clang --target=wasm32 -msimd128 -O2
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#endif

typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long uint64_t;

/* Hull ABI: hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written */
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    if (out_max < 8)
        return -2;
    if (in_len < 4)
        return -2;

    const unsigned char *in = (const unsigned char *)in_ptr;

    /* Read element count (little-endian u32) */
    uint32_t n = *(const uint32_t *)in;
    int32_t expected = 4 + (int32_t)(n * 4 * 2); /* header + 2 vectors */
    if (in_len < expected)
        return -2;

    const float *a = (const float *)(in + 4);
    const float *b = (const float *)(in + 4 + n * 4);

#ifdef __wasm_simd128__
    /* SIMD path: process 4 floats at a time */
    v128_t sum_vec = wasm_f32x4_splat(0.0f);
    uint32_t i = 0;
    uint32_t simd_end = n & ~3u; /* round down to multiple of 4 */

    for (; i < simd_end; i += 4) {
        v128_t va = wasm_v128_load(&a[i]);
        v128_t vb = wasm_v128_load(&b[i]);
        sum_vec = wasm_f32x4_add(sum_vec, wasm_f32x4_mul(va, vb));
    }

    /* Horizontal sum of 4 lanes */
    float lanes[4];
    wasm_v128_store(lanes, sum_vec);
    double sum = (double)lanes[0] + (double)lanes[1] +
                 (double)lanes[2] + (double)lanes[3];

    /* Scalar tail */
    for (; i < n; i++)
        sum += (double)a[i] * (double)b[i];
#else
    /* Scalar fallback */
    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++)
        sum += (double)a[i] * (double)b[i];
#endif

    *(double *)out_ptr = sum;
    return 8;
}

int32_t hull_version(void) { return 1; }
