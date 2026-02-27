/*
 * test_tool.c — Tests for tool hardening (spawn, find_files, copy, rmdir, unveil)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/tool.h"

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

    /* Should succeed — /tmp is unveiled for write */
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

    /* Should fail — /etc is not unveiled */
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
