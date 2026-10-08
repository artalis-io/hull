/*
 * cap/db_backend.h - Database backend vtable
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
#include "hull/cap/db_budget.h"   /* hl_db_batch_leave: unbound cleanup */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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
    HL_DB_NATIVE_MYSQL,
    HL_DB_NATIVE_DUCKDB,
} HlDbNativeTag;

/* Callback used by `table_columns`: invoked once per column name
 * in whatever order the backend's catalog returns them. */
typedef void (*HlDbColumnCallback)(void *cb_ctx, const char *col_name);

/* Per-backend SQL dialect descriptor: the single source of dialect truth,
 * consumed by hl_db_quote_ident, the connection object's `dialect` sub-table,
 * and (in future) the query / schema builders. Every field is a compile-time
 * constant per backend, so the whole HlDbBackend stays in .rodata. Only axes
 * where backends actually differ AND a consumer needs them are modelled;
 * LIMIT/OFFSET, boolean literals, and string concat are uniform (or arrive as
 * bound params) across the current backends and are added when one diverges. */
typedef struct HlDbDialect {
    /* Identifier-quoting char: '"' for SQLite / Postgres / DuckDB, '`' for
     * MySQL. hl_db_quote_ident wraps a name with it (doubling internals).
     * 0 falls back to '"'. */
    char identifier_quote;

    /* Native parameter placeholder style: "?" (SQLite / MySQL / DuckDB) or
     * "$n" (Postgres). The query builder emits '?' uniformly and the Postgres
     * backend rewrites '?' -> '$n' in C, so this is exposed only so a raw-SQL
     * author who bypasses the builder knows the native style. */
    const char *placeholder;

    /* Upsert grammar: "on_conflict" (SQLite / Postgres / DuckDB) or
     * "on_duplicate_key" (MySQL). Consumed by the builder's upsert compile. */
    const char *upsert_style;

    /* Whether INSERT/UPDATE/DELETE ... RETURNING is supported. 0 => the
     * builder's .returning() falls back to last_id (single-row only). MySQL 8
     * is 0 (MariaDB is 1, but the shared backend is conservative). */
    unsigned char supports_returning;

    /* Whether CREATE INDEX ... IF NOT EXISTS is native. 0 = MySQL 8, where the
     * backend transparently rewrites it and swallows the duplicate-index error,
     * so the schema builder can still emit the portable form. */
    unsigned char supports_index_if_not_exists;

    /* Whether SELECT ... FOR UPDATE SKIP LOCKED is supported (Postgres 9.5+,
     * MySQL 8+). 0 for SQLite, whose single-writer serializes every write so a
     * claim is atomic without lock-skipping. Read by hull/jobs (via
     * conn.dialect.supports_skip_locked) to pick the concurrency-safe atomic
     * claim shape per backend. See docs/jobs_design.md. */
    unsigned char supports_skip_locked;

    /* Whether the backend supports a low-latency LISTEN/NOTIFY wakeup (the
     * optional wait_notify vtable method below). 1 = Postgres; 0 = SQLite /
     * MySQL / DuckDB, which keep polling. Read by hull/jobs (via
     * conn.dialect.supports_notify) to decide whether an idle worker parks on
     * a NOTIFY-wait or a plain sleep. Latency only, never a correctness
     * dependency: every wait is bounded by a poll timeout. See
     * docs/jobs_events_phase4_design.md. */
    unsigned char supports_notify;

    /* Inline DDL fragment declaring an auto-increment integer primary-key
     * column. SQLite: "INTEGER PRIMARY KEY AUTOINCREMENT". Postgres:
     * "BIGSERIAL PRIMARY KEY". MySQL: "BIGINT AUTO_INCREMENT PRIMARY KEY".
     * DuckDB references a sequence: "BIGINT DEFAULT nextval('%s') PRIMARY KEY".
     * Used by stdlib CREATE TABLE (audit-log, outbox) and the schema builder. */
    const char *identity_column;

    /* printf-style companion template ("CREATE SEQUENCE %s") for engines whose
     * identity needs a separate sequence (DuckDB); NULL where identity is
     * inline. The schema builder generates one sequence name and threads it
     * through both identity_sequence and identity_column. */
    const char *identity_sequence;
} HlDbDialect;

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

    /* SQL dialect descriptor: the single home for identifier quoting,
     * placeholder style, upsert grammar, RETURNING support, and identity DDL.
     * hl_db_quote_ident / hl_db_autoincrement_id_ddl read it, and the
     * connection object exposes it as `conn.dialect`. See HlDbDialect. */
    HlDbDialect dialect;

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

    /* Dialect-aware SQL helpers - moved into the vtable so the
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

    /* Optional low-latency wakeup: LISTEN on @p channel (idempotent per
     * connection) then block up to @p timeout_ms for a notification. Returns
     * 1 if notified, 0 on timeout, -1 on a dead connection. NULL where the
     * backend has no such primitive (SQLite / MySQL / DuckDB) - callers gate
     * on dialect.supports_notify. This is a blocking call, meant to run on the
     * db.async worker pool, not the event-loop thread. See
     * docs/jobs_events_phase4_design.md. */
    int    (*wait_notify)(HlDbHandle *h, const char *channel, int timeout_ms);

    /* Whether the connection is inside a transaction (1) or in autocommit
     * (0), from the backend's own state - SQLite's autocommit flag, Postgres'
     * ReadyForQuery status, MySQL's SERVER_STATUS_IN_TRANS, DuckDB's tracked
     * BEGIN / COMMIT / ROLLBACK. A connection lost inside a transaction
     * counts as inside one until the app rolls back. No I/O. NULL = unknown
     * (treated as 0). Used to refuse a wait while a transaction is open on a
     * shared connection (hl_db_registry_open_txn) and by hl_db_batch_*. */
    int    (*in_txn)(HlDbHandle *h);
} HlDbBackend;

struct HlDbHandle {
    const HlDbBackend *backend;
    void              *ctx;
    int                batch_depth;   /* open db.batch levels (hl_db_batch_*) */
    /* A static message for the last hl_db_batch_* failure that the backend
     * did not report itself (its transaction ended underneath the batch).
     * hl_db_errmsg returns it until the next statement. */
    const char        *batch_err;
    /* Set while the session holds state a new connection would not have
     * (the migration lock: hl_migrate_run). A network backend then refuses
     * to reconnect a lost connection instead of going on without it. */
    int                session_pinned;
};

/* ── Inline wrappers ──────────────────────────────────────────────── */

static inline int hl_db_query(HlDbHandle *h, const char *sql,
                              const HlValue *params, int nparams,
                              HlRowCallback cb, void *cb_ctx,
                              HlAllocator *alloc)
{
    if (!h || !h->backend) return -1;
    h->batch_err = NULL;
    return h->backend->query(h, sql, params, nparams,
                             cb, cb_ctx, alloc);
}

static inline int hl_db_exec(HlDbHandle *h, const char *sql,
                             const HlValue *params, int nparams)
{
    if (!h || !h->backend) return -1;
    h->batch_err = NULL;
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
    if (h->batch_err) return h->batch_err;
    return h->backend->errmsg(h);
}

/* 1 inside a transaction, 0 in autocommit or unknown (no in_txn method). */
static inline int hl_db_in_txn(HlDbHandle *h)
{
    if (!h || !h->backend || !h->backend->in_txn) return 0;
    return h->backend->in_txn(h) ? 1 : 0;
}

/* Roll back a transaction left open by an entry that has finished. Any
 * db.batch bookkeeping goes with it: a batch cannot legitimately span an
 * entry (its fn cannot wait), so a depth still set here belongs to the
 * transaction just rolled back, and keeping it would turn the next batch's
 * BEGIN into a SAVEPOINT outside any transaction (audit 5 L1). */
static inline void hl_db_guard_stale_txn(HlDbHandle *h)
{
    if (!h || !h->backend) return;
    if (h->backend->guard_stale_txn) h->backend->guard_stale_txn(h);
    h->batch_depth = 0;
}

/* Low-latency LISTEN/NOTIFY wait. Returns 1 (notified), 0 (timeout), or -1
 * (dead connection / backend has no wait_notify). Callers gate on
 * dialect.supports_notify; a -1 from an unsupported backend is a caller bug,
 * not a runtime path (hull/jobs never calls this unless supports_notify). */
static inline int hl_db_wait_notify(HlDbHandle *h, const char *channel,
                                    int timeout_ms)
{
    if (!h || !h->backend || !h->backend->wait_notify) return -1;
    return h->backend->wait_notify(h, channel, timeout_ms);
}

static inline const char *hl_db_autoincrement_id_ddl(HlDbHandle *h)
{
    if (!h || !h->backend || !h->backend->dialect.identity_column)
        return "INTEGER PRIMARY KEY";
    return h->backend->dialect.identity_column;
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
    char q = (h && h->backend && h->backend->dialect.identifier_quote)
             ? h->backend->dialect.identifier_quote : '"';
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

/*
 * Write a loggable form of @p dsn to @p out: "<scheme>://<host>[:port]" for a
 * DSN with a scheme (user, password, database and every query parameter -
 * a "?password=" too - dropped), "<scheme>://(redacted)" when an '@' follows
 * the authority (a password holding an unencoded '/', '?' or '#' cannot be
 * told from an '@' in the path or query), the DSN itself for a scheme-less
 * file path.
 * For error messages: a network DSN carries the password, often a resolved
 * "$VAR" secret. Returns snprintf's length.
 */
size_t hl_db_dsn_redact(const char *dsn, char *out, size_t outsz);

/*
 * Feature backends: large, composable DB connectors (e.g. DuckDB) that are NOT
 * compiled into the base but linked in at `hull build` time from a signed
 * feature lib. Returns a table of `*count` backends the build composed in;
 * hl_db_backend_select iterates it after the base BACKENDS[].
 *
 * The base ships a WEAK default returning an empty set (a base build has no
 * feature backends). A feature build links a STRONG override - a generated
 * const registry that references each composed feature's backend - which the
 * linker prefers. The override MUST be a direct object (not an archive member)
 * so it displaces the weak default. See docs/features_and_flavors.md §3.2.
 */
const HlDbBackend *const *hl_db_feature_backends(size_t *count);

/* ── Nested db.batch ────────────────────────────────────────────────
 *
 * db.batch opens a transaction; a batch run inside another one (a stdlib
 * helper that batches, called from the app's batch) is a SAVEPOINT in the
 * outer transaction. It used to issue its own BEGIN and COMMIT: on Postgres
 * the inner COMMIT committed the caller's transaction early (the nested BEGIN
 * is only a warning), on MySQL START TRANSACTION commits the open one, and
 * SQLite raised. DuckDB has no savepoints, so there an inner batch joins the
 * outer transaction (its error still reaches the outer batch's fn).
 *
 * enter: 0, or -1 with the backend error in hl_db_errmsg.
 * leave(ok): commits / releases when ok, rolls back otherwise; 0, or -1 when
 * the commit or release failed (the transaction is then rolled back). */
static inline int hl_db_batch_savepoints_(const HlDbHandle *h)
{
    return !(h->backend && h->backend->name &&
             strcmp(h->backend->name, "duckdb") == 0);
}

static inline void hl_db_batch_spname_(char *buf, size_t sz, const char *verb,
                                       int level)
{
    snprintf(buf, sz, "%s hull_batch_%d", verb, level);
}

/* The transaction an open batch relies on has ended underneath it: the
 * batch's fn ran a raw COMMIT / ROLLBACK, or an in-process nested dispatch
 * (hull test's test.post inside a batch) ran the stale-transaction guard.
 * Only knowable on a backend with in_txn. */
static inline int hl_db_batch_lost_(HlDbHandle *h)
{
    return h->batch_depth > 0 && h->backend && h->backend->in_txn &&
           !hl_db_in_txn(h);
}

#define HL_DB_BATCH_LOST_MSG \
    "the batch's transaction was ended inside the batch (a COMMIT or " \
    "ROLLBACK statement, a nested request, or on MySQL a DDL statement in a " \
    "nested batch), so its statements were not applied as one transaction"

static inline int hl_db_batch_enter(HlDbHandle *h)
{
    h->batch_err = NULL;
    /* An enclosing batch whose transaction is gone: a SAVEPOINT now would run
     * outside any transaction (SQLite silently opens one that RELEASE then
     * commits; Postgres refuses). Start this batch as a fresh transaction;
     * the enclosing batch's leave reports the loss. */
    if (hl_db_batch_lost_(h)) h->batch_depth = 0;
    if (h->batch_depth == 0) {
        if (hl_db_begin(h) != 0) return -1;
    } else if (hl_db_batch_savepoints_(h)) {
        char sql[64];
        hl_db_batch_spname_(sql, sizeof sql, "SAVEPOINT", h->batch_depth);
        if (hl_db_exec(h, sql, NULL, 0) < 0) return -1;
    }
    h->batch_depth++;
    return 0;
}

static inline int hl_db_batch_leave(HlDbHandle *h, int ok)
{
    h->batch_err = NULL;
    if (h->batch_depth <= 0) {
        /* The depth was reset under this batch (the guard, or a nested batch
         * that found the transaction gone): its transaction is gone. */
        if (ok) h->batch_err = HL_DB_BATCH_LOST_MSG;
        return ok ? -1 : 0;
    }
    if (hl_db_batch_lost_(h)) {
        h->batch_depth--;
        if (!ok) return 0;
        h->batch_err = HL_DB_BATCH_LOST_MSG;
        return -1;
    }
    int level = --h->batch_depth;
    if (level == 0) {
        if (!ok) { hl_db_rollback(h); return 0; }
        if (hl_db_commit(h) != 0) {
            /* Roll back only what is still open: a COMMIT the server refused
             * (Postgres / MySQL) already ended the transaction, and a needless
             * ROLLBACK would replace the commit's error message. */
            if (!h->backend->in_txn || hl_db_in_txn(h)) hl_db_rollback(h);
            return -1;
        }
        return 0;
    }
    if (!hl_db_batch_savepoints_(h)) return 0;
    char sql[64];
    if (!ok) {
        /* Cleanup for a fn that failed - perhaps by exhausting the run's
         * budget, which would refuse these statements' allocations
         * (cap/db_budget.h): run them unbound (audit 10 H3). */
        HlDbBudgetBinding budget = hl_db_budget_swap(NULL, NULL);
        hl_db_batch_spname_(sql, sizeof sql, "ROLLBACK TO SAVEPOINT", level);
        (void)hl_db_exec(h, sql, NULL, 0);
        hl_db_batch_spname_(sql, sizeof sql, "RELEASE SAVEPOINT", level);
        int rc = hl_db_exec(h, sql, NULL, 0) < 0 ? -1 : 0;
        hl_db_budget_restore(budget);
        return rc;
    }
    hl_db_batch_spname_(sql, sizeof sql, "RELEASE SAVEPOINT", level);
    return hl_db_exec(h, sql, NULL, 0) < 0 ? -1 : 0;
}

#endif /* HL_CAP_DB_BACKEND_H */
