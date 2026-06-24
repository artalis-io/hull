/*
 * module_resolver.c — Validate manifest.modules against the registry.
 *
 * Failure modes (all surface as a one-line error string in `errbuf`):
 *   - unknown module name
 *   - api_major mismatch
 *   - duplicate declaration
 *   - required capability section absent (fs/hosts/env)
 *   - module requires compile-time subsystem that is disabled
 *     (HL_ENABLE_DB / WASM / GPU)
 *   - internal dep not also declared
 *
 * Errors fire before the kernel sandbox is sealed; main.c is expected
 * to bail out of startup on -1.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/module_resolver.h"
#include "hull/manifest.h"
#include "hull/runtime.h"  /* HlRuntime — import_tracker fields */

#include <stdio.h>
#include <string.h>

/* ── Bitset operations ─────────────────────────────────────────────── */

#define BIT_WORD(i)   ((unsigned)(i) / 64u)
#define BIT_MASK(i)   ((uint64_t)1 << ((unsigned)(i) % 64u))
#define MAX_BITS      (HL_MODULE_BITSET_WORDS * 64)

static void set_bit(HlResolvedModuleSet *s, int i)
{
    if (i < 0 || i >= (int)MAX_BITS) return;
    s->bits[BIT_WORD(i)] |= BIT_MASK(i);
}

static bool get_bit(const HlResolvedModuleSet *s, int i)
{
    if (!s || i < 0 || i >= (int)MAX_BITS) return false;
    return (s->bits[BIT_WORD(i)] & BIT_MASK(i)) != 0;
}

void hl_module_set_clear(HlResolvedModuleSet *s)
{
    if (s) memset(s, 0, sizeof(*s));
}

bool hl_module_set_contains_index(const HlResolvedModuleSet *s, int i)
{
    return get_bit(s, i);
}

bool hl_module_set_contains_spec(const HlResolvedModuleSet *s,
                                  const HlModuleSpec *spec)
{
    return get_bit(s, hl_module_registry_index(spec));
}

bool hl_module_set_contains_name(const HlResolvedModuleSet *s,
                                  const char *canonical_name)
{
    return hl_module_set_contains_spec(s, hl_module_registry_find(canonical_name));
}

bool hl_module_set_contains_short(const HlResolvedModuleSet *s,
                                   const char *name)
{
    return hl_module_set_contains_spec(s, hl_module_registry_find_short(name));
}

int hl_module_set_count(const HlResolvedModuleSet *s)
{
    if (!s) return 0;
    int n = 0;
    for (int w = 0; w < HL_MODULE_BITSET_WORDS; w++) {
        uint64_t v = s->bits[w];
        while (v) { v &= v - 1; n++; }
    }
    return n;
}

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Each ERR_* macro writes a single formatted error message into errbuf
 * (NUL-terminated, truncated to errlen) and evaluates to a statement.
 * Using macros over a varargs helper keeps every format string literal,
 * so the printf-format warning fires at the actual call site if a
 * mismatched format ever creeps in. */
#define ERR0(fmt) \
    do { if (errbuf && errlen) snprintf(errbuf, errlen, fmt); } while (0)
#define ERR1(fmt, a) \
    do { if (errbuf && errlen) snprintf(errbuf, errlen, fmt, (a)); } while (0)
#define ERR2(fmt, a, b) \
    do { if (errbuf && errlen) snprintf(errbuf, errlen, fmt, (a), (b)); } while (0)
#define ERR3(fmt, a, b, c) \
    do { if (errbuf && errlen) snprintf(errbuf, errlen, fmt, (a), (b), (c)); } while (0)
#define ERR4(fmt, a, b, c, d) \
    do { if (errbuf && errlen) snprintf(errbuf, errlen, fmt, (a), (b), (c), (d)); } while (0)

/* Capabilities the *build* provides (compile-time flags). */
static uint32_t build_provided_caps(void)
{
    uint32_t caps = 0;
#ifdef HL_ENABLE_DB
    caps |= HL_MOD_CAP_DB;
#endif
#ifdef HL_ENABLE_WASM
    caps |= HL_MOD_CAP_WASM;
#endif
#ifdef HL_ENABLE_GPU
    caps |= HL_MOD_CAP_GPU;
#endif
#ifdef HL_ENABLE_HTTP_CLIENT
    caps |= HL_MOD_CAP_HTTP_CLIENT;
#endif
#ifdef HL_ENABLE_HTTP_SERVER
    caps |= HL_MOD_CAP_HTTP_SERVER;
#endif
#ifdef HL_ENABLE_TUI
    caps |= HL_MOD_CAP_TUI;
#endif
    return caps;
}

/* Human-readable name for a capability bit (for error messages). */
static const char *cap_label(uint32_t cap)
{
    switch (cap) {
    case HL_MOD_CAP_FS:          return "fs.read or fs.write";
    case HL_MOD_CAP_HOSTS:       return "hosts";
    case HL_MOD_CAP_ENV:         return "env";
    case HL_MOD_CAP_DB:          return "HL_ENABLE_DB (build-time)";
    case HL_MOD_CAP_WASM:        return "HL_ENABLE_WASM (build-time)";
    case HL_MOD_CAP_GPU:         return "HL_ENABLE_GPU (build-time)";
    case HL_MOD_CAP_HTTP_CLIENT: return "HL_ENABLE_HTTP_CLIENT (build-time)";
    case HL_MOD_CAP_HTTP_SERVER: return "HL_ENABLE_HTTP_SERVER (build-time)";
    case HL_MOD_CAP_HTTP:        return "HL_ENABLE_HTTP (build-time)";
    case HL_MOD_CAP_TUI:         return "HL_ENABLE_TUI (build-time)";
    default:                     return "unknown";
    }
}

uint32_t hl_module_build_caps(void)
{
    return build_provided_caps();
}

/* ── Build flavors ─────────────────────────────────────────────────────
 *
 * Each flavor clears a subset of the build caps vs `full`. The asset stem
 * names the matching libhull_platform-<flavor>.a. Single source of truth
 * for the resolver target-caps, the platform-lib lookup, and listing.
 * Note: HL_MOD_CAP_HTTP == HTTP_CLIENT | HTTP_SERVER (the alias). */
static const HlBuildFlavor BUILD_FLAVORS[] = {
    { "full",         0,                      "libhull_platform" },
    { "server-only",  HL_MOD_CAP_HTTP_CLIENT, "libhull_platform-server-only" },
    { "client-only",  HL_MOD_CAP_HTTP_SERVER, "libhull_platform-client-only" },
    { "pure-compute", HL_MOD_CAP_HTTP,        "libhull_platform-pure-compute" },
};

const HlBuildFlavor *hl_build_flavor_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(BUILD_FLAVORS) / sizeof(BUILD_FLAVORS[0]); i++) {
        if (strcmp(BUILD_FLAVORS[i].name, name) == 0)
            return &BUILD_FLAVORS[i];
    }
    return NULL;
}

int hl_build_flavor_all(const HlBuildFlavor **out)
{
    if (out) *out = BUILD_FLAVORS;
    return (int)(sizeof(BUILD_FLAVORS) / sizeof(BUILD_FLAVORS[0]));
}

uint32_t hl_build_flavor_caps(const HlBuildFlavor *f, uint32_t base)
{
    return f ? (base & ~f->clear_caps) : base;
}

uint32_t hl_module_set_required_caps(const HlResolvedModuleSet *set)
{
    if (!set) return 0;
    uint32_t caps = 0;
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);
    for (size_t i = 0; i < total; i++) {
        if (hl_module_set_contains_index(set, (int)i))
            caps |= all[i].required_caps;
    }
    return caps;
}

/* Popcount without relying on a builtin (cosmo/older toolchains). */
static int popcount32(uint32_t v)
{
    int n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}

const HlBuildFlavor *hl_build_flavor_auto(uint32_t needed_caps)
{
    /* Pick the flavor clearing the most caps without clearing one the app
     * needs. Flavors only differ in the HTTP_SERVER / HTTP_CLIENT bits, so
     * only those of `needed_caps` matter here; "full" (clears nothing) is
     * always a valid fallback. */
    const HlBuildFlavor *best = NULL;
    int best_bits = -1;
    size_t n = sizeof(BUILD_FLAVORS) / sizeof(BUILD_FLAVORS[0]);
    for (size_t i = 0; i < n; i++) {
        const HlBuildFlavor *f = &BUILD_FLAVORS[i];
        if (f->clear_caps & needed_caps) continue;   /* would drop a needed cap */
        int bits = popcount32(f->clear_caps);
        if (bits > best_bits) { best_bits = bits; best = f; }
    }
    return best;
}

/* ── The resolver ──────────────────────────────────────────────────── */

int hl_module_resolver_resolve(const HlManifest *manifest,
                                HlResolvedModuleSet *out,
                                char *errbuf, size_t errlen)
{
    return hl_module_resolver_resolve_caps(manifest, out,
                                           build_provided_caps(),
                                           errbuf, errlen);
}

int hl_module_resolver_resolve_caps(const HlManifest *manifest,
                                     HlResolvedModuleSet *out,
                                     uint32_t build_caps,
                                     char *errbuf, size_t errlen)
{
    if (!out) {
        ERR0("module resolver: null output");
        return -1;
    }
    hl_module_set_clear(out);

    /* Always seed the intrinsic core, even with no manifest. */
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);
    for (size_t i = 0; i < total; i++) {
        if (all[i].intrinsic) set_bit(out, (int)i);
    }

    /* No manifest, or manifest with no `modules` key: intrinsic only. */
    if (!manifest || !manifest->modules_declared) return 0;

    const uint32_t prov_build    = build_caps;
    const uint32_t build_cap_mask = HL_MOD_CAP_DB | HL_MOD_CAP_WASM
                                    | HL_MOD_CAP_GPU | HL_MOD_CAP_HTTP
                                    | HL_MOD_CAP_TUI;

    /* Pass 1: look up each declared module + admit it. Detect unknown
     * names, version mismatches, duplicates, missing capabilities. */
    for (int i = 0; i < manifest->modules_count; i++) {
        const HlManifestModule *m = &manifest->modules[i];
        const HlModuleSpec *spec = hl_module_registry_find_short(m->name);

        if (!spec) {
            /* v0.2.0 rename table: explicit fix-it for the 20 modules
             * moved into the hull/web/ namespace. Hits before the
             * fuzzy-suggest path so users see "renamed to X in v0.2.0"
             * instead of the more vague "did you mean X?". Entries can
             * be removed after a few releases once the migration
             * window closes.
             *
             * Ordering note: this fires before the api_major check
             * below — a user declaring `cookie@99` gets the rename
             * hint, not a version-mismatch error. Intentional: the
             * rename is the dominant cause of name resolution failure
             * in v0.2.0; a misleading version mismatch would push the
             * user toward fixing the wrong thing. */
            static const struct { const char *old; const char *new; }
                V0_2_0_RENAMES[] = {
                {"hull/cookie",                   "hull/web/cookie"},
                {"hull/form",                     "hull/web/form"},
                {"hull/htmx",                     "hull/web/htmx"},
                {"hull/middleware/auth",          "hull/web/middleware/auth"},
                {"hull/middleware/cors",          "hull/web/middleware/cors"},
                {"hull/middleware/csp",           "hull/web/middleware/csp"},
                {"hull/middleware/csrf",          "hull/web/middleware/csrf"},
                {"hull/middleware/etag",          "hull/web/middleware/etag"},
                {"hull/middleware/health",        "hull/web/middleware/health"},
                {"hull/middleware/idempotency",   "hull/web/middleware/idempotency"},
                {"hull/middleware/inbox",         "hull/web/middleware/inbox"},
                {"hull/middleware/logger",        "hull/web/middleware/logger"},
                {"hull/middleware/outbox",        "hull/web/middleware/outbox"},
                {"hull/middleware/ratelimit",     "hull/web/middleware/ratelimit"},
                {"hull/middleware/rbac",          "hull/web/middleware/rbac"},
                {"hull/middleware/session",       "hull/web/middleware/session"},
                {"hull/middleware/transaction",   "hull/web/middleware/transaction"},
                {"hull/sse",                      "hull/web/sse"},
                {"hull/ws-client",                "hull/web/ws-client"},
                {"hull/ws-server",                "hull/web/ws-server"},
            };

            /* Normalize the input the same way find_short does: prepend
             * "hull/" if absent, so both "cookie" and "hull/cookie"
             * trigger the rename hint. */
            char canon[HL_MODULE_NAME_MAX];
            const char *check = m->name;
            if (strncmp(check, "hull/", 5) != 0) {
                int n = snprintf(canon, sizeof(canon), "hull/%s", check);
                if (n > 0 && (size_t)n < sizeof(canon)) check = canon;
            }
            for (size_t r = 0; r < sizeof(V0_2_0_RENAMES) / sizeof(V0_2_0_RENAMES[0]); r++) {
                if (strcmp(check, V0_2_0_RENAMES[r].old) == 0) {
                    ERR3("module '%s@%u' was renamed to '%s@1' in v0.2.0; "
                         "update app.manifest.modules (and any "
                         "require/import sites)",
                         m->name, (unsigned)m->api_major,
                         V0_2_0_RENAMES[r].new);
                    return -1;
                }
            }

            const HlModuleSpec *guess = hl_module_registry_suggest(m->name);
            if (guess) {
                ERR3("unknown module '%s' in app.manifest.modules — "
                     "did you mean \"%s@%u\"? "
                     "(see `hull modules available` for the full list)",
                     m->name, guess->name, (unsigned)guess->api_major);
                return -1;
            }
            ERR1("unknown module '%s' in app.manifest.modules — "
                 "see `hull modules available` for the v1 set",
                 m->name);
            return -1;
        }

        if (spec->api_major != m->api_major) {
            ERR3("module '%s' requested api_major=%u, registry has %u",
                 spec->name, (unsigned)m->api_major,
                 (unsigned)spec->api_major);
            return -1;
        }

        int idx = hl_module_registry_index(spec);
        if (get_bit(out, idx)) {
            ERR1("module '%s' declared more than once in manifest.modules",
                 spec->name);
            return -1;
        }

        /* Build-time (compile flag) caps are hard-blocked at the
         * resolver — there's no per-call recovery once they're
         * compiled out. Manifest-side caps (fs.read/write, hosts, env)
         * are intentionally NOT checked here: the per-call cap layer
         * fails closed against an empty allowlist, and some module
         * surface (e.g. `fs.realpath`, future `fs.stat`) doesn't need
         * any path declared at all. Pairing the module with a section
         * is documentation, not a load-time requirement. */
        uint32_t need = spec->required_caps;
        uint32_t need_build = need & build_cap_mask;

        /* Walk the cap-bit space by index instead of shifting until
         * overflow. Cap bits are at indices 0..8 today (see
         * include/hull/module_registry.h); 32 iterations is a no-op
         * upper bound that lets the loop terminate without relying on
         * unsigned-wrap-to-zero. Counter is `bi` (not `i`) so it
         * doesn't shadow the outer manifest-modules loop counter. */
        for (int bi = 0; bi < 32; bi++) {
            uint32_t bit = 1u << bi;
            if (!(need_build & bit)) continue;
            if (!(prov_build & bit)) {
                ERR2("module '%s' requires %s, but it is disabled in "
                     "this hull build",
                     spec->name, cap_label(bit));
                return -1;
            }
        }

        /* Modules that require a manifest boolean field in addition to
         * a build-time subsystem. The build-time check above has
         * already passed; here we enforce the matching declaration.
         * Same shape as the GPU runtime-wiring check, but raised at
         * resolve time so apps see the error before reaching cap
         * code. */
        if ((spec->required_caps & HL_MOD_CAP_TUI) && !manifest->tui) {
            ERR1("module '%s' requires the 'tui' capability in the "
                 "manifest (add `tui = true`)", spec->name);
            return -1;
        }

        set_bit(out, idx);
    }

    /* Pass 2: auto-admit transitive deps of every admitted module.
     *
     * Policy (changed 2026-05-22): declaring a module implicitly admits
     * its registry-declared deps, recursively. The previous policy
     * required users to re-declare every transitive dep ("declared
     * jwt? add crypto too; declared email? add log + json + http-client
     * + smtp"), which produced unhelpful error chains and forced apps
     * declaring any stdlib middleware to list hull/log + hull/json
     * even though they're internal-only utility imports.
     *
     * Build-time caps of auto-admitted deps are re-checked so a
     * declared module can't transitively pull in a subsystem the
     * binary was compiled without (e.g. declaring `hull/email`
     * shouldn't smuggle hull/smtp into an HL_ENABLE_HTTP_CLIENT=0
     * build). Manifest-side caps (fs/hosts/env) stay gated at call
     * time as before — the module gate only controls import visibility.
     *
     * Walked as a fixed-point loop so deps-of-deps get admitted.
     * Bounded by total registry size; terminates when no new bits
     * flip in a pass. */
    size_t reg_count = 0;
    const HlModuleSpec *reg = hl_module_registry_all(&reg_count);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (size_t idx = 0; idx < reg_count; idx++) {
            if (!get_bit(out, (int)idx)) continue;
            const HlModuleSpec *spec = &reg[idx];

            for (int j = 0; j < HL_MODULE_MAX_DEPS && spec->deps[j]; j++) {
                const HlModuleSpec *dep_spec =
                    hl_module_registry_find(spec->deps[j]);
                if (!dep_spec) {
                    /* Registry inconsistency — programming bug. */
                    ERR2("internal: module '%s' declares unknown dep '%s'",
                         spec->name, spec->deps[j]);
                    return -1;
                }
                int dep_idx = hl_module_registry_index(dep_spec);
                if (get_bit(out, dep_idx)) continue;  /* already in */

                /* Re-check build-time gates for the dep — auto-admit
                 * must not bypass HL_ENABLE_* requirements. Same
                 * counter-based loop as Pass 1 (see comment there). */
                uint32_t need_build = dep_spec->required_caps & build_cap_mask;
                for (int bi = 0; bi < 32; bi++) {
                    uint32_t bit = 1u << bi;
                    if (!(need_build & bit)) continue;
                    if (!(prov_build & bit)) {
                        ERR3("module '%s' transitively requires '%s', "
                             "which needs %s but it is disabled in this "
                             "hull build",
                             spec->name, dep_spec->name, cap_label(bit));
                        return -1;
                    }
                }
                if ((dep_spec->required_caps & HL_MOD_CAP_TUI)
                        && !manifest->tui) {
                    ERR2("module '%s' transitively requires '%s', which "
                         "needs the 'tui' capability — add `tui = true` "
                         "to the manifest",
                         spec->name, dep_spec->name);
                    return -1;
                }

                set_bit(out, dep_idx);
                changed = 1;
            }
        }
    }

    return 0;
}

/* ── Pre-manifest import tracker ───────────────────────────────────── */

void hl_import_tracker_record(HlRuntime *rt, const char *canonical_name)
{
    if (!rt || !canonical_name) return;
    if (rt->import_tracker_count >= HL_MANIFEST_MAX_MODULES) return;

    /* Dedup against the existing list — apps commonly require/import
     * the same module from multiple files, and we want each name to
     * appear at most once in the validation error. Linear scan is
     * fine; HL_MANIFEST_MAX_MODULES is small. */
    for (int i = 0; i < rt->import_tracker_count; i++) {
        if (rt->import_tracker_names[i] == canonical_name) return;
        if (strcmp(rt->import_tracker_names[i], canonical_name) == 0) return;
    }
    rt->import_tracker_names[rt->import_tracker_count++] = canonical_name;
}

int hl_import_tracker_validate(const HlRuntime *rt,
                                const HlResolvedModuleSet *set,
                                char *errbuf, size_t errlen)
{
    if (!rt || !set) return 0;

    int missing_count = 0;
    const char *first_missing = NULL;
    for (int i = 0; i < rt->import_tracker_count; i++) {
        const char *name = rt->import_tracker_names[i];
        if (!hl_module_set_contains_name(set, name)) {
            if (!first_missing) first_missing = name;
            missing_count++;
        }
    }
    if (missing_count == 0) return 0;

    if (errbuf && errlen) {
        if (missing_count == 1) {
            snprintf(errbuf, errlen,
                     "module '%s' was imported at top-of-file but is not "
                     "declared in app.manifest. Add \"%s@1\" to the "
                     "modules array (or declare a module that auto-pulls "
                     "it via deps).",
                     first_missing, first_missing);
        } else {
            snprintf(errbuf, errlen,
                     "%d top-level imports are not declared in app.manifest "
                     "(first: '%s'). Add each to the modules array, or "
                     "declare a module that auto-pulls them via deps. "
                     "Run `hull modules available` for the full list.",
                     missing_count, first_missing);
        }
    }
    return -1;
}
