/*
 * test_fs_policy.c - hull.fs path-authorization policy core.
 *
 * Pure compile + select over a real fixture tree: grant parse (incl. rejections),
 * the four entry kinds (SUBTREE/EXACT/CREATE/PATTERN), deterministic most-specific
 * selection + shadowing, dedup/conflict, compile-time symlink refusal, the
 * PATTERN acceptance matrix, INIT/idempotent free, and allocator-leak accounting.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "utest.h"
#include "hull/cap/fs_policy.h"
#include "hull/utils/alloc.h"
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../test_tmpdir.h"
#include "hull/cap/policy_seal.h"
#include "hull/cap/fs.h"
#include "hull/cap/env.h"
#include "hull/runtime.h"
#include "hull/cap/db_registry.h"
#include "hull/manifest.h"
#include <sh_seal_arena.h>
#include <signal.h>
#include <sys/wait.h>

static char base[256];

static void setup(void) { hl_test_path(base, sizeof(base), "hull_pol_%d", (int)getpid()); mkdir(base, 0755); }
static int rm_entry(const char *p, const struct stat *sb, int t, struct FTW *f)
{ (void)sb; (void)t; (void)f; return remove(p); }
static void teardown(void) { if (nftw(base, rm_entry, 16, FTW_DEPTH | FTW_PHYS) != 0 && errno != ENOENT) {} }

static void mk_dir(const char *rel)  { char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); mkdir(p, 0755); }
static void mk_file(const char *rel) { char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel);
    FILE *f = fopen(p, "wb"); if (f) { fputs("x", f); fclose(f); } }
static void mk_link(const char *target, const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", base, rel); unlink(p); symlink(target, p); }

static void build_tree(void)
{
    mk_file("data.bin"); mk_file("sibling.bin"); mk_file("secret.txt");
    mk_dir("data"); mk_dir("data/private"); mk_dir("data/sub"); mk_dir("database");
    mk_file("data/a.csv"); mk_file("data/a.txt"); mk_file("data/.csv");
    mk_file("data/sub/a.csv"); mk_file("data/private/p.txt");
    mk_dir("logs"); mk_dir("logs/2026"); mk_file("logs/2026/a.txt");
    mk_link("data", "linkdir");            /* symlink -> real dir (compile-refusal case) */
    mk_link("secret.txt", "data/link.csv");/* pattern-matching symlink (runtime-refusal) */
}

/* Compile a policy from raw grant-string arrays; grants are freed here (compile
 * deep-copies). Returns compile rc; on success caller frees *out. */
static int build_policy(HlAllocator *a, const char **rd, size_t rn,
                        const char **wr, size_t wn, HlFsPolicy *out, const char **err)
{
    HlFsGrant rg[16], wg[16];
    size_t ri = 0, wi = 0;
    int rc = -1;
    const char *e = "?";
    for (; ri < rn; ri++) if (hl_fs_grant_parse(rd[ri], strlen(rd[ri]), a, &rg[ri], &e) != 0) goto cleanup;
    for (; wi < wn; wi++) if (hl_fs_grant_parse(wr[wi], strlen(wr[wi]), a, &wg[wi], &e) != 0) goto cleanup;
    rc = hl_fs_policy_compile(base, a, rg, rn, wg, wn, out, &e);
cleanup:
    for (size_t i = 0; i < ri; i++) hl_fs_grant_free(&rg[i]);
    for (size_t i = 0; i < wi; i++) hl_fs_grant_free(&wg[i]);
    if (err) *err = e;
    return rc;
}

/* ── grant parse: acceptance + rejections ────────────────────────────────────── */
UTEST(fs_policy, parse_ok_and_rejections)
{
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsGrant g = HL_FS_GRANT_INIT; const char *err = NULL;

    ASSERT_EQ(0, hl_fs_grant_parse("data/a.csv", 10, &a, &g, &err));
    ASSERT_EQ((size_t)2, g.n); ASSERT_EQ((size_t)2, g.first_pattern);   /* pure literal */
    ASSERT_EQ(0, g.directory_intent);
    hl_fs_grant_free(&g);

    ASSERT_EQ(0, hl_fs_grant_parse("uploads/", 8, &a, &g, &err));
    ASSERT_EQ((size_t)1, g.n); ASSERT_EQ(1, g.directory_intent);        /* trailing slash */
    hl_fs_grant_free(&g);

    ASSERT_EQ(0, hl_fs_grant_parse("data/*.csv", 10, &a, &g, &err));
    ASSERT_EQ((size_t)2, g.n); ASSERT_EQ((size_t)1, g.first_pattern);   /* pattern tail */
    hl_fs_grant_free(&g);

    /* rejections */
    ASSERT_EQ(-1, hl_fs_grant_parse("/etc/passwd", 11, &a, &g, &err)); ASSERT_STREQ("invalid_path", err);
    ASSERT_EQ(-1, hl_fs_grant_parse("a/../b", 6, &a, &g, &err));       ASSERT_STREQ("invalid_path", err);
    ASSERT_EQ(-1, hl_fs_grant_parse("", 0, &a, &g, &err));             ASSERT_STREQ("invalid_path", err);
    ASSERT_EQ(-1, hl_fs_grant_parse("data\0/x", 7, &a, &g, &err));     ASSERT_STREQ("invalid_path", err); /* embedded NUL */
    ASSERT_EQ(-1, hl_fs_grant_parse("a/b?c", 5, &a, &g, &err));        ASSERT_STREQ("unsupported_pattern", err);
    ASSERT_EQ(-1, hl_fs_grant_parse("a/**/b", 6, &a, &g, &err));       ASSERT_STREQ("unsupported_pattern", err);
    ASSERT_EQ(-1, hl_fs_grant_parse("data/*/", 7, &a, &g, &err));      ASSERT_STREQ("unsupported_pattern", err); /* trailing-slash pattern */

    ASSERT_EQ((size_t)0, hl_alloc_used(&a));   /* every parsed grant freed */
}

/* ── EXACT: file allowed, sibling denied ─────────────────────────────────────── */
UTEST(fs_policy, exact_allows_file_denies_sibling)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data.bin" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    ASSERT_EQ((size_t)1, p.read_n);
    ASSERT_EQ((int)HL_FS_ENTRY_EXACT, (int)p.read[0].kind);

    char sc[256];
    HlFsSelection s = hl_fs_policy_select(&p, "data.bin", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL); ASSERT_STREQ("data.bin", s.residual);  /* residual under parent anchor */

    s = hl_fs_policy_select(&p, "sibling.bin", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry == NULL); ASSERT_STREQ("permission", s.err);

    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── SUBTREE: descendants; component-aware ("data" != "database") ────────────── */
UTEST(fs_policy, subtree_descendants_component_aware)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    ASSERT_EQ((int)HL_FS_ENTRY_SUBTREE, (int)p.read[0].kind);

    char sc[256];
    HlFsSelection s = hl_fs_policy_select(&p, "data/sub/a.csv", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL); ASSERT_STREQ("sub/a.csv", s.residual);
    s = hl_fs_policy_select(&p, "data", HL_FS_OPEN_READ, sc, sizeof(sc));   /* grant root */
    ASSERT_TRUE(s.entry != NULL); ASSERT_STREQ(".", s.residual);
    s = hl_fs_policy_select(&p, "database/x", HL_FS_OPEN_READ, sc, sizeof(sc)); /* NOT under data */
    ASSERT_TRUE(s.entry == NULL); ASSERT_STREQ("permission", s.err);

    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── most-specific: data/ + data/private/ selects the deeper grant ───────────── */
UTEST(fs_policy, most_specific_wins)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data", "data/private" };
    ASSERT_EQ(0, build_policy(&a, rd, 2, NULL, 0, &p, &err));
    ASSERT_EQ((size_t)2, p.read_n);

    char sc[256];
    /* a path under data/private selects the deeper (2-component) grant: residual is
     * relative to the data/private anchor, so just the leaf. */
    HlFsSelection s = hl_fs_policy_select(&p, "data/private/p.txt", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL);
    ASSERT_EQ((size_t)2, s.entry->grant_n);        /* the data/private entry */
    ASSERT_STREQ("p.txt", s.residual);
    /* a path only under data selects data (residual keeps the deeper components) */
    s = hl_fs_policy_select(&p, "data/a.csv", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL); ASSERT_EQ((size_t)1, s.entry->grant_n); ASSERT_STREQ("a.csv", s.residual);

    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── CREATE (write): out/result.bin, out absent -> anchor at base, deny sibling ─ */
UTEST(fs_policy, create_write_terminal_file)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *wr[] = { "out/result.bin" };       /* out/ absent */
    ASSERT_EQ(0, build_policy(&a, NULL, 0, wr, 1, &p, &err));
    ASSERT_EQ((size_t)1, p.write_n);
    ASSERT_EQ((int)HL_FS_ENTRY_CREATE, (int)p.write[0].kind);
    ASSERT_EQ((size_t)0, p.write[0].anchor_depth);  /* nearest existing ancestor = base */

    char sc[256];
    HlFsSelection s = hl_fs_policy_select(&p, "out/result.bin", HL_FS_OPEN_WRITE, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL); ASSERT_STREQ("out/result.bin", s.residual);
    s = hl_fs_policy_select(&p, "out/other.bin", HL_FS_OPEN_WRITE, sc, sizeof(sc));
    ASSERT_TRUE(s.entry == NULL); ASSERT_STREQ("permission", s.err);
    /* write grant is NOT selectable for read (independent sets) */
    s = hl_fs_policy_select(&p, "out/result.bin", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry == NULL); ASSERT_STREQ("permission", s.err);

    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── empty policy denies everything (fails closed) ───────────────────────────── */
UTEST(fs_policy, empty_policy_denies_all)
{
    setup();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    ASSERT_EQ(0, build_policy(&a, NULL, 0, NULL, 0, &p, &err));
    char sc[256];
    HlFsSelection s = hl_fs_policy_select(&p, "anything", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry == NULL); ASSERT_STREQ("permission", s.err);
    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── PATTERN acceptance matrix (data + *.csv) ────────────────────────────────── */
UTEST(fs_policy, pattern_acceptance_matrix)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data/*.csv" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    ASSERT_EQ((int)HL_FS_ENTRY_PATTERN, (int)p.read[0].kind);

    char sc[256];
    #define SEL(path) hl_fs_policy_select(&p, path, HL_FS_OPEN_READ, sc, sizeof(sc))
    ASSERT_TRUE(SEL("data/a.csv").entry != NULL);          /* allowed */
    ASSERT_STREQ("a.csv", SEL("data/a.csv").residual);      /* residual = literal caller name */
    ASSERT_TRUE(SEL("data/.csv").entry != NULL);           /* '*' matches zero bytes */
    ASSERT_TRUE(SEL("data/a.txt").entry == NULL);          /* denied: extension mismatch */
    ASSERT_TRUE(SEL("data/sub/a.csv").entry == NULL);      /* denied: '*' never crosses '/' */
    ASSERT_TRUE(SEL("data/a.csv/x").entry == NULL);        /* denied: pattern is terminal */
    /* a symlink NAME matching the pattern still SELECTS at this pure layer; the
     * symlink REFUSAL is enforced when the resolver opens the residual O_NOFOLLOW. */
    ASSERT_TRUE(SEL("data/link.csv").entry != NULL);
    ASSERT_STREQ("link.csv", SEL("data/link.csv").residual);
    #undef SEL

    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── shadowing: narrower PATTERN outranks the broader SUBTREE ─────────────────── */
UTEST(fs_policy, pattern_shadows_subtree)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data", "data/*.csv" };
    ASSERT_EQ(0, build_policy(&a, rd, 2, NULL, 0, &p, &err));
    ASSERT_EQ((size_t)2, p.read_n);

    char sc[256];
    /* data/a.csv matches both; the PATTERN entry (more literal bytes) shadows the
     * SUBTREE - most-specific-wins, not union. */
    HlFsSelection s = hl_fs_policy_select(&p, "data/a.csv", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL); ASSERT_EQ((int)HL_FS_ENTRY_PATTERN, (int)s.entry->kind);
    /* data/a.txt matches only the SUBTREE -> allowed via SUBTREE */
    s = hl_fs_policy_select(&p, "data/a.txt", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL); ASSERT_EQ((int)HL_FS_ENTRY_SUBTREE, (int)s.entry->kind);

    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── multi-component PATTERN selection (existing + missing intermediate) ──────── */
UTEST(fs_policy, multi_component_pattern)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "logs/*/*.txt" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    ASSERT_EQ((int)HL_FS_ENTRY_PATTERN, (int)p.read[0].kind);
    ASSERT_EQ((size_t)1, p.read[0].first_pattern);   /* "logs" literal, then two patterns */
    ASSERT_EQ((size_t)1, p.read[0].anchor_depth);    /* logs/ exists */

    char sc[256];
    HlFsSelection s = hl_fs_policy_select(&p, "logs/2026/a.txt", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL); ASSERT_STREQ("2026/a.txt", s.residual);  /* literal residual */
    s = hl_fs_policy_select(&p, "logs/2026/a.csv", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry == NULL);                    /* .csv != *.txt */

    /* missing intermediate: write grant whose whole pattern-prefix dir is absent */
    HlFsPolicy p2 = HL_FS_POLICY_INIT;
    const char *wr[] = { "arch/*/*.txt" };            /* arch/ absent */
    ASSERT_EQ(0, build_policy(&a, NULL, 0, wr, 1, &p2, &err));
    ASSERT_EQ((int)HL_FS_ENTRY_PATTERN, (int)p2.write[0].kind);
    ASSERT_EQ((size_t)0, p2.write[0].anchor_depth);  /* anchor falls back to base */
    s = hl_fs_policy_select(&p2, "arch/x/y.txt", HL_FS_OPEN_WRITE, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL); ASSERT_STREQ("arch/x/y.txt", s.residual);
    hl_fs_policy_free(&p2);

    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── dedup + conflict rejection ──────────────────────────────────────────────── */
UTEST(fs_policy, dedup_and_conflict)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;

    /* identical grants deduplicate to one entry */
    const char *rd[] = { "data", "data" };
    ASSERT_EQ(0, build_policy(&a, rd, 2, NULL, 0, &p, &err));
    ASSERT_EQ((size_t)1, p.read_n);
    hl_fs_policy_free(&p);

    /* same components, conflicting terminal intent (file vs dir) -> rejected */
    const char *rd2[] = { "conf", "conf/" };
    ASSERT_EQ(-1, build_policy(&a, rd2, 2, NULL, 0, &p, &err));
    ASSERT_STREQ("conflicting_grant", err);

    ASSERT_EQ((size_t)0, hl_alloc_used(&a));   /* failed compile leaks nothing */
    teardown();
}

/* ── compile-time symlink refusal (EXACT/PATTERN literal prefix is a symlink) ─── */
UTEST(fs_policy, compile_refuses_symlink_prefix)
{
    setup(); build_tree();      /* linkdir -> data (a symlink) */
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;

    const char *rd[] = { "linkdir/x.bin" };            /* EXACT/CREATE via a symlink prefix */
    ASSERT_EQ(-1, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    ASSERT_STREQ("symlink_denied", err);

    const char *rd2[] = { "linkdir/*.csv" };           /* PATTERN via a symlink prefix */
    ASSERT_EQ(-1, build_policy(&a, rd2, 1, NULL, 0, &p, &err));
    ASSERT_STREQ("symlink_denied", err);

    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── invalid mode / scratch too small / INIT + idempotent free ───────────────── */
UTEST(fs_policy, error_paths_and_free_idempotent)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));

    char sc[256];
    HlFsSelection s = hl_fs_policy_select(&p, "data/x", (HlFsOpenMode)999, sc, sizeof(sc));
    ASSERT_TRUE(s.entry == NULL); ASSERT_STREQ("permission", s.err);           /* invalid mode */
    char tiny[2];
    s = hl_fs_policy_select(&p, "data/averylongname", HL_FS_OPEN_READ, tiny, sizeof(tiny));
    ASSERT_TRUE(s.entry == NULL); ASSERT_STREQ("invalid_args", s.err);         /* scratch too small */
    s = hl_fs_policy_select(&p, "../escape", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry == NULL); ASSERT_STREQ("invalid_path", s.err);         /* lexical */

    hl_fs_policy_free(&p);
    hl_fs_policy_free(&p);                    /* idempotent double free */
    HlFsPolicy z = HL_FS_POLICY_INIT;
    hl_fs_policy_free(&z);                     /* free on INIT is safe */
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── SUBTREE via a symlinked directory grant is FOLLOWED (contained), not refused ─
 * The ratified design (sec. 6): a SUBTREE follows in-root symlinks. `linkdir` is a
 * symlink to the real `data` dir, so the grant loads and anchors at the target. */
UTEST(fs_policy, subtree_via_symlink_dir)
{
    setup(); build_tree();      /* linkdir -> data (a real dir) */
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "linkdir" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));
    ASSERT_EQ((size_t)1, p.read_n);
    ASSERT_EQ((int)HL_FS_ENTRY_SUBTREE, (int)p.read[0].kind);   /* followed to a dir */

    char sc[256];
    HlFsSelection s = hl_fs_policy_select(&p, "linkdir/a.csv", HL_FS_OPEN_READ, sc, sizeof(sc));
    ASSERT_TRUE(s.entry != NULL); ASSERT_STREQ("a.csv", s.residual);

    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── every RETAINED policy fd is close-on-exec (base + all anchors) ──────────── */
UTEST(fs_policy, retained_fds_cloexec)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    /* "data.bin" is a root-level EXACT -> its anchor IS the dup'd base fd (the
     * F_DUPFD_CLOEXEC path); include SUBTREE / PATTERN / CREATE too. */
    const char *rd[] = { "data.bin", "data", "data/*.csv" };
    const char *wr[] = { "out/result.bin" };
    ASSERT_EQ(0, build_policy(&a, rd, 3, wr, 1, &p, &err));

    ASSERT_TRUE((fcntl(p.base_fd, F_GETFD) & FD_CLOEXEC) != 0);
    for (size_t i = 0; i < p.read_n; i++)
        ASSERT_TRUE((fcntl(p.read[i].anchor_fd, F_GETFD) & FD_CLOEXEC) != 0);
    for (size_t i = 0; i < p.write_n; i++)
        ASSERT_TRUE((fcntl(p.write[i].anchor_fd, F_GETFD) & FD_CLOEXEC) != 0);

    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

/* ── allocation-failure injection: every capped-alloc failure leaks nothing ──── */
UTEST(fs_policy, alloc_failure_no_leak)
{
    setup(); build_tree();
    const char *rd[] = { "x/y/z/w.bin" };   /* CREATE: 4 components deep-copied */
    const char *err = NULL;

    /* full footprint (uncapped) */
    HlAllocator a0; hl_alloc_init(&a0, 0);
    HlFsPolicy p0 = HL_FS_POLICY_INIT;
    ASSERT_EQ(0, build_policy(&a0, rd, 1, NULL, 0, &p0, &err));
    size_t peak = hl_alloc_peak(&a0);
    hl_fs_policy_free(&p0);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a0));

    /* Sweep every cap below the footprint: alloc fails at each position in turn
     * (entries array, grant array, and every component string). Each MUST either
     * succeed and free cleanly or fail with zero bytes retained. */
    for (size_t lim = 1; lim <= peak + 16; lim++) {
        HlAllocator a; hl_alloc_init(&a, lim);
        HlFsPolicy p = HL_FS_POLICY_INIT;
        int rc = build_policy(&a, rd, 1, NULL, 0, &p, &err);
        if (rc == 0) hl_fs_policy_free(&p);
        ASSERT_EQ_MSG((size_t)0, hl_alloc_used(&a), "capped-alloc failure leaked bytes");
    }
    teardown();
}

/* ── Sealed copies (policy_seal.h) ───────────────────────────────────────────
 *
 * The cap layer reads the compiled policy and the cap configs on every call, so
 * they live in sealed memory: a heap write must not widen a grant or re-count
 * an allowlist. */

/* Run `poke` in a child; 1 if it died of a memory fault (the page is read-only). */
static int faults(void (*poke)(void *), void *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);   /* not a sanitizer's handler */
        signal(SIGBUS,  SIG_DFL);
        poke(arg);
        _exit(0);
    }
    if (pid < 0) return 0;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 0;
    return WIFSIGNALED(status) &&
           (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
}

static void poke_entry(void *arg)
{
    ((volatile HlFsAuthEntry *)arg)->grant_n = 99;
}

UTEST(fs_policy, a_sealed_copy_decides_the_same_and_is_read_only)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data.bin", "data/*.csv", "logs" };
    const char *wr[] = { "data/sub" };
    ASSERT_EQ(0, build_policy(&a, rd, 3, wr, 1, &p, &err));

    ShSealArena arena;
    ASSERT_EQ(0, sh_seal_arena_init(&arena, hl_fs_policy_sealed_size(&p), "t"));
    const HlFsPolicy *q = hl_fs_policy_copy_sealed(&p, &arena);
    ASSERT_TRUE(q != NULL);
    ASSERT_EQ(0, sh_seal_arena_seal(&arena));
    ASSERT_TRUE(q->read != p.read);

    static const char *paths[] = {
        "data.bin", "sibling.bin", "data/a.csv", "data/a.txt",
        "data/sub/a.csv", "logs/2026/a.txt", "secret.txt", "data/sub/new.txt",
    };
    static const int modes[] = { HL_FS_OPEN_READ, HL_FS_OPEN_WRITE };
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
            char s1[256], s2[256];
            HlFsSelection x = hl_fs_policy_select(&p, paths[i], modes[m], s1, sizeof s1);
            HlFsSelection y = hl_fs_policy_select(q, paths[i], modes[m], s2, sizeof s2);
            EXPECT_EQ(x.entry == NULL, y.entry == NULL);
            if (x.entry && y.entry) EXPECT_STREQ(x.residual, y.residual);
            if (!x.entry && !y.entry) EXPECT_STREQ(x.err, y.err);
        }
    }

    EXPECT_TRUE(faults(poke_entry, (void *)(uintptr_t)&q->read[0]));

    sh_seal_arena_destroy(&arena);
    hl_fs_policy_free(&p);
    ASSERT_EQ((size_t)0, hl_alloc_used(&a));
    teardown();
}

static void poke_env_count(void *arg)
{
    ((volatile HlEnvConfig *)arg)->count = 32;
}

UTEST(fs_policy, the_runtime_s_cap_configs_move_into_sealed_memory)
{
    setup(); build_tree();
    HlAllocator a; hl_alloc_init(&a, 0);
    HlFsPolicy p = HL_FS_POLICY_INIT; const char *err = NULL;
    const char *rd[] = { "data.bin" };
    ASSERT_EQ(0, build_policy(&a, rd, 1, NULL, 0, &p, &err));

    static const char *envs[] = { "HOME", "PATH" };
    HlEnvConfig env = { envs, 2 };
    HlFsConfig  fs  = { base, strlen(base), &p };
    HlRuntime rt;
    memset(&rt, 0, sizeof rt);
    rt.env_cfg = &env;
    rt.fs_cfg  = &fs;

    ShSealArena arena;
    ASSERT_EQ(0, hl_policy_seal_cap_configs(&rt, &arena));
    ASSERT_TRUE(rt.env_cfg != &env);
    ASSERT_EQ(2, rt.env_cfg->count);
    ASSERT_TRUE(rt.env_cfg->allowed == envs);
    ASSERT_TRUE(rt.fs_cfg != &fs);
    ASSERT_TRUE(rt.fs_cfg->base_dir != base);
    ASSERT_STREQ(base, rt.fs_cfg->base_dir);
    ASSERT_TRUE(rt.fs_cfg->policy != &p);

    char sc[256];
    HlFsSelection sel = hl_fs_policy_select(rt.fs_cfg->policy, "data.bin",
                                            HL_FS_OPEN_READ, sc, sizeof sc);
    EXPECT_TRUE(sel.entry != NULL);
    EXPECT_TRUE(faults(poke_env_count, rt.env_cfg));

    sh_seal_arena_destroy(&arena);
    hl_fs_policy_free(&p);
    teardown();
}

/* ── Policy spans: the ROOT pointers are sealed too ───────────────────────── */

static void poke_rt_http_cfg(void *arg)
{
    ((volatile HlRuntime *)arg)->http_cfg = (HlHttpConfig *)(uintptr_t)1;
}

static void poke_rt_mutable(void *arg)
{
    ((volatile HlRuntime *)arg)->import_tracker_count = 5;   /* outside the span */
    _exit(0);
}

UTEST(policy_span, a_sealed_runtime_cannot_be_repointed)
{
    /* What a factory does: page-aligned, owned. */
    HlRuntime *rt = hl_policy_page_alloc(sizeof *rt);
    ASSERT_TRUE(rt != NULL);
    rt->policy_page_owned = 1;
    static HlEnvConfig env;
    rt->env_cfg = &env;
    ASSERT_EQ(0, hl_policy_seal_runtime(rt));

    EXPECT_TRUE(rt->env_cfg == &env);                 /* still readable */
    EXPECT_TRUE(faults(poke_rt_http_cfg, rt));        /* the span is RO */
    EXPECT_FALSE(faults(poke_rt_mutable, rt));        /* the rest is not */

    ASSERT_EQ(0, hl_policy_unseal_runtime(rt));
    rt->http_cfg = NULL;                              /* writable again */
    hl_policy_page_free(rt, sizeof *rt);
}

UTEST(policy_span, a_runtime_not_from_a_factory_refuses_to_seal)
{
    /* Not page-aligned / not owned: sealing would protect the wrong bytes. */
    static HlRuntime rt;
    EXPECT_EQ(-1, hl_policy_seal_runtime(&rt));
}

#ifdef HL_ENABLE_DB
static void poke_registry(void *arg)
{
    /* The first word of the registry is its manifest pointer (in the span). */
    *(volatile uintptr_t *)arg = 1;
}

UTEST(policy_span, a_sealed_registry_keeps_its_dsn_sources)
{
    static HlManifest m;
    HlDbRegistry *reg = hl_db_registry_create(NULL, ":memory:", NULL);
    ASSERT_TRUE(reg != NULL);
    hl_db_registry_set_manifest(reg, &m);
    ASSERT_EQ(0, hl_db_registry_seal(reg));

    /* After the seal the manifest can no longer be swapped... */
    static HlManifest other;
    hl_db_registry_set_manifest(reg, &other);
    EXPECT_TRUE(hl_db_registry_dynamic_policy(reg) == &m.databases.dynamic);
    /* ...nor written behind the API. */
    EXPECT_TRUE(faults(poke_registry, reg));

    /* The -d DSN still resolves, and a worker's DSN is worked out from the
     * sealed sources rather than a stored copy. */
    const char *err = NULL;
    HlDbHandle *h = hl_db_registry_get(reg, "default", &err);
    ASSERT_TRUE(h != NULL);
    EXPECT_STREQ(":memory:", hl_db_registry_dsn_for(reg, h));

    hl_db_registry_destroy(reg);
}

UTEST(policy_span, an_overlong_default_dsn_is_refused)
{
    static char dsn[9000];
    memset(dsn, 'a', sizeof dsn - 1);
    EXPECT_TRUE(hl_db_registry_create(NULL, dsn, NULL) == NULL);
}
#endif

UTEST_MAIN();
