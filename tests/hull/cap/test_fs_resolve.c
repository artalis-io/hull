/*
 * test_fs_resolve.c - descriptor-relative virtual-root resolver .
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
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

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

/* ── special-file leaf rejection (O_NONBLOCK + regular-file type gate, §5a) ─────
 * A terminal READ/WRITE leaf that is a FIFO, socket, character/block device, or
 * directory is rejected with the single stable token "not_a_regular_file" and NEVER
 * blocks the open (O_NONBLOCK). Each case runs under a watchdog (proving no hang),
 * on BOTH the openat2 fast path and the forced-manual walk (Linux; macOS/cosmo are
 * always manual), and asserts no fd is leaked on the failure path. */

/* Lowest free fd number: a proxy for "no descriptor leaked" - stable across a
 * failing resolve iff every fd the resolver opened was closed. */
static int probe_lowest_fd(void)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    close(fd);
    return fd;
}

/* Watchdog: run hl_fs_open_at under a wall-clock alarm. If the open ever BLOCKS
 * (a regression - a special-file leaf without O_NONBLOCK), SIGALRM fires and
 * siglongjmps back with *hung=1 so the test fails loudly instead of hanging CI. */
static sigjmp_buf g_wd_jmp;
static void wd_alarm(int s) { (void)s; siglongjmp(g_wd_jmp, 1); }
static int open_watchdog(int root, const char *rel, HlFsOpenMode mode,
                         mode_t cm, const char **err, int *hung)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa); sa.sa_handler = wd_alarm;
    sigaction(SIGALRM, &sa, &old);
    *hung = 0;
    int fd = -1;
    if (sigsetjmp(g_wd_jmp, 1) == 0) {
        alarm(5);
        fd = hl_fs_open_at(root, rel, mode, cm, err);
        alarm(0);
    } else {
        *hung = 1;
    }
    sigaction(SIGALRM, &old, NULL);
    return fd;
}

/* Check that resolving `rel` in `mode` is rejected "not_a_regular_file" without
 * blocking and without leaking an fd, on BOTH implementations. Returns NULL on
 * success, or a static message naming the first failure (utest ASSERT_* macros
 * only work inside a UTEST body, so a plain helper reports via a message the caller
 * asserts on). */
static const char *check_special_rejected(const char *rel, HlFsOpenMode mode, mode_t cm)
{
    for (int m = 0; m < 2; m++) {
        if (m) setenv("HL_FS_FORCE_MANUAL", "1", 1); else unsetenv("HL_FS_FORCE_MANUAL");
        const char *err = NULL;
        int root = hl_fs_open_base(base, &err);
        if (root < 0) { unsetenv("HL_FS_FORCE_MANUAL"); return "open_base failed"; }
        int before = probe_lowest_fd();
        int hung = 0;
        err = NULL;
        int fd = open_watchdog(root, rel, mode, cm, &err, &hung);
        int after = probe_lowest_fd();
        close(root);
        unsetenv("HL_FS_FORCE_MANUAL");
        if (hung) return "resolve BLOCKED on a special-file leaf (hang)";
        if (fd != -1) { close(fd); return "expected rejection, got an open fd"; }
        if (!err || strcmp(err, "not_a_regular_file") != 0)
            return "wrong error token (expected not_a_regular_file)";
        if (before != after) return "fd leaked on the failure path";
    }
    return NULL;
}
/* Surface the helper's message through a utest assertion: NULL -> "OK" == "OK". */
#define ASSERT_REJECTED(r) do { const char *rr_ = (r); ASSERT_STREQ("OK", rr_ ? rr_ : "OK"); } while (0)

#if !defined(__COSMOPOLITAN__)
static int mkfifo_host(const char *rel)
{ char p[512]; snprintf(p, sizeof p, "%s/%s", base, rel); unlink(p); return mkfifo(p, 0644); }

UTEST(fs_resolve, read_fifo_rejected_no_hang)
{
    setup();
    ASSERT_EQ(0, mkfifo_host("pipe"));                   /* no writer: O_RDONLY would block */
    ASSERT_REJECTED(check_special_rejected("pipe", HL_FS_OPEN_READ, 0));
    teardown();
}

UTEST(fs_resolve, write_fifo_rejected_no_hang)
{
    setup();
    ASSERT_EQ(0, mkfifo_host("pipe"));                   /* no reader: O_WRONLY would block */
    ASSERT_REJECTED(check_special_rejected("pipe", HL_FS_OPEN_WRITE, 0644));
    teardown();
}
#endif /* !__COSMOPOLITAN__ */

/* AF_UNIX socket special file: open() fails ENXIO (Linux) / EOPNOTSUPP (macOS),
 * mapped to the same token as the fstat-gated types. */
static int mksocket_host(const char *rel)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", base, rel);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_un un;
    memset(&un, 0, sizeof un); un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", p);
    unlink(p);
    int r = bind(s, (struct sockaddr *)&un, sizeof un);  /* creates the fs node */
    close(s);                                            /* node persists after close */
    return r;
}

UTEST(fs_resolve, read_socket_rejected)
{
    setup();
    if (mksocket_host("sk") != 0) { teardown(); return; } /* skip if unsupported */
    ASSERT_REJECTED(check_special_rejected("sk", HL_FS_OPEN_READ, 0));
    teardown();
}

UTEST(fs_resolve, write_socket_rejected)
{
    setup();
    if (mksocket_host("sk") != 0) { teardown(); return; }
    ASSERT_REJECTED(check_special_rejected("sk", HL_FS_OPEN_WRITE, 0644));
    teardown();
}

/* A directory as a READ/WRITE LEAF is not a regular file -> same token. The READ
 * case also covers the residual-"." path (reading a directory that IS the resolved
 * root); WRITE-to-an-existing-directory-leaf surfaces EISDIR at open(). */
UTEST(fs_resolve, read_directory_leaf_rejected)
{
    setup();
    mkdirp_host("d");
    ASSERT_REJECTED(check_special_rejected("d", HL_FS_OPEN_READ, 0));
    teardown();
}

UTEST(fs_resolve, read_dot_directory_rejected)
{
    setup();
    for (int m = 0; m < 2; m++) {
        if (m) setenv("HL_FS_FORCE_MANUAL", "1", 1); else unsetenv("HL_FS_FORCE_MANUAL");
        const char *err = NULL;
        int root = hl_fs_open_base(base, &err); ASSERT_GE(root, 0);
        int before = probe_lowest_fd(), hung = 0;
        err = NULL;
        int fd = open_watchdog(root, ".", HL_FS_OPEN_READ, 0, &err, &hung);
        ASSERT_FALSE(hung);
        ASSERT_EQ(-1, fd);
        ASSERT_STREQ("not_a_regular_file", err);         /* reading the root dir as a file */
        ASSERT_EQ(before, probe_lowest_fd());
        close(root);
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    teardown();
}

UTEST(fs_resolve, write_directory_leaf_rejected)
{
    setup();
    mkdirp_host("d");
    ASSERT_REJECTED(check_special_rejected("d", HL_FS_OPEN_WRITE, 0644));
    teardown();
}

/* Character device where creatable (needs privilege): the same S_ISREG gate. Skips
 * cleanly when a device node cannot be made without privilege. */
#if !defined(__COSMOPOLITAN__)
UTEST(fs_resolve, read_chardev_rejected_if_creatable)
{
    setup();
    char p[512]; snprintf(p, sizeof p, "%s/cdev", base);
    if (mknod(p, S_IFCHR | 0644, 0) != 0) { teardown(); return; } /* skip w/o privilege */
    ASSERT_REJECTED(check_special_rejected("cdev", HL_FS_OPEN_READ, 0));
    teardown();
}
#endif

/* Regression: a REGULAR leaf still resolves, and the returned fd has O_NONBLOCK
 * CLEARED (ordinary blocking read/write semantics), on both implementations. */
UTEST(fs_resolve, regular_leaf_fd_is_blocking)
{
    setup();
    wfile("r.txt", "data");
    for (int m = 0; m < 2; m++) {
        if (m) setenv("HL_FS_FORCE_MANUAL", "1", 1); else unsetenv("HL_FS_FORCE_MANUAL");
        const char *err = NULL;
        int root = hl_fs_open_base(base, &err); ASSERT_GE(root, 0);
        err = NULL;
        int fd = hl_fs_open_at(root, "r.txt", HL_FS_OPEN_READ, 0, &err);
        ASSERT_GE(fd, 0);
        int fl = fcntl(fd, F_GETFL);
        ASSERT_GE(fl, 0);
        ASSERT_EQ(0, fl & O_NONBLOCK);                   /* cleared on the accepted fd */
        char b[16]; ASSERT_STREQ("data", slurp(fd, b, sizeof b));
        close(fd); close(root);
    }
    unsetenv("HL_FS_FORCE_MANUAL");
    teardown();
}

UTEST_MAIN();
