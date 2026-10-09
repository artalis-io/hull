/*
 * hull_cap_db.c - Shared database capability
 *
 * All SQLite access goes through these functions. Both Lua and JS
 * bindings call hl_cap_db_* - neither runtime touches SQLite directly.
 * Parameterized binding is the ONLY path; no string concatenation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db.h"
#include "hull/cap/audit.h"
#include "hull/cap/db_budget.h"
#include "hull/utils/alloc.h"
#include "hull/utils/parse_size.h"
#include "hull/shared/cache_dir.h"   /* hl_hull_sqlite_temp_dir */
#include <sqlite3.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Prepared statement cache ──────────────────────────────────────── */

void hl_stmt_cache_init(HlStmtCache *cache, sqlite3 *db, HlAllocator *alloc)
{
    memset(cache, 0, sizeof(*cache));
    cache->db = db;
    cache->alloc = alloc;
}

void hl_stmt_cache_destroy(HlStmtCache *cache)
{
    for (int i = 0; i < cache->count; i++) {
        sqlite3_finalize(cache->entries[i].stmt);
        hl_alloc_free_const(cache->alloc, cache->entries[i].sql,
                      cache->entries[i].sql_len + 1);
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

    /* Miss - prepare new statement */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(cache->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return NULL;

    /* Evict oldest (LRU) if cache is full. NOTE: this finalize is why row
     * callbacks must not re-enter the cache mid-iteration - see the INVARIANT
     * note at the cb() call site in the query loop below. */
    if (cache->count >= HL_STMT_CACHE_SIZE) {
        sqlite3_finalize(cache->entries[0].stmt);
        hl_alloc_free_const(cache->alloc, cache->entries[0].sql,
                      cache->entries[0].sql_len + 1);
        for (int j = 0; j < cache->count - 1; j++)
            cache->entries[j] = cache->entries[j + 1];
        cache->count--;
    }

    /* Copy SQL string for cache ownership */
    size_t sql_len = strlen(sql);
    char *sql_copy = hl_alloc_malloc(cache->alloc, sql_len + 1);
    if (!sql_copy) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    memcpy(sql_copy, sql, sql_len + 1);

    cache->entries[cache->count].sql     = sql_copy;
    cache->entries[cache->count].sql_len = sql_len;
    cache->entries[cache->count].stmt    = stmt;
    cache->count++;

    return stmt;
}

/* ── Database initialization ───────────────────────────────────────── */

/* SQL is a second route to the filesystem, and it must not reach files the fs
 * capability never granted. ATTACH opens another database file by name.
 * VACUUM INTO writes a copy of the database to any path: SQLite carries it out
 * as an internal ATTACH of that path, which reaches this callback too. So both
 * are refused, except the two attaches that name no file: '' (the temporary
 * database a plain VACUUM attaches) and ":memory:". An ATTACH whose name is an
 * expression arrives with a NULL name and is refused.
 *
 * writable_schema lets SQL rewrite sqlite_schema directly (DEFENSIVE below
 * disables it as well), and the *_store_directory pragmas move where SQLite
 * writes its files. None of these has a use in application SQL.
 *
 * Nor do the pragmas that SET how much memory or how many threads SQLite uses
 * (audit 10 H3): hard_heap_limit / soft_heap_limit are process-wide (one app
 * statement changed them for every connection), cache_size / cache_spill let
 * a connection hold up to its whole database in memory across runs,
 * temp_store moves temp b-trees out of the charged heap into files, and
 * threads starts helper threads whose allocations no run's budget sees.
 * default_cache_size is cache_size by another name (audit 11). Reading them
 * stays legal (a2 is NULL then).
 *
 * Two setters defeat what Hull's other guarantees rest on (audit 11):
 * locking_mode=EXCLUSIVE keeps the connection's lock once taken, so every
 * other connection to the file - the db.async / worker.dispatch pool's - waits
 * out busy_timeout and fails; and journal_mode=OFF / MEMORY / DELETE / ...
 * drops the rollback journal (ROLLBACK, which the stale-transaction guard and
 * db.batch rely on, stops working; a crash corrupts) or the WAL that
 * synchronous=NORMAL is safe under. So locking_mode may only be set to NORMAL
 * and journal_mode only to WAL (SQLite matches a journal_mode value as a
 * PREFIX of the mode name, so "w" / "wa" mean WAL and "" means DELETE).
 *
 * @p ud non-NULL (hl_cap_db_refuse_txn_control): transaction control is
 * refused too - SQLite reports it here at prepare time, whatever comments or
 * spelling the text uses - and so is every pragma given an argument, except
 * the introspection ones whose argument only names what to describe: a flag
 * pragma (foreign_keys, query_only, defer_foreign_keys, recursive_triggers,
 * busy_timeout, synchronous, ...) takes effect when it is PREPARED and still
 * passes sqlite3_stmt_readonly, so a "read-only" agent query changed the warm
 * app connection (audit 11). */
static int db_authorizer(void *ud, int action, const char *a1, const char *a2,
                         const char *a3, const char *a4)
{
    (void)a3; (void)a4;
    static const char *const set_refused[] = {
        "hard_heap_limit", "soft_heap_limit", "cache_size", "cache_spill",
        "default_cache_size", "temp_store", "threads",
    };
    static const char *const describe_ok[] = {
        "table_info", "table_xinfo", "table_list", "index_info", "index_xinfo",
        "index_list", "foreign_key_list", "foreign_key_check",
        "integrity_check", "quick_check",
    };
    switch (action) {
    case SQLITE_ATTACH:
        if (a1 && (a1[0] == '\0' || strcmp(a1, ":memory:") == 0))
            return SQLITE_OK;
        return SQLITE_DENY;
    case SQLITE_PRAGMA:
        if (!a1) return SQLITE_OK;
        if (strcasecmp(a1, "writable_schema") == 0 ||
            strcasecmp(a1, "temp_store_directory") == 0 ||
            strcasecmp(a1, "data_store_directory") == 0)
            return SQLITE_DENY;
        if (!a2) return SQLITE_OK;
        for (size_t i = 0; i < sizeof set_refused / sizeof set_refused[0]; i++)
            if (strcasecmp(a1, set_refused[i]) == 0)
                return SQLITE_DENY;
        if (strcasecmp(a1, "locking_mode") == 0 && strcasecmp(a2, "normal") != 0)
            return SQLITE_DENY;
        if (strcasecmp(a1, "journal_mode") == 0) {
            size_t n = strlen(a2);
            if (n == 0 || n > 3 || strncasecmp(a2, "wal", n) != 0)
                return SQLITE_DENY;
        }
        if (ud) {
            for (size_t i = 0; i < sizeof describe_ok / sizeof describe_ok[0]; i++)
                if (strcasecmp(a1, describe_ok[i]) == 0)
                    return SQLITE_OK;
            return SQLITE_DENY;
        }
        return SQLITE_OK;
    case SQLITE_TRANSACTION:
    case SQLITE_SAVEPOINT:
        return ud ? SQLITE_DENY : SQLITE_OK;
    default:
        return SQLITE_OK;
    }
}

/* A non-NULL authorizer argument: refuse transaction control. */
static char db_refuse_txn_tag;

void hl_cap_db_refuse_txn_control(sqlite3 *db, int on)
{
    if (db)
        (void)sqlite3_set_authorizer(db, db_authorizer,
                                     on ? &db_refuse_txn_tag : NULL);
}

/* ── Process-wide setup: every SQLite allocation is charged (audit 10 H3) ──
 *
 * The progress handler counts opcodes, and one opcode can do unbounded work:
 * randomblob / zeroblob / replace / printf build their value in one step, a
 * sorter sorts in one, and under temp_store=MEMORY a temp b-tree grows in the
 * heap. None of it touched the VM heap or the budget. SQLite's allocator is
 * wrapped instead: each allocation (and each realloc's growth) is charged to
 * the calling thread's bound budget (hl_db_budget_charge - none bound, none
 * charged), and once that budget is exhausted the allocation fails, so the
 * statement stops with SQLITE_NOMEM there and then; the runtimes see their
 * budget tripped and raise the limit, not the SQL error. Process-wide: a
 * thread with no binding (the tool VM, migrations, a pool thread between ops)
 * is never charged or refused, and the rollbacks Hull runs on a run's behalf
 * run unbound (hl_cap_db_rollback). */
static sqlite3_mem_methods g_sqlite_mem;   /* SQLite's own allocator */

static int db_mem_charge(int64_t bytes)
{
    if (bytes <= 0) return 0;
    return hl_db_budget_charge((bytes + HL_DB_ALLOC_UNIT_BYTES - 1) /
                               HL_DB_ALLOC_UNIT_BYTES);
}

static void *db_mem_malloc(int n)
{
    if (db_mem_charge(n)) return NULL;
    return g_sqlite_mem.xMalloc(n);
}

static void *db_mem_realloc(void *p, int n)
{
    int64_t old = p ? g_sqlite_mem.xSize(p) : 0;
    if (n > old && db_mem_charge((int64_t)n - old)) return NULL;
    return g_sqlite_mem.xRealloc(p, n);
}

static void db_mem_free(void *p)      { g_sqlite_mem.xFree(p); }
static int  db_mem_size(void *p)      { return g_sqlite_mem.xSize(p); }
static int  db_mem_roundup(int n)     { return g_sqlite_mem.xRoundup(n); }
static int  db_mem_init(void *ud)     { (void)ud; return g_sqlite_mem.xInit(g_sqlite_mem.pAppData); }
static void db_mem_shutdown(void *ud) { (void)ud; g_sqlite_mem.xShutdown(g_sqlite_mem.pAppData); }

static pthread_once_t g_sqlite_setup_once = PTHREAD_ONCE_INIT;

static void db_sqlite_setup_once(void)
{
    /* sqlite3_config works only before sqlite3_initialize: every Hull open
     * path calls the setup first, so nothing should have initialized SQLite
     * yet. If something did, say so - allocations then go uncharged (the
     * progress handler and the heap limits still apply). */
    int ok = sqlite3_config(SQLITE_CONFIG_GETMALLOC, &g_sqlite_mem) == SQLITE_OK &&
             g_sqlite_mem.xMalloc && g_sqlite_mem.xRealloc && g_sqlite_mem.xSize;
    if (ok) {
        sqlite3_mem_methods wrap = {
            db_mem_malloc, db_mem_free, db_mem_realloc, db_mem_size,
            db_mem_roundup, db_mem_init, db_mem_shutdown, NULL,
        };
        ok = sqlite3_config(SQLITE_CONFIG_MALLOC, &wrap) == SQLITE_OK;
    }
    if (!ok)
        fprintf(stderr, "hull: WARN sqlite was initialized before Hull could "
                        "install its allocator; SQL allocations are not "
                        "charged to the instruction budget\n");
    hl_cap_db_set_heap_limit(hl_cap_db_heap_limit_from_env(
        getenv("HULL_SQLITE_HEAP_LIMIT")));
    /* temp_store=FILE (hl_cap_db_init) writes temp files here - the directory
     * the kernel sandbox grants (hl_hull_sqlite_temp_dir). Set once, before
     * any connection opens; it must be sqlite3_malloc'd memory. Without one,
     * the connections keep temp_store=MEMORY (hl_cap_db_temp_on_disk). */
    const char *tmp = hl_hull_sqlite_temp_dir();
    if (tmp && !sqlite3_temp_directory)
        sqlite3_temp_directory = sqlite3_mprintf("%s", tmp);
}

int hl_cap_db_temp_on_disk(void)
{
    hl_cap_db_sqlite_setup();
    return sqlite3_temp_directory != NULL;
}

long long hl_cap_db_heap_limit_from_env(const char *v)
{
    if (!v || !v[0]) return HL_DB_SQLITE_HARD_HEAP_LIMIT;
    long n = hl_parse_size(v);
    if (n < 0) {
        fprintf(stderr, "hull: WARN ignoring HULL_SQLITE_HEAP_LIMIT=%s (want a "
                        "size such as 512M or 2G, or 0 for no limit)\n", v);
        return HL_DB_SQLITE_HARD_HEAP_LIMIT;
    }
    if (n > 0 && n < HL_DB_SQLITE_MIN_HEAP_LIMIT) n = HL_DB_SQLITE_MIN_HEAP_LIMIT;
    return (long long)n;
}

void hl_cap_db_set_heap_limit(long long hard)
{
    if (hard < 0) return;
    long long soft = HL_DB_SQLITE_SOFT_HEAP_LIMIT;
    if (hard > 0 && soft > hard / 4) soft = hard / 4;
    (void)sqlite3_hard_heap_limit64(hard);
    (void)sqlite3_soft_heap_limit64(soft);
}

void hl_cap_db_sqlite_setup(void)
{
    (void)pthread_once(&g_sqlite_setup_once, db_sqlite_setup_once);
}

int hl_cap_db_init(sqlite3 *db)
{
    if (!db)
        return -1;

    /*
     * Performance PRAGMAs - applied once at connection open.
     *
     * journal_mode=WAL    - Write-Ahead Logging for concurrent readers/writer.
     * synchronous=NORMAL  - Sync WAL on checkpoint only (not every commit).
     *                       Safe: WAL protects against corruption; only risk is
     *                       losing the last transaction on OS crash (not app crash).
     * foreign_keys=ON     - Referential integrity.
     * busy_timeout=5000   - Wait up to 5 seconds on lock contention.
     * cache_size=-16384   - 16 MB page cache (default is 2 MB).
     * temp_store=FILE     - Temp tables / indexes, sorter runs and VACUUM's
     *                       copy spill to files (hl_hull_sqlite_temp_dir) once
     *                       they outgrow the page cache. Under MEMORY the
     *                       sorter never spilled, so every big ORDER BY /
     *                       CREATE INDEX / GROUP BY / VACUUM had to fit in the
     *                       process-wide hard heap limit, which one request
     *                       could fill for every connection (audit 11).
     * mmap_size=268435456 - Memory-map up to 256 MB of the DB file for reads.
     * wal_autocheckpoint=1000 - Checkpoint every 1000 pages (~4 MB).
     *                          Default is 1000; explicit for clarity.
     */
    /* REQUIRED: a failure here is a genuine error and aborts the open.
     *
     * busy_timeout goes first because the pragmas after it can contend, and
     * with SQLite's default timeout of 0 a locked file fails instantly rather
     * than waiting. foreign_keys is semantic - an app that believes its
     * referential integrity is enforced must not run if it is not. */
    static const char *required[] = {
        "PRAGMA busy_timeout=5000",
        "PRAGMA foreign_keys=ON",
        NULL,
    };
    for (const char **p = required; *p; p++) {
        if (sqlite3_exec(db, *p, NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "hull: sqlite %s failed: %s\n", *p, sqlite3_errmsg(db));
            return -1;
        }
    }

    /* WAL is a PERFORMANCE choice, so losing it must not stop the app.
     *
     * Converting a database to WAL takes an EXCLUSIVE lock, and several
     * processes opening the same fresh database at once race for it. The loser
     * gets SQLITE_PROTOCOL - not a busy condition, so busy_timeout does not
     * cover it and retrying does not clear it. Measured on Windows: of 8 hull
     * processes started together against one data.db, 1 served. They were not
     * failing to open the database; they opened it fine and then aborted
     * because a tuning pragma did not apply.
     *
     * SQLite is entirely correct in rollback mode - just slower under
     * concurrent readers - so a lost race now costs performance instead of
     * startup. Note the loser is not necessarily IN rollback mode:
     * journal mode is a property of the FILE, so once the winner converts
     * it, a connection whose own pragma failed is still using WAL. It
     * failed to SET the mode, not to get it. The mode is a property of the FILE and persists, so whichever
     * process wins converts it for everyone, and subsequent opens simply find
     * it already WAL. Hull's default DSN is the relative path data.db
     * (serve.c), which is why processes sharing a working directory share a
     * database, and hit this window at all. */
    int wal = (sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL) == SQLITE_OK);
    if (!wal) {
        fprintf(stderr,
                "hull: sqlite could not set WAL (%s); continuing with the "
                "database's current journal mode\n",
                sqlite3_errmsg(db));
    }

    /* synchronous=NORMAL is only SAFE under WAL: the docs' guarantee is that
     * WAL protects against corruption and NORMAL merely risks losing the last
     * transaction on an OS crash. Without WAL that reasoning does not hold, so
     * leave the default (FULL) rather than quietly weakening durability for a
     * connection that lost the race. Same for wal_autocheckpoint, which has no
     * meaning outside WAL. */
    static const char *wal_only[] = {
        "PRAGMA synchronous=NORMAL",
        "PRAGMA wal_autocheckpoint=1000",
        NULL,
    };
    if (wal) {
        for (const char **p = wal_only; *p; p++)
            (void)sqlite3_exec(db, *p, NULL, NULL, NULL);
    }

    /* Pure tuning: best-effort everywhere. None of these change behaviour. */
    static const char *tuning[] = {
        "PRAGMA cache_size=-16384",
        "PRAGMA mmap_size=268435456",
        NULL,
    };
    for (const char **p = tuning; *p; p++)
        (void)sqlite3_exec(db, *p, NULL, NULL, NULL);
    /* Temp files only where Hull has the directory for them - the one it set
     * as sqlite3_temp_directory and the kernel sandbox grants. Without it
     * (Windows: SQLite's unix VFS under Cosmopolitan finds no temp path at
     * all, SQLITE_IOERR_GETTEMPPATH) temp stays in memory, as before. */
    (void)sqlite3_exec(db, hl_cap_db_temp_on_disk() ? "PRAGMA temp_store=FILE"
                                                     : "PRAGMA temp_store=MEMORY",
                       NULL, NULL, NULL);

    /* REQUIRED, and last, so the pragmas above are not subject to it: a
     * connection that cannot refuse ATTACH / VACUUM INTO would let SQL write
     * outside the fs grants, so a failure here aborts the open. */
    return hl_cap_db_guard(db);
}

/* Every HL_DB_PROGRESS_OPS SQLite instructions: charge the calling run's
 * budget, and interrupt the statement once it is exhausted (audit 9 H4). */
static int db_progress(void *ud)
{
    (void)ud;
    return hl_db_budget_charge(HL_DB_PROGRESS_OPS);
}

int hl_cap_db_guard(sqlite3 *db)
{
    if (!db)
        return -1;
    if (sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, (int *)NULL) != SQLITE_OK ||
        sqlite3_set_authorizer(db, db_authorizer, NULL) != SQLITE_OK) {
        fprintf(stderr, "hull: sqlite could not install its SQL guard: %s\n",
                sqlite3_errmsg(db));
        return -1;
    }
    /* One statement used to run unmetered: a recursive CTE held the event
     * loop for good. With no budget bound (migrations, tooling) the handler
     * never interrupts. */
    sqlite3_progress_handler(db, HL_DB_PROGRESS_OPS, db_progress, NULL);
    /* randomblob(1e9) / zeroblob(1e9) / a long replace() built a value of up
     * to SQLite's 1 GB default outside the VM heap limit; nothing larger
     * than a VM could hold is any use to it. */
    size_t max = hl_db_max_value_bytes();
    if (max > 0 && max < (size_t)INT_MAX)
        sqlite3_limit(db, SQLITE_LIMIT_LENGTH, (int)max);
    return 0;
}

void hl_cap_db_shutdown(sqlite3 *db)
{
    if (!db)
        return;

    /* Not a run's work (the thread may still hold a tripped run's binding). */
    HlDbBudgetBinding budget = hl_db_budget_swap(NULL, NULL);

    /* Run PRAGMA optimize - lets SQLite update internal statistics */
    sqlite3_exec(db, "PRAGMA optimize", NULL, NULL, NULL);

    /* Final WAL checkpoint - merge WAL back into main DB file */
    sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                              NULL, NULL);
    hl_db_budget_restore(budget);
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
            if (params[i].len > (size_t)INT_MAX) return -1;
            rc = sqlite3_bind_text(stmt, idx, params[i].s,
                                   (int)params[i].len, SQLITE_TRANSIENT);
            break;
        case HL_TYPE_BLOB:
            if (params[i].len > (size_t)INT_MAX) return -1;
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

static int db_query_inner(HlStmtCache *cache, const char *sql,
                          const HlValue *params, int nparams,
                          HlRowCallback cb, void *ctx,
                          HlAllocator *alloc)
{
    if (!cache || !sql || !cb)
        return HL_DB_ERR_PREPARE;

    sqlite3_stmt *stmt = cache_get(cache, sql);
    if (!stmt)
        return HL_DB_ERR_PREPARE;

    if (nparams > 0 && params) {
        if (bind_params(stmt, params, nparams) != 0) {
            sqlite3_reset(stmt);
            return HL_DB_ERR_BIND;
        }
    }

    int ncols = sqlite3_column_count(stmt);
    if (ncols <= 0) {
        sqlite3_reset(stmt);
        return HL_DB_ERR_PREPARE;
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
            return HL_DB_ERR_EXEC;
        }
        size_t cols_size = (size_t)ncols * sizeof(HlColumn);
        size_t vals_size = (size_t)ncols * sizeof(HlValue);
        cols = hl_alloc_malloc(alloc, cols_size);
        vals = hl_alloc_malloc(alloc, vals_size);
        if (!cols || !vals) {
            hl_alloc_free(alloc, cols, cols_size);
            hl_alloc_free(alloc, vals, vals_size);
            sqlite3_reset(stmt);
            return HL_DB_ERR_EXEC;
        }
    }

    int result = HL_DB_OK;
    int rc;
    int names_populated = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        /*
         * Populate column names AFTER the first sqlite3_step().
         *
         * sqlite3_column_name() pointers are invalidated if the statement
         * is auto-recompiled on the first step - which happens when bound
         * parameters affect the query plan (e.g. LIMIT ?).  Fetching names
         * after step guarantees the pointers remain valid until reset.
         */
        if (!names_populated) {
            for (int i = 0; i < ncols; i++)
                cols[i].name = sqlite3_column_name(stmt, i);
            names_populated = 1;
        }
        for (int i = 0; i < ncols; i++) {
            column_to_value(stmt, i, &vals[i]);
            cols[i].value = vals[i];
        }
        /* INVARIANT: the row callback MUST NOT re-enter the statement cache
         * (db.query / db.exec with a new SQL string). `stmt` is mid-iteration
         * and is owned by the LRU cache; a re-entrant cache_get that evicts it
         * (sqlite3_finalize) would dangle `stmt` for the next sqlite3_step ->
         * use-after-free. All current callers are pure C result-marshallers
         * that build the result set and only hand it to script AFTER this
         * loop ends, so this holds. Preserve it if a streaming/per-row script
         * callback API is ever added (pin the in-use stmt against eviction). */
        if (cb(ctx, cols, ncols) != 0)
            break; /* caller requested stop */
    }

    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        result = (rc == SQLITE_BUSY) ? HL_DB_ERR_BUSY : HL_DB_ERR_EXEC;

    if (cols != stack_cols) {
        hl_alloc_free(alloc, cols, (size_t)ncols * sizeof(HlColumn));
        hl_alloc_free(alloc, vals, (size_t)ncols * sizeof(HlValue));
    }

    sqlite3_reset(stmt);

    {
        ShJsonWriter w = hl_audit_begin("db.query");
        size_t sql_len = strlen(sql);
        sh_json_write_key(&w, "sql");
        sh_json_write_string_n(&w, sql, sql_len < 512 ? sql_len : 512);
        sh_json_write_kv_int(&w, "nparams", nparams);
        sh_json_write_kv_int(&w, "result", result);
        hl_audit_end(&w);
    }
    return result;
}

static int db_exec_inner(HlStmtCache *cache, const char *sql,
                         const HlValue *params, int nparams)
{
    if (!cache || !sql)
        return HL_DB_ERR_PREPARE;

    sqlite3_stmt *stmt = cache_get(cache, sql);
    if (!stmt)
        return HL_DB_ERR_PREPARE;

    if (nparams > 0 && params) {
        if (bind_params(stmt, params, nparams) != 0) {
            sqlite3_reset(stmt);
            return HL_DB_ERR_BIND;
        }
    }

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    int result;
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        result = (rc == SQLITE_BUSY) ? HL_DB_ERR_BUSY : HL_DB_ERR_EXEC;
    else
        result = sqlite3_changes(cache->db);

    {
        ShJsonWriter w = hl_audit_begin("db.exec");
        size_t sql_len = strlen(sql);
        sh_json_write_key(&w, "sql");
        sh_json_write_string_n(&w, sql, sql_len < 512 ? sql_len : 512);
        sh_json_write_kv_int(&w, "nparams", nparams);
        sh_json_write_kv_int(&w, "result", result);
        hl_audit_end(&w);
    }
    return result;
}

/* The entry points: refuse a call made from inside one of this cache's own
 * statements (see HlStmtCache.stepping), and mark the cache for the call's
 * whole run otherwise. */
int hl_cap_db_query(HlStmtCache *cache, const char *sql,
                    const HlValue *params, int nparams,
                    HlRowCallback cb, void *ctx,
                    HlAllocator *alloc)
{
    if (cache && cache->stepping)
        return HL_DB_ERR_REENTERED;
    if (cache) cache->stepping = 1;
    int rc = db_query_inner(cache, sql, params, nparams, cb, ctx, alloc);
    if (cache) cache->stepping = 0;
    return rc;
}

int hl_cap_db_exec(HlStmtCache *cache, const char *sql,
                   const HlValue *params, int nparams)
{
    if (cache && cache->stepping)
        return HL_DB_ERR_REENTERED;
    if (cache) cache->stepping = 1;
    int rc = db_exec_inner(cache, sql, params, nparams);
    if (cache) cache->stepping = 0;
    return rc;
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
        return HL_DB_ERR_EXEC;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc == SQLITE_OK)    return HL_DB_OK;
    if (rc == SQLITE_BUSY)  return HL_DB_ERR_BUSY;
    return HL_DB_ERR_EXEC;
}

int hl_cap_db_commit(sqlite3 *db)
{
    if (!db)
        return HL_DB_ERR_EXEC;
    int rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc == SQLITE_OK)    return HL_DB_OK;
    if (rc == SQLITE_BUSY)  return HL_DB_ERR_BUSY;
    return HL_DB_ERR_EXEC;
}

/* A rollback is cleanup a run's budget must not stop: the batch whose fn hit
 * the limit, the stale-transaction guard after a tripped entry, the end of a
 * db.async op. It runs unbound, so neither the allocation wrapper nor the
 * progress handler refuses it (audit 10 H3). */
int hl_cap_db_rollback(sqlite3 *db)
{
    if (!db)
        return HL_DB_ERR_EXEC;
    HlDbBudgetBinding budget = hl_db_budget_swap(NULL, NULL);
    int rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    hl_db_budget_restore(budget);
    return rc == SQLITE_OK ? HL_DB_OK : HL_DB_ERR_EXEC;
}

/* The ROLLBACK is checked, not assumed (audit 11): it can fail - out of
 * memory under the hard heap limit, say - and a connection left inside the
 * transaction makes every later entry run in it. A failed one is retried once
 * after resetting the connection's statements and shedding its cache; the
 * caller (sqlite_guard_stale_txn) replaces the connection when even that
 * leaves it in the transaction. */
int hl_cap_db_guard_stale_txn(sqlite3 *db)
{
    if (!db || sqlite3_get_autocommit(db)) return 0;
    fprintf(stderr, "[hull:c] rolling back stale transaction from previous request\n");
    (void)hl_cap_db_rollback(db);
    if (sqlite3_get_autocommit(db)) return 0;
    for (sqlite3_stmt *st = sqlite3_next_stmt(db, NULL); st;
         st = sqlite3_next_stmt(db, st))
        (void)sqlite3_reset(st);
    (void)sqlite3_db_release_memory(db);
    (void)hl_cap_db_rollback(db);
    if (sqlite3_get_autocommit(db)) return 0;
    fprintf(stderr, "[hull:c] the stale transaction could not be rolled back: %s\n",
            sqlite3_errmsg(db));
    return -1;
}

#endif /* HL_ENABLE_DB */
