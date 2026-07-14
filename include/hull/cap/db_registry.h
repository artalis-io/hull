/*
 * cap/db_registry.h: Named database connection registry
 *
 * Backs the multi-backend API (§1 Phase 5b): an app declares named
 * connections via `manifest.databases`, and db.connect(name) / db.default()
 * resolve them through this registry. Connections open lazily on first use
 * and are cached by name, so an app can mix, e.g., a Postgres primary and a
 * local SQLite cache in one process.
 *
 * The connection named "default" is the stdlib / db.default() target; when an
 * app declares no databases map it falls back to the -d flag DSN, seeded via
 * hl_db_registry_seed so the already-opened default connection is reused
 * rather than opened twice.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_REGISTRY_H
#define HL_CAP_DB_REGISTRY_H

#include "hull/cap/db_backend.h"

typedef struct HlManifest  HlManifest;
typedef struct HlAllocator HlAllocator;
typedef struct HlDbRegistry HlDbRegistry;

/*
 * Create a registry. @p manifest supplies the declared databases map (may be
 * NULL); it is borrowed and must outlive the registry (the sealed manifest
 * does). Nothing is opened here. @p alloc is passed to each backend open
 * (may be NULL for raw malloc). Returns NULL on allocation failure.
 */
HlDbRegistry *hl_db_registry_create(const HlManifest *manifest,
                                    const char *default_dsn,
                                    HlAllocator *alloc);

/*
 * Fast accessor for the already-open "default" connection (the -d flag DSN,
 * opened at startup so it is cached). Returns NULL if absent (compute-only /
 * --no-db). No open, no error path: this is the per-request hot path used by
 * the stale-transaction guards.
 *
 * The per-request guard sites call this unconditionally, so a pure-compute
 * build (no registry compiled in) gets an inline no-op instead of an
 * undefined symbol.
 */
#ifdef HL_ENABLE_DB
HlDbHandle *hl_db_registry_default(HlDbRegistry *reg);
#else
static inline HlDbHandle *hl_db_registry_default(HlDbRegistry *reg)
{ (void)reg; return (HlDbHandle *)0; }
#endif

/*
 * Point the registry at the app's databases map. The manifest is only known
 * after the app runs app.manifest(), which is after the registry is created
 * at db-open time, so the serve path injects the sealed manifest here once it
 * is available. Borrowed; must outlive the registry (the sealed manifest
 * does). Until set, only seeded connections (e.g. "default") resolve.
 */
void hl_db_registry_set_manifest(HlDbRegistry *reg, const HlManifest *manifest);

/*
 * Seed a pre-opened, externally-owned connection under @p name (typically
 * "default", the -d flag connection that app_context already opened). The
 * handle is value-copied and marked not-owned, so hl_db_registry_destroy
 * will NOT close it; the caller keeps ownership. Returns 0 / -1.
 */
int hl_db_registry_seed(HlDbRegistry *reg, const char *name,
                        const HlDbHandle *h);

/*
 * Resolve @p name to an open connection, opening + caching it on first use.
 * Resolution order: a cached connection, then a manifest databases entry
 * (with { dsn_env } resolved from the environment), then a seeded connection.
 * Returns NULL with *err (a static message) set on unknown name / unset
 * dsn_env / backend-select failure / open failure. The returned handle is
 * owned by the registry (unless seeded) and valid until destroy.
 */
HlDbHandle *hl_db_registry_get(HlDbRegistry *reg, const char *name,
                               const char **err);

/* Close every registry-owned connection and free the registry. Seeded
 * (externally-owned) connections are left untouched. */
void hl_db_registry_destroy(HlDbRegistry *reg);

#endif /* HL_CAP_DB_REGISTRY_H */
