/*
 * test_vfs.c — Tests for the unified VFS module
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/vfs.h"

#include <string.h>

/* ── Test data: sorted entry arrays ───────────────────────────────── */

static const HlEntry sorted_entries[] = {
    { "./app",             (const unsigned char *)"app_code",    8 },
    { "./db",              (const unsigned char *)"db_code",     7 },
    { "./locales/en.json", (const unsigned char *)"{\"hi\":1}",  8 },
    { "./routes",          (const unsigned char *)"routes_code", 11 },
    { "migrations/001_init.sql", (const unsigned char *)"CREATE TABLE t(id INT);", 23 },
    { "migrations/002_add.sql",  (const unsigned char *)"ALTER TABLE t ADD col TEXT;", 27 },
    { "static/style.css",  (const unsigned char *)"body{}", 6 },
    { "templates/base.html",  (const unsigned char *)"<html></html>", 13 },
    { "templates/login.html", (const unsigned char *)"<form></form>", 13 },
    { 0, 0, 0 }
};

static const HlEntry empty_entries[] = {
    { 0, 0, 0 }
};

/* ── hl_vfs_init ──────────────────────────────────────────────────── */

UTEST(vfs, init_counts_entries)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, "/tmp/app");
    ASSERT_EQ(vfs.count, (size_t)9);
    ASSERT_STREQ(vfs.root_dir, "/tmp/app");
}

UTEST(vfs, init_empty)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, empty_entries, NULL);
    ASSERT_EQ(vfs.count, (size_t)0);
    ASSERT_EQ(vfs.root_dir, NULL);
}

/* ── hl_vfs_find ──────────────────────────────────────────────────── */

UTEST(vfs, find_exact_first)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    const HlEntry *e = hl_vfs_find(&vfs, "./app");
    ASSERT_NE(e, NULL);
    ASSERT_STREQ(e->name, "./app");
    ASSERT_EQ(e->len, 8u);
}

UTEST(vfs, find_exact_last)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    const HlEntry *e = hl_vfs_find(&vfs, "templates/login.html");
    ASSERT_NE(e, NULL);
    ASSERT_STREQ(e->name, "templates/login.html");
}

UTEST(vfs, find_exact_middle)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    const HlEntry *e = hl_vfs_find(&vfs, "migrations/001_init.sql");
    ASSERT_NE(e, NULL);
    ASSERT_STREQ(e->name, "migrations/001_init.sql");
}

UTEST(vfs, find_not_found)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    ASSERT_EQ(hl_vfs_find(&vfs, "nonexistent"), NULL);
    ASSERT_EQ(hl_vfs_find(&vfs, "./app.js"), NULL);
    ASSERT_EQ(hl_vfs_find(&vfs, ""), NULL);
}

UTEST(vfs, find_empty_vfs)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, empty_entries, NULL);

    ASSERT_EQ(hl_vfs_find(&vfs, "./app"), NULL);
}

/* ── hl_vfs_prefix ────────────────────────────────────────────────── */

UTEST(vfs, prefix_multiple_matches)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    const HlEntry *first = NULL;
    size_t count = hl_vfs_prefix(&vfs, "templates/", &first);

    ASSERT_EQ(count, (size_t)2);
    ASSERT_NE(first, NULL);
    ASSERT_STREQ(first[0].name, "templates/base.html");
    ASSERT_STREQ(first[1].name, "templates/login.html");
}

UTEST(vfs, prefix_single_match)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    const HlEntry *first = NULL;
    size_t count = hl_vfs_prefix(&vfs, "static/", &first);

    ASSERT_EQ(count, (size_t)1);
    ASSERT_NE(first, NULL);
    ASSERT_STREQ(first->name, "static/style.css");
}

UTEST(vfs, prefix_no_match)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    const HlEntry *first = NULL;
    size_t count = hl_vfs_prefix(&vfs, "nonexistent/", &first);

    ASSERT_EQ(count, (size_t)0);
    ASSERT_EQ(first, NULL);
}

UTEST(vfs, prefix_empty_vfs)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, empty_entries, NULL);

    const HlEntry *first = NULL;
    size_t count = hl_vfs_prefix(&vfs, "static/", &first);

    ASSERT_EQ(count, (size_t)0);
    ASSERT_EQ(first, NULL);
}

UTEST(vfs, prefix_dot_modules)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    const HlEntry *first = NULL;
    size_t count = hl_vfs_prefix(&vfs, "./", &first);

    ASSERT_EQ(count, (size_t)4);
    ASSERT_NE(first, NULL);
    ASSERT_STREQ(first[0].name, "./app");
}

UTEST(vfs, prefix_migrations)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    const HlEntry *first = NULL;
    size_t count = hl_vfs_prefix(&vfs, "migrations/", &first);

    ASSERT_EQ(count, (size_t)2);
    ASSERT_NE(first, NULL);
    ASSERT_STREQ(first[0].name, "migrations/001_init.sql");
    ASSERT_STREQ(first[1].name, "migrations/002_add.sql");
}

/* ── hl_vfs_has_prefix ────────────────────────────────────────────── */

UTEST(vfs, has_prefix_true)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    ASSERT_EQ(hl_vfs_has_prefix(&vfs, "static/"), 1);
    ASSERT_EQ(hl_vfs_has_prefix(&vfs, "templates/"), 1);
    ASSERT_EQ(hl_vfs_has_prefix(&vfs, "migrations/"), 1);
    ASSERT_EQ(hl_vfs_has_prefix(&vfs, "./"), 1);
}

UTEST(vfs, has_prefix_false)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    ASSERT_EQ(hl_vfs_has_prefix(&vfs, "nonexistent/"), 0);
    ASSERT_EQ(hl_vfs_has_prefix(&vfs, "vendor/"), 0);
}

UTEST(vfs, has_prefix_empty_vfs)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, empty_entries, NULL);

    ASSERT_EQ(hl_vfs_has_prefix(&vfs, "static/"), 0);
}

/* ── hl_vfs_path ──────────────────────────────────────────────────── */

UTEST(vfs, path_success)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, "/tmp/app");

    char buf[256];
    int n = hl_vfs_path(&vfs, "static/style.css", buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_STREQ(buf, "/tmp/app/static/style.css");
}

UTEST(vfs, path_overflow)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, "/tmp/app");

    char buf[10]; /* too small */
    int n = hl_vfs_path(&vfs, "static/style.css", buf, sizeof(buf));
    ASSERT_EQ(n, -1);
}

UTEST(vfs, path_null_root)
{
    HlVfs vfs;
    hl_vfs_init(&vfs, sorted_entries, NULL);

    char buf[256];
    int n = hl_vfs_path(&vfs, "static/style.css", buf, sizeof(buf));
    ASSERT_EQ(n, -1);
}

UTEST_MAIN();
