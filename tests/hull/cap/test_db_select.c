/*
 * test_db_select.c — DSN-scheme backend routing (hl_db_backend_select)
 *
 * Flag-aware: asserts the routing for whatever backends this build compiled,
 * plus the reserved-but-uncompiled and unknown-scheme error paths (which are
 * backend-independent). See cap/db_select.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/db_backend.h"
#include <string.h>

/* Scheme-less DSNs (bare path, ":memory:", "file:" URI) and "sqlite://" route
 * to SQLite when it is compiled; otherwise they error (no default backend). */
UTEST(db_select, sqlite_and_scheme_less)
{
    const char *err = NULL;
    const HlDbBackend *b;
#ifdef HL_ENABLE_SQLITE
    b = hl_db_backend_select(":memory:", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite"); ASSERT_FALSE(err);
    b = hl_db_backend_select("./data.db", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite");
    b = hl_db_backend_select("sqlite://:memory:", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite");
    /* "file:" URI has no "://" -> scheme-less -> SQLite default. */
    b = hl_db_backend_select("file:data.db?mode=ro", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite");
    /* Scheme match is case-insensitive. */
    b = hl_db_backend_select("SQLITE://x", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "sqlite");
#else
    b = hl_db_backend_select(":memory:", &err);
    ASSERT_FALSE(b); ASSERT_TRUE(err);
    err = NULL;
    b = hl_db_backend_select("sqlite://x", &err);
    ASSERT_FALSE(b); ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "HL_ENABLE_SQLITE") != NULL);
#endif
}

/* "postgres://" / "postgresql://" route to Postgres when compiled, else give a
 * build-flag hint (not a generic "unknown scheme"). */
UTEST(db_select, postgres_scheme)
{
    const char *err = NULL;
    const HlDbBackend *b = hl_db_backend_select("postgres://u@h/db", &err);
#ifdef HL_ENABLE_POSTGRES
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "postgres"); ASSERT_FALSE(err);
    b = hl_db_backend_select("postgresql://u@h/db", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "postgres");
#else
    ASSERT_FALSE(b); ASSERT_TRUE(err);
    ASSERT_TRUE(strstr(err, "HL_ENABLE_POSTGRES") != NULL);
#endif
}

/* Reserved schemes with no backend in any current build: a specific hint, never
 * a backend. (DuckDB/MySQL backends do not exist yet.) */
UTEST(db_select, reserved_schemes)
{
    const char *err = NULL;
    ASSERT_FALSE(hl_db_backend_select("duckdb://x.duckdb", &err));
    ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "DuckDB") != NULL);
    err = NULL;
    ASSERT_FALSE(hl_db_backend_select("mysql://u@h/db", &err));
    ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "MySQL") != NULL);
    err = NULL;
    ASSERT_FALSE(hl_db_backend_select("mariadb://u@h/db", &err));
    ASSERT_TRUE(err);
}

/* An unrecognized scheme gets a generic hint. */
UTEST(db_select, unknown_scheme)
{
    const char *err = NULL;
    ASSERT_FALSE(hl_db_backend_select("bogus://x", &err));
    ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "unknown") != NULL);
}

UTEST_MAIN()
