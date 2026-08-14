/* segread.c — minimal fixture: reports the loaded compute.segment count and the
 * first byte of segment 0, via hull_compute.h's segment host-calls (no
 * hull_span.h, memcpy-free). Segments are WAMR shared heaps, so this exercises
 * the same AOT shared-heap path as mapped spans (#326). Output: byte 0 = segment
 * count, byte 1 = segment0[0] (only when count > 0).
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "hull_compute.h"
HULL_VERSION_EXPORT
HULL_EXPORT
int32_t hull_process(const void *in, int32_t in_len, void *out, int32_t out_max)
{
    (void)in; (void)in_len;
    unsigned char *o = (unsigned char *)out;
    if (out_max < 1) return HULL_ERR_OUTPUT;
    int32_t n = hull_segment_count();
    o[0] = (unsigned char)n;
    if (n <= 0) return 1;
    const unsigned char *seg = (const unsigned char *)hull_segment_addr(0);
    if (out_max < 2 || !seg) return 1;
    o[1] = seg[0];                 /* bounded read of segment 0's first byte */
    return 2;
}
