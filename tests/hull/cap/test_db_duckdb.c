/*
 * test_db_duckdb.c — DuckDB backend smoke tests (HL_ENABLE_DUCKDB only)
 *
 * Exercises the thin vertical slice: open :memory:, prepared-statement param
 * binding, columnar chunk decode to HlValue (int / text / double / bool / NULL),
 * the dialect descriptor, transactions, and the security lockdown (external file
 * access blocked + configuration locked). Self-gates to an empty program when
 * DuckDB is not compiled, so it is harmless in every other build flavor.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DUCKDB

#include "utest.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_duckdb.h"
#include "hull/cap/types.h"

#include <string.h>

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
    HlDbHandle h;
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
    HlDbHandle h;
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

/* BEGIN + INSERT + ROLLBACK leaves no row; a COMMITted one persists. */
UTEST(db_duckdb, transaction_commit_and_rollback)
{
    HlDbHandle h;
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

/* The security keystone: external file access is off and the configuration is
 * locked, so neither reading a local file nor re-enabling access succeeds. */
UTEST(db_duckdb, security_lockdown)
{
    HlDbHandle h;
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

/* The dialect descriptor matches the design table + the backend identity. */
UTEST(db_duckdb, dialect_descriptor)
{
    const HlDbBackend *b = &hl_db_backend_duckdb;
    ASSERT_EQ(b->dialect.identifier_quote, '"');
    ASSERT_STREQ(b->dialect.placeholder, "?");
    ASSERT_STREQ(b->dialect.upsert_style, "on_conflict");
    ASSERT_EQ(b->dialect.supports_returning, 1);
    ASSERT_EQ(b->dialect.supports_index_if_not_exists, 1);
    ASSERT_TRUE(strstr(b->dialect.identity_column, "nextval") != NULL);
    ASSERT_STREQ(b->dialect.identity_sequence, "CREATE SEQUENCE %s");
    ASSERT_EQ((int)b->native_tag, (int)HL_DB_NATIVE_DUCKDB);
    ASSERT_EQ(b->supports_udf, 0);
    ASSERT_STREQ(b->schemes[0], "duckdb");
    ASSERT_TRUE(b->schemes[1] == NULL);
}

UTEST_MAIN()

#else  /* !HL_ENABLE_DUCKDB — no-op so the file is harmless in every flavor */

int main(void) { return 0; }

#endif /* HL_ENABLE_DUCKDB */
