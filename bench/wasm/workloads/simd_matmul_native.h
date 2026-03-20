#ifndef SIMD_MATMUL_NATIVE_H
#define SIMD_MATMUL_NATIVE_H

#include <stddef.h>
#include <stdint.h>

/* Square f32 matrix multiply C = A × B.
 * Input: [dim:u32] [A: dim×dim×f32] [B: dim×dim×f32]
 * Output: [C: dim×dim×f32]
 * Returns bytes written (dim*dim*4) or -2 on error. */
int32_t simd_matmul_native(const void *in, size_t in_len,
                           void *out, size_t out_max);

#endif
