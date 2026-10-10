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
#endif
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>   /* getpid */
#include <limits.h>
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
    HlDbOpBudget b = { 200000, 0, 0 };
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
    HlDbOpBudget b = { 5000, 0, 0 };   /* ~320 KB of allocations */
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
    HlDbOpBudget b = { 100000000, 0, 0 };
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
    HlDbOpBudget b = { 1, 0, 1 };   /* tripped */
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

UTEST_MAIN();
