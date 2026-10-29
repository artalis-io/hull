/*
 * test_hull_cap_db.c - Tests for shared database capability
 *
 * Uses utest.h (from Keel vendor) for the test framework.
 * Tests run against an in-memory SQLite database.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"
#include "hull/cap/db_budget.h"
#ifdef HL_ENABLE_WASM
#include "hull/cap/db_udf.h"
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm.h"
#include "hull/vfs.h"
#include "hull/entry.h"
#endif
#endif
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>   /* getpid */
#include <limits.h>
#include <time.h>     /* clock */
#include <sys/stat.h>
#include "hull/shared/cache_dir.h"   /* hl_hull_sqlite_temp_dir_make */
#include "hull/shared/host.h"        /* hl_host_is_windows */
#include "../test_tmpdir.h"

/* ── Test fixtures ──────────────────────────────────────────────────── */

static sqlite3 *test_db = NULL;
static HlStmtCache test_cache;

static void setup_db(void)
{
    hl_cap_db_sqlite_setup();   /* as every Hull open path does */
    sqlite3_open(":memory:", &test_db);
    hl_cap_db_init(test_db);
    hl_stmt_cache_init(&test_cache, test_db, NULL);
    sqlite3_exec(test_db,
        "CREATE TABLE users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  age INTEGER,"
        "  score REAL"
        ")",
        NULL, NULL, NULL);
}

static void teardown_db(void)
{
    if (test_db) {
        hl_stmt_cache_destroy(&test_cache);
        hl_cap_db_shutdown(test_db);
        sqlite3_close(test_db);
        test_db = NULL;
    }
}

/* ── Row callback helpers ───────────────────────────────────────────── */

typedef struct {
    int    count;
    char   names[10][64];
    int64_t ages[10];
    double scores[10];
} QueryResult;

static int collect_rows(void *ctx, HlColumn *cols, int ncols)
{
    QueryResult *r = (QueryResult *)ctx;
    if (r->count >= 10)
        return 1; /* stop */

    for (int i = 0; i < ncols; i++) {
        if (strcmp(cols[i].name, "name") == 0 &&
            cols[i].value.type == HL_TYPE_TEXT) {
            size_t len = cols[i].value.len < 63 ? cols[i].value.len : 63;
            memcpy(r->names[r->count], cols[i].value.s, len);
            r->names[r->count][len] = '\0';
        }
        if (strcmp(cols[i].name, "age") == 0 &&
            cols[i].value.type == HL_TYPE_INT) {
            r->ages[r->count] = cols[i].value.i;
        }
        if (strcmp(cols[i].name, "score") == 0 &&
            cols[i].value.type == HL_TYPE_DOUBLE) {
            r->scores[r->count] = cols[i].value.d;
        }
    }
    r->count++;
    return 0;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

UTEST(hl_cap_db, exec_insert)
{
    setup_db();

    HlValue params[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };

    int rc = hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)",
        params, 3);

    ASSERT_GE(rc, 0);

    teardown_db();
}

UTEST(hl_cap_db, exec_returns_changes)
{
    setup_db();

    HlValue p1[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);

    HlValue p2[] = {
        { .type = HL_TYPE_TEXT, .s = "Bob", .len = 3 },
        { .type = HL_TYPE_INT, .i = 25 },
        { .type = HL_TYPE_DOUBLE, .d = 87.0 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p2, 3);

    /* Update all ages to 99 */
    HlValue p3[] = {
        { .type = HL_TYPE_INT, .i = 99 },
    };
    int changes = hl_cap_db_exec(&test_cache,
        "UPDATE users SET age = ?", p3, 1);

    ASSERT_EQ(changes, 2);

    teardown_db();
}

UTEST(hl_cap_db, query_basic)
{
    setup_db();

    HlValue p1[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);

    QueryResult result = { .count = 0 };
    int rc = hl_cap_db_query(&test_cache,
        "SELECT name, age, score FROM users", NULL, 0,
        collect_rows, &result, NULL);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(result.count, 1);
    ASSERT_STREQ(result.names[0], "Alice");
    ASSERT_EQ(result.ages[0], 30);

    teardown_db();
}

UTEST(hl_cap_db, query_with_params)
{
    setup_db();

    /* Insert two rows */
    HlValue p1[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);

    HlValue p2[] = {
        { .type = HL_TYPE_TEXT, .s = "Bob", .len = 3 },
        { .type = HL_TYPE_INT, .i = 25 },
        { .type = HL_TYPE_DOUBLE, .d = 87.0 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p2, 3);

    /* Query with param filter */
    HlValue filter[] = {
        { .type = HL_TYPE_INT, .i = 28 },
    };
    QueryResult result = { .count = 0 };
    int rc = hl_cap_db_query(&test_cache,
        "SELECT name, age, score FROM users WHERE age > ?",
        filter, 1, collect_rows, &result, NULL);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(result.count, 1);
    ASSERT_STREQ(result.names[0], "Alice");

    teardown_db();
}

UTEST(hl_cap_db, query_null_param)
{
    setup_db();

    HlValue p1[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_NIL },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };
    int rc = hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);
    ASSERT_GE(rc, 0);

    QueryResult result = { .count = 0 };
    rc = hl_cap_db_query(&test_cache,
        "SELECT name, age FROM users", NULL, 0,
        collect_rows, &result, NULL);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(result.count, 1);
    /* age should be 0 since NIL was inserted */

    teardown_db();
}

UTEST(hl_cap_db, last_id)
{
    setup_db();

    HlValue p[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p, 3);

    int64_t id = hl_cap_db_last_id(test_db);
    ASSERT_EQ(id, 1);

    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p, 3);

    id = hl_cap_db_last_id(test_db);
    ASSERT_EQ(id, 2);

    teardown_db();
}

UTEST(hl_cap_db, null_db)
{
    int rc = hl_cap_db_query(NULL, "SELECT 1", NULL, 0, collect_rows, NULL, NULL);
    ASSERT_EQ(rc, HL_DB_ERR_PREPARE);

    rc = hl_cap_db_exec(NULL, "SELECT 1", NULL, 0);
    ASSERT_EQ(rc, HL_DB_ERR_PREPARE);
}

UTEST(hl_cap_db, null_sql)
{
    setup_db();

    int rc = hl_cap_db_query(&test_cache, NULL, NULL, 0, collect_rows, NULL, NULL);
    ASSERT_EQ(rc, HL_DB_ERR_PREPARE);

    rc = hl_cap_db_exec(&test_cache, NULL, NULL, 0);
    ASSERT_EQ(rc, HL_DB_ERR_PREPARE);

    teardown_db();
}

UTEST(hl_cap_db, invalid_sql)
{
    setup_db();

    QueryResult result = { .count = 0 };
    int rc = hl_cap_db_query(&test_cache,
        "SELECT * FROM nonexistent_table", NULL, 0,
        collect_rows, &result, NULL);
    ASSERT_EQ(rc, HL_DB_ERR_PREPARE);

    teardown_db();
}

UTEST(hl_cap_db, bool_param)
{
    setup_db();

    /* SQLite stores booleans as integers */
    HlValue p[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_BOOL, .b = 1 },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };
    int rc = hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p, 3);
    ASSERT_GE(rc, 0);

    QueryResult result = { .count = 0 };
    rc = hl_cap_db_query(&test_cache,
        "SELECT name, age FROM users", NULL, 0,
        collect_rows, &result, NULL);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(result.count, 1);
    ASSERT_EQ(result.ages[0], 1); /* bool true → 1 */

    teardown_db();
}

UTEST(hl_cap_db, transaction_commit)
{
    setup_db();

    ASSERT_EQ(hl_cap_db_begin(test_db), 0);

    HlValue p[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p, 3);
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p, 3);

    ASSERT_EQ(hl_cap_db_commit(test_db), 0);

    /* Both rows should be visible */
    QueryResult result = { .count = 0 };
    hl_cap_db_query(&test_cache,
        "SELECT name FROM users", NULL, 0,
        collect_rows, &result, NULL);
    ASSERT_EQ(result.count, 2);

    teardown_db();
}

UTEST(hl_cap_db, transaction_rollback)
{
    setup_db();

    ASSERT_EQ(hl_cap_db_begin(test_db), 0);

    HlValue p[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p, 3);

    ASSERT_EQ(hl_cap_db_rollback(test_db), 0);

    /* Row should NOT be visible */
    QueryResult result = { .count = 0 };
    hl_cap_db_query(&test_cache,
        "SELECT name FROM users", NULL, 0,
        collect_rows, &result, NULL);
    ASSERT_EQ(result.count, 0);

    teardown_db();
}

UTEST(hl_cap_db, stmt_cache_reuse)
{
    setup_db();

    /* Execute the same SQL twice - second should hit cache */
    HlValue p1[] = {
        { .type = HL_TYPE_TEXT, .s = "Alice", .len = 5 },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 95.5 },
    };
    int rc1 = hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);
    ASSERT_GE(rc1, 0);

    HlValue p2[] = {
        { .type = HL_TYPE_TEXT, .s = "Bob", .len = 3 },
        { .type = HL_TYPE_INT, .i = 25 },
        { .type = HL_TYPE_DOUBLE, .d = 87.0 },
    };
    int rc2 = hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p2, 3);
    ASSERT_GE(rc2, 0);

    /* Verify both rows inserted */
    QueryResult result = { .count = 0 };
    hl_cap_db_query(&test_cache,
        "SELECT name FROM users", NULL, 0,
        collect_rows, &result, NULL);
    ASSERT_EQ(result.count, 2);

    teardown_db();
}

/* ── Namespace check tests ──────────────────────────────────────────── */

/* ── Audit 9 H4: SQL charged to the calling run's budget ─────────────── */

static int count_cb(void *ctx, HlColumn *cols, int ncols)
{
    if (ncols > 0) *(int64_t *)ctx = cols[0].value.i;
    return 0;
}

UTEST(hl_cap_db, runaway_statement_is_interrupted_by_the_bound_budget)
{
    setup_db();
    HlDbOpBudget b = { .limit = 200000, .used = 0, .tripped = 0 };
    HlDbBudgetBinding prev = hl_db_budget_swap(hl_db_op_budget_charge, &b);
    int64_t n = 0;
    int rc = hl_cap_db_query(&test_cache,
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) AS n FROM c", NULL, 0, count_cb, &n, NULL);
    hl_db_budget_restore(prev);
    EXPECT_NE(0, rc);
    EXPECT_EQ(1, b.tripped);
    EXPECT_EQ(SQLITE_INTERRUPT, sqlite3_errcode(test_db));
    /* Sticky: the next statement of the run is refused too. */
    prev = hl_db_budget_swap(hl_db_op_budget_charge, &b);
    EXPECT_NE(0, hl_cap_db_query(&test_cache,
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c "
        "LIMIT 10000) SELECT count(*) FROM c", NULL, 0, count_cb, &n, NULL));
    hl_db_budget_restore(prev);
    teardown_db();
}

UTEST(hl_cap_db, no_bound_budget_never_interrupts)
{
    setup_db();
    HlDbBudgetBinding prev = hl_db_budget_swap(NULL, NULL);
    int64_t n = 0;
    EXPECT_EQ(0, hl_cap_db_query(&test_cache,
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c "
        "LIMIT 100000) SELECT count(*) FROM c", NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(100000, (int)n);
    hl_db_budget_restore(prev);
    teardown_db();
}

/* randomblob / zeroblob built values up to SQLite's 1 GB outside the VM heap
 * limit: a connection is capped at the largest heap a runtime reported. */
UTEST(hl_cap_db, value_length_is_capped_at_the_heap_limit)
{
    hl_db_note_heap_limit(1u << 20);
    setup_db();
    int64_t n = 0;
    EXPECT_NE(0, hl_cap_db_query(&test_cache,
        "SELECT length(zeroblob(2000000))", NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(SQLITE_TOOBIG, sqlite3_errcode(test_db));
    EXPECT_EQ(0, hl_cap_db_query(&test_cache,
        "SELECT length(randomblob(1000))", NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(1000, (int)n);
    teardown_db();
}

/* hl_cap_db_guard alone (the read-only agent connection, audit 9 M2) still
 * refuses ATTACH of a file and VACUUM INTO. */
UTEST(hl_cap_db, guard_alone_refuses_attach_and_vacuum_into)
{
    sqlite3 *db = NULL;
    hl_cap_db_sqlite_setup();
    ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
    ASSERT_EQ(0, hl_cap_db_guard(db));
    EXPECT_NE(SQLITE_OK, sqlite3_exec(db, "ATTACH 'other.db' AS o", NULL, NULL, NULL));
    EXPECT_NE(SQLITE_OK, sqlite3_exec(db, "VACUUM INTO 'copy.db'", NULL, NULL, NULL));
    sqlite3_close(db);
}

/* ── Audit 10 H3: SQLite's allocations are charged too ───────────────── */

/* randomblob(N) is one opcode however large N is: the progress handler saw
 * one instruction. Its allocation is charged by size, and once the budget is
 * gone the allocation fails, so the statement stops at once. */
UTEST(hl_cap_db, a_large_allocation_is_charged_and_stops_the_statement)
{
    setup_db();
    HlDbOpBudget b = { .limit = 5000, .used = 0, .tripped = 0 };   /* ~320 KB of allocations */
    HlDbBudgetBinding prev = hl_db_budget_swap(hl_db_op_budget_charge, &b);
    int64_t n = 0;
    int rc = hl_cap_db_query(&test_cache, "SELECT length(randomblob(600000))",
                             NULL, 0, count_cb, &n, NULL);
    hl_db_budget_restore(prev);
    EXPECT_NE(0, rc);
    EXPECT_EQ(1, b.tripped);
    EXPECT_NE(600000, (int)n);
    teardown_db();
}

UTEST(hl_cap_db, allocations_are_charged_by_size)
{
    setup_db();
    HlDbOpBudget b = { .limit = 100000000, .used = 0, .tripped = 0 };
    HlDbBudgetBinding prev = hl_db_budget_swap(hl_db_op_budget_charge, &b);
    int64_t n = 0;
    int rc = hl_cap_db_query(&test_cache, "SELECT length(randomblob(640000))",
                             NULL, 0, count_cb, &n, NULL);
    hl_db_budget_restore(prev);
    EXPECT_EQ(0, rc);
    EXPECT_EQ(640000, (int)n);
    EXPECT_GE(b.used, (int64_t)(640000 / HL_DB_ALLOC_UNIT_BYTES));
    EXPECT_EQ(0, b.tripped);
    teardown_db();
}

/* A tripped budget refuses allocations - but not the rollback Hull runs on
 * the run's behalf (a batch whose fn hit the limit, the stale-txn guard). */
UTEST(hl_cap_db, rollback_runs_after_the_budget_tripped)
{
    setup_db();
    ASSERT_EQ(0, hl_cap_db_begin(test_db));
    ASSERT_EQ(0, sqlite3_exec(test_db, "INSERT INTO users (name) VALUES ('a')",
                              NULL, NULL, NULL));
    HlDbOpBudget b = { .limit = 1, .used = 0, .tripped = 1 };   /* tripped */
    HlDbBudgetBinding prev = hl_db_budget_swap(hl_db_op_budget_charge, &b);
    EXPECT_NE(SQLITE_OK, sqlite3_exec(test_db, "SELECT length(randomblob(100000))",
                                      NULL, NULL, NULL));
    EXPECT_EQ(0, hl_cap_db_rollback(test_db));
    hl_db_budget_restore(prev);
    EXPECT_NE(0, sqlite3_get_autocommit(test_db));
    int64_t n = -1;
    EXPECT_EQ(0, hl_cap_db_query(&test_cache, "SELECT count(*) FROM users",
                                 NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(0, (int)n);
    teardown_db();
}

/* Audit 27: a db.async op's budget tripped only on units, and a wait for the
 * server charges none - a pg_sleep held a pool thread for good, and the
 * SIGTERM join with it. A 0-unit charge now trips once the op's deadline has
 * passed, once its continuation was cancelled, or while the pool shuts down. */
UTEST(hl_cap_db, op_budget_stops_on_deadline_cancel_and_shutdown)
{
    HlDbOpBudget b = { 0, 0, 0, 0, 0, NULL };
    EXPECT_EQ(0, hl_db_op_budget_charge(&b, 0));          /* nothing set */

    b.deadline_ms = hl_db_monotonic_ms() + 60000;
    EXPECT_EQ(0, hl_db_op_budget_charge(&b, 0));          /* not yet */
    b.deadline_ms = hl_db_monotonic_ms() - 1;
    EXPECT_EQ(1, hl_db_op_budget_charge(&b, 0));
    EXPECT_EQ(HL_DB_OP_STOP_DEADLINE, b.reason);
    EXPECT_EQ(1, hl_db_op_budget_charge(&b, 0));          /* sticky */

    int cancelled = 0;
    HlDbOpBudget c = { 1000, 0, 0, 0, 0, &cancelled };
    EXPECT_EQ(0, hl_db_op_budget_charge(&c, 0));
    cancelled = 1;
    EXPECT_EQ(1, hl_db_op_budget_charge(&c, 0));
    EXPECT_EQ(HL_DB_OP_STOP_CANCEL, c.reason);

    HlDbOpBudget d = { 1000, 0, 0, 0, 0, NULL };
    hl_db_op_shutdown(1);
    EXPECT_EQ(1, hl_db_op_budget_charge(&d, 0));
    hl_db_op_shutdown(0);
    EXPECT_EQ(HL_DB_OP_STOP_CANCEL, d.reason);
    HlDbOpBudget e = { 1000, 0, 0, 0, 0, NULL };
    EXPECT_EQ(0, hl_db_op_budget_charge(&e, 0));          /* flag cleared */
    EXPECT_EQ(1, hl_db_op_budget_charge(&e, 2000));       /* units still trip */
    EXPECT_EQ(HL_DB_OP_STOP_UNITS, e.reason);
}

/* Audit 28: a worker.dispatch job's SQL is charged to its VM's budget, not
 * an HlDbOpBudget, so the pool-shutdown stop missed it. Any bound charge
 * interrupts while the pool is freed; the binding is still asked. */
static int g_bound_calls;
static int bound_never_trips(void *ud, int64_t units)
{
    (void)ud; (void)units;
    g_bound_calls++;
    return 0;
}

UTEST(hl_cap_db, pool_shutdown_stops_every_bound_charge)
{
    HlDbBudgetBinding prev = hl_db_budget_swap(bound_never_trips, &g_bound_calls);
    g_bound_calls = 0;
    EXPECT_EQ(0, hl_db_budget_charge(0));
    hl_db_op_shutdown(1);
    EXPECT_EQ(1, hl_db_budget_charge(0));
    EXPECT_EQ(2, g_bound_calls);
    hl_db_op_shutdown(0);
    EXPECT_EQ(0, hl_db_budget_charge(5));
    (void)hl_db_budget_swap(NULL, NULL);
    hl_db_op_shutdown(1);
    EXPECT_EQ(0, hl_db_budget_charge(0));   /* unbound: never an interrupt */
    hl_db_op_shutdown(0);
    hl_db_budget_restore(prev);
}

/* The pragmas that set how much memory or how many threads SQLite uses are
 * refused to SQL; reading them is not. */
UTEST(hl_cap_db, memory_pragmas_cannot_be_set)
{
    setup_db();
    static const char *const refused[] = {
        "PRAGMA cache_size=-1000000", "PRAGMA main.cache_size = 100000",
        "PRAGMA hard_heap_limit=1", "PRAGMA soft_heap_limit=0",
        "PRAGMA threads=4", "PRAGMA temp_store=FILE", "PRAGMA cache_spill=0",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        EXPECT_NE(SQLITE_OK, sqlite3_exec(test_db, refused[i], NULL, NULL, NULL));
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, "PRAGMA cache_size", NULL, NULL, NULL));
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, "PRAGMA hard_heap_limit", NULL, NULL, NULL));
    teardown_db();
}

/* hl_cap_db_refuse_txn_control: the agent query's gate, at prepare time, so a
 * comment the text reader nests (SQLite does not) cannot hide a BEGIN. */
UTEST(hl_cap_db, refuse_txn_control_sees_through_comments)
{
    setup_db();
    hl_cap_db_refuse_txn_control(test_db, 1);
    static const char *const refused[] = {
        "BEGIN", "/* /* */ BEGIN; -- */", "COMMIT", "ROLLBACK",
        "SAVEPOINT s", "RELEASE s", "END",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        sqlite3_stmt *st = NULL;
        EXPECT_NE(SQLITE_OK, sqlite3_prepare_v2(test_db, refused[i], -1, &st, NULL));
        sqlite3_finalize(st);
    }
    /* The rest of the guard still applies, and plain reads work. */
    EXPECT_NE(SQLITE_OK, sqlite3_exec(test_db, "ATTACH 'x.db' AS x", NULL, NULL, NULL));
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, "SELECT 1", NULL, NULL, NULL));
    hl_cap_db_refuse_txn_control(test_db, 0);
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, "BEGIN; COMMIT", NULL, NULL, NULL));
    teardown_db();
}

UTEST(hl_cap_db, namespace_check_blocks_hull_tables)
{
    ASSERT_EQ(hl_cap_db_check_namespace("SELECT * FROM _hull_outbox"), HL_DB_ERR_DENIED);
    ASSERT_EQ(hl_cap_db_check_namespace("DROP TABLE _hull_migrations"), HL_DB_ERR_DENIED);
    ASSERT_EQ(hl_cap_db_check_namespace("INSERT INTO _HULL_OUTBOX VALUES(1)"), HL_DB_ERR_DENIED);
    ASSERT_EQ(hl_cap_db_check_namespace("SELECT * FROM users"), HL_DB_OK);
    ASSERT_EQ(hl_cap_db_check_namespace("SELECT * FROM hull_data"), HL_DB_OK);
    ASSERT_EQ(hl_cap_db_check_namespace(NULL), HL_DB_ERR_DENIED);
}

/* Audit 25: DuckDB's query() / query_table() build a statement or a table
 * name at run time ('_hu' || 'll_sessions'), past the text check above. */
UTEST(hl_cap_db, dynamic_sql_calls_are_found)
{
    static const char *const hit[] = {
        "SELECT * FROM query('SELECT * FROM _hu' || 'll_sessions')",
        "SELECT * FROM query_table('_hu' || 'll_sessions')",
        "select * from QUERY_TABLE ('x')",
        "SELECT * FROM query/* gap */('x')",
        "SELECT * FROM query -- gap\n ('x')",
        "SELECT * FROM query /* /* nested */ */ ('x')",
        "SELECT * FROM query /* /* not nested */ ('x')",
        "SELECT * FROM \"query\"('x')",
        "SELECT * FROM \"Query_Table\"('x')",
        "SELECT * FROM main.query('x')",
        "SELECT * FROM system.main.\"query\"('x')",
        "SELECT json_execute_serialized_sql(j) FROM t",
        "CREATE MACRO m() AS TABLE FROM query('x')",
        "SELECT * FROM U&\"\\0071uery\"('x')",
        "SELECT 'query(' AS s",          /* literals are not skipped: refused */
        /* Audit 28 (fuzz_sql_kw): a '"' in a literal or comment opened a
         * "name" that swallowed the call, and CR did not end a -- comment. */
        "SELECT '\"', * FROM query('SELECT * FROM _h'||'ull_sessions')",
        "--\"\n SELECT * FROM query('x')",
        "SELECT * FROM query --\r ('x')",
    };
    for (size_t i = 0; i < sizeof hit / sizeof hit[0]; i++)
        EXPECT_EQ(1, hl_cap_db_sql_calls_dynamic_sql(hit[i]));

    static const char *const miss[] = {
        "SELECT query FROM t",
        "SELECT query, query_table FROM t WHERE query = 1",
        "SELECT myquery('x'), query_tables('y'), query_x('z')",
        "SELECT \"query\" FROM t",
        "CREATE TABLE t (query TEXT, query_table TEXT)",
        "SELECT * FROM t",
        "",
    };
    for (size_t i = 0; i < sizeof miss / sizeof miss[0]; i++)
        EXPECT_EQ(0, hl_cap_db_sql_calls_dynamic_sql(miss[i]));
    EXPECT_EQ(0, hl_cap_db_sql_calls_dynamic_sql(NULL));
}

/* Audit 32: DuckDB statements that run SQL from a file or copy every table
 * without naming one are refused, in any spelling and any statement. */
UTEST(hl_cap_db, whole_database_statements_are_refused)
{
    static const char *const hit[] = {
        "IMPORT DATABASE 'data/imp'",
        "import database 'data/imp'",
        "  \n\tImPoRt   DaTaBaSe 'x'",
        "EXPORT DATABASE 'data/x'",
        "EXPORT DATABASE 'data/x' (FORMAT parquet)",
        "COPY FROM DATABASE db TO c",
        "copy from database db to c (SCHEMA)",
        "/* lead */ IMPORT /* gap */ DATABASE 'x'",
        "-- lead\nEXPORT -- gap\n DATABASE 'x'",
        "IMPORT /* /* nested */ */ DATABASE 'x'",
        "IMPORT /* /* not nested */ DATABASE 'x'",
        "COPY /* a */ FROM -- b\n DATABASE db TO c",
        "COPY --\r FROM DATABASE db TO c",
        "SELECT 1; IMPORT DATABASE 'x'",
        "ATTACH 'data/c.duckdb' AS c; COPY FROM DATABASE db TO c",
        "CREATE TABLE t (x INT); EXPORT DATABASE 'x'; SELECT 1",
        "PRAGMA import_database('data/imp')",
        "PRAGMA IMPORT_DATABASE('data/imp')",
        "PRAGMA \"import_database\"('data/imp')",
        "CALL export_database('x')",
        "SELECT 'export database' AS s",   /* literals are not skipped */
    };
    for (size_t i = 0; i < sizeof hit / sizeof hit[0]; i++)
        EXPECT_EQ(1, hl_cap_db_sql_reads_whole_database(hit[i]));

    static const char *const miss[] = {
        "SELECT * FROM t",
        "COPY t TO 'data/t.csv'",
        "COPY t FROM 'data/t.csv'",
        "SELECT import, export, copy FROM t",
        "SELECT database FROM t",
        "CREATE TABLE imports (export_id INT, database_name TEXT)",
        "SELECT * FROM t WHERE importance = 1",
        "ATTACH 'data/c.duckdb' AS c",
        "",
    };
    for (size_t i = 0; i < sizeof miss / sizeof miss[0]; i++)
        EXPECT_EQ(0, hl_cap_db_sql_reads_whole_database(miss[i]));
    EXPECT_EQ(0, hl_cap_db_sql_reads_whole_database(NULL));
}

/* The keyword gap skips share one budget: `import/*` repeated, each opening
 * an unterminated comment, stays linear and fails closed. */
UTEST(hl_cap_db, whole_database_scan_is_linear)
{
    size_t n = 1u << 20;
    char *big = malloc(n + 1);
    ASSERT_TRUE(big != NULL);
    for (size_t i = 0; i + 8 <= n; i += 8) memcpy(big + i, "import/*", 8);
    big[n - n % 8] = '\0';
    clock_t t0 = clock();
    EXPECT_EQ(1, hl_cap_db_sql_reads_whole_database(big));
    for (size_t i = 0; i + 8 <= n; i += 8) memcpy(big + i, "copy  --", 8);
    EXPECT_EQ(1, hl_cap_db_sql_reads_whole_database(big));
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    EXPECT_LT(secs, 5.0);
    free(big);
}

/* Audit 33: ATTACH / DETACH / USE / SET search_path|schema switch catalogs,
 * which would read _hull_* tables through a cross-catalog view with no _hull_
 * in the app SQL. The DuckDB backend refuses them. */
UTEST(hl_cap_db, catalog_switch_statements_are_refused)
{
    static const char *const hit[] = {
        "ATTACH 'data/c.duckdb' AS c",
        "attach 'x' as c (READ_ONLY)",
        "DETACH c",
        "detach database c",
        "USE c",
        "USE c.main",
        "use \"c\"",
        "SET schema = 'c.main'",
        "SET schema TO 'c.main'",
        "set search_path = 'a,b'",
        "SET SESSION schema = 'c'",
        "SET GLOBAL search_path='a'",
        "RESET schema",
        "RESET search_path",
        "PRAGMA schema='c'",
        "\"ATTACH\" 'x' AS c",
        "/* lead */ ATTACH 'x' AS c",
        "-- lead\nUSE c",
        "SET /* gap */ schema = 'c'",
        "SELECT 1; ATTACH 'x' AS c",
        "CREATE TABLE t (x INT); USE c",
        "SET -- c\n SESSION /* d */ search_path = 'a'",
    };
    for (size_t i = 0; i < sizeof hit / sizeof hit[0]; i++)
        EXPECT_EQ(1, hl_cap_db_sql_switches_catalog(hit[i]));

    static const char *const miss[] = {
        "SELECT * FROM t",
        "SET memory_limit = '1GB'",
        "SET threads = 4",
        "RESET memory_limit",
        "PRAGMA database_list",
        "SELECT schema, search_path FROM t",       /* not after SET/RESET */
        "CREATE TABLE schemas (search_path TEXT)",
        "BEGIN TRANSACTION",
        "COMMIT",
        "ROLLBACK",
        "",
    };
    for (size_t i = 0; i < sizeof miss / sizeof miss[0]; i++)
        EXPECT_EQ(0, hl_cap_db_sql_switches_catalog(miss[i]));
    EXPECT_EQ(0, hl_cap_db_sql_switches_catalog(NULL));
}

/* The SET/RESET/PRAGMA next-word peeks share one gap budget, so a run of
 * `set/*` openers stays linear and fails closed. */
UTEST(hl_cap_db, catalog_switch_scan_is_linear)
{
    size_t n = 1u << 20;
    char *big = malloc(n + 1);
    ASSERT_TRUE(big != NULL);
    for (size_t i = 0; i + 8 <= n; i += 8) memcpy(big + i, "set   /*", 8);
    big[n - n % 8] = '\0';
    clock_t t0 = clock();
    (void)hl_cap_db_sql_switches_catalog(big);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    EXPECT_LT(secs, 5.0);
    free(big);
}

/* Audit 30: the quoted-name scan ran to the closing quote from every '"',
 * so a run of quotes was quadratic (1 MB ~ minutes, before any budget
 * charge). It stops once a name cannot fit; detection is unchanged. */
UTEST(hl_cap_db, dynamic_sql_scan_is_linear)
{
    EXPECT_EQ(1, hl_cap_db_sql_calls_dynamic_sql("SELECT * FROM \"QUERY\" ('x')"));
    EXPECT_EQ(1, hl_cap_db_sql_calls_dynamic_sql("SELECT * FROM \"query\"('x')"));

    size_t n = 1u << 20;
    char *big = malloc(n + 64);
    ASSERT_TRUE(big != NULL);
    memset(big, '"', n);
    big[n] = '\0';
    clock_t t0 = clock();
    EXPECT_EQ(0, hl_cap_db_sql_calls_dynamic_sql(big));
    /* Every byte is still scanned: a call after the quotes is found. */
    strcpy(big + n, " query('x')");
    EXPECT_EQ(1, hl_cap_db_sql_calls_dynamic_sql(big));
    /* And one unterminated name a megabyte long. */
    memset(big, 'a', n);
    big[0] = '"';
    big[n] = '\0';
    EXPECT_EQ(0, hl_cap_db_sql_calls_dynamic_sql(big));
    /* Every bare dynamic name skips the comment after it, and an
     * unterminated one runs to the end: `query--query--...` and its
     * block-comment twin were quadratic (audit 30 re-audit). Past the
     * shared skip budget the scan fails closed. */
    for (size_t i = 0; i + 7 <= n; i += 7) memcpy(big + i, "query--", 7);
    big[n - n % 7] = '\0';
    EXPECT_EQ(1, hl_cap_db_sql_calls_dynamic_sql(big));
    for (size_t i = 0; i + 7 <= n; i += 7) memcpy(big + i, "query/*", 7);
    big[n - n % 7] = '\0';
    EXPECT_EQ(1, hl_cap_db_sql_calls_dynamic_sql(big));
    /* A short comment after a name that is not a call stays allowed. */
    EXPECT_EQ(0, hl_cap_db_sql_calls_dynamic_sql("SELECT query -- note\nFROM t"));
    EXPECT_EQ(1, hl_cap_db_sql_calls_dynamic_sql("SELECT * FROM query /* x */ ('y')"));
    /* The quadratic scan took ~10 min on 1 MB; the linear one ~0.3 s, and
     * up to ~3 s under MSan / coverage instrumentation. 30 s separates the
     * two on any CI build. */
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    EXPECT_LT(secs, 30.0);
    free(big);
}

/* Audit 34: DuckDB constant-folds table-function arguments, so a table name
 * built at run time reached _hull_* tables through storage_info (column
 * min / max values), pragma_table_info, pragma_show and histogram. */
UTEST(hl_cap_db, table_name_values_are_refused)
{
    static const char *const hit[] = {
        "SELECT * FROM pragma_storage_info(concat('_', 'hull_sessions'))",
        "SELECT * FROM pragma_storage_info('users')",   /* any spelling */
        "PRAGMA storage_info('users')",
        "PRAGMA storage_info = 'users'",
        "CALL pragma_storage_info(chr(95) || 'hull_sessions')",
        "SELECT * FROM \"pragma_storage_info\"('t')",
        "SELECT * FROM \"Storage_Info\"('t')",
        "SELECT 'storage_info'",                         /* literals not skipped */
        "SELECT * FROM pragma_table_info(concat('_', 'hull_sessions'))",
        "SELECT * FROM pragma_table_info(chr(95) || 'hull_x')",
        "SELECT * FROM pragma_table_info('_' || 'hull_x')",
        "SELECT * FROM pragma_table_info(?)",
        "SELECT * FROM pragma_table_info($1)",
        "SELECT * FROM pragma_table_info(E'\\x5fhull_x')",
        "SELECT * FROM pragma_table_info($$_hull_x$$)",
        "PRAGMA table_info(getvariable('t'))",
        "PRAGMA table_info = concat('a', 'b')",
        "SELECT * FROM pragma_show(concat('_', 'hull_x'))",
        "SELECT * FROM histogram(concat('_', 'hull_sessions'), x)",
        "SELECT * FROM histogram_values(chr(95) || 'hull_s', x)",
        "SELECT * FROM duckdb_table_sample(concat('a', 'b'))",
        "SELECT * FROM \"pragma_table_info\"(concat('a', 'b'))",
        "SELECT * FROM pragma_table_info /* gap */ (concat('a', 'b'))",
        "SELECT * FROM pragma_table_info -- gap\n (concat('a', 'b'))",
        "SELECT * FROM pragma_table_info /* /* nested */ */ (lower('A'))",
        "SELECT * FROM pragma_table_info('a'\n'b')",
        "CALL enable_profiling(save_location = 'x.txt')",
        "PRAGMA enable_profiling",
        "CALL enable_logging(level = 'debug')",
        "CALL truncate_duckdb_logs()",
        /* Audit 35: still refused with show / table_info / histogram read
         * by position. */
        "PRAGMA show = concat('_', 'hull_x')",
        "PRAGMA \"table_info\" = concat('a', 'b')",
        "PRAGMA /* c */ table_info = lower('X')",
        "CALL show(concat('a', 'b'))",
        "SELECT * FROM show(concat('a', 'b'))",
        "SELECT * FROM table_info(lower('X'))",
        "SELECT * FROM t, histogram(concat('_', 'hull_s'), x)",
        "SELECT * FROM t JOIN histogram(lower('X'), x) h ON true",
        "SELECT * FROM (histogram(lower('X'), x))",
        "SELECT * FROM /* c */ histogram(lower('X'), x)",
        "SELECT * FROM \"histogram\"(lower('X'), x)",
        "SELECT * FROM LATERAL histogram(lower('X'), x)",
        "SUMMARIZE histogram(lower('X'), x)",
        "SELECT * FROM histogram(lower('X') /* ) */, x)",
        "SELECT * FROM histogram(lower('X') -- )\n, x)",
        "SELECT * FROM histogram(lower($$)$$), x)",
        "SELECT * FROM histogram(lower(E'\\'), x)",
        "SELECT * FROM histogram(lower('X')",
        /* POINTER arguments: refused by name (defence in depth). */
        "SELECT * FROM arrow_scan(?, ?, ?)",
        "SELECT * FROM \"Arrow_Scan_Dumb\"(?, ?, ?)",
    };
    for (size_t i = 0; i < sizeof hit / sizeof hit[0]; i++)
        EXPECT_EQ(1, hl_cap_db_sql_names_table_by_value(hit[i]));

    static const char *const miss[] = {
        "SELECT * FROM pragma_table_info('users')",
        "SELECT * FROM pragma_table_info(users)",
        "SELECT * FROM pragma_table_info(main.users)",
        "SELECT * FROM pragma_table_info(\"My Table\")",
        "PRAGMA table_info('users')",
        "PRAGMA table_info = 'users'",
        "SELECT * FROM pragma_show('users')",
        "SELECT * FROM histogram('t', x)",
        "SELECT * FROM histogram(t, x, bin_count := 4)",
        "SELECT histogram(x) FROM t",
        "SELECT histogram(t.x, [1, 2, 3]) FROM t",
        "SHOW TABLES",
        "SHOW t",
        "SELECT table_info, histogram FROM t",
        "CREATE TABLE stats (storage TEXT, info TEXT)",
        /* Audit 35: #912 refused these columns and aggregates. */
        "UPDATE items SET show = 1",
        "UPDATE items SET \"show\" = 1",          /* DuckDB reserves show */
        "SELECT * FROM items WHERE \"Show\" = lower(name)",
        "UPDATE items SET show = ? WHERE id = ?",
        "SELECT * FROM items WHERE show = ?",
        "SELECT * FROM items WHERE show = lower(name)",
        "SELECT * FROM items WHERE table_info = concat('a', 'b')",
        "SELECT histogram(lower(name)) FROM t",
        "SELECT id, histogram(lower(name)) FROM t",
        "SELECT histogram(lower(name), [1, 2, 3]) FROM t",
        "SELECT (histogram(lower(name))) FROM t",
        "SELECT * FROM t WHERE x IN (SELECT histogram(lower(y)) FROM u)",
        "SELECT * FROM histogram('t', x)",
        "PRAGMA show('users')",
        "PRAGMA table_info = users",
        "SELECT * FROM t",
        "",
    };
    for (size_t i = 0; i < sizeof miss / sizeof miss[0]; i++)
        EXPECT_EQ(0, hl_cap_db_sql_names_table_by_value(miss[i]));
    EXPECT_EQ(0, hl_cap_db_sql_names_table_by_value(NULL));
}

/* Audit 34: DuckDB keeps the database file, its WAL sidecars and its temp
 * directory readable from SQL whatever external access says, so
 * read_blob('app.duckdb') returned the raw file, _hull_* rows included. */
UTEST(hl_cap_db, database_file_reads_are_refused)
{
    char root[HL_TEST_PATH_MAX];
    ASSERT_TRUE(hl_test_mkdtemp(root, sizeof root, "hull_duckfile") != NULL);
    char real[PATH_MAX];
    ASSERT_TRUE(realpath(root, real) != NULL);
    char dbdir[PATH_MAX + 8], datadir[PATH_MAX + 8], protect[PATH_MAX + 32];
    snprintf(dbdir, sizeof dbdir, "%s/db", real);
    snprintf(datadir, sizeof datadir, "%s/data", real);
    snprintf(protect, sizeof protect, "%s/app.duckdb", dbdir);
    ASSERT_EQ(0, mkdir(dbdir, 0700));
    ASSERT_EQ(0, mkdir(datadir, 0700));

    static const char *const hit_b[] = {   /* mode B: an fs policy */
        "SELECT content FROM read_blob('app.duckdb')",
        "SELECT content FROM read_text('APP.DUCKDB.wal')",
        "SELECT * FROM read_text('db/x/../app.duckdb.wal.checkpoint')",
        "SELECT * FROM read_blob('file:///abs/app.duckdb')",
        "SELECT * FROM 'app.duckdb.wal'",
        "SELECT * FROM glob('app.duckdb.tmp/*')",
        "COPY t TO 'app.duckdb'",
        "COPY t FROM 'db/app.duckdb.wal' (FORMAT csv)",
        "SELECT 'app.duckdb'",                         /* the name, anywhere */
        "SELECT * FROM read_blob(concat('app.', 'duckdb'))",
        "SELECT * FROM read_blob('app.' || 'duck' || 'db')",
        "SELECT * FROM read_blob(?)",
        "SELECT * FROM read_blob($$x$$)",
        "SELECT * FROM read_blob(E'\\x61pp.duckdb')",
        "SELECT E'\\x61'",
        "SELECT * FROM read_text('a'\n'pp.duckdb')",
        "SELECT * FROM read_text(['data/a.csv', concat('a', 'b')])",
        "SELECT * FROM read_text(lower('X'))",
        "SELECT * FROM \"read_blob\"(lower('X'))",
        "SELECT * FROM read_blob /* gap */ (lower('X'))",
        "SELECT * FROM parquet_metadata(lower('X'))",
        "SELECT * FROM sniff_csv(lower('X'))",
        "SELECT * FROM read_blob('~/x*')",
        "SELECT * FROM read_blob('/*')",
        "SELECT * FROM read_blob('no_such_dir_hull_zz/*')",
        "COPY t TO (concat('a', '.csv'))",
        "COPY (SELECT 1) TO $$x.csv$$",
        "COPY t FROM 'a.csv'\n'b'",
    };
    for (size_t i = 0; i < sizeof hit_b / sizeof hit_b[0]; i++)
        EXPECT_EQ(1, hl_cap_db_sql_reaches_db_files(hit_b[i], 0, protect));

    /* Globs whose directory reaches the database directory. */
    char sql[3 * PATH_MAX];
    const char *const reach[] = { "/*", "/db/*", "/db/*.wal", "/**/x",
                                  "/data/../db/a*", "/d?/x" };
    for (size_t i = 0; i < sizeof reach / sizeof reach[0]; i++) {
        char path[2 * PATH_MAX];
        snprintf(path, sizeof path, "%s%s", real, reach[i]);
        snprintf(sql, sizeof sql, "SELECT * FROM read_blob('%s')", path);
        EXPECT_EQ(1, hl_cap_db_sql_reaches_db_files(sql, 0, protect));
        snprintf(sql, sizeof sql, "COPY t FROM '%s'", path);
        EXPECT_EQ(1, hl_cap_db_sql_reaches_db_files(sql, 0, protect));
        snprintf(sql, sizeof sql, "SELECT * FROM read_csv(['%s/data/a.csv', '%s'])",
                 real, path);
        EXPECT_EQ(1, hl_cap_db_sql_reaches_db_files(sql, 0, protect));
    }
    /* Mode B's documented use: files in the declared directories. */
    snprintf(sql, sizeof sql,
             "SELECT * FROM read_csv('%s/data/*.csv', header = true)", real);
    EXPECT_EQ(0, hl_cap_db_sql_reaches_db_files(sql, 0, protect));
    snprintf(sql, sizeof sql, "COPY t FROM '%s/data/*.csv' (FORMAT csv)", real);
    EXPECT_EQ(0, hl_cap_db_sql_reaches_db_files(sql, 0, protect));
    snprintf(sql, sizeof sql, "SELECT * FROM read_parquet(['%s/data/a.parquet', "
             "\"%s/data/b.parquet\"])", real, real);
    EXPECT_EQ(0, hl_cap_db_sql_reaches_db_files(sql, 0, protect));
    static const char *const miss_b[] = {
        "SELECT * FROM read_csv('data/a.csv')",
        "SELECT * FROM read_csv('data/a.csv', header = true, columns = {'a': 'INT'})",
        "SELECT * FROM read_json_auto(\"data/a.json\")",
        "COPY t TO 'data/out.csv' (FORMAT csv, HEADER)",
        "COPY (SELECT * FROM (SELECT 1) s WHERE 1 = 1) TO 'data/out.parquet'",
        "COPY t FROM STDIN",
        "SELECT copy FROM t",
        "SELECT read_count, glob_x FROM t WHERE name GLOB 'x*'",
        "SELECT * FROM t WHERE e = 'e'",
        "SELECT E'plain'",
        "SELECT * FROM t",
        "",
        /* Audit 35: the name outside a literal, or not a path component. */
        "SELECT app.duckdb FROM app",
        "SELECT * FROM app_duckdb_stats",
        "SELECT 'my_app.duckdb'",
        "SELECT 'app.duckdbx'",
        "SELECT * FROM read_csv('data/app.duckdb.csv.old')",
    };
    for (size_t i = 0; i < sizeof miss_b / sizeof miss_b[0]; i++)
        EXPECT_EQ(0, hl_cap_db_sql_reaches_db_files(miss_b[i], 0, protect));
    /* Audit 35: still refused - every spelling of the file in a literal. */
    static const char *const hit_35[] = {
        "SELECT * FROM read_blob('./APP.duckdb')",
        "SELECT * FROM read_text('db/app.duckdb.WAL.recovery')",
        "SELECT * FROM read_text('db/app.duckdb.tmp/x')",
        "SELECT * FROM read_blob('db\\app.duckdb')",
        "SELECT * FROM read_blob(\"app.duckdb\")",
        "SELECT * FROM 'db/app.duckdb'",
        "SELECT * FROM $$app.duckdb$$",
        "SELECT * FROM $t$app.duckdb$t$",
        "COPY t TO './app.duckdb.wal'",
        "COPY t FROM \"app.duckdb\"",
        "SELECT * FROM read_blob('app.duckdb/')",
        /* A quote inside a comment shifts the parity: still found. */
        "/* ' */ SELECT * FROM read_blob('app.duckdb') /* ' */",
        "SELECT * FROM read_blob(U&'\\0061pp.duckdb')",
        "SELECT U&'\\0061'",
    };
    for (size_t i = 0; i < sizeof hit_35 / sizeof hit_35[0]; i++)
        EXPECT_EQ(1, hl_cap_db_sql_reaches_db_files(hit_35[i], 0, protect));

    /* Mode A: no fs policy, so no file function may be called at all. */
    static const char *const hit_a[] = {
        "SELECT * FROM read_csv('data/a.csv')",
        "SELECT * FROM read_text('x')",
        "SELECT * FROM glob('*')",
        "SELECT * FROM parquet_schema('x.parquet')",
        "SELECT * FROM \"read_blob\"('x')",
        "SELECT * FROM read_ndjson_objects('x')",
    };
    for (size_t i = 0; i < sizeof hit_a / sizeof hit_a[0]; i++)
        EXPECT_EQ(1, hl_cap_db_sql_reaches_db_files(hit_a[i], 1, protect));
    EXPECT_EQ(0, hl_cap_db_sql_reaches_db_files("SELECT read_x FROM t", 1, protect));
    EXPECT_EQ(0, hl_cap_db_sql_reaches_db_files("SELECT 42 AS answer", 1, protect));
    EXPECT_EQ(0, hl_cap_db_sql_reaches_db_files(NULL, 1, protect));
    /* A path that could not be resolved ("/": no name) refuses everything. */
    EXPECT_EQ(1, hl_cap_db_sql_reaches_db_files("SELECT 1", 0, "/"));

    rmdir(dbdir);
    rmdir(datadir);
    rmdir(real);
}

/* Audit 35: #912 refused any statement holding the database file's name as
 * a substring, so an extensionless name broke ordinary SQL - `duckdb://hull`
 * refused every _hull_migrations statement (the app could not boot),
 * `duckdb://sessions` the session store, `duckdb://app` `approvals`. The
 * name is read only in literals, on path-component boundaries. */
UTEST(hl_cap_db, extensionless_database_names_do_not_refuse_sql)
{
    static const struct { const char *protect, *sql; } miss[] = {
        { "/nonexistent/d/hull",
          "CREATE TABLE IF NOT EXISTS _hull_migrations (name VARCHAR(255) "
          "PRIMARY KEY, applied_at TEXT)" },
        { "/nonexistent/d/hull", "SELECT name FROM _hull_migrations ORDER BY name" },
        { "/nonexistent/d/hull", "INSERT INTO _hull_migrations (name) VALUES ('001_init.sql')" },
        { "/nonexistent/d/hull", "SELECT 'hull_x', 'my/hullabaloo'" },
        { "/nonexistent/d/sessions", "SELECT * FROM _hull_sessions WHERE id = ?" },
        { "/nonexistent/d/sessions", "DELETE FROM _hull_sessions WHERE expires_at < ?" },
        { "/nonexistent/d/app", "SELECT * FROM approvals WHERE app = ?" },
        { "/nonexistent/d/data", "SELECT data FROM user_data" },
        { "/nonexistent/d/data", "SELECT * FROM read_csv('data_2024.csv')" },
    };
    for (size_t i = 0; i < sizeof miss / sizeof miss[0]; i++)
        EXPECT_EQ(0, hl_cap_db_sql_reaches_db_files(miss[i].sql, 0, miss[i].protect));
    static const struct { const char *protect, *sql; } hit[] = {
        { "/nonexistent/d/hull", "SELECT * FROM read_text('hull')" },
        { "/nonexistent/d/hull", "SELECT * FROM read_blob('./HULL.wal')" },
        { "/nonexistent/d/hull", "SELECT * FROM read_blob('/nonexistent/d/hull')" },
        { "/nonexistent/d/hull", "COPY t TO 'hull.tmp/x.csv'" },
        { "/nonexistent/d/data", "SELECT * FROM read_blob('d/../Data')" },
        { "/nonexistent/d/data", "SELECT * FROM 'data.wal'" },
        { "/nonexistent/d/sessions", "SELECT content FROM read_blob('sessions')" },
    };
    for (size_t i = 0; i < sizeof hit / sizeof hit[0]; i++)
        EXPECT_EQ(1, hl_cap_db_sql_reaches_db_files(hit[i].sql, 0, hit[i].protect));
}

/* The look-aheads of both scans share one byte budget: runs of call
 * openers, COPYs and unterminated literals stay linear and fail closed. */
UTEST(hl_cap_db, table_value_and_file_scans_are_linear)
{
    size_t n = 1u << 20;
    char *big = malloc(n + 1);
    ASSERT_TRUE(big != NULL);
    clock_t t0 = clock();
    static const char *const pats[] = {
        "table_info(/*", "histogram('x", "read_csv('a',", "copy --",
        "copy (", "e'e'", "histogram(", "show = ",
        "(histogram((", ",histogram(lower(x)", "from histogram(", "pragma show = ",
    };
    for (size_t k = 0; k < sizeof pats / sizeof pats[0]; k++) {
        size_t pl = strlen(pats[k]);
        for (size_t i = 0; i + pl <= n; i += pl) memcpy(big + i, pats[k], pl);
        big[n - n % pl] = '\0';
        (void)hl_cap_db_sql_names_table_by_value(big);
        (void)hl_cap_db_sql_reaches_db_files(big, 0, "/nonexistent/app.duckdb");
    }
    /* A long text with no match still scans for the name at every byte. */
    memset(big, 'a', n);
    big[n] = '\0';
    EXPECT_EQ(0, hl_cap_db_sql_reaches_db_files(big, 0, "/nonexistent/app.duckdb"));
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    EXPECT_LT(secs, 30.0);
    free(big);
}

/* Postgres decodes U&"\005fhull_sessions" to _hull_sessions after this text
 * check has looked, so the Unicode-escape identifier form is refused. The
 * U&'...' string literal is not an identifier and stays allowed. */
UTEST(hl_cap_db, namespace_check_refuses_unicode_escape_identifiers)
{
    EXPECT_EQ(hl_cap_db_check_namespace("SELECT * FROM U&\"\\005fhull_sessions\""),
              HL_DB_ERR_DENIED);
    EXPECT_EQ(hl_cap_db_check_namespace("select * from u&\"d\\0061ta\""),
              HL_DB_ERR_DENIED);
    EXPECT_EQ(hl_cap_db_check_namespace("SELECT U&'d\\0061t\\+000061'"), HL_DB_OK);
    EXPECT_EQ(hl_cap_db_check_namespace("SELECT a & b FROM t"), HL_DB_OK);
}

UTEST(hl_cap_db, namespace_check_case_insensitive)
{
    ASSERT_EQ(hl_cap_db_check_namespace("SELECT * FROM _Hull_Outbox"), HL_DB_ERR_DENIED);
    ASSERT_EQ(hl_cap_db_check_namespace("SELECT * FROM _HULL_sessions"), HL_DB_ERR_DENIED);
    ASSERT_EQ(hl_cap_db_check_namespace("CREATE TABLE _hull_test (id INT)"), HL_DB_ERR_DENIED);
}

UTEST(hl_cap_db, namespace_check_allows_normal_tables)
{
    ASSERT_EQ(hl_cap_db_check_namespace("CREATE TABLE users (id INT)"), HL_DB_OK);
    ASSERT_EQ(hl_cap_db_check_namespace("SELECT * FROM orders"), HL_DB_OK);
    ASSERT_EQ(hl_cap_db_check_namespace("INSERT INTO items VALUES (1)"), HL_DB_OK);
    ASSERT_EQ(hl_cap_db_check_namespace(""), HL_DB_OK);
}

/* ── UDF integration tests ──────────────────────────────────────────── */

/* Simple C UDF for testing: returns the length of a TEXT argument */
static void test_strlen_func(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    int len = sqlite3_value_bytes(argv[0]);
    sqlite3_result_int(ctx, len);
}

/* Simple C aggregate UDF for testing: sums integer values */
typedef struct { int64_t sum; int initialized; } TestSumCtx;

static void test_sum_step(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    TestSumCtx *p = sqlite3_aggregate_context(ctx, (int)sizeof(TestSumCtx));
    if (!p) return;
    if (sqlite3_value_type(argv[0]) != SQLITE_NULL) {
        p->sum += sqlite3_value_int64(argv[0]);
        p->initialized = 1;
    }
}

static void test_sum_finalize(sqlite3_context *ctx)
{
    TestSumCtx *p = sqlite3_aggregate_context(ctx, 0);
    if (!p || !p->initialized)
        sqlite3_result_null(ctx);
    else
        sqlite3_result_int64(ctx, p->sum);
}

/* UDF test helpers (must be file-scope for C11 compliance) */

typedef struct { int count; int64_t lens[10]; } LenResult;

static int udf_len_cb(void *ctx, HlColumn *cols, int ncols)
{
    LenResult *r = (LenResult *)ctx;
    for (int i = 0; i < ncols; i++) {
        if (strcmp(cols[i].name, "nlen") == 0 && cols[i].value.type == HL_TYPE_INT)
            r->lens[r->count] = cols[i].value.i;
    }
    r->count++;
    return 0;
}

typedef struct { int count; int got_null; } NullResult;

static int udf_null_cb(void *ctx, HlColumn *cols, int ncols)
{
    NullResult *r = (NullResult *)ctx;
    for (int i = 0; i < ncols; i++) {
        if (strcmp(cols[i].name, "nlen") == 0 && cols[i].value.type == HL_TYPE_NIL)
            r->got_null = 1;
    }
    r->count++;
    return 0;
}

typedef struct { int count; int64_t total; } SumResult;

static int udf_sum_cb(void *ctx, HlColumn *cols, int ncols)
{
    SumResult *r = (SumResult *)ctx;
    for (int i = 0; i < ncols; i++) {
        if (strcmp(cols[i].name, "total") == 0 && cols[i].value.type == HL_TYPE_INT)
            r->total = cols[i].value.i;
    }
    r->count++;
    return 0;
}

UTEST(hl_cap_db, udf_scalar_register_and_query)
{
    setup_db();

    int rc = sqlite3_create_function_v2(
        test_db, "hull_strlen", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
        NULL, test_strlen_func, NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    HlValue p1[] = {
        { .type = HL_TYPE_TEXT, .s = "Hello", .len = 5 },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 0.0 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);

    HlValue p2[] = {
        { .type = HL_TYPE_TEXT, .s = "Hi", .len = 2 },
        { .type = HL_TYPE_INT, .i = 25 },
        { .type = HL_TYPE_DOUBLE, .d = 0.0 },
    };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p2, 3);

    LenResult lr = { .count = 0 };
    rc = hl_cap_db_query(&test_cache,
        "SELECT hull_strlen(name) AS nlen FROM users ORDER BY name",
        NULL, 0, udf_len_cb, &lr, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(lr.count, 2);
    ASSERT_EQ(lr.lens[0], 5);
    ASSERT_EQ(lr.lens[1], 2);

    teardown_db();
}

UTEST(hl_cap_db, udf_null_input_returns_null)
{
    setup_db();

    int rc = sqlite3_create_function_v2(
        test_db, "hull_strlen", 1, SQLITE_UTF8,
        NULL, test_strlen_func, NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    HlValue p[] = {
        { .type = HL_TYPE_NIL },
        { .type = HL_TYPE_INT, .i = 30 },
        { .type = HL_TYPE_DOUBLE, .d = 0.0 },
    };
    sqlite3_exec(test_db,
        "CREATE TABLE test_null (name TEXT, age INTEGER, score REAL)",
        NULL, NULL, NULL);
    hl_cap_db_exec(&test_cache,
        "INSERT INTO test_null (name, age, score) VALUES (?, ?, ?)", p, 3);

    NullResult nr = { .count = 0, .got_null = 0 };
    rc = hl_cap_db_query(&test_cache,
        "SELECT hull_strlen(name) AS nlen FROM test_null",
        NULL, 0, udf_null_cb, &nr, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(nr.count, 1);
    ASSERT_EQ(nr.got_null, 1);

    teardown_db();
}

UTEST(hl_cap_db, udf_aggregate_register_and_query)
{
    setup_db();

    int rc = sqlite3_create_function_v2(
        test_db, "hull_mysum", 1, SQLITE_UTF8,
        NULL, NULL, test_sum_step, test_sum_finalize, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    HlValue p1[] = { { .type = HL_TYPE_TEXT, .s = "A", .len = 1 },
                      { .type = HL_TYPE_INT, .i = 10 },
                      { .type = HL_TYPE_DOUBLE, .d = 0.0 } };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);

    HlValue p2[] = { { .type = HL_TYPE_TEXT, .s = "B", .len = 1 },
                      { .type = HL_TYPE_INT, .i = 20 },
                      { .type = HL_TYPE_DOUBLE, .d = 0.0 } };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p2, 3);

    HlValue p3[] = { { .type = HL_TYPE_TEXT, .s = "C", .len = 1 },
                      { .type = HL_TYPE_INT, .i = 30 },
                      { .type = HL_TYPE_DOUBLE, .d = 0.0 } };
    hl_cap_db_exec(&test_cache,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p3, 3);

    SumResult sr = { .count = 0, .total = 0 };
    rc = hl_cap_db_query(&test_cache,
        "SELECT hull_mysum(age) AS total FROM users",
        NULL, 0, udf_sum_cb, &sr, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(sr.count, 1);
    ASSERT_EQ(sr.total, 60);

    teardown_db();
}

UTEST(hl_cap_db, udf_unregister_removes_function)
{
    setup_db();

    int rc = sqlite3_create_function_v2(
        test_db, "hull_strlen", 1, SQLITE_UTF8,
        NULL, test_strlen_func, NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    /* Unregister by passing NULL function pointers (same nargs) */
    rc = sqlite3_create_function_v2(
        test_db, "hull_strlen", 1, SQLITE_UTF8,
        NULL, NULL, NULL, NULL, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    /* Should fail - function no longer exists */
    QueryResult result = { .count = 0 };
    rc = hl_cap_db_query(&test_cache,
        "SELECT hull_strlen('test') AS nlen", NULL, 0,
        collect_rows, &result, NULL);
    ASSERT_NE(rc, 0); /* Should error */

    teardown_db();
}

#ifdef HL_ENABLE_WASM
UTEST(hl_cap_db, udf_wasm_rejects_bad_prefix)
{
    setup_db();

    HlDbUdfOpts opts = {
        .sql_name    = "bad_name",  /* no hull_ prefix */
        .module_name = "echo",
        .nargs       = 1,
    };
    const char *err_msg = NULL;
    /* Wrap the test sqlite3* in a transient HlDbHandle - the UDF API now
     * takes HlDbHandle * so non-SQLite backends can fail-fast. */
    HlDbHandle handle = {0};
    ASSERT_EQ(hl_db_sqlite_wrap(&handle, test_db), 0);
    int rc = hl_cap_db_udf_register_wasm(&handle, NULL, &opts, NULL, NULL, NULL, &err_msg);
    ASSERT_NE(rc, 0);
    ASSERT_TRUE(err_msg != NULL);
    hl_db_sqlite_unwrap(&handle);

    teardown_db();
}

/* An arity or a name sqlite3_create_function_v2 refuses is refused up front.
 * The registration used to reach create_function_v2, which frees the udf ctx
 * through its xDestroy on failure, and then freed it again: a double free any
 * app with a compute module could trigger (audit 19). */
static const unsigned char udf_echo_wasm[] = { /* test_wasm.c's echo.wasm */
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14, 0x03, 0x60,
  0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f,
  0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x11, 0x01, 0x03, 0x65, 0x6e,
  0x76, 0x09, 0x68, 0x6f, 0x73, 0x74, 0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00,
  0x00, 0x03, 0x03, 0x02, 0x01, 0x02, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07,
  0x28, 0x03, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c,
  0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73,
  0x00, 0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73,
  0x69, 0x6f, 0x6e, 0x00, 0x02, 0x0a, 0x20, 0x02, 0x19, 0x00, 0x20, 0x01,
  0x20, 0x03, 0x4b, 0x04, 0x40, 0x41, 0x7e, 0x0f, 0x0b, 0x20, 0x02, 0x20,
  0x00, 0x20, 0x01, 0xfc, 0x0a, 0x00, 0x00, 0x20, 0x01, 0x0b, 0x04, 0x00,
  0x41, 0x01, 0x0b
};
static const HlEntry udf_entries[] = {
    { "compute/echo.wasm", udf_echo_wasm, sizeof udf_echo_wasm },
    { 0, 0, 0 }
};

UTEST(hl_cap_db, udf_wasm_refuses_bad_arity_and_long_name)
{
    setup_db();
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs;
    hl_vfs_init(&vfs, udf_entries, NULL);
    HlDbHandle handle = {0};
    ASSERT_EQ(hl_db_sqlite_wrap(&handle, test_db), 0);

    const char *err_msg = NULL;
    HlDbUdfOpts opts = { .sql_name = "hull_echo", .module_name = "echo",
                         .nargs = -5 };
    EXPECT_NE(hl_cap_db_udf_register_wasm(&handle, &cache, &opts, &vfs, NULL,
                                          NULL, &err_msg), 0);
    EXPECT_TRUE(err_msg && strcmp(err_msg, "invalid arguments") == 0);

    opts.nargs = 1001; /* over SQLITE_MAX_FUNCTION_ARG */
    err_msg = NULL;
    EXPECT_NE(hl_cap_db_udf_register_wasm(&handle, &cache, &opts, &vfs, NULL,
                                          NULL, &err_msg), 0);
    EXPECT_TRUE(err_msg && strcmp(err_msg, "invalid arguments") == 0);

    opts.is_aggregate = 1; /* the aggregate path too */
    err_msg = NULL;
    EXPECT_NE(hl_cap_db_udf_register_wasm(&handle, &cache, &opts, &vfs, NULL,
                                          NULL, &err_msg), 0);
    opts.is_aggregate = 0;

    char long_name[300];
    memcpy(long_name, "hull_", 5);
    memset(long_name + 5, 'x', sizeof long_name - 6);
    long_name[sizeof long_name - 1] = '\0';
    opts.sql_name = long_name;
    opts.nargs = 1;
    err_msg = NULL;
    EXPECT_NE(hl_cap_db_udf_register_wasm(&handle, &cache, &opts, &vfs, NULL,
                                          NULL, &err_msg), 0);
    EXPECT_TRUE(err_msg && strcmp(err_msg, "invalid arguments") == 0);

    /* A valid one still registers, and unregisters. */
    opts.sql_name = "hull_echo";
    err_msg = NULL;
    EXPECT_EQ(hl_cap_db_udf_register_wasm(&handle, &cache, &opts, &vfs, NULL,
                                          NULL, &err_msg), 0);
    EXPECT_EQ(hl_cap_db_udf_unregister(&handle, "hull_echo"), 0);

    hl_db_sqlite_unwrap(&handle);
    teardown_db();
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_db, udf_wasm_rejects_null_args)
{
    const char *err_msg = NULL;
    int rc = hl_cap_db_udf_register_wasm(NULL, NULL, NULL, NULL, NULL, NULL, &err_msg);
    ASSERT_NE(rc, 0);
    ASSERT_TRUE(err_msg != NULL);
}
#endif /* HL_ENABLE_WASM */

/* SQL cannot reach files the fs capability never granted: ATTACH of a file and
 * VACUUM INTO are refused, and neither leaves a file behind. The attaches that
 * name no file still work, and so does a plain VACUUM (which attaches ''). */
UTEST(hl_cap_db, sql_cannot_open_or_write_other_files)
{
    setup_db();
    const char *probe = "hull_sql_guard_probe.db";
    remove(probe);

    char sql[128];
    snprintf(sql, sizeof sql, "ATTACH '%s' AS other", probe);
    EXPECT_NE(sqlite3_exec(test_db, sql, NULL, NULL, NULL), SQLITE_OK);
    snprintf(sql, sizeof sql, "VACUUM INTO '%s'", probe);
    EXPECT_NE(sqlite3_exec(test_db, sql, NULL, NULL, NULL), SQLITE_OK);
    EXPECT_NE(sqlite3_exec(test_db, "ATTACH 'x' || '.db' AS other", NULL, NULL, NULL),
              SQLITE_OK);
    FILE *f = fopen(probe, "rb");
    EXPECT_TRUE(f == NULL);
    if (f) { fclose(f); remove(probe); }

    EXPECT_NE(sqlite3_exec(test_db, "PRAGMA writable_schema = ON", NULL, NULL, NULL),
              SQLITE_OK);

    EXPECT_EQ(sqlite3_exec(test_db, "ATTACH ':memory:' AS scratch", NULL, NULL, NULL),
              SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(test_db, "DETACH scratch", NULL, NULL, NULL), SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(test_db, "VACUUM", NULL, NULL, NULL), SQLITE_OK);
    teardown_db();
}

/* ── Audit 11 ─────────────────────────────────────────────────────────── */

/* default_cache_size is cache_size by another name; locking_mode=EXCLUSIVE
 * starves every other connection to the file; a journal_mode other than WAL
 * drops the rollback journal or the WAL. Reading each stays legal. */
UTEST(hl_cap_db, journal_locking_and_default_cache_pragmas_cannot_be_set)
{
    setup_db();
    static const char *const refused[] = {
        "PRAGMA default_cache_size=100000", "PRAGMA main.default_cache_size = 1",
        "PRAGMA locking_mode=EXCLUSIVE", "PRAGMA main.locking_mode = 'exclusive'",
        "PRAGMA journal_mode=OFF", "PRAGMA journal_mode=memory",
        "PRAGMA journal_mode=DELETE", "PRAGMA journal_mode=truncate",
        "PRAGMA journal_mode=persist", "PRAGMA journal_mode=o",
        "PRAGMA journal_mode=''", "PRAGMA journal_mode=walx",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        EXPECT_NE(SQLITE_OK, sqlite3_exec(test_db, refused[i], NULL, NULL, NULL));
    static const char *const allowed[] = {
        "PRAGMA default_cache_size", "PRAGMA locking_mode",
        "PRAGMA locking_mode=NORMAL", "PRAGMA journal_mode",
        "PRAGMA journal_mode=WAL", "PRAGMA journal_mode=wal",
    };
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
        EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, allowed[i], NULL, NULL, NULL));
    teardown_db();
}

/* The agent's read-only gate: a flag pragma takes effect when it is prepared
 * and still passes sqlite3_stmt_readonly, so every pragma given an argument
 * is refused there - except the introspection ones naming what to describe. */
UTEST(hl_cap_db, agent_gate_refuses_pragma_setters)
{
    setup_db();
    hl_cap_db_refuse_txn_control(test_db, 1);
    static const char *const refused[] = {
        "PRAGMA foreign_keys=OFF", "PRAGMA query_only=1",
        "PRAGMA defer_foreign_keys=ON", "PRAGMA recursive_triggers=1",
        "PRAGMA ignore_check_constraints=1", "PRAGMA busy_timeout=0",
        "PRAGMA synchronous=OFF", "PRAGMA locking_mode=NORMAL",
        "PRAGMA case_sensitive_like=1", "PRAGMA user_version=7",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        sqlite3_stmt *st = NULL;
        EXPECT_NE(SQLITE_OK, sqlite3_prepare_v2(test_db, refused[i], -1, &st, NULL));
        sqlite3_finalize(st);
    }
    static const char *const allowed[] = {
        "PRAGMA foreign_keys", "PRAGMA table_info(users)",
        "PRAGMA index_list('users')", "PRAGMA integrity_check",
        "SELECT * FROM pragma_table_info('users')",
    };
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
        sqlite3_stmt *st = NULL;
        EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(test_db, allowed[i], -1, &st, NULL));
        sqlite3_finalize(st);
    }
    hl_cap_db_refuse_txn_control(test_db, 0);
    /* foreign_keys is still on: the refused setter never ran. */
    int64_t fk = -1;
    EXPECT_EQ(0, hl_cap_db_query(&test_cache, "PRAGMA foreign_keys", NULL, 0,
                                 count_cb, &fk, NULL));
    EXPECT_EQ(1, (int)fk);
    teardown_db();
}

UTEST(hl_cap_db, heap_limit_from_env)
{
    EXPECT_EQ(HL_DB_SQLITE_HARD_HEAP_LIMIT, hl_cap_db_heap_limit_from_env(NULL));
    EXPECT_EQ(HL_DB_SQLITE_HARD_HEAP_LIMIT, hl_cap_db_heap_limit_from_env(""));
    EXPECT_EQ(HL_DB_SQLITE_HARD_HEAP_LIMIT, hl_cap_db_heap_limit_from_env("lots"));
    EXPECT_EQ(0LL, hl_cap_db_heap_limit_from_env("0"));
    EXPECT_EQ((long long)4 << 30, hl_cap_db_heap_limit_from_env("4G"));
    EXPECT_EQ((long long)512 << 20, hl_cap_db_heap_limit_from_env("512m"));
    EXPECT_EQ(HL_DB_SQLITE_MIN_HEAP_LIMIT, hl_cap_db_heap_limit_from_env("1k"));
    /* A trailing B, as `hull cache prune --max-size` takes (audit 12). */
    EXPECT_EQ((long long)512 << 20, hl_cap_db_heap_limit_from_env("512MB"));
    EXPECT_EQ((long long)2 << 30, hl_cap_db_heap_limit_from_env("2gb"));
    EXPECT_EQ((long long)128 << 20, hl_cap_db_heap_limit_from_env("134217728B"));
    /* Out of range: refused (the default), never clamped to LLONG_MAX. */
    EXPECT_EQ(HL_DB_SQLITE_HARD_HEAP_LIMIT,
              hl_cap_db_heap_limit_from_env("99999999999999999999"));
    EXPECT_EQ(HL_DB_SQLITE_HARD_HEAP_LIMIT,
              hl_cap_db_heap_limit_from_env("9999999999999G"));
    EXPECT_EQ(HL_DB_SQLITE_HARD_HEAP_LIMIT, hl_cap_db_heap_limit_from_env("-1"));
    EXPECT_EQ(HL_DB_SQLITE_HARD_HEAP_LIMIT, hl_cap_db_heap_limit_from_env("5MBx"));
    EXPECT_EQ(HL_DB_SQLITE_HARD_HEAP_LIMIT, hl_cap_db_heap_limit_from_env("B"));
}

/* SQLite's temp directory: every candidate base is tried until one works - a
 * missing, relative, non-directory, unwritable or world-writable-without-
 * sticky base is skipped, not the end of the search - and the directory is a
 * fresh mkdtemp one, so a name planted beside it (the old fixed
 * hull-sqlite-<euid>) neither blocks it nor is shared (audit 12). */
UTEST(hl_cap_db, sqlite_temp_dir_tries_every_candidate)
{
    if (hl_host_is_windows()) { UTEST_SKIP("temp_store stays MEMORY on Windows"); }
    char root[HL_TEST_PATH_MAX];
    ASSERT_EQ(0, hl_test_path(root, sizeof root, "hull_sqltmp_%ld", (long)getpid()));
    char file[HL_TEST_PATH_MAX + 16], ro[HL_TEST_PATH_MAX + 16];
    char ww[HL_TEST_PATH_MAX + 16], good[HL_TEST_PATH_MAX + 16];
    char squat[HL_TEST_PATH_MAX + 64];
    snprintf(file, sizeof file, "%s/file", root);
    snprintf(ro, sizeof ro, "%s/ro", root);
    snprintf(ww, sizeof ww, "%s/ww", root);
    snprintf(good, sizeof good, "%s/good", root);
    snprintf(squat, sizeof squat, "%s/hull-sqlite-%lu", good,
             (unsigned long)geteuid());
    ASSERT_EQ(0, mkdir(root, 0700));
    FILE *f = fopen(file, "w");
    ASSERT_TRUE(f != NULL);
    fclose(f);
    ASSERT_EQ(0, mkdir(ro, 0500));
    ASSERT_EQ(0, mkdir(ww, 0700));
    ASSERT_EQ(0, chmod(ww, 0777));          /* world-writable, no sticky bit */
    ASSERT_EQ(0, mkdir(good, 0700));
    f = fopen(squat, "w");                   /* the old fixed name, squatted */
    ASSERT_TRUE(f != NULL);
    fclose(f);

    const char *cands[] = {
        NULL, "relative/tmp", "/no/such/hull/dir", file,
        geteuid() == 0 ? NULL : ro, ww, good,
    };
    size_t nc = sizeof cands / sizeof cands[0];
    char a[PATH_MAX], b[PATH_MAX];
    EXPECT_EQ((int)nc - 1, hl_hull_sqlite_temp_dir_make(cands, nc, a, sizeof a));
    EXPECT_EQ((int)nc - 1, hl_hull_sqlite_temp_dir_make(cands, nc, b, sizeof b));
    EXPECT_STRNE(a, b);                      /* per call / process, not shared */
    char good_real[PATH_MAX];
    ASSERT_TRUE(realpath(good, good_real) != NULL);
    char prefix[PATH_MAX + 32];
    snprintf(prefix, sizeof prefix, "%s/hull-sqlite-%lu-", good_real,
             (unsigned long)geteuid());
    EXPECT_EQ(0, strncmp(a, prefix, strlen(prefix)));
    struct stat st;
    ASSERT_EQ(0, lstat(a, &st));
    EXPECT_TRUE(S_ISDIR(st.st_mode));
    EXPECT_EQ(0700, (int)(st.st_mode & 0777));

    /* Nothing usable: -1, nothing made. */
    const char *none[] = { file, ww };
    char c[PATH_MAX];
    EXPECT_EQ(-1, hl_hull_sqlite_temp_dir_make(none, 2, c, sizeof c));

    rmdir(a);
    rmdir(b);
    remove(squat);
    rmdir(good);
    rmdir(ww);
    rmdir(ro);
    remove(file);
    rmdir(root);
}

/* busy_timeout sleeps the calling thread (the event loop) uncharged, and
 * synchronous below NORMAL drops the syncs WAL's crash safety rests on: both
 * are bounded for app SQL (audit 12). SQLite reads synchronous's numeric
 * value modulo 8 ((n + 1) & 7), so 7 means OFF too. */
UTEST(hl_cap_db, busy_timeout_and_synchronous_are_bounded)
{
    setup_db();
    static const char *const refused[] = {
        "PRAGMA busy_timeout=60000", "PRAGMA busy_timeout = 5001",
        "PRAGMA busy_timeout=2147483647", "PRAGMA busy_timeout=99999999999",
        "PRAGMA busy_timeout='1e9'",
        "PRAGMA synchronous=OFF", "PRAGMA synchronous=0",
        "PRAGMA synchronous=off", "PRAGMA synchronous=no",
        "PRAGMA synchronous=false", "PRAGMA synchronous=7",
        "PRAGMA synchronous=4", "PRAGMA main.synchronous = 0",
        "PRAGMA synchronous=-1",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        EXPECT_NE(SQLITE_OK, sqlite3_exec(test_db, refused[i], NULL, NULL, NULL));
    static const char *const allowed[] = {
        "PRAGMA busy_timeout", "PRAGMA busy_timeout=0",
        "PRAGMA busy_timeout=1000", "PRAGMA busy_timeout=5000",
        "PRAGMA busy_timeout=-1", "PRAGMA synchronous",
        "PRAGMA synchronous=NORMAL", "PRAGMA synchronous=FULL",
        "PRAGMA synchronous=extra", "PRAGMA synchronous=1",
        "PRAGMA synchronous=2", "PRAGMA synchronous=3",
    };
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
        EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, allowed[i], NULL, NULL, NULL));
    teardown_db();
}

/* The selector and the sandbox match a DSN scheme case-insensitively, so the
 * backend strips it that way too: "SQLITE://<path>" opens <path>, not a file
 * literally named "SQLITE:" + ... under the working directory (audit 12). */
UTEST(hl_cap_db, sqlite_scheme_is_stripped_case_insensitively)
{
    char path[HL_TEST_PATH_MAX];
    ASSERT_EQ(0, hl_test_path(path, sizeof path, "hull_scheme_%ld.db", (long)getpid()));
    remove(path);
    char dsn[HL_TEST_PATH_MAX + 16];
    snprintf(dsn, sizeof dsn, "SQLite://%s", path);
    HlDbHandle h = { .backend = &hl_db_backend_sqlite, .ctx = NULL };
    ASSERT_EQ(0, hl_db_backend_sqlite.open(&h.ctx, dsn, NULL));
    EXPECT_EQ(0, hl_db_exec(&h, "CREATE TABLE t(x)", NULL, 0) < 0);
    hl_db_backend_sqlite.close(&h);
    FILE *f = fopen(path, "rb");
    EXPECT_TRUE(f != NULL);
    if (f) fclose(f);
    remove(path);
    char side[HL_TEST_PATH_MAX + 8];
    snprintf(side, sizeof side, "%s-wal", path); remove(side);
    snprintf(side, sizeof side, "%s-shm", path); remove(side);
}

/* A file database under a low hard heap limit and a small page cache. The
 * authorizer refuses cache_size / temp_store to SQL, so the test lifts it to
 * set them and puts the guard back. */
static sqlite3 *open_spill_db(const char *path, const char *temp_store)
{
    sqlite3 *db = NULL;
    hl_cap_db_sqlite_setup();
    if (sqlite3_open(path, &db) != SQLITE_OK || hl_cap_db_init(db) != 0) {
        sqlite3_close(db);
        return NULL;
    }
    char sql[128];
    sqlite3_set_authorizer(db, NULL, NULL);
    /* Rollback journal, not WAL: this is about temp files, and a cosmo build
     * on Windows fails to grow a WAL's shm map (SQLITE_IOERR_SHMMAP). */
    snprintf(sql, sizeof sql, "PRAGMA journal_mode=DELETE; "
             "PRAGMA cache_size=-1024; PRAGMA temp_store=%s", temp_store);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    hl_cap_db_guard(db);
    return db;
}

static int spill_exec(sqlite3 *db, const char *sql)
{
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        fprintf(stderr, "spill: %.40s...: %s (%d)\n", sql, sqlite3_errmsg(db),
                sqlite3_extended_errcode(db));
    return rc;
}

#define SPILL_ROWS "60000"   /* x 300 bytes: ~18 MB, past the 8 MiB limit */
/* A sort SQLite cannot skip: every row comes back, and the key is an
 * expression no index (big_v, made below) provides. A count over an ordered
 * subquery would not sort at all - the optimizer drops that ORDER BY. */
#define SPILL_SORT "SELECT v FROM big ORDER BY substr(v, 2) DESC"

static int rows_cb(void *ctx, HlColumn *cols, int ncols)
{
    (void)cols; (void)ncols;
    (*(int64_t *)ctx)++;
    return 0;
}

/* Audit 11: under temp_store=MEMORY the sorter never spilled, so a sort,
 * CREATE INDEX or VACUUM bigger than the process-wide hard heap limit failed
 * (a migration's CREATE INDEX stopped the app starting). With temp_store=FILE
 * each spills to the temp directory and succeeds under the same limit. */
UTEST(hl_cap_db, big_sorts_and_index_builds_spill_under_a_low_heap_limit)
{
    if (!hl_cap_db_temp_on_disk())
        UTEST_SKIP("no SQLite temp dir on this host (Windows): temp stays in memory");
    /* Hull's own connections get temp_store=FILE. */
    {
        sqlite3 *probe = NULL;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &probe));
        ASSERT_EQ(0, hl_cap_db_init(probe));
        int64_t ts = -1;
        HlStmtCache c;
        hl_stmt_cache_init(&c, probe, NULL);
        EXPECT_EQ(0, hl_cap_db_query(&c, "PRAGMA temp_store", NULL, 0,
                                     count_cb, &ts, NULL));
        EXPECT_EQ(1, (int)ts);   /* FILE */
        hl_stmt_cache_destroy(&c);
        sqlite3_close(probe);
    }
    char path[HL_TEST_PATH_MAX];
    ASSERT_EQ(0, hl_test_path(path, sizeof path, "hull_spill_%ld.db", (long)getpid()));
    remove(path);

    sqlite3 *db = open_spill_db(path, "FILE");
    ASSERT_TRUE(db != NULL);
    EXPECT_EQ(SQLITE_OK, spill_exec(db,
        "CREATE TABLE big(v TEXT);"
        "BEGIN;"
        "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < "
        SPILL_ROWS ") INSERT INTO big SELECT hex(randomblob(150)) FROM n;"
        "COMMIT;"));

    hl_cap_db_set_heap_limit((long long)8 << 20);
    int64_t n = -1;
    HlStmtCache cache;
    hl_stmt_cache_init(&cache, db, NULL);
    n = 0;
    EXPECT_EQ(0, hl_cap_db_query(&cache, SPILL_SORT, NULL, 0, rows_cb, &n, NULL));
    EXPECT_EQ(60000, (int)n);
    hl_stmt_cache_destroy(&cache);
    EXPECT_EQ(SQLITE_OK, spill_exec(db, "SELECT DISTINCT v FROM big"));
    EXPECT_EQ(SQLITE_OK, spill_exec(db, "CREATE INDEX big_v ON big(v)"));
    EXPECT_EQ(SQLITE_OK, spill_exec(db, "VACUUM"));
    sqlite3_close(db);

    /* The same sort with temp in memory runs out of the same limit. */
    db = open_spill_db(path, "MEMORY");
    ASSERT_TRUE(db != NULL);
    EXPECT_EQ(SQLITE_OK, spill_exec(db, "SELECT count(*) FROM big"));
    EXPECT_EQ(SQLITE_NOMEM, sqlite3_exec(db, SPILL_SORT, NULL, NULL, NULL));
    sqlite3_close(db);

    hl_cap_db_set_heap_limit(HL_DB_SQLITE_HARD_HEAP_LIMIT);
    remove(path);
    char side[HL_TEST_PATH_MAX + 16];
    snprintf(side, sizeof side, "%s-journal", path); remove(side);
}

/* Audit 11: the stale-transaction guard checks its ROLLBACK took. Here the
 * agent gate (which refuses transaction control) stands in for a ROLLBACK
 * that fails: the guard reports the connection still in the transaction, and
 * the SQLite backend replaces the connection - which rolls it back. */
UTEST(hl_cap_db, stale_txn_guard_replaces_a_connection_it_cannot_roll_back)
{
    char path[HL_TEST_PATH_MAX];
    ASSERT_EQ(0, hl_test_path(path, sizeof path, "hull_stale_%ld.db", (long)getpid()));
    remove(path);

    HlDbHandle h = { .backend = &hl_db_backend_sqlite, .ctx = NULL };
    ASSERT_EQ(0, hl_db_backend_sqlite.open(&h.ctx, path, NULL));
    ASSERT_EQ(0, hl_db_exec(&h, "CREATE TABLE t(x)", NULL, 0) < 0);
    ASSERT_EQ(0, hl_db_exec(&h, "BEGIN", NULL, 0) < 0);
    ASSERT_EQ(0, hl_db_exec(&h, "INSERT INTO t VALUES (1)", NULL, 0) < 0);

    sqlite3 *old = hl_db_sqlite_raw(&h);
    EXPECT_EQ(0, hl_cap_db_guard_stale_txn(NULL));
    hl_cap_db_refuse_txn_control(old, 1);
    EXPECT_EQ(-1, hl_cap_db_guard_stale_txn(old));
    EXPECT_FALSE(sqlite3_get_autocommit(old));

    hl_db_guard_stale_txn(&h);
    EXPECT_EQ(0, hl_db_in_txn(&h));
    EXPECT_TRUE(hl_db_sqlite_raw(&h) != NULL);
    int64_t n = -1;
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM t", NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(0, (int)n);
    /* The new connection is guarded and usable. */
    EXPECT_NE(SQLITE_OK, sqlite3_exec(hl_db_sqlite_raw(&h), "PRAGMA cache_size=1",
                                      NULL, NULL, NULL));
    EXPECT_EQ(0, hl_db_exec(&h, "INSERT INTO t VALUES (2)", NULL, 0) < 0);

    /* A plain stale transaction is just rolled back on the same connection. */
    sqlite3 *now = hl_db_sqlite_raw(&h);
    ASSERT_EQ(0, hl_db_exec(&h, "BEGIN", NULL, 0) < 0);
    EXPECT_EQ(0, hl_cap_db_guard_stale_txn(now));
    EXPECT_TRUE(sqlite3_get_autocommit(now));

    hl_db_backend_sqlite.close(&h);
    remove(path);
    char side[HL_TEST_PATH_MAX + 8];
    snprintf(side, sizeof side, "%s-wal", path); remove(side);
    snprintf(side, sizeof side, "%s-shm", path); remove(side);
}

/* An in-memory database cannot be reopened (that would drop it), so a
 * connection stuck in a transaction it cannot roll back is marked broken:
 * every call fails with a clear message instead of running inside the stuck
 * transaction, the raw handle is withheld, and no wait is refused for it. A
 * later check that finds the transaction gone puts it back (audit 12). */
UTEST(hl_cap_db, stale_txn_guard_breaks_a_connection_it_cannot_reopen)
{
    HlDbHandle h = { .backend = &hl_db_backend_sqlite, .ctx = NULL };
    ASSERT_EQ(0, hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL));
    ASSERT_EQ(0, hl_db_exec(&h, "CREATE TABLE t(x)", NULL, 0) < 0);
    ASSERT_EQ(0, hl_db_exec(&h, "BEGIN", NULL, 0) < 0);
    ASSERT_EQ(0, hl_db_exec(&h, "INSERT INTO t VALUES (1)", NULL, 0) < 0);

    sqlite3 *raw = hl_db_sqlite_raw(&h);
    ASSERT_TRUE(raw != NULL);
    hl_cap_db_refuse_txn_control(raw, 1);   /* the ROLLBACK cannot run */
    hl_db_guard_stale_txn(&h);

    EXPECT_TRUE(hl_db_sqlite_raw(&h) == NULL);
    EXPECT_TRUE(hl_db_sqlite_cache(&h) == NULL);
    EXPECT_EQ(0, hl_db_in_txn(&h));
    EXPECT_TRUE(hl_db_exec(&h, "INSERT INTO t VALUES (2)", NULL, 0) < 0);
    EXPECT_TRUE(strstr(hl_db_errmsg(&h), "stuck inside a transaction") != NULL);
    int64_t n = -1;
    EXPECT_NE(0, hl_db_query(&h, "SELECT count(*) FROM t", NULL, 0, count_cb, &n, NULL));
    EXPECT_NE(0, hl_db_begin(&h));
    /* Still broken on the next entry while the rollback keeps failing. */
    hl_db_guard_stale_txn(&h);
    EXPECT_TRUE(hl_db_sqlite_raw(&h) == NULL);

    /* Once the rollback can run, the next check leaves the transaction and
     * the connection works again - without the stuck transaction's row. */
    hl_cap_db_refuse_txn_control(raw, 0);
    hl_db_guard_stale_txn(&h);
    EXPECT_TRUE(hl_db_sqlite_raw(&h) == raw);
    n = -1;
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM t", NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(0, (int)n);
    hl_db_backend_sqlite.close(&h);
}

/* Running as root, SQLite fchown()s every WAL / journal / shm file it creates,
 * and the kernel sandbox grants no chown: on Linux the seccomp filter killed a
 * root process (a container) at its first write. Hull's setup replaces it. */
UTEST(hl_cap_db, sqlite_fchown_is_replaced)
{
    hl_cap_db_sqlite_setup();
    sqlite3_vfs *vfs = sqlite3_vfs_find("unix");
    ASSERT_TRUE(vfs != NULL);
    ASSERT_TRUE(vfs->iVersion >= 3 && vfs->xGetSystemCall != NULL);
    sqlite3_syscall_ptr cur = vfs->xGetSystemCall(vfs, "fchown");
    EXPECT_TRUE(cur != NULL);
    EXPECT_TRUE(cur != (sqlite3_syscall_ptr)fchown);
}

/* ── Audit 29 ─────────────────────────────────────────────────────────── */

static int64_t seq_of(const char *name)
{
    sqlite3_stmt *st = NULL;
    int64_t v = -1;
    if (sqlite3_prepare_v2(test_db, "SELECT seq FROM sqlite_sequence WHERE name = ?",
                           -1, &st, NULL) != SQLITE_OK)
        return -2;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* App SQL may not write SQLite's bookkeeping tables: a forged
 * sqlite_sequence.seq made every later AUTOINCREMENT insert fail SQLITE_FULL.
 * The statements SQLite runs itself (DROP TABLE / ANALYZE deleting rows,
 * ALTER TABLE ... RENAME renaming the sequence row) keep working. */
UTEST(hl_cap_db, sqlite_bookkeeping_tables_are_not_writable)
{
    setup_db();
    ASSERT_EQ(SQLITE_OK, sqlite3_exec(test_db,
        "INSERT INTO users(name) VALUES ('a');"
        "CREATE INDEX users_age ON users(age); ANALYZE", NULL, NULL, NULL));
    EXPECT_EQ(1, (int)seq_of("users"));
    static const char *const refused[] = {
        "UPDATE sqlite_sequence SET seq = 9223372036854775807",
        "UPDATE sqlite_sequence SET name = 'other' WHERE name = 'users'",
        "INSERT INTO sqlite_sequence(name, seq) VALUES ('_hull_jobs', 9223372036854775807)",
        "INSERT OR REPLACE INTO main.sqlite_sequence VALUES ('users', 9223372036854775807)",
        "UPDATE sqlite_stat1 SET stat = '1 1'",
        "INSERT INTO sqlite_stat1 VALUES ('users', 'users_age', '1000000 1')",
        "ALTER TABLE users ADD COLUMN z; UPDATE sqlite_sequence SET name = 'x'",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        EXPECT_NE(SQLITE_OK, sqlite3_exec(test_db, refused[i], NULL, NULL, NULL));
    EXPECT_EQ(1, (int)seq_of("users"));
    EXPECT_EQ(-1, (int)seq_of("x"));
    /* SQLite's own nested statements still run. */
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, "ALTER TABLE users RENAME TO people",
                                      NULL, NULL, NULL));
    EXPECT_EQ(1, (int)seq_of("people"));
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, "ANALYZE", NULL, NULL, NULL));
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, "DROP TABLE people", NULL, NULL, NULL));
    EXPECT_EQ(-1, (int)seq_of("people"));
    teardown_db();
}

static int64_t count_of(const char *sql)
{
    sqlite3_stmt *st = NULL;
    int64_t v = -1;
    if (sqlite3_prepare_v2(test_db, sql, -1, &st, NULL) != SQLITE_OK)
        return -2;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Audit 29 follow-up: an app DELETE of sqlite_sequence rows made the
 * _hull_jobs / _hull_job_events ids restart after a purge (a jobs subscriber
 * whose cursor was past them missed events), so app SQL may not DELETE from
 * sqlite_sequence / sqlite_stat* either - in a script, through the statement
 * cache, from a CTE, or from a trigger the app defines on its own table. The
 * DELETEs SQLite runs itself (DROP TABLE / DROP INDEX / ANALYZE t) still run,
 * and one statement's DROP does not let the next statement through. */
UTEST(hl_cap_db, sqlite_bookkeeping_delete_refused)
{
    setup_db();
    ASSERT_EQ(0, hl_cap_db_exec_script(test_db,
        "INSERT INTO users(name) VALUES ('a'), ('b');"
        "CREATE INDEX users_age ON users(age); ANALYZE;"
        "CREATE TABLE log(id INTEGER PRIMARY KEY AUTOINCREMENT, v);"
        "INSERT INTO log(v) VALUES (1), (2), (3);"));
    EXPECT_EQ(2, (int)seq_of("users"));
    EXPECT_EQ(3, (int)seq_of("log"));
    int64_t stats = count_of("SELECT count(*) FROM sqlite_stat1");
    EXPECT_TRUE(stats > 0);

    static const char *const refused[] = {
        "DELETE FROM sqlite_sequence",
        "DELETE FROM sqlite_sequence WHERE name = 'log'",
        "DELETE FROM main.sqlite_sequence",
        "delete from SQLITE_SEQUENCE where seq > 0",
        "WITH c(n) AS (SELECT 'log') DELETE FROM sqlite_sequence WHERE name IN c",
        "DELETE FROM sqlite_sequence WHERE name IN (SELECT name FROM sqlite_master)",
        "DELETE FROM sqlite_sequence RETURNING *",
        "DELETE FROM sqlite_stat1",
        "DELETE FROM main.sqlite_stat1 WHERE tbl = 'users'",
        "WITH c(t) AS (SELECT 'users') DELETE FROM sqlite_stat1 WHERE tbl IN c",
        "DROP TABLE sqlite_stat1",
        /* a covering DROP earlier in the same script does not carry over */
        "CREATE TABLE z(id INTEGER PRIMARY KEY AUTOINCREMENT); INSERT INTO z DEFAULT VALUES; DROP TABLE z; DELETE FROM sqlite_sequence",
        "DROP INDEX users_age; DELETE FROM sqlite_stat1",
        "ANALYZE users; DELETE FROM sqlite_stat1 WHERE tbl = 'users'",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        EXPECT_EQ(-1, hl_cap_db_exec_script(test_db, refused[i]));
        EXPECT_EQ(SQLITE_AUTH, sqlite3_errcode(test_db));
        /* The cached single-statement path refuses it too. */
        if (!strchr(refused[i], ';'))
            EXPECT_LT(hl_cap_db_exec(&test_cache, refused[i], NULL, 0), 0);
    }
    EXPECT_EQ(2, (int)seq_of("users"));
    EXPECT_EQ(3, (int)seq_of("log"));
    EXPECT_TRUE(count_of("SELECT count(*) FROM sqlite_stat1") > 0);

    /* A DROP prepared (never run) leaves nothing behind for the next one. */
    sqlite3_stmt *st = NULL;
    ASSERT_EQ(SQLITE_OK, hl_cap_db_prepare(test_db, "DROP TABLE log", -1, &st, NULL));
    sqlite3_finalize(st);
    st = NULL;
    EXPECT_EQ(SQLITE_AUTH, hl_cap_db_prepare(test_db, "DELETE FROM sqlite_sequence",
                                             -1, &st, NULL));
    EXPECT_TRUE(st == NULL);
    EXPECT_EQ(SQLITE_AUTH, hl_cap_db_prepare(test_db, "DELETE FROM sqlite_stat1",
                                             -1, &st, NULL));
    EXPECT_TRUE(st == NULL);

    /* A trigger the app defines on its own table: refused when the statement
     * that fires it is prepared, so the INSERT fails and nothing is deleted. */
    ASSERT_EQ(0, hl_cap_db_exec_script(test_db,
        "CREATE TRIGGER log_reset AFTER INSERT ON log BEGIN "
        "DELETE FROM sqlite_sequence WHERE name = 'log'; END;"
        "CREATE TRIGGER users_reset AFTER UPDATE ON users BEGIN "
        "DELETE FROM sqlite_stat1; END;"));
    EXPECT_EQ(-1, hl_cap_db_exec_script(test_db, "INSERT INTO log(v) VALUES (4)"));
    EXPECT_EQ(SQLITE_AUTH, sqlite3_errcode(test_db));
    EXPECT_LT(hl_cap_db_exec(&test_cache, "INSERT INTO log(v) VALUES (4)", NULL, 0), 0);
    EXPECT_EQ(-1, hl_cap_db_exec_script(test_db, "UPDATE users SET age = 1"));
    EXPECT_EQ(3, (int)seq_of("log"));
    EXPECT_EQ(3, (int)count_of("SELECT count(*) FROM log"));
    EXPECT_TRUE(count_of("SELECT count(*) FROM sqlite_stat1") > 0);
    /* Even raw sqlite3_exec (no hl_cap_db_prepare) refuses the trigger. */
    EXPECT_NE(SQLITE_OK, sqlite3_exec(test_db, "INSERT INTO log(v) VALUES (4)",
                                      NULL, NULL, NULL));
    ASSERT_EQ(0, hl_cap_db_exec_script(test_db,
        "DROP TRIGGER log_reset; DROP TRIGGER users_reset"));
    teardown_db();
}

/* The DELETEs SQLite runs itself while it drops, analyzes, vacuums or renames
 * keep working, through both prepare paths. */
UTEST(hl_cap_db, sqlite_bookkeeping_internal_deletes_allowed)
{
    setup_db();
    ASSERT_EQ(0, hl_cap_db_exec_script(test_db,
        "INSERT INTO users(name, age) VALUES ('a', 1), ('b', 2);"
        "CREATE INDEX users_age ON users(age);"
        "CREATE TABLE log(id INTEGER PRIMARY KEY AUTOINCREMENT, v);"
        "CREATE INDEX log_v ON log(v);"
        "INSERT INTO log(v) VALUES (1), (2);"
        "CREATE TRIGGER log_t AFTER INSERT ON log BEGIN SELECT 1; END;"));
    /* ANALYZE, then ANALYZE <table> / <index> (their DELETE of the old rows
     * comes before their own authorizer callback). */
    EXPECT_EQ(0, hl_cap_db_exec_script(test_db, "ANALYZE"));
    EXPECT_EQ(0, hl_cap_db_exec_script(test_db, "ANALYZE users"));
    EXPECT_GE(hl_cap_db_exec(&test_cache, "ANALYZE log", NULL, 0), 0);
    EXPECT_GE(hl_cap_db_exec(&test_cache, "ANALYZE log", NULL, 0), 0); /* cached */
    EXPECT_EQ(0, hl_cap_db_exec_script(test_db, "ANALYZE users_age"));
    EXPECT_EQ(0, hl_cap_db_exec_script(test_db, "PRAGMA optimize"));
    EXPECT_EQ(0, hl_cap_db_exec_script(test_db, "VACUUM"));
    /* RENAME of an AUTOINCREMENT table moves its sequence row. */
    EXPECT_EQ(0, hl_cap_db_exec_script(test_db, "ALTER TABLE log RENAME TO journal"));
    EXPECT_EQ(2, (int)seq_of("journal"));
    /* DROP INDEX / DROP TABLE (with a trigger, after ANALYZE) delete their
     * stat and sequence rows. */
    EXPECT_EQ(0, hl_cap_db_exec_script(test_db, "DROP INDEX log_v"));
    EXPECT_EQ(0, (int)count_of("SELECT count(*) FROM sqlite_stat1 WHERE idx = 'log_v'"));
    EXPECT_GE(hl_cap_db_exec(&test_cache, "DROP TABLE journal", NULL, 0), 0);
    EXPECT_EQ(-1, (int)seq_of("journal"));
    EXPECT_EQ(0, (int)count_of("SELECT count(*) FROM sqlite_stat1 WHERE tbl = 'journal'"));
    /* A migration-shaped script: CREATE ... AUTOINCREMENT, use it, DROP it. */
    EXPECT_EQ(0, hl_cap_db_exec_script(test_db,
        "CREATE TABLE tmp(id INTEGER PRIMARY KEY AUTOINCREMENT, v);"
        "INSERT INTO tmp(v) VALUES (1);"
        "INSERT INTO users(name) SELECT 'm' || v FROM tmp;"
        "DROP TABLE tmp;"));
    EXPECT_EQ(-1, (int)seq_of("tmp"));
    /* A TEMP AUTOINCREMENT table. */
    EXPECT_EQ(0, hl_cap_db_exec_script(test_db,
        "CREATE TEMP TABLE t(id INTEGER PRIMARY KEY AUTOINCREMENT);"
        "INSERT INTO t DEFAULT VALUES; DROP TABLE t;"));
    EXPECT_EQ(3, (int)count_of("SELECT seq FROM main.sqlite_sequence WHERE name = 'users'"));
    teardown_db();
}

/* The agent's read-only gate checks argument-less pragmas too (PRAGMA
 * optimize runs ANALYZE and writes) and refuses ATTACH of any name, DETACH
 * and ANALYZE. */
UTEST(hl_cap_db, agent_gate_refuses_bare_pragmas_attach_and_analyze)
{
    setup_db();
    hl_cap_db_refuse_txn_control(test_db, 1);
    static const char *const refused[] = {
        "PRAGMA optimize", "PRAGMA shrink_memory", "PRAGMA wal_checkpoint",
        "PRAGMA incremental_vacuum", "ATTACH ':memory:' AS m", "ATTACH '' AS t",
        "DETACH temp", "ANALYZE", "PRAGMA main.optimize",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        sqlite3_stmt *st = NULL;
        EXPECT_NE(SQLITE_OK, sqlite3_prepare_v2(test_db, refused[i], -1, &st, NULL));
        sqlite3_finalize(st);
    }
    static const char *const allowed[] = {
        "PRAGMA user_version", "PRAGMA page_count", "PRAGMA table_info(users)",
        "PRAGMA quick_check",
    };
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
        sqlite3_stmt *st = NULL;
        EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(test_db, allowed[i], -1, &st, NULL));
        sqlite3_finalize(st);
    }
    hl_cap_db_refuse_txn_control(test_db, 0);
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(test_db, "ATTACH ':memory:' AS m; DETACH m",
                                      NULL, NULL, NULL));
    teardown_db();
}

UTEST_MAIN();
