/*
 * stdlib_feature.c - composable seam for runtime-owned stdlib VFS entries.
 *
 * Base (weak) default: no runtime stdlib is composed, so the platform VFS is
 * just the runtime-agnostic base array. A `--with=<runtime>` compose (and the
 * `hull` toolchain, which force-loads both runtime archives) links a STRONG
 * hl_stdlib_feature_entries() override returning the composed runtime arrays.
 * Same weak-default + strong-override shape as hl_runtime_feature_factories.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/stdlib_feature.h"

#include <stdlib.h>

__attribute__((weak))
const HlEntry *const *hl_stdlib_feature_entries(size_t *count)
{
    if (count) *count = 0;
    return NULL;
}

void hl_platform_vfs_init(HlVfs *vfs, void **out_owned)
{
    extern const HlEntry hl_stdlib_entries[];
    size_t fc = 0;
    const HlEntry *const *feats = hl_stdlib_feature_entries(&fc);
    hl_vfs_init_composed(vfs, hl_stdlib_entries, feats, fc, NULL, out_owned);
}

void hl_platform_vfs_dispose(void *owned)
{
    hl_vfs_composed_free(owned);
}
