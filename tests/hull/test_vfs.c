/*
 * test_vfs.c - Tests for the unified VFS module
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/vfs.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

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

/* ── hl_vfs_init_composed (base U runtime-feature stdlib) ─────────── */

/* A runtime-agnostic base (context/static/templates), sorted. */
static const HlEntry composed_base[] = {
    { "context:build",       (const unsigned char *)"b", 1 },
    { "static/hull/w.css",   (const unsigned char *)"c", 1 },
    { "templates/base.html", (const unsigned char *)"t", 1 },
    { 0, 0, 0 }
};

/* Two runtime archives' stdlib halves (Lua "hull.*", JS "hull:*"),
 * each individually sorted but interleaving with the base under strcmp. */
static const HlEntry composed_lua[] = {
    { "hull.json",     (const unsigned char *)"L", 1 },
    { "hull.template", (const unsigned char *)"L", 1 },
    { 0, 0, 0 }
};
static const HlEntry composed_js[] = {
    { "hull:json",     (const unsigned char *)"J", 1 },
    { "hull:template", (const unsigned char *)"J", 1 },
    { 0, 0, 0 }
};

UTEST(vfs, composed_empty_features_borrows_base)
{
    HlVfs vfs;
    void *owned = (void *)0x1;  /* poison: must be set to NULL */
    hl_vfs_init_composed(&vfs, composed_base, NULL, 0, NULL, &owned);

    /* No feature entries -> borrow the static base, no allocation. */
    ASSERT_TRUE(owned == NULL);
    ASSERT_EQ(vfs.count, (size_t)3);
    ASSERT_TRUE(vfs.entries == composed_base);
    hl_vfs_composed_free(owned);  /* NULL-safe */
}

UTEST(vfs, composed_merges_and_sorts)
{
    const HlEntry *const feats[] = { composed_lua, composed_js };
    HlVfs vfs;
    void *owned = NULL;
    hl_vfs_init_composed(&vfs, composed_base, feats, 2, NULL, &owned);

    /* 3 base + 2 lua + 2 js = 7, heap-merged (owned set). */
    ASSERT_TRUE(owned != NULL);
    ASSERT_EQ(vfs.count, (size_t)7);

    /* Sorted total order holds -> binary search finds every entry, base + both
     * runtimes, regardless of which array each came from. */
    ASSERT_TRUE(hl_vfs_find(&vfs, "context:build") != NULL);
    ASSERT_TRUE(hl_vfs_find(&vfs, "hull.json") != NULL);
    ASSERT_TRUE(hl_vfs_find(&vfs, "hull:template") != NULL);
    ASSERT_TRUE(hl_vfs_find(&vfs, "templates/base.html") != NULL);
    ASSERT_TRUE(hl_vfs_find(&vfs, "nope") == NULL);

    /* Entries are strcmp-ascending (the sort invariant hl_vfs_init asserts). */
    for (size_t i = 1; i < vfs.count; i++)
        ASSERT_LT(strcmp(vfs.entries[i - 1].name, vfs.entries[i].name), 0);

    hl_vfs_composed_free(owned);
}

/* Death test: the composed table is sealed READ-ONLY. Fork a child, write into
 * the merged array, and assert the child dies by signal. Without this a no-op
 * mprotect (or a dropped seal) would silently pass every other test. */
UTEST(vfs, composed_array_is_sealed_readonly)
{
    const HlEntry *const feats[] = { composed_lua, composed_js };
    HlVfs vfs;
    void *owned = NULL;
    hl_vfs_init_composed(&vfs, composed_base, feats, 2, NULL, &owned);
    ASSERT_TRUE(owned != NULL);   /* the merged (sealed) path was taken */

    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        /* Reset sanitizer SEGV/BUS handlers so we die by signal, not _exit(). */
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        HlEntry *e = (HlEntry *)(uintptr_t)&vfs.entries[0];
        e->len = 0xdeadu;         /* write into the RO mapping -> must fault */
        _exit(0);                 /* reached only if NOT sealed */
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status));   /* SIGSEGV / SIGBUS from the RO write */

    hl_vfs_composed_free(owned);
}

UTEST_MAIN();
