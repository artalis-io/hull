/*
 * cosine_native.h - Native C cosine similarity for benchmark comparison
 *
 * Computes cosine similarity between a query vector and N candidate vectors.
 * Input format:  [dimensions:u32] [count:u32] [query: dims×f32] [candidates: count×dims×f32]
 * Output format: [results: count×f32]
 */

#ifndef COSINE_NATIVE_H
#define COSINE_NATIVE_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int32_t cosine_native(const void *in, size_t in_len,
                              void *out, size_t out_max)
{
    if (in_len < 8) return -1;
    const uint8_t *p = (const uint8_t *)in;

    uint32_t dims, count;
    memcpy(&dims, p, 4);
    memcpy(&count, p + 4, 4);

    size_t query_bytes = (size_t)dims * 4;
    size_t cand_bytes = (size_t)count * dims * 4;
    size_t expected = 8 + query_bytes + cand_bytes;
    if (in_len < expected) return -1;
    if ((size_t)count * 4 > out_max) return -1;

    const float *query = (const float *)(p + 8);
    const float *candidates = (const float *)(p + 8 + query_bytes);
    float *results = (float *)out;

    for (uint32_t c = 0; c < count; c++) {
        const float *cand = candidates + (size_t)c * dims;
        float dot = 0.0f, nq = 0.0f, nc = 0.0f;
        for (uint32_t d = 0; d < dims; d++) {
            dot += query[d] * cand[d];
            nq += query[d] * query[d];
            nc += cand[d] * cand[d];
        }
        float denom = sqrtf(nq) * sqrtf(nc);
        results[c] = (denom > 0.0f) ? dot / denom : 0.0f;
    }

    return (int32_t)(count * 4);
}

#endif /* COSINE_NATIVE_H */
