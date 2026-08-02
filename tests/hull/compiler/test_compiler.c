/*
 * test_compiler.c — Tests for HlCompilerVtable (compiler.c)
 *
 * Tests the system compiler backend and selection logic.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* FTW_DEPTH / FTW_PHYS are XSI extensions to nftw; on glibc they're
 * only declared when _XOPEN_SOURCE >= 500. macOS exposes them
 * unconditionally AND uses _XOPEN_SOURCE to gate Darwin extensions
 * the other way (defining it hides clock_gettime_nsec_np /
 * CLOCK_UPTIME_RAW that utest.h needs), so this define has to stay
 * Linux-only. */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

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

/* ── Tests: compile ─────────────────────────────────────────────── */

/* The compiler vtable is compile-only now; linking goes through the linker
 * vtable (hl_linker_link), exercised by the linker e2es (e2e_linker.sh,
 * e2e_compiler_free.sh) and every `hull build` e2e. This asserts only the
 * .c → .o step. */
UTEST(compiler, compile_hello)
{
    char *tmpdir = make_tmpdir();
    ASSERT_NE(tmpdir, NULL);

    /* Write a trivial C program */
    char src[512], obj[512];
    snprintf(src, sizeof(src), "%s/hello.c", tmpdir);
    snprintf(obj, sizeof(obj), "%s/hello.o", tmpdir);

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

    int rc = hl_compiler_compile(c, src, obj, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(access(obj, F_OK), 0);

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
    hl_compiler_destroy(c);
}

/* ── Tests: allowlist ───────────────────────────────────────────── */

UTEST(compiler, allowlist_ld)
{
    ASSERT_EQ(hl_tool_check_allowlist("ld"), 0);
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

/* ── Tests: auto-selection resolves a usable compiler ───────────── */

UTEST(compiler, default_compiler_resolves)
{
    /* Auto-select must resolve an available compiler (the system cc). */
    HlCompiler *c = hl_compiler_select(NULL);
    ASSERT_NE(c, NULL);
    ASSERT_EQ(hl_compiler_is_available(c), 1);
    hl_compiler_destroy(c);
}

UTEST_MAIN()
