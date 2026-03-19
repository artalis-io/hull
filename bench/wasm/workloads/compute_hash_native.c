#include "compute_hash_native.h"
#include <string.h>

static inline uint32_t rotr32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

int32_t compute_hash_native(const void *in, size_t in_len,
                            void *out, size_t out_max)
{
    if (out_max < 32)
        return -2;

    const uint8_t *data = (const uint8_t *)in;

    /* Initial state (SHA-256 IV) */
    uint32_t s0 = 0x6a09e667, s1 = 0xbb67ae85;
    uint32_t s2 = 0x3c6ef372, s3 = 0xa54ff53a;
    uint32_t s4 = 0x510e527f, s5 = 0x9b05688c;
    uint32_t s6 = 0x1f83d9ab, s7 = 0x5be0cd19;

    /* Process each 64-byte block */
    size_t off = 0;
    while (off < in_len) {
        for (int round = 0; round < 64; round++) {
            /* Load message byte (wrap around) */
            uint32_t w = data[(off + (size_t)round) % in_len];

            /* Sigma0 */
            uint32_t a = rotr32(s0, 2) ^ rotr32(s0, 13) ^ rotr32(s0, 22);

            /* Sigma1 */
            uint32_t b = rotr32(s4, 6) ^ rotr32(s4, 11) ^ rotr32(s4, 25);

            /* Ch */
            uint32_t t = (s4 & s5) ^ (~s4 & s6);

            /* t1 = s7 + Sigma1 + Ch + w + round_constant */
            t = s7 + b + t + w + ((uint32_t)round * 0x5a827999u);

            /* Maj */
            a = a + ((s0 & s1) ^ (s0 & s2) ^ (s1 & s2));

            /* Rotate state */
            s7 = s6; s6 = s5; s5 = s4; s4 = s3 + t;
            s3 = s2; s2 = s1; s1 = s0; s0 = t + a;
        }
        off += 64;
    }

    /* Write state to output (little-endian, matching WASM i32.store) */
    uint32_t *out32 = (uint32_t *)out;
    out32[0] = s0; out32[1] = s1; out32[2] = s2; out32[3] = s3;
    out32[4] = s4; out32[5] = s5; out32[6] = s6; out32[7] = s7;

    return 32;
}
