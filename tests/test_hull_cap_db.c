/*
 * test_hull_cap_db.c — Tests for shared database capability
 *
 * Uses utest.h (from Keel vendor) for the test framework.
 * Tests run against an in-memory SQLite database.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/hull_cap.h"
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>

/* ── Test fixtures ──────────────────────────────────────────────────── */

static sqlite3 *test_db = NULL;

static void setup_db(void)
{
    sqlite3_open(":memory:", &test_db);
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

    int rc = hl_cap_db_exec(test_db,
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
    hl_cap_db_exec(test_db,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);

    HlValue p2[] = {
        { .type = HL_TYPE_TEXT, .s = "Bob", .len = 3 },
        { .type = HL_TYPE_INT, .i = 25 },
        { .type = HL_TYPE_DOUBLE, .d = 87.0 },
    };
    hl_cap_db_exec(test_db,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p2, 3);

    /* Update all ages to 99 */
    HlValue p3[] = {
        { .type = HL_TYPE_INT, .i = 99 },
    };
    int changes = hl_cap_db_exec(test_db,
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
    hl_cap_db_exec(test_db,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);

    QueryResult result = { .count = 0 };
    int rc = hl_cap_db_query(test_db,
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
    hl_cap_db_exec(test_db,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);

    HlValue p2[] = {
        { .type = HL_TYPE_TEXT, .s = "Bob", .len = 3 },
        { .type = HL_TYPE_INT, .i = 25 },
        { .type = HL_TYPE_DOUBLE, .d = 87.0 },
    };
    hl_cap_db_exec(test_db,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p2, 3);

    /* Query with param filter */
    HlValue filter[] = {
        { .type = HL_TYPE_INT, .i = 28 },
    };
    QueryResult result = { .count = 0 };
    int rc = hl_cap_db_query(test_db,
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
    int rc = hl_cap_db_exec(test_db,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p1, 3);
    ASSERT_GE(rc, 0);

    QueryResult result = { .count = 0 };
    rc = hl_cap_db_query(test_db,
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
    hl_cap_db_exec(test_db,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p, 3);

    int64_t id = hl_cap_db_last_id(test_db);
    ASSERT_EQ(id, 1);

    hl_cap_db_exec(test_db,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p, 3);

    id = hl_cap_db_last_id(test_db);
    ASSERT_EQ(id, 2);

    teardown_db();
}

UTEST(hl_cap_db, null_db)
{
    int rc = hl_cap_db_query(NULL, "SELECT 1", NULL, 0, collect_rows, NULL, NULL);
    ASSERT_EQ(rc, -1);

    rc = hl_cap_db_exec(NULL, "SELECT 1", NULL, 0);
    ASSERT_EQ(rc, -1);
}

UTEST(hl_cap_db, null_sql)
{
    setup_db();

    int rc = hl_cap_db_query(test_db, NULL, NULL, 0, collect_rows, NULL, NULL);
    ASSERT_EQ(rc, -1);

    rc = hl_cap_db_exec(test_db, NULL, NULL, 0);
    ASSERT_EQ(rc, -1);

    teardown_db();
}

UTEST(hl_cap_db, invalid_sql)
{
    setup_db();

    QueryResult result = { .count = 0 };
    int rc = hl_cap_db_query(test_db,
        "SELECT * FROM nonexistent_table", NULL, 0,
        collect_rows, &result, NULL);
    ASSERT_EQ(rc, -1);

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
    int rc = hl_cap_db_exec(test_db,
        "INSERT INTO users (name, age, score) VALUES (?, ?, ?)", p, 3);
    ASSERT_GE(rc, 0);

    QueryResult result = { .count = 0 };
    rc = hl_cap_db_query(test_db,
        "SELECT name, age FROM users", NULL, 0,
        collect_rows, &result, NULL);

    ASSERT_EQ(rc, 0);
    ASSERT_EQ(result.count, 1);
    ASSERT_EQ(result.ages[0], 1); /* bool true → 1 */

    teardown_db();
}

UTEST_MAIN();
