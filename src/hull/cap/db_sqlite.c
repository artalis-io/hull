/*
 * cap/db_sqlite.c — SQLite backend for the database vtable
 *
 * Wraps existing hl_cap_db_* functions into the HlDbBackend vtable.
 * Statement cache, init/shutdown, and all SQLite-specific operations
 * are encapsulated in HlDbSqliteCtx.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db_backend.h"
#include "hull/cap/db.h"
#include "hull/utils/alloc.h"

#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── SQLite context ───────────────────────────────────────────────── */

typedef struct {
    sqlite3     *db;
    HlStmtCache  cache;
    HlAllocator *alloc;
} HlDbSqliteCtx;

/* ── Vtable implementations ──────────────────────────────────────── */

static int sqlite_open(void **ctx, const char *dsn, HlAllocator *alloc)
{
    HlDbSqliteCtx *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    s->alloc = alloc;

    int rc = sqlite3_open(dsn, &s->db);
    if (rc != SQLITE_OK) {
        if (s->db) sqlite3_close(s->db);
        free(s);
        return -1;
    }

    if (hl_cap_db_init(s->db) != 0) {
        sqlite3_close(s->db);
        free(s);
        return -1;
    }

    hl_stmt_cache_init(&s->cache, s->db, alloc);

    *ctx = s;
    return 0;
}

static void sqlite_close(HlDbHandle *h)
{
    if (!h || !h->ctx) return;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;

    hl_stmt_cache_destroy(&s->cache);
    hl_cap_db_shutdown(s->db);
    sqlite3_close(s->db);
    free(s);
}

static int sqlite_query(HlDbHandle *h, const char *sql,
                        const HlValue *params, int nparams,
                        HlRowCallback cb, void *cb_ctx,
                        HlAllocator *alloc)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return hl_cap_db_query(&s->cache, sql, params, nparams,
                           cb, cb_ctx, alloc);
}

static int sqlite_exec(HlDbHandle *h, const char *sql,
                       const HlValue *params, int nparams)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return hl_cap_db_exec(&s->cache, sql, params, nparams);
}

/* Multi-statement script, no params. sqlite3_exec compiles + runs every
 * ;-separated statement in one call (unlike the single-statement stmt
 * cache path). The specific error stays reachable via sqlite3_errmsg. */
static int sqlite_exec_script(HlDbHandle *h, const char *sql)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    char *err = NULL;
    int rc = sqlite3_exec(s->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int sqlite_begin(HlDbHandle *h)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return hl_cap_db_begin(s->db);
}

static int sqlite_commit(HlDbHandle *h)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return hl_cap_db_commit(s->db);
}

static int sqlite_rollback(HlDbHandle *h)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return hl_cap_db_rollback(s->db);
}

static int64_t sqlite_last_id(HlDbHandle *h)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return hl_cap_db_last_id(s->db);
}

static const char *sqlite_errmsg(HlDbHandle *h)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return sqlite3_errmsg(s->db);
}

static void sqlite_guard_stale_txn(HlDbHandle *h)
{
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    hl_cap_db_guard_stale_txn(s->db);
}

/* ── Dialect-aware SQL helpers ───────────────────────────────────────
 *
 * These keep the stdlib (auth-flows, audit-log, session, rbac, etc.)
 * free of SQLite-specific syntax. Each builds the right SQL for the
 * SQLite dialect and dispatches to the existing exec path.
 *
 * Each helper allocates a temporary buffer on the stack-with-malloc-
 * fallback pattern so a 4 KB SQL string fits without heap, and longer
 * ones don't overflow the stack. */

/* Conservative upper bound for a generated SQL string: every column
 * name contributes once for the column list, once for VALUES (with a
 * `?`), and in the upsert case once more for the DO UPDATE clause.
 * We cap each identifier at 63 chars (SQL standard) and add fixed
 * overhead for keywords. */
static size_t sqlite_estimate_sql_size(const char *table,
                                        int n_conflict, int n_cols,
                                        int include_update)
{
    size_t per_ident = 64;       /* col name + comma + space */
    size_t fixed = 200;          /* keywords, parens */
    size_t cols_part = (size_t)n_cols * (per_ident + 4);   /* + "?, " */
    size_t conflict_part = (size_t)n_conflict * per_ident;
    size_t update_part = include_update
        ? (size_t)n_cols * (per_ident * 2 + 16)  /* "col=excluded.col, " */
        : 0;
    return strlen(table) + fixed + cols_part + conflict_part + update_part;
}

/* Append helper — bounded snprintf into an offset cursor. Returns
 * remaining capacity (or -1 on overflow). */
static int sqlite_append(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    if (*off >= cap) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *off) return -1;
    *off += (size_t)n;
    return (int)(cap - *off);
}

static int sqlite_insert_if_absent(HlDbHandle *h, const char *table,
                                    const char *const *conflict_cols,
                                    int n_conflict,
                                    const char *const *cols,
                                    const HlValue *values, int n_cols)
{
    if (!table || n_cols < 1 || !cols || !values) return -1;
    /* conflict_cols may be NULL/0 — in that case we emit
     * "INSERT OR IGNORE" without an explicit conflict target,
     * which SQLite treats as "any constraint violation". */
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    size_t cap = sqlite_estimate_sql_size(table, n_conflict, n_cols, 0);
    char stack_buf[4096];
    char *buf = (cap <= sizeof(stack_buf)) ? stack_buf : malloc(cap);
    if (!buf) return -1;
    size_t off = 0;

    int rc = -1;
    if (sqlite_append(buf, cap, &off, "INSERT OR IGNORE INTO %s (", table) < 0)
        goto done;
    for (int i = 0; i < n_cols; i++) {
        if (sqlite_append(buf, cap, &off, "%s%s",
                          i == 0 ? "" : ", ", cols[i]) < 0) goto done;
    }
    if (sqlite_append(buf, cap, &off, ") VALUES (") < 0) goto done;
    for (int i = 0; i < n_cols; i++) {
        if (sqlite_append(buf, cap, &off, "%s?", i == 0 ? "" : ", ") < 0)
            goto done;
    }
    if (sqlite_append(buf, cap, &off, ")") < 0) goto done;

    rc = hl_cap_db_exec(&s->cache, buf, values, n_cols);
done:
    if (buf != stack_buf) free(buf);
    return rc;
}

static int sqlite_upsert(HlDbHandle *h, const char *table,
                          const char *const *conflict_cols, int n_conflict,
                          const char *const *cols,
                          const HlValue *values, int n_cols)
{
    if (!table || n_cols < 1 || !cols || !values
        || n_conflict < 1 || !conflict_cols) return -1;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    size_t cap = sqlite_estimate_sql_size(table, n_conflict, n_cols, 1);
    char stack_buf[4096];
    char *buf = (cap <= sizeof(stack_buf)) ? stack_buf : malloc(cap);
    if (!buf) return -1;
    size_t off = 0;

    int rc = -1;
    /* INSERT INTO t (c1, c2, ...) VALUES (?, ?, ...) */
    if (sqlite_append(buf, cap, &off, "INSERT INTO %s (", table) < 0)
        goto done;
    for (int i = 0; i < n_cols; i++) {
        if (sqlite_append(buf, cap, &off, "%s%s",
                          i == 0 ? "" : ", ", cols[i]) < 0) goto done;
    }
    if (sqlite_append(buf, cap, &off, ") VALUES (") < 0) goto done;
    for (int i = 0; i < n_cols; i++) {
        if (sqlite_append(buf, cap, &off, "%s?", i == 0 ? "" : ", ") < 0)
            goto done;
    }
    if (sqlite_append(buf, cap, &off, ") ON CONFLICT(") < 0) goto done;
    for (int i = 0; i < n_conflict; i++) {
        if (sqlite_append(buf, cap, &off, "%s%s",
                          i == 0 ? "" : ", ", conflict_cols[i]) < 0)
            goto done;
    }
    if (sqlite_append(buf, cap, &off, ") DO UPDATE SET ") < 0) goto done;
    /* Skip conflict cols on the update side — they're the keys. */
    int first = 1;
    for (int i = 0; i < n_cols; i++) {
        int is_conflict = 0;
        for (int j = 0; j < n_conflict; j++) {
            if (strcmp(cols[i], conflict_cols[j]) == 0) {
                is_conflict = 1; break;
            }
        }
        if (is_conflict) continue;
        if (sqlite_append(buf, cap, &off, "%s%s=excluded.%s",
                          first ? "" : ", ", cols[i], cols[i]) < 0)
            goto done;
        first = 0;
    }
    if (first) {
        /* All columns are conflict columns — degenerate "INSERT
         * OR IGNORE" semantics. Emit DO NOTHING. */
        off -= strlen(" DO UPDATE SET ");
        if (sqlite_append(buf, cap, &off, " DO NOTHING") < 0) goto done;
    }

    rc = hl_cap_db_exec(&s->cache, buf, values, n_cols);
done:
    if (buf != stack_buf) free(buf);
    return rc;
}

/* Row callback for table_columns: PRAGMA table_info returns
 * (cid, name, type, notnull, dflt_value, pk). We pull column 1
 * ("name") and pass to the caller's HlDbColumnCallback. */
typedef struct {
    HlDbColumnCallback cb;
    void              *cb_ctx;
} HlDbColumnsForward;

static int sqlite_table_columns_row_cb(void *ctx, HlColumn *cols, int ncols)
{
    HlDbColumnsForward *fwd = (HlDbColumnsForward *)ctx;
    for (int i = 0; i < ncols; i++) {
        if (cols[i].name && strcmp(cols[i].name, "name") == 0
            && cols[i].value.type == HL_TYPE_TEXT
            && cols[i].value.s) {
            fwd->cb(fwd->cb_ctx, cols[i].value.s);
            return 0;
        }
    }
    return 0;
}

static int sqlite_table_columns(HlDbHandle *h, const char *table,
                                 HlDbColumnCallback cb, void *cb_ctx)
{
    if (!table || !cb) return -1;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    /* PRAGMA table_info doesn't accept bound parameters in SQLite
     * <3.41; substitute table name directly. The stdlib never
     * passes user input here (table names come from module
     * source). Still, sanitise to A-Z a-z 0-9 _ to be safe. */
    for (const char *p = table; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_')) {
            return -1;
        }
    }
    char sql[160];
    int n = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    if (n < 0 || (size_t)n >= sizeof(sql)) return -1;
    HlDbColumnsForward fwd = { cb, cb_ctx };
    return hl_cap_db_query(&s->cache, sql, NULL, 0,
                           sqlite_table_columns_row_cb, &fwd, NULL);
}

/* ── Exported backend ─────────────────────────────────────────────── */

const HlDbBackend hl_db_backend_sqlite = {
    .name                  = "sqlite",
    .autoincrement_id_ddl  = "INTEGER PRIMARY KEY AUTOINCREMENT",
    .open                  = sqlite_open,
    .close                 = sqlite_close,
    .query                 = sqlite_query,
    .exec                  = sqlite_exec,
    .exec_script           = sqlite_exec_script,
    .begin                 = sqlite_begin,
    .commit                = sqlite_commit,
    .rollback              = sqlite_rollback,
    .last_id               = sqlite_last_id,
    .errmsg                = sqlite_errmsg,
    .guard_stale_txn       = sqlite_guard_stale_txn,
    .insert_if_absent      = sqlite_insert_if_absent,
    .upsert                = sqlite_upsert,
    .table_columns         = sqlite_table_columns,
};

/* ── Accessors ────────────────────────────────────────────────────── */

sqlite3 *hl_db_sqlite_raw(HlDbHandle *h)
{
    if (!h || h->backend != &hl_db_backend_sqlite) return NULL;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return s ? s->db : NULL;
}

HlStmtCache *hl_db_sqlite_cache(HlDbHandle *h)
{
    if (!h || h->backend != &hl_db_backend_sqlite) return NULL;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    return s ? &s->cache : NULL;
}

/* ── Wrap an externally-owned sqlite3* ────────────────────────────── */

int hl_db_sqlite_wrap(HlDbHandle *out, sqlite3 *db)
{
    if (!out || !db) return -1;
    HlDbSqliteCtx *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->db    = db;       /* borrowed — caller owns lifetime */
    s->alloc = NULL;
    hl_stmt_cache_init(&s->cache, db, NULL);
    out->backend = &hl_db_backend_sqlite;
    out->ctx     = s;
    return 0;
}

void hl_db_sqlite_unwrap(HlDbHandle *h)
{
    if (!h || h->backend != &hl_db_backend_sqlite || !h->ctx) return;
    HlDbSqliteCtx *s = (HlDbSqliteCtx *)h->ctx;
    hl_stmt_cache_destroy(&s->cache);
    /* NOTE: s->db is borrowed — do NOT sqlite3_close it here. */
    free(s);
    h->ctx = NULL;
}

#endif /* HL_ENABLE_DB */
