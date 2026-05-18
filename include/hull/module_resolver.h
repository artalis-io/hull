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

#endif /* HL_MODULE_RESOLVER_H */
