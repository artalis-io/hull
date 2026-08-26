/* memops.c - #327 fixture. Exercises the canonical header's bare
 * memcpy/memset/memmove via BOTH compiler-generated lowering (runtime-length
 * byte loops) AND direct calls, covering every tricky case the reviewer asked
 * for: memmove in both overlap directions, identical pointers, and zero length;
 * memcpy at misaligned source/destination offsets (disjoint only - memcpy keeps
 * the standard no-overlap contract); memset at a misaligned destination.
 *
 * The module only PERFORMS the operations and copies each result into the output
 * buffer; the harness (tests/hull/cap/test_wasm_memops.c and
 * tests/e2e_compute_memops.sh) holds the expected bytes and verifies. Nothing is
 * compared inside the module, so no memcmp/bcmp is emitted - the module stays
 * self-contained (no libc/host imports at all).
 *
 * Output is 103 bytes:
 *   [0 ..7]   memmove forward overlap  (dst>src): {0..7}, memmove(a+2,a,6)
 *   [8 ..15]  memmove backward overlap (dst<src): {0..7}, memmove(b,b+2,6)
 *   [16..19]  memmove identical pointers:         {9,8,7,6}, memmove(c,c,4)
 *   [20..21]  memmove zero length (no write):     {5,6}, memmove(d+1,d,0)
 *   [22..30]  memcpy misaligned disjoint:         memcpy(dst+1, src+2, 5)
 *   [31..38]  memset misaligned:                  memset(e+1, 0xAB, 5)
 *   [39..102] compiler-generated copy+fill of the input prefix (len clamped 64)
 *
 * Built from the canonical hull_compute.h with the exact `hull compute build`
 * flags; regenerate with tests/fixtures/compute/build_memops.sh.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "hull_compute.h"

#define BLK 64
#define OUT_LEN (39 + BLK)

HULL_VERSION_EXPORT
HULL_EXPORT
int32_t hull_process(const void *in, int32_t in_len, void *out, int32_t out_max)
{
    if (out_max < OUT_LEN) return HULL_ERR_OUTPUT;
    uint8_t *o = (uint8_t *)out;
    const uint8_t *s = (const uint8_t *)in;

    /* memmove forward overlap (dst > src): shift right by 2. */
    { uint8_t a[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
      memmove(a + 2, a, 6);
      memcpy(o + 0, a, 8); }

    /* memmove backward overlap (dst < src): shift left by 2. */
    { uint8_t b[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
      memmove(b, b + 2, 6);
      memcpy(o + 8, b, 8); }

    /* memmove identical pointers: must be a no-op. */
    { uint8_t c[4] = { 9, 8, 7, 6 };
      memmove(c, c, 4);
      memcpy(o + 16, c, 4); }

    /* memmove zero length: must not write the destination. */
    { uint8_t d[2] = { 5, 6 };
      memmove(d + 1, d, 0);
      memcpy(o + 20, d, 2); }

    /* memcpy misaligned + disjoint: src offset 2, dst offset 1, 5 bytes. */
    { uint8_t src[9], dst[9];
      for (int i = 0; i < 9; i++) { src[i] = (uint8_t)(i * 11 + 1); dst[i] = 0; }
      memcpy(dst + 1, src + 2, 5);
      memcpy(o + 22, dst, 9); }

    /* memset misaligned: fill e[1..5] with 0xAB, leave e[0], e[6..7]. */
    { uint8_t e[8];
      for (int i = 0; i < 8; i++) e[i] = 0x11;
      memset(e + 1, 0xAB, 5);
      memcpy(o + 31, e, 8); }

    /* Compiler-generated memcpy + memset via runtime-length loops: copy the
     * input prefix (clamped to BLK), zero-fill the tail. */
    { int n = in_len; if (n > BLK) n = BLK; if (n < 0) n = 0;
      uint8_t g[BLK];
      for (int i = 0; i < n; i++) g[i] = s[i];        /* -> memcpy */
      for (int i = n; i < BLK; i++) g[i] = 0;         /* -> memset */
      memcpy(o + 39, g, BLK); }

    return OUT_LEN;
}
