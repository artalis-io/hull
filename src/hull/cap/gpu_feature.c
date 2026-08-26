/*
 * cap/gpu_feature.c - composable-feature hook for the GPU backend.
 *
 * Base-resident weak default: a base build carries no GPU backend, so this
 * returns none. A `hull build --with=gpu` build links a STRONG override (a
 * generated registry) that returns the wgpu backend; serve.c then hands it to
 * hl_cap_gpu_init. Mirrors hl_db_feature_backends in cap/db_select.c. Always
 * compiled (picked up by the cap-directory wildcard) so the symbol always resolves
 * whether or not GPU is present.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/gpu.h"

__attribute__((weak))
const HlGpuBackend *const *hl_gpu_feature_backends(size_t *count)
{
    if (count) *count = 0;
    return NULL;
}
