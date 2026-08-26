/* spancount.c - minimal test fixture: reports the attached mapped-span count and
 * the first byte of span 0's window, via the RAW host_call(SPAN_INFO) ABI (no
 * hull_span.h, so it needs no freestanding memcpy - #327 is independent). Output:
 * byte 0 = span count, byte 1 = window[0] (only when count > 0). Proves the sync
 * compute.call / instance:call bindings forward spans (#325).
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "hull_compute.h"
HULL_VERSION_EXPORT
HULL_EXPORT
int32_t hull_process(const void *in, int32_t in_len, void *out, int32_t out_max)
{
    (void)in; (void)in_len;
    unsigned char *o = (unsigned char *)out;
    if (out_max < 1) return HULL_ERR_OUTPUT;
    int32_t count = host_call(0x04, 0, -1);      /* SPAN_INFO count query */
    o[0] = (unsigned char)count;
    if (count <= 0) return 1;
    unsigned char rec[96];
    rec[2] = 96; rec[3] = 0;                      /* advertise cbSize = 96 */
    int32_t r = host_call(0x04, (int32_t)(unsigned)(size_t)rec, 0);  /* record idx 0 */
    if (out_max < 2) return 1;
    if (r != 96) { o[1] = 0xff; return 2; }
    unsigned long base = 0;
    int i = 0;
    while (i < 4) { base |= ((unsigned long)rec[72 + i]) << (8 * i); i++; }
    const unsigned char *w = (const unsigned char *)(size_t)base;
    o[1] = w[0];                                   /* bounded read of window[0] */
    return 2;
}
