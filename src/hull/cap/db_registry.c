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
#include "hull/cap/policy_seal.h"  /* hl_policy_page_* */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* One extra slot beyond the manifest cap for the seeded "default". */
#define HL_DB_REGISTRY_MAX (HL_MANIFEST_MAX_DATABASES + 1)

/* The registry name of the stdlib's internal connection. It starts with a
 * byte no manifest name can, and hl_db_registry_get refuses it, so app code
 * cannot reach the connection by name (db.connect). */
#define INTERNAL_NAME "\x01internal"

/* Longest -d DSN the registry keeps (inline, in the sealed span). */
#define HL_DB_REGISTRY_DSN_MAX 8192

typedef struct {
    char       *name;    /* owned */
    int         dsn_known; /* 0 for a seeded/borrowed connection */
    HlDbHandle  handle;
    int         open;
    int         owned;   /* 1 = registry closes it; 0 = seeded/borrowed */
} RegSlot;

struct HlDbRegistry {
    /* Where each connection's DSN comes from: a leading policy span (see
     * cap/policy_seal.h), sealed by hl_db_registry_seal once wiring is done,
     * so a write cannot point a database at another host. The connection
     * cache after it stays writable. */
    union {
        struct {
            const HlManifest *manifest;    /* borrowed (the sealed manifest) */
            int               has_default_dsn;
            char              default_dsn[HL_DB_REGISTRY_DSN_MAX]; /* the -d DSN */
            /* databases.internal learned before the manifest is wired (a
             * stdlib init() at app top level); the wired manifest's own
             * value takes over once set. */
            int               has_internal_dsn;
            char              internal_dsn[HL_DB_REGISTRY_DSN_MAX];
        };
        unsigned char policy_span[HL_POLICY_SPAN];
    };
    int               sealed;
    HlAllocator      *alloc;       /* borrowed */
    RegSlot           slots[HL_DB_REGISTRY_MAX];
    int               nslots;
};

HlDbRegistry *hl_db_registry_create(const HlManifest *manifest,
                                    const char *default_dsn,
                                    HlAllocator *alloc)
{
    size_t dlen = default_dsn ? strlen(default_dsn) : 0;
    if (dlen >= HL_DB_REGISTRY_DSN_MAX) return NULL;
    HlDbRegistry *r = hl_policy_page_alloc(sizeof *r);   /* zeroed */
    if (!r) return NULL;
    r->manifest = manifest;
    r->alloc = alloc;
    if (dlen > 0) {
        memcpy(r->default_dsn, default_dsn, dlen + 1);
        r->has_default_dsn = 1;
    }
    return r;
}

int hl_db_registry_seal(HlDbRegistry *reg)
{
    if (!reg) return -1;
    if (reg->sealed) return 0;
    if (hl_policy_page_seal(reg) != 0) return -1;
    reg->sealed = 1;
    return 0;
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
    if (reg && !reg->sealed) reg->manifest = manifest;
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

/* The DSN for @p name, from the sealed sources only: the manifest, then for
 * "default" the -d DSN when the manifest declares no entry for it (derr stays
 * NULL for a plain miss; a set derr, e.g. an unset env ref, is a real error).
 * Worked out afresh on every call rather than kept per connection: a stored
 * copy would be writable heap the worker pool connects to. */
static const char *lookup_dsn(HlDbRegistry *reg, const char *name,
                              const char **err)
{
    if (strcmp(name, INTERNAL_NAME) == 0) {
        const char *d = (reg->manifest && reg->manifest->databases.internal)
                        ? reg->manifest->databases.internal
                        : (reg->has_internal_dsn ? reg->internal_dsn : NULL);
        char var[128];
        if (d && hl_env_ref(d, var, sizeof var)) {
            const char *v = getenv(var);
            if (!v || !v[0]) {
                *err = "databases.internal env var is unset";
                return NULL;
            }
            return v;
        }
        return d;
    }
    const char *dsn = resolve_manifest_dsn(reg, name, err);
    if (!dsn && !*err && reg->has_default_dsn && strcmp(name, "default") == 0)
        dsn = reg->default_dsn;
    return dsn;
}

static HlDbHandle *registry_get(HlDbRegistry *reg, const char *name,
                                const char **err);

HlDbHandle *hl_db_registry_get(HlDbRegistry *reg, const char *name,
                               const char **err)
{
    if (err) *err = NULL;
    if (!reg || !name || !name[0] || name[0] == INTERNAL_NAME[0]) {
        if (err) *err = "invalid database name";
        return NULL;
    }
    return registry_get(reg, name, err);
}

int hl_db_registry_has_internal(const HlDbRegistry *reg)
{
    return reg && ((reg->manifest && reg->manifest->databases.internal)
                   || reg->has_internal_dsn);
}

HlDbHandle *hl_db_registry_internal(HlDbRegistry *reg, const char **err)
{
    if (err) *err = NULL;
    if (!reg) return NULL;
    if (!hl_db_registry_has_internal(reg))
        return registry_get(reg, "default", err);
    return registry_get(reg, INTERNAL_NAME, err);
}

int hl_db_registry_set_internal_dsn(HlDbRegistry *reg, const char *dsn)
{
    if (!reg || reg->sealed) return -1;
    if (!dsn || !dsn[0]) {
        reg->has_internal_dsn = 0;
        return 0;
    }
    size_t n = strlen(dsn);
    if (n >= sizeof reg->internal_dsn) return -1;
    memcpy(reg->internal_dsn, dsn, n + 1);
    reg->has_internal_dsn = 1;
    return 0;
}

int hl_db_registry_default_is_network(const HlDbRegistry *reg)
{
    if (!reg) return 0;
    const char *err = NULL;
    const char *dsn = lookup_dsn((HlDbRegistry *)(uintptr_t)reg, "default", &err);
    if (!dsn) return 0;
    if (dsn[0] == '$') return 1;
    return strncmp(dsn, "postgres", 8) == 0 || strncmp(dsn, "mysql", 5) == 0
        || strncmp(dsn, "mariadb", 7) == 0;
}

int hl_db_registry_manifest_wired(const HlDbRegistry *reg)
{
    return reg && reg->manifest;
}

static HlDbHandle *registry_get(HlDbRegistry *reg, const char *name,
                                const char **err)
{

    /* Cache hit (includes seeded connections like "default"). */
    for (int i = 0; i < reg->nslots; i++)
        if (reg->slots[i].open && strcmp(reg->slots[i].name, name) == 0)
            return &reg->slots[i].handle;

    if (reg->nslots >= HL_DB_REGISTRY_MAX) {
        if (err) *err = "too many database connections";
        return NULL;
    }

    const char *derr = NULL;
    const char *dsn = lookup_dsn(reg, name, &derr);
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
    s->handle.backend = be;
    if (be->open(&s->handle.ctx, dsn, reg->alloc) != 0) {
        free(s->name);
        s->name = NULL;
        if (err) *err = "failed to open database connection";
        return NULL;
    }
    s->dsn_known = 1;
    s->open = 1;
    s->owned = 1;
    reg->nslots++;
    return &s->handle;
}

const char *hl_db_registry_dsn_for(HlDbRegistry *reg, const HlDbHandle *h)
{
    if (!reg || !h) return NULL;
    for (int i = 0; i < reg->nslots; i++) {
        const RegSlot *s = &reg->slots[i];
        if (s->open && &s->handle == h) {
            if (!s->dsn_known) return NULL;
            const char *err = NULL;
            return lookup_dsn(reg, s->name, &err);
        }
    }
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
    }
    if (reg->sealed) hl_policy_page_unseal(reg);
    hl_policy_page_free(reg, sizeof *reg);
}

#endif /* HL_ENABLE_DB */
