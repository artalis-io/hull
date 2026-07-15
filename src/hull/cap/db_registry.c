/*
 * cap/db_registry.c: Named database connection registry
 *
 * See db_registry.h. Lazily opens + caches HlDbHandle connections by name,
 * resolving DSNs from the sealed manifest's databases map (with { dsn_env }
 * read from the environment at open time).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db_registry.h"
#include "hull/manifest.h"
#include "hull/utils/env_ref.h"   /* hl_env_ref */

#include <stdlib.h>
#include <string.h>

/* One extra slot beyond the manifest cap for the seeded "default". */
#define HL_DB_REGISTRY_MAX (HL_MANIFEST_MAX_DATABASES + 1)

typedef struct {
    char       *name;    /* owned */
    char       *dsn;     /* owned; the DSN this connection opened from, NULL
                          * for a seeded/borrowed connection (dsn unknown) */
    HlDbHandle  handle;
    int         open;
    int         owned;   /* 1 = registry closes it; 0 = seeded/borrowed */
} RegSlot;

struct HlDbRegistry {
    const HlManifest *manifest;    /* borrowed */
    HlAllocator      *alloc;       /* borrowed */
    char             *default_dsn; /* owned; the -d flag DSN for "default" */
    RegSlot           slots[HL_DB_REGISTRY_MAX];
    int               nslots;
};

HlDbRegistry *hl_db_registry_create(const HlManifest *manifest,
                                    const char *default_dsn,
                                    HlAllocator *alloc)
{
    HlDbRegistry *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->manifest = manifest;
    r->alloc = alloc;
    if (default_dsn && default_dsn[0]) {
        r->default_dsn = strdup(default_dsn);
        if (!r->default_dsn) { free(r); return NULL; }
    }
    return r;
}

/* Fast accessor for the already-open "default" connection (opened at startup
 * for migrations, so it is cached). Returns NULL if absent. No open, no error
 * path: this is the per-request hot path used by the stale-txn guards. */
HlDbHandle *hl_db_registry_default(HlDbRegistry *reg)
{
    if (!reg) return NULL;
    for (int i = 0; i < reg->nslots; i++)
        if (reg->slots[i].open && strcmp(reg->slots[i].name, "default") == 0)
            return &reg->slots[i].handle;
    return NULL;
}

void hl_db_registry_set_manifest(HlDbRegistry *reg, const HlManifest *manifest)
{
    if (reg) reg->manifest = manifest;
}

int hl_db_registry_seed(HlDbRegistry *reg, const char *name,
                        const HlDbHandle *h)
{
    if (!reg || !name || !h || !h->backend) return -1;
    if (reg->nslots >= HL_DB_REGISTRY_MAX) return -1;
    RegSlot *s = &reg->slots[reg->nslots];
    s->name = strdup(name);
    if (!s->name) return -1;
    s->handle = *h;      /* value-copy; shares the underlying ctx */
    s->open = 1;
    s->owned = 0;        /* caller keeps ownership; destroy won't close it */
    reg->nslots++;
    return 0;
}

/* Look up the DSN for @p name: manifest databases first (resolving a "$VAR"
 * env reference from the environment), then any seeded fallback is handled by
 * the cache scan in _get. Returns NULL + *err on failure. */
static const char *resolve_manifest_dsn(HlDbRegistry *reg, const char *name,
                                         const char **err)
{
    if (!reg->manifest) return NULL;
    for (int i = 0; i < reg->manifest->databases.named_count; i++) {
        const HlManifestDbNamed *d = &reg->manifest->databases.named[i];
        if (strcmp(d->name, name) != 0) continue;
        char var[128];
        if (hl_env_ref(d->dsn, var, sizeof var)) {
            const char *v = getenv(var);
            if (!v || !v[0]) {
                *err = "database DSN env var is unset";
                return NULL;
            }
            return v;
        }
        return d->dsn;
    }
    return NULL;
}

HlDbHandle *hl_db_registry_get(HlDbRegistry *reg, const char *name,
                               const char **err)
{
    if (err) *err = NULL;
    if (!reg || !name || !name[0]) {
        if (err) *err = "invalid database name";
        return NULL;
    }

    /* Cache hit (includes seeded connections like "default"). */
    for (int i = 0; i < reg->nslots; i++)
        if (reg->slots[i].open && strcmp(reg->slots[i].name, name) == 0)
            return &reg->slots[i].handle;

    if (reg->nslots >= HL_DB_REGISTRY_MAX) {
        if (err) *err = "too many database connections";
        return NULL;
    }

    const char *derr = NULL;
    const char *dsn = resolve_manifest_dsn(reg, name, &derr);
    /* "default" falls back to the -d flag DSN when the manifest declares no
     * entry for it (derr stays NULL for a plain miss; a set derr, e.g. an
     * unset dsn_env, is a real error we must surface). */
    if (!dsn && !derr && reg->default_dsn && strcmp(name, "default") == 0)
        dsn = reg->default_dsn;
    if (!dsn) {
        if (err) *err = derr ? derr
                             : "unknown database name (not in manifest.databases)";
        return NULL;
    }

    const char *sel_err = NULL;
    const HlDbBackend *be = hl_db_backend_select(dsn, &sel_err);
    if (!be) {
        if (err) *err = sel_err ? sel_err : "no database backend for dsn";
        return NULL;
    }

    RegSlot *s = &reg->slots[reg->nslots];
    memset(s, 0, sizeof *s);
    s->name = strdup(name);
    if (!s->name) { if (err) *err = "out of memory"; return NULL; }
    s->dsn = strdup(dsn);
    if (!s->dsn) {
        free(s->name);
        s->name = NULL;
        if (err) *err = "out of memory";
        return NULL;
    }
    s->handle.backend = be;
    if (be->open(&s->handle.ctx, dsn, reg->alloc) != 0) {
        free(s->name);
        s->name = NULL;
        free(s->dsn);
        s->dsn = NULL;
        if (err) *err = "failed to open database connection";
        return NULL;
    }
    s->open = 1;
    s->owned = 1;
    reg->nslots++;
    return &s->handle;
}

const char *hl_db_registry_dsn_for(HlDbRegistry *reg, const HlDbHandle *h)
{
    if (!reg || !h) return NULL;
    for (int i = 0; i < reg->nslots; i++)
        if (reg->slots[i].open && &reg->slots[i].handle == h)
            return reg->slots[i].dsn;
    return NULL;
}

const HlManifestDbDynamic *hl_db_registry_dynamic_policy(HlDbRegistry *reg)
{
    if (!reg || !reg->manifest) return NULL;
    return &reg->manifest->databases.dynamic;
}

void hl_db_registry_destroy(HlDbRegistry *reg)
{
    if (!reg) return;
    for (int i = 0; i < reg->nslots; i++) {
        RegSlot *s = &reg->slots[i];
        if (s->open && s->owned && s->handle.backend)
            s->handle.backend->close(&s->handle);
        free(s->name);
        free(s->dsn);
    }
    free(reg->default_dsn);
    free(reg);
}

#endif /* HL_ENABLE_DB */
