/*
 * test_compiler.c — Tests for HlCompilerVtable (compiler.c)
 *
 * Tests the system compiler backend, selection logic, and (when
 * compiled with HL_ENABLE_TCC and embedded tcc is available) the
 * tcc backend end-to-end.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/compiler.h"
#include "hull/cap/tool.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static char *make_tmpdir(void)
{
    char tmpl[] = "/tmp/hull_compiler_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    return dir ? strdup(dir) : NULL;
}

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static int rm_rf_entry(const char *path, const struct stat *sb,
                       int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

static void rm_rf(const char *dir)
{
    /* In-process recursive delete via nftw(FTW_DEPTH). Cosmopolitan's
     * toybox rm rejects `-r` ("rm: illegal option -- r"), and a leaked
     * tmpdir cascades into the next test failing on mkdir EEXIST.
     * nftw is POSIX and works on Linux, macOS, and cosmo. ENOENT means
     * the dir's already gone — fine. */
    if (nftw(dir, rm_rf_entry, 16, FTW_DEPTH | FTW_PHYS) != 0
        && errno != ENOENT) {
        /* Best-effort cleanup; don't mask test failures. */
    }
}

/* ── Tests: hl_compiler_system_new ─────────────────────────────── */

UTEST(compiler, system_new_cc)
{
    HlCompiler *c = hl_compiler_system_new("cc");
    ASSERT_NE(c, NULL);
    ASSERT_STREQ(hl_compiler_name(c), "cc");
    hl_compiler_destroy(c);
}

UTEST(compiler, system_new_null_returns_null)
{
    HlCompiler *c = hl_compiler_system_new(NULL);
    ASSERT_EQ(c, NULL);
}

UTEST(compiler, system_is_available_cc)
{
    HlCompiler *c = hl_compiler_system_new("cc");
    ASSERT_NE(c, NULL);
    /* cc should be available in CI and dev environments */
    ASSERT_EQ(hl_compiler_is_available(c), 1);
    hl_compiler_destroy(c);
}

UTEST(compiler, system_is_not_available_fake)
{
    HlCompiler *c = hl_compiler_system_new("__hull_fake_compiler_xyz__");
    ASSERT_NE(c, NULL);
    ASSERT_EQ(hl_compiler_is_available(c), 0);
    hl_compiler_destroy(c);
}

UTEST(compiler, system_version_cc)
{
    HlCompiler *c = hl_compiler_system_new("cc");
    ASSERT_NE(c, NULL);
    if (hl_compiler_is_available(c)) {
        char *v = hl_compiler_version(c);
        ASSERT_NE(v, NULL);
        ASSERT_GT((int)strlen(v), 0);
        free(v);
    }
    hl_compiler_destroy(c);
}

/* ── Tests: compile + link round-trip ───────────────────────────── */

UTEST(compiler, compile_and_link_hello)
{
    char *tmpdir = make_tmpdir();
    ASSERT_NE(tmpdir, NULL);

    /* Write a trivial C program */
    char src[512], obj[512], out[512];
    snprintf(src, sizeof(src), "%s/hello.c", tmpdir);
    snprintf(obj, sizeof(obj), "%s/hello.o", tmpdir);
    snprintf(out, sizeof(out), "%s/hello", tmpdir);

    int w = write_file(src,
        "extern int puts(const char *);\n"
        "int main(void) { puts(\"hello\"); return 0; }\n");
    ASSERT_EQ(w, 0);

    HlCompiler *c = hl_compiler_select(NULL);
    if (!c) {
        /* No compiler available — skip */
        rm_rf(tmpdir); free(tmpdir);
        return;
    }

    /* Compile */
    int rc = hl_compiler_compile(c, src, obj, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(access(obj, F_OK), 0);

    /* Link */
    const char *objs[] = { obj, NULL };
    const char *libs[] = { NULL };
    rc = hl_compiler_link(c, out, objs, libs);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(access(out, X_OK), 0);

    hl_compiler_destroy(c);
    rm_rf(tmpdir);
    free(tmpdir);
}

UTEST(compiler, compile_with_include_dir)
{
    char *tmpdir = make_tmpdir();
    ASSERT_NE(tmpdir, NULL);

    /* Write entry.h */
    char hdr[512];
    snprintf(hdr, sizeof(hdr), "%s/entry.h", tmpdir);
    write_file(hdr,
        "typedef struct { const char *name; const unsigned char *data; "
        "unsigned int len; } HlEntry;\n");

    /* Write source that includes it */
    char src[512], obj[512];
    snprintf(src, sizeof(src), "%s/reg.c", tmpdir);
    snprintf(obj, sizeof(obj), "%s/reg.o", tmpdir);
    write_file(src,
        "#include \"entry.h\"\n"
        "const HlEntry entries[] = { { 0, 0, 0 } };\n");

    HlCompiler *c = hl_compiler_select(NULL);
    if (!c) { rm_rf(tmpdir); free(tmpdir); return; }

    int rc = hl_compiler_compile(c, src, obj, tmpdir);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(access(obj, F_OK), 0);

    hl_compiler_destroy(c);
    rm_rf(tmpdir);
    free(tmpdir);
}

/* ── Tests: hl_compiler_select ─────────────────────────────────── */

UTEST(compiler, select_null_returns_compiler)
{
    HlCompiler *c = hl_compiler_select(NULL);
    /* At least one compiler should be available in CI */
    ASSERT_NE(c, NULL);
    ASSERT_EQ(hl_compiler_is_available(c), 1);
    hl_compiler_destroy(c);
}

UTEST(compiler, select_explicit_cc)
{
    HlCompiler *c = hl_compiler_select("cc");
    ASSERT_NE(c, NULL);
    ASSERT_STREQ(hl_compiler_name(c), "cc");
    hl_compiler_destroy(c);
}

UTEST(compiler, select_fake_returns_null)
{
    HlCompiler *c = hl_compiler_select("__nonexistent_xyz__");
    ASSERT_EQ(c, NULL);
}

UTEST(compiler, select_system_forces_system)
{
    HlCompiler *c = hl_compiler_select("system");
    /* system compilers should always be available in CI */
    ASSERT_NE(c, NULL);
    /* name should not be "tcc" */
    ASSERT_NE(strcmp(hl_compiler_name(c), "tcc"), 0);
    hl_compiler_destroy(c);
}

/* ── Tests: allowlist (tcc and ld added) ────────────────────────── */

UTEST(compiler, allowlist_tcc)
{
    ASSERT_EQ(hl_tool_check_allowlist("tcc"), 0);
}

UTEST(compiler, allowlist_ld)
{
    ASSERT_EQ(hl_tool_check_allowlist("ld"), 0);
}

UTEST(compiler, allowlist_tcc_with_path)
{
    ASSERT_EQ(hl_tool_check_allowlist("/tmp/hull_tcc_abc/tcc"), 0);
}

/* ── Tests: compile app_registry.c pattern (pure refactor check) ── */

UTEST(compiler, compile_app_registry_pattern)
{
    char *tmpdir = make_tmpdir();
    ASSERT_NE(tmpdir, NULL);

    /* Write entry.h */
    char hdr[512];
    snprintf(hdr, sizeof(hdr), "%s/entry.h", tmpdir);
    write_file(hdr,
        "#ifndef HL_ENTRY_H\n#define HL_ENTRY_H\n"
        "typedef struct { const char *name; const unsigned char *data; "
        "unsigned int len; } HlEntry;\n"
        "#endif\n");

    /* Write app_registry.c pattern */
    char src[512], obj[512];
    snprintf(src, sizeof(src), "%s/app_registry.c", tmpdir);
    snprintf(obj, sizeof(obj), "%s/app_registry.o", tmpdir);
    write_file(src,
        "#include \"entry.h\"\n"
        "static const unsigned char app_data[] = {0x68,0x69,0x0a};\n"
        "const HlEntry hl_app_entries[] = {\n"
        "    {\"./app.lua\", app_data, sizeof(app_data)},\n"
        "    { 0, 0, 0 }\n"
        "};\n");

    HlCompiler *c = hl_compiler_select(NULL);
    if (!c) { rm_rf(tmpdir); free(tmpdir); return; }

    int rc = hl_compiler_compile(c, src, obj, tmpdir);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(access(obj, F_OK), 0);

    hl_compiler_destroy(c);
    rm_rf(tmpdir);
    free(tmpdir);
}

/* ── Tests: tcc backend end-to-end (HL_ENABLE_TCC only) ─────────── */

#ifdef HL_ENABLE_TCC

/* These tests skip themselves on platforms where tcc is not viable
 * (cosmo APE archives, macOS Mach-O linker). They MUST run for real
 * on Linux/x86_64 and Linux/arm64 CI runners — that's how we verify
 * the embedded-tcc build path actually works end-to-end. */

UTEST(compiler, tcc_explicit_sentinel)
{
    HlCompiler *c = hl_compiler_select("tcc");
    if (!c) {
        /* macOS: tcc_is_available returns 0 by design (Mach-O linker
         * incompatibility); cosmo: rejected when platforms embedded.
         * Skip silently — coverage is enforced via the linux_tcc_*
         * tests below which fail loudly if tcc isn't selected. */
        return;
    }
    ASSERT_STREQ(hl_compiler_name(c), "tcc");
    ASSERT_EQ(hl_compiler_is_available(c), 1);
    hl_compiler_destroy(c);
}

#if defined(__linux__) && !defined(__COSMOPOLITAN__)

/* On Linux native (non-cosmo) builds with HL_ENABLE_TCC, the embedded
 * tcc binary MUST be functional and selected by default. Anything
 * else means the zero-dependency build path is broken. */

UTEST(compiler, linux_tcc_is_default)
{
    HlCompiler *c = hl_compiler_select(NULL);
    ASSERT_NE(c, NULL);
    ASSERT_STREQ_MSG(hl_compiler_name(c), "tcc",
        "tcc must be selected by default on Linux when "
        "HL_ENABLE_TCC=1 and embedded_tcc_len > 0");
    hl_compiler_destroy(c);
}

UTEST(compiler, linux_tcc_version_string)
{
    HlCompiler *c = hl_compiler_select("tcc");
    ASSERT_NE(c, NULL);
    char *v = hl_compiler_version(c);
    ASSERT_NE(v, NULL);
    /* version string should start with "tcc" — exact format:
     * "tcc version 0.9.28rc 2026-MM-DD mob@<hash> (<arch> Linux)" */
    ASSERT_STRNEQ("tcc", v, 3);
    free(v);
    hl_compiler_destroy(c);
}

UTEST(compiler, linux_tcc_compile_and_link)
{
    char *tmpdir = make_tmpdir();
    ASSERT_NE(tmpdir, NULL);

    char src[512], obj[512], out[512];
    snprintf(src, sizeof(src), "%s/hello.c", tmpdir);
    snprintf(obj, sizeof(obj), "%s/hello.o", tmpdir);
    snprintf(out, sizeof(out), "%s/hello", tmpdir);

    write_file(src,
        "extern int puts(const char *);\n"
        "int main(void) { puts(\"tcc-linux-ok\"); return 0; }\n");

    /* Force the tcc backend explicitly so we know that's what ran */
    HlCompiler *c = hl_compiler_select("tcc");
    ASSERT_NE_MSG(c, NULL, "tcc backend must be available on Linux");
    ASSERT_STREQ(hl_compiler_name(c), "tcc");

    /* Compile via tcc → ELF .o */
    int rc = hl_compiler_compile(c, src, obj, NULL);
    ASSERT_EQ_MSG(rc, 0, "tcc compile of hello.c must succeed");
    ASSERT_EQ(access(obj, F_OK), 0);

    /* Link via tcc backend (delegates to system cc/ld on Linux) */
    const char *objs[] = { obj, NULL };
    const char *libs[] = { NULL };
    rc = hl_compiler_link(c, out, objs, libs);
    ASSERT_EQ_MSG(rc, 0, "tcc-backend link must succeed on Linux");
    ASSERT_EQ(access(out, X_OK), 0);

    /* The proof: actually execute the binary and check it runs.
     * If the .o is the wrong format or the link produced garbage,
     * this will fail. */
    char run_cmd[1024];
    snprintf(run_cmd, sizeof(run_cmd), "%s 2>&1", out);
    FILE *p = popen(run_cmd, "r");
    ASSERT_NE(p, NULL);
    char buf[64] = {0};
    fgets(buf, sizeof(buf), p);
    int status = pclose(p);
    ASSERT_EQ_MSG(status, 0, "binary built by tcc must run and exit 0");
    ASSERT_TRUE(strstr(buf, "tcc-linux-ok") != NULL);

    hl_compiler_destroy(c);
    rm_rf(tmpdir);
    free(tmpdir);
}

#endif /* __linux__ && !__COSMOPOLITAN__ */
#endif /* HL_ENABLE_TCC */

UTEST_MAIN()
