/*
 * test_tool.c - Tests for tool hardening (spawn, find_files, copy, rmdir, unveil)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/tool.h"
#include "hull/compilers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Helper: create a temp directory ──────────────────────────────── */

static char *make_tmpdir(void)
{
    char tmpl[] = "/tmp/hull_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) return NULL;
    return strdup(dir);
}

/* Helper: write a file */
static int write_test_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

/* ── Allowlist tests ──────────────────────────────────────────────── */

UTEST(tool, allowlist_accept_cc)
{
    ASSERT_EQ(hl_tool_check_allowlist("cc"), 0);
}

UTEST(tool, allowlist_accept_gcc)
{
    ASSERT_EQ(hl_tool_check_allowlist("gcc"), 0);
}

UTEST(tool, allowlist_accept_clang)
{
    ASSERT_EQ(hl_tool_check_allowlist("clang"), 0);
}

UTEST(tool, allowlist_accept_cosmocc)
{
    ASSERT_EQ(hl_tool_check_allowlist("cosmocc"), 0);
}

UTEST(tool, allowlist_accepts_default_cc)
{
    ASSERT_EQ(hl_tool_check_allowlist(HL_DEFAULT_CC), 0);
}

UTEST(tool, allowlist_accept_ar)
{
    ASSERT_EQ(hl_tool_check_allowlist("ar"), 0);
}

UTEST(tool, allowlist_accept_cosmoar)
{
    ASSERT_EQ(hl_tool_check_allowlist("cosmoar"), 0);
}

UTEST(tool, allowlist_versioned_clang)
{
    ASSERT_EQ(hl_tool_check_allowlist("clang-18"), 0);
}

UTEST(tool, allowlist_versioned_gcc)
{
    ASSERT_EQ(hl_tool_check_allowlist("gcc-12"), 0);
}

UTEST(tool, allowlist_with_path)
{
    ASSERT_EQ(hl_tool_check_allowlist("/usr/bin/cc"), 0);
    ASSERT_EQ(hl_tool_check_allowlist("/usr/bin/clang-18"), 0);
}

UTEST(tool, allowlist_reject_sh)
{
    ASSERT_NE(hl_tool_check_allowlist("sh"), 0);
}

UTEST(tool, allowlist_reject_bash)
{
    ASSERT_NE(hl_tool_check_allowlist("bash"), 0);
}

UTEST(tool, allowlist_reject_rm)
{
    ASSERT_NE(hl_tool_check_allowlist("rm"), 0);
}

UTEST(tool, allowlist_reject_curl)
{
    ASSERT_NE(hl_tool_check_allowlist("curl"), 0);
}

UTEST(tool, allowlist_reject_null)
{
    ASSERT_NE(hl_tool_check_allowlist(NULL), 0);
}

UTEST(tool, allowlist_reject_empty)
{
    ASSERT_NE(hl_tool_check_allowlist(""), 0);
}

/* Windows executable names. A cosmo APE reaches Windows, where the tools
 * `hull build` spawns are `cc.exe` / `gcc.exe` and paths are backslash-
 * separated. Two blind spots made every one of them DENIED there: the basename
 * was split on '/' only, so "C:\tools\gcc.exe" stayed whole and matched
 * nothing; and the match accepts only an exact name or a `-<digit>` version, so
 * a clean "cc.exe" fell through as well. */
UTEST(tool, allowlist_accept_windows_com_suffix)
{
    ASSERT_EQ(hl_tool_check_allowlist("hull.com"), 0);
    ASSERT_EQ(hl_tool_check_allowlist("cosmocc.com"), 0);
}

UTEST(tool, allowlist_accept_windows_exe_suffix)
{
    ASSERT_EQ(hl_tool_check_allowlist("cc.exe"), 0);
    ASSERT_EQ(hl_tool_check_allowlist("clang-18.exe"), 0);
}

UTEST(tool, allowlist_accept_windows_backslash_path)
{
    ASSERT_EQ(hl_tool_check_allowlist("C:\\Users\\m\\.local\\bin\\hull.com"), 0);
    ASSERT_EQ(hl_tool_check_allowlist("C:\\tools\\gcc.exe"), 0);
}

/* Stripping a suffix must not admit a name that was not already allowed:
 * only the SUFFIX is removed, and what remains still has to be on the list. */
UTEST(tool, allowlist_suffix_strip_admits_no_new_name)
{
    ASSERT_NE(hl_tool_check_allowlist("evil.com"), 0);
    ASSERT_NE(hl_tool_check_allowlist("sh.exe"), 0);
    ASSERT_NE(hl_tool_check_allowlist("cc-evil.exe"), 0);
    ASSERT_NE(hl_tool_check_allowlist("rm.com"), 0);
    /* not a trailing suffix - must not be stripped from the middle */
    ASSERT_NE(hl_tool_check_allowlist("cc.exe.evil"), 0);
    /* the suffix alone is not a name */
    ASSERT_NE(hl_tool_check_allowlist(".com"), 0);
    ASSERT_NE(hl_tool_check_allowlist(".exe"), 0);
}

/* The boundary this fix does NOT cross. `hull-cosmo.exe` is a real name the CI
 * runs, and it stays denied: stripping ".exe" leaves "hull-cosmo", which the
 * exact-or-`-<digit>` match rejects. That is deliberate - loosening it would
 * admit any `hull-*`. Re-execing hull does not need the allowlist at all and
 * goes through hl_tool_spawn_self instead (#427). */
UTEST(tool, allowlist_still_rejects_suffixed_variant_names)
{
    ASSERT_NE(hl_tool_check_allowlist("hull-cosmo.exe"), 0);
    ASSERT_NE(hl_tool_check_allowlist("gcc-wrapper.exe"), 0);
}

/* The suffix strip is case-SENSITIVE, like the name match it feeds. "CC.EXE"
 * is therefore denied - exactly as bare "CC" is, so this is a limitation
 * carried over rather than introduced. Matching names case-insensitively would
 * be a real behaviour change, and wrong on POSIX where case is significant. */
UTEST(tool, allowlist_suffix_strip_is_case_sensitive)
{
    ASSERT_NE(hl_tool_check_allowlist("CC.EXE"), 0);
    ASSERT_NE(hl_tool_check_allowlist("cc.EXE"), 0);
}

UTEST(tool, allowlist_reject_cc_evil)
{
    ASSERT_NE(hl_tool_check_allowlist("cc-evil"), 0);
}

UTEST(tool, allowlist_reject_ar_malicious)
{
    ASSERT_NE(hl_tool_check_allowlist("ar-malicious"), 0);
}

UTEST(tool, allowlist_reject_clang_backdoor)
{
    ASSERT_NE(hl_tool_check_allowlist("clang-backdoor"), 0);
}

UTEST(tool, allowlist_reject_evil_cc)
{
    ASSERT_NE(hl_tool_check_allowlist("evil-cc"), 0);
}

UTEST(tool, allowlist_reject_cross_cc)
{
    ASSERT_NE(hl_tool_check_allowlist("x86_64-unknown-cosmo-cc"), 0);
}

UTEST(tool, allowlist_accept_ld)
{
    ASSERT_EQ(hl_tool_check_allowlist("ld"), 0);
}

UTEST(tool, allowlist_accept_gcc_12)
{
    ASSERT_EQ(hl_tool_check_allowlist("gcc-12"), 0);
}

UTEST(tool, allowlist_accept_clang_18)
{
    ASSERT_EQ(hl_tool_check_allowlist("clang-18"), 0);
}

/* ── Dangerous flag validation tests ──────────────────────────────── */

UTEST(tool, validate_reject_load)
{
    const char *argv[] = { "cc", "-load", "evil.so", NULL };
    ASSERT_NE(hl_tool_validate_args(argv), 0);
}

UTEST(tool, validate_reject_fplugin)
{
    const char *argv[] = { "cc", "-fplugin=evil.so", NULL };
    ASSERT_NE(hl_tool_validate_args(argv), 0);
}

UTEST(tool, validate_reject_fplugin_separate)
{
    const char *argv[] = { "cc", "-fplugin", "evil.so", NULL };
    ASSERT_NE(hl_tool_validate_args(argv), 0);
}

UTEST(tool, validate_reject_xlinker)
{
    const char *argv[] = { "cc", "-Xlinker", "-rpath", NULL };
    ASSERT_NE(hl_tool_validate_args(argv), 0);
}

UTEST(tool, validate_reject_wl)
{
    const char *argv[] = { "cc", "-Wl,-rpath,/evil", NULL };
    ASSERT_NE(hl_tool_validate_args(argv), 0);
}

UTEST(tool, validate_reject_response_file)
{
    const char *argv[] = { "cc", "@commands.txt", NULL };
    ASSERT_NE(hl_tool_validate_args(argv), 0);
}

UTEST(tool, validate_accept_normal)
{
    const char *argv[] = { "cc", "-std=c11", "-O2", "-c", "-o", "out.o", "main.c", NULL };
    ASSERT_EQ(hl_tool_validate_args(argv), 0);
}

/* ── Spawn tests ──────────────────────────────────────────────────── */

UTEST(tool, spawn_reject_disallowed)
{
    const char *argv[] = { "ls", "-la", NULL };
    int rc = hl_tool_spawn(argv);
    ASSERT_EQ(rc, -1);
}

UTEST(tool, spawn_null_argv)
{
    ASSERT_EQ(hl_tool_spawn(NULL), -1);
}

UTEST(tool, spawn_read_reject_disallowed)
{
    const char *argv[] = { "echo", "hello", NULL };
    char *out = hl_tool_spawn_read(argv, NULL);
    ASSERT_TRUE(out == NULL);
}

/* ── hl_tool_spawn_driver_shell (cosmo/Windows shell-driver path) ─────── */

UTEST(tool, driver_shell_rejects_non_allowlisted_driver)
{
    /* The driver must be an allowlisted compiler - never an arbitrary program. */
    const char *args[] = { "--version", NULL };
    ASSERT_EQ(hl_tool_spawn_driver_shell("/bin/sh", "python", args, NULL), -1);
    ASSERT_EQ(hl_tool_spawn_driver_shell("/bin/sh", "rm", args, NULL), -1);
}

UTEST(tool, driver_shell_rejects_non_shell)
{
    /* The shell must be a bare sh / busybox - not an arbitrary interpreter. */
    const char *args[] = { "--version", NULL };
    ASSERT_EQ(hl_tool_spawn_driver_shell("/bin/bash", "cc", args, NULL), -1);
    ASSERT_EQ(hl_tool_spawn_driver_shell("/usr/bin/python3", "cc", args, NULL), -1);
    ASSERT_EQ(hl_tool_spawn_driver_shell(NULL, "cc", args, NULL), -1);
    ASSERT_EQ(hl_tool_spawn_driver_shell("/bin/sh", NULL, args, NULL), -1);
}

UTEST(tool, driver_shell_rejects_dangerous_driver_args)
{
    /* The driver's args go through the same dangerous-flag filter. */
    const char *load[]    = { "-load", "x.so", NULL };
    const char *xlinker[] = { "-Xlinker", "--bad", NULL };
    const char *respfile[] = { "@resp", NULL };
    ASSERT_EQ(hl_tool_spawn_driver_shell("/bin/sh", "cc", load, NULL), -1);
    ASSERT_EQ(hl_tool_spawn_driver_shell("/bin/sh", "cc", xlinker, NULL), -1);
    ASSERT_EQ(hl_tool_spawn_driver_shell("/bin/sh", "cc", respfile, NULL), -1);
}

/* Positive: run an allowlisted driver THROUGH /bin/sh and prove the $0/$@
 * plumbing reaches it. `sh -c 'exec "$0" "$@"' cc --version` -> `cc --version`.
 * A real cc is standard on the CI hosts; skip cleanly if it is somehow absent. */
UTEST(tool, driver_shell_runs_driver_through_sh)
{
    const char *probe[] = { "cc", "--version", NULL };
    char *out = hl_tool_spawn_read(probe, NULL);
    if (!out) UTEST_SKIP("no cc on this host");   /* UTEST_SKIP returns */
    free(out);

    const char *args[] = { "--version", NULL };
    ASSERT_EQ(hl_tool_spawn_driver_shell("/bin/sh", "cc", args, NULL), 0);
}

/* ── find_files tests ─────────────────────────────────────────────── */

UTEST(tool, find_files_basic)
{
    char *tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);

    /* Create test files */
    char path[512];
    snprintf(path, sizeof(path), "%s/test_one.lua", tmpdir);
    write_test_file(path, "-- test");
    snprintf(path, sizeof(path), "%s/test_two.lua", tmpdir);
    write_test_file(path, "-- test");
    snprintf(path, sizeof(path), "%s/other.txt", tmpdir);
    write_test_file(path, "not a lua file");

    char **files = hl_tool_find_files(tmpdir, "*.lua", NULL);
    ASSERT_TRUE(files != NULL);

    int count = 0;
    for (char **p = files; *p; p++) count++;
    ASSERT_EQ(count, 2);

    /* Cleanup */
    for (char **p = files; *p; p++) free(*p);
    free(files);
    hl_tool_rmdir(tmpdir, NULL);
    free(tmpdir);
}

UTEST(tool, find_files_recursive)
{
    char *tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);

    /* Create subdirectory */
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/sub", tmpdir);
    mkdir(subdir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/test_a.lua", tmpdir);
    write_test_file(path, "-- test");
    snprintf(path, sizeof(path), "%s/test_b.lua", subdir);
    write_test_file(path, "-- test");

    char **files = hl_tool_find_files(tmpdir, "*.lua", NULL);
    ASSERT_TRUE(files != NULL);

    int count = 0;
    for (char **p = files; *p; p++) count++;
    ASSERT_EQ(count, 2);

    for (char **p = files; *p; p++) free(*p);
    free(files);
    hl_tool_rmdir(tmpdir, NULL);
    free(tmpdir);
}

UTEST(tool, find_files_skips_dotdirs)
{
    char *tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);

    char dotdir[512];
    snprintf(dotdir, sizeof(dotdir), "%s/.hidden", tmpdir);
    mkdir(dotdir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/test_a.lua", tmpdir);
    write_test_file(path, "-- visible");
    snprintf(path, sizeof(path), "%s/.hidden/test_b.lua", tmpdir);
    write_test_file(path, "-- hidden");

    char **files = hl_tool_find_files(tmpdir, "*.lua", NULL);
    ASSERT_TRUE(files != NULL);

    int count = 0;
    for (char **p = files; *p; p++) count++;
    ASSERT_EQ(count, 1);

    for (char **p = files; *p; p++) free(*p);
    free(files);
    hl_tool_rmdir(tmpdir, NULL);
    free(tmpdir);
}

UTEST(tool, find_files_skips_vendor)
{
    char *tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);

    char vendordir[512];
    snprintf(vendordir, sizeof(vendordir), "%s/vendor", tmpdir);
    mkdir(vendordir, 0755);
    char nodedir[512];
    snprintf(nodedir, sizeof(nodedir), "%s/node_modules", tmpdir);
    mkdir(nodedir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/test_a.lua", tmpdir);
    write_test_file(path, "-- visible");
    snprintf(path, sizeof(path), "%s/vendor/test_b.lua", tmpdir);
    write_test_file(path, "-- skipped");
    snprintf(path, sizeof(path), "%s/node_modules/test_c.lua", tmpdir);
    write_test_file(path, "-- skipped");

    char **files = hl_tool_find_files(tmpdir, "*.lua", NULL);
    ASSERT_TRUE(files != NULL);

    int count = 0;
    for (char **p = files; *p; p++) count++;
    ASSERT_EQ(count, 1);

    for (char **p = files; *p; p++) free(*p);
    free(files);
    hl_tool_rmdir(tmpdir, NULL);
    free(tmpdir);
}

UTEST(tool, find_files_pattern_match)
{
    char *tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);

    char path[512];
    snprintf(path, sizeof(path), "%s/test_foo.lua", tmpdir);
    write_test_file(path, "-- test");
    snprintf(path, sizeof(path), "%s/helper.lua", tmpdir);
    write_test_file(path, "-- helper");

    char **files = hl_tool_find_files(tmpdir, "test_*.lua", NULL);
    ASSERT_TRUE(files != NULL);

    int count = 0;
    for (char **p = files; *p; p++) count++;
    ASSERT_EQ(count, 1);

    for (char **p = files; *p; p++) free(*p);
    free(files);
    hl_tool_rmdir(tmpdir, NULL);
    free(tmpdir);
}

/* ── copy tests ───────────────────────────────────────────────────── */

UTEST(tool, copy_basic)
{
    char *tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);

    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/original.txt", tmpdir);
    snprintf(dst, sizeof(dst), "%s/copied.txt", tmpdir);

    write_test_file(src, "hello world");

    int rc = hl_tool_copy(src, dst, NULL);
    ASSERT_EQ(rc, 0);

    /* Verify content */
    FILE *f = fopen(dst, "r");
    ASSERT_TRUE(f != NULL);
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    buf[n] = '\0';
    ASSERT_STREQ(buf, "hello world");

    hl_tool_rmdir(tmpdir, NULL);
    free(tmpdir);
}

UTEST(tool, copy_path_validation)
{
    HlToolUnveilCtx ctx;
    hl_tool_unveil_init(&ctx);
    hl_tool_unveil_add(&ctx, "/tmp", "rwc");
    hl_tool_unveil_seal(&ctx);

    /* Copy within /tmp should work */
    char *tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);

    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/src.txt", tmpdir);
    snprintf(dst, sizeof(dst), "%s/dst.txt", tmpdir);
    write_test_file(src, "test data");

    ASSERT_EQ(hl_tool_copy(src, dst, &ctx), 0);

    hl_tool_rmdir(tmpdir, NULL);
    free(tmpdir);
    hl_tool_unveil_free(&ctx);
}

/* ── rmdir tests ──────────────────────────────────────────────────── */

UTEST(tool, rmdir_basic)
{
    char *tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);

    /* Create files in it */
    char path[512];
    snprintf(path, sizeof(path), "%s/file.txt", tmpdir);
    write_test_file(path, "data");

    /* Create subdirectory with file */
    char sub[512];
    snprintf(sub, sizeof(sub), "%s/subdir", tmpdir);
    mkdir(sub, 0755);
    snprintf(path, sizeof(path), "%s/subdir/nested.txt", tmpdir);
    write_test_file(path, "nested");

    int rc = hl_tool_rmdir(tmpdir, NULL);
    ASSERT_EQ(rc, 0);

    /* Verify it's gone */
    ASSERT_NE(access(tmpdir, F_OK), 0);

    free(tmpdir);
}

UTEST(tool, rmdir_path_validation)
{
    HlToolUnveilCtx ctx;
    hl_tool_unveil_init(&ctx);
    hl_tool_unveil_add(&ctx, "/tmp", "rwc");
    hl_tool_unveil_seal(&ctx);

    char *tmpdir = make_tmpdir();
    ASSERT_TRUE(tmpdir != NULL);

    /* Should succeed - /tmp is unveiled for write */
    ASSERT_EQ(hl_tool_rmdir(tmpdir, &ctx), 0);

    free(tmpdir);
    hl_tool_unveil_free(&ctx);
}

/* ── Unveil context tests ─────────────────────────────────────────── */

UTEST(tool, unveil_init_and_add)
{
    HlToolUnveilCtx ctx;
    hl_tool_unveil_init(&ctx);
    ASSERT_EQ(ctx.count, 0);
    ASSERT_EQ(ctx.sealed, 0);

    ASSERT_EQ(hl_tool_unveil_add(&ctx, "/tmp", "rwc"), 0);
    /* On macOS /tmp → /private/tmp, so both paths are stored (count=2).
     * On Linux /tmp is real, so only one entry (count=1). */
    ASSERT_TRUE(ctx.count >= 1);
    hl_tool_unveil_free(&ctx);
}

UTEST(tool, unveil_seal_prevents_add)
{
    HlToolUnveilCtx ctx;
    hl_tool_unveil_init(&ctx);
    hl_tool_unveil_add(&ctx, "/tmp", "rwc");
    hl_tool_unveil_seal(&ctx);
    ASSERT_EQ(ctx.sealed, 1);

    /* Adding after seal should fail */
    ASSERT_EQ(hl_tool_unveil_add(&ctx, "/usr", "r"), -1);
    hl_tool_unveil_free(&ctx);
}

/* mkdir -p must not abort on a component that already exists, whatever
 * errno the platform reports for it. Regression: Cosmopolitan spells absolute
 * paths "/C/Users/...", so the first component of the walk is the drive root
 * "/C" - and mkdir("/C") answers EACCES on Windows, not EEXIST. The walk bailed
 * there, so every hl_tool_mkdir under a drive root failed and `hull build` could
 * not create <app>/.hull/build (leaving cosmocc's debug sidecars in the app
 * root). "/tmp/..." never showed it: that first component answers EEXIST. */
UTEST(tool, mkdir_p_under_an_existing_root)
{
    /* Somewhere real and deep enough to exercise several components. Default
     * to the temp dir; HULL_UNVEIL_PROBE_DIR points it at another volume. */
    char tmpl[] = "/tmp/hull_mkdirp_XXXXXX";
    const char *env = getenv("HULL_UNVEIL_PROBE_DIR");
    const char *base = env;
    if (!base) { ASSERT_TRUE(mkdtemp(tmpl) != NULL); base = tmpl; }

    HlToolUnveilCtx ctx;
    hl_tool_unveil_init(&ctx);
    ASSERT_EQ(hl_tool_unveil_add(&ctx, base, "rwc"), 0);
    hl_tool_unveil_seal(&ctx);

    char nested[PATH_MAX];
    snprintf(nested, sizeof(nested), "%s/.hull/build", base);

    ASSERT_EQ(hl_tool_mkdir(nested, &ctx), 0);
    struct stat st;
    ASSERT_EQ(stat(nested, &st), 0);
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    /* Idempotent: a second call over the same tree must also succeed. */
    ASSERT_EQ(hl_tool_mkdir(nested, &ctx), 0);

    hl_tool_unveil_free(&ctx);
    rmdir(nested);
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s/.hull", base);
    rmdir(parent);
    if (!env) rmdir(tmpl);
}

UTEST(tool, unveil_check_allowed)
{
    HlToolUnveilCtx ctx;
    hl_tool_unveil_init(&ctx);
    hl_tool_unveil_add(&ctx, "/tmp", "rwc");
    hl_tool_unveil_seal(&ctx);

    ASSERT_EQ(hl_tool_unveil_check(&ctx, "/tmp/foo/bar", 'r'), 0);
    ASSERT_EQ(hl_tool_unveil_check(&ctx, "/tmp/foo/bar", 'w'), 0);
    hl_tool_unveil_free(&ctx);
}

UTEST(tool, unveil_check_denied)
{
    HlToolUnveilCtx ctx;
    hl_tool_unveil_init(&ctx);
    hl_tool_unveil_add(&ctx, "/tmp", "r");
    hl_tool_unveil_seal(&ctx);

    /* Read allowed, write denied */
    ASSERT_EQ(hl_tool_unveil_check(&ctx, "/tmp/foo", 'r'), 0);
    ASSERT_NE(hl_tool_unveil_check(&ctx, "/tmp/foo", 'w'), 0);

    /* Path outside unveiled dirs denied */
    ASSERT_NE(hl_tool_unveil_check(&ctx, "/etc/passwd", 'r'), 0);
    hl_tool_unveil_free(&ctx);
}

UTEST(tool, unveil_enforcement_find_files)
{
    HlToolUnveilCtx ctx;
    hl_tool_unveil_init(&ctx);
    hl_tool_unveil_add(&ctx, "/tmp", "r");
    hl_tool_unveil_seal(&ctx);

    /* Should fail - /etc is not unveiled */
    char **files = hl_tool_find_files("/etc", "*.conf", &ctx);
    ASSERT_TRUE(files == NULL);
    hl_tool_unveil_free(&ctx);
}

UTEST(tool, find_files_null_args)
{
    ASSERT_TRUE(hl_tool_find_files(NULL, "*.lua", NULL) == NULL);
    ASSERT_TRUE(hl_tool_find_files("/tmp", NULL, NULL) == NULL);
}

UTEST(tool, copy_null_args)
{
    ASSERT_EQ(hl_tool_copy(NULL, "/tmp/dst", NULL), -1);
    ASSERT_EQ(hl_tool_copy("/tmp/src", NULL, NULL), -1);
}

UTEST(tool, rmdir_null_args)
{
    ASSERT_EQ(hl_tool_rmdir(NULL, NULL), -1);
}

UTEST_MAIN();
