#include "mem_histogram_native.h"
#include <string.h>

int32_t mem_histogram_native(const void *in, size_t in_len,
                             void *out, size_t out_max)
{
    if (in_len > out_max)
        return -2;

    const uint8_t *data = (const uint8_t *)in;
    uint8_t *dst = (uint8_t *)out;

    /* Pass 1: Build histogram */
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (size_t i = 0; i < in_len; i++)
        hist[data[i]]++;

    /* Pass 2: Prefix sum (inclusive scan) */
    uint32_t sum = 0;
    for (int i = 0; i < 256; i++) {
        sum += hist[i];
        hist[i] = sum;
    }

    /* Pass 3: Counting sort (backwards for stability) */
    for (size_t i = in_len; i > 0; ) {
        i--;
        uint8_t byte = data[i];
        hist[byte]--;
        dst[hist[byte]] = byte;
    }

    return (int32_t)in_len;
}
