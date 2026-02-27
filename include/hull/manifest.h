/*
 * manifest.h — Application manifest declaration and extraction
 *
 * Apps declare capabilities via app.manifest({...}) in Lua.
 * This module extracts the manifest from the Lua registry into
 * a C struct for cap config wiring and signing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_MANIFEST_H
#define HL_MANIFEST_H

#include <stddef.h>

/* Forward declarations */
typedef struct lua_State lua_State;

/* ── Limits ────────────────────────────────────────────────────────── */

#define HL_MANIFEST_MAX_PATHS  32
#define HL_MANIFEST_MAX_ENVS   32
#define HL_MANIFEST_MAX_HOSTS  32

/* ── Manifest struct ───────────────────────────────────────────────── */

typedef struct HlManifest {
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

    /* Whether app.manifest() was called */
    int         present;
} HlManifest;

/* ── API ───────────────────────────────────────────────────────────── */

/*
 * Extract manifest from Lua registry key "__hull_manifest".
 * Populates `out` with string pointers into the Lua state
 * (valid as long as the Lua state is alive).
 *
 * Returns 0 on success, -1 if no manifest was declared.
 */
int hl_manifest_extract(lua_State *L, HlManifest *out);

#ifdef HL_ENABLE_JS

/* Forward declaration */
typedef struct JSContext JSContext;

/*
 * Extract manifest from globalThis.__hull_manifest in QuickJS.
 * Populates `out` with string pointers from JS_ToCString
 * (must be freed with hl_manifest_free_js_strings before ctx is destroyed).
 *
 * Returns 0 on success, -1 if no manifest was declared.
 */
int hl_manifest_extract_js(JSContext *ctx, HlManifest *out);

/*
 * Free JS_ToCString pointers stored in the manifest by
 * hl_manifest_extract_js(). Call after sandbox is applied.
 */
void hl_manifest_free_js_strings(JSContext *ctx, HlManifest *m);

#endif /* HL_ENABLE_JS */

#endif /* HL_MANIFEST_H */
