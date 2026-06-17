/**
 * @file manifest.h
 * @brief App-declared manifest: capability extraction + signing input.
 *
 * Apps declare their capability surface via `app.manifest({...})`.
 * This module extracts the declaration from the runtime state into a
 * C struct (#HlManifest) which:
 *   1. Configures the cap layer's allowlists at startup.
 *   2. Is hashed and signed for `package.sig` integrity checks.
 *
 * @par Memory:
 *   All strings in #HlManifest are Hull-owned copies (allocated via
 *   the stored allocator). Call @ref hl_manifest_free to release them.
 *
 * @par Limits (compile-time):
 *   - `HL_MANIFEST_MAX_PATHS` = 32 (`fs.read` / `fs.write` patterns)
 *   - `HL_MANIFEST_MAX_ENVS` = 32 (env var names)
 *   - `HL_MANIFEST_MAX_HOSTS` = 32 (HTTP/WS hosts)
 *   - `HL_MANIFEST_MAX_CORS_ORIGINS` = 16
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_MANIFEST_H
#define HL_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "hull/limits/gpu.h"  /* HL_GPU_MAX_DEVICES */

/* Forward declarations */
typedef struct lua_State lua_State;
typedef struct HlAllocator HlAllocator;

/* ── Limits ────────────────────────────────────────────────────────── */

#define HL_MANIFEST_MAX_PATHS  32
#define HL_MANIFEST_MAX_ENVS   32
#define HL_MANIFEST_MAX_HOSTS  32
#define HL_MANIFEST_MAX_CORS_ORIGINS 16
#define HL_MANIFEST_MAX_MODULES 32

/* One declared module: `modules = { crypto = "1" }` → name="crypto", api_major=1.
 * Names are stored as the app wrote them (short alias or full canonical) —
 * the resolver normalizes via hl_module_registry_find_short(). */
typedef struct HlManifestModule {
    const char *name;       /* allocator-owned copy */
    uint8_t     api_major;  /* parsed from version string (e.g. "1" → 1) */
} HlManifestModule;

/* ── Manifest struct ───────────────────────────────────────────────── */

typedef struct HlManifest {
    /* Allocator used for all owned strings (NULL = use raw malloc/free) */
    HlAllocator *alloc;

    /* Filesystem capabilities */
    const char *fs_read[HL_MANIFEST_MAX_PATHS];
    int         fs_read_count;
    const char *fs_write[HL_MANIFEST_MAX_PATHS];
    int         fs_write_count;

    /* Environment variable allowlist */
    const char *env[HL_MANIFEST_MAX_ENVS];
    int         env_count;

    /* Outbound HTTP host allowlist */
    const char *hosts[HL_MANIFEST_MAX_HOSTS];
    int         hosts_count;

    /* Content-Security-Policy for HTML responses */
    const char *csp;        /* Custom CSP string (NULL if not set or disabled) */
    int         csp_set;    /* 1 if app explicitly set csp key in manifest */

    /* CORS configuration */
    const char *cors_origins[HL_MANIFEST_MAX_CORS_ORIGINS];
    int         cors_origin_count;
    const char *cors_methods;     /* NULL = Keel default */
    const char *cors_headers;     /* NULL = Keel default */
    int         cors_credentials; /* 0 or 1 */
    int         cors_max_age;     /* 0 = Keel default (86400) */
    int         cors_set;         /* 1 if cors key present in manifest */

    /* WASM compute limits (from manifest "wasm" key) */
    uint32_t    wasm_heap;        /* 0 = not set */
    uint32_t    wasm_stack;
    int64_t     wasm_gas;
    uint32_t    wasm_max_input;
    uint32_t    wasm_max_output;

    /* Capability flags */
    int         gpu;              /* 1 if app declares gpu: true or gpu: {...} */
    int         gpu_devices[HL_GPU_MAX_DEVICES]; /* allowed device indices */
    int         gpu_device_count; /* 0 = all devices allowed (backward compat) */
    int         compute;          /* 1 if app declares compute: true */
    int         tui;              /* 1 if app declares tui: true (terminal UI) */

    /* Declared first-party module set.
     *
     * `modules = { crypto = "1", fs = "1" }` in the manifest. Names
     * stored as written; the resolver in `module_resolver.h` normalizes
     * short aliases to canonical `hull/<name>` form. An app that does
     * not include a `modules` key at all (or has the key absent) has
     * `modules_declared = 0`; one that sets `modules = {}` has
     * `modules_declared = 1` with `modules_count = 0`.
     *
     * Both states are valid — the resolver still seeds the intrinsic
     * core (hull/app, hull/log, hull/json) automatically.
     */
    HlManifestModule modules[HL_MANIFEST_MAX_MODULES];
    int              modules_count;
    int              modules_declared;

    /* W^X / no runtime dynamic code — opt-in escape hatches.
     * Both default to 0 (deny). Setting either to 1 in a manifest is
     * rejected by `hl_sandbox_apply` unless the user opts out of the
     * kernel sandbox entirely via `--no-sandbox` (development only).
     * Surfaced by `hull inspect` with a risk marker when true. */
    int         allow_dynamic_code;       /* opt-in: JIT / runtime codegen */
    int         allow_dynamic_libraries;  /* opt-in: dlopen() native libs */

    /* Whether app.manifest() was called */
    int         present;
} HlManifest;

/* ── API ───────────────────────────────────────────────────────────── */

/*
 * Extract manifest from Lua registry key "__hull_manifest".
 * All strings are copied into Hull-owned allocations via `alloc`.
 * Returns 0 on success, -1 if no manifest was declared.
 */
int hl_manifest_extract_lua(lua_State *L, HlManifest *out, HlAllocator *alloc);

/*
 * Free all owned strings in the manifest.
 * Safe to call on a zeroed or partially-populated manifest.
 */
void hl_manifest_free(HlManifest *m);

#ifdef HL_ENABLE_JS

/* Forward declaration */
typedef struct JSContext JSContext;

/*
 * Extract manifest from globalThis.__hull_manifest in QuickJS.
 * All strings are copied into Hull-owned allocations via `alloc`.
 * Returns 0 on success, -1 if no manifest was declared.
 */
int hl_manifest_extract_js(JSContext *ctx, HlManifest *out, HlAllocator *alloc);

#endif /* HL_ENABLE_JS */

/* Forward decl for the sealed-arena helper below. */
typedef struct HlSealArena HlSealArena;

/*
 * Copy a fully-extracted manifest into a sealed arena.
 *
 * The manifest is the canonical example of "boot-built then read-only
 * for the rest of the process": fs/hosts/env allowlists, declared
 * modules, CSP, CORS policy — all derived from `app.manifest({...})`
 * at startup, then read on every request by the capability layer.
 * After this call returns 0:
 *
 *   - All of `dst`'s string pointers point into the arena.
 *   - The integer / flag fields are copied by value.
 *   - The HlManifest struct itself stays where the CALLER put it
 *     (typically embedded in HlRuntime or HlServerState). If the
 *     caller wants the struct ALSO in the arena, they can allocate
 *     it via hl_seal_arena_alloc first and pass the in-arena address
 *     as `dst`. Otherwise the parent struct's mutability is the
 *     caller's responsibility.
 *
 * The caller is responsible for:
 *   - Calling `hl_seal_arena_seal(arena)` ONCE after all desired
 *     seal-into-arena work is complete (sealing this manifest, any
 *     sandbox-policy derived from it, etc.). The seal flips the
 *     arena RO; subsequent writes to anything allocated here fault.
 *   - Freeing the SOURCE manifest's strings (`hl_manifest_free(src)`)
 *     after this returns 0 — dst no longer aliases them.
 *
 * `dst->alloc` is set to NULL after sealing, since the strings are
 * no longer allocator-owned. Calling `hl_manifest_free(dst)` after
 * this is therefore a no-op for the strings (which is what we want
 * — the arena owns them, lifetime is process-lifetime).
 *
 * Returns 0 on success, -1 on failure (arena out of space, NULL
 * inputs, src not present). On -1 `dst` is zero-initialized.
 */
int hl_manifest_seal(HlManifest *dst, const HlManifest *src, HlSealArena *arena);

#endif /* HL_MANIFEST_H */
