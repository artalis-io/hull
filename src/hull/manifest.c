/*
 * manifest.c — Shared manifest helpers + free
 *
 * The per-runtime extractors live in sibling files:
 *   manifest_lua.c  — hl_manifest_extract_lua     (Lua registry → HlManifest)
 *   manifest_js.c   — hl_manifest_extract_js  (QuickJS globalThis → HlManifest)
 *
 * This file holds only what's runtime-free: the shared string helpers, the
 * CSP CR/LF guard, and hl_manifest_free (which knows nothing about the
 * source runtime — every owned pointer was allocated through the same
 * allocator regardless of extractor).
 *
 * Split as part of architectural roadmap item G.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest.h"
#include "hull/alloc.h"
#include "hull/seal_arena.h"
#include "manifest_internal.h"
#include "log.h"
#include <string.h>

/* ── Helpers (shared with manifest_lua.c / manifest_js.c) ───────────── */

int hl_manifest_csp_is_valid(const char *s)
{
    if (!s) return 1;
    for (const char *p = s; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            log_warn("[manifest] CSP string contains CR/LF — rejected");
            return 0;
        }
    }
    return 1;
}

const char *hl_manifest_strdup(HlAllocator *alloc, const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *copy = hl_alloc_malloc(alloc, len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

void hl_manifest_str_free(HlAllocator *alloc, const char **sp)
{
    if (*sp) {
        hl_alloc_free(alloc, (void *)*sp, strlen(*sp) + 1);
        *sp = NULL;
    }
}

/* ── hl_manifest_free ──────────────────────────────────────────────── */

void hl_manifest_free(HlManifest *m)
{
    if (!m) return;
    HlAllocator *a = m->alloc;

    for (int i = 0; i < m->fs_read_count; i++)
        hl_manifest_str_free(a, &m->fs_read[i]);
    for (int i = 0; i < m->fs_write_count; i++)
        hl_manifest_str_free(a, &m->fs_write[i]);
    for (int i = 0; i < m->env_count; i++)
        hl_manifest_str_free(a, &m->env[i]);
    for (int i = 0; i < m->hosts_count; i++)
        hl_manifest_str_free(a, &m->hosts[i]);
    hl_manifest_str_free(a, &m->csp);
    for (int i = 0; i < m->cors_origin_count; i++)
        hl_manifest_str_free(a, &m->cors_origins[i]);
    hl_manifest_str_free(a, &m->cors_methods);
    hl_manifest_str_free(a, &m->cors_headers);
    for (int i = 0; i < m->modules_count; i++)
        hl_manifest_str_free(a, &m->modules[i].name);

    memset(m, 0, sizeof(*m));
}

/* ── hl_manifest_seal ──────────────────────────────────────────────── */

/* Shared helper: copy one string into the arena, store the in-arena
 * pointer into *out, returning -1 on arena OOM. Treats NULL src as
 * success-with-NULL (some manifest fields are legitimately optional). */
static int seal_str(HlSealArena *arena, const char **out, const char *src)
{
    if (!src) { *out = NULL; return 0; }
    char *dup = hl_seal_arena_strdup(arena, src);
    if (!dup) return -1;
    *out = dup;
    return 0;
}

int hl_manifest_seal(HlManifest *dst, const HlManifest *src, HlSealArena *arena)
{
    if (!dst || !src || !arena) return -1;
    if (!src->present) {
        /* Nothing to seal — apps without a manifest declaration get
         * a deny-default policy elsewhere. Zero dst to be safe. */
        memset(dst, 0, sizeof(*dst));
        return -1;
    }
    if (hl_seal_arena_is_sealed(arena)) {
        /* Arena already sealed; can't allocate. Programming bug. */
        memset(dst, 0, sizeof(*dst));
        return -1;
    }

    /* Start with a value-copy of integer + bounded-array fields. We'll
     * overwrite the pointer fields below. The wasm_*, gpu_*, compute,
     * tui, allow_dynamic_*, present, *_count, *_set flags all travel
     * as scalars and are immutable-by-copy here. */
    *dst = *src;
    /* The sealed manifest is NOT allocator-owned for its strings; clear
     * the allocator pointer so hl_manifest_free on dst is a safe no-op. */
    dst->alloc = NULL;

    /* Strings: walk and dup each one into the arena. If any fails we
     * bail with -1; the arena's already-allocated portion stays put
     * (sealed-arena allocations can't be individually freed; the
     * caller is expected to destroy the arena and start over on
     * failure). */
    for (int i = 0; i < src->fs_read_count; i++)
        if (seal_str(arena, &dst->fs_read[i], src->fs_read[i]) != 0) goto fail;
    for (int i = 0; i < src->fs_write_count; i++)
        if (seal_str(arena, &dst->fs_write[i], src->fs_write[i]) != 0) goto fail;
    for (int i = 0; i < src->env_count; i++)
        if (seal_str(arena, &dst->env[i], src->env[i]) != 0) goto fail;
    for (int i = 0; i < src->hosts_count; i++)
        if (seal_str(arena, &dst->hosts[i], src->hosts[i]) != 0) goto fail;
    if (seal_str(arena, &dst->csp, src->csp) != 0) goto fail;
    for (int i = 0; i < src->cors_origin_count; i++)
        if (seal_str(arena, &dst->cors_origins[i], src->cors_origins[i]) != 0)
            goto fail;
    if (seal_str(arena, &dst->cors_methods, src->cors_methods) != 0) goto fail;
    if (seal_str(arena, &dst->cors_headers, src->cors_headers) != 0) goto fail;
    for (int i = 0; i < src->modules_count; i++)
        if (seal_str(arena, &dst->modules[i].name, src->modules[i].name) != 0)
            goto fail;

    return 0;

fail:
    /* Don't zero dst — partial pointers into the arena are valid reads
     * (the arena is still mutable + alive); zeroing would also blow
     * away the integer fields. Caller treats -1 as "destroy the arena
     * and don't use dst". */
    return -1;
}
