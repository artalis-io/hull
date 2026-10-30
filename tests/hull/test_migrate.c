/*
 * test_migrate.c - migration runner + default-database path checks (audit 25)
 *
 *  - hl_migrate_run / hl_migrate_status apply only TOP-LEVEL embedded
 *    migrations/<name>.sql entries: a nested migrations/down/*.sql never ran
 *    in development (discover_fs_migrations reads one level) and must not
 *    run in a built binary either.
 *  - hl_default_db_path_check refuses an implicit <app_dir>/data.db (or a
 *    SQLite sidecar of it) that is a symbolic link or not a regular file.
 *  - a development (filesystem) migrations/ that is a symbolic link, or a
 *    .sql in it that is one, fails the run (audit 26).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/migrate.h"
#include "hull/vfs.h"
#include "hull/entry.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"
#include "hull/shared/fs_util.h"
#include "test_tmpdir.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

static const char SQL_UP[]   = "CREATE TABLE t (x INTEGER);";
static const char SQL_DOWN[] = "DROP TABLE t;";
static const char SQL_TWO[]  = "INSERT INTO t VALUES (1);";
static const char TXT[]      = "not a migration";

/* Sorted in strcmp order, as hl_vfs_init requires. */
static const HlEntry nested_entries[] = {
    { "migrations/001_init.sql", (const unsigned char *)SQL_UP,
      sizeof SQL_UP - 1 },
    { "migrations/002_seed.sql", (const unsigned char *)SQL_TWO,
      sizeof SQL_TWO - 1 },
    { "migrations/README.txt", (const unsigned char *)TXT, sizeof TXT - 1 },
    { "migrations/down/001_init.sql", (const unsigned char *)SQL_DOWN,
      sizeof SQL_DOWN - 1 },
    { "migrations/down/002_seed.sql", (const unsigned char *)SQL_DOWN,
      sizeof SQL_DOWN - 1 },
    { NULL, NULL, 0 }
};

static int count_cb(void *ctx, int ncols, char **vals, char **names)
{
    (void)ncols; (void)names;
    *(int *)ctx = vals[0] ? atoi(vals[0]) : -1;
    return 0;
}

static int scalar(sqlite3 *db, const char *sql)
{
    int v = -1;
    /* Reads Hull's own _hull_migrations: trusted (audit 39). */
    int ns_prev = hl_cap_db_ns_trust(1);
    int rc = sqlite3_exec(db, sql, count_cb, &v, NULL);
    hl_cap_db_ns_restore(ns_prev);
    if (rc != SQLITE_OK)
        return -2;
    return v;
}

UTEST(migrate, nested_embedded_entries_are_not_applied)
{
    sqlite3 *db = NULL;
    hl_cap_db_sqlite_setup();
    ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
    ASSERT_EQ(0, hl_cap_db_init(db));
    HlDbHandle h = {0};
    ASSERT_EQ(0, hl_db_sqlite_wrap(&h, db));

    HlVfs vfs;
    hl_vfs_init(&vfs, nested_entries, NULL);

    /* The two top-level .sql files only: down/ would drop t. */
    EXPECT_EQ(2, hl_migrate_run(&h, &vfs));
    EXPECT_EQ(1, scalar(db, "SELECT count(*) FROM t"));
    EXPECT_EQ(2, scalar(db, "SELECT count(*) FROM _hull_migrations"));
    EXPECT_EQ(0, scalar(db, "SELECT count(*) FROM _hull_migrations "
                            "WHERE name LIKE '%/%' OR name LIKE '%.txt'"));

    /* Idempotent: nothing more to apply. */
    EXPECT_EQ(0, hl_migrate_run(&h, &vfs));
    EXPECT_EQ(1, scalar(db, "SELECT count(*) FROM t"));

    HlMigrationStatus *st = NULL;
    int n = -1;
    ASSERT_EQ(0, hl_migrate_status(&h, &vfs, &st, &n));
    EXPECT_EQ(2, n);
    for (int i = 0; i < n; i++) {
        EXPECT_EQ(NULL, strchr(st[i].name, '/'));
        EXPECT_EQ(1, st[i].applied);
    }
    hl_migrate_status_free(st, n);

    hl_db_sqlite_unwrap(&h);
    sqlite3_close(db);
}

UTEST(migrate, only_nested_entries_apply_nothing)
{
    static const HlEntry only_nested[] = {
        { "migrations/down/001_init.sql", (const unsigned char *)SQL_DOWN,
          sizeof SQL_DOWN - 1 },
        { NULL, NULL, 0 }
    };
    sqlite3 *db = NULL;
    hl_cap_db_sqlite_setup();
    ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
    ASSERT_EQ(0, hl_cap_db_init(db));
    HlDbHandle h = {0};
    ASSERT_EQ(0, hl_db_sqlite_wrap(&h, db));

    HlVfs vfs;
    hl_vfs_init(&vfs, only_nested, NULL);
    EXPECT_EQ(0, hl_migrate_run(&h, &vfs));
    EXPECT_EQ(0, scalar(db, "SELECT count(*) FROM _hull_migrations"));

    HlMigrationStatus *st = NULL;
    int n = -1;
    ASSERT_EQ(0, hl_migrate_status(&h, &vfs, &st, &n));
    EXPECT_EQ(0, n);
    hl_migrate_status_free(st, n);

    hl_db_sqlite_unwrap(&h);
    sqlite3_close(db);
}

/* ── hl_default_db_path_check ─────────────────────────────────────── */

static void make_dir(char *out, size_t sz, const char *tag)
{
    snprintf(out, sz, "%s/hull_test_migrate_%s_%d", hl_test_tmpdir(), tag,
             (int)getpid());
    mkdir(out, 0700);
}

static void touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs("x", f); fclose(f); }
}

UTEST(default_db, absent_and_regular_pass)
{
    char dir[PATH_MAX], db[PATH_MAX + 16];
    make_dir(dir, sizeof dir, "ok");
    snprintf(db, sizeof db, "%s/data.db", dir);
    unlink(db);

    char why[PATH_MAX + 200];
    EXPECT_EQ(0, hl_default_db_path_check(db, why, sizeof why));
    touch(db);
    EXPECT_EQ(0, hl_default_db_path_check(db, why, sizeof why));

    char wal[PATH_MAX + 32];
    snprintf(wal, sizeof wal, "%s-wal", db);
    touch(wal);
    EXPECT_EQ(0, hl_default_db_path_check(db, why, sizeof why));

    unlink(wal);
    unlink(db);
    rmdir(dir);
}

UTEST(default_db, symlink_refused)
{
    char dir[PATH_MAX], db[PATH_MAX + 16], target[PATH_MAX + 16];
    make_dir(dir, sizeof dir, "link");
    snprintf(db, sizeof db, "%s/data.db", dir);
    snprintf(target, sizeof target, "%s/other.sqlite", dir);
    touch(target);
    unlink(db);
    ASSERT_EQ(0, symlink(target, db));

    char why[PATH_MAX + 200];
    EXPECT_EQ(-1, hl_default_db_path_check(db, why, sizeof why));
    EXPECT_NE(NULL, strstr(why, "symbolic link"));

    /* A dangling link is refused too: SQLite would create its target. */
    unlink(target);
    EXPECT_EQ(-1, hl_default_db_path_check(db, NULL, 0));

    unlink(db);
    rmdir(dir);
}

UTEST(default_db, sidecar_symlink_refused)
{
    char dir[PATH_MAX], db[PATH_MAX + 16], target[PATH_MAX + 16];
    make_dir(dir, sizeof dir, "side");
    snprintf(db, sizeof db, "%s/data.db", dir);
    snprintf(target, sizeof target, "%s/elsewhere", dir);
    touch(db);
    touch(target);

    static const char *const sides[] = { "-wal", "-shm", "-journal" };
    for (size_t i = 0; i < 3; i++) {
        char side[PATH_MAX + 32];
        snprintf(side, sizeof side, "%s%s", db, sides[i]);
        unlink(side);
        ASSERT_EQ(0, symlink(target, side));
        EXPECT_EQ(-1, hl_default_db_path_check(db, NULL, 0));
        unlink(side);
    }
    EXPECT_EQ(0, hl_default_db_path_check(db, NULL, 0));

    unlink(target);
    unlink(db);
    rmdir(dir);
}

UTEST(default_db, directory_refused)
{
    char dir[PATH_MAX], db[PATH_MAX + 16];
    make_dir(dir, sizeof dir, "dir");
    snprintf(db, sizeof db, "%s/data.db", dir);
    mkdir(db, 0700);
    char why[PATH_MAX + 200];
    EXPECT_EQ(-1, hl_default_db_path_check(db, why, sizeof why));
    EXPECT_NE(NULL, strstr(why, "not a regular file"));
    rmdir(db);
    rmdir(dir);
}

/* ── filesystem migrations/: no symlinks (audit 26) ───────────────── */

static const HlEntry no_entries[] = { { NULL, NULL, 0 } };

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int open_mem(sqlite3 **db, HlDbHandle *h)
{
    hl_cap_db_sqlite_setup();
    if (sqlite3_open(":memory:", db) != SQLITE_OK) return -1;
    if (hl_cap_db_init(*db) != 0) return -1;
    return hl_db_sqlite_wrap(h, *db);
}

UTEST(migrate_fs, real_directory_applies)
{
    char app[PATH_MAX], p[PATH_MAX + 64];
    make_dir(app, sizeof app, "fs_real");
    snprintf(p, sizeof p, "%s/migrations", app);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/migrations/001_t.sql", app);
    write_file(p, SQL_UP);

    sqlite3 *db = NULL;
    HlDbHandle h = {0};
    ASSERT_EQ(0, open_mem(&db, &h));
    HlVfs vfs;
    hl_vfs_init(&vfs, no_entries, app);
    EXPECT_EQ(1, hl_migrate_run(&h, &vfs));
    EXPECT_EQ(0, scalar(db, "SELECT count(*) FROM t"));
    hl_db_sqlite_unwrap(&h);
    sqlite3_close(db);

    unlink(p);
    snprintf(p, sizeof p, "%s/migrations", app);
    rmdir(p);
    rmdir(app);
}

UTEST(migrate_fs, symlinked_directory_refused)
{
    char app[PATH_MAX], other[PATH_MAX], p[PATH_MAX + 64];
    make_dir(app, sizeof app, "fs_link_app");
    make_dir(other, sizeof other, "fs_link_other");
    snprintf(p, sizeof p, "%s/001_evil.sql", other);
    write_file(p, SQL_UP);
    char link[PATH_MAX + 64];
    snprintf(link, sizeof link, "%s/migrations", app);
    ASSERT_EQ(0, symlink(other, link));

    sqlite3 *db = NULL;
    HlDbHandle h = {0};
    ASSERT_EQ(0, open_mem(&db, &h));
    HlVfs vfs;
    hl_vfs_init(&vfs, no_entries, app);
    /* Refused - the run fails rather than reading "no migrations". */
    EXPECT_EQ(HL_MIGRATE_ERR, hl_migrate_run(&h, &vfs));
    EXPECT_EQ(-2, scalar(db, "SELECT count(*) FROM t"));   /* no table t */

    HlMigrationStatus *st = NULL;
    int n = -1;
    EXPECT_EQ(-1, hl_migrate_status(&h, &vfs, &st, &n));
    hl_db_sqlite_unwrap(&h);
    sqlite3_close(db);

    unlink(link);
    unlink(p);
    rmdir(other);
    rmdir(app);
}

UTEST(migrate_fs, symlinked_leaf_refused)
{
    char app[PATH_MAX], p[PATH_MAX + 64], tgt[PATH_MAX + 64], link[PATH_MAX + 64];
    make_dir(app, sizeof app, "fs_leaf");
    snprintf(p, sizeof p, "%s/migrations", app);
    mkdir(p, 0700);
    snprintf(tgt, sizeof tgt, "%s/elsewhere.txt", app);
    write_file(tgt, SQL_UP);
    snprintf(link, sizeof link, "%s/migrations/001_t.sql", app);
    ASSERT_EQ(0, symlink(tgt, link));

    sqlite3 *db = NULL;
    HlDbHandle h = {0};
    ASSERT_EQ(0, open_mem(&db, &h));
    HlVfs vfs;
    hl_vfs_init(&vfs, no_entries, app);
    EXPECT_EQ(HL_MIGRATE_ERR, hl_migrate_run(&h, &vfs));
    EXPECT_EQ(-2, scalar(db, "SELECT count(*) FROM t"));
    hl_db_sqlite_unwrap(&h);
    sqlite3_close(db);

    unlink(link);
    unlink(tgt);
    rmdir(p);
    rmdir(app);
}

/* Audit 27: the predicate the runner skips nested embedded entries with is
 * shared (the agent's schema-diff read them all). */
UTEST(migrate_embedded, only_top_level_sql_is_a_migration)
{
    HlEntry top    = { "migrations/001_init.sql", NULL, 0 };
    HlEntry nested = { "migrations/down/001_init.sql", NULL, 0 };
    HlEntry other  = { "migrations/README.md", NULL, 0 };
    HlEntry bare   = { "migrations/.sql", NULL, 0 };
    EXPECT_STREQ("001_init.sql", hl_migrate_embedded_name(&top));
    EXPECT_TRUE(hl_migrate_embedded_name(&nested) == NULL);
    EXPECT_TRUE(hl_migrate_embedded_name(&other) == NULL);
    EXPECT_TRUE(hl_migrate_embedded_name(&bare) == NULL);
}

UTEST_MAIN();
