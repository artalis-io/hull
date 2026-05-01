/*
 * test_db_backend.c — Tests for the database backend vtable abstraction
 *
 * Exercises the HlDbBackend vtable and HlDbHandle inline wrappers
 * using the SQLite backend against an in-memory database.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db.h"
#include "hull/cap/types.h"
#include <sqlite3.h>
#include <string.h>

/* ── Row callback helpers ───────────────────────────────────────────── */

static int count_rows_cb(void *ctx, HlColumn *cols, int ncols)
{
    (void)cols;
    (void)ncols;
    int *count = (int *)ctx;
    (*count)++;
    return 0;
}

typedef struct {
    int     count;
    char    vals[10][64];
    int64_t ints[10];
} RowResult;

static int collect_vals_cb(void *ctx, HlColumn *cols, int ncols)
{
    RowResult *r = (RowResult *)ctx;
    if (r->count >= 10)
        return 1;

    for (int i = 0; i < ncols; i++) {
        if (strcmp(cols[i].name, "val") == 0 &&
            cols[i].value.type == HL_TYPE_TEXT) {
            size_t len = cols[i].value.len < 63 ? cols[i].value.len : 63;
            memcpy(r->vals[r->count], cols[i].value.s, len);
            r->vals[r->count][len] = '\0';
        }
        if (strcmp(cols[i].name, "n") == 0 &&
            cols[i].value.type == HL_TYPE_INT) {
            r->ints[r->count] = cols[i].value.i;
        }
    }
    r->count++;
    return 0;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

UTEST(db_backend, sqlite_open_close)
{
    void *ctx = NULL;
    int rc = hl_db_backend_sqlite.open(&ctx, ":memory:", NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(ctx != NULL);

    hl_db_backend_sqlite.close(ctx);
}

UTEST(db_backend, sqlite_exec_via_vtable)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    int rc = hl_db_exec(&h,
        "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)", NULL, 0);
    ASSERT_EQ(rc, 0);

    HlValue params[] = {
        { .type = HL_TYPE_TEXT, .s = "hello", .len = 5 },
    };
    int changes = hl_db_exec(&h,
        "INSERT INTO t (name) VALUES (?)", params, 1);
    ASSERT_EQ(changes, 1);

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST(db_backend, sqlite_query_via_vtable)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    hl_db_exec(&h,
        "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)", NULL, 0);

    HlValue p1[] = {
        { .type = HL_TYPE_TEXT, .s = "foo", .len = 3 },
    };
    hl_db_exec(&h, "INSERT INTO t (val) VALUES (?)", p1, 1);

    HlValue p2[] = {
        { .type = HL_TYPE_TEXT, .s = "bar", .len = 3 },
    };
    hl_db_exec(&h, "INSERT INTO t (val) VALUES (?)", p2, 1);

    RowResult result = { .count = 0 };
    int rc = hl_db_query(&h,
        "SELECT val FROM t ORDER BY id", NULL, 0,
        collect_vals_cb, &result, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(result.count, 2);
    ASSERT_STREQ(result.vals[0], "foo");
    ASSERT_STREQ(result.vals[1], "bar");

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST(db_backend, sqlite_transaction_rollback)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    hl_db_exec(&h,
        "CREATE TABLE t (id INTEGER PRIMARY KEY)", NULL, 0);

    /* Begin, insert, rollback — no rows */
    ASSERT_EQ(hl_db_begin(&h), 0);
    hl_db_exec(&h, "INSERT INTO t (id) VALUES (1)", NULL, 0);
    ASSERT_EQ(hl_db_rollback(&h), 0);

    RowResult result = { .count = 0 };
    hl_db_query(&h,
        "SELECT COUNT(*) AS n FROM t", NULL, 0,
        collect_vals_cb, &result, NULL);
    ASSERT_EQ(result.count, 1);
    ASSERT_EQ(result.ints[0], 0);

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST(db_backend, sqlite_transaction_commit)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    hl_db_exec(&h,
        "CREATE TABLE t (id INTEGER PRIMARY KEY)", NULL, 0);

    /* Begin, insert, commit — 1 row */
    ASSERT_EQ(hl_db_begin(&h), 0);
    hl_db_exec(&h, "INSERT INTO t (id) VALUES (1)", NULL, 0);
    ASSERT_EQ(hl_db_commit(&h), 0);

    RowResult result = { .count = 0 };
    hl_db_query(&h,
        "SELECT COUNT(*) AS n FROM t", NULL, 0,
        collect_vals_cb, &result, NULL);
    ASSERT_EQ(result.count, 1);
    ASSERT_EQ(result.ints[0], 1);

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST(db_backend, sqlite_last_id)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    hl_db_exec(&h,
        "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)", NULL, 0);

    HlValue p[] = {
        { .type = HL_TYPE_TEXT, .s = "test", .len = 4 },
    };
    hl_db_exec(&h, "INSERT INTO t (name) VALUES (?)", p, 1);

    int64_t id = hl_db_last_id(&h);
    ASSERT_EQ(id, 1);

    hl_db_exec(&h, "INSERT INTO t (name) VALUES (?)", p, 1);

    id = hl_db_last_id(&h);
    ASSERT_EQ(id, 2);

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST(db_backend, sqlite_errmsg)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    /* Execute invalid SQL to trigger an error */
    int rc = hl_db_exec(&h, "INVALID SQL HERE", NULL, 0);
    ASSERT_TRUE(rc < 0);

    const char *msg = hl_db_errmsg(&h);
    ASSERT_TRUE(msg != NULL);
    ASSERT_TRUE(strlen(msg) > 0);

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST(db_backend, sqlite_raw_accessor)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    sqlite3 *raw = hl_db_sqlite_raw(&h);
    ASSERT_TRUE(raw != NULL);

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST(db_backend, sqlite_cache_accessor)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    HlStmtCache *cache = hl_db_sqlite_cache(&h);
    ASSERT_TRUE(cache != NULL);

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST(db_backend, null_handle_safety)
{
    /* All inline wrappers should handle NULL gracefully */
    ASSERT_EQ(hl_db_exec(NULL, "SELECT 1", NULL, 0), -1);
    ASSERT_EQ(hl_db_query(NULL, "SELECT 1", NULL, 0, count_rows_cb, NULL, NULL), -1);
    ASSERT_EQ(hl_db_begin(NULL), -1);
    ASSERT_EQ(hl_db_commit(NULL), -1);
    ASSERT_EQ(hl_db_rollback(NULL), -1);
    ASSERT_EQ(hl_db_last_id(NULL), -1);
    ASSERT_STREQ(hl_db_errmsg(NULL), "no database");

    /* guard_stale_txn should not crash on NULL */
    hl_db_guard_stale_txn(NULL);
}

UTEST(db_backend, null_backend_safety)
{
    /* Handle with NULL backend */
    HlDbHandle h = { .backend = NULL, .ctx = NULL };
    ASSERT_EQ(hl_db_exec(&h, "SELECT 1", NULL, 0), -1);
    ASSERT_EQ(hl_db_query(&h, "SELECT 1", NULL, 0, count_rows_cb, NULL, NULL), -1);
    ASSERT_EQ(hl_db_begin(&h), -1);
    ASSERT_EQ(hl_db_commit(&h), -1);
    ASSERT_EQ(hl_db_rollback(&h), -1);
    ASSERT_EQ(hl_db_last_id(&h), -1);
    ASSERT_STREQ(hl_db_errmsg(&h), "no database");

    hl_db_guard_stale_txn(&h);
}

UTEST(db_backend, guard_stale_txn)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    hl_db_exec(&h,
        "CREATE TABLE t (id INTEGER PRIMARY KEY)", NULL, 0);

    /* Guard should be a no-op when no transaction is active */
    hl_db_guard_stale_txn(&h);

    /* Start a transaction, insert, then guard should rollback */
    hl_db_begin(&h);
    hl_db_exec(&h, "INSERT INTO t (id) VALUES (1)", NULL, 0);
    hl_db_guard_stale_txn(&h);

    /* After guard, the transaction should be rolled back */
    RowResult result = { .count = 0 };
    hl_db_query(&h,
        "SELECT COUNT(*) AS n FROM t", NULL, 0,
        collect_vals_cb, &result, NULL);
    ASSERT_EQ(result.ints[0], 0);

    /* Should be able to begin a new transaction */
    ASSERT_EQ(hl_db_begin(&h), 0);
    hl_db_exec(&h, "INSERT INTO t (id) VALUES (1)", NULL, 0);
    ASSERT_EQ(hl_db_commit(&h), 0);

    result.count = 0;
    hl_db_query(&h,
        "SELECT COUNT(*) AS n FROM t", NULL, 0,
        collect_vals_cb, &result, NULL);
    ASSERT_EQ(result.ints[0], 1);

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST(db_backend, sqlite_raw_returns_null_for_wrong_backend)
{
    /* hl_db_sqlite_raw should return NULL for a non-SQLite handle */
    HlDbHandle h = { .backend = NULL, .ctx = NULL };
    sqlite3 *raw = hl_db_sqlite_raw(&h);
    ASSERT_TRUE(raw == NULL);

    HlStmtCache *cache = hl_db_sqlite_cache(&h);
    ASSERT_TRUE(cache == NULL);
}

UTEST(db_backend, vtable_name)
{
    ASSERT_STREQ(hl_db_backend_sqlite.name, "sqlite");
}

UTEST(db_backend, close_null_ctx)
{
    /* Closing NULL context should not crash */
    hl_db_backend_sqlite.close(NULL);
}

UTEST(db_backend, query_with_params)
{
    HlDbHandle h;
    h.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(hl_db_backend_sqlite.open(&h.ctx, ":memory:", NULL), 0);

    hl_db_exec(&h,
        "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)", NULL, 0);

    HlValue p1[] = {
        { .type = HL_TYPE_TEXT, .s = "alpha", .len = 5 },
    };
    hl_db_exec(&h, "INSERT INTO t (val) VALUES (?)", p1, 1);

    HlValue p2[] = {
        { .type = HL_TYPE_TEXT, .s = "beta", .len = 4 },
    };
    hl_db_exec(&h, "INSERT INTO t (val) VALUES (?)", p2, 1);

    /* Query with param filter */
    HlValue filter[] = {
        { .type = HL_TYPE_TEXT, .s = "alpha", .len = 5 },
    };
    RowResult result = { .count = 0 };
    int rc = hl_db_query(&h,
        "SELECT val FROM t WHERE val = ?", filter, 1,
        collect_vals_cb, &result, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(result.count, 1);
    ASSERT_STREQ(result.vals[0], "alpha");

    hl_db_backend_sqlite.close(h.ctx);
}

UTEST_MAIN();
