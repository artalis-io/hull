/*
 * test_db_duckdb.c - DuckDB backend smoke tests (HL_ENABLE_DUCKDB only)
 *
 * Exercises open :memory:, prepared-statement param binding, columnar chunk
 * decode to HlValue (int / text / double / bool / NULL), the dialect descriptor,
 * transactions, the security lockdown (external file access blocked +
 * configuration locked), and the dialect helpers (insert_if_absent / upsert /
 * table_columns). Self-gates to an empty program when DuckDB is not compiled, so
 * it is harmless in every other build flavor.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DUCKDB

#include "utest.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_duckdb.h"
#include "hull/cap/db_budget.h"
#include "hull/cap/types.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <duckdb.h>
#pragma GCC diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Open a fresh in-memory DuckDB handle. */
static int duck_open_mem(HlDbHandle *h)
{
    void *ctx = NULL;
    int rc = hl_db_backend_duckdb.open(&ctx, "duckdb://:memory:", NULL);
    h->backend = &hl_db_backend_duckdb;
    h->ctx = ctx;
    return rc;
}

typedef struct {
    int      nrows;
    int64_t  id;
    char     name[64];
    int      name_is_nil;
    double   score;
    int      ok;
} RowCapture;

static int capture_cb(void *ctx, HlColumn *cols, int ncols)
{
    RowCapture *c = ctx;
    for (int i = 0; i < ncols; i++) {
        const char *nm = cols[i].name ? cols[i].name : "";
        if (strcmp(nm, "id") == 0) {
            c->id = cols[i].value.i;
        } else if (strcmp(nm, "name") == 0) {
            if (cols[i].value.type == HL_TYPE_NIL) {
                c->name_is_nil = 1;
                c->name[0] = '\0';
            } else {
                size_t n = cols[i].value.len < sizeof c->name - 1
                    ? cols[i].value.len : sizeof c->name - 1;
                memcpy(c->name, cols[i].value.s, n);
                c->name[n] = '\0';
            }
        } else if (strcmp(nm, "score") == 0) {
            c->score = cols[i].value.d;
        } else if (strcmp(nm, "ok") == 0) {
            c->ok = cols[i].value.b;
        }
    }
    c->nrows++;
    return 0;
}

/* Single-column integer collector (for COUNT(*) checks). */
static int count_cb(void *ctx, HlColumn *cols, int ncols)
{
    (void)ncols;
    *(int64_t *)ctx = cols[0].value.i;
    return 0;
}

UTEST(db_duckdb, open_bind_decode_types)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);

    ASSERT_EQ(hl_db_exec(&h,
        "CREATE TABLE t (id INTEGER, name VARCHAR, score DOUBLE, ok BOOLEAN)",
        NULL, 0), 0);

    HlValue row1[4] = {
        { .type = HL_TYPE_INT,    .i = 7 },
        { .type = HL_TYPE_TEXT,   .s = "alice", .len = 5 },
        { .type = HL_TYPE_DOUBLE, .d = 1.5 },
        { .type = HL_TYPE_BOOL,   .b = 1 },
    };
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO t VALUES (?, ?, ?, ?)", row1, 4), 0);

    RowCapture c;
    memset(&c, 0, sizeof c);
    ASSERT_EQ(hl_db_query(&h, "SELECT id, name, score, ok FROM t",
                          NULL, 0, capture_cb, &c, NULL), 0);
    ASSERT_EQ(c.nrows, 1);
    ASSERT_EQ(c.id, 7);
    ASSERT_STREQ(c.name, "alice");
    ASSERT_NEAR(c.score, 1.5, 1e-9);
    ASSERT_EQ(c.ok, 1);

    h.backend->close(&h);
}

/* A bound NIL binds NULL; the column reads back as HL_TYPE_NIL. Also covers a
 * VARCHAR longer than the 12-byte inline threshold (pointer arm of
 * duckdb_string_t). */
UTEST(db_duckdb, null_bind_and_long_string)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "CREATE TABLE n (id INTEGER, name VARCHAR)", NULL, 0), 0);

    HlValue nul_row[2] = {
        { .type = HL_TYPE_INT, .i = 1 },
        { .type = HL_TYPE_NIL },
    };
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO n VALUES (?, ?)", nul_row, 2), 0);

    const char *longstr = "a-string-well-past-twelve-bytes";
    HlValue long_row[2] = {
        { .type = HL_TYPE_INT,  .i = 2 },
        { .type = HL_TYPE_TEXT, .s = longstr, .len = strlen(longstr) },
    };
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO n VALUES (?, ?)", long_row, 2), 0);

    RowCapture c;
    memset(&c, 0, sizeof c);
    ASSERT_EQ(hl_db_query(&h, "SELECT id, name FROM n WHERE id = 1",
                          NULL, 0, capture_cb, &c, NULL), 0);
    ASSERT_EQ(c.nrows, 1);
    ASSERT_TRUE(c.name_is_nil);

    memset(&c, 0, sizeof c);
    ASSERT_EQ(hl_db_query(&h, "SELECT id, name FROM n WHERE id = 2",
                          NULL, 0, capture_cb, &c, NULL), 0);
    ASSERT_EQ(c.nrows, 1);
    ASSERT_FALSE(c.name_is_nil);
    ASSERT_STREQ(c.name, longstr);

    h.backend->close(&h);
}

/* Every column of a row, as text ("" for NIL), copied in the row callback. */
typedef struct {
    int  nrows;
    int  ncols;
    char vals[8][64];
} TextRow;

static int text_row_cb(void *ctx, HlColumn *cols, int ncols)
{
    TextRow *t = ctx;
    t->ncols = ncols;
    for (int i = 0; i < ncols && i < 8; i++) {
        if (cols[i].value.type == HL_TYPE_TEXT) {
            size_t n = cols[i].value.len < sizeof t->vals[i] - 1
                ? cols[i].value.len : sizeof t->vals[i] - 1;
            memcpy(t->vals[i], cols[i].value.s, n);
            t->vals[i][n] = '\0';
        } else if (cols[i].value.type == HL_TYPE_INT) {
            snprintf(t->vals[i], sizeof t->vals[i], "%lld",
                     (long long)cols[i].value.i);
        } else {
            t->vals[i][0] = '\0';
        }
    }
    t->nrows++;
    return 0;
}

/* Strings of up to 12 bytes are stored inline in the vector's duckdb_string_t.
 * duck_decode copied that struct to its stack and pointed the value into the
 * copy, so once it returned every short column of a row read whatever the
 * stack slot held next - another column's value (audit 35). */
UTEST(db_duckdb, short_inline_strings_keep_their_own_value)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);

    TextRow t;
    memset(&t, 0, sizeof t);
    ASSERT_EQ(hl_db_query(&h,
        "SELECT 'x1' AS a, 42 AS n, 'y2' AS b, 'alice' AS c, "
        "'twelve-bytes' AS d, 'a-long-string-over-twelve' AS e, 'bob' AS f",
        NULL, 0, text_row_cb, &t, NULL), 0);
    ASSERT_EQ(t.nrows, 1);
    ASSERT_EQ(t.ncols, 7);
    ASSERT_STREQ(t.vals[0], "x1");
    ASSERT_STREQ(t.vals[1], "42");
    ASSERT_STREQ(t.vals[2], "y2");
    ASSERT_STREQ(t.vals[3], "alice");
    ASSERT_STREQ(t.vals[4], "twelve-bytes");
    ASSERT_STREQ(t.vals[5], "a-long-string-over-twelve");
    ASSERT_STREQ(t.vals[6], "bob");

    h.backend->close(&h);
}

/* BEGIN + INSERT + ROLLBACK leaves no row; a COMMITted one persists. */
UTEST(db_duckdb, transaction_commit_and_rollback)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "CREATE TABLE tx (v INTEGER)", NULL, 0), 0);

    HlValue v = { .type = HL_TYPE_INT, .i = 42 };

    ASSERT_EQ(hl_db_begin(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO tx VALUES (?)", &v, 1), 0);
    ASSERT_EQ(hl_db_rollback(&h), 0);

    int64_t n = -1;
    ASSERT_EQ(hl_db_query(&h, "SELECT COUNT(*) FROM tx", NULL, 0, count_cb, &n, NULL), 0);
    ASSERT_EQ(n, 0);

    ASSERT_EQ(hl_db_begin(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO tx VALUES (?)", &v, 1), 0);
    ASSERT_EQ(hl_db_commit(&h), 0);

    n = -1;
    ASSERT_EQ(hl_db_query(&h, "SELECT COUNT(*) FROM tx", NULL, 0, count_cb, &n, NULL), 0);
    ASSERT_EQ(n, 1);

    h.backend->close(&h);
}

/* A budget that is always over: every charge (and every 0-unit ask) trips. */
static int always_tripped(void *ud, int64_t units)
{
    (void)ud; (void)units;
    return 1;
}

/* The rollback Hull runs (a failed batch, the end of a db.async op) runs
 * unbound, as SQLite's does: charged to the tripped budget of the run it
 * cleans up after, it was interrupted and the transaction stayed open. And a
 * ROLLBACK that did not run leaves the transaction tracked as open
 * (audit 16). */
UTEST(db_duckdb, rollback_runs_after_the_budget_tripped)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "CREATE TABLE tr (v INTEGER)", NULL, 0), 0);
    HlValue v = { .type = HL_TYPE_INT, .i = 7 };
    ASSERT_EQ(hl_db_begin(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO tr VALUES (?)", &v, 1), 0);
    EXPECT_TRUE(hl_db_in_txn(&h));

    HlDbBudgetBinding prev = hl_db_budget_swap(always_tripped, NULL);
    /* The app's own ROLLBACK is never interrupted (a transaction statement
     * takes effect in a task DuckDB's threads may already hold, audit 36):
     * it runs and reports that it ended the transaction, and the cleanup
     * rollback after it finds none and still succeeds. */
    EXPECT_EQ(0, hl_db_exec(&h, "ROLLBACK", NULL, 0));
    EXPECT_FALSE(hl_db_in_txn(&h));
    EXPECT_EQ(0, hl_db_rollback(&h));
    EXPECT_FALSE(hl_db_in_txn(&h));
    hl_db_budget_restore(prev);
    ASSERT_EQ(hl_db_begin(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO tr VALUES (?)", &v, 1), 0);
    prev = hl_db_budget_swap(always_tripped, NULL);
    EXPECT_EQ(0, hl_db_rollback(&h));
    EXPECT_FALSE(hl_db_in_txn(&h));
    hl_db_budget_restore(prev);

    int64_t n = -1;
    ASSERT_EQ(hl_db_query(&h, "SELECT COUNT(*) FROM tr", NULL, 0, count_cb, &n, NULL), 0);
    EXPECT_EQ(n, 0);
    h.backend->close(&h);
}

/* Audit 36: duckdb_interrupt does not stop a task one of DuckDB's own
 * threads already took, and BEGIN / COMMIT / ROLLBACK take effect there, so a
 * COMMIT reported "interrupted" under a tripped budget had often committed.
 * A transaction statement now runs to its end and reports what it did: under
 * an always-tripped budget, every COMMIT succeeds and its row persists, every
 * app ROLLBACK succeeds and its row is gone, in_txn matches the connection's
 * real state, and the cleanup rollback that follows still succeeds. */
UTEST(db_duckdb, txn_statement_outcome_is_reported_under_a_tripped_budget)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "CREATE TABLE ta (v INTEGER)", NULL, 0), 0);
    HlValue v = { .type = HL_TYPE_INT, .i = 1 };
    int64_t expect = 0;
    int bad_commit = 0, bad_rollback = 0, bad_track = 0, bad_cleanup = 0;
    for (int k = 0; k < 2000; k++) {
        ASSERT_EQ(hl_db_begin(&h), 0);
        ASSERT_EQ(hl_db_exec(&h, "INSERT INTO ta VALUES (?)", &v, 1), 0);
        int commit = (k % 2) == 0;
        HlDbBudgetBinding prev = hl_db_budget_swap(always_tripped, NULL);
        int rc = hl_db_exec(&h, commit ? "COMMIT" : "ROLLBACK", NULL, 0);
        if (rc != 0) { if (commit) bad_commit++; else bad_rollback++; }
        if (hl_db_in_txn(&h)) bad_track++;
        /* The cleanup a batch or a job end runs after it. */
        if (hl_db_rollback(&h) != 0) bad_cleanup++;
        hl_db_budget_restore(prev);
        if (commit) expect++;
        /* Reality: no transaction is open (BEGIN would fail inside one). */
        if (hl_db_exec(&h, "BEGIN TRANSACTION", NULL, 0) != 0) bad_track++;
        else ASSERT_EQ(hl_db_rollback(&h), 0);
    }
    EXPECT_EQ(bad_commit, 0);
    EXPECT_EQ(bad_rollback, 0);
    EXPECT_EQ(bad_track, 0);
    EXPECT_EQ(bad_cleanup, 0);
    EXPECT_FALSE(hl_db_in_txn(&h));
    int64_t n = -1;
    ASSERT_EQ(hl_db_query(&h, "SELECT COUNT(*) FROM ta", NULL, 0, count_cb, &n, NULL), 0);
    EXPECT_EQ(n, expect);
    h.backend->close(&h);
}

/* The security keystone: external file access is off and the configuration is
 * locked, so neither reading a local file nor re-enabling access succeeds. */
UTEST(db_duckdb, security_lockdown)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);

    /* read_csv on a real local file must fail (enable_external_access=false). */
    int rc = hl_db_query(&h, "SELECT * FROM read_csv('/etc/hosts')",
                         NULL, 0, NULL, NULL, NULL);
    ASSERT_EQ(rc, -1);

    /* And the app cannot re-enable it: configuration is locked. */
    int rc2 = hl_db_exec(&h, "SET enable_external_access = true", NULL, 0);
    ASSERT_EQ(rc2, -1);

    h.backend->close(&h);
}

/* Audit 36: a catalog / schema qualifier hid `histogram` from the
 * table-position check, and DuckDB resolved main.histogram /
 * system.main.histogram to the table macro, reading a table whose name was
 * built at run time. Refused by the backend; a plain source still runs. */
UTEST(db_duckdb, qualified_table_macro_is_refused)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "CREATE TABLE users (name VARCHAR, n INTEGER)",
                         NULL, 0), 0);
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO users VALUES ('alice', 7)", NULL, 0), 0);

    static const char *const refused[] = {
        "SELECT * FROM main.histogram(lower('USERS'), n)",
        "SELECT * FROM system.main.histogram('us' || 'ers', n)",
        "SELECT * FROM main.\"histogram\"(concat('us', 'ers'), n)",
        "SELECT * FROM main . /* c */ histogram(lower('USERS'), n)",
        "SELECT * FROM main.pragma_table_info(lower('USERS'))",
        "SELECT * FROM system.main.query_table(lower('USERS'))",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        EXPECT_EQ(-1, hl_db_query(&h, refused[i], NULL, 0, NULL, NULL, NULL));

    int64_t c = 0;
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM main.histogram(users, n)",
                             NULL, 0, count_cb, &c, NULL));
    EXPECT_EQ(1, (int)c);
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM main.users",
                             NULL, 0, count_cb, &c, NULL));
    EXPECT_EQ(1, (int)c);
    h.backend->close(&h);
}

/* Audit 37: a bare name in a macro body is a parameter, filled at call time
 * with a value built at run time, so a macro wrapping histogram /
 * pragma_table_info / ... read any table by a constructed name. Creating one
 * is refused; a macro over a literal, or one that uses none of them, runs. */
UTEST(db_duckdb, macro_parameter_table_names_are_refused)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "CREATE TABLE _hull_x (name VARCHAR, checksum VARCHAR)",
                         NULL, 0), 0);
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO _hull_x VALUES ('s1', 'c1')", NULL, 0), 0);
    ASSERT_EQ(hl_db_exec(&h, "CREATE TABLE users (name VARCHAR, id INTEGER)",
                         NULL, 0), 0);
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO users VALUES ('bob', 1)", NULL, 0), 0);

    static const char *const refused[] = {
        "CREATE OR REPLACE TEMP MACRO hm(t, c) AS TABLE SELECT * FROM histogram(t, c)",
        "CREATE MACRO hp(t, c) AS TABLE SELECT * FROM histogram(t, c)",
        "CREATE OR REPLACE TEMP MACRO q3(t) AS (SELECT string_agg(bin, ',') FROM histogram(t, checksum))",
        "CREATE OR REPLACE TEMP MACRO tm(t) AS TABLE SELECT * FROM pragma_table_info(t)",
        "CREATE OR REPLACE TEMP MACRO sh(t) AS TABLE SELECT * FROM pragma_show(t)",
        "CREATE OR REPLACE TEMP MACRO sm(t) AS TABLE SELECT * FROM duckdb_table_sample(t)",
        "CREATE OR REPLACE TEMP MACRO hv(t) AS TABLE SELECT * FROM histogram_values(t, name)",
        "CREATE OR REPLACE TEMP FUNCTION fm(t) AS TABLE SELECT * FROM histogram(t, name)",
        "CREATE /* x */ TEMPORARY -- y\n MACRO h2(t, c) AS TABLE FROM main.histogram(t, c)",
        /* run-time values outside a macro */
        "SELECT * FROM histogram(getvariable('t'), name)",
        "SELECT * FROM pragma_table_info(getvariable('t'))",
        "PREPARE p3 AS SELECT * FROM histogram(?, name)",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        EXPECT_EQ(-1, hl_db_exec(&h, refused[i], NULL, 0));
    /* Nothing was created: the macros the reported attack called are absent. */
    EXPECT_NE(0, hl_db_query(&h, "SELECT * FROM hm('_hu' || 'll_x', name)",
                             NULL, 0, NULL, NULL, NULL));
    EXPECT_NE(0, hl_db_query(&h, "SELECT * FROM tm('_hu' || 'll_x')",
                             NULL, 0, NULL, NULL, NULL));

    /* Legitimate macros still work. */
    int64_t c = 0;
    EXPECT_EQ(0, hl_db_exec(&h, "CREATE OR REPLACE TEMP MACRO ok1(a) AS a + 1",
                            NULL, 0));
    EXPECT_EQ(0, hl_db_query(&h, "SELECT ok1(41)", NULL, 0, count_cb, &c, NULL));
    EXPECT_EQ(42, (int)c);
    EXPECT_EQ(0, hl_db_exec(&h,
        "CREATE OR REPLACE TEMP MACRO ok2() AS TABLE SELECT * FROM histogram('users', name)",
        NULL, 0));
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM ok2()", NULL, 0,
                             count_cb, &c, NULL));
    EXPECT_EQ(1, (int)c);
    EXPECT_EQ(0, hl_db_exec(&h,
        "CREATE OR REPLACE TEMP MACRO ok3(n) AS TABLE SELECT * FROM main.users WHERE id = n",
        NULL, 0));
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM main.ok3(1)", NULL, 0,
                             count_cb, &c, NULL));
    EXPECT_EQ(1, (int)c);
    EXPECT_EQ(0, hl_db_exec(&h,
        "CREATE OR REPLACE TEMP MACRO ok4(x) AS (SELECT histogram(lower(name)) FROM users)",
        NULL, 0));
    h.backend->close(&h);
}

/* Audit 37: a macro already stored in the file (made before the fix, or by
 * another program) is the same reach, so the open refuses a database that
 * holds one; a database whose macros are harmless opens. */
UTEST(db_duckdb, stored_table_name_macro_refuses_the_open)
{
    char path[] = "/tmp/hull_duck_macro_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    unlink(path);
    char dsn[128];
    snprintf(dsn, sizeof dsn, "duckdb://%s", path);
    char wal[160];
    snprintf(wal, sizeof wal, "%s.wal", path);

    duckdb_database db;
    duckdb_connection con;
    duckdb_result r;
    ASSERT_TRUE(duckdb_open(path, &db) == DuckDBSuccess);
    ASSERT_TRUE(duckdb_connect(db, &con) == DuckDBSuccess);
    ASSERT_TRUE(duckdb_query(con, "CREATE MACRO ok1(a) AS a + 1", &r) == DuckDBSuccess);
    duckdb_destroy_result(&r);
    duckdb_disconnect(&con);
    duckdb_close(&db);

    HlDbHandle h = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(hl_db_backend_duckdb.open(&h.ctx, dsn, NULL), 0);
    h.backend->close(&h);

    ASSERT_TRUE(duckdb_open(path, &db) == DuckDBSuccess);
    ASSERT_TRUE(duckdb_connect(db, &con) == DuckDBSuccess);
    ASSERT_TRUE(duckdb_query(con,
        "CREATE MACRO hp(t, c) AS TABLE SELECT * FROM histogram(t, c)", &r) ==
        DuckDBSuccess);
    duckdb_destroy_result(&r);
    duckdb_disconnect(&con);
    duckdb_close(&db);

    void *ctx = NULL;
    EXPECT_EQ(-1, hl_db_backend_duckdb.open(&ctx, dsn, NULL));
    EXPECT_TRUE(ctx == NULL);

    unlink(path);
    unlink(wal);
}

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* A budget that trips once the monotonic clock passes *(double *)ud. */
static int trips_at(void *ud, int64_t units)
{
    (void)units;
    return mono_ms() >= *(double *)ud;
}

/* Audit 38: sleep_ms / pg_sleep block inside one execution task, which
 * duckdb_interrupt does not stop - SELECT sleep_ms(5000) returned after 5 s
 * under a budget that tripped at 500 ms. They are refused before they run;
 * a budget trip still lands near its time for ordinary work; sequence names
 * and read_duckdb table names are literal-only. */
UTEST(db_duckdb, sleep_refused_and_trip_lands_on_time)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);

    static const char *const refused[] = {
        "SELECT sleep_ms(5000)",
        "SELECT main.sleep_ms(5000)",
        "SELECT \"sleep_ms\"(5000)",
        "SELECT pg_sleep(4)",
        "SELECT pg_catalog.pg_sleep(4)",
        "SELECT \"pg_catalog\".\"pg_sleep\"(4)",
        "CREATE MACRO zz(n) AS sleep_ms(n)",
        "CREATE VIEW vz AS SELECT pg_sleep(1)",
        "CREATE TABLE tz (a INT DEFAULT sleep_ms(1000))",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        double t0 = mono_ms();
        EXPECT_EQ(-1, hl_db_query(&h, refused[i], NULL, 0, NULL, NULL, NULL));
        EXPECT_LT(mono_ms() - t0, 500.0);
    }

    /* Ordinary long work is still stopped near the budget's time. */
    double deadline = mono_ms() + 300.0;
    HlDbBudgetBinding prev = hl_db_budget_swap(trips_at, &deadline);
    double t0 = mono_ms();
    int64_t c = 0;
    EXPECT_EQ(-1, hl_db_query(&h, "SELECT count(*) FROM range(10000000000)",
                              NULL, 0, count_cb, &c, NULL));
    double took = mono_ms() - t0;
    hl_db_budget_restore(prev);
    EXPECT_LT(took, 2000.0);

    /* Sequences: a literal name works, a run-time one is refused. */
    EXPECT_EQ(0, hl_db_exec(&h, "CREATE SEQUENCE s38", NULL, 0));
    EXPECT_EQ(0, hl_db_query(&h, "SELECT nextval('s38')", NULL, 0,
                             count_cb, &c, NULL));
    EXPECT_EQ(1, (int)c);
    EXPECT_EQ(0, hl_db_exec(&h,
        "CREATE TABLE o38 (id BIGINT DEFAULT nextval('s38') PRIMARY KEY, v INT)",
        NULL, 0));
    EXPECT_EQ(0, hl_db_exec(&h, "INSERT INTO o38 (v) VALUES (1)", NULL, 0));
    EXPECT_EQ(-1, hl_db_query(&h, "SELECT nextval(concat('s', '38'))", NULL, 0,
                              NULL, NULL, NULL));
    EXPECT_EQ(-1, hl_db_query(&h, "SELECT currval(lower('S38'))", NULL, 0,
                              NULL, NULL, NULL));
    h.backend->close(&h);
}

/* Audit 38: a view or a column DEFAULT stored in the file that calls
 * sleep_ms refuses the open, as a stored table-name macro does (audit 37);
 * a CHECK constraint too (audit 40). */
UTEST(db_duckdb, stored_sleep_view_or_default_refuses_the_open)
{
    static const char *const ddl[] = {
        "CREATE VIEW v AS SELECT sleep_ms(1000) AS x",
        "CREATE TABLE t (a INT DEFAULT sleep_ms(1000))",
        "CREATE MACRO m(n) AS pg_sleep(n)",
        /* audit 40: a CHECK constraint runs on every insert too */
        "CREATE TABLE tc (a INT CHECK (sleep_ms(1000) IS NULL OR a > 0))",
    };
    for (size_t k = 0; k < sizeof ddl / sizeof ddl[0]; k++) {
        char path[] = "/tmp/hull_duck_sleep_XXXXXX";
        int fd = mkstemp(path);
        ASSERT_TRUE(fd >= 0);
        close(fd);
        unlink(path);
        char dsn[128];
        snprintf(dsn, sizeof dsn, "duckdb://%s", path);
        char wal[160];
        snprintf(wal, sizeof wal, "%s.wal", path);

        duckdb_database db;
        duckdb_connection con;
        duckdb_result r;
        ASSERT_TRUE(duckdb_open(path, &db) == DuckDBSuccess);
        ASSERT_TRUE(duckdb_connect(db, &con) == DuckDBSuccess);
        ASSERT_TRUE(duckdb_query(con,
            "CREATE SEQUENCE sq; CREATE TABLE ok (id BIGINT DEFAULT "
            "nextval('sq'), v INT CHECK (v > 0)); CREATE VIEW okv AS SELECT v FROM ok",
            &r) == DuckDBSuccess);
        duckdb_destroy_result(&r);
        duckdb_disconnect(&con);
        duckdb_close(&db);

        /* Harmless views and defaults open. */
        HlDbHandle h = { .backend = &hl_db_backend_duckdb };
        ASSERT_EQ(hl_db_backend_duckdb.open(&h.ctx, dsn, NULL), 0);
        h.backend->close(&h);

        ASSERT_TRUE(duckdb_open(path, &db) == DuckDBSuccess);
        ASSERT_TRUE(duckdb_connect(db, &con) == DuckDBSuccess);
        ASSERT_TRUE(duckdb_query(con, ddl[k], &r) == DuckDBSuccess);
        duckdb_destroy_result(&r);
        duckdb_disconnect(&con);
        duckdb_close(&db);

        void *ctx = NULL;
        EXPECT_EQ(-1, hl_db_backend_duckdb.open(&ctx, dsn, NULL));
        EXPECT_TRUE(ctx == NULL);

        unlink(path);
        unlink(wal);
    }
}

/* Audit 39: a view or macro stored in the file that names a `_hull_*` table
 * handed its rows to app SQL whose text names none (`SELECT tok FROM
 * leakview`). The open refuses such a database; the stdlib's own tables
 * (with their nextval('_hull_..._seq') DEFAULTs) open fine. */
UTEST(db_duckdb, stored_object_naming_hull_tables_refuses_the_open)
{
    static const char *const ddl[] = {
        "CREATE VIEW leakview AS SELECT tok FROM _hull_sessions",
        "CREATE MACRO leakm() AS TABLE (SELECT tok FROM _hull_sessions)",
        "CREATE MACRO leaks() AS (SELECT max(tok) FROM main._hull_sessions)",
        "CREATE TABLE app_t (id BIGINT DEFAULT nextval('_hull_s_seq'))",
    };
    for (size_t k = 0; k < sizeof ddl / sizeof ddl[0]; k++) {
        char path[] = "/tmp/hull_duck_ns_XXXXXX";
        int fd = mkstemp(path);
        ASSERT_TRUE(fd >= 0);
        close(fd);
        unlink(path);
        char dsn[128];
        snprintf(dsn, sizeof dsn, "duckdb://%s", path);
        char wal[160];
        snprintf(wal, sizeof wal, "%s.wal", path);

        duckdb_database db;
        duckdb_connection con;
        duckdb_result r;
        ASSERT_TRUE(duckdb_open(path, &db) == DuckDBSuccess);
        ASSERT_TRUE(duckdb_connect(db, &con) == DuckDBSuccess);
        ASSERT_TRUE(duckdb_query(con,
            "CREATE SEQUENCE _hull_s_seq; "
            "CREATE TABLE _hull_sessions (id BIGINT DEFAULT "
            "nextval('_hull_s_seq') PRIMARY KEY, tok VARCHAR); "
            "INSERT INTO _hull_sessions (tok) VALUES ('secret'); "
            "CREATE TABLE items (v INT); CREATE VIEW itemv AS SELECT v FROM items",
            &r) == DuckDBSuccess);
        duckdb_destroy_result(&r);
        duckdb_disconnect(&con);
        duckdb_close(&db);

        /* Stdlib-shaped tables and app views open. */
        HlDbHandle h = { .backend = &hl_db_backend_duckdb };
        ASSERT_EQ(hl_db_backend_duckdb.open(&h.ctx, dsn, NULL), 0);
        h.backend->close(&h);

        ASSERT_TRUE(duckdb_open(path, &db) == DuckDBSuccess);
        ASSERT_TRUE(duckdb_connect(db, &con) == DuckDBSuccess);
        ASSERT_TRUE(duckdb_query(con, ddl[k], &r) == DuckDBSuccess);
        duckdb_destroy_result(&r);
        duckdb_disconnect(&con);
        duckdb_close(&db);

        void *ctx = NULL;
        EXPECT_EQ(-1, hl_db_backend_duckdb.open(&ctx, dsn, NULL));
        EXPECT_TRUE(ctx == NULL);

        unlink(path);
        unlink(wal);
    }
}

/* Audit 39: DuckDB rewrites x.nextval() as nextval(x), so the sequence name
 * stood before the dot: concat('_hu', 'll_..._seq').nextval() advanced a
 * stdlib sequence. Refused on the real engine. */
UTEST(db_duckdb, method_call_sequence_names_are_refused)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(0, hl_db_exec(&h, "CREATE SEQUENCE s39", NULL, 0));
    static const char *const refused[] = {
        "SELECT concat('s', '39').nextval()",
        "SELECT concat('s', '39').\"nextval\"()",
        "SELECT concat('s', '39') . nextval ( )",
        "SELECT concat('s', '39').currval()",
        "SELECT nextval()",
    };
    int64_t c = 0;
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        EXPECT_EQ(-1, hl_db_query(&h, refused[i], NULL, 0, count_cb, &c, NULL));
        /* Refused by Hull, not by DuckDB's parser. */
        EXPECT_TRUE(strstr(hl_db_errmsg(&h), "table-name") != NULL);
    }
    /* None of them advanced the sequence. ('s39'.nextval() is not valid
     * DuckDB syntax, so the literal form is nextval('s39').) */
    EXPECT_EQ(0, hl_db_query(&h, "SELECT nextval('s39')", NULL, 0,
                             count_cb, &c, NULL));
    EXPECT_EQ(1, (int)c);
    h.backend->close(&h);
}

/* Audit 37: a params array short of the placeholders (Lua drops trailing
 * nils) binds the tail NULL, as the other backends do. */
UTEST(db_duckdb, short_params_bind_the_tail_null)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h, "CREATE TABLE t (a INTEGER, b VARCHAR, c VARCHAR)",
                         NULL, 0), 0);
    HlValue v = { .type = HL_TYPE_INT, .i = 5 };
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO t VALUES (?, ?, ?)", &v, 1), 0);
    int64_t c = 0;
    ASSERT_EQ(hl_db_query(&h, "SELECT count(*) FROM t WHERE a = 5 AND b IS NULL "
                          "AND c IS NULL", NULL, 0, count_cb, &c, NULL), 0);
    EXPECT_EQ(1, (int)c);
    HlValue two[2] = { { .type = HL_TYPE_INT, .i = 1 }, { .type = HL_TYPE_INT, .i = 2 } };
    EXPECT_NE(0, hl_db_exec(&h, "INSERT INTO t (a) VALUES (?)", two, 2));
    h.backend->close(&h);
}

/* @p tpl with its one %s replaced by @p name (the stdlib's substitution). */
static void fill_seq(char *out, size_t cap, const char *tpl, const char *name)
{
    const char *m = strstr(tpl, "%s");
    if (!m) { snprintf(out, cap, "%s", tpl); return; }
    snprintf(out, cap, "%.*s%s%s", (int)(m - tpl), tpl, name, m + 2);
}

/* Audit 37: the stdlib's id column on DuckDB names a Hull-owned sequence it
 * creates first (the raw template named one called %s). */
UTEST(db_duckdb, identity_template_makes_a_working_table)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    char sql[256];
    for (int round = 0; round < 2; round++) {   /* idempotent, as init is */
        fill_seq(sql, sizeof sql, hl_db_backend_duckdb.dialect.identity_sequence,
                 "_hull_t_id_seq");
        ASSERT_EQ(hl_db_exec(&h, sql, NULL, 0), 0);
        char col[128];
        fill_seq(col, sizeof col, hl_db_backend_duckdb.dialect.identity_column,
                 "_hull_t_id_seq");
        snprintf(sql, sizeof sql,
                 "CREATE TABLE IF NOT EXISTS _hull_t (id %s, v TEXT)", col);
        ASSERT_EQ(hl_db_exec(&h, sql, NULL, 0), 0);
    }
    ASSERT_EQ(hl_db_exec(&h, "INSERT INTO _hull_t (v) VALUES ('a'), ('b')",
                         NULL, 0), 0);
    int64_t c = 0;
    ASSERT_EQ(hl_db_query(&h, "SELECT CAST(max(id) AS BIGINT) FROM _hull_t",
                          NULL, 0, count_cb, &c, NULL), 0);
    EXPECT_EQ(2, (int)c);
    h.backend->close(&h);
}

/* Audit 19: the selector matches a DSN's scheme in any case (db_select.c
 * lowercases it), but the backend stripped only a lowercase "duckdb://", so
 * "DuckDB://:memory:" was opened as a file of that name. */
UTEST(db_duckdb, dsn_scheme_is_matched_in_any_case)
{
    void *ctx = NULL;
    ASSERT_EQ(hl_db_backend_duckdb.open(&ctx, "DuckDB://:memory:", NULL), 0);
    HlDbHandle h = {0};
    h.backend = &hl_db_backend_duckdb;
    h.ctx = ctx;
    EXPECT_EQ(hl_db_exec(&h, "CREATE TABLE t (x INTEGER)", NULL, 0), 0);
    h.backend->close(&h);
}

/* The dialect descriptor matches the design table + the backend identity. */
UTEST(db_duckdb, dialect_descriptor)
{
    const HlDbBackend *b = &hl_db_backend_duckdb;
    ASSERT_EQ(b->dialect.identifier_quote, '"');
    ASSERT_STREQ(b->dialect.placeholder, "?");
    ASSERT_STREQ(b->dialect.upsert_style, "on_conflict");
    ASSERT_EQ(b->dialect.supports_returning, 1);
    ASSERT_EQ(b->dialect.supports_index_if_not_exists, 1);
    ASSERT_EQ(b->dialect.supports_skip_locked, 0);   /* not a hull/jobs target */
    ASSERT_TRUE(strstr(b->dialect.identity_column, "nextval") != NULL);
    ASSERT_STREQ(b->dialect.identity_sequence, "CREATE SEQUENCE IF NOT EXISTS %s");
    ASSERT_EQ((int)b->native_tag, (int)HL_DB_NATIVE_DUCKDB);
    ASSERT_EQ(b->supports_udf, 0);
    ASSERT_STREQ(b->schemes[0], "duckdb");
    ASSERT_TRUE(b->schemes[1] == NULL);
}

/* insert_if_absent: the first row lands; a second with the same conflict key is
 * silently ignored (ON CONFLICT DO NOTHING), leaving the original value. */
UTEST(db_duckdb, insert_if_absent)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h,
        "CREATE TABLE kv (k INTEGER PRIMARY KEY, v INTEGER)", NULL, 0), 0);

    const char *conflict[] = { "k" };
    const char *cols[]     = { "k", "v" };
    HlValue first[2]  = { { .type = HL_TYPE_INT, .i = 1 }, { .type = HL_TYPE_INT, .i = 100 } };
    HlValue second[2] = { { .type = HL_TYPE_INT, .i = 1 }, { .type = HL_TYPE_INT, .i = 200 } };

    ASSERT_EQ(hl_db_insert_if_absent(&h, "kv", conflict, 1, cols, first, 2), 0);
    ASSERT_EQ(hl_db_insert_if_absent(&h, "kv", conflict, 1, cols, second, 2), 0);

    int64_t v = -1;
    ASSERT_EQ(hl_db_query(&h, "SELECT v FROM kv WHERE k = 1",
                          NULL, 0, count_cb, &v, NULL), 0);
    ASSERT_EQ(v, 100);   /* the DO NOTHING kept the original */

    int64_t n = -1;
    ASSERT_EQ(hl_db_query(&h, "SELECT COUNT(*) FROM kv", NULL, 0, count_cb, &n, NULL), 0);
    ASSERT_EQ(n, 1);

    h.backend->close(&h);
}

/* upsert: the first row lands; a second with the same conflict key overwrites
 * the non-key columns (ON CONFLICT DO UPDATE SET v = excluded.v). */
UTEST(db_duckdb, upsert)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h,
        "CREATE TABLE kv (k INTEGER PRIMARY KEY, v INTEGER)", NULL, 0), 0);

    const char *conflict[] = { "k" };
    const char *cols[]     = { "k", "v" };
    HlValue first[2]  = { { .type = HL_TYPE_INT, .i = 1 }, { .type = HL_TYPE_INT, .i = 100 } };
    HlValue second[2] = { { .type = HL_TYPE_INT, .i = 1 }, { .type = HL_TYPE_INT, .i = 200 } };

    ASSERT_EQ(hl_db_upsert(&h, "kv", conflict, 1, cols, first, 2), 0);
    ASSERT_EQ(hl_db_upsert(&h, "kv", conflict, 1, cols, second, 2), 0);

    int64_t v = -1;
    ASSERT_EQ(hl_db_query(&h, "SELECT v FROM kv WHERE k = 1",
                          NULL, 0, count_cb, &v, NULL), 0);
    ASSERT_EQ(v, 200);   /* the DO UPDATE overwrote it */

    int64_t n = -1;
    ASSERT_EQ(hl_db_query(&h, "SELECT COUNT(*) FROM kv", NULL, 0, count_cb, &n, NULL), 0);
    ASSERT_EQ(n, 1);

    h.backend->close(&h);
}

typedef struct {
    int  count;
    char names[8][64];
} ColNames;

static void colname_cb(void *ctx, const char *name)
{
    ColNames *c = ctx;
    if (c->count < 8) {
        snprintf(c->names[c->count], sizeof c->names[0], "%s", name ? name : "");
        c->count++;
    }
}

/* table_columns walks information_schema.columns in ordinal order. Includes a
 * column name at exactly the 12-byte inline threshold to exercise the copy +
 * NUL-terminate path in duck_table_columns_row. */
UTEST(db_duckdb, table_columns)
{
    HlDbHandle h = {0};
    ASSERT_EQ(duck_open_mem(&h), 0);
    ASSERT_EQ(hl_db_exec(&h,
        "CREATE TABLE cols (id INTEGER, twelve_bytes VARCHAR, note VARCHAR)",
        NULL, 0), 0);

    ColNames c;
    memset(&c, 0, sizeof c);
    ASSERT_EQ(hl_db_table_columns(&h, "cols", colname_cb, &c), 0);
    ASSERT_EQ(c.count, 3);
    ASSERT_STREQ(c.names[0], "id");
    ASSERT_STREQ(c.names[1], "twelve_bytes");   /* exactly 12 bytes */
    ASSERT_STREQ(c.names[2], "note");

    h.backend->close(&h);
}

/* Audit 20: every open was an instance of its own, so the db.async pool
 * threads' connections to a file were separate DuckDB instances from the
 * event loop's: a pool thread read what its instance had replayed at open,
 * not what the other had committed since. One instance per file now, shared
 * by refcount; the last close closes it, and :memory: stays private. */
UTEST(db_duckdb, one_instance_per_file)
{
    char path[] = "/tmp/hull_duck_shared_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    unlink(path);   /* DuckDB makes the file itself */
    char dsn[128];
    snprintf(dsn, sizeof dsn, "duckdb://%s", path);

    HlDbHandle a = { .backend = &hl_db_backend_duckdb };
    HlDbHandle b = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(hl_db_backend_duckdb.open(&a.ctx, dsn, NULL), 0);
    ASSERT_EQ(hl_db_backend_duckdb.open(&b.ctx, dsn, NULL), 0);
    ASSERT_EQ(hl_db_exec(&a, "CREATE TABLE t (x INTEGER)", NULL, 0), 0);
    ASSERT_EQ(hl_db_exec(&a, "INSERT INTO t VALUES (41), (1)", NULL, 0), 0);
    int64_t sum = 0;
    ASSERT_EQ(hl_db_query(&b, "SELECT CAST(SUM(x) AS BIGINT) FROM t", NULL, 0,
                          count_cb, &sum, NULL), 0);
    EXPECT_EQ(sum, (int64_t)42);           /* b sees a's committed rows */
    a.backend->close(&a);
    sum = 0;
    ASSERT_EQ(hl_db_query(&b, "SELECT CAST(SUM(x) AS BIGINT) FROM t", NULL, 0,
                          count_cb, &sum, NULL), 0);
    EXPECT_EQ(sum, (int64_t)42);           /* the instance outlives a */
    b.backend->close(&b);

    /* The last close closed it: a new open is a new instance of the file. */
    HlDbHandle c = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(hl_db_backend_duckdb.open(&c.ctx, dsn, NULL), 0);
    sum = 0;
    ASSERT_EQ(hl_db_query(&c, "SELECT CAST(SUM(x) AS BIGINT) FROM t", NULL, 0,
                          count_cb, &sum, NULL), 0);
    EXPECT_EQ(sum, (int64_t)42);
    c.backend->close(&c);

    /* :memory: stays private to its open. */
    HlDbHandle m1 = {0}, m2 = {0};
    ASSERT_EQ(duck_open_mem(&m1), 0);
    ASSERT_EQ(duck_open_mem(&m2), 0);
    ASSERT_EQ(hl_db_exec(&m1, "CREATE TABLE only_m1 (x INTEGER)", NULL, 0), 0);
    EXPECT_NE(hl_db_exec(&m2, "SELECT * FROM only_m1", NULL, 0), 0);
    m1.backend->close(&m1);
    m2.backend->close(&m2);

    unlink(path);
    char wal[160];
    snprintf(wal, sizeof wal, "%s.wal", path);
    unlink(wal);
}

/* Audit 21: the registry matched the DSN's spelling, so "/tmp/x" and
 * "/tmp/./x" (or a relative and an absolute spelling) each got an instance of
 * their own - the split the registry exists to prevent. */
UTEST(db_duckdb, one_instance_whatever_the_spelling)
{
    char path[] = "/tmp/hull_duck_spell_XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    unlink(path);
    const char *name = strrchr(path, '/') + 1;
    char dsn_a[160], dsn_b[160];
    snprintf(dsn_a, sizeof dsn_a, "duckdb://%s", path);
    snprintf(dsn_b, sizeof dsn_b, "duckdb:///tmp/./%s", name);

    HlDbHandle a = { .backend = &hl_db_backend_duckdb };
    HlDbHandle b = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(hl_db_backend_duckdb.open(&a.ctx, dsn_a, NULL), 0);
    ASSERT_EQ(hl_db_backend_duckdb.open(&b.ctx, dsn_b, NULL), 0);
    ASSERT_EQ(hl_db_exec(&a, "CREATE TABLE t (x INTEGER)", NULL, 0), 0);
    ASSERT_EQ(hl_db_exec(&a, "INSERT INTO t VALUES (40), (2)", NULL, 0), 0);
    int64_t sum = 0;
    ASSERT_EQ(hl_db_query(&b, "SELECT CAST(SUM(x) AS BIGINT) FROM t", NULL, 0,
                          count_cb, &sum, NULL), 0);
    EXPECT_EQ(sum, (int64_t)42);   /* b is a's instance */
    a.backend->close(&a);
    b.backend->close(&b);

    unlink(path);
    char wal[160];
    snprintf(wal, sizeof wal, "%s.wal", path);
    unlink(wal);
}

UTEST_MAIN()

#else  /* !HL_ENABLE_DUCKDB - no-op so the file is harmless in every flavor */

int main(void) { return 0; }

#endif /* HL_ENABLE_DUCKDB */
