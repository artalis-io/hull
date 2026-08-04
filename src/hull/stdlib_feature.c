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

/* hl_stdlib_feature_entries() is the composable seam. There is deliberately NO
 * weak default here: a weak default in this always-linked TU (hl_platform_vfs_init
 * below references the hook, so this file is always pulled) would be BOUND FIRST
 * from the base archive and SHADOW a strong override that lives in another
 * archive member - the archive-resolution rule pulls a member only to satisfy an
 * UNDEFINED symbol, and a weak def already satisfies it. That shadow is exactly
 * what shipped an empty runtime VFS ("module not found: hull.json") on the
 * composition-exempt cosmo base. Leaving the symbol UNDEFINED in the base turns a
 * forgotten override into a link-time "undefined reference" instead of a silent
 * boot failure. Providers today: a produced app's emitted registry, or the hull
 * toolchain registry. Every consumer that builds a platform VFS links one of
 * those; a future non-app consumer that ships no composed stdlib would add an
 * explicit strong empty (mirroring runtime/factory_none.c for the factory seam)
 * rather than reintroduce a shadowing weak default here. */

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
