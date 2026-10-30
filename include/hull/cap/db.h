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
    HL_DB_ERR_REENTERED = -6, /**< Called while one of this connection's statements is stepping (from a UDF). */
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
    /* Set while a statement from this cache is being stepped. A UDF runs
     * inside that step; a query or exec from it on the same connection
     * re-entered the cache, whose LRU eviction (or reset, for the same
     * SQL) finalized the statement still executing - a use-after-free.
     * Such a call is refused with HL_DB_ERR_REENTERED. */
    int                stepping;
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
 * The rollback is verified (sqlite3_get_autocommit); a failed one is retried
 * once after resetting the connection's statements (audit 11).
 *
 * @param db  Connection.
 * @return `0` when the connection is out of any transaction, `-1` when it is
 *         still inside one (the caller must replace the connection).
 */
int hl_cap_db_guard_stale_txn(sqlite3 *db);

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

/**
 * @brief Whether @p sql calls one of DuckDB's dynamic-SQL table functions -
 * `query(sql)`, `query_table(name)`, `json_execute_serialized_sql(plan)` -
 * which assemble a statement or a table name at run time and so get past
 * #hl_cap_db_check_namespace (audit 25). The DuckDB backend refuses such a
 * statement. Read conservatively: the name, bare or as a "quoted"
 * identifier (any case), followed by `(` past whitespace and comments
 * (nesting or not); string literals are NOT skipped, so the text inside one
 * is refused too, and a U&"..." identifier always counts. 1 = calls one.
 */
int hl_cap_db_sql_calls_dynamic_sql(const char *sql);

/**
 * @brief Whether @p sql holds a DuckDB statement that runs SQL from a file or
 * copies every table without naming one - IMPORT DATABASE (it runs the
 * directory's schema.sql / load.sql), EXPORT DATABASE, COPY FROM DATABASE -
 * or names the pragmas behind them (import_database, export_database,
 * copy_database), past #hl_cap_db_check_namespace (audit 32). The DuckDB
 * backend refuses such a text under every file policy. Read conservatively,
 * as #hl_cap_db_sql_calls_dynamic_sql: every word of the text (string
 * literals are not skipped), the keywords in any case with whitespace and
 * comments (nesting or not) between them; any statement of a multi-statement
 * text counts. 1 = refused.
 */
int hl_cap_db_sql_reads_whole_database(const char *sql);

/**
 * @brief Whether @p sql holds a DuckDB statement that changes which catalog or
 * schema an unqualified name resolves to - ATTACH / DETACH another database
 * file, USE a database/schema, or SET / RESET / PRAGMA of `schema` or
 * `search_path` - past #hl_cap_db_check_namespace (audit 33). Once a second
 * catalog is attached or made current, a stored view or macro in it reads the
 * app's `_hull_*` tables cross-catalog without the name ever appearing in the
 * app's SQL text, so the lexical check cannot see it. The DuckDB backend
 * refuses such a text under every file policy. Read conservatively, as
 * #hl_cap_db_sql_reads_whole_database: every word of the text (string literals
 * are not skipped), the keywords in any case with whitespace and comments
 * (nesting or not) between them; any statement of a multi-statement text
 * counts. 1 = refused.
 */
int hl_cap_db_sql_switches_catalog(const char *sql);

/**
 * @brief Whether @p sql hands a DuckDB table function or pragma a TABLE NAME
 * it can build at run time, or names one refused outright (audit 34).
 * DuckDB constant-folds table-function arguments, so
 * `pragma_storage_info(concat('_', 'hull_sessions'))` reaches a `_hull_*`
 * table past #hl_cap_db_check_namespace. Refused in every spelling (bare or
 * "quoted", any case, PRAGMA / CALL / FROM): `storage_info` /
 * `pragma_storage_info` (column min / max statistics), and the profiling and
 * logging calls DuckDB runs past lock_configuration. `pragma_table_info`,
 * `table_info`, `pragma_show`, `show`, `histogram`, `histogram_values` and
 * `duckdb_table_sample` are refused when called (`(` or `=`) with anything
 * but one plain argument - a '...' literal or a (dotted, maybe "quoted")
 * name - so `histogram(col)` (the aggregate) passes. `show` / `table_info`
 * count in their `= value` spelling only after PRAGMA (a `"show" = 1` column
 * is not one), and `histogram` only after a table-position word (FROM, JOIN,
 * CALL, PRAGMA, `,`, `(`, ...) with more than one argument, so the aggregate
 * passes anywhere (audit 35). `arrow_scan` / `arrow_scan_dumb` (POINTER
 * arguments) are refused by name. Read as
 * #hl_cap_db_sql_calls_dynamic_sql (literals not skipped, both comment
 * rules); every look-ahead shares one byte budget, past which the text is
 * refused. In a text that creates a macro or function (see
 * #hl_cap_db_sql_creates_macro) the reading is strict (audit 37): a bare
 * name in a macro body may be a PARAMETER, which the call fills with a value
 * built at run time (`CREATE MACRO hm(t, c) AS TABLE FROM histogram(t, c)`,
 * then `FROM hm('_hu' || 'll_sessions', id)`), so only a '...' literal is a
 * plain argument there, and `histogram` with more than one argument is
 * refused in every position. 1 = refused.
 */
int hl_cap_db_sql_names_table_by_value(const char *sql);

/**
 * @brief #hl_cap_db_sql_names_table_by_value, read strictly when @p in_macro
 * is set (for a stored macro's definition text: every bare name may be a
 * parameter).
 */
int hl_cap_db_sql_names_table_by_value_ex(const char *sql, int in_macro);

/**
 * @brief Whether @p sql creates a DuckDB macro or function: `CREATE`, any run
 * of `OR` / `REPLACE` / `TEMP` / `TEMPORARY` / `LOCAL` / `GLOBAL` /
 * `UNLOGGED` / `PERSISTENT`, then `MACRO` or `FUNCTION`, keywords in any
 * case with whitespace and comments (nesting or not) between them, in any
 * statement of the text (literals not skipped). A look-ahead past the byte
 * budget counts as a macro (audit 37). 1 = it does.
 */
int hl_cap_db_sql_creates_macro(const char *sql);

/**
 * @brief Whether @p sql can reach the DuckDB database file itself (audit 34).
 * DuckDB adds the database file and its `.wal`, `.wal.checkpoint` and
 * `.wal.recovery` sidecars to `allowed_paths` (and `<db>.tmp/` to
 * `allowed_directories`) whenever external access is off, and app SQL cannot
 * clear them (DuckDB refuses to change `allowed_paths` then), so
 * `read_blob('app.duckdb')` returned the raw file, `_hull_*` rows included.
 * Refused: any literal naming @p protect_path's file as a path component
 * (case-insensitive; preceded by the start or a separator, followed by the
 * end, a separator or a `.wal` / `.tmp` suffix - every spelling of a path to
 * the file, a sidecar or the temp directory; the text between every two
 * consecutive `'`, `"` or `$` is read, whatever the parity; audit 35); an
 * E'...' or U&'...' literal with a backslash (escapes spell a name without
 * its text); with @p deny_file_functions (no fs policy: DuckDB external access
 * off) any call of a file-reading table function (`read_*`, `parquet_*`,
 * `glob`, `sniff_csv`), and otherwise one whose first argument is not a
 * '...' / "..." path literal or a [list] of them, or is a glob whose
 * directory part resolves to @p protect_path's directory or an ancestor;
 * and a COPY whose TO / FROM target is not a literal or bare name (a
 * (constant expression)) or is such a glob. @p protect_path is the canonical
 * database path (`<cwd>/.tmp` for an in-memory database); NULL skips the
 * name and glob checks. 1 = refused.
 */
int hl_cap_db_sql_reaches_db_files(const char *sql, int deny_file_functions,
                                   const char *protect_path);

/**
 * @brief Install Hull's SQL guard on a connection: the authorizer that refuses
 *        ATTACH of a file / VACUUM INTO and the file-moving pragmas,
 *        SQLITE_DBCONFIG_DEFENSIVE, the progress handler that charges the
 *        calling run's budget (cap/db_budget.h) and SQLITE_LIMIT_LENGTH.
 *
 * Part of @ref hl_cap_db_init; on its own for a connection that must not get
 * the init's pragmas (a read-only agent connection).
 *
 * @return `0`, or `-1` when the guard could not be installed (close the
 *         connection).
 */
int hl_cap_db_guard(sqlite3 *db);

/**
 * @brief sqlite3_prepare_v2 for app SQL on a guarded connection: also refuses
 *        (SQLITE_AUTH, "not authorized") a statement that deletes
 *        sqlite_stat* rows when it is not a DROP / ANALYZE, a decision the
 *        authorizer can only make once the whole statement has been seen
 *        (`ANALYZE t` deletes t's rows before its own callback; audit 29).
 *
 * Every app statement is prepared through it (the statement cache and
 * @ref hl_cap_db_exec_script); it also clears the authorizer's per-prepare
 * state before and after, so it never carries from one statement to another.
 *
 * @return an SQLite result code, as sqlite3_prepare_v2.
 */
int hl_cap_db_prepare(sqlite3 *db, const char *sql, int len,
                      sqlite3_stmt **out, const char **tail);

/**
 * @brief Run a `;`-separated script, no parameters, each statement prepared
 *        through @ref hl_cap_db_prepare (sqlite3_exec prepares with no such
 *        check).
 *
 * @return `0`, or `-1` with the error readable through sqlite3_errmsg.
 */
int hl_cap_db_exec_script(sqlite3 *db, const char *sql);

/**
 * @brief Process-wide SQLite setup, before the first connection opens: the
 *        SQLITE_CONFIG_MALLOC wrapper that charges every allocation to the
 *        calling thread's bound budget (cap/db_budget.h) and fails it once
 *        that budget is exhausted, and the process heap limits (audit 10 H3).
 *
 * Idempotent and thread-safe; every Hull path that opens a SQLite connection
 * calls it first (sqlite3_config only works before sqlite3_initialize).
 */
void hl_cap_db_sqlite_setup(void);

/** The default hard heap limit SQLite as a whole may use
 *  (sqlite3_hard_heap_limit64): past it every SQLite allocation in the process
 *  fails (SQLITE_NOMEM) instead of the process running out of memory. Sorts,
 *  temp b-trees and VACUUM spill to temp files (temp_store=FILE), so what a
 *  connection holds is bounded by its page cache (cache_size, 16 MiB) plus a
 *  sorter run of the same size - the limit is headroom for many connections,
 *  not the ceiling on how big a sort or index build may be. The operator sets
 *  it with HULL_SQLITE_HEAP_LIMIT (a size: 512M, 4G; 0 = no hard limit). The
 *  soft limit (at most a quarter of the hard one) makes SQLite shed page cache
 *  before it gets there. */
#define HL_DB_SQLITE_HARD_HEAP_LIMIT ((long long)1 << 30)   /* 1 GiB */
#define HL_DB_SQLITE_SOFT_HEAP_LIMIT ((long long)256 << 20) /* 256 MiB */
/** The largest busy_timeout app SQL may set (PRAGMA busy_timeout): the
 *  calling thread sleeps that long on a locked database, uncharged, and on the
 *  event loop that stalls every request. Hull's own setting. */
#define HL_DB_MAX_BUSY_TIMEOUT_MS 5000
/** The smallest hard limit HULL_SQLITE_HEAP_LIMIT may set: a few connections'
 *  page caches. A smaller non-zero value is raised to it. */
#define HL_DB_SQLITE_MIN_HEAP_LIMIT  ((long long)64 << 20)  /* 64 MiB */

/** HULL_SQLITE_HEAP_LIMIT's value (NULL / "" = the default) as a hard limit in
 *  bytes: a size with an optional K / M / G suffix and an optional trailing B
 *  (512M, 512MB), 0 for no limit, raised (with a WARN) to
 *  HL_DB_SQLITE_MIN_HEAP_LIMIT; an unreadable or out-of-range value warns and
 *  gives the default. */
long long hl_cap_db_heap_limit_from_env(const char *value);

/** Set SQLite's process-wide hard heap limit (0 = none; negative = leave it)
 *  and the soft limit that goes with it (256 MiB, at most a quarter of the
 *  hard limit). hl_cap_db_sqlite_setup applies HULL_SQLITE_HEAP_LIMIT once. */
void hl_cap_db_set_heap_limit(long long hard);

/** 1 when Hull's connections keep temp data in files (temp_store=FILE): the
 *  setup found a private temp dir (hl_hull_sqlite_temp_dir) and pointed
 *  sqlite3_temp_directory at it. 0 = temp_store=MEMORY (Windows, or no safe
 *  temp dir - which logs a WARN), where a big sort is bounded by the hard
 *  heap limit. Temp FILES are not size-capped: a big sort or VACUUM uses disk
 *  in that directory, bounded only by the filesystem (and each run's
 *  instruction budget, which the work to write it is charged to). */
int hl_cap_db_temp_on_disk(void);

/**
 * @brief While @p on, the connection's authorizer also refuses transaction
 *        control (BEGIN / COMMIT / ROLLBACK / SAVEPOINT / RELEASE) at prepare
 *        time - for the read-only agent queries, where a text check alone can
 *        be misled by comments (audit 10).
 */
void hl_cap_db_refuse_txn_control(sqlite3 *db, int on);

#endif /* HL_CAP_DB_H */
