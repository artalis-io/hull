#ifndef SIMD_DOT_PRODUCT_NATIVE_H
#define SIMD_DOT_PRODUCT_NATIVE_H

#include <stddef.h>
#include <stdint.h>

/* Dot product of two f32 vectors.
 * Input: [n:u32] [vec_a: n×f32] [vec_b: n×f32]
 * Output: [result: f64]
 * Returns 8 (bytes written) or -2 on error. */
int32_t simd_dot_product_native(const void *in, size_t in_len,
                                void *out, size_t out_max);

#endif
