/**
 * @file module_resolver.h
 * @brief Resolve `manifest.modules` against the canonical registry.
 *
 * Takes a parsed @ref HlManifest and produces a frozen
 * #HlResolvedModuleSet - a bitset, indexed by registry position, of
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
 * Intrinsic-core modules (those with `intrinsic = 1` in the registry -
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
 *   3. The set has trivial lifetime - stack-allocated on the server
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
 * uint64s (see hull/limits/core.h) - 128 bits today gives headroom over
 * the current ~38-entry registry without forcing dynamic allocation.
 */
typedef struct HlResolvedModuleSet {
    uint64_t bits[HL_MODULE_BITSET_WORDS];
    /* Modules declared optional ("hull/gpu@1?") that were SKIPPED because a
     * build cap is absent. Distinct from `bits` (admitted) and from "never
     * declared" (in neither): the require/import gate consults this to return
     * nil/null gracefully instead of erroring. Parallel-indexed to `bits`. */
    uint64_t optional_absent[HL_MODULE_BITSET_WORDS];
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

/* True iff `spec` was declared optional but skipped (build cap absent). NULL →
 * false. The require/import gate uses this to yield nil/null instead of an error. */
bool hl_module_set_optional_absent_spec(const HlResolvedModuleSet *set,
                                         const HlModuleSpec *spec);

/* Same, by canonical name. NULL → false. */
bool hl_module_set_optional_absent_name(const HlResolvedModuleSet *set,
                                         const char *canonical_name);

/* Number of bits set. */
int hl_module_set_count(const HlResolvedModuleSet *set);

/* Reset to empty (no modules - not even intrinsics). */
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
 * `out` are unspecified after an error - discard.
 *
 * On success: returns 0; `out` contains the frozen set.
 */
int hl_module_resolver_resolve(const HlManifest *manifest,
                                HlResolvedModuleSet *out,
                                char *errbuf, size_t errlen);

/*
 * Compile-time capabilities THIS hull build provides, as an HL_MOD_CAP_*
 * bitset (DB / WASM / GPU / HTTP_SERVER / HTTP_CLIENT / TUI). This is what
 * the plain resolver validates declared modules against.
 */
uint32_t hl_module_build_caps(void);

/*
 * Build cap an optional composable feature provides (HL_MOD_CAP_* or 0).
 * `hull build --with=<feature>` passes its composed features here so the base
 * build-tool's resolver admits the modules the feature will supply. Only
 * features that carry a module-gated capability map to a nonzero cap (today:
 * "gpu" -> HL_MOD_CAP_GPU; "duckdb" -> 0, it rides on the always-present hull/db).
 */
uint32_t hl_module_feature_cap(const char *name);

/*
 * True iff `spec` requires a build-time subsystem cap (DB/WASM/GPU/HTTP/TUI)
 * that THIS binary lacks - compiled out and not composed as a feature. The JS
 * loader uses it to tell a build-cap-optional top-level import (return a null
 * stub, defer the decision to the resolver + import tracker) from a genuine
 * unresolvable import (throw now): a module absent for OTHER reasons (e.g. a db
 * runtime not configured) still errors at import.
 */
bool hl_module_needs_absent_build_cap(const HlModuleSpec *spec);

/*
 * Resolve against an EXPLICIT build-cap set instead of this binary's
 * compiled-in flags. Used by `hull build --flavor`, which must validate an
 * app's manifest against the TARGET flavor's caps (e.g. reject hull/web/
 * ws-server when building --flavor=pure-compute) even though the running
 * hull is full-HTTP. hl_module_resolver_resolve() == this with
 * hl_module_build_caps().
 */
int hl_module_resolver_resolve_caps(const HlManifest *manifest,
                                     HlResolvedModuleSet *out,
                                     uint32_t build_caps,
                                     char *errbuf, size_t errlen);

/* ── Build flavors ─────────────────────────────────────────────────────
 *
 * A build flavor is a named, finite cut of the platform's capability set,
 * carried by a correspondingly-compiled libhull_platform.a. `hull build
 * --flavor=<name>` selects one. The registry is the single source of truth
 * (resolver target-caps, platform-lib asset name, doctor/agent listing).
 * See docs/build_flavors.md.
 */
typedef struct {
    const char *name;        /* "pure-compute" */
    uint32_t    clear_caps;  /* HL_MOD_CAP_* this flavor turns OFF vs full */
    const char *asset;       /* platform-lib stem, "libhull_platform-<flavor>" */
} HlBuildFlavor;

/* Look up a flavor by name; NULL if unknown. "full" is a zero-clear entry. */
const HlBuildFlavor *hl_build_flavor_find(const char *name);

/* Enumerate the flavor table. Returns count; writes the table base to *out. */
int hl_build_flavor_all(const HlBuildFlavor **out);

/* Target build-caps for a flavor: base with the flavor's clear_caps removed.
 * `base` is normally hl_module_build_caps() (the running hull's caps). */
uint32_t hl_build_flavor_caps(const HlBuildFlavor *f, uint32_t base);

/* Aggregate required-caps (HL_MOD_CAP_* OR) of every module in a resolved
 * set. The HTTP_SERVER / HTTP_CLIENT bits are what distinguish the flavors. */
uint32_t hl_module_set_required_caps(const HlResolvedModuleSet *set);

/* Pick the minimal flavor that still provides every cap in `needed_caps`:
 * the flavor clearing the most caps without clearing a needed one. Always
 * returns at least "full". Drives `hull build --flavor=auto`. */
const HlBuildFlavor *hl_build_flavor_auto(uint32_t needed_caps);

/* ── Pre-manifest import tracker ───────────────────────────────────── */

/*
 * Forward decl of HlRuntime - this header is shared by runtime/lua/ and
 * runtime/js/ which both build against the same HlRuntime base struct.
 */
typedef struct HlRuntime HlRuntime;

/*
 * Record a hull:* / hull.X canonical name that the per-runtime require/
 * import gate let through while the runtime's `module_set` was still
 * NULL (top-level imports before app.manifest() runs).
 *
 * Dedups against the existing tracker contents. Silently truncates at
 * HL_MANIFEST_MAX_MODULES - no app can legitimately import more
 * registry-known modules than the manifest itself can declare.
 *
 * `canonical_name` must outlive the runtime (point into the registry
 * or another static string - the tracker stores the pointer, not a copy).
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
