/*
 * hash.c — FNV-1a 64-bit hash
 *
 * Input:  arbitrary bytes
 * Output: 8 bytes uint64 LE (FNV-1a hash)
 *
 * Build:  hull compute build hash
 * Test:   hull compute test hash
 */

#include "hull_compute.h"

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME  0x100000001b3ULL

HULL_VERSION_EXPORT

HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    if (out_max < 8) return HULL_ERR_OUTPUT;

    const unsigned char *data = (const unsigned char *)in_ptr;
    uint64_t hash = FNV_OFFSET;

    for (int32_t i = 0; i < in_len; i++) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }

    /* Write uint64 LE output */
    unsigned char *out = (unsigned char *)out_ptr;
    for (int i = 0; i < 8; i++)
        out[i] = (unsigned char)(hash >> (i * 8));

    return 8;
}
