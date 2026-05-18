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

/* Capabilities the *manifest* claims (versus what the build provides). */
static uint32_t manifest_provided_caps(const HlManifest *m)
{
    uint32_t caps = 0;
    if (!m) return 0;
    if (m->fs_read_count > 0 || m->fs_write_count > 0) caps |= HL_MOD_CAP_FS;
    if (m->hosts_count > 0)                            caps |= HL_MOD_CAP_HOSTS;
    if (m->env_count > 0)                              caps |= HL_MOD_CAP_ENV;
    /* DB / WASM / GPU are compile-time gated AND manifest-gated;
     * the resolver only checks the compile-time bit here. The manifest
     * fields (compute, gpu) are surfaced by the runtime wiring, not by
     * the module declaration. */
    return caps;
}

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
    return caps;
}

/* Human-readable name for a capability bit (for error messages). */
static const char *cap_label(uint32_t cap)
{
    switch (cap) {
    case HL_MOD_CAP_FS:    return "fs.read or fs.write";
    case HL_MOD_CAP_HOSTS: return "hosts";
    case HL_MOD_CAP_ENV:   return "env";
    case HL_MOD_CAP_DB:    return "HL_ENABLE_DB (build-time)";
    case HL_MOD_CAP_WASM:  return "HL_ENABLE_WASM (build-time)";
    case HL_MOD_CAP_GPU:   return "HL_ENABLE_GPU (build-time)";
    default:               return "unknown";
    }
}

/* ── The resolver ──────────────────────────────────────────────────── */

int hl_module_resolver_resolve(const HlManifest *manifest,
                                HlResolvedModuleSet *out,
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

    const uint32_t prov_manifest = manifest_provided_caps(manifest);
    const uint32_t prov_build    = build_provided_caps();

    /* Pass 1: look up each declared module + admit it. Detect unknown
     * names, version mismatches, duplicates, missing capabilities. */
    for (int i = 0; i < manifest->modules_count; i++) {
        const HlManifestModule *m = &manifest->modules[i];
        const HlModuleSpec *spec = hl_module_registry_find_short(m->name);

        if (!spec) {
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

        /* Required capabilities — check build first (compile-time
         * subsystems), then manifest sections. */
        uint32_t need = spec->required_caps;
        uint32_t need_build    = need & (HL_MOD_CAP_DB | HL_MOD_CAP_WASM | HL_MOD_CAP_GPU);
        uint32_t need_manifest = need & ~(HL_MOD_CAP_DB | HL_MOD_CAP_WASM | HL_MOD_CAP_GPU);

        for (uint32_t bit = 1; bit; bit <<= 1) {
            if (!(need_build & bit)) continue;
            if (!(prov_build & bit)) {
                ERR2("module '%s' requires %s, but it is disabled in "
                     "this hull build",
                     spec->name, cap_label(bit));
                return -1;
            }
        }
        for (uint32_t bit = 1; bit; bit <<= 1) {
            if (!(need_manifest & bit)) continue;
            if (!(prov_manifest & bit)) {
                ERR2("module '%s' requires a non-empty `%s` section in "
                     "the manifest",
                     spec->name, cap_label(bit));
                return -1;
            }
        }

        set_bit(out, idx);
    }

    /* Pass 2: verify every dep of every admitted module is also
     * declared. Walked separately so error messages are deterministic
     * (we don't surface a dep error before the module that names it is
     * itself validated). */
    for (int i = 0; i < manifest->modules_count; i++) {
        const HlManifestModule *m = &manifest->modules[i];
        const HlModuleSpec *spec = hl_module_registry_find_short(m->name);
        if (!spec) continue;  /* unreachable: caught in pass 1 */

        for (int j = 0; j < HL_MODULE_MAX_DEPS && spec->deps[j]; j++) {
            const HlModuleSpec *dep_spec = hl_module_registry_find(spec->deps[j]);
            if (!dep_spec) {
                /* Registry inconsistency — would be a programming bug. */
                ERR2("internal: module '%s' declares unknown dep '%s'",
                     spec->name, spec->deps[j]);
                return -1;
            }
            if (!get_bit(out, hl_module_registry_index(dep_spec))) {
                ERR3("module '%s' requires '%s' but it is not declared — "
                     "add `%s` to modules in app.manifest",
                     spec->name, dep_spec->name, dep_spec->name);
                return -1;
            }
        }
    }

    return 0;
}
