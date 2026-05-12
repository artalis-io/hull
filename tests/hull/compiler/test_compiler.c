/*
 * test_compiler.c — Tests for HlCompilerVtable (compiler.c)
 *
 * Tests the system compiler backend and selection logic.
 * Does NOT test the tcc backend (requires embedded tcc binary).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/compiler.h"
#include "hull/cap/tool.h"

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

static void rm_rf(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    (void)system(cmd);
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

UTEST_MAIN()
