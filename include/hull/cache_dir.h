/**
 * @file cache_dir.h
 * @brief Hull runtime cache directory helpers ($HOME/.hull/cache/).
 *
 * Runtime caches (Lua bytecode, compute AOT artifacts, template AST
 * compilations) live OUTSIDE the per-app sandbox manifest in a
 * shared system-wide pool — they're runtime infrastructure, not part
 * of the app's I/O surface (see docs/blob.md §Runtime-infrastructure
 * caches for the manifest-line rationale).
 *
 * The sandbox auto-allows the cache root via the same path it
 * auto-allows the embedded CA bundle and SQLite WAL/SHM siblings —
 * apps can't opt out per-app, but the global `HULL_NO_CACHE=1` env
 * var disables every consumer at runtime.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CACHE_DIR_H
#define HL_CACHE_DIR_H

#include <stddef.h>

/**
 * @brief Resolve the runtime-cache root, creating all layers.
 *
 * Resolution order:
 *   1. `HULL_CACHE_DIR` env (must be absolute) — for per-app cache
 *      isolation in shared / multi-tenant deployments. Set per-
 *      service in systemd / k8s / Docker and the entire runtime
 *      cache pool lives under that path with no `$HOME` exposure.
 *   2. `$HOME/.hull/blobs/runtime/` — default shared pool.
 *
 * Returns a path WITH trailing slash for easy concatenation.
 *
 * The name retains "cache" terminology because that's the
 * user-facing intent — the directories under this root are caches
 * even though the disk layout is now the blob shape (shared with
 * hull/blob@1 via hl_blob_store_*).
 *
 * @param out      Caller-supplied buffer (≥ PATH_MAX bytes recommended).
 * @param out_sz   Buffer size.
 * @return 0 on success; -1 on HOME unset (with no HULL_CACHE_DIR),
 *         invalid override path, or mkdir failure.
 */
int hl_hull_cache_dir(char *out, size_t out_sz);

/**
 * @brief Resolve `$HOME/.hull/blobs/runtime/<name>/`, creating all
 *        layers.
 *
 * @param name     Cache subdir name (e.g. "lua-bytecode",
 *                 "compute-aot"). Validated: must match
 *                 `[A-Za-z0-9_-]+`.
 * @param out      Caller-supplied buffer.
 * @param out_sz   Buffer size.
 * @return 0 on success; -1 on invalid name, $HOME unset, or mkdir
 *         failure.
 */
int hl_hull_cache_subdir(const char *name, char *out, size_t out_sz);

/**
 * @brief Check whether runtime caches are globally disabled.
 *
 * Honors:
 *   - `HULL_NO_CACHE=1`         — disables everything
 *   - `HULL_NO_BYTECODE_CACHE=1` — disables Lua bytecode cache only
 *   - `HULL_NO_AOT_CACHE=1`     — disables compute AOT cache only
 *   - `HULL_NO_TEMPLATE_CACHE=1` — disables template AST cache only
 *
 * Pass NULL for `kind` to check only the global switch.
 *
 * @param kind  Per-cache specifier ("bytecode", "aot", "template"),
 *              or NULL for global-only.
 * @return Non-zero if caches (of the specified kind) are disabled.
 */
int hl_hull_cache_disabled(const char *kind);

#endif /* HL_CACHE_DIR_H */
