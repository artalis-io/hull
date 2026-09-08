/*
 * test_host.c - host-OS facts and user-facing command rendering.
 *
 * Covers src/hull/shared/host.c: the single place Hull decides what host it is
 * on, what suffix a produced executable needs there, how to spell "run this"
 * in the host's shell, and how to split PATH.
 *
 * Two of these are REGRESSION tests for shipped Windows defects:
 *
 *   - PATH search used to split on ':' and join with '/', which shredded the
 *     drive letters in a Windows PATH and made every compiler probe in
 *     `hull doctor` report "not found" regardless of what was installed.
 *   - `hull build` produced an extensionless APE, which Windows will not
 *     execute; the artifact needs the ".com" suffix.
 *
 * The host is fixed at process level, so the platform-specific expectations
 * are asserted for whichever host the suite is compiled/run on, and the
 * host-INDEPENDENT invariants (which are the ones that actually protect the
 * cross-platform contract) are asserted unconditionally.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/shared/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Host identity ─────────────────────────────────────────────────── */

UTEST(host, is_windows_is_stable)
{
    /* Cached after the first call; a flapping answer would make artifact
     * naming non-deterministic within one build. */
    int a = hl_host_is_windows();
    int b = hl_host_is_windows();
    ASSERT_EQ(a, b);
    ASSERT_TRUE(a == 0 || a == 1);
}

UTEST(host, os_label_matches_is_windows)
{
    const char *os = hl_host_os();
    ASSERT_TRUE(os != NULL);
    if (hl_host_is_windows())
        ASSERT_STREQ("windows", os);
    else
        ASSERT_STRNE("windows", os);
}

UTEST(host, separators_follow_host)
{
    if (hl_host_is_windows()) {
        ASSERT_EQ(';',  hl_host_path_list_sep());
        ASSERT_EQ('\\', hl_host_dir_sep());
    } else {
        ASSERT_EQ(':', hl_host_path_list_sep());
        ASSERT_EQ('/', hl_host_dir_sep());
    }
}

/* ── Executable suffix ─────────────────────────────────────────────── */

UTEST(host, exe_suffix_is_com_on_windows_empty_elsewhere)
{
    const char *sfx = hl_host_exe_suffix();
    ASSERT_TRUE(sfx != NULL);          /* never NULL - callers concatenate it */
    if (hl_host_is_windows())
        ASSERT_STREQ(".com", sfx);
    else
        ASSERT_STREQ("", sfx);         /* POSIX naming must not regress */
}

/* ── Command rendering ─────────────────────────────────────────────── */

UTEST(host, render_exec_adds_a_current_directory_prefix)
{
    /* Neither PowerShell nor a POSIX shell searches the current directory,
     * so a bare name must never be printed as a runnable command. */
    char out[256];
    ASSERT_EQ(0, hl_host_render_exec("app", out, sizeof(out)));
    if (hl_host_is_windows())
        ASSERT_STREQ(".\\app", out);
    else
        ASSERT_STREQ("./app", out);
}

UTEST(host, render_exec_never_emits_a_bare_relative_name)
{
    /* The host-independent invariant: whatever the host, the rendered form is
     * not the bare input, and it carries an explicit prefix. */
    static const char *inputs[] = { "app", "app.com", "build/app", NULL };
    for (const char **p = inputs; *p; p++) {
        char out[256];
        ASSERT_EQ(0, hl_host_render_exec(*p, out, sizeof(out)));
        ASSERT_STRNE(*p, out);
        ASSERT_EQ('.', out[0]);
        ASSERT_TRUE(out[1] == '/' || out[1] == '\\');
    }
}

UTEST(host, render_exec_keeps_an_existing_prefix)
{
    char out[256];
    ASSERT_EQ(0, hl_host_render_exec("./app", out, sizeof(out)));
    if (hl_host_is_windows())
        ASSERT_STREQ(".\\app", out);   /* separators normalised, not doubled */
    else
        ASSERT_STREQ("./app", out);
}

UTEST(host, render_exec_passes_absolute_paths_through)
{
    char out[256];
    ASSERT_EQ(0, hl_host_render_exec("/usr/local/bin/app", out, sizeof(out)));
    /* An absolute path needs no current-directory prefix on either host. */
    ASSERT_TRUE(out[0] == '/' || out[0] == '\\');
    ASSERT_TRUE(strstr(out, "app") != NULL);
}

UTEST(host, render_exec_windows_uses_backslashes)
{
    char out[256];
    ASSERT_EQ(0, hl_host_render_exec("build/app.com", out, sizeof(out)));
    if (hl_host_is_windows()) {
        ASSERT_TRUE(strchr(out, '/') == NULL);
        ASSERT_STREQ(".\\build\\app.com", out);
    } else {
        ASSERT_STREQ("./build/app.com", out);
    }
}

UTEST(host, render_exec_rejects_bad_arguments)
{
    char out[8];
    ASSERT_EQ(-1, hl_host_render_exec(NULL, out, sizeof(out)));
    ASSERT_STREQ("", out);             /* never left uninitialised */
    ASSERT_EQ(-1, hl_host_render_exec("app", NULL, 16));
    ASSERT_EQ(-1, hl_host_render_exec("app", out, 0));
    /* Overflow is reported, not truncated into a wrong command. */
    ASSERT_EQ(-1, hl_host_render_exec("a-very-long-artifact-name", out,
                                      sizeof(out)));
    ASSERT_STREQ("", out);
}

UTEST(host, render_exec_quotes_a_path_with_spaces)
{
    /* REGRESSION: the rendering was printed bare, so a perfectly ordinary
     * Windows home directory produced `C:\Users\Jane Doe\myapp\app.com` -
     * which is not a command, it is two words. */
    char out[256];
    ASSERT_EQ(0, hl_host_render_exec("myapp/my app", out, sizeof(out)));
    if (hl_host_is_windows())
        /* PowerShell executes a quoted string only via the call operator. */
        ASSERT_STREQ("& '.\\myapp\\my app'", out);
    else
        ASSERT_STREQ("'./myapp/my app'", out);
}

UTEST(host, render_exec_quotes_an_absolute_path_with_spaces)
{
    char out[256];
    if (hl_host_is_windows()) {
        ASSERT_EQ(0, hl_host_render_exec("C:/Users/Jane Doe/myapp/app.com",
                                         out, sizeof(out)));
        ASSERT_STREQ("& 'C:\\Users\\Jane Doe\\myapp\\app.com'", out);
    } else {
        ASSERT_EQ(0, hl_host_render_exec("/home/jane doe/myapp/app",
                                         out, sizeof(out)));
        ASSERT_STREQ("'/home/jane doe/myapp/app'", out);
    }
}

UTEST(host, render_exec_escapes_an_embedded_quote)
{
    /* A single quote is legal in a filename on both hosts, and it is the one
     * byte that could break OUT of the quoting we just added. */
    char out[256];
    ASSERT_EQ(0, hl_host_render_exec("o'brien/my app", out, sizeof(out)));
    if (hl_host_is_windows())
        ASSERT_STREQ("& '.\\o''brien\\my app'", out);   /* doubled */
    else
        ASSERT_STREQ("'./o'\\''brien/my app'", out);    /* close-escape-reopen */
}

UTEST(host, render_exec_leaves_ordinary_paths_unquoted)
{
    /* Quoting is only for paths that need it: the common case must stay
     * exactly as it reads today, or every "run it with" line in the docs and
     * the e2e assertions would change shape. */
    static const char *inputs[] = {
        "app", "app.com", "build/app", "firstrun/app.com",
        "/tmp/hull-e2e.XYZ/app/app", "my-app_v2.0/app", NULL
    };
    for (const char **p = inputs; *p; p++) {
        char out[256];
        ASSERT_EQ(0, hl_host_render_exec(*p, out, sizeof(out)));
        ASSERT_TRUE(strchr(out, '\'') == NULL);
        ASSERT_TRUE(strchr(out, '&') == NULL);
    }
}

UTEST(host, render_exec_reports_overflow_when_quoting)
{
    /* The quoted form is longer than the input, so the bound must be checked
     * against what is actually emitted - never truncated into a command that
     * names a different file (or has an unterminated quote). */
    char out[12];
    ASSERT_EQ(-1, hl_host_render_exec("a directory/with a long name/app",
                                      out, sizeof(out)));
    ASSERT_STREQ("", out);
}

/* ── PATH search ───────────────────────────────────────────────────── */

UTEST(host, find_in_path_rejects_bad_arguments)
{
    char out[64];
    ASSERT_EQ(0, hl_host_find_in_path(NULL, out, sizeof(out)));
    ASSERT_EQ(0, hl_host_find_in_path("", out, sizeof(out)));
    ASSERT_EQ(0, hl_host_find_in_path("x", NULL, 16));
    ASSERT_EQ(0, hl_host_find_in_path("x", out, 0));
}

UTEST(host, find_in_path_misses_cleanly)
{
    char out[512];
    out[0] = 'X';
    ASSERT_EQ(0, hl_host_find_in_path(
        "hull-definitely-not-a-real-binary-zzz", out, sizeof(out)));
    ASSERT_STREQ("", out);             /* miss clears the buffer */
}

/* Create a temp directory holding an executable probe file. Returns 1 on
 * success, 0 if the environment will not allow it (the caller then skips).
 *
 * Deliberately POSIX calls rather than system("mkdir -p") / system("chmod +x"):
 * a Cosmopolitan APE on Windows has no /bin/sh, so every shelling-out test
 * SKIPPED there - on the one platform whose PATH handling these tests exist to
 * pin. mkdir/fopen/chmod work on every host Hull runs on. */
static int host_make_probe(const char *tag, char *dir, size_t dir_sz,
                           const char *leaf)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(dir, dir_sz, "%s/hull-host-%s", tmp, tag);

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return 0;

    char exe[700];
    snprintf(exe, sizeof(exe), "%s/%s", dir, leaf);
    FILE *f = fopen(exe, "w");
    if (!f) return 0;
    fputs("#!/bin/sh\nexit 0\n", f);
    fclose(f);
    return chmod(exe, 0755) == 0;
}

/* Best-effort teardown for host_make_probe. */
static void host_drop_probe(const char *dir, const char *leaf)
{
    char exe[700];
    snprintf(exe, sizeof(exe), "%s/%s", dir, leaf);
    (void)unlink(exe);
    (void)rmdir(dir);
}

UTEST(host, find_in_path_finds_a_real_executable)
{
    /* Use a directory we control, joined with the HOST separator, so the test
     * does not depend on what happens to be installed. The point is that
     * splitting/joining works AT ALL: the shipped bug made this return 0 for
     * everything on Windows. hl_host_find_in_path_ex takes the search list
     * explicitly, so no environment mutation is needed. */
    char dir[512];
    if (!host_make_probe("test", dir, sizeof(dir), "hulltestprobe"))
        UTEST_SKIP("cannot create a temp probe");

    char list[2048];
    snprintf(list, sizeof(list), "%s%c%s", dir, hl_host_path_list_sep(),
             "/nonexistent-hull-dir");

    char out[700];
    int found = hl_host_find_in_path_ex(list, "hulltestprobe",
                                        out, sizeof(out));
    host_drop_probe(dir, "hulltestprobe");

    ASSERT_EQ(1, found);
    ASSERT_TRUE(strstr(out, "hulltestprobe") != NULL);
}

UTEST(host, find_in_path_splits_a_posix_shaped_list_on_any_host)
{
    /* THE WINDOWS BUG, in one assertion. A Cosmopolitan APE on Windows is
     * handed a POSIX-shaped PATH by its own runtime - measured on Windows 11:
     *
     *   PATH=/C/Users/.../ft_gcc:/C/Program Files (x86)/.../bin:...
     *
     * Inferring ';' from the host collapsed that entire list into one nonsense
     * component, so `hull doctor` reported every compiler missing however many
     * were installed. The separator has to come from the list, not the host,
     * which is what makes this pass on Windows as well as POSIX. */
    char dir[512];
    if (!host_make_probe("posixlist", dir, sizeof(dir), "hullprobeposix"))
        UTEST_SKIP("cannot create a temp probe");

    /* Colon-separated, forward slashes, and a component with a space in it -
     * the exact shape measured above. */
    char list[2048];
    snprintf(list, sizeof(list), "/C/Program Files (x86)/nowhere:%s", dir);

    char out[700];
    int found = hl_host_find_in_path_ex(list, "hullprobeposix",
                                        out, sizeof(out));

    host_drop_probe(dir, "hullprobeposix");
    ASSERT_EQ(1, found);
    /* Composed with the COMPONENT's separator, so it stays POSIX-shaped. */
    ASSERT_TRUE(strstr(out, "hull-host-posixlist/hullprobeposix") != NULL);
}

UTEST(host, find_in_path_splits_a_win32_shaped_list_on_semicolons)
{
    /* The mirror: a genuine Win32 list must still split on ';', and its ':'
     * drive letters must survive. Nothing here exists, so the assertion is
     * about not crashing and not reporting a bogus hit - the components are
     * the point, not the miss. */
    char out[512];
    ASSERT_EQ(0, hl_host_find_in_path_ex(
        "C:\tools;C:\Program Files\LLVM\bin;D:\bin",
        "hull-definitely-not-a-real-binary-zzz", out, sizeof(out)));
    ASSERT_STREQ("", out);
}

UTEST(host, find_in_path_keeps_a_single_win32_component_whole)
{
    /* A one-entry Win32 PATH carries no ';' but does carry the drive letter's
     * ':'. Splitting on ':' there would shred `C:\tools` into `C` and
     * `\tools`, so a lone drive-prefixed entry must be left whole. Again a
     * clean miss is the observable; the point is that it does not split. */
    char out[512];
    ASSERT_EQ(0, hl_host_find_in_path_ex(
        "C:\tools", "hull-definitely-not-a-real-binary-zzz",
        out, sizeof(out)));
    ASSERT_STREQ("", out);
}

UTEST(host, find_in_path_skips_empty_components)
{
    /* A leading / duplicated / trailing separator (very common on Windows)
     * must not abort the walk or probe the current directory. */
    char sep = hl_host_path_list_sep();
    char list[256];
    snprintf(list, sizeof(list), "%c%c/nonexistent-hull-dir%c", sep, sep, sep);

    char out[512];
    ASSERT_EQ(0, hl_host_find_in_path_ex(
        list, "hull-definitely-not-a-real-binary-zzz", out, sizeof(out)));
    ASSERT_STREQ("", out);
}

UTEST(host, find_in_path_ex_handles_an_empty_or_null_list)
{
    char out[64];
    ASSERT_EQ(0, hl_host_find_in_path_ex(NULL, "sh", out, sizeof(out)));
    ASSERT_EQ(0, hl_host_find_in_path_ex("",   "sh", out, sizeof(out)));
}

UTEST(host, find_in_path_ex_does_not_shred_windows_style_components)
{
    /* The shipped defect in one assertion. A Windows-shaped search list whose
     * components embed a colon must not make the walk crash or report a bogus
     * hit. On Windows the ';' split keeps `C:\tools` whole; on a POSIX host
     * ':' is the separator and the same string legitimately splits - either
     * way the answer for a name that does not exist is a clean miss. */
    char out[512];
    int rc = hl_host_find_in_path_ex("C:\\tools;C:\\other",
                                     "hull-definitely-not-a-real-binary-zzz",
                                     out, sizeof(out));
    ASSERT_EQ(0, rc);
    ASSERT_STREQ("", out);
}

UTEST(host, find_in_path_has_no_fixed_length_cap)
{
    /* An earlier revision copied PATH into a fixed 8 KB stack buffer, which
     * silently dropped everything past the cap. A Windows user PATH is a
     * registry value that can legitimately run to tens of KB, so a real
     * toolchain in a late entry would report as "not found" - the very bug
     * this function exists to fix. Bury a findable directory past 32 KB of
     * padding and require it to still resolve. */
    char dir[512];
    if (!host_make_probe("longpath", dir, sizeof(dir), "hulllongprobe"))
        UTEST_SKIP("cannot create a temp probe");

    size_t cap = 40000;
    char *list = (char *)malloc(cap);
    ASSERT_TRUE(list != NULL);
    char sep = hl_host_path_list_sep();
    size_t n = 0;
    /* ~32 KB of decoy components, then the real one LAST. */
    while (n < 32000) {
        int w = snprintf(list + n, cap - n, "/nonexistent-hull-pad-%zu%c", n, sep);
        if (w <= 0 || (size_t)w >= cap - n) break;
        n += (size_t)w;
    }
    snprintf(list + n, cap - n, "%s", dir);

    char out[700];
    int found = hl_host_find_in_path_ex(list, "hulllongprobe", out, sizeof(out));

    free(list);
    host_drop_probe(dir, "hulllongprobe");

    ASSERT_EQ(1, found);
    ASSERT_TRUE(strstr(out, "hulllongprobe") != NULL);
}

UTEST(host, find_in_path_never_probes_the_current_directory)
{
    /* An empty PATH component must not be joined into a cwd-relative
     * candidate: a file dropped in the process's working directory could
     * otherwise shadow a real toolchain binary. Build a list that is nothing
     * BUT empty components and assert a name that exists in the cwd is not
     * resolved through them. */
    FILE *f = fopen("hullcwdprobe", "w");
    if (!f) { UTEST_SKIP("cannot write into the working directory"); }
    fputs("#!/bin/sh\nexit 0\n", f);
    fclose(f);
    (void)chmod("hullcwdprobe", 0755);

    char sep = hl_host_path_list_sep();
    char list[32];
    snprintf(list, sizeof(list), "%c%c%c", sep, sep, sep);

    char out[512];
    int found = hl_host_find_in_path_ex(list, "hullcwdprobe", out, sizeof(out));

    remove("hullcwdprobe");

    ASSERT_EQ(0, found);
    ASSERT_STREQ("", out);
}

UTEST(host, find_in_path_leaves_out_empty_when_the_buffer_is_too_small)
{
    /* Contract: a 0 return means "not resolved" and MUST leave `out` empty.
     * snprintf writes a truncated string before reporting overflow, so the
     * too-small-buffer path has to clear it explicitly - otherwise a caller
     * that ignores the return value (or logs the buffer on a miss) gets a
     * truncated path, which names a DIFFERENT file than the one found. */
    char dir[512];
    if (!host_make_probe("small", dir, sizeof(dir), "hullsmallprobe"))
        UTEST_SKIP("cannot create a temp probe");

    /* Deliberately far too small to hold the resolved path. */
    char small[8];
    memset(small, 'X', sizeof(small));
    int found = hl_host_find_in_path_ex(dir, "hullsmallprobe",
                                        small, sizeof(small));

    host_drop_probe(dir, "hullsmallprobe");

    ASSERT_EQ(0, found);          /* cannot report a path that does not fit */
    ASSERT_STREQ("", small);      /* and must not leave a truncated one */
}

UTEST_MAIN()
