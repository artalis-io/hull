/*
 * test_fs_resolve.c — descriptor-relative virtual-root resolver (checkpoint 1).
 *
 * Exercises hl_fs_open_at / hl_fs_open_base against a real temp tree: READ,
 * WRITE (mkdir-p + O_TRUNC), in-base symlink follow, absolute-symlink re-root,
 * ".." clamp, symlink loop, and the caller-path lexical pre-check. On macOS this
 * runs the manual held-fd-stack walk; on Linux it exercises the openat2 fast path
 * for READ + the manual walk for WRITE.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "utest.h"
#include "hull/cap/fs_resolve.h"
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static char base[256];

static void setup(void)
{
    snprintf(base, sizeof(base), "/tmp/hull_res_%d", (int)getpid());
    mkdir(base, 0755);
}
static int rm_entry(const char *p, const struct stat *sb, int t, struct FTW *f)
{ (void)sb; (void)t; (void)f; return remove(p); }
static void teardown(void)
{ if (nftw(base, rm_entry, 16, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT) {} }

/* helpers relative to base (real host paths, for fixture construction) */
static void wfile(const char *rel, const char *data)
{
    char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel);
    FILE *f = fopen(p, "wb"); if (f) { fputs(data, f); fclose(f); }
}
static void mkdirp_host(const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); mkdir(p, 0755); }
static void symln(const char *target, const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); unlink(p); symlink(target, p); }

static char *slurp(int fd, char *buf, size_t n)
{
    ssize_t r = read(fd, buf, n - 1); if (r < 0) r = 0; buf[r] = '\0'; return buf;
}

/* ── READ ─────────────────────────────────────────────────────────────── */
UTEST(fs_resolve, read_basic)
{
    setup();
    wfile("hello.txt", "world");
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);
    int fd = hl_fs_open_at(root, "hello.txt", HL_FS_OPEN_READ, 0, &err);
    ASSERT_GE(fd, 0);
    char b[64]; ASSERT_STREQ("world", slurp(fd, b, sizeof(b)));
    close(fd); close(root);
    teardown();
}

UTEST(fs_resolve, read_nested)
{
    setup();
    mkdirp_host("a"); mkdirp_host("a/b"); wfile("a/b/c.txt", "deep");
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "a/b/c.txt", HL_FS_OPEN_READ, 0, &err);
    ASSERT_GE(fd, 0);
    char b[64]; ASSERT_STREQ("deep", slurp(fd, b, sizeof(b)));
    close(fd); close(root);
    teardown();
}

UTEST(fs_resolve, read_not_found)
{
    setup();
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "nope.txt", HL_FS_OPEN_READ, 0, &err);
    ASSERT_EQ(fd, -1);
    ASSERT_STREQ("not_found", err);
    close(root);
    teardown();
}

/* ── caller-path lexical pre-check ────────────────────────────────────── */
UTEST(fs_resolve, caller_dotdot_rejected)
{
    setup();
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "a/../../etc/passwd", HL_FS_OPEN_READ, 0, &err);
    ASSERT_EQ(fd, -1);
    ASSERT_STREQ("invalid_path", err);
    close(root);
    teardown();
}
UTEST(fs_resolve, caller_absolute_rejected)
{
    setup();
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "/etc/passwd", HL_FS_OPEN_READ, 0, &err);
    ASSERT_EQ(fd, -1);
    ASSERT_STREQ("invalid_path", err);
    close(root);
    teardown();
}

/* ── WRITE: mkdir-p + O_TRUNC ─────────────────────────────────────────── */
UTEST(fs_resolve, write_creates_parents)
{
    setup();
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "out/deep/result.bin", HL_FS_OPEN_WRITE, 0644, &err);
    ASSERT_GE(fd, 0);
    ASSERT_EQ((ssize_t)5, write(fd, "abcde", 5));
    close(fd);
    /* verify via READ back through the resolver */
    fd = hl_fs_open_at(root, "out/deep/result.bin", HL_FS_OPEN_READ, 0, &err);
    ASSERT_GE(fd, 0);
    char b[64]; ASSERT_STREQ("abcde", slurp(fd, b, sizeof(b)));
    close(fd); close(root);
    teardown();
}

UTEST(fs_resolve, write_truncates)
{
    setup();
    wfile("f.txt", "abcdef");
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "f.txt", HL_FS_OPEN_WRITE, 0644, &err);
    ASSERT_GE(fd, 0);
    ASSERT_EQ((ssize_t)2, write(fd, "xy", 2));   /* shorter replacement */
    close(fd);
    fd = hl_fs_open_at(root, "f.txt", HL_FS_OPEN_READ, 0, &err);
    char b[64]; ASSERT_STREQ("xy", slurp(fd, b, sizeof(b)));  /* NOT "xycdef" */
    close(fd); close(root);
    teardown();
}

/* ── symlinks: virtual-root ───────────────────────────────────────────── */
UTEST(fs_resolve, symlink_in_base_followed)
{
    setup();
    mkdirp_host("real"); wfile("real/data", "payload");
    symln("real/data", "link");           /* relative, in-base */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "link", HL_FS_OPEN_READ, 0, &err);
    ASSERT_GE(fd, 0);
    char b[64]; ASSERT_STREQ("payload", slurp(fd, b, sizeof(b)));
    close(fd); close(root);
    teardown();
}

UTEST(fs_resolve, symlink_absolute_rerooted)
{
    setup();
    /* absolute target /etc/hostname must re-root to base/etc/hostname (absent)
     * -> not_found, and MUST NOT read the real host /etc/hostname. */
    symln("/etc/hostname", "abs");
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "abs", HL_FS_OPEN_READ, 0, &err);
    ASSERT_EQ(fd, -1);
    ASSERT_STREQ("not_found", err);
    close(root);
    teardown();
}

UTEST(fs_resolve, symlink_absolute_rerooted_hits_inbase)
{
    setup();
    mkdirp_host("etc"); wfile("etc/hostname", "sandboxed");
    symln("/etc/hostname", "abs");        /* re-roots to base/etc/hostname */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "abs", HL_FS_OPEN_READ, 0, &err);
    ASSERT_GE(fd, 0);
    char b[64]; ASSERT_STREQ("sandboxed", slurp(fd, b, sizeof(b)));
    close(fd); close(root);
    teardown();
}

UTEST(fs_resolve, symlink_dotdot_clamped)
{
    setup();
    wfile("top", "at-root");
    mkdirp_host("d");
    symln("../../../../top", "d/up");     /* excess .. clamps at base -> base/top */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "d/up", HL_FS_OPEN_READ, 0, &err);
    ASSERT_GE(fd, 0);
    char b[64]; ASSERT_STREQ("at-root", slurp(fd, b, sizeof(b)));
    close(fd); close(root);
    teardown();
}

UTEST(fs_resolve, symlink_loop_bounded)
{
    setup();
    symln("b", "a"); symln("a", "b");     /* a -> b -> a ... */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "a", HL_FS_OPEN_READ, 0, &err);
    ASSERT_EQ(fd, -1);
    ASSERT_STREQ("symlink_loop", err);
    close(root);
    teardown();
}

UTEST_MAIN();
