/*
 * migrate.h — SQL migration runner
 *
 * Convention-based migrations: discovers *.sql files from embedded
 * entries or filesystem, tracks applied migrations in _hull_migrations,
 * executes pending ones in filename order.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_MIGRATE_H
#define HL_MIGRATE_H

#include <sqlite3.h>

/* Forward declaration */
typedef struct HlVfs HlVfs;

/* Return codes for hl_migrate_run() */
#define HL_MIGRATE_ERR    (-1)   /* SQL error during migration */
#define HL_MIGRATE_NO_DIR (-2)   /* no migrations dir and no embedded entries */

/*
 * Discover and apply pending SQL migrations.
 *
 * Uses vfs for O(log n) lookup of "migrations/" entries (embedded in built
 * binaries). If no embedded entries found and vfs->root_dir is set, falls back
 * to scanning root_dir/migrations/ for .sql files.
 * Migrations are sorted by filename and executed in order.
 * Each migration runs in its own BEGIN IMMEDIATE / COMMIT.
 *
 * Returns: count of applied migrations (>=0), HL_MIGRATE_ERR, or HL_MIGRATE_NO_DIR.
 */
int hl_migrate_run(sqlite3 *db, const HlVfs *vfs);

/* ── Status query ──────────────────────────────────────────────────── */

typedef struct {
    const char *name;       /* migration filename (e.g. "001_init.sql") */
    int         applied;    /* 1 = applied, 0 = pending */
    const char *applied_at; /* datetime string or NULL if pending */
} HlMigrationStatus;

/*
 * Query migration status: lists all discovered migrations with
 * their applied/pending state.
 *
 * Caller must free with hl_migrate_status_free().
 * Returns 0 on success, -1 on error.
 */
int hl_migrate_status(sqlite3 *db, const HlVfs *vfs,
                      HlMigrationStatus **out, int *out_count);

void hl_migrate_status_free(HlMigrationStatus *entries, int count);

#endif /* HL_MIGRATE_H */
