/* spandiff.c — WASM guest for the hull_span.h native-vs-WASM differential test
 * (#324 3b). Includes the canonical SDK headers and dispatches one fixture per
 * call through the shared spandiff_run (spandiff_ops.h) — the exact same body the
 * native side runs. Uses only the pure, host_call-free SDK ops, so the module
 * imports nothing and loads in a bare WAMR harness on interpreter and AOT.
 *
 * Built from the canonical hull_compute.h + hull_span.h with the exact
 * `hull compute build` flags; regenerate with build_spandiff.sh.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "hull_compute.h"
#include "hull_span.h"
#include "spandiff_ops.h"

HULL_VERSION_EXPORT
HULL_EXPORT
int32_t hull_process(const void *in, int32_t in_len, void *out, int32_t out_max)
{
    if (in_len < 0 || out_max < 0) return HULL_ERR_INPUT;
    int r = spandiff_run((const hull_span_u8 *)in, (hull_span_u32)in_len,
                         (hull_span_u8 *)out, (hull_span_u32)out_max);
    return (int32_t)r;
}
