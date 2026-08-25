/*
 * sbom.h. Software Bill of Materials surface for `hull sbom` / `hull agent sbom`.
 *
 * Read-only data exporter. Pure compile-time data table + a few output
 * formatters. No runtime dependencies on db/fs/http/etc. capabilities.
 * Self-contained. Orthogonal to the rest of the runtime.
 *
 * Data flow (per-build auto-refresh):
 *   Makefile reads vendor submodule SHAs via `git -C vendor/<name> rev-parse HEAD`
 *   and passes each as -DHULL_VENDOR_<NAME>_COMMIT="...". The static entry
 *   table in sbom.c references those defines. Result: every binary
 *   self-describes its actual vendored contents.
 *
 * Build-flag gating: entries are conditional on HL_ENABLE_* flags so a
 *   `make HL_ENABLE_DB=0` build correctly omits SQLite from the SBOM.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SBOM_H
#define HL_SBOM_H

#include <stdio.h>
#include <stddef.h>

/* Output formats. */
typedef enum {
    HL_SBOM_HUMAN = 0,   /* default: ANSI-ish table mirroring LICENSING.md */
    HL_SBOM_JSON,        /* flat array; also what `hull agent sbom` returns */
    HL_SBOM_CYCLONEDX,   /* CycloneDX 1.5 JSON; NTIA-aligned */
    HL_SBOM_SPDX,        /* SPDX 2.3 JSON */
} HlSbomFormat;

/* Modularization tier: which axis of Hull's composable build carries this
 * component. The distributed hull embeds the SLIM base plus every
 * auto-composed feature archive, but a `hull build` app links only the
 * subset it composes - so a flat "everything is present" list would
 * misrepresent a built app. The tier makes the composition explicit. */
typedef enum {
    HL_SBOM_TIER_BASE = 0,   /* always linked into every hull build (the SLIM base) */
    HL_SBOM_TIER_FEATURE,    /* auto-composed feature: embedded in hull, composed
                              * per-app only when the app needs it (the runtime
                              * inferred from the entry extension, plus
                              * needs_http / needs_sqlite / needs_wasm /
                              * needs_image / needs_tls). Static, no dlopen. */
    HL_SBOM_TIER_WITH,       /* opt-in `hull build --with=<name>`: NOT in the base
                              * binary; installed + composed on demand */
} HlSbomTier;

/* Per-component entry. All string pointers are static; never freed. */
typedef struct {
    const char *name;          /* e.g. "keel", "hull", "Lua 5.4" */
    const char *version;       /* semver, "n/a", or "" if unknown */
    const char *commit;        /* git SHA at build time, or "" if not vendored */
    const char *license_spdx;  /* SPDX identifier, e.g. "MIT", "AGPL-3.0-or-later" */
    const char *url;           /* upstream source URL */
    const char *role;          /* one-line role in Hull, e.g. "HTTP server" */
    /* Common Platform Enumerator (CPE) 2.3 string for CVE-database
     * cross-reference. NULL for components without a registered CPE
     * (Hull itself, project-internal vendors, ad-hoc snapshots). When
     * emitted, written into CycloneDX output as the component's `cpe`
     * field; downstream scanners (Dependency-Track, Trivy, etc.)
     * consume CPE for automated vulnerability matching. */
    const char *cpe;
    /* SHA-256 of an embedded blob (CA bundle, etc.) computed
     * at runtime from the actual bytes. Returns a static hex string
     * (cached on first call) or NULL if no embedded blob. */
    const char *(*embedded_blob_sha256)(void);
    /* 1 if this component is part of the libhull embedding surface (linked
     * by a libhull.a + libkeel.a native host); 0 for runtime-only
     * components (the Lua / QuickJS script engines) that libhull excludes.
     * Consumed by the `hull sbom --subject=libhull` scope. */
    int in_libhull;
    /* 1 if this component is only present when HTTP is compiled in (Keel +
     * mbedTLS). The `pure-compute` preset (an app declaring no HTTP/TLS
     * module) drops these; consumed by the `hull sbom --flavor=<flavor>`
     * scope. */
    int needs_http;
    /* Modularization tier (base / auto-composed feature / --with). Default
     * HL_SBOM_TIER_BASE for entries that don't set it. See HlSbomTier. */
    HlSbomTier tier;
    /* When tier != BASE, the composable-archive stem that carries this
     * component ("keel", "tls", "sqlite", "wasm", "image", "lua", "js",
     * "gpu", "duckdb", ...). NULL for base-tier components. */
    const char *feature;
} HlSbomEntry;

/* Returns a pointer to the static entry table. Sets *count to its length.
 * Build-flag gating (HL_ENABLE_*) is applied at compile time.
 * The returned pointer is valid for the lifetime of the process. */
const HlSbomEntry *hl_sbom_entries(size_t *count);

/* Scope the next hl_sbom_format() to the libhull embedding surface: the
 * subject component is named "libhull" and components with in_libhull == 0
 * (the script runtimes) are omitted. Pass 0 to restore the default
 * whole-hull scope. Process-global, like hl_sbom_set_binary_path. */
void hl_sbom_set_scope_libhull(int on);

/* Scope the next hl_sbom_format() to a `hull build --flavor` target: report the
 * dependency set that flavor validates for. A flavor is a
 * build.lua preset on the composable base (no per-flavor platform lib):
 * "pure-compute" is the preset for an app that declares no HTTP/TLS, so it drops
 * the needs_http components (Keel + mbedTLS); "full" is the whole vendored set.
 * (A real compute app drops more still - SQLite / WASM / image compose per app;
 * this coarse flavor scope reflects only the HTTP/TLS axis.) @p flavor is one of
 * those two names, or NULL/"" to clear the scope. Returns 0 on success, -1 on an
 * unknown flavor. Process-global. */
int hl_sbom_set_scope_flavor(const char *flavor);

/* Format the SBOM and write to `fp`. Returns 0 on success, -1 on error.
 * Errors are printed to stderr. */
int hl_sbom_format(HlSbomFormat format, FILE *fp);

/* Parse a format string. Returns the enum value or -1 if unknown.
 * Accepted: "human", "json", "cyclonedx", "spdx". */
int hl_sbom_parse_format(const char *str);

/* Register the path of the running hull binary (typically argv[0]).
 * The SHA-256 of that file is computed lazily on the first format()
 * call and emitted as `binary_sha256` in json / cyclonedx / spdx
 * output, letting consumers cross-reference the live binary against
 * the signed `hull.sha256` release manifest. Pass NULL to clear.
 * The pointer is stored; the caller owns the lifetime. */
void hl_sbom_set_binary_path(const char *path);

#endif /* HL_SBOM_H */
