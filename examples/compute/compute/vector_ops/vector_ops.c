/*
 * vector_ops.c - Cosine similarity between two float32 vectors
 *
 * Input format:
 *   4 bytes  - uint32 LE: vector dimension N
 *   N×4 bytes - float32 LE: vector A
 *   N×4 bytes - float32 LE: vector B
 *
 * Output: 4 bytes float32 LE (cosine similarity score)
 *
 * Build:  hull compute build vector_ops
 * Test:   hull compute test vector_ops
 */

#include "hull_compute.h"

HULL_VERSION_EXPORT

HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    if (out_max < 4) return HULL_ERR_OUTPUT;
    if (in_len < 4) return HULL_ERR_INPUT;

    const unsigned char *in = (const unsigned char *)in_ptr;

    /* Read dimension (first 4 bytes, little-endian uint32) */
    uint32_t dim = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                   ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);

    if (dim == 0) return HULL_ERR_INPUT;

    int32_t expected = 4 + (int32_t)(dim * 4 * 2);
    if (in_len < expected) return HULL_ERR_INPUT;

    const float *a = (const float *)(in + 4);
    const float *b = (const float *)(in + 4 + dim * 4);

    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        dot    += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    /* Avoid division by zero */
    float denom = norm_a * norm_b;
    float result;
    if (denom <= 0.0f) {
        result = 0.0f;
    } else {
        float sq = __builtin_sqrtf(denom);
        result = dot / sq;
    }

    /* Write float32 LE output */
    unsigned char *out = (unsigned char *)out_ptr;
    hull_memcpy(out, &result, 4);
    return 4;
}
