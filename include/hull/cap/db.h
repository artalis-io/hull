/*
 * cap/db.h — Database capability (SQLite)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_H
#define HL_CAP_DB_H

#include "hull/cap/types.h"

/* ── Prepared statement cache ──────────────────────────────────────── */

#define HL_STMT_CACHE_SIZE 32

typedef struct {
    const char     *sql;     /* SQL text (owned copy) */
    sqlite3_stmt   *stmt;    /* compiled statement */
} HlStmtCacheEntry;

typedef struct HlStmtCache {
    sqlite3           *db;
    HlStmtCacheEntry   entries[HL_STMT_CACHE_SIZE];
    int                count;
} HlStmtCache;

void hl_stmt_cache_init(HlStmtCache *cache, sqlite3 *db);
void hl_stmt_cache_destroy(HlStmtCache *cache);

/* ── Database initialization ───────────────────────────────────────── */

int hl_cap_db_init(sqlite3 *db);
void hl_cap_db_shutdown(sqlite3 *db);

/* ── Query API ─────────────────────────────────────────────────────── */

typedef int (*HlRowCallback)(void *ctx, HlColumn *cols, int ncols);

int hl_cap_db_query(HlStmtCache *cache, const char *sql,
                    const HlValue *params, int nparams,
                    HlRowCallback cb, void *ctx,
                    HlAllocator *alloc);

int hl_cap_db_exec(HlStmtCache *cache, const char *sql,
                   const HlValue *params, int nparams);

int64_t hl_cap_db_last_id(sqlite3 *db);

/* ── Transaction API ───────────────────────────────────────────────── */

int hl_cap_db_begin(sqlite3 *db);
int hl_cap_db_commit(sqlite3 *db);
int hl_cap_db_rollback(sqlite3 *db);

/* Roll back any stale transaction left by a crashed handler.
 * Safe to call unconditionally before each request dispatch. */
void hl_cap_db_guard_stale_txn(sqlite3 *db);

#endif /* HL_CAP_DB_H */
