/*
 * test_fs_resolve_parity.c — checkpoint 2: prove the two resolver implementations
 * (openat2 fast path + the portable manual held-fd-stack walk) implement the SAME
 * virtual-root contract, and that resolution is race-resistant.
 *
 * On Linux the READ path defaults to openat2; HL_FS_FORCE_MANUAL forces the manual
 * walk, so every case runs through BOTH implementations from the SAME fixture setup
 * and their outcomes (file contents, success/failure state, AND stable error token)
 * are compared directly. macOS/cosmo have only the manual walk (self-check).
 *
 * See docs/hull_fs_resolver_parity.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "utest.h"
#include "hull/cap/fs_resolve.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static char base[256];

static void setup(void)
{
    snprintf(base, sizeof(base), "/tmp/hull_par_%d", (int)getpid());
    mkdir(base, 0755);
}
static int rm_entry(const char *p, const struct stat *sb, int t, struct FTW *f)
{ (void)sb; (void)t; (void)f; return remove(p); }
static void rm_tree(const char *p)
{ if (nftw(p, rm_entry, 24, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT) {} }
static void teardown(void) { rm_tree(base); }

static void wfile(const char *rel, const char *data)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel);
  FILE *f = fopen(p, "wb"); if (f) { fputs(data, f); fclose(f); } }
static void mkdirp_host(const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); mkdir(p, 0755); }
static void symln(const char *target, const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); unlink(p); symlink(target, p); }

/* Assertion-free: run one resolve through a chosen implementation. */
static int resolve_via(int root, const char *path, HlFsOpenMode mode, int manual,
                       const char **tok)
{
    if (manual) setenv("HL_FS_FORCE_MANUAL", "1", 1);
    else        unsetenv("HL_FS_FORCE_MANUAL");
    const char *e = NULL;
    int fd = hl_fs_open_at(root, path, mode, 0644, &e);
    unsetenv("HL_FS_FORCE_MANUAL");
    *tok = (fd < 0) ? (e ? e : "?") : NULL;
    return fd;
}
static void slurp(int fd, char *b, size_t n)
{ ssize_t r = read(fd, b, n - 1); if (r < 0) r = 0; b[r] = '\0'; }

/* Count this process's open descriptors (leak guard). */
static int count_open_fds(void)
{
#if defined(__linux__)
    DIR *d = opendir("/proc/self/fd");
    if (d) { int n = 0; struct dirent *e;
        while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
        closedir(d); return n - 1; /* minus the opendir fd itself */
    }
#endif
    int n = 0;
    for (int fd = 0; fd < 512; fd++) if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

/* Whether openat2 is actually usable on this host (so the ratified divergence is
 * exercised, not silently dormant). Linux only; ENOSYS => unavailable. */
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
#include <sys/syscall.h>
#ifndef __NR_openat2
#define __NR_openat2 437
#endif
static int openat2_available(void)
{
    struct { uint64_t flags, mode, resolve; } how = { O_RDONLY | O_CLOEXEC, 0, 0 };
    long r = syscall(__NR_openat2, AT_FDCWD, ".", &how, sizeof(how));
    if (r >= 0) { close((int)r); return 1; }
    return errno != ENOSYS && errno != EPERM;
}
#endif

static int build_tree(void)
{
    const char *err = NULL;
    wfile("f", "hello");
    mkdirp_host("d");
    wfile("d/g", "nested");
    symln("d/g", "rel");
    symln("/d/g", "abs");
    symln("../../../../d/g", "up");
    symln("lo", "lo");
    return hl_fs_open_base(base, &err);
}

/* ── fixture battery: identical content + state + token on both impls ───────── */
UTEST(fs_resolve_parity, read_battery)
{
    setup();
    int root = build_tree();
    ASSERT_GE(root, 0);

    struct { const char *path, *tok, *data; } cases[] = {
        { "f",   NULL, "hello"  },
        { "d/g", NULL, "nested" },
        { "rel", NULL, "nested" },
        { "abs", NULL, "nested" },
        { "up",  NULL, "nested" },
        { "nope", "not_found", NULL },
        { "lo",   "symlink_loop", NULL },
        { "f/",   "invalid_path", NULL },
        { "a/../../etc/passwd", "invalid_path", NULL },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *mt = NULL, *ot = NULL;
        int mfd = resolve_via(root, cases[i].path, HL_FS_OPEN_READ, 1, &mt);
        int ofd = resolve_via(root, cases[i].path, HL_FS_OPEN_READ, 0, &ot);
        /* success/failure STATE matches */
        ASSERT_EQ_MSG((mfd >= 0), (ofd >= 0), cases[i].path);
        if (cases[i].tok) {
            ASSERT_EQ_MSG(mfd, -1, cases[i].path);
            ASSERT_STREQ_MSG(cases[i].tok, mt, cases[i].path);
            ASSERT_STREQ_MSG(cases[i].tok, ot, cases[i].path);
        } else {
            ASSERT_GE_MSG(mfd, 0, cases[i].path);
            ASSERT_GE_MSG(ofd, 0, cases[i].path);
            char bm[128], bo[128];
            slurp(mfd, bm, sizeof(bm)); slurp(ofd, bo, sizeof(bo));
            ASSERT_STREQ_MSG(bm, bo, cases[i].path);        /* contents match */
            ASSERT_STREQ_MSG(cases[i].data, bm, cases[i].path);
            close(mfd); close(ofd);
        }
    }
    close(root); teardown();
}

/* ── depth boundary parity ─────────────────────────────────────────────────── */
static char *deep(int n)
{ char *b = (char *)malloc((size_t)n * 2 + 1); int o = 0;
  for (int i = 0; i < n; i++) { if (i) b[o++] = '/'; b[o++] = 'c'; } b[o] = 0; return b; }

UTEST(fs_resolve_parity, depth_boundary)
{
    setup();
    int root = build_tree();
    char *over = deep(HL_FS_MAX_DEPTH + 1);
    const char *mt = NULL, *ot = NULL;
    ASSERT_EQ(-1, resolve_via(root, over, HL_FS_OPEN_READ, 1, &mt));
    ASSERT_EQ(-1, resolve_via(root, over, HL_FS_OPEN_READ, 0, &ot));
    ASSERT_STREQ("path_too_deep", mt);
    ASSERT_STREQ("path_too_deep", ot);
    free(over);
    char *at = deep(HL_FS_MAX_DEPTH);
    (void)resolve_via(root, at, HL_FS_OPEN_READ, 1, &mt);
    (void)resolve_via(root, at, HL_FS_OPEN_READ, 0, &ot);
    ASSERT_STRNE("path_too_deep", mt ? mt : "");
    ASSERT_STRNE("path_too_deep", ot ? ot : "");
    free(at);
    close(root); teardown();
}

/* ── RATIFIED divergence: symlink-expanded depth (Linux only) ────────────────
 * The ONLY allowlisted divergence. A symlink whose target expands the resolved
 * path past HL_FS_MAX_DEPTH: manual fails closed ("path_too_deep"); openat2
 * resolves within the kernel's PATH_MAX/ELOOP. Both contained; manual is stricter.
 * REQUIRED to be exercised where openat2 is available (not dormant). */
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
UTEST(fs_resolve_parity, ratified_symlink_expanded_depth)
{
    setup();
    const char *err = NULL;
    char acc[HL_FS_MAX_DEPTH * 2 + 16] = {0};
    int ao = 0;
    for (int i = 0; i < HL_FS_MAX_DEPTH; i++) {
        if (i) acc[ao++] = '/';
        acc[ao++] = 'c'; acc[ao] = '\0';
        mkdirp_host(acc);
    }
    char leafrel[HL_FS_MAX_DEPTH * 2 + 24];
    snprintf(leafrel, sizeof(leafrel), "%s/leaf", acc);
    wfile(leafrel, "deep");
    symln(acc, "expand");

    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);
    const char *mt = NULL, *ot = NULL;
    int mfd = resolve_via(root, "expand/leaf", HL_FS_OPEN_READ, 1, &mt);
    int ofd = resolve_via(root, "expand/leaf", HL_FS_OPEN_READ, 0, &ot);

    ASSERT_EQ(mfd, -1);                       /* manual: fail-closed */
    ASSERT_STREQ("path_too_deep", mt);
    if (openat2_available()) {
        /* divergence EXERCISED, not dormant: openat2 resolves within kernel limits */
        ASSERT_GE_MSG(ofd, 0, "openat2 present but did not exercise the ratified divergence");
        char b[16]; slurp(ofd, b, sizeof(b)); ASSERT_STREQ("deep", b); close(ofd);
    } else {
        /* no openat2 -> the second pass is also the manual walk -> both reject */
        ASSERT_EQ(ofd, -1);
        ASSERT_STREQ("path_too_deep", ot);
    }
    close(root); teardown();
}
#endif

/* ── component-swap race: containment under concurrent mutation ─────────────── */
static atomic_int g_swap_stop;
static char g_swap_path[512];    /* interior component flipped by the swapper */
static char g_ext_dir[256];      /* an EXTERNAL sentinel dir (outside base) */

static void *swapper(void *arg)
{
    (void)arg;
    unsigned seed = 0xC0FFEE;
    while (!atomic_load_explicit(&g_swap_stop, memory_order_relaxed)) {
        rmdir(g_swap_path);
        unlink(g_swap_path);
        if (rand_r(&seed) & 1) mkdir(g_swap_path, 0755);
        else                   symlink(g_ext_dir, g_swap_path);  /* absolute, out of base */
    }
    return NULL;
}

UTEST(fs_resolve_parity, component_swap_race_stays_contained)
{
    setup();
    const char *err = NULL;
    mkdirp_host("a");
    mkdirp_host("a/mid");
    wfile("a/mid/f", "inbase");
    /* external sentinel: if containment ever failed and followed the escaping
     * symlink as a raw host path, "a/mid/f" would resolve to g_ext_dir/f. */
    snprintf(g_ext_dir, sizeof(g_ext_dir), "/tmp/hull_ext_%d", (int)getpid());
    mkdir(g_ext_dir, 0755);
    char extf[300]; snprintf(extf, sizeof(extf), "%s/f", g_ext_dir);
    FILE *ef = fopen(extf, "wb"); if (ef) { fputs("SECRET-SENTINEL", ef); fclose(ef); }
    snprintf(g_swap_path, sizeof(g_swap_path), "%s/a/mid", base);

    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);

    int fds_before = count_open_fds();

    atomic_store(&g_swap_stop, 0);
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, swapper, NULL));

    const int ITERS = 8000;
    const long CAP_SEC = 60;              /* bounded wall clock */
    time_t start = time(NULL);
    int escapes = 0, ok = 0, bad_tok = 0, done = 0;
    int first_bad_iter = -1, first_bad_manual = -1;
    char first_bad[96] = {0};
    for (int i = 0; i < ITERS && !done; i++) {
        if (time(NULL) - start > CAP_SEC) break;
        for (int manual = 0; manual < 2; manual++) {
            const char *tok = NULL;
            int fd = resolve_via(root, "a/mid/f", HL_FS_OPEN_READ, manual, &tok);
            if (fd >= 0) {
                char b[64]; slurp(fd, b, sizeof(b)); close(fd);
                if (strcmp(b, "inbase") != 0) {          /* an escape / wrong object */
                    if (first_bad_iter < 0) { first_bad_iter = i; first_bad_manual = manual;
                        snprintf(first_bad, sizeof(first_bad), "read=%.40s", b); }
                    escapes++; done = 1;
                } else ok++;
            } else if (tok &&
                       strcmp(tok, "not_found") && strcmp(tok, "not_a_directory") &&
                       strcmp(tok, "symlink_loop") && strcmp(tok, "io_error") &&
                       strcmp(tok, "permission") && strcmp(tok, "path_too_deep") &&
                       strcmp(tok, "invalid_path")) {
                if (first_bad_iter < 0) { first_bad_iter = i; first_bad_manual = manual;
                    snprintf(first_bad, sizeof(first_bad), "tok=%.40s", tok); }
                bad_tok++; done = 1;
            }
        }
    }
    atomic_store(&g_swap_stop, 1);
    pthread_join(th, NULL);

    char ctx[160];
    snprintf(ctx, sizeof(ctx),
             "swapper_seed=0xC0FFEE first_bad_iter=%d impl=%s %s",
             first_bad_iter, first_bad_manual == 1 ? "manual" : "openat2", first_bad);
    ASSERT_EQ_MSG(0, escapes, ctx);       /* never read the external sentinel */
    ASSERT_EQ_MSG(0, bad_tok, ctx);       /* every failure was a contained token */
    ASSERT_GT(ok, 0);                     /* and the in-base file resolved sometimes */

    /* fd leak guard: no net descriptor growth across all iterations */
    unlink(g_swap_path); rmdir(g_swap_path);
    int fds_after = count_open_fds();
    char fdctx[64]; snprintf(fdctx, sizeof(fdctx), "before=%d after=%d", fds_before, fds_after);
    ASSERT_GE_MSG(fds_before, fds_after, fdctx);   /* fds_after <= fds_before: no leak */

    close(root);
    rm_tree(g_ext_dir);
    teardown();
}

UTEST_MAIN();
