/* bench_span_ops.h — the FOUR mapped-span benchmark workloads, as ONE body compiled
 * on TWO targets: natively (in the harness TU) and as a wasm32 guest
 * (bench_span_guest.c). Reads go through the shipped hull_span.h accessors so every
 * implementation (native mmap / HullSpan AOT / copy-once / chunked) exercises the
 * SAME inlineable, bounds-checked code path -- the only difference is what `w`
 * points at. Deterministic: same bytes + same access order => identical checksum
 * across implementations (the correctness gate). See docs/mapped_span_benchmark_design.md.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef BENCH_SPAN_OPS_H
#define BENCH_SPAN_OPS_H

#include "hull_span.h"   /* dual-target typed accessors; -Itemplates */

enum {
    BW_SEQ_BYTES = 0,   /* sum every u8                        */
    BW_SEQ_WORDS = 1,   /* sum le u32 stride, then le u64 stride */
    BW_RANDOM    = 2,   /* fixed-LCG walk of len/64 offsets    */
    BW_PARSER    = 3,   /* length-prefixed record walk         */
    BW_COUNT     = 4
};

/* One deterministic pass over [w, w+len); returns a u64 checksum. No host imports. */
static inline hull_span_u64 bench_run(int workload, const void *w, hull_span_u64 len)
{
    hull_span_u64 sum = 0;

    if (workload == BW_SEQ_BYTES) {
        for (hull_span_u64 o = 0; o < len; o++) {
            hull_span_u8 v;
            if (hull_span_read_u8(w, len, o, &v)) break;
            sum += v;
        }
    }
    else if (workload == BW_SEQ_WORDS) {
        hull_span_u64 o;
        for (o = 0; o + 4 <= len; o += 4) {
            hull_span_u32 v;
            if (hull_span_read_u32le(w, len, o, &v)) break;
            sum += v;
        }
        for (o = 0; o + 8 <= len; o += 8) {
            hull_span_u64 v;
            if (hull_span_read_u64le(w, len, o, &v)) break;
            sum += v;
        }
    }
    else if (workload == BW_RANDOM) {
        /* Fixed LCG => identical offset sequence on every run and every impl. */
        hull_span_u64 x = 0x2545F4914F6CDD1DULL;      /* constant seed */
        hull_span_u64 span = (len >= 4) ? (len - 3) : 1;
        hull_span_u64 n = len / 64; if (n == 0) n = 1;
        for (hull_span_u64 i = 0; i < n; i++) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            hull_span_u64 off = (x >> 11) % span;
            hull_span_u32 v;
            if (hull_span_read_u32le(w, len, off, &v)) continue;
            sum += v;
        }
    }
    else /* BW_PARSER: read a length, checksum that many bytes, advance. Record
          * lengths are derived from the data (1..64) so random bytes form a valid,
          * deterministic record stream -- the OSM/Parquet-shaped access pattern. */ {
        hull_span_u64 o = 0;
        while (o + 4 <= len) {
            hull_span_u32 raw;
            if (hull_span_read_u32le(w, len, o, &raw)) break;
            o += 4;
            hull_span_u64 rlen = (hull_span_u64)(raw & 63u) + 1u;
            hull_span_u64 end = o + rlen;
            if (end > len || end < o) break;
            for (hull_span_u64 k = o; k < end; k++) {
                hull_span_u8 b;
                if (hull_span_read_u8(w, len, k, &b)) break;
                sum += b;
            }
            o = end;
        }
    }

    return sum;
}

#endif /* BENCH_SPAN_OPS_H */
