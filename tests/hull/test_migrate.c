/*
 * test_migrate.c - migration runner + default-database path checks (audit 25)
 *
 *  - hl_migrate_run / hl_migrate_status apply only TOP-LEVEL embedded
 *    migrations/<name>.sql entries: a nested migrations/down/*.sql never ran
 *    in development (discover_fs_migrations reads one level) and must not
 *    run in a built binary either.
 *  - hl_default_db_path_check refuses an implicit <app_dir>/data.db (or a
 *    SQLite sidecar of it) that is a symbolic link or not a regular file.
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
    if (sqlite3_exec(db, sql, count_cb, &v, NULL) != SQLITE_OK)
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

UTEST_MAIN();
