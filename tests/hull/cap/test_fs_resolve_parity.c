/*
 * test_fs_resolve_parity.c - prove the two resolver implementations
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
#include <setjmp.h>
#include <signal.h>
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
    symln("d", "dsym");                   /* a symlink used as an INTERIOR component */
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
        { "dsym/g", NULL, "nested" },              /* symlink as an INTERIOR component */
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

    /* Build HL_FS_MAX_DEPTH nested "c" dirs via DESCRIPTOR-RELATIVE mkdirat/openat.
     * A 256-component ABSOLUTE path is ~511 chars + the base prefix, which overflows
     * a PATH-buffer mkdir (mkdirp_host's char[512]) and would silently truncate the
     * tree short of the depth limit -> the manual walk would hit not_found before
     * path_too_deep. Held-fd descent has no absolute-path-length limit, so the tree
     * is truly HL_FS_MAX_DEPTH deep. `leaf` is created at the deepest level (via the
     * held fd, its absolute path is unrepresentable in a fixed buffer) so the openat2
     * side can resolve it and the ratified divergence is exercised, not vacuous. */
    int b = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    ASSERT_GE(b, 0);
    int cur = b;
    for (int i = 0; i < HL_FS_MAX_DEPTH; i++) {
        if (mkdirat(cur, "c", 0755) != 0 && errno != EEXIST) {
            ASSERT_EQ_MSG(0, 1, "mkdirat c");
        }
        int nxt = openat(cur, "c", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ASSERT_GE(nxt, 0);
        if (cur != b) close(cur);
        cur = nxt;
    }
    int lf = openat(cur, "leaf", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    ASSERT_GE(lf, 0);
    ssize_t wr = write(lf, "deep", 4);
    ASSERT_EQ((ssize_t)4, wr);
    close(lf);
    if (cur != b) close(cur);
    close(b);

    /* The symlink target: "c/c/.../c" (HL_FS_MAX_DEPTH components), relative to base
     * (expand's parent), so it re-roots to the physical deep tree built above. */
    char acc[HL_FS_MAX_DEPTH * 2 + 16];
    int ao = 0;
    for (int i = 0; i < HL_FS_MAX_DEPTH; i++) { if (i) acc[ao++] = '/'; acc[ao++] = 'c'; }
    acc[ao] = '\0';
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

/* The swapped component "a/mid" flips between: a symlink to "../real" (in-base ->
 * "a/mid/f" reads base/real/f = "inbase"), a symlink to the external sentinel dir
 * (absolute, out of base -> re-roots to a non-existent in-base path -> not_found),
 * and an empty directory (not_found). It NEVER contains the legitimate file, so it
 * can always be rmdir'd/unlinked, and the only source of "inbase" is base/real/f. */
/* Self-contained deterministic PRNG. NOT rand_r: its declaration is gated on
 * feature-test macros that differ across glibc / BSD / cosmo (this file defines
 * _XOPEN_SOURCE on Linux, which flips cosmo's libc into strict mode and hides the
 * non-standard rand_r). A tiny LCG is enough to pick among the three swap states
 * and keeps the sequence reproducible from the fixed seed on every platform. */
static unsigned lcg_next(unsigned *s)
{ *s = *s * 1103515245u + 12345u; return (*s >> 16) & 0x7fffu; }

static void *swapper(void *arg)
{
    (void)arg;
    unsigned seed = 0xC0FFEE;
    while (!atomic_load_explicit(&g_swap_stop, memory_order_relaxed)) {
        unlink(g_swap_path);
        rmdir(g_swap_path);
        switch (lcg_next(&seed) % 3) {
        case 0: symlink("../real", g_swap_path); break;   /* in-base -> reads inbase */
        case 1: symlink(g_ext_dir, g_swap_path); break;   /* escape -> not_found */
        default: mkdir(g_swap_path, 0755); break;         /* empty dir -> not_found */
        }
    }
    return NULL;
}

UTEST(fs_resolve_parity, component_swap_race_stays_contained)
{
    setup();
    const char *err = NULL;
    mkdirp_host("a");
    mkdirp_host("real");
    wfile("real/f", "inbase");                 /* the ONLY source of "inbase" */
    /* external sentinel: if containment ever failed and followed the escaping
     * symlink as a raw host path, "a/mid/f" would resolve to g_ext_dir/f. Content
     * is PID-unique and distinct from "inbase" so a mistaken read is unambiguous.
     * Its virtual-root RE-ROOTED path (base + "/tmp/hull_ext_PID") is never created,
     * so a re-rooted resolve is not_found - not a same-named in-base file that could
     * make a wrong read look legitimate (asserted below). */
    char sentinel[64]; snprintf(sentinel, sizeof(sentinel), "SECRET-SENTINEL-%d", (int)getpid());
    snprintf(g_ext_dir, sizeof(g_ext_dir), "/tmp/hull_ext_%d", (int)getpid());
    mkdir(g_ext_dir, 0755);
    char extf[300]; snprintf(extf, sizeof(extf), "%s/f", g_ext_dir);
    FILE *ef = fopen(extf, "wb"); if (ef) { fputs(sentinel, ef); fclose(ef); }
    snprintf(g_swap_path, sizeof(g_swap_path), "%s/a/mid", base);

    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);

    /* Pre-race invariant: with mid AS the escaping symlink, resolving "a/mid/f"
     * must yield NEITHER "inbase" (no same-named in-root confound) NOR the sentinel
     * (no escape) - it re-roots to a non-existent in-base path -> not_found. Proven
     * for BOTH implementations before the race so the loop's "read==inbase => ok"
     * is sound. */
    unlink(g_swap_path); rmdir(g_swap_path);
    ASSERT_EQ(0, symlink(g_ext_dir, g_swap_path));
    for (int manual = 0; manual < 2; manual++) {
        const char *tk = NULL;
        int fd = resolve_via(root, "a/mid/f", HL_FS_OPEN_READ, manual, &tk);
        if (fd >= 0) {
            char b[64]; slurp(fd, b, sizeof(b)); close(fd);
            ASSERT_STRNE_MSG("inbase", b, "escaping-symlink state read a same-named in-root file");
            ASSERT_STRNE_MSG(sentinel, b, "escaping-symlink state read the external sentinel");
        } else {
            ASSERT_STREQ_MSG("not_found", tk, "escaping-symlink state should re-root to not_found");
        }
    }
    unlink(g_swap_path); rmdir(g_swap_path);   /* clean initial state for the race */

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

/* ── leaf regular<->special swap: containment + no-hang under concurrent flip ───
 * A LEAF "leaf" is flipped between a regular file and a FIFO by a background thread.
 * Resolving it READ, repeatedly, through BOTH implementations, must ALWAYS resolve
 * to a REGULAR fd, or fail "not_a_regular_file" (the FIFO), or "not_found" (the
 * brief unlink gap) - never BLOCK (O_NONBLOCK), never hand back a non-regular fd,
 * never a different token - and leak no fd. This is the concurrent analogue of the
 * single-shot special-file rejection: the regular<->special TOCTOU can never turn a
 * regular open into a blocking / mistyped special-file open. */
#if !defined(__COSMOPOLITAN__)
static atomic_int g_leaf_stop;
static char g_leaf_path[512];

static void *leaf_swapper(void *arg)
{
    (void)arg;
    unsigned seed = 0x5EEDu;
    while (!atomic_load_explicit(&g_leaf_stop, memory_order_relaxed)) {
        unlink(g_leaf_path);
        if (lcg_next(&seed) & 1) {
            int fd = open(g_leaf_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { ssize_t w = write(fd, "reg", 3); (void)w; close(fd); }
        } else {
            mkfifo(g_leaf_path, 0644);
        }
    }
    return NULL;
}

/* per-call alarm watchdog: a blocked open (regression) trips SIGALRM -> siglongjmp. */
static sigjmp_buf g_leaf_wd;
static void leaf_wd_alarm(int s) { (void)s; siglongjmp(g_leaf_wd, 1); }

UTEST(fs_resolve_parity, leaf_regular_special_swap_stays_contained)
{
    setup();
    const char *err = NULL;
    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);
    snprintf(g_leaf_path, sizeof g_leaf_path, "%s/leaf", base);

    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa); sa.sa_handler = leaf_wd_alarm;
    sigaction(SIGALRM, &sa, &old);

    int fds_before = count_open_fds();
    atomic_store(&g_leaf_stop, 0);
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, leaf_swapper, NULL));

    const int ITERS = 8000;
    const long CAP_SEC = 60;
    time_t start = time(NULL);
    int reg = 0, rejected = 0, bad = 0, hung = 0, done = 0;
    char firstbad[96] = {0};
    for (int i = 0; i < ITERS && !done; i++) {
        if (time(NULL) - start > CAP_SEC) break;
        for (int manual = 0; manual < 2 && !done; manual++) {
            const char *tok = NULL;
            int fd = -1;
            if (sigsetjmp(g_leaf_wd, 1) == 0) {
                alarm(5);
                fd = resolve_via(root, "leaf", HL_FS_OPEN_READ, manual, &tok);
                alarm(0);
            } else { hung = 1; done = 1; break; }
            if (fd >= 0) {
                struct stat st;                          /* MUST be a regular file */
                if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) reg++;
                else { if (!firstbad[0]) snprintf(firstbad, sizeof firstbad,
                            "opened a NON-regular fd (mode=%o)", (unsigned)st.st_mode);
                       bad++; done = 1; }
                close(fd);
            } else if (tok && strcmp(tok, "not_a_regular_file") == 0) {
                rejected++;                              /* the FIFO, correctly refused */
            } else if (tok && strcmp(tok, "not_found") == 0) {
                /* brief unlink gap - contained */
            } else {
                if (!firstbad[0]) snprintf(firstbad, sizeof firstbad, "tok=%.40s", tok ? tok : "?");
                bad++; done = 1;
            }
        }
    }
    atomic_store(&g_leaf_stop, 1);
    pthread_join(th, NULL);
    alarm(0);
    sigaction(SIGALRM, &old, NULL);

    ASSERT_EQ_MSG(0, hung, "resolve BLOCKED on a FIFO leaf during the swap");
    ASSERT_EQ_MSG(0, bad, firstbad);
    ASSERT_GT(reg, 0);            /* the regular file resolved sometimes */
    ASSERT_GT(rejected, 0);      /* the FIFO was rejected sometimes */

    unlink(g_leaf_path);
    int fds_after = count_open_fds();
    char fdctx[64]; snprintf(fdctx, sizeof fdctx, "before=%d after=%d", fds_before, fds_after);
    ASSERT_GE_MSG(fds_before, fds_after, fdctx);   /* no fd leaked */
    close(root);
    teardown();
}
#endif /* !__COSMOPOLITAN__ */

UTEST_MAIN();
