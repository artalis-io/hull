#include "simd_matmul_native.h"
#include <string.h>

int32_t simd_matmul_native(const void *in, size_t in_len,
                           void *out, size_t out_max)
{
    if (in_len < 4)
        return -2;

    const unsigned char *p = (const unsigned char *)in;
    uint32_t dim;
    memcpy(&dim, p, 4);

    if (dim == 0)
        return -2;

    size_t mat_elems = (size_t)dim * dim;
    size_t expected_in = 4 + mat_elems * 4 * 2;
    size_t expected_out = mat_elems * 4;

    if (in_len < expected_in || out_max < expected_out)
        return -2;

    const float *A = (const float *)(p + 4);
    const float *B = (const float *)(p + 4 + mat_elems * 4);
    float *C = (float *)out;

    for (uint32_t i = 0; i < dim; i++) {
        for (uint32_t j = 0; j < dim; j++) {
            float sum = 0.0f;
            for (uint32_t k = 0; k < dim; k++)
                sum += A[i * dim + k] * B[k * dim + j];
            C[i * dim + j] = sum;
        }
    }

    return (int32_t)expected_out;
}
