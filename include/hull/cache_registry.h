/**
 * @file cache_registry.h
 * @brief Compile-time registry of every blob_store-backed cache Hull
 *        manages on this host.
 *
 * Single source of truth for `hull cache list|prune|clear`, future
 * `hull doctor` cache reporting (§1.5.b-X-6), and `hull inspect`
 * cache disclosure (§1.5.b-X-7). Each kind names a subtree under
 * `$HOME/.hull/` plus metadata: human description, whether it's a
 * runtime (sweep-by-default) cache or a durable system store
 * (preserve on prune).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CACHE_REGISTRY_H
#define HL_CACHE_REGISTRY_H

#include <stddef.h>

/**
 * @brief One row of the cache registry.
 *
 * Per-kind metadata used by every cache management surface. Pointer
 * is stable for the lifetime of the process; iterate via
 * `hl_cache_registry()` and walk until `name == NULL`.
 */
typedef struct {
    /** Short identifier (e.g. "lua-bytecode"). Validated as
     *  `[A-Za-z0-9_-]+`; safe to interpolate into filesystem paths. */
    const char *name;

    /** One-line human description for `cache list` and doctor. */
    const char *description;

    /** 1 = runtime cache. Subject to `HULL_CACHE_DIR` override and
     *      swept by default in `cache prune`. Path:
     *        $HULL_CACHE_DIR/<name>/        if override is set
     *        $HOME/.hull/blobs/runtime/<name>/  otherwise
     *  0 = system store. Always under `$HOME/.hull/blobs/<name>/`,
     *      ignored by `cache prune`, only touched by explicit
     *      `cache clear --kind=<name>`. Used for durable signed
     *      artifacts (tools install). */
    int is_runtime;
} HlCacheKind;

/**
 * @brief Return the NULL-terminated static cache registry.
 *
 * Pointer is stable; iterate until `entry->name == NULL`.
 */
const HlCacheKind *hl_cache_registry(void);

/**
 * @brief Look up a cache kind by name. NULL if not registered.
 */
const HlCacheKind *hl_cache_find(const char *name);

/**
 * @brief Resolve the absolute path for @p kind.
 *
 * Composes `$HOME/.hull/<subpath>`. Does NOT create the directory
 * (the blob_store opens it on first use). Returns 0 on success,
 * -1 if HOME is unset or the buffer is too small.
 */
int hl_cache_resolve_path(const HlCacheKind *kind,
                          char *out, size_t out_sz);

#endif /* HL_CACHE_REGISTRY_H */
