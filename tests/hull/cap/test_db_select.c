/*
 * test_db_select.c - DSN-scheme backend routing (hl_db_backend_select)
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

/* "postgres://" / "postgresql://" route to Postgres when compiled, else point at
 * the composable Postgres feature (not a generic "unknown scheme"). */
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
    ASSERT_TRUE(strstr(err, "feature install postgres") != NULL);
#endif
}

/* "mysql://" / "mariadb://" route to the MySQL backend when compiled, else point
 * at the composable MySQL feature. Both schemes share the one backend. */
UTEST(db_select, mysql_scheme)
{
    const char *err = NULL;
    const HlDbBackend *b = hl_db_backend_select("mysql://u@h/db", &err);
#ifdef HL_ENABLE_MYSQL
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "mysql"); ASSERT_FALSE(err);
    ASSERT_EQ(b->dialect.identifier_quote, '`');   /* backtick dialect */
    b = hl_db_backend_select("mariadb://u@h/db", &err);
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "mysql");
#else
    ASSERT_FALSE(b); ASSERT_TRUE(err);
    ASSERT_TRUE(strstr(err, "feature install mysql") != NULL);
    b = hl_db_backend_select("mariadb://u@h/db", &err);
    ASSERT_FALSE(b); ASSERT_TRUE(strstr(err, "feature install mysql") != NULL);
#endif
}

/* "duckdb://" routes to the DuckDB backend when compiled, else a build-flag
 * hint (reserved-scheme path, covered below). */
UTEST(db_select, duckdb_scheme)
{
    const char *err = NULL;
    const HlDbBackend *b = hl_db_backend_select("duckdb://:memory:", &err);
#ifdef HL_ENABLE_DUCKDB
    ASSERT_TRUE(b); ASSERT_STREQ(b->name, "duckdb"); ASSERT_FALSE(err);
    ASSERT_EQ(b->dialect.identifier_quote, '"');
    ASSERT_EQ((int)b->native_tag, (int)HL_DB_NATIVE_DUCKDB);
#else
    ASSERT_FALSE(b); ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "DuckDB") != NULL);
#endif
}

/* Reserved schemes with no backend in this build: a specific hint, never a
 * backend. mysql/mariadb are reserved only when the MySQL backend isn't
 * compiled; duckdb only when the DuckDB backend isn't. */
UTEST(db_select, reserved_schemes)
{
    const char *err = NULL;
#ifndef HL_ENABLE_DUCKDB
    ASSERT_FALSE(hl_db_backend_select("duckdb://x.duckdb", &err));
    ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "DuckDB") != NULL);
    /* The hint points at the composable-feature path, not just a build flag. */
    ASSERT_TRUE(strstr(err, "feature install") != NULL);
    ASSERT_TRUE(strstr(err, "--with=duckdb") != NULL);
    err = NULL;
#endif
#ifndef HL_ENABLE_MYSQL
    err = NULL;
    ASSERT_FALSE(hl_db_backend_select("mysql://u@h/db", &err));
    ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "MySQL") != NULL);
    err = NULL;
    ASSERT_FALSE(hl_db_backend_select("mariadb://u@h/db", &err));
    ASSERT_TRUE(err);
#endif
}

/* An unrecognized scheme gets a generic hint. */
UTEST(db_select, unknown_scheme)
{
    const char *err = NULL;
    ASSERT_FALSE(hl_db_backend_select("bogus://x", &err));
    ASSERT_TRUE(err); ASSERT_TRUE(strstr(err, "unknown") != NULL);
}

/* Feature-composition hook: a STRONG override of the base's weak
 * hl_db_feature_backends (this is exactly how a `hull build --with=<feature>`
 * build injects a composed backend). The synthetic "featuretest" scheme is
 * claimed by no base backend and is not reserved, so it exercises only the
 * feature path. This strong definition displaces db_select.c's weak default for
 * the whole test binary; it is inert for every other scheme, so the tests above
 * are unaffected. */
static const char *const feature_schemes[] = { "featuretest", NULL };
static const HlDbBackend feature_backend = {
    .name    = "featuretest",
    .schemes = feature_schemes,
};
static const HlDbBackend *const FEATURE_TABLE[] = { &feature_backend };
const HlDbBackend *const *hl_db_feature_backends(size_t *count)
{
    if (count) *count = 1;
    return FEATURE_TABLE;
}

UTEST(db_select, feature_backend_composed)
{
    const char *err = NULL;
    const HlDbBackend *b = hl_db_backend_select("featuretest://x", &err);
    ASSERT_TRUE(b);
    ASSERT_STREQ(b->name, "featuretest");
    ASSERT_FALSE(err);
    /* Base backends still win + a truly unknown scheme still errors. */
    err = NULL;
    ASSERT_FALSE(hl_db_backend_select("bogus://x", &err));
    ASSERT_TRUE(err);
}

UTEST_MAIN()
