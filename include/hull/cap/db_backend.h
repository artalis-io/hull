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
#include <stddef.h>

/* Forward declarations */
typedef struct HlAllocator HlAllocator;
typedef struct HlDbHandle HlDbHandle;
typedef int (*HlRowCallback)(void *ctx, HlColumn *cols, int ncols);

/* ── Backend vtable ───────────────────────────────────────────────── */

/* Backend identity for consumers that need the native connection handle
 * (udf registration, agent introspection) without depending on a concrete
 * backend symbol. Pairs with the native_handle vtable method. */
typedef enum {
    HL_DB_NATIVE_NONE = 0,
    HL_DB_NATIVE_SQLITE,
    HL_DB_NATIVE_POSTGRES,
} HlDbNativeTag;

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
    const char *name;   /* "sqlite", "postgres", "none" */

    /* NULL-terminated list of DSN schemes this backend claims, matched
     * (case-insensitively) against the text before "://" in a DSN by
     * hl_db_backend_select. e.g. {"postgres", "postgresql", NULL}. A NULL
     * schemes pointer means the backend is not DSN-selectable (it is never
     * matched, and scheme-less DSNs fall through to the SQLite default). */
    const char *const *schemes;

    /* DDL fragment that declares an integer primary-key column
     * with auto-increment semantics. SQLite: "INTEGER PRIMARY
     * KEY AUTOINCREMENT". Postgres: "BIGSERIAL PRIMARY KEY".
     * Used by stdlib modules in CREATE TABLE statements where
     * a surrogate id is needed (audit-log, outbox). */
    const char *autoincrement_id_ddl;

    /* Identifier-quoting char for this dialect: '"' for SQLite / Postgres /
     * DuckDB, '`' for MySQL. Used by hl_db_quote_ident to wrap a table /
     * column name so a reserved word or special char is safe. 0 falls back
     * to '"'. */
    char identifier_quote;

    /* Backend identity (see HlDbNativeTag). Pairs with native_handle. */
    HlDbNativeTag native_tag;

    /* Capability: this backend supports user-defined SQL functions (db.udf).
     * SQLite: 1. Postgres: 0 (no in-process UDF). Gates whether the connection
     * object exposes a `udf` sub-object at all, so "method present but fails at
     * call time" doesn't happen. A future DuckDB backend would set it. */
    unsigned char supports_udf;

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
    /* Execute a (possibly multi-statement) SQL script with no parameters.
     * `exec` runs a single statement (SQLite stmt cache / PG extended
     * protocol); multi-statement migration files need this. SQLite:
     * sqlite3_exec. Postgres: the simple Query protocol. Results discarded.
     * NULL = unsupported (caller must fall back or error). Trusted SQL only
     * (no parameterization). */
    int    (*exec_script)(HlDbHandle *h, const char *sql);
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

    /* Native connection handle (sqlite3* for SQLite, PGconn* for Postgres),
     * or NULL. Consumers key off native_tag to know the concrete type; this
     * replaces the per-backend hl_db_<x>_raw accessors. NULL = none exposed. */
    void  *(*native_handle)(HlDbHandle *h);
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

/* Returns -1 if the backend lacks exec_script (caller decides how to
 * degrade); errors from the backend also return -1. */
static inline int hl_db_exec_script(HlDbHandle *h, const char *sql)
{
    if (!h || !h->backend || !h->backend->exec_script) return -1;
    return h->backend->exec_script(h, sql);
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

/* Native connection handle + its backend tag, for the few paths that need the
 * concrete sqlite3* / PGconn* (udf registration, agent introspection). Keeps
 * the abstract interface free of any concrete-backend symbol. *out_tag is set
 * even when the return is NULL, so a caller can distinguish "not that backend"
 * from "no handle". */
static inline void *hl_db_backend_native_handle(HlDbHandle *h,
                                                HlDbNativeTag *out_tag)
{
    if (out_tag)
        *out_tag = (h && h->backend) ? h->backend->native_tag
                                     : HL_DB_NATIVE_NONE;
    if (!h || !h->backend || !h->backend->native_handle) return NULL;
    return h->backend->native_handle(h);
}

/* Quote @p name as a SQL identifier for @p h's dialect into @p out (capacity
 * @p outsz), wrapping in the dialect's quote char and doubling any internal
 * occurrence of it. Returns the byte length written (excluding the NUL), or -1
 * if it would not fit. A NULL / none backend uses '"'. Defends the stdlib
 * against reserved words + lets a MySQL backend (backtick) drop in unchanged. */
static inline int hl_db_quote_ident(HlDbHandle *h, const char *name,
                                    char *out, size_t outsz)
{
    if (!name || !out || outsz < 3) return -1;
    char q = (h && h->backend && h->backend->identifier_quote)
             ? h->backend->identifier_quote : '"';
    size_t o = 0;
    out[o++] = q;
    for (const char *p = name; *p; p++) {
        if (*p == q) {                       /* escape by doubling */
            if (o + 1 >= outsz) return -1;
            out[o++] = q;
        }
        if (o + 1 >= outsz) return -1;
        out[o++] = *p;
    }
    if (o + 2 > outsz) return -1;            /* closing quote + NUL */
    out[o++] = q;
    out[o] = '\0';
    return (int)o;
}

/* ── Backend selection ────────────────────────────────────────────── */

/*
 * Choose a backend for @p dsn by its "<scheme>://" prefix, matched against each
 * compiled backend's `schemes` list: "postgres://" / "postgresql://" -> PG,
 * "sqlite://" -> SQLite. A scheme-less DSN (a bare path, ":memory:", or a
 * single-colon "file:" URI) defaults to SQLite. Reserved-but-uncompiled schemes
 * (e.g. "duckdb://", "mysql://", or "postgres://" without HL_ENABLE_POSTGRES)
 * return NULL with a specific *err hint; an unrecognized scheme returns NULL
 * with a generic hint. *err (when non-NULL) is always a static message; never
 * allocates. Adding a backend needs no change here (see db_select.c BACKENDS[]).
 */
const HlDbBackend *hl_db_backend_select(const char *dsn, const char **err);

#endif /* HL_CAP_DB_BACKEND_H */
