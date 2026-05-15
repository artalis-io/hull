/*
 * manifest.c — Shared manifest helpers + free
 *
 * The per-runtime extractors live in sibling files:
 *   manifest_lua.c  — hl_manifest_extract     (Lua registry → HlManifest)
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

    memset(m, 0, sizeof(*m));
}
