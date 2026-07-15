/*
 * test_hull_cap_db.c — Tests for shared database capability
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
#ifdef HL_ENABLE_WASM
#include "hull/cap/db_udf.h"
#endif
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>

/* ── Test fixtures ──────────────────────────────────────────────────── */

static sqlite3 *test_db = NULL;
static HlStmtCache test_cache;

static void setup_db(void)
{
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

    /* Execute the same SQL twice — second should hit cache */
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

UTEST(hl_cap_db, namespace_check_blocks_hull_tables)
{
    ASSERT_EQ(hl_cap_db_check_namespace("SELECT * FROM _hull_outbox"), HL_DB_ERR_DENIED);
    ASSERT_EQ(hl_cap_db_check_namespace("DROP TABLE _hull_migrations"), HL_DB_ERR_DENIED);
    ASSERT_EQ(hl_cap_db_check_namespace("INSERT INTO _HULL_OUTBOX VALUES(1)"), HL_DB_ERR_DENIED);
    ASSERT_EQ(hl_cap_db_check_namespace("SELECT * FROM users"), HL_DB_OK);
    ASSERT_EQ(hl_cap_db_check_namespace("SELECT * FROM hull_data"), HL_DB_OK);
    ASSERT_EQ(hl_cap_db_check_namespace(NULL), HL_DB_ERR_DENIED);
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

    /* Should fail — function no longer exists */
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
    /* Wrap the test sqlite3* in a transient HlDbHandle — the UDF API now
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

UTEST_MAIN();
