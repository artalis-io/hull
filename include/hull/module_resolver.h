/**
 * @file module_resolver.h
 * @brief Resolve `manifest.modules` against the canonical registry.
 *
 * Takes a parsed @ref HlManifest and produces a frozen
 * #HlResolvedModuleSet — a bitset, indexed by registry position, of
 * the modules an app is allowed to import at runtime. The resolver
 * fails closed on:
 *
 *   - unknown module name
 *   - unsupported API major version
 *   - duplicate declaration of the same module name
 *   - side-effect module without its required manifest capability
 *     (e.g. `hull/http@1` declared with no `hosts = {...}`)
 *   - undeclared internal first-party dep
 *     (e.g. `hull/jwt@1` declared but not `hull/crypto@1`)
 *   - declaration of a module that depends on a compile-time-disabled
 *     subsystem (HL_ENABLE_DB / WASM / GPU)
 *
 * Intrinsic-core modules (those with `intrinsic = 1` in the registry —
 * `hull/app`, `hull/log`, `hull/json`) are seeded automatically; the
 * app does not need to declare them, but the resolver still adds their
 * bits to the resolved set.
 *
 * @par Lifecycle
 *   1. main.c calls `hl_module_resolver_resolve()` after manifest
 *      extraction, before the kernel sandbox is sealed.
 *   2. The resulting set is borrowed by the runtime gating layer
 *      (Lua/JS module loaders, WASM host_call) for fast checks via
 *      `hl_module_set_contains()`.
 *   3. The set has trivial lifetime — stack-allocated on the server
 *      state. No free required.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_MODULE_RESOLVER_H
#define HL_MODULE_RESOLVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "hull/limits/core.h"     /* HL_MODULE_BITSET_WORDS */
#include "hull/module_registry.h"

/* Forward declarations */
typedef struct HlManifest HlManifest;

/* ── Resolved module set ───────────────────────────────────────────── */

/*
 * Bitset indexed by registry position. Width is HL_MODULE_BITSET_WORDS
 * uint64s (see hull/limits/core.h) — 128 bits today gives headroom over
 * the current ~38-entry registry without forcing dynamic allocation.
 */
typedef struct HlResolvedModuleSet {
    uint64_t bits[HL_MODULE_BITSET_WORDS];
} HlResolvedModuleSet;

/* ── Set operations ────────────────────────────────────────────────── */

/* True iff registry[index] is in the set. Out-of-range indices → false. */
bool hl_module_set_contains_index(const HlResolvedModuleSet *set, int index);

/* True iff `spec` is in the set. NULL → false. */
bool hl_module_set_contains_spec(const HlResolvedModuleSet *set,
                                  const HlModuleSpec *spec);

/* True iff a canonical name is in the set. NULL → false. */
bool hl_module_set_contains_name(const HlResolvedModuleSet *set,
                                  const char *canonical_name);

/* True iff a short or canonical name is in the set. */
bool hl_module_set_contains_short(const HlResolvedModuleSet *set,
                                   const char *name);

/* Number of bits set. */
int hl_module_set_count(const HlResolvedModuleSet *set);

/* Reset to empty (no modules — not even intrinsics). */
void hl_module_set_clear(HlResolvedModuleSet *set);

/* ── Resolver ──────────────────────────────────────────────────────── */

/*
 * Resolve manifest.modules into `out`.
 *
 * Always seeds the intrinsic core first. If `manifest` is NULL or its
 * `modules_declared` bit is 0, only the intrinsic core is admitted.
 *
 * On error: writes a human-readable message into `errbuf` (NUL-
 * terminated; truncated to `errlen`) and returns -1. The contents of
 * `out` are unspecified after an error — discard.
 *
 * On success: returns 0; `out` contains the frozen set.
 */
int hl_module_resolver_resolve(const HlManifest *manifest,
                                HlResolvedModuleSet *out,
                                char *errbuf, size_t errlen);

/* ── Pre-manifest import tracker ───────────────────────────────────── */

/*
 * Forward decl of HlRuntime — this header is shared by runtime/lua/ and
 * runtime/js/ which both build against the same HlRuntime base struct.
 */
typedef struct HlRuntime HlRuntime;

/*
 * Record a hull:* / hull.X canonical name that the per-runtime require/
 * import gate let through while the runtime's `module_set` was still
 * NULL (top-level imports before app.manifest() runs).
 *
 * Dedups against the existing tracker contents. Silently truncates at
 * HL_MANIFEST_MAX_MODULES — no app can legitimately import more
 * registry-known modules than the manifest itself can declare.
 *
 * `canonical_name` must outlive the runtime (point into the registry
 * or another static string — the tracker stores the pointer, not a copy).
 */
void hl_import_tracker_record(HlRuntime *rt, const char *canonical_name);

/*
 * After the resolver wires `rt->module_set`, validate that every
 * tracked top-level import was admitted by the resolved set.
 *
 * Returns 0 if all tracked imports are admitted, -1 with a formatted
 * message in errbuf otherwise. The message names the first missing
 * import and counts the rest.
 */
int hl_import_tracker_validate(const HlRuntime *rt,
                                const HlResolvedModuleSet *set,
                                char *errbuf, size_t errlen);

#endif /* HL_MODULE_RESOLVER_H */
