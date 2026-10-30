/*
 * test_db_registry.c: Named connection registry (§1)
 *
 * Covers lazy open + caching, manifest DSN resolution (literal and
 * { dsn_env }), the seeded "default" connection, and error paths.
 * Uses SQLite :memory: DSNs so no external server is needed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/db_registry.h"
#include "hull/cap/db.h"   /* hl_cap_db_ns_trust */
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"
#include "hull/manifest.h"
#include "hull/utils/env_ref.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Seed a pre-opened SQLite connection as "default", then resolve it. */
UTEST(db_registry, seed_and_get_default)
{
    HlDbHandle def = {0};
    def.backend = &hl_db_backend_sqlite;
    ASSERT_EQ(0, hl_db_backend_sqlite.open(&def.ctx, ":memory:", NULL));

    HlDbRegistry *reg = hl_db_registry_create(NULL, NULL, NULL);
    ASSERT_TRUE(reg != NULL);
    ASSERT_EQ(0, hl_db_registry_seed(reg, "default", &def));

    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "default", &err);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(err == NULL);
    /* Same underlying connection as the seed (value-copied ctx). */
    ASSERT_EQ(h->ctx, def.ctx);
    ASSERT_TRUE(hl_db_exec(h, "CREATE TABLE t (x INTEGER)", NULL, 0) >= 0);

    hl_db_registry_destroy(reg);           /* must NOT close the seeded conn */
    def.backend->close(&def);              /* caller still owns it */
}

/* A manifest literal-DSN entry opens lazily and caches. */
UTEST(db_registry, manifest_lazy_open_and_cache)
{
    HlManifest m = {0};
    m.databases.named[0].name = "cache";
    m.databases.named[0].dsn = ":memory:";
    m.databases.named_count = 1;

    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    ASSERT_TRUE(reg != NULL);

    const char *err = NULL;
    HlDbHandle *h1 = hl_db_registry_get(reg, "cache", &err);
    ASSERT_TRUE(h1 != NULL);
    ASSERT_TRUE(err == NULL);
    ASSERT_STREQ(h1->backend->name, "sqlite");
    ASSERT_TRUE(hl_db_exec(h1, "CREATE TABLE c (x INTEGER)", NULL, 0) >= 0);

    /* Second get returns the SAME cached handle. */
    HlDbHandle *h2 = hl_db_registry_get(reg, "cache", &err);
    ASSERT_EQ(h1, h2);

    hl_db_registry_destroy(reg);           /* closes the owned "cache" conn */
}

/* A "$VAR" entry reads the DSN from the environment at open time. */
UTEST(db_registry, dsn_env_resolves)
{
    setenv("HULL_TEST_DB_DSN", ":memory:", 1);

    HlManifest m = {0};
    m.databases.named[0].name = "primary";
    m.databases.named[0].dsn = "$HULL_TEST_DB_DSN";   /* "$VAR" env ref */
    m.databases.named_count = 1;

    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "primary", &err);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(err == NULL);

    hl_db_registry_destroy(reg);
    unsetenv("HULL_TEST_DB_DSN");
}

/* An unset "$VAR" env var is a clear error, not a crash. */
UTEST(db_registry, dsn_env_unset_errors)
{
    unsetenv("HULL_TEST_MISSING_DSN");

    HlManifest m = {0};
    m.databases.named[0].name = "primary";
    m.databases.named[0].dsn = "$HULL_TEST_MISSING_DSN";
    m.databases.named_count = 1;

    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "primary", &err);
    ASSERT_TRUE(h == NULL);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(strstr(err, "unset") != NULL);

    hl_db_registry_destroy(reg);
}

/* An undeclared name fails closed. */
UTEST(db_registry, unknown_name_errors)
{
    HlManifest m = {0};   /* no databases */
    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "nope", &err);
    ASSERT_TRUE(h == NULL);
    ASSERT_TRUE(err != NULL);
    hl_db_registry_destroy(reg);
}

/* "default" falls back to the create-time default_dsn (the -d flag), and the
 * fast accessor returns the same open handle. */
UTEST(db_registry, default_dsn_and_accessor)
{
    HlDbRegistry *reg = hl_db_registry_create(NULL, ":memory:", NULL);
    ASSERT_TRUE(reg != NULL);
    /* Before first get, the accessor sees nothing open. */
    ASSERT_TRUE(hl_db_registry_default(reg) == NULL);

    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "default", &err);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(err == NULL);
    ASSERT_STREQ(h->backend->name, "sqlite");
    /* The accessor now returns the cached default (same pointer). */
    ASSERT_EQ(hl_db_registry_default(reg), h);

    hl_db_registry_destroy(reg);   /* owns + closes the default */
}

/* Closing the registry (the app context's step before the runtime's destroy)
 * keeps its memory but kills every handle in it: a Lua __gc finalizer that
 * lua_close runs later still holds a pointer to a slot, and must get an
 * error, not freed memory or a reopened connection (audit 17). */
UTEST(db_registry, close_all_keeps_the_memory_and_refuses_everything)
{
    HlManifest m = {0};
    m.databases.named[0].name = "cache";
    m.databases.named[0].dsn = ":memory:";
    m.databases.named_count = 1;
    m.databases.dynamic.declared = 1;

    HlDbRegistry *reg = hl_db_registry_create(&m, ":memory:", NULL);
    ASSERT_TRUE(reg != NULL);
    const char *err = NULL;
    HlDbHandle *def = hl_db_registry_get(reg, "default", &err);
    HlDbHandle *named = hl_db_registry_get(reg, "cache", &err);
    ASSERT_TRUE(def != NULL && named != NULL);
    ASSERT_TRUE(hl_db_registry_dynamic_policy(reg) != NULL);
    ASSERT_FALSE(hl_db_registry_is_closed(reg));

    hl_db_registry_close_all(reg);
    ASSERT_TRUE(hl_db_registry_is_closed(reg));

    /* The slots are still readable, with no backend: every call fails. */
    ASSERT_TRUE(def->backend == NULL && def->ctx == NULL);
    ASSERT_TRUE(named->backend == NULL);
    ASSERT_EQ(-1, hl_db_exec(def, "SELECT 1", NULL, 0));
    ASSERT_EQ(-1, hl_db_query(named, "SELECT 1", NULL, 0, NULL, NULL, NULL));

    /* Nothing resolves, and nothing reopens. */
    ASSERT_TRUE(hl_db_registry_default(reg) == NULL);
    err = NULL;
    ASSERT_TRUE(hl_db_registry_get(reg, "default", &err) == NULL);
    ASSERT_TRUE(err != NULL);
    err = NULL;
    ASSERT_TRUE(hl_db_registry_get(reg, "cache", &err) == NULL);
    ASSERT_TRUE(err != NULL);
    ASSERT_TRUE(hl_db_registry_internal(reg, &err) == NULL);
    ASSERT_TRUE(hl_db_registry_dynamic_policy(reg) == NULL);
    ASSERT_TRUE(hl_db_registry_dsn_for(reg, def) == NULL);
    ASSERT_TRUE(hl_db_registry_open_txn(reg) == NULL);
    hl_db_registry_guard_stale_txns(reg);   /* no open slot: a no-op */

    hl_db_registry_close_all(reg);          /* idempotent */
    hl_db_registry_destroy(reg);
}

/* The "$VAR" env-reference parser (used to resolve named-connection DSNs). */
UTEST(db_registry, env_ref_parsing)
{
    char v[128];
    ASSERT_TRUE(hl_env_ref("$FOO", v, sizeof v));
    ASSERT_STREQ(v, "FOO");
    ASSERT_TRUE(hl_env_ref("${BAR_BAZ2}", v, sizeof v));
    ASSERT_STREQ(v, "BAR_BAZ2");
    /* Literals: a bare path, and a value that merely CONTAINS '$' (a password). */
    ASSERT_FALSE(hl_env_ref("./cache.db", v, sizeof v));
    ASSERT_FALSE(hl_env_ref("postgres://u:p$w@h/db", v, sizeof v));
    /* Must be the WHOLE value; malformed forms are literal. */
    ASSERT_FALSE(hl_env_ref("$FOO extra", v, sizeof v));
    ASSERT_FALSE(hl_env_ref("${FOO", v, sizeof v));
    ASSERT_FALSE(hl_env_ref("$1BAD", v, sizeof v));
    ASSERT_FALSE(hl_env_ref("$$FOO", v, sizeof v));
    ASSERT_FALSE(hl_env_ref("$", v, sizeof v));
    ASSERT_FALSE(hl_env_ref(NULL, v, sizeof v));
}

/* ── databases.internal: the stdlib's own connection ──────────────────── */

UTEST(db_registry, internal_is_the_default_unless_declared)
{
    HlDbRegistry *reg = hl_db_registry_create(NULL, ":memory:", NULL);
    ASSERT_TRUE(reg != NULL);
    const char *err = NULL;
    HlDbHandle *def = hl_db_registry_get(reg, "default", &err);
    ASSERT_TRUE(def != NULL);
    EXPECT_FALSE(hl_db_registry_has_internal(reg));
    EXPECT_TRUE(hl_db_registry_internal(reg, &err) == def);
    hl_db_registry_destroy(reg);
}

UTEST(db_registry, a_declared_internal_dsn_is_its_own_connection)
{
    HlManifest m = {0};
    m.databases.internal = ":memory:";
    HlDbRegistry *reg = hl_db_registry_create(NULL, ":memory:", NULL);
    ASSERT_TRUE(reg != NULL);
    hl_db_registry_set_manifest(reg, &m);
    const char *err = NULL;
    HlDbHandle *def = hl_db_registry_get(reg, "default", &err);
    HlDbHandle *in  = hl_db_registry_internal(reg, &err);
    ASSERT_TRUE(def != NULL);
    ASSERT_TRUE(in != NULL);
    EXPECT_TRUE(in != def);                                    /* separate DB */
    EXPECT_TRUE(hl_db_registry_internal(reg, &err) == in);    /* cached */
    /* A worker resolves the same database for it. */
    EXPECT_STREQ(":memory:", hl_db_registry_dsn_for(reg, in));

    /* The internal tables live there, not in the app's database. (As the
     * stdlib: the SQLite authorizer refuses `_hull_*` otherwise, audit 39.) */
    int ns_prev = hl_cap_db_ns_trust(1);
    ASSERT_TRUE(hl_db_exec(in, "CREATE TABLE _hull_probe (x INTEGER)", NULL, 0) >= 0);
    EXPECT_TRUE(hl_db_exec(def, "INSERT INTO _hull_probe VALUES (1)", NULL, 0) < 0);
    hl_cap_db_ns_restore(ns_prev);
    hl_db_registry_destroy(reg);
}

UTEST(db_registry, app_code_cannot_name_the_internal_connection)
{
    HlManifest m = {0};
    m.databases.internal = ":memory:";
    HlDbRegistry *reg = hl_db_registry_create(&m, ":memory:", NULL);
    ASSERT_TRUE(reg != NULL);
    const char *err = NULL;
    ASSERT_TRUE(hl_db_registry_internal(reg, &err) != NULL);  /* now cached */
    err = NULL;
    EXPECT_TRUE(hl_db_registry_get(reg, "\x01internal", &err) == NULL);
    EXPECT_TRUE(err != NULL);
    hl_db_registry_destroy(reg);
}

UTEST(db_registry, an_internal_dsn_learned_before_wiring_is_used)
{
    /* A stdlib init() at app top level, before the manifest is wired. */
    HlDbRegistry *reg = hl_db_registry_create(NULL, ":memory:", NULL);
    ASSERT_TRUE(reg != NULL);
    EXPECT_FALSE(hl_db_registry_manifest_wired(reg));
    ASSERT_EQ(0, hl_db_registry_set_internal_dsn(reg, ":memory:"));
    EXPECT_TRUE(hl_db_registry_has_internal(reg));
    const char *err = NULL;
    HlDbHandle *def = hl_db_registry_get(reg, "default", &err);
    HlDbHandle *in  = hl_db_registry_internal(reg, &err);
    EXPECT_TRUE(in != NULL && in != def);

    /* Once sealed, it can no longer be swapped. */
    HlManifest m = {0};
    hl_db_registry_set_manifest(reg, &m);
    EXPECT_TRUE(hl_db_registry_manifest_wired(reg));
    ASSERT_EQ(0, hl_db_registry_seal(reg));
    EXPECT_EQ(-1, hl_db_registry_set_internal_dsn(reg, "postgres://evil/db"));
    hl_db_registry_destroy(reg);
}

UTEST(db_registry, a_network_default_without_internal_is_reported)
{
    HlDbRegistry *pg = hl_db_registry_create(NULL, "postgres://u@db/app", NULL);
    HlDbRegistry *sq = hl_db_registry_create(NULL, "./app.db", NULL);
    ASSERT_TRUE(pg != NULL && sq != NULL);
    EXPECT_TRUE(hl_db_registry_default_is_network(pg));
    EXPECT_FALSE(hl_db_registry_default_is_network(sq));
    hl_db_registry_destroy(pg);
    hl_db_registry_destroy(sq);
    /* The scheme is read without case, as the backend selector reads it
     * (audit 14). */
    static const char *const upper[] = {
        "PostgreSQL://u@db/app", "MYSQL://u@db/app", "MariaDB://u@db/app",
        /* DuckDB: its dynamic-SQL table functions also got past the text
         * check (audit 25), so the databases.internal WARN fires for it. */
        "duckdb:///tmp/app.duckdb", "DuckDB://./app.duckdb",
    };
    for (size_t i = 0; i < sizeof upper / sizeof upper[0]; i++) {
        HlDbRegistry *r = hl_db_registry_create(NULL, upper[i], NULL);
        ASSERT_TRUE(r != NULL);
        EXPECT_TRUE(hl_db_registry_default_is_network(r));
        hl_db_registry_destroy(r);
    }
}

/* hl_db_registry_open_txn names a registry connection inside a transaction
 * (what the runtimes refuse to wait across), and the guard rolls it back -
 * batch bookkeeping included (audit 5 M1 / L1). */
UTEST(db_registry, open_txn_names_the_connection_and_guard_clears_it)
{
    HlManifest m = {0};
    m.databases.named[0].name = "cache";
    m.databases.named[0].dsn = ":memory:";
    m.databases.named_count = 1;

    HlDbRegistry *reg = hl_db_registry_create(&m, NULL, NULL);
    ASSERT_TRUE(reg != NULL);
    ASSERT_TRUE(hl_db_registry_open_txn(reg) == NULL);   /* nothing open */

    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "cache", &err);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(hl_db_exec(h, "CREATE TABLE c (x INTEGER)", NULL, 0) >= 0);
    EXPECT_TRUE(hl_db_registry_open_txn(reg) == NULL);   /* autocommit */

    ASSERT_EQ(0, hl_db_batch_enter(h));
    ASSERT_TRUE(hl_db_exec(h, "INSERT INTO c VALUES (1)", NULL, 0) >= 0);
    const char *open = hl_db_registry_open_txn(reg);
    ASSERT_TRUE(open != NULL);
    EXPECT_STREQ(open, "cache");

    hl_db_registry_guard_stale_txns(reg);
    EXPECT_TRUE(hl_db_registry_open_txn(reg) == NULL);
    EXPECT_EQ(0, h->batch_depth);

    hl_db_registry_destroy(reg);
}


/* ── audit 34: manifest file DSNs stay inside the app tree ───────────── */

/* A fresh canonical directory "<tmp>/app" plus a sibling "<tmp>/outside.db"
 * path; returns 0 on success. */
static int confine_fixture(char *root, size_t rsz, char *app, size_t asz)
{
    const char *t = getenv("TMPDIR");
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "%s/hull_reg_XXXXXX", (t && t[0]) ? t : "/tmp");
    if (!mkdtemp(tmpl)) return -1;
    if (!realpath(tmpl, root)) return -1;
    (void)rsz;
    snprintf(app, asz, "%s/app", root);
    return mkdir(app, 0700);
}

static void rm_tree(const char *root)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    (void)system(cmd);
}

static HlDbHandle *get_named(const char *app, const char *dsn,
                             HlDbRegistry **out_reg, HlManifest *m,
                             const char **err)
{
    memset(m, 0, sizeof *m);
    m->databases.named[0].name = "cache";
    m->databases.named[0].dsn = dsn;
    m->databases.named_count = 1;
    HlDbRegistry *reg = hl_db_registry_create(m, NULL, NULL);
    *out_reg = reg;
    if (!reg) return NULL;
    if (app && hl_db_registry_set_app_dir(reg, app) != 0) return NULL;
    return hl_db_registry_get(reg, "cache", err);
}

/* A relative file DSN opens under the app directory, not the working
 * directory, and the worker pool is handed the same absolute file. */
UTEST(db_registry, relative_file_dsn_opens_under_the_app_dir)
{
    char root[PATH_MAX], app[PATH_MAX];
    ASSERT_EQ(0, confine_fixture(root, sizeof root, app, sizeof app));
    HlManifest m;
    HlDbRegistry *reg = NULL;
    const char *err = NULL;
    HlDbHandle *h = get_named(app, "data/../cache.db", &reg, &m, &err);
    EXPECT_TRUE(h == NULL);                       /* ".." refused */
    hl_db_registry_destroy(reg);

    err = NULL;
    h = get_named(app, "cache.db", &reg, &m, &err);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(hl_db_exec(h, "CREATE TABLE t (x INTEGER)", NULL, 0) >= 0);
    char want[PATH_MAX + 16];
    snprintf(want, sizeof want, "%s/cache.db", app);
    struct stat st;
    EXPECT_EQ(0, stat(want, &st));
    EXPECT_STREQ(want, hl_db_registry_dsn_for(reg, h));
    hl_db_registry_destroy(reg);

    /* sqlite:// keeps its scheme; an absolute path inside the tree is fine. */
    char dsn[PATH_MAX + 32];
    snprintf(dsn, sizeof dsn, "sqlite://%s/other.db", app);
    err = NULL;
    h = get_named(app, dsn, &reg, &m, &err);
    ASSERT_TRUE(h != NULL);
    EXPECT_STREQ(dsn, hl_db_registry_dsn_for(reg, h));
    hl_db_registry_destroy(reg);
    rm_tree(root);
}

/* A symlinked file, a symlinked directory, a symlinked sidecar and a path
 * outside the tree are all refused, and the outside file is never made. */
UTEST(db_registry, symlinked_or_escaping_file_dsn_is_refused)
{
    char root[PATH_MAX], app[PATH_MAX];
    ASSERT_EQ(0, confine_fixture(root, sizeof root, app, sizeof app));
    char outside[PATH_MAX + 16], p[PATH_MAX + 32];
    snprintf(outside, sizeof outside, "%s/outside.db", root);

    snprintf(p, sizeof p, "%s/cache.db", app);
    ASSERT_EQ(0, symlink("../outside.db", p));
    snprintf(p, sizeof p, "%s/sub", app);
    ASSERT_EQ(0, symlink("..", p));
    snprintf(p, sizeof p, "%s/side.db-wal", app);
    ASSERT_EQ(0, symlink("../outside.db", p));

    const char *cases[4];
    char abs_out[PATH_MAX + 32];
    snprintf(abs_out, sizeof abs_out, "%s", outside);
    cases[0] = "cache.db";
    cases[1] = "sub/outside.db";
    cases[2] = "side.db";
    cases[3] = abs_out;
    for (int i = 0; i < 4; i++) {
        HlManifest m;
        HlDbRegistry *reg = NULL;
        const char *err = NULL;
        HlDbHandle *h = get_named(app, cases[i], &reg, &m, &err);
        EXPECT_TRUE(h == NULL);
        EXPECT_TRUE(err != NULL);
        hl_db_registry_destroy(reg);
    }
    struct stat st;
    EXPECT_NE(0, lstat(outside, &st));
    rm_tree(root);
}

/* "$VAR" DSNs are checked after resolution; :memory: needs no app dir; a
 * file DSN with no app dir known is refused; the internal DSN is confined. */
UTEST(db_registry, env_memory_internal_and_no_app_dir)
{
    char root[PATH_MAX], app[PATH_MAX];
    ASSERT_EQ(0, confine_fixture(root, sizeof root, app, sizeof app));
    HlManifest m;
    HlDbRegistry *reg = NULL;
    const char *err = NULL;

    setenv("HULL_TEST_CONFINE_DSN", "../evil.db", 1);
    HlDbHandle *h = get_named(app, "$HULL_TEST_CONFINE_DSN", &reg, &m, &err);
    EXPECT_TRUE(h == NULL);
    hl_db_registry_destroy(reg);
    unsetenv("HULL_TEST_CONFINE_DSN");

    err = NULL;
    h = get_named(NULL, ":memory:", &reg, &m, &err);
    EXPECT_TRUE(h != NULL);
    hl_db_registry_destroy(reg);

    err = NULL;
    h = get_named(NULL, "cache.db", &reg, &m, &err);
    EXPECT_TRUE(h == NULL);
    hl_db_registry_destroy(reg);

    char outside[PATH_MAX + 16];
    snprintf(outside, sizeof outside, "%s/internal.db", root);
    HlManifest mi = {0};
    mi.databases.internal = outside;
    reg = hl_db_registry_create(&mi, ":memory:", NULL);
    ASSERT_TRUE(reg != NULL);
    ASSERT_EQ(0, hl_db_registry_set_app_dir(reg, app));
    err = NULL;
    EXPECT_TRUE(hl_db_registry_internal(reg, &err) == NULL);
    hl_db_registry_destroy(reg);
    struct stat st;
    EXPECT_NE(0, lstat(outside, &st));

    /* The operator's -d DSN is not checked (it is the operator's choice). */
    reg = hl_db_registry_create(NULL, outside, NULL);
    ASSERT_TRUE(reg != NULL);
    ASSERT_EQ(0, hl_db_registry_set_app_dir(reg, app));
    err = NULL;
    EXPECT_TRUE(hl_db_registry_get(reg, "default", &err) != NULL);
    hl_db_registry_destroy(reg);
    rm_tree(root);
}

UTEST_MAIN()
