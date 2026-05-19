/**
 * @file module_registry.h
 * @brief Canonical registry of first-party Hull stdlib modules.
 *
 * Apps declare which stdlib modules they use via
 * `app.manifest({ modules = { crypto = "1", fs = "1", ... } })`.
 * The names in that table are looked up against `hl_module_registry`
 * by the resolver in `module_resolver.h` — unknown names, version
 * mismatches, missing capabilities, and undeclared internal deps are
 * all rejected before the guest code runs.
 *
 * The registry is a sorted (by `name`, `strcmp` order) static table.
 * `hl_module_registry_find` does an O(log n) binary search.
 *
 * @par Canonical naming
 *   Names use a `vendor/module` form (e.g. `hull/crypto`) so that
 *   third-party packages can later coexist (`acme/widgets`). In the
 *   manifest, the `hull/` prefix is implicit for first-party modules:
 *   `modules.crypto = "1"` resolves to `hull/crypto@1`.
 *
 * @par Versioning
 *   Each entry declares an `api_major`. The manifest specifies a
 *   single major version per module. Multiple majors of the same
 *   capability-bearing module cannot coexist.
 *
 * @par Intrinsic core
 *   Modules with `intrinsic = 1` are always available — no
 *   declaration needed. They are minimal infrastructure: route
 *   registration (`hull/app`), the logger (`hull/log`), and pure data
 *   codecs (`hull/json`). Every other module must be declared.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_MODULE_REGISTRY_H
#define HL_MODULE_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "hull/limits/core.h"  /* HL_MODULE_MAX_DEPS, HL_MODULE_NAME_MAX */

/* ── Capability-requirement bitmask ────────────────────────────────── */
/*
 * A side-effect module may require one or more manifest capability
 * sections to be non-empty. The resolver enforces these: declaring
 * `hull/http@1` without any `hosts` entries is an error.
 */

#define HL_MOD_CAP_FS      (1u << 0)  /* requires manifest.fs.read/write */
#define HL_MOD_CAP_HOSTS   (1u << 1)  /* requires manifest.hosts */
#define HL_MOD_CAP_ENV     (1u << 2)  /* requires manifest.env */
#define HL_MOD_CAP_DB      (1u << 3)  /* requires HL_ENABLE_DB at build */
#define HL_MOD_CAP_WASM    (1u << 4)  /* requires HL_ENABLE_WASM + compute */
#define HL_MOD_CAP_GPU     (1u << 5)  /* requires HL_ENABLE_GPU + gpu:true */
#define HL_MOD_CAP_HTTP    (1u << 6)  /* requires HL_ENABLE_HTTP at build */

/* ── Module spec ────────────────────────────────────────────────────── */

typedef struct HlModuleSpec {
    const char *name;                   /* canonical "hull/<x>" name */
    uint8_t     api_major;              /* API major version (1 for v1) */
    uint8_t     intrinsic;              /* 1 = always-on, no declaration */
    uint8_t     pure;                   /* 1 = no side effects (informational) */
    uint8_t     _pad;
    uint32_t    required_caps;          /* HL_MOD_CAP_* bitmask */

    /*
     * Internal first-party deps. Each must also be declared in the
     * app manifest (no implicit pull-in). NULL terminator marks end.
     */
    const char *deps[HL_MODULE_MAX_DEPS];
} HlModuleSpec;

/* ── Registry access ───────────────────────────────────────────────── */

/*
 * Look up a module by canonical name (e.g. "hull/crypto").
 * Returns a borrowed pointer into the static registry, or NULL if not
 * found. The pointer is stable for the lifetime of the process.
 */
const HlModuleSpec *hl_module_registry_find(const char *name);

/*
 * Look up a module by short name (e.g. "crypto") — prepends "hull/".
 * Strings already containing '/' are passed through unchanged.
 * Returns NULL if not found.
 */
const HlModuleSpec *hl_module_registry_find_short(const char *short_name);

/*
 * Look up a module by its runtime-syntax name and a separator char:
 *
 *   Lua: "hull.crypto"           sep='.' → "hull/crypto"
 *        "hull.middleware.session"      → "hull/middleware/session"
 *   JS:  "hull:crypto"           sep=':' → "hull/crypto"
 *        "hull:middleware:session"      → "hull/middleware/session"
 *
 * Used by the per-runtime require/import gating in module_resolver.h.
 * Returns NULL if the canonical name is not in the registry, or if the
 * normalized form would exceed HL_MODULE_NAME_MAX.
 */
const HlModuleSpec *hl_module_registry_find_runtime(const char *runtime_name,
                                                     char separator);

/*
 * Iterate the registry. `out_total` receives the entry count; the
 * return value points at index 0. Used by `hull modules available`.
 */
const HlModuleSpec *hl_module_registry_all(size_t *out_total);

/*
 * Convert a registry pointer back to its index (0..count-1).
 * Returns -1 if `spec` does not point into the registry.
 */
int hl_module_registry_index(const HlModuleSpec *spec);

/*
 * Total number of entries in the registry.
 */
size_t hl_module_registry_count(void);

/*
 * Format a comma-separated list of a spec's deps (stripped of the
 * `hull/` prefix where present) into `out`. Used by the runtime gates
 * to surface "this module also needs: X, Y, Z" in error messages.
 * Writes "" if the spec has no deps. Always NUL-terminates.
 */
void hl_module_registry_format_deps(const HlModuleSpec *spec,
                                     char *out, size_t cap);

/*
 * Find the registry entry whose name is closest (Levenshtein) to the
 * input. Used to suggest "did you mean `hull/crypto`?" when the user
 * mistypes a module name.
 *
 * `input` is matched against canonical names with the leading `hull/`
 * stripped — so callers should pass either the short form (e.g.
 * "cyrpto", "middleware/sesssion") or the full canonical form (the
 * `hull/` prefix is stripped automatically).
 *
 * Returns the closest match if its edit distance fits within an
 * input-length-scaled threshold (≤1 for very short names, ≤2 for
 * short, ≤3 for long). Returns NULL when nothing is close enough —
 * callers should fall back to the plain "see `hull modules available`"
 * message in that case.
 */
const HlModuleSpec *hl_module_registry_suggest(const char *input);

#endif /* HL_MODULE_REGISTRY_H */
