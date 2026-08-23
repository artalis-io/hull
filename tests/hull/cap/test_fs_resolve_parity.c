/*
 * test_fs_resolve_parity.c — checkpoint 2: prove the two resolver implementations
 * (openat2 fast path + the portable manual held-fd-stack walk) implement the SAME
 * virtual-root contract, and that resolution is race-resistant.
 *
 * On Linux the READ path defaults to openat2; HL_FS_FORCE_MANUAL forces the manual
 * walk, so every case here runs through BOTH implementations on one host and their
 * outcomes (success + stable error tokens) are compared directly. macOS/cosmo have
 * only the manual walk, so both passes coincide there (still a useful self-check).
 *
 * Coverage:
 *   - a fixture battery of READ cases -> identical outcome + token on both impls;
 *   - the depth boundary (exactly HL_FS_MAX_DEPTH vs +1) -> identical on both;
 *   - the pathological symlink-expanded-depth case, where the manual walk is
 *     STRICTER (fail-closed "path_too_deep") than openat2 (kernel PATH_MAX/ELOOP) -
 *     a RATIFIED platform-resource-limit divergence (docs/hull_fs_resolver_parity.md),
 *     asserted explicitly rather than papered over;
 *   - a component-swap race: a swapper thread flips an interior component between a
 *     real dir and a symlink pointing OUT of the sandbox while the resolver runs;
 *     the resolved object must NEVER escape base_dir, on either implementation.
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
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
static void teardown(void)
{ if (nftw(base, rm_entry, 24, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT) {} }

static void wfile(const char *rel, const char *data)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel);
  FILE *f = fopen(p, "wb"); if (f) { fputs(data, f); fclose(f); } }
static void mkdirp_host(const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); mkdir(p, 0755); }
static void symln(const char *target, const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); unlink(p); symlink(target, p); }

/* Run one resolve through a chosen implementation; return fd (>=0) or -1, token via
 * *tok (NULL on success). Assertion-free so it is safe outside a UTEST body. */
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

static int build_tree(void)
{
    const char *err = NULL;
    wfile("f", "hello");
    mkdirp_host("d");
    wfile("d/g", "nested");
    symln("d/g", "rel");                 /* relative, in-base */
    symln("/d/g", "abs");                /* absolute -> re-root to base/d/g */
    symln("../../../../d/g", "up");      /* excess .. -> clamp to base/d/g */
    symln("lo", "lo");                   /* self loop */
    return hl_fs_open_base(base, &err);
}

/* ── fixture battery: identical outcome + token on both implementations ────── */
UTEST(fs_resolve_parity, read_battery)
{
    setup();
    int root = build_tree();
    ASSERT_GE(root, 0);

    struct { const char *path, *tok, *data; } cases[] = {
        { "f",   NULL, "hello"  },
        { "d/g", NULL, "nested" },
        { "rel", NULL, "nested" },                 /* relative symlink */
        { "abs", NULL, "nested" },                 /* absolute re-root */
        { "up",  NULL, "nested" },                 /* dotdot clamp */
        { "nope", "not_found", NULL },
        { "lo",   "symlink_loop", NULL },
        { "f/",   "invalid_path", NULL },          /* trailing slash */
        { "a/../../etc/passwd", "invalid_path", NULL }, /* caller ".." */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *mt = NULL, *ot = NULL;
        int mfd = resolve_via(root, cases[i].path, HL_FS_OPEN_READ, 1, &mt); /* manual */
        int ofd = resolve_via(root, cases[i].path, HL_FS_OPEN_READ, 0, &ot); /* openat2 */
        if (cases[i].tok) {
            ASSERT_EQ_MSG(mfd, -1, cases[i].path);
            ASSERT_EQ_MSG(ofd, -1, cases[i].path);
            ASSERT_STREQ_MSG(cases[i].tok, mt, cases[i].path);
            ASSERT_STREQ_MSG(cases[i].tok, ot, cases[i].path);  /* same token, both impls */
        } else {
            ASSERT_GE_MSG(mfd, 0, cases[i].path);
            ASSERT_GE_MSG(ofd, 0, cases[i].path);
            char bm[128], bo[128];
            slurp(mfd, bm, sizeof(bm)); slurp(ofd, bo, sizeof(bo));
            ASSERT_STREQ_MSG(bm, bo, cases[i].path);            /* identical content */
            if (cases[i].data) ASSERT_STREQ_MSG(cases[i].data, bm, cases[i].path);
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

    char *over = deep(HL_FS_MAX_DEPTH + 1);          /* boundary + 1: both reject */
    const char *mt = NULL, *ot = NULL;
    ASSERT_EQ(-1, resolve_via(root, over, HL_FS_OPEN_READ, 1, &mt));
    ASSERT_EQ(-1, resolve_via(root, over, HL_FS_OPEN_READ, 0, &ot));
    ASSERT_STREQ("path_too_deep", mt);
    ASSERT_STREQ("path_too_deep", ot);
    free(over);

    char *at = deep(HL_FS_MAX_DEPTH);                /* at limit: not "too deep" either impl */
    (void)resolve_via(root, at, HL_FS_OPEN_READ, 1, &mt);
    (void)resolve_via(root, at, HL_FS_OPEN_READ, 0, &ot);
    ASSERT_STRNE("path_too_deep", mt ? mt : "");
    ASSERT_STRNE("path_too_deep", ot ? ot : "");
    free(at);
    close(root); teardown();
}

/* ── RATIFIED divergence: symlink-expanded depth (Linux only) ────────────────
 * A symlink whose target expands the RESOLVED path past HL_FS_MAX_DEPTH: the manual
 * walk fails closed ("path_too_deep") because its held-fd stack is a platform
 * resource limit; openat2 resolves it within the kernel's own PATH_MAX/ELOOP. Both
 * stay CONTAINED (no authority escape); the difference is a documented, ratified
 * platform-resource-limit asymmetry, asserted explicitly. See
 * docs/hull_fs_resolver_parity.md. On macOS/cosmo both are the manual walk, so
 * there is no divergence to assert. */
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
UTEST(fs_resolve_parity, ratified_symlink_expanded_depth)
{
    setup();
    const char *err = NULL;
    /* create an existing HL_FS_MAX_DEPTH-level 'c' dir tree + a leaf file */
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
    symln(acc, "expand");   /* target is HL_FS_MAX_DEPTH components */

    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);
    const char *mt = NULL, *ot = NULL;
    int mfd = resolve_via(root, "expand/leaf", HL_FS_OPEN_READ, 1, &mt);   /* manual */
    int ofd = resolve_via(root, "expand/leaf", HL_FS_OPEN_READ, 0, &ot);   /* openat2 */

    ASSERT_EQ(mfd, -1);                       /* manual: fail-closed at its depth bound */
    ASSERT_STREQ("path_too_deep", mt);
    if (ofd >= 0) {
        /* openat2 in use: resolves within kernel limits -> the RATIFIED divergence */
        char b[16]; slurp(ofd, b, sizeof(b)); ASSERT_STREQ("deep", b); close(ofd);
    } else {
        /* openat2 unavailable on this kernel -> the "openat2" pass also fell back
         * to the manual walk, so both reject identically (no divergence to ratify) */
        ASSERT_STREQ("path_too_deep", ot);
    }

    close(root); teardown();
}
#endif

/* ── component-swap race: containment holds under concurrent mutation ──────── */
static volatile int g_swap_stop;
static char g_swap_path[512];
static void *swapper(void *arg)
{
    (void)arg;
    unsigned seed = 12345;
    while (!g_swap_stop) {
        rmdir(g_swap_path);
        unlink(g_swap_path);
        if (rand_r(&seed) & 1) mkdir(g_swap_path, 0755);
        else                   symlink("/", g_swap_path);   /* escaping if followed raw */
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
    snprintf(g_swap_path, sizeof(g_swap_path), "%s/a/mid", base);

    int root = hl_fs_open_base(base, &err);
    ASSERT_GE(root, 0);

    g_swap_stop = 0;
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, swapper, NULL));

    int escapes = 0, ok = 0, bad_tok = 0;
    for (int i = 0; i < 4000; i++) {
        for (int manual = 0; manual < 2; manual++) {
            const char *tok = NULL;
            int fd = resolve_via(root, "a/mid/f", HL_FS_OPEN_READ, manual, &tok);
            if (fd >= 0) {
                char b[64]; slurp(fd, b, sizeof(b)); close(fd);
                if (strcmp(b, "inbase") != 0) escapes++;   /* would mean an escape */
                else ok++;
            } else if (tok &&
                       strcmp(tok, "not_found") && strcmp(tok, "not_a_directory") &&
                       strcmp(tok, "symlink_loop") && strcmp(tok, "io_error") &&
                       strcmp(tok, "permission") && strcmp(tok, "path_too_deep") &&
                       strcmp(tok, "invalid_path")) {
                bad_tok++;                                  /* unexpected token */
            }
        }
    }
    g_swap_stop = 1;
    pthread_join(th, NULL);

    ASSERT_EQ(0, escapes);        /* never resolved anything outside base */
    ASSERT_EQ(0, bad_tok);        /* every failure was a known contained token */
    ASSERT_GT(ok, 0);             /* and the real in-base file did resolve sometimes */

    unlink(g_swap_path); rmdir(g_swap_path);
    close(root); teardown();
}

UTEST_MAIN();
