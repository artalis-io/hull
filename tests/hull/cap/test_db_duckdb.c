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
#include <limits.h>   /* PATH_MAX */
#include <sys/stat.h> /* mkdir */
#include <dirent.h>   /* opendir: idb_rm_tree */

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

/* ── Audit 41 ─────────────────────────────────────────────────────── */

/* @p tmpl with every "%s" replaced by @p path, into @p out. */
static void a41_subst(char *out, size_t cap, const char *tmpl, const char *path)
{
    size_t o = 0, pl = strlen(path);
    for (const char *t = tmpl; *t && o + 1 < cap; t++) {
        if (t[0] == '%' && t[1] == 's' && o + pl + 1 < cap) {
            memcpy(out + o, path, pl);
            o += pl;
            t++;
        } else {
            out[o++] = *t;
        }
    }
    out[o] = '\0';
}

/* A fresh file at @p path (made by the caller's mkstemp name) holding the
 * placeholder table `_hull_probe` plus @p ddl, with every %s in @p ddl
 * replaced by the database path. 0, or -1 when DuckDB refused the DDL. */
static int a41_plant(const char *path, const char *ddl)
{
    char sql[2048];
    a41_subst(sql, sizeof sql, ddl, path);
    duckdb_database db;
    duckdb_connection con;
    duckdb_result r;
    if (duckdb_open(path, &db) != DuckDBSuccess) return -1;
    if (duckdb_connect(db, &con) != DuckDBSuccess) {
        duckdb_close(&db);
        return -1;
    }
    int ok = duckdb_query(con,
        "CREATE TABLE _hull_probe (tok VARCHAR); "
        "INSERT INTO _hull_probe VALUES ('SECRET-TOKEN')", &r) == DuckDBSuccess;
    duckdb_destroy_result(&r);
    if (ok && ddl[0]) {
        ok = duckdb_query(con, sql, &r) == DuckDBSuccess;
        if (!ok) fprintf(stderr, "ddl refused by DuckDB: %s: %s\n", sql,
                         duckdb_result_error(&r));
        duckdb_destroy_result(&r);
    }
    duckdb_disconnect(&con);
    duckdb_close(&db);
    return ok ? 0 : -1;
}

static void a41_tmpname(char *path)
{
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    unlink(path);
}

static void a41_unlink_db(const char *path)
{
    char wal[600];
    unlink(path);
    snprintf(wal, sizeof wal, "%s.wal", path);
    unlink(wal);
}

/* Audit 41: stored views, DEFAULTs, CHECKs, generated columns and index
 * expressions went through two of the five refusal scanners stored macros
 * get, so a planted file holding a view that calls the dynamic-SQL table
 * functions on a name built at run time, or that reads the database's own
 * file (which DuckDB always allows), opened cleanly - and app SQL reading
 * the view then got `_hull_*` rows or the raw file. Each is refused at open
 * now. */
UTEST(db_duckdb, stored_view_full_refusal_set_refuses_the_open)
{
    static const char *const ddl[] = {
        "CREATE VIEW leak AS SELECT * FROM "
            "query('SELECT tok FROM _hu' || 'll_probe')",
        "CREATE VIEW leak AS SELECT * FROM query_table('_hu' || 'll_probe')",
        "CREATE VIEW leak AS SELECT * FROM json_execute_serialized_sql("
            "json_serialize_sql('SELECT tok FROM _hu' || 'll_probe'))",
        "CREATE VIEW leak AS SELECT size FROM read_blob('%s')",
        "CREATE VIEW leak AS SELECT size FROM read_blob('%s' || '')",
        "CREATE VIEW leak AS SELECT content FROM read_text('%s.wal')",
        "CREATE VIEW leak AS SELECT content FROM read_text("
            "concat('%s', '.w', 'al'))",
        "CREATE TABLE g (x INT, y BIGINT AS (sleep_ms(400 + x * 0)::BIGINT))",
    };
    for (size_t k = 0; k < sizeof ddl / sizeof ddl[0]; k++) {
        char path[] = "/tmp/hull_duck_a41v_XXXXXX";
        a41_tmpname(path);
        char dsn[128];
        snprintf(dsn, sizeof dsn, "duckdb://%s", path);
        ASSERT_EQ(0, a41_plant(path, ddl[k]));
        void *ctx = NULL;
        int rc = hl_db_backend_duckdb.open(&ctx, dsn, NULL);
        if (rc == 0) {
            fprintf(stderr, "opened a file holding: %s\n", ddl[k]);
            HlDbHandle h = { .backend = &hl_db_backend_duckdb, .ctx = ctx };
            h.backend->close(&h);
        }
        EXPECT_EQ(-1, rc);
        EXPECT_TRUE(ctx == NULL);
        a41_unlink_db(path);
    }
}

/* The controls: harmless views, DEFAULTs, CHECKs, generated columns, index
 * expressions and table macros (with a parameter default) open and run. */
UTEST(db_duckdb, harmless_stored_objects_open_and_run)
{
    char path[] = "/tmp/hull_duck_a41h_XXXXXX";
    a41_tmpname(path);
    char dsn[128];
    snprintf(dsn, sizeof dsn, "duckdb://%s", path);
    ASSERT_EQ(0, a41_plant(path,
        "CREATE TABLE items (v INT CHECK (v > 0), w INT AS (v * 2), "
        "note VARCHAR DEFAULT 'none'); "
        "CREATE INDEX items_vi ON items ((v + 1)); "
        "INSERT INTO items (v) VALUES (3); "
        "CREATE VIEW iv AS SELECT w, note FROM items; "
        "CREATE MACRO tm(n := 2) AS TABLE SELECT n AS x; "
        "CREATE MACRO twice(a) AS a * 2"));
    HlDbHandle h = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(0, hl_db_backend_duckdb.open(&h.ctx, dsn, NULL));
    int64_t n = 0;
    EXPECT_EQ(0, hl_db_query(&h, "SELECT w FROM iv", NULL, 0, count_cb, &n,
                             NULL));
    EXPECT_EQ(6, n);
    EXPECT_EQ(0, hl_db_query(&h, "SELECT x FROM tm()", NULL, 0, count_cb, &n,
                             NULL));
    EXPECT_EQ(2, n);
    EXPECT_EQ(0, hl_db_query(&h, "SELECT twice(21)", NULL, 0, count_cb, &n,
                             NULL));
    EXPECT_EQ(42, n);
    EXPECT_EQ(0, hl_db_exec(&h, "INSERT INTO items (v) VALUES (4)", NULL, 0));
    EXPECT_EQ(-1, hl_db_exec(&h, "INSERT INTO items (v) VALUES (-1)", NULL, 0));
    h.backend->close(&h);
    a41_unlink_db(path);
}

/* Audit 41 (Low): only a statement's first word is a session SET. A column
 * assignment named schema / search_path runs; the session forms are still
 * refused. */
UTEST(db_duckdb, set_column_assignment_is_not_a_catalog_switch)
{
    HlDbHandle h = {0};
    ASSERT_EQ(0, duck_open_mem(&h));
    static const char *const ok[] = {
        "CREATE TABLE prefs (id INT PRIMARY KEY, schema VARCHAR, "
            "search_path VARCHAR)",
        "INSERT INTO prefs VALUES (1, 'v1', 'p1')",
        "UPDATE prefs SET schema = 'v2' WHERE id = 1",
        "UPDATE prefs SET search_path = 'p2', schema = 'v3' WHERE id = 1",
        "INSERT INTO prefs VALUES (1, 'v4', 'p4') ON CONFLICT DO UPDATE "
            "SET schema = excluded.schema",
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        EXPECT_EQ(0, hl_db_exec(&h, ok[i], NULL, 0));
    static const char *const refused[] = {
        "SET schema = 'main'",
        "SET search_path = 'main'",
        "  /* lead */ SET SESSION search_path = 'main'",
        "SELECT 1; SET schema = 'main'",
        "EXPLAIN ANALYZE SET schema = 'main'",
        "RESET search_path",
        "PRAGMA search_path",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        EXPECT_EQ(-1, hl_db_exec(&h, refused[i], NULL, 0));
    h.backend->close(&h);
}

/* Audit 41: in mode B the protected-file and glob checks read only a path's
 * text, and DuckDB follows symbolic links: a link inside an allowed
 * directory naming the database file (or its WAL) read it under another
 * name, a glob over a directory holding such a link did too, and a link to
 * a file outside the allowed directories passed DuckDB's lexical
 * allowed_directories check. Paths running through a link are refused. */
UTEST(db_duckdb, mode_b_paths_through_symlinks_are_refused)
{
    char dir[] = "/tmp/hull_duck_a41d_XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    char rdir[PATH_MAX];
    ASSERT_TRUE(realpath(dir, rdir) != NULL);
    char db[PATH_MAX + 32], data[PATH_MAX + 32], clean[PATH_MAX + 32];
    char link_db[PATH_MAX + 64], link_wal[PATH_MAX + 64];
    char link_out[PATH_MAX + 64], outside[PATH_MAX + 64], okcsv[PATH_MAX + 64];
    char dsn[PATH_MAX + 64], sql[3 * PATH_MAX];
    snprintf(db, sizeof db, "%s/app.duckdb", rdir);
    snprintf(data, sizeof data, "%s/data", rdir);
    snprintf(clean, sizeof clean, "%s/clean", rdir);
    snprintf(link_db, sizeof link_db, "%s/notes.csv", data);
    snprintf(link_wal, sizeof link_wal, "%s/journal.bin", data);
    snprintf(link_out, sizeof link_out, "%s/in.txt", data);
    snprintf(outside, sizeof outside, "%s/outside.txt", rdir);
    snprintf(okcsv, sizeof okcsv, "%s/ok.csv", clean);
    ASSERT_EQ(0, mkdir(data, 0700));
    ASSERT_EQ(0, mkdir(clean, 0700));
    ASSERT_EQ(0, a41_plant(db, "CHECKPOINT"));
    FILE *fp = fopen(outside, "w");
    ASSERT_TRUE(fp != NULL);
    fputs("OUTSIDE\n", fp);
    fclose(fp);
    fp = fopen(okcsv, "w");
    ASSERT_TRUE(fp != NULL);
    fputs("a\n1\n2\n", fp);
    fclose(fp);
    char wal[PATH_MAX + 32];
    snprintf(wal, sizeof wal, "%s.wal", db);
    ASSERT_EQ(0, symlink(db, link_db));
    ASSERT_EQ(0, symlink(wal, link_wal));
    ASSERT_EQ(0, symlink(outside, link_out));

    static const char *dirs[2];
    dirs[0] = data;
    dirs[1] = clean;
    hl_db_duckdb_set_fs_policy(dirs, 2);
    snprintf(dsn, sizeof dsn, "duckdb://%s", db);
    HlDbHandle h = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(0, hl_db_backend_duckdb.open(&h.ctx, dsn, NULL));

    const char *const refused[] = {
        "SELECT size FROM read_blob('%s')",           /* -> the db file */
        "SELECT content FROM read_text('%s')",
        "SELECT * FROM '%s'",                         /* replacement scan */
    };
    const char *const targets[] = { link_db, link_wal, link_out };
    for (size_t t = 0; t < sizeof targets / sizeof targets[0]; t++) {
        for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
            a41_subst(sql, sizeof sql, refused[i], targets[t]);
            int rc = hl_db_query(&h, sql, NULL, 0, NULL, NULL, NULL);
            if (rc == 0) fprintf(stderr, "ran: %s\n", sql);
            EXPECT_EQ(-1, rc);
        }
    }
    /* A glob over a directory holding such a link. */
    snprintf(sql, sizeof sql, "SELECT size FROM read_blob('%s/*')", data);
    EXPECT_EQ(-1, hl_db_query(&h, sql, NULL, 0, NULL, NULL, NULL));
    snprintf(sql, sizeof sql, "SELECT size FROM read_blob('%s/**')", rdir);
    EXPECT_EQ(-1, hl_db_query(&h, sql, NULL, 0, NULL, NULL, NULL));
    snprintf(sql, sizeof sql, "SELECT * FROM '%s/*.csv'", data);
    EXPECT_EQ(-1, hl_db_query(&h, sql, NULL, 0, NULL, NULL, NULL));
    /* COPY to a link. */
    snprintf(sql, sizeof sql, "COPY (SELECT 1) TO '%s'", link_out);
    EXPECT_EQ(-1, hl_db_exec(&h, sql, NULL, 0));

    /* Controls: an ordinary file, and a glob over a directory with no link. */
    int64_t n = 0;
    snprintf(sql, sizeof sql, "SELECT count(*) FROM read_csv('%s')", okcsv);
    EXPECT_EQ(0, hl_db_query(&h, sql, NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(2, n);
    snprintf(sql, sizeof sql, "SELECT count(*) FROM read_csv('%s/*.csv')", clean);
    EXPECT_EQ(0, hl_db_query(&h, sql, NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(2, n);
    snprintf(sql, sizeof sql, "SELECT count(*) FROM '%s'", okcsv);
    EXPECT_EQ(0, hl_db_query(&h, sql, NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(2, n);

    h.backend->close(&h);
    hl_db_duckdb_set_fs_policy(NULL, 0);
    unlink(link_db);
    unlink(link_wal);
    unlink(link_out);
    unlink(outside);
    unlink(okcsv);
    a41_unlink_db(db);
    rmdir(data);
    rmdir(clean);
    rmdir(rdir);
}

/* ── Audit convergence §7.2: DuckDB SQL cannot reach the app's .hull ──
 *
 * The app's reserved `.hull` directory holds Hull's internal database
 * (sessions, roles, TOTP secrets). A duckdb:// connection with fs.read =
 * fs.write = {"."} (mode B) had the whole app root in allowed_directories,
 * so read_blob('.hull/internal.db') returned the internal file raw and COPY
 * could write into `.hull`. Two layers now: the root is granted as its
 * top-level entries minus `.hull` (hl_db_duckdb_split_root), and the SQL scan
 * refuses a literal naming `.hull` and a path or glob that resolves into it.
 * Each fixture runs from a fresh app root (DuckDB resolves relative paths
 * against the working directory). */

typedef struct {
    char root[PATH_MAX];      /* canonical */
    char reserved[PATH_MAX + 8];
    char dbdir[PATH_MAX];
    char db[PATH_MAX + 32];
    char cwd[PATH_MAX];
} IdbTree;

static void idb_write(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (fp) { fputs(text, fp); fclose(fp); }
}

static void idb_rm_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    continue;
                char child[PATH_MAX + 256];
                snprintf(child, sizeof child, "%s/%s", path, de->d_name);
                idb_rm_tree(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* <root>/.hull/{internal.db,internal.db-wal,data.csv}, <root>/data/x.csv,
 * <root>/top.csv, an empty <root>/db, the database at <dbdir>/ana.duckdb;
 * the working directory is the root. 0 / -1. */
static int idb_setup(IdbTree *t)
{
    char tmpl[] = "/tmp/hull_duck_idb_XXXXXX";
    if (!mkdtemp(tmpl) || !realpath(tmpl, t->root)) return -1;
    if (!getcwd(t->cwd, sizeof t->cwd)) return -1;
    snprintf(t->reserved, sizeof t->reserved, "%s/.hull", t->root);
    /* The database outside the app root (a deploy volume): the
     * database-directory glob check (audit 41) refuses a glob from any
     * ancestor of the database's directory, so a database inside the root
     * would refuse every root glob already and hide the checks tested. */
    char dtmpl[] = "/tmp/hull_duck_idb_db_XXXXXX";
    if (!mkdtemp(dtmpl) || !realpath(dtmpl, t->dbdir)) return -1;
    snprintf(t->db, sizeof t->db, "%s/ana.duckdb", t->dbdir);
    char p[PATH_MAX + 64];
    if (mkdir(t->reserved, 0700) != 0) return -1;
    snprintf(p, sizeof p, "%s/db", t->root);
    if (mkdir(p, 0700) != 0) return -1;
    snprintf(p, sizeof p, "%s/internal.db", t->reserved);
    idb_write(p, "SQLite format 3 SECRET-SESSION-TOKEN\n");
    snprintf(p, sizeof p, "%s/internal.db-wal", t->reserved);
    idb_write(p, "WAL SECRET\n");
    snprintf(p, sizeof p, "%s/data.csv", t->reserved);
    idb_write(p, "a\n1\n");
    snprintf(p, sizeof p, "%s/data", t->root);
    if (mkdir(p, 0700) != 0) return -1;
    snprintf(p, sizeof p, "%s/data/x.csv", t->root);
    idb_write(p, "a,b\n1,2\n3,4\n");
    snprintf(p, sizeof p, "%s/top.csv", t->root);
    idb_write(p, "a\n5\n");
    return chdir(t->root);
}

static void idb_teardown(IdbTree *t)
{
    hl_db_duckdb_set_fs_policy(NULL, 0);
    if (chdir(t->cwd) != 0) { /* nothing to do */ }
    idb_rm_tree(t->root);
    idb_rm_tree(t->dbdir);
}

typedef struct {
    char *dirs[64];
    int   nd;
    char *files[64];
    int   nf;
} IdbSplit;

static int idb_split_cb(void *ud, const char *path, int is_dir)
{
    IdbSplit *s = ud;
    if (is_dir ? s->nd >= 64 : s->nf >= 64) return -1;
    char *c = strdup(path);
    if (!c) return -1;
    if (is_dir) s->dirs[s->nd++] = c; else s->files[s->nf++] = c;
    return 0;
}

static void idb_split_free(IdbSplit *s)
{
    for (int i = 0; i < s->nd; i++) free(s->dirs[i]);
    for (int i = 0; i < s->nf; i++) free(s->files[i]);
    s->nd = s->nf = 0;
}

static int idb_run(HlDbHandle *h, const char *tmpl, const char *root)
{
    char sql[3 * PATH_MAX];
    a41_subst(sql, sizeof sql, tmpl, root);
    int rc = hl_db_query(h, sql, NULL, 0, NULL, NULL, NULL);
    if (rc == 0) fprintf(stderr, "ran: %s\n", sql);
    else if (getenv("IDB_DEBUG")) fprintf(stderr, "refused: %s: %s\n", sql, hl_db_errmsg(h));
    return rc;
}

/* Every spelling of a path into .hull, with the app root granted whole (as
 * serve.c granted it before the fix) and nothing else known: the name check
 * alone refuses them. */
UTEST(duckdb_convergence_idb, dot_hull_paths_refused)
{
    IdbTree t;
    ASSERT_EQ(0, idb_setup(&t));
    static const char *dirs[1];
    dirs[0] = t.root;
    hl_db_duckdb_set_fs_policy(dirs, 1);
    char dsn[PATH_MAX + 64];
    snprintf(dsn, sizeof dsn, "duckdb://%s", t.db);
    HlDbHandle h = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(0, hl_db_backend_duckdb.open(&h.ctx, dsn, NULL));

    static const char *const refused[] = {
        "SELECT size FROM read_blob('.hull/internal.db')",
        "SELECT hex(content) FROM read_blob('.hull/internal.db')",
        "SELECT content FROM read_text('.hull/internal.db-wal')",
        "SELECT size FROM read_blob('.HULL/internal.db')",
        "SELECT size FROM read_blob('.HuLl/internal.db')",
        "SELECT size FROM read_blob('./.hull/internal.db')",
        "SELECT size FROM read_blob('.//.hull//internal.db')",
        "SELECT size FROM read_blob('data/../.hull/internal.db')",
        "SELECT size FROM read_blob('x/../.hull/internal.db')",
        "SELECT size FROM read_blob('%s/.hull/internal.db')",
        "SELECT size FROM read_blob('%s/data/../.hull/internal.db')",
        "SELECT size FROM read_blob('file://%s/.hull/internal.db')",
        "SELECT size FROM read_blob(['data/x.csv', '.hull/internal.db'])",
        "SELECT * FROM read_csv('.hull/data.csv')",
        "SELECT * FROM '.hull/data.csv'",
        "SELECT * FROM '.hull/internal.db'",
        "SELECT size FROM '.hull/internal.db'.read_blob()",
        "SELECT * FROM glob('.hull/*')",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        EXPECT_EQ(-1, idb_run(&h, refused[i], t.root));

    static const char *const writes[] = {
        "COPY (SELECT 'CORRUPTED' AS a) TO '.hull/internal.db' (FORMAT csv)",
        "COPY (SELECT 1 AS a) TO '.hull/internal.db-journal'",
        "COPY (SELECT 1 AS a) TO './.HULL/new.csv'",
        "COPY (SELECT 1 AS a) TO '%s/.hull/new.csv'",
        "CREATE TABLE t AS SELECT 1 AS a; COPY t FROM '.hull/data.csv'",
    };
    for (size_t i = 0; i < sizeof writes / sizeof writes[0]; i++) {
        char sql[3 * PATH_MAX];
        a41_subst(sql, sizeof sql, writes[i], t.root);
        int rc = hl_db_exec(&h, sql, NULL, 0);
        if (rc == 0) fprintf(stderr, "ran: %s\n", sql);
        EXPECT_EQ(-1, rc);
    }
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/new.csv", t.reserved);
    struct stat st;
    EXPECT_NE(0, stat(p, &st));
    snprintf(p, sizeof p, "%s/internal.db", t.reserved);
    FILE *fp = fopen(p, "r");
    char head[16] = {0};
    if (fp) { if (!fgets(head, sizeof head, fp)) head[0] = '\0'; fclose(fp); }
    EXPECT_EQ(0, strncmp(head, "SQLite format 3", 15));

    h.backend->close(&h);
    idb_teardown(&t);
}

/* Globs from the app root reach .hull (DuckDB's glob matches dot entries):
 * with the root still granted whole, the resolved check (the reserved
 * directory known) refuses every one whose wildcards could reach a file in
 * it, and keeps a last-component wildcard in a subdirectory. */
UTEST(duckdb_convergence_idb, globs_reaching_dot_hull_refused)
{
    IdbTree t;
    ASSERT_EQ(0, idb_setup(&t));
    static const char *dirs[1];
    static char reserved[PATH_MAX + 8];
    dirs[0] = t.root;
    snprintf(reserved, sizeof reserved, "%s", t.reserved);
    hl_db_duckdb_set_fs_policy_ex(dirs, 1, NULL, 0, reserved);
    char dsn[PATH_MAX + 64];
    snprintf(dsn, sizeof dsn, "duckdb://%s", t.db);
    HlDbHandle h = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(0, hl_db_backend_duckdb.open(&h.ctx, dsn, NULL));

    static const char *const refused[] = {
        "SELECT size FROM read_blob('*/internal.db')",
        "SELECT size FROM read_blob('**/*.db')",
        "SELECT size FROM read_blob('**')",
        "SELECT size FROM read_blob('.h*/internal.db')",
        "SELECT size FROM read_blob('.hu[l]l/internal.db')",
        "SELECT size FROM read_blob('.hul?/internal.db*')",
        "SELECT size FROM read_blob('%s/*/internal.db')",
        "SELECT size FROM read_blob('data/../*/internal.db')",
        "SELECT file FROM glob('**')",
        "SELECT file FROM glob('*/*')",
        "SELECT * FROM read_csv('*/*.csv')",
        "SELECT * FROM '*/*.csv'",
        "SELECT * FROM '**/*.csv'",
        "COPY (SELECT 1 AS a) TO '*/x.csv'",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        EXPECT_EQ(-1, idb_run(&h, refused[i], t.root));

    int64_t n = 0;
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM glob('data/*.csv')",
                             NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM read_csv('data/*.csv')",
                             NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(2, n);

    h.backend->close(&h);
    idb_teardown(&t);
}

/* The structural layer: the root split into its entries (as serve.c grants a
 * root grant now) leaves .hull and symbolic links out, and with it DuckDB
 * itself refuses a glob from the root - here with the scan's resolved checks
 * off (no reserved directory given), so only allowed_directories stands in
 * the way. */
UTEST(duckdb_convergence_idb, root_grant_split_keeps_dot_hull_out)
{
    IdbTree t;
    ASSERT_EQ(0, idb_setup(&t));
    char lnk[PATH_MAX + 32];
    snprintf(lnk, sizeof lnk, "%s/lnk", t.root);
    ASSERT_EQ(0, symlink(".hull", lnk));

    IdbSplit sp = {0};
    int n = hl_db_duckdb_split_root(t.root, idb_split_cb, &sp);
    EXPECT_EQ(3, n);              /* data, db, top.csv (not .hull, not lnk) */
    EXPECT_EQ(2, sp.nd);
    EXPECT_EQ(1, sp.nf);
    for (int i = 0; i < sp.nd; i++) {
        EXPECT_TRUE(strstr(sp.dirs[i], "/.hull") == NULL);
        EXPECT_TRUE(strstr(sp.dirs[i], "/lnk") == NULL);
    }
    char want[PATH_MAX + 32], want2[PATH_MAX + 32];
    snprintf(want, sizeof want, "%s/data", t.root);
    snprintf(want2, sizeof want2, "%s/db", t.root);
    int seen = 0;
    for (int i = 0; i < sp.nd; i++)
        seen += strcmp(sp.dirs[i], want) == 0 || strcmp(sp.dirs[i], want2) == 0;
    EXPECT_EQ(2, seen);
    snprintf(want, sizeof want, "%s/top.csv", t.root);
    EXPECT_STREQ(want, sp.nf > 0 ? sp.files[0] : "");

    hl_db_duckdb_set_fs_policy_ex((const char *const *)sp.dirs, sp.nd,
                                  (const char *const *)sp.files, sp.nf, NULL);
    char dsn[PATH_MAX + 64];
    snprintf(dsn, sizeof dsn, "duckdb://%s", t.db);
    HlDbHandle h = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(0, hl_db_backend_duckdb.open(&h.ctx, dsn, NULL));

    EXPECT_EQ(-1, idb_run(&h, "SELECT size FROM read_blob('*/internal.db')", t.root));
    EXPECT_EQ(-1, idb_run(&h, "SELECT size FROM read_blob('%s/*/internal.db')", t.root));
    EXPECT_EQ(-1, idb_run(&h, "SELECT file FROM glob('*/*')", t.root));
    /* A file made at the root after the policy was installed is not granted. */
    EXPECT_EQ(-1, hl_db_exec(&h, "COPY (SELECT 1 AS a) TO 'late.csv'", NULL, 0));

    int64_t c = 0;
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM read_csv('top.csv')",
                             NULL, 0, count_cb, &c, NULL));
    EXPECT_EQ(1, c);

    h.backend->close(&h);
    idb_split_free(&sp);
    unlink(lnk);
    idb_teardown(&t);
}

/* No false refusal: ordinary mode-B reads and writes in other directories,
 * under the policy serve.c installs (root split + reserved directory). */
UTEST(duckdb_convergence_idb, ordinary_mode_b_files_still_work)
{
    IdbTree t;
    ASSERT_EQ(0, idb_setup(&t));
    IdbSplit sp = {0};
    ASSERT_EQ(3, hl_db_duckdb_split_root(t.root, idb_split_cb, &sp));
    static char reserved[PATH_MAX + 8];
    snprintf(reserved, sizeof reserved, "%s", t.reserved);
    hl_db_duckdb_set_fs_policy_ex((const char *const *)sp.dirs, sp.nd,
                                  (const char *const *)sp.files, sp.nf, reserved);
    char dsn[PATH_MAX + 64];
    snprintf(dsn, sizeof dsn, "duckdb://%s", t.db);
    HlDbHandle h = { .backend = &hl_db_backend_duckdb };
    ASSERT_EQ(0, hl_db_backend_duckdb.open(&h.ctx, dsn, NULL));

    int64_t n = 0;
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM read_csv('data/x.csv')",
                             NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(2, n);
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM glob('data/*.csv')",
                             NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM 'data/x.csv'",
                             NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(2, n);
    EXPECT_EQ(0, hl_db_exec(&h, "COPY (SELECT 7 AS a) TO 'data/out.csv'", NULL, 0));
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM read_csv('data/out.csv')",
                             NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(1, n);
    EXPECT_EQ(0, hl_db_query(&h, "SELECT count(*) FROM read_csv('top.csv')",
                             NULL, 0, count_cb, &n, NULL));
    EXPECT_EQ(1, n);
    /* A string that merely contains the letters is not a path component. */
    EXPECT_EQ(0, hl_db_query(&h, "SELECT 'shull' || 'x.hull' AS s",
                             NULL, 0, NULL, NULL, NULL));

    h.backend->close(&h);
    idb_split_free(&sp);
    idb_teardown(&t);
}

UTEST_MAIN()

#else  /* !HL_ENABLE_DUCKDB - no-op so the file is harmless in every flavor */

int main(void) { return 0; }

#endif /* HL_ENABLE_DUCKDB */
