/*
 * hull_cap_db.c — Shared database capability
 *
 * All SQLite access goes through these functions. Both Lua and JS
 * bindings call hl_cap_db_* — neither runtime touches SQLite directly.
 * Parameterized binding is the ONLY path; no string concatenation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/db.h"
#include "hull/alloc.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Prepared statement cache ──────────────────────────────────────── */

void hl_stmt_cache_init(HlStmtCache *cache, sqlite3 *db)
{
    memset(cache, 0, sizeof(*cache));
    cache->db = db;
}

void hl_stmt_cache_destroy(HlStmtCache *cache)
{
    for (int i = 0; i < cache->count; i++) {
        sqlite3_finalize(cache->entries[i].stmt);
        free((void *)cache->entries[i].sql);
    }
    cache->count = 0;
}

/*
 * Look up a compiled statement by SQL text. On hit, reset it for reuse.
 * On miss, prepare a new statement and cache it (evicting oldest if full).
 */
static sqlite3_stmt *cache_get(HlStmtCache *cache, const char *sql)
{
    /* Search for existing entry */
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->entries[i].sql, sql) == 0) {
            /* Move to end (MRU position) */
            HlStmtCacheEntry hit = cache->entries[i];
            for (int j = i; j < cache->count - 1; j++)
                cache->entries[j] = cache->entries[j + 1];
            cache->entries[cache->count - 1] = hit;
            sqlite3_reset(hit.stmt);
            sqlite3_clear_bindings(hit.stmt);
            return hit.stmt;
        }
    }

    /* Miss — prepare new statement */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(cache->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return NULL;

    /* Evict oldest (LRU) if cache is full */
    if (cache->count >= HL_STMT_CACHE_SIZE) {
        sqlite3_finalize(cache->entries[0].stmt);
        free((void *)cache->entries[0].sql);
        for (int j = 0; j < cache->count - 1; j++)
            cache->entries[j] = cache->entries[j + 1];
        cache->count--;
    }

    /* Copy SQL string for cache ownership */
    size_t sql_len = strlen(sql);
    char *sql_copy = malloc(sql_len + 1);
    if (!sql_copy) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    memcpy(sql_copy, sql, sql_len + 1);

    cache->entries[cache->count].sql  = sql_copy;
    cache->entries[cache->count].stmt = stmt;
    cache->count++;

    return stmt;
}

/* ── Database initialization ───────────────────────────────────────── */

int hl_cap_db_init(sqlite3 *db)
{
    if (!db)
        return -1;

    /*
     * Performance PRAGMAs — applied once at connection open.
     *
     * journal_mode=WAL    — Write-Ahead Logging for concurrent readers/writer.
     * synchronous=NORMAL  — Sync WAL on checkpoint only (not every commit).
     *                       Safe: WAL protects against corruption; only risk is
     *                       losing the last transaction on OS crash (not app crash).
     * foreign_keys=ON     — Referential integrity.
     * busy_timeout=5000   — Wait up to 5 seconds on lock contention.
     * cache_size=-16384   — 16 MB page cache (default is 2 MB).
     * temp_store=MEMORY   — Temp tables/indexes in memory (not temp files).
     * mmap_size=268435456 — Memory-map up to 256 MB of the DB file for reads.
     * wal_autocheckpoint=1000 — Checkpoint every 1000 pages (~4 MB).
     *                          Default is 1000; explicit for clarity.
     */
    const char *pragmas[] = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "PRAGMA foreign_keys=ON",
        "PRAGMA busy_timeout=5000",
        "PRAGMA cache_size=-16384",
        "PRAGMA temp_store=MEMORY",
        "PRAGMA mmap_size=268435456",
        "PRAGMA wal_autocheckpoint=1000",
        NULL,
    };

    for (const char **p = pragmas; *p; p++) {
        int rc = sqlite3_exec(db, *p, NULL, NULL, NULL);
        if (rc != SQLITE_OK)
            return -1;
    }

    return 0;
}

void hl_cap_db_shutdown(sqlite3 *db)
{
    if (!db)
        return;

    /* Run PRAGMA optimize — lets SQLite update internal statistics */
    sqlite3_exec(db, "PRAGMA optimize", NULL, NULL, NULL);

    /* Final WAL checkpoint — merge WAL back into main DB file */
    sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                              NULL, NULL);
}

/* ── Internal: bind HlValue array to a prepared statement ─────────── */

static int bind_params(sqlite3_stmt *stmt, const HlValue *params, int n)
{
    for (int i = 0; i < n; i++) {
        int rc;
        int idx = i + 1; /* SQLite params are 1-indexed */

        switch (params[i].type) {
        case HL_TYPE_NIL:
            rc = sqlite3_bind_null(stmt, idx);
            break;
        case HL_TYPE_INT:
            rc = sqlite3_bind_int64(stmt, idx, params[i].i);
            break;
        case HL_TYPE_DOUBLE:
            rc = sqlite3_bind_double(stmt, idx, params[i].d);
            break;
        case HL_TYPE_TEXT:
            rc = sqlite3_bind_text(stmt, idx, params[i].s,
                                   (int)params[i].len, SQLITE_TRANSIENT);
            break;
        case HL_TYPE_BLOB:
            rc = sqlite3_bind_blob(stmt, idx, params[i].s,
                                   (int)params[i].len, SQLITE_TRANSIENT);
            break;
        case HL_TYPE_BOOL:
            rc = sqlite3_bind_int(stmt, idx, params[i].b ? 1 : 0);
            break;
        default:
            return -1;
        }

        if (rc != SQLITE_OK)
            return -1;
    }
    return 0;
}

/* ── Internal: convert a column to HlValue ────────────────────────── */

static void column_to_value(sqlite3_stmt *stmt, int col, HlValue *out)
{
    switch (sqlite3_column_type(stmt, col)) {
    case SQLITE_INTEGER:
        out->type = HL_TYPE_INT;
        out->i    = sqlite3_column_int64(stmt, col);
        break;
    case SQLITE_FLOAT:
        out->type = HL_TYPE_DOUBLE;
        out->d    = sqlite3_column_double(stmt, col);
        break;
    case SQLITE_TEXT:
        out->type = HL_TYPE_TEXT;
        out->s    = (const char *)sqlite3_column_text(stmt, col);
        out->len  = (size_t)sqlite3_column_bytes(stmt, col);
        break;
    case SQLITE_BLOB:
        out->type = HL_TYPE_BLOB;
        out->s    = (const char *)sqlite3_column_blob(stmt, col);
        out->len  = (size_t)sqlite3_column_bytes(stmt, col);
        break;
    case SQLITE_NULL:
    default:
        out->type = HL_TYPE_NIL;
        break;
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

int hl_cap_db_query(HlStmtCache *cache, const char *sql,
                    const HlValue *params, int nparams,
                    HlRowCallback cb, void *ctx,
                    HlAllocator *alloc)
{
    if (!cache || !sql || !cb)
        return -1;

    sqlite3_stmt *stmt = cache_get(cache, sql);
    if (!stmt)
        return -1;

    if (nparams > 0 && params) {
        if (bind_params(stmt, params, nparams) != 0) {
            sqlite3_reset(stmt);
            return -1;
        }
    }

    int ncols = sqlite3_column_count(stmt);
    if (ncols <= 0) {
        sqlite3_reset(stmt);
        return -1;
    }

    /* Stack-allocate columns for small result sets, heap for large */
    HlColumn stack_cols[32];
    HlValue  stack_vals[32];
    HlColumn *cols = stack_cols;
    HlValue  *vals = stack_vals;

    if (ncols > 32) {
        /* Overflow guard */
        if ((size_t)ncols > SIZE_MAX / sizeof(HlColumn) ||
            (size_t)ncols > SIZE_MAX / sizeof(HlValue)) {
            sqlite3_reset(stmt);
            return -1;
        }
        size_t cols_size = (size_t)ncols * sizeof(HlColumn);
        size_t vals_size = (size_t)ncols * sizeof(HlValue);
        cols = hl_alloc_malloc(alloc, cols_size);
        vals = hl_alloc_malloc(alloc, vals_size);
        if (!cols || !vals) {
            hl_alloc_free(alloc, cols, cols_size);
            hl_alloc_free(alloc, vals, vals_size);
            sqlite3_reset(stmt);
            return -1;
        }
    }

    /* Populate column names (stable for lifetime of stmt) */
    for (int i = 0; i < ncols; i++) {
        cols[i].name = sqlite3_column_name(stmt, i);
    }

    int result = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        for (int i = 0; i < ncols; i++) {
            column_to_value(stmt, i, &vals[i]);
            cols[i].value = vals[i];
        }
        if (cb(ctx, cols, ncols) != 0)
            break; /* caller requested stop */
    }

    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        result = -1;

    if (cols != stack_cols) {
        hl_alloc_free(alloc, cols, (size_t)ncols * sizeof(HlColumn));
        hl_alloc_free(alloc, vals, (size_t)ncols * sizeof(HlValue));
    }

    sqlite3_reset(stmt);
    return result;
}

int hl_cap_db_exec(HlStmtCache *cache, const char *sql,
                   const HlValue *params, int nparams)
{
    if (!cache || !sql)
        return -1;

    sqlite3_stmt *stmt = cache_get(cache, sql);
    if (!stmt)
        return -1;

    if (nparams > 0 && params) {
        if (bind_params(stmt, params, nparams) != 0) {
            sqlite3_reset(stmt);
            return -1;
        }
    }

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        return -1;

    return sqlite3_changes(cache->db);
}

int64_t hl_cap_db_last_id(sqlite3 *db)
{
    if (!db)
        return -1;
    return sqlite3_last_insert_rowid(db);
}

/* ── Transaction API ───────────────────────────────────────────────── */

int hl_cap_db_begin(sqlite3 *db)
{
    if (!db)
        return -1;
    return sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL)
               == SQLITE_OK ? 0 : -1;
}

int hl_cap_db_commit(sqlite3 *db)
{
    if (!db)
        return -1;
    return sqlite3_exec(db, "COMMIT", NULL, NULL, NULL)
               == SQLITE_OK ? 0 : -1;
}

int hl_cap_db_rollback(sqlite3 *db)
{
    if (!db)
        return -1;
    return sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL)
               == SQLITE_OK ? 0 : -1;
}

void hl_cap_db_guard_stale_txn(sqlite3 *db)
{
    if (db && !sqlite3_get_autocommit(db)) {
        fprintf(stderr, "[hull:c] rolling back stale transaction from previous request\n");
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
}
