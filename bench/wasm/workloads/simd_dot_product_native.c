#include "simd_dot_product_native.h"
#include <string.h>

int32_t simd_dot_product_native(const void *in, size_t in_len,
                                void *out, size_t out_max)
{
    if (out_max < 8 || in_len < 4)
        return -2;

    const unsigned char *p = (const unsigned char *)in;
    uint32_t n;
    memcpy(&n, p, 4);

    size_t expected = 4 + (size_t)n * 4 * 2;
    if (in_len < expected)
        return -2;

    const float *a = (const float *)(p + 4);
    const float *b = (const float *)(p + 4 + n * 4);

    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++)
        sum += (double)a[i] * (double)b[i];

    memcpy(out, &sum, 8);
    return 8;
}
