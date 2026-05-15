/*
 * manifest.h — Application manifest declaration and extraction
 *
 * Apps declare capabilities via app.manifest({...}).
 * This module extracts the manifest from the runtime state into
 * a C struct for cap config wiring and signing.
 *
 * All strings in HlManifest are Hull-owned copies (allocated via
 * the stored allocator). Call hl_manifest_free() to release them.
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

#endif /* HL_MANIFEST_H */
