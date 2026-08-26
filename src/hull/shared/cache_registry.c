/**
 * @file cache_registry.c
 * @brief Static registry of every blob_store-backed cache Hull knows
 *        about, plus path resolution that honours `HULL_CACHE_DIR`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/cache_registry.h"
#include "hull/shared/cache_dir.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Registry ─────────────────────────────────────────────────────
 *
 * Order matters for `cache list` output - runtime caches first
 * (most-relevant for prune), system stores at the end. */
/* env_kind maps to HULL_NO_<UPPER>_CACHE - see hl_hull_cache_disabled
 * in cache_dir.c. Display names stay kebab-case for path / UI;
 * env-var kinds are snake_case so the SCREAMING_SNAKE result reads
 * naturally. */
static const HlCacheKind REGISTRY[] = {
    { "lua-bytecode", "Lua stdlib + app bytecode (luaL_loadbuffer skip)",     1, "lua_bytecode", 0 },
    { "js-bytecode",  "QuickJS module bytecode (JS_Eval parse skip)",          1, "js_bytecode",  0 },
    { "compute-aot",  "WASM AOT artifacts (wamrc output, sha-keyed)",          1, "aot",          0 },
    { "templates",    "Lua template render functions (parse + pcall skip)",    1, "template",     0 },
    { "js-templates", "JS template render functions (parse + IIFE skip)",      1, "js_template",  0 },
    { "tools",        "Signed side-loaded tool binaries (wamrc, ...)",         0, NULL,           1 },
    { NULL, NULL, 0, NULL, 0 }
};

const HlCacheKind *hl_cache_registry(void)
{
    return REGISTRY;
}

const HlCacheKind *hl_cache_find(const char *name)
{
    if (!name) return NULL;
    for (const HlCacheKind *k = REGISTRY; k->name; k++) {
        if (strcmp(k->name, name) == 0) return k;
    }
    return NULL;
}

int hl_cache_resolve_path(const HlCacheKind *kind, char *out, size_t out_sz)
{
    if (!kind || !out || out_sz < 2) return -1;

    if (kind->is_runtime) {
        /* Runtime caches go through hl_hull_cache_subdir, which
         * honours HULL_CACHE_DIR. Returned path has a trailing
         * slash; strip it here so resolve callers can format
         * paths uniformly. */
        char buf[PATH_MAX];
        if (hl_hull_cache_subdir(kind->name, buf, sizeof(buf)) != 0)
            return -1;
        size_t n = strlen(buf);
        while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
        if (n + 1 > out_sz) { errno = ENAMETOOLONG; return -1; }
        memcpy(out, buf, n + 1);
        return 0;
    }

    /* System stores: always under $HOME/.hull/blobs/<name>/.
     * HULL_CACHE_DIR doesn't apply - these are not per-app caches,
     * they're durable signed downloads with a stable system home. */
    const char *home = getenv("HOME");
    if (!home || !*home) { errno = ENOENT; return -1; }
    int n = snprintf(out, out_sz, "%s/.hull/blobs/%s", home, kind->name);
    if (n < 0 || (size_t)n >= out_sz) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
