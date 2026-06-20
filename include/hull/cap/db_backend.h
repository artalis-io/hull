/*
 * cap/db_backend.h — Database backend vtable
 *
 * Decouples the query engine from SQLite via a pluggable vtable.
 * Enables pure compute apps (no DB), future alternative backends
 * (PostgreSQL, DuckDB), while hull internals always use embedded SQLite.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_BACKEND_H
#define HL_CAP_DB_BACKEND_H

#include "hull/cap/types.h"
#include <stdint.h>

/* Forward declarations */
typedef struct HlAllocator HlAllocator;
typedef struct HlDbHandle HlDbHandle;
typedef int (*HlRowCallback)(void *ctx, HlColumn *cols, int ncols);

/* ── Backend vtable ───────────────────────────────────────────────── */

/* Callback used by `table_columns`: invoked once per column name
 * in whatever order the backend's catalog returns them. */
typedef void (*HlDbColumnCallback)(void *cb_ctx, const char *col_name);

/* Vtable methods take `HlDbHandle *h` instead of `void *ctx`.  The
 * concrete backend context lives in h->ctx and each method casts it
 * to its own concrete type at the top of the function (the cast is
 * a normal pointer access inside the body, not visible to CFI).
 *
 * Compared to the historical `void *ctx` shape, the typed handle
 * gives clang -fsanitize=cfi-icall a matching signature at every
 * call site (`int(*)(HlDbHandle*, ...)` registered, same expected at
 * the dispatch site) so CFI no longer flags the polymorphic vtable
 * dispatch as a type mismatch.  See docs/security.md § 4c. */
typedef struct HlDbBackend {
    const char *name;   /* "sqlite", "none" */

    /* DDL fragment that declares an integer primary-key column
     * with auto-increment semantics. SQLite: "INTEGER PRIMARY
     * KEY AUTOINCREMENT". Postgres: "BIGSERIAL PRIMARY KEY".
     * Used by stdlib modules in CREATE TABLE statements where
     * a surrogate id is needed (audit-log, outbox). */
    const char *autoincrement_id_ddl;

    /* `open` is the one method that doesn't take an HlDbHandle*
     * because the handle is what `open` populates.  Output is
     * written to *out_ctx for the caller to wire into a handle. */
    int    (*open)(void **out_ctx, const char *dsn, HlAllocator *alloc);
    void   (*close)(HlDbHandle *h);
    int    (*query)(HlDbHandle *h, const char *sql,
                    const HlValue *params, int nparams,
                    HlRowCallback cb, void *cb_ctx, HlAllocator *alloc);
    int    (*exec)(HlDbHandle *h, const char *sql,
                   const HlValue *params, int nparams);
    int    (*begin)(HlDbHandle *h);
    int    (*commit)(HlDbHandle *h);
    int    (*rollback)(HlDbHandle *h);
    int64_t (*last_id)(HlDbHandle *h);
    const char *(*errmsg)(HlDbHandle *h);
    void   (*guard_stale_txn)(HlDbHandle *h);  /* NULL = no-op */

    /* Dialect-aware SQL helpers — moved into the vtable so the
     * stdlib stays DB-agnostic.
     *
     * insert_if_absent: `INSERT OR IGNORE` (SQLite) /
     *                   `INSERT ... ON CONFLICT(...) DO NOTHING` (PG).
     * upsert:           `INSERT OR REPLACE` (SQLite) /
     *                   `INSERT ... ON CONFLICT(...) DO UPDATE SET
     *                   col=excluded.col, ...` (PG).
     * table_columns:    `PRAGMA table_info(t)` (SQLite) /
     *                   information_schema query (PG). Calls @p cb
     *                   once per column. */
    int    (*insert_if_absent)(HlDbHandle *h, const char *table,
                                const char *const *conflict_cols,
                                int n_conflict,
                                const char *const *cols,
                                const HlValue *values, int n_cols);
    int    (*upsert)(HlDbHandle *h, const char *table,
                     const char *const *conflict_cols, int n_conflict,
                     const char *const *cols,
                     const HlValue *values, int n_cols);
    int    (*table_columns)(HlDbHandle *h, const char *table,
                            HlDbColumnCallback cb, void *cb_ctx);
} HlDbBackend;

struct HlDbHandle {
    const HlDbBackend *backend;
    void              *ctx;
};

/* ── Inline wrappers ──────────────────────────────────────────────── */

static inline int hl_db_query(HlDbHandle *h, const char *sql,
                              const HlValue *params, int nparams,
                              HlRowCallback cb, void *cb_ctx,
                              HlAllocator *alloc)
{
    if (!h || !h->backend) return -1;
    return h->backend->query(h, sql, params, nparams,
                             cb, cb_ctx, alloc);
}

static inline int hl_db_exec(HlDbHandle *h, const char *sql,
                             const HlValue *params, int nparams)
{
    if (!h || !h->backend) return -1;
    return h->backend->exec(h, sql, params, nparams);
}

static inline int hl_db_begin(HlDbHandle *h)
{
    if (!h || !h->backend) return -1;
    return h->backend->begin(h);
}

static inline int hl_db_commit(HlDbHandle *h)
{
    if (!h || !h->backend) return -1;
    return h->backend->commit(h);
}

static inline int hl_db_rollback(HlDbHandle *h)
{
    if (!h || !h->backend) return -1;
    return h->backend->rollback(h);
}

static inline int64_t hl_db_last_id(HlDbHandle *h)
{
    if (!h || !h->backend) return -1;
    return h->backend->last_id(h);
}

static inline const char *hl_db_errmsg(HlDbHandle *h)
{
    if (!h || !h->backend) return "no database";
    return h->backend->errmsg(h);
}

static inline void hl_db_guard_stale_txn(HlDbHandle *h)
{
    if (!h || !h->backend || !h->backend->guard_stale_txn) return;
    h->backend->guard_stale_txn(h);
}

static inline const char *hl_db_autoincrement_id_ddl(HlDbHandle *h)
{
    if (!h || !h->backend || !h->backend->autoincrement_id_ddl)
        return "INTEGER PRIMARY KEY";
    return h->backend->autoincrement_id_ddl;
}

static inline int hl_db_insert_if_absent(HlDbHandle *h, const char *table,
                                          const char *const *conflict_cols,
                                          int n_conflict,
                                          const char *const *cols,
                                          const HlValue *values, int n_cols)
{
    if (!h || !h->backend || !h->backend->insert_if_absent) return -1;
    return h->backend->insert_if_absent(h, table,
                                         conflict_cols, n_conflict,
                                         cols, values, n_cols);
}

static inline int hl_db_upsert(HlDbHandle *h, const char *table,
                                const char *const *conflict_cols,
                                int n_conflict,
                                const char *const *cols,
                                const HlValue *values, int n_cols)
{
    if (!h || !h->backend || !h->backend->upsert) return -1;
    return h->backend->upsert(h, table, conflict_cols, n_conflict,
                              cols, values, n_cols);
}

static inline int hl_db_table_columns(HlDbHandle *h, const char *table,
                                       HlDbColumnCallback cb, void *cb_ctx)
{
    if (!h || !h->backend || !h->backend->table_columns) return -1;
    return h->backend->table_columns(h, table, cb, cb_ctx);
}

/* ── SQLite backend ───────────────────────────────────────────────── */

extern const HlDbBackend hl_db_backend_sqlite;

/* Get raw sqlite3* from an HlDbHandle (NULL if not SQLite backend) */
struct sqlite3 *hl_db_sqlite_raw(HlDbHandle *h);

/* Get statement cache from a SQLite-backed HlDbHandle (NULL if not SQLite) */
struct HlStmtCache *hl_db_sqlite_cache(HlDbHandle *h);

/*
 * Wrap an externally-managed sqlite3* in an HlDbHandle for code paths that
 * still open SQLite directly (agent_lib, tests). The handle does NOT own
 * the underlying sqlite3* — caller stays responsible for sqlite3_close.
 *
 * Allocates a small adapter context (with its own statement cache pointing
 * at the borrowed sqlite3). Call hl_db_sqlite_unwrap(&handle) when done.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
struct sqlite3;  /* forward decl for parameter type below */
int  hl_db_sqlite_wrap(HlDbHandle *out, struct sqlite3 *db);
void hl_db_sqlite_unwrap(HlDbHandle *h);

#endif /* HL_CAP_DB_BACKEND_H */
