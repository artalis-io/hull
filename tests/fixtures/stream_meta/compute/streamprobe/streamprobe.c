/* streamprobe.c - reports the per-chunk stream metadata the host exposes via the
 * public hull_stream_* SDK helpers. Output is 3 bytes per chunk:
 *   [0] = hull_stream_is_first()   [1] = hull_stream_is_last()   [2] = hull_stream_chunk_index()
 * For an ordinary (non-stream) compute.call these are all 0 (host default).
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "hull_compute.h"

HULL_VERSION_EXPORT
HULL_EXPORT
int32_t hull_process(const void *in, int32_t in_len, void *out, int32_t out_max)
{
    (void)in; (void)in_len;
    if (out_max < 3) return HULL_ERR_OUTPUT;
    unsigned char *o = (unsigned char *)out;
    o[0] = (unsigned char)hull_stream_is_first();
    o[1] = (unsigned char)hull_stream_is_last();
    o[2] = (unsigned char)hull_stream_chunk_index();
    return 3;
}
