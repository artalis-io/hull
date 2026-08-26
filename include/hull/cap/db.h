/**
 * @file cap/db.h
 * @brief Database capability (SQLite-backed).
 *
 * Mediates every database access from Lua/JS runtimes. Apps that don't
 * need SQL can be built without this whole subsystem via
 * `make HL_ENABLE_DB=0` - see CLAUDE.md "Compute-only builds".
 *
 * Security invariants:
 *   - All SQL goes through parameterised binding (#hl_cap_db_query /
 *     #hl_cap_db_exec). Hull does not expose any path that interpolates
 *     user data into SQL.
 *   - Tables matching `_hull_*` are reserved for stdlib internals; see
 *     #hl_cap_db_check_namespace. User code is rejected by a call-stack
 *     check (Lua `ar.source` / JS module name).
 *
 * Related: CLAUDE.md "## Stdlib Middleware" `db.*` global.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_H
#define HL_CAP_DB_H

#include "hull/cap/types.h"

/* Forward declaration */
typedef struct HlAllocator HlAllocator;

/* ── Error codes ─────────────────────────────────────────────────────── */

/**
 * @brief Domain error codes for `hl_cap_db_*` functions.
 *
 * Returned where the function signature says `int` and the documentation
 * doesn't specify a different convention (e.g. affected-row count).
 */
typedef enum {
    HL_DB_OK           =  0,  /**< Success. */
    HL_DB_ERR_PREPARE  = -1,  /**< `sqlite3_prepare_v2` failed (invalid SQL). */
    HL_DB_ERR_BIND     = -2,  /**< `sqlite3_bind_*` failed (param type/count mismatch). */
    HL_DB_ERR_EXEC     = -3,  /**< `sqlite3_step` returned an error. */
    HL_DB_ERR_BUSY     = -4,  /**< Lock contention; caller may retry. */
    HL_DB_ERR_DENIED   = -5,  /**< Namespace violation (`_hull_*` table access from user code). */
} HlDbError;

/* ── Prepared statement cache ──────────────────────────────────────── */

/** Max entries in a per-connection prepared-statement LRU cache.
 *  @internal */
#define HL_STMT_CACHE_SIZE 32

/**
 * @brief Single entry in the prepared-statement cache.
 * @internal Tier 4 - layout will change post-v0.1.0.
 */
typedef struct {
    const char     *sql;        /**< Owned key - the SQL string the statement was prepared for. */
    size_t          sql_len;    /**< Byte length of `sql` (excluding NUL). */
    sqlite3_stmt   *stmt;       /**< Prepared statement; finalized by @ref hl_stmt_cache_destroy. */
} HlStmtCacheEntry;

/**
 * @brief Per-connection prepared-statement LRU cache.
 * @internal Tier 4 - treat as opaque; use the entry points below.
 */
typedef struct HlStmtCache {
    sqlite3           *db;
    HlAllocator       *alloc;
    HlStmtCacheEntry   entries[HL_STMT_CACHE_SIZE];
    int                count;
} HlStmtCache;

/**
 * @brief Initialize a prepared-statement LRU cache.
 *
 * @param cache  Caller-owned cache struct to populate. Memory uninitialised
 *               on entry; left zero+populated on exit.
 * @param db     Live `sqlite3` handle from `sqlite3_open*`. Borrowed -
 *               not retained on @ref hl_stmt_cache_destroy.
 * @param alloc  Allocator for any cache-internal allocations. `NULL` =
 *               raw `malloc`/`free`.
 *
 * @note The cache has a fixed capacity of @ref HL_STMT_CACHE_SIZE
 *       entries. Inserting a 33rd evicts the LRU entry (with
 *       `sqlite3_finalize`).
 */
void hl_stmt_cache_init(HlStmtCache *cache, sqlite3 *db, HlAllocator *alloc);

/**
 * @brief Finalize every prepared statement in the cache and zero the struct.
 *
 * @param cache  Cache to destroy. Safe on a zeroed struct (no-op).
 *
 * @note Does NOT close the underlying `sqlite3` handle.
 */
void hl_stmt_cache_destroy(HlStmtCache *cache);

/* ── Database initialization ───────────────────────────────────────── */

/**
 * @brief Apply Hull's standard PRAGMA configuration to a fresh handle.
 *
 * Sets WAL mode, foreign keys on, busy timeout, secure-delete=off,
 * and the journal size limit. Idempotent on an already-initialized
 * handle.
 *
 * @param db  Live handle from `sqlite3_open*`.
 *
 * @return `0` on success, `-1` if any PRAGMA failed (handle should be
 *         closed by caller).
 *
 * @par Example:
 * @code
 * sqlite3 *db;
 * if (sqlite3_open(":memory:", &db) != SQLITE_OK) return -1;
 * if (hl_cap_db_init(db) != 0) { sqlite3_close(db); return -1; }
 * @endcode
 */
int hl_cap_db_init(sqlite3 *db);

/**
 * @brief Flush pending writes and finalize cached statements.
 *
 * Optional - `sqlite3_close` works without it - but recommended for
 * clean shutdown logs.
 *
 * @param db  Handle to shut down. Caller still calls `sqlite3_close`
 *            after.
 */
void hl_cap_db_shutdown(sqlite3 *db);

/* ── Query API ─────────────────────────────────────────────────────── */

/**
 * @brief Per-row callback for @ref hl_cap_db_query.
 *
 * @param ctx    Opaque pointer passed through from the call site.
 * @param cols   Array of `ncols` column results for this row. Pointer
 *               is owned by the caller (allocated from the same
 *               @ref HlAllocator passed to `hl_cap_db_query`).
 * @param ncols  Number of columns.
 *
 * @return `0` to continue iteration, non-zero to abort. The abort value
 *         is returned to the caller of @ref hl_cap_db_query.
 */
typedef int (*HlRowCallback)(void *ctx, HlColumn *cols, int ncols);

/**
 * @brief Run a SELECT with parameterised bindings, invoking @p cb per row.
 *
 * @param cache    Prepared-statement cache (lookup-or-prepare).
 * @param sql      Literal SQL with `?` placeholders. **MUST NOT** contain
 *                 interpolated user input.
 * @param params   Array of `nparams` parameter values, bound to the `?`
 *                 placeholders in order. May be `NULL` iff @p nparams is `0`.
 * @param nparams  Length of @p params.
 * @param cb       Row callback. Return non-zero to abort iteration.
 * @param ctx      Opaque pointer passed back to @p cb.
 * @param alloc    Allocator for the per-row `HlColumn` array. `NULL`
 *                 means raw malloc.
 *
 * @return `0` on full iteration completion, an @ref HlDbError code on
 *         prepare/bind/step failure, or the value the callback returned
 *         to abort.
 *
 * @par Example:
 * @code
 * static int print_row(void *ctx, HlColumn *cols, int ncols) {
 *     for (int i = 0; i < ncols; i++)
 *         printf("%s=%s ", cols[i].name, cols[i].value.s);
 *     return 0;
 * }
 * HlValue params[] = { { .type = HL_TYPE_INT, .i = 42 } };
 * hl_cap_db_query(cache, "SELECT id, name FROM users WHERE id = ?",
 *                 params, 1, print_row, NULL, NULL);
 * @endcode
 *
 * @see hl_cap_db_exec
 * @see hl_cap_db_check_namespace
 */
int hl_cap_db_query(HlStmtCache *cache, const char *sql,
                    const HlValue *params, int nparams,
                    HlRowCallback cb, void *ctx,
                    HlAllocator *alloc);

/**
 * @brief Run INSERT/UPDATE/DELETE/DDL with parameterised bindings.
 *
 * @param cache    Prepared-statement cache.
 * @param sql      Literal SQL with `?` placeholders.
 * @param params   Bindings (may be `NULL` iff @p nparams is `0`).
 * @param nparams  Length of @p params.
 *
 * @return Number of affected rows on success (≥ 0), or an @ref HlDbError
 *         code on failure.
 *
 * @see hl_cap_db_last_id
 */
int hl_cap_db_exec(HlStmtCache *cache, const char *sql,
                   const HlValue *params, int nparams);

/**
 * @brief Return the ROWID of the most-recently-inserted row.
 *
 * @param db  Connection. Each connection has its own counter.
 *
 * @return SQLite's `last_insert_rowid()` for this connection; `0` if no
 *         INSERT has occurred since the connection was opened.
 */
int64_t hl_cap_db_last_id(sqlite3 *db);

/* ── Transaction API ───────────────────────────────────────────────── */

/**
 * @brief Begin a transaction with `BEGIN IMMEDIATE`.
 *
 * @param db  Connection.
 *
 * @return @ref HL_DB_OK on success; an @ref HlDbError code on failure
 *         (typically @ref HL_DB_ERR_BUSY if another writer holds the lock).
 *
 * @warning Nested transactions are **not** supported by SQLite. Call
 *          this only when no transaction is open on the same connection.
 *          App code should prefer the stdlib `db.batch(fn)` wrapper.
 */
int hl_cap_db_begin(sqlite3 *db);

/**
 * @brief Commit the current transaction.
 * @param db  Connection.
 * @return @ref HL_DB_OK on success, @ref HlDbError otherwise.
 */
int hl_cap_db_commit(sqlite3 *db);

/**
 * @brief Roll back the current transaction.
 * @param db  Connection.
 * @return @ref HL_DB_OK on success, @ref HlDbError otherwise.
 */
int hl_cap_db_rollback(sqlite3 *db);

/**
 * @brief Roll back any stale transaction left by a crashed handler.
 *
 * Safe to call unconditionally before each request dispatch - no-op
 * if no transaction is open.
 *
 * @param db  Connection.
 */
void hl_cap_db_guard_stale_txn(sqlite3 *db);

/**
 * @brief Reject SQL referencing the internal `_hull_*` namespace.
 *
 * Used by @ref hl_cap_db_query / @ref hl_cap_db_exec to enforce the
 * stdlib/user-code boundary. The actual gate uses call-stack inspection
 * (Lua `ar.source` prefix, JS module name) so stdlib modules transparently
 * bypass this check.
 *
 * @param sql  SQL string to scan.
 *
 * @return `0` if safe, @ref HL_DB_ERR_DENIED if the SQL touches
 *         `_hull_*` tables.
 *
 * @note Direct callers from C (no caller-source context) MUST invoke
 *       this before any stdlib-internal SQL - the dispatch helper in
 *       `cap/db.c` already does so for the public entry points.
 */
int hl_cap_db_check_namespace(const char *sql);

#endif /* HL_CAP_DB_H */
