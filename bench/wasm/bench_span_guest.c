/* bench_span_guest.c — the wasm32 guest for the mapped-span benchmark. Runs a
 * chosen workload REPS times over either the attached mapped span (mode SPAN) or a
 * host-provided linear-memory buffer (mode LINEAR, for the copy-once / chunked
 * baselines). The REPS loop makes NO host calls -- hull_span_setup issues its
 * SPAN_INFO host_call ONCE, before the loop -- so the two-point (t(1),t(K))
 * amortization isolates steady per-scan cost and the runtime host-call counter
 * stays flat across rep counts. Compiled with the SAME bench_span_ops.h body the
 * native baseline uses. See docs/mapped_span_benchmark_design.md.
 *
 * Input layout (little-endian):
 *   [0]      workload (0..3)
 *   [1]      mode (0 = SPAN, 1 = LINEAR)
 *   [2..9]   reps (u64)
 *   [10..]   LINEAR data (mode 1 only)
 * Output: u64 checksum (8 bytes, little-endian).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "hull_compute.h"
#include "hull_span.h"
#include "bench_span_ops.h"

#define BENCH_HDR 10

HULL_VERSION_EXPORT
HULL_EXPORT
int32_t hull_process(const void *in, int32_t in_len, void *out, int32_t out_max)
{
    const unsigned char *ip = (const unsigned char *)in;
    if (in_len < BENCH_HDR || out_max < 8)
        return HULL_ERR_OUTPUT;

    int workload = ip[0];
    int mode = ip[1];
    hull_span_u64 reps = 0;
    for (int i = 0; i < 8; i++)
        reps |= ((hull_span_u64)ip[2 + i]) << (8 * i);

    const void *w;
    hull_span_u64 len;
    if (mode == 0) {                 /* SPAN: read span 0's window in place */
        HullSpan spans[HULL_SPAN_MAX];
        int n = hull_span_setup(spans, HULL_SPAN_MAX);   /* the ONLY host_call(s) */
        if (n < 1)
            return -2;
        w = (const void *)(hull_span_uptr)spans[0].base;
        len = spans[0].len;
    }
    else {                           /* LINEAR: the data follows the header */
        w = ip + BENCH_HDR;
        len = (hull_span_u64)(in_len - BENCH_HDR);
    }

    hull_span_u64 sum = 0;
    for (hull_span_u64 r = 0; r < reps; r++)
        sum += bench_run(workload, w, len);

    unsigned char *op = (unsigned char *)out;
    for (int i = 0; i < 8; i++)
        op[i] = (unsigned char)(sum >> (8 * i));
    return 8;
}
