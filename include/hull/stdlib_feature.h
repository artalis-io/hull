/**
 * @file stdlib_feature.h
 * @brief Composable seam for runtime-owned stdlib VFS entries.
 *
 * The base platform lib carries only the runtime-agnostic stdlib entries
 * (`context/`, `static/`, `templates/`) in `hl_stdlib_entries[]`. Each runtime
 * composed as a feature (`--with=lua` / `--with=js`, and the `hull` toolchain
 * which force-loads both) contributes its own module set through the weak
 * `hl_stdlib_feature_entries()` hook - the exact analog of
 * `hl_runtime_feature_factories()` for the runtime factory registry.
 *
 * The base defines the weak default (returns none). A composed build links a
 * strong override (the generated feature registry / the toolchain registry)
 * returning the composed runtime arrays. `hl_platform_vfs_init()` unions the
 * base with whatever the hook reports.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_STDLIB_FEATURE_H
#define HL_STDLIB_FEATURE_H

#include "hull/vfs.h"
#include <stddef.h>

/**
 * @brief Runtime-owned stdlib entry arrays composed as features.
 *
 * Weak default returns 0 arrays (runtime-agnostic base build). A strong
 * override returns one sentinel-terminated `HlEntry[]` per composed runtime.
 *
 * @param[out] count Receives the number of arrays returned.
 * @return Array of `*count` sentinel-terminated entry arrays, or NULL.
 */
const HlEntry *const *hl_stdlib_feature_entries(size_t *count);

/**
 * @brief Initialize a platform VFS over the base stdlib unioned with every
 *        composed runtime's stdlib (via #hl_stdlib_feature_entries).
 *
 * Thin wrapper over #hl_vfs_init_composed: passes `hl_stdlib_entries[]` as the
 * base and the hook's arrays as the features. `root_dir` is NULL (stdlib is
 * always embedded). Free the returned owned array with #hl_platform_vfs_dispose.
 *
 * @param vfs       Storage to initialize.
 * @param out_owned Receives an opaque handle (the sealed arena) to free, or NULL
 *                  when the static base was borrowed (no runtime feature).
 */
void hl_platform_vfs_init(HlVfs *vfs, void **out_owned);

/** @brief Free an opaque handle from #hl_platform_vfs_init (NULL-safe). */
void hl_platform_vfs_dispose(void *owned);

#endif /* HL_STDLIB_FEATURE_H */
