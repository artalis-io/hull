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
#include <signal.h>
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

UTEST(fs_resolve, symlink_interior_component_followed)
{
    setup();
    mkdirp_host("realdir"); wfile("realdir/leaf", "viaint");
    symln("realdir", "dsym");             /* symlink used as a NON-final component */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    int fd = hl_fs_open_at(root, "dsym/leaf", HL_FS_OPEN_READ, 0, &err);
    ASSERT_GE(fd, 0);                      /* must follow the interior symlink, not ENOTDIR */
    char b[64]; ASSERT_STREQ("viaint", slurp(fd, b, sizeof(b)));
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

/* Select the resolver implementation for a pass: 0 = default (openat2 fast path on
 * Linux, manual elsewhere), 1 = forced manual on every platform. force_manual()
 * re-reads the env, so this switches paths at runtime for parity testing. */
static void select_path(int pass)
{
    if (pass == 0) unsetenv("HL_FS_FORCE_MANUAL");
    else setenv("HL_FS_FORCE_MANUAL", "1", 1);
}

/* ── Fix 1: trailing-slash caller paths are rejected for READ/WRITE leaf modes,
 * identically on openat2 and the manual walk (a dir-shaped path must not open a
 * regular file, and WRITE must not create/truncate one). ──────────────────── */
UTEST(fs_resolve, trailing_slash_regular_file)
{
    setup();
    wfile("rf", "x");
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    for (int pass = 0; pass < 2; pass++) {
        select_path(pass);
        err = NULL;
        int fd = hl_fs_open_at(root, "rf/", HL_FS_OPEN_READ, 0, &err);
        ASSERT_EQ(fd, -1);
        ASSERT_STREQ("invalid_path", err);
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    close(root); teardown();
}
UTEST(fs_resolve, trailing_slash_directory)
{
    setup();
    mkdirp_host("d");
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    for (int pass = 0; pass < 2; pass++) {
        select_path(pass);
        err = NULL;
        int fd = hl_fs_open_at(root, "d/", HL_FS_OPEN_READ, 0, &err);
        ASSERT_EQ(fd, -1);              /* dir-shaped path rejected on both */
        ASSERT_STREQ("invalid_path", err);
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    close(root); teardown();
}
UTEST(fs_resolve, trailing_slash_symlink_to_file)
{
    setup();
    wfile("real", "x");
    symln("real", "l2f");               /* symlink to a regular file */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    for (int pass = 0; pass < 2; pass++) {
        select_path(pass);
        err = NULL;
        int fd = hl_fs_open_at(root, "l2f/", HL_FS_OPEN_READ, 0, &err);
        ASSERT_EQ(fd, -1);
        ASSERT_STREQ("invalid_path", err);
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    close(root); teardown();
}
UTEST(fs_resolve, trailing_slash_write_target)
{
    setup();
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    for (int pass = 0; pass < 2; pass++) {
        select_path(pass);
        err = NULL;
        int fd = hl_fs_open_at(root, "wt/", HL_FS_OPEN_WRITE, 0644, &err);
        ASSERT_EQ(fd, -1);              /* must NOT create/truncate a file at "wt/" */
        ASSERT_STREQ("invalid_path", err);
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    /* and no stray file was created at "wt" */
    char pth[512]; snprintf(pth, sizeof(pth), "%s/wt", base);
    struct stat st; ASSERT_NE(0, stat(pth, &st));
    close(root); teardown();
}

/* ── Fix 2: the HL_FS_MAX_DEPTH component limit is enforced BEFORE both
 * implementations, so openat2 and the manual walk agree at the boundary. ──── */
static char *deep_path(int n)   /* "c/c/.../c" with n components (caller owns) */
{
    char *b = (char *)malloc((size_t)n * 2 + 1);
    int o = 0;
    for (int i = 0; i < n; i++) { if (i) b[o++] = '/'; b[o++] = 'c'; }
    b[o] = '\0';
    return b;
}
UTEST(fs_resolve, depth_at_limit_accepted)
{
    setup();
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    char *p = deep_path(HL_FS_MAX_DEPTH);         /* exactly at the limit */
    for (int pass = 0; pass < 2; pass++) {
        select_path(pass);
        err = NULL;
        /* WRITE creates the parents + leaf; must NOT be rejected as too deep */
        int fd = hl_fs_open_at(root, p, HL_FS_OPEN_WRITE, 0644, &err);
        if (fd < 0) ASSERT_STRNE("path_too_deep", err ? err : "");
        else close(fd);
        err = NULL;
        int rfd = hl_fs_open_at(root, p, HL_FS_OPEN_READ, 0, &err);  /* also openat2 */
        if (rfd < 0) ASSERT_STRNE("path_too_deep", err ? err : "");
        else close(rfd);
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    free(p); close(root); teardown();
}
UTEST(fs_resolve, depth_over_limit_rejected)
{
    setup();
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    char *p = deep_path(HL_FS_MAX_DEPTH + 1);      /* boundary + 1 */
    for (int pass = 0; pass < 2; pass++) {
        select_path(pass);
        err = NULL;
        ASSERT_EQ(-1, hl_fs_open_at(root, p, HL_FS_OPEN_READ, 0, &err));
        ASSERT_STREQ("path_too_deep", err);        /* identical on both paths */
        err = NULL;
        ASSERT_EQ(-1, hl_fs_open_at(root, p, HL_FS_OPEN_WRITE, 0644, &err));
        ASSERT_STREQ("path_too_deep", err);
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    free(p); close(root); teardown();
}

/* ── Fix 3 (checkpoint-2 review): a non-directory INTERIOR component is CLASSIFIED
 * (fstatat) before being opened, so it is rejected as "not_a_directory" WITHOUT
 * being opened. This closes a resolution-hang DoS: opening an attacker-planted
 * interior FIFO O_RDONLY (no O_NONBLOCK) blocks forever. A bounded SIGALRM
 * watchdog proves resolution returns promptly instead of blocking. Rejected
 * identically by openat2 (ENOTDIR in-kernel) and the manual walk (fstatat).
 * The interior-symlink regression (symlink_interior_component_followed) above
 * proves a symlink is still followed, not misclassified.
 *
 * The FIFO watchdog test is compiled out on Cosmopolitan: cosmo's headers do
 * not declare mkfifo under the feature level Hull's tests use (the project sets
 * no feature macro for cosmo, per tests/sandbox_violation.c), and no other Hull
 * code needs it. The manual held-fd walk is identical C on every platform and
 * is no-block-proven here on macOS + Linux-forced-manual; interior_regular_file
 * _rejected (below, unguarded) still exercises the classify-before-open path on
 * cosmo. ──────────────────────────────────────────────────────────────────── */
#if !defined(__COSMOPOLITAN__)
static void on_resolve_timeout(int sig)
{
    (void)sig;
    static const char m[] =
        "FATAL: fs resolve BLOCKED on a non-directory interior component\n";
    ssize_t n = write(2, m, sizeof(m) - 1); (void)n;
    _exit(97);   /* fail the whole test binary loudly if the resolver ever hangs */
}

UTEST(fs_resolve, interior_fifo_rejected_without_blocking)
{
    setup();
    char fp[512]; snprintf(fp, sizeof(fp), "%s/fifo", base);
    ASSERT_EQ(0, mkfifo(fp, 0644));            /* interior FIFO, no writer -> would hang */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_resolve_timeout;
    sigaction(SIGALRM, &sa, NULL);

    for (int pass = 0; pass < 2; pass++) {     /* openat2 fast path + forced manual */
        select_path(pass);
        err = NULL;
        alarm(5);                              /* watchdog: fires only if resolve blocks */
        int fd = hl_fs_open_at(root, "fifo/leaf", HL_FS_OPEN_READ, 0, &err);
        alarm(0);                              /* completed promptly -> cancel */
        ASSERT_EQ(fd, -1);
        ASSERT_STREQ("not_a_directory", err);  /* classified, never opened */
    }
    /* WRITE mode (always the manual walk) must likewise classify, not block. */
    select_path(1);
    err = NULL;
    alarm(5);
    int wfd = hl_fs_open_at(root, "fifo/leaf", HL_FS_OPEN_WRITE, 0644, &err);
    alarm(0);
    ASSERT_EQ(wfd, -1);
    ASSERT_STREQ("not_a_directory", err);

    signal(SIGALRM, SIG_DFL);
    unsetenv("HL_FS_FORCE_MANUAL");
    close(root); teardown();
}
#endif  /* !__COSMOPOLITAN__ (mkfifo undeclared on cosmo) */

UTEST(fs_resolve, interior_regular_file_rejected)
{
    setup();
    wfile("rf", "x");                          /* interior regular file */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);
    for (int pass = 0; pass < 2; pass++) {
        select_path(pass);
        err = NULL;
        int fd = hl_fs_open_at(root, "rf/leaf", HL_FS_OPEN_READ, 0, &err);
        ASSERT_EQ(fd, -1);
        ASSERT_STREQ("not_a_directory", err);  /* rejected without opening */
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    close(root); teardown();
}

/* ── HL_FS_OPEN_DIR: resolve a directory, follow a symlinked dir contained, and
 * reject a non-directory target with not_a_directory (both impls). ───────────── */
UTEST(fs_resolve, open_dir_mode)
{
    setup();
    mkdirp_host("adir"); wfile("adir/leaf", "x");
    mkdirp_host("realdir2"); symln("realdir2", "dsym2");   /* symlink -> a real dir */
    wfile("plainfile", "y");
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);
    for (int pass = 0; pass < 2; pass++) {
        select_path(pass);
        err = NULL;
        int fd = hl_fs_open_at(root, "adir", HL_FS_OPEN_DIR, 0, &err);
        ASSERT_GE(fd, 0);
        struct stat st; ASSERT_EQ(0, fstat(fd, &st)); ASSERT_TRUE(S_ISDIR(st.st_mode));
        close(fd);

        err = NULL;
        fd = hl_fs_open_at(root, "dsym2", HL_FS_OPEN_DIR, 0, &err);
        ASSERT_GE(fd, 0);                       /* follows the symlinked dir, contained */
        close(fd);

        err = NULL;
        fd = hl_fs_open_at(root, "plainfile", HL_FS_OPEN_DIR, 0, &err);
        ASSERT_EQ(fd, -1); ASSERT_STREQ("not_a_directory", err);  /* a file is refused */
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    close(root); teardown();
}

/* ── HL_FS_SYMLINK_REFUSE: FOLLOW resolves symlinks, REFUSE denies them
 * (intermediate + terminal), on both openat2 and the forced-manual walk. ─────── */
UTEST(fs_resolve, symlink_refuse_mode)
{
    setup();
    mkdirp_host("rd"); wfile("rd/f", "x");
    symln("rd", "dl");            /* symlink -> dir (interior component) */
    symln("rd/f", "fl");          /* symlink -> file (terminal component) */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);
    for (int pass = 0; pass < 2; pass++) {
        select_path(pass);
        /* FOLLOW (the default) resolves both symlinks */
        err = NULL;
        int fd = hl_fs_open_at_ex(root, "dl/f", HL_FS_OPEN_READ, HL_FS_SYMLINK_FOLLOW, 0, &err);
        ASSERT_GE(fd, 0); close(fd);
        err = NULL;
        fd = hl_fs_open_at_ex(root, "fl", HL_FS_OPEN_READ, HL_FS_SYMLINK_FOLLOW, 0, &err);
        ASSERT_GE(fd, 0); close(fd);
        /* REFUSE denies an interior symlink... */
        err = NULL;
        fd = hl_fs_open_at_ex(root, "dl/f", HL_FS_OPEN_READ, HL_FS_SYMLINK_REFUSE, 0, &err);
        ASSERT_EQ(fd, -1); ASSERT_STREQ("symlink_denied", err);
        /* ...and a terminal symlink */
        err = NULL;
        fd = hl_fs_open_at_ex(root, "fl", HL_FS_OPEN_READ, HL_FS_SYMLINK_REFUSE, 0, &err);
        ASSERT_EQ(fd, -1); ASSERT_STREQ("symlink_denied", err);
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    close(root); teardown();
}

/* ── hl_fs_resolve_parent: (parent_fd, leaf) for a NO-FOLLOW metadata op ──────── */
UTEST(fs_resolve, parent_nested_and_root_and_leaf)
{
    setup();
    mkdirp_host("a"); mkdirp_host("a/b"); wfile("a/b/c.txt", "deep");
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);

    /* nested: parent walks a/b, leaf = "c.txt"; fstatat off parent_fd finds it. */
    HlFsParent pr; err = NULL;
    ASSERT_EQ(0, hl_fs_resolve_parent(root, "a/b/c.txt", HL_FS_SYMLINK_FOLLOW, &pr, &err));
    ASSERT_GE(pr.parent_fd, 0);
    ASSERT_STREQ("c.txt", pr.leaf);
    struct stat st;
    ASSERT_EQ(0, fstatat(pr.parent_fd, pr.leaf, &st, AT_SYMLINK_NOFOLLOW));
    ASSERT_TRUE(S_ISREG(st.st_mode));
    close(pr.parent_fd);

    /* single component: parent IS the anchor (root), leaf is the whole name. */
    wfile("top.txt", "T"); err = NULL;
    ASSERT_EQ(0, hl_fs_resolve_parent(root, "top.txt", HL_FS_SYMLINK_FOLLOW, &pr, &err));
    ASSERT_GE(pr.parent_fd, 0); ASSERT_STREQ("top.txt", pr.leaf);
    ASSERT_EQ(0, fstatat(pr.parent_fd, pr.leaf, &st, AT_SYMLINK_NOFOLLOW));
    close(pr.parent_fd);

    /* grant root ".": empty leaf, parent_fd is a dup of the anchor -> fstat the dir. */
    err = NULL;
    ASSERT_EQ(0, hl_fs_resolve_parent(root, ".", HL_FS_SYMLINK_FOLLOW, &pr, &err));
    ASSERT_GE(pr.parent_fd, 0); ASSERT_STREQ("", pr.leaf);
    ASSERT_EQ(0, fstat(pr.parent_fd, &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    close(pr.parent_fd);

    close(root); teardown();
}

/* ── parent resolution honours the per-kind symlink policy on the WALK, never the
 * terminal (REFUSE denies a symlinked intermediate; the leaf is never opened) ─── */
UTEST(fs_resolve, parent_refuses_symlink_intermediate)
{
    setup();
    mkdirp_host("real"); wfile("real/leaf.txt", "L");
    symln("real", "dl");                        /* dl -> real (dir symlink) */
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);

    HlFsParent pr;
    /* REFUSE: the symlinked intermediate "dl" is denied. */
    err = NULL;
    ASSERT_EQ(-1, hl_fs_resolve_parent(root, "dl/leaf.txt", HL_FS_SYMLINK_REFUSE, &pr, &err));
    ASSERT_STREQ("symlink_denied", err);
    ASSERT_EQ(-1, pr.parent_fd);
    /* FOLLOW: the same intermediate is followed contained; leaf resolved off it. */
    err = NULL;
    ASSERT_EQ(0, hl_fs_resolve_parent(root, "dl/leaf.txt", HL_FS_SYMLINK_FOLLOW, &pr, &err));
    ASSERT_GE(pr.parent_fd, 0); ASSERT_STREQ("leaf.txt", pr.leaf);
    struct stat st;
    ASSERT_EQ(0, fstatat(pr.parent_fd, pr.leaf, &st, AT_SYMLINK_NOFOLLOW));
    close(pr.parent_fd);

    close(root); teardown();
}

UTEST_MAIN();
