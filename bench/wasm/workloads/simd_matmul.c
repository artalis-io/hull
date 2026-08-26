/*
 * simd_matmul.c - WASM SIMD128 matrix multiply benchmark
 *
 * Computes C = A × B for square f32 matrices.
 * Input layout: [dim:u32] [A: dim×dim×f32] [B: dim×dim×f32]
 * Output: [C: dim×dim×f32]
 *
 * Uses wasm_simd128.h intrinsics for 4-wide f32 multiply-add in the
 * inner loop (k-dimension), accumulating 4 products per iteration.
 *
 * Compile with: clang --target=wasm32 -msimd128 -O2
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#endif

typedef unsigned int uint32_t;
typedef int int32_t;

/* Hull ABI: hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written */
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    if (in_len < 4)
        return -2;

    const unsigned char *in = (const unsigned char *)in_ptr;
    uint32_t dim = *(const uint32_t *)in;

    if (dim == 0)
        return -2;

    uint32_t mat_elems = dim * dim;
    int32_t expected_in = 4 + (int32_t)(mat_elems * 4 * 2);
    int32_t expected_out = (int32_t)(mat_elems * 4);

    if (in_len < expected_in || out_max < expected_out)
        return -2;

    const float *A = (const float *)(in + 4);
    const float *B = (const float *)(in + 4 + mat_elems * 4);
    float *C = (float *)out_ptr;

    /* C[i][j] = sum_k A[i][k] * B[k][j] */
    for (uint32_t i = 0; i < dim; i++) {
        for (uint32_t j = 0; j < dim; j++) {
#ifdef __wasm_simd128__
            v128_t sum_vec = wasm_f32x4_splat(0.0f);
            uint32_t k = 0;
            uint32_t simd_end = dim & ~3u;

            for (; k < simd_end; k += 4) {
                /* Load A[i][k..k+3] */
                v128_t va = wasm_v128_load(&A[i * dim + k]);
                /* Load B[k..k+3][j] - need to gather (not contiguous) */
                float bvals[4] = {
                    B[(k + 0) * dim + j],
                    B[(k + 1) * dim + j],
                    B[(k + 2) * dim + j],
                    B[(k + 3) * dim + j]
                };
                v128_t vb = wasm_v128_load(bvals);
                sum_vec = wasm_f32x4_add(sum_vec, wasm_f32x4_mul(va, vb));
            }

            /* Horizontal sum */
            float lanes[4];
            wasm_v128_store(lanes, sum_vec);
            float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];

            /* Scalar tail */
            for (; k < dim; k++)
                sum += A[i * dim + k] * B[k * dim + j];
#else
            float sum = 0.0f;
            for (uint32_t k = 0; k < dim; k++)
                sum += A[i * dim + k] * B[k * dim + j];
#endif
            C[i * dim + j] = sum;
        }
    }

    return expected_out;
}

int32_t hull_version(void) { return 1; }
