/*
 * hull_cap_db.c — Shared database capability
 *
 * All SQLite access goes through these functions. Both Lua and JS
 * bindings call hl_cap_db_* — neither runtime touches SQLite directly.
 * Parameterized binding is the ONLY path; no string concatenation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/hull_cap.h"
#include "hull/hull_alloc.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

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

int hl_cap_db_query(sqlite3 *db, const char *sql,
                      const HlValue *params, int nparams,
                      HlRowCallback cb, void *ctx,
                      HlAllocator *alloc)
{
    if (!db || !sql || !cb)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    if (nparams > 0 && params) {
        if (bind_params(stmt, params, nparams) != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    int ncols = sqlite3_column_count(stmt);
    if (ncols <= 0) {
        sqlite3_finalize(stmt);
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
            sqlite3_finalize(stmt);
            return -1;
        }
        size_t cols_size = (size_t)ncols * sizeof(HlColumn);
        size_t vals_size = (size_t)ncols * sizeof(HlValue);
        cols = hl_alloc_malloc(alloc, cols_size);
        vals = hl_alloc_malloc(alloc, vals_size);
        if (!cols || !vals) {
            hl_alloc_free(alloc, cols, cols_size);
            hl_alloc_free(alloc, vals, vals_size);
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    /* Populate column names (stable for lifetime of stmt) */
    for (int i = 0; i < ncols; i++) {
        cols[i].name = sqlite3_column_name(stmt, i);
    }

    int result = 0;
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

    sqlite3_finalize(stmt);
    return result;
}

int hl_cap_db_exec(sqlite3 *db, const char *sql,
                     const HlValue *params, int nparams)
{
    if (!db || !sql)
        return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    if (nparams > 0 && params) {
        if (bind_params(stmt, params, nparams) != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        return -1;

    return sqlite3_changes(db);
}

int64_t hl_cap_db_last_id(sqlite3 *db)
{
    if (!db)
        return -1;
    return sqlite3_last_insert_rowid(db);
}
