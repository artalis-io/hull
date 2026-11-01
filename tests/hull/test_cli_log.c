/*
 * test_cli_log.c - CLI logging policy (src/hull/shared/cli_log.c).
 *
 * Regression coverage for the shipped defect: ordinary commands such as
 * `hull new` and `hull build` printed Hull's own internal source coordinates,
 *
 *     INFO src/hull/sandbox_tool.c:243: [sandbox] tool mode applied
 *
 * because nothing ever configured rxi/log.c on the CLI path, so its
 * zero-initialised default (level TRACE, quiet false, built-in file:line
 * callback) applied.
 *
 * The tests capture what the policy actually writes to its stream, so they
 * assert the OUTPUT contract rather than the implementation:
 *
 *   default:   no internal file:line, no sub-WARN Hull chatter, no app output
 *              from the build-time evaluation window; warnings/errors survive.
 *   --verbose: everything, with file:line, and app-phase records tagged.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/shared/cli_log.h"
#include "hull/shared/term_escape.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_tmpdir.h"

/* ── Source-domain classification ──────────────────────────────────── */

UTEST(cli_log, classifies_c_translation_units_as_native)
{
    ASSERT_EQ(1, hl_cli_log_is_native_source("src/hull/sandbox_tool.c"));
    ASSERT_EQ(1, hl_cli_log_is_native_source("cli_log.c"));
    ASSERT_EQ(1, hl_cli_log_is_native_source("include/hull/runtime.h"));
}

UTEST(cli_log, classifies_script_sources_as_not_native)
{
    /* These are what a Lua/JS log call site passes: a chunk short_src. */
    ASSERT_EQ(0, hl_cli_log_is_native_source("./app.lua"));
    ASSERT_EQ(0, hl_cli_log_is_native_source("app.js"));
    ASSERT_EQ(0, hl_cli_log_is_native_source("hull.build"));
    ASSERT_EQ(0, hl_cli_log_is_native_source("routes/users.lua"));
}

UTEST(cli_log, classification_is_null_safe)
{
    ASSERT_EQ(0, hl_cli_log_is_native_source(NULL));
    ASSERT_EQ(0, hl_cli_log_is_native_source(""));
    ASSERT_EQ(0, hl_cli_log_is_native_source("c"));   /* too short to be x.c */
}

/* ── App-phase flag ────────────────────────────────────────────────── */

UTEST(cli_log, app_phase_toggles)
{
    ASSERT_EQ(0, hl_cli_log_app_phase());
    hl_cli_log_set_app_phase(1);
    ASSERT_EQ(1, hl_cli_log_app_phase());
    hl_cli_log_set_app_phase(0);
    ASSERT_EQ(0, hl_cli_log_app_phase());
}

/* ── Emitted output ────────────────────────────────────────────────── */

/* The policy writes through log.c's callback list. Rather than reach into
 * cli_log's statics, install the SAME policy and redirect its stream by
 * pointing the process's stderr at a temp file for the duration. */
static char captured[8192];

/* Build a temp path so `make test` never leaves scratch files in the tree. */
static void capture_path(char *out, size_t out_sz, const char *tag)
{
    hl_test_path(out, out_sz, "hull-cli-log-%s.txt", tag);
}

static void capture_begin(FILE **saved_out, const char *path)
{
    fflush(stderr);
    *saved_out = freopen(path, "w+", stderr);
}

static void capture_end(const char *path)
{
    fflush(stderr);
    captured[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(captured, 1, sizeof(captured) - 1, f);
    captured[n] = '\0';
    fclose(f);
}

UTEST(cli_log, default_hides_internal_source_paths_and_info_chatter)
{
    FILE *saved = NULL;
    char path[512];
    capture_path(path, sizeof(path), "default");

    hl_cli_log_init(0);                       /* default (non-verbose) */
    capture_begin(&saved, path);
    /* Exactly the record that leaked into `hull build` output. */
    log_log(LOG_INFO, "src/hull/sandbox_tool.c", 243,
            "[sandbox] tool mode applied (%d unveiled paths)", 16);
    /* A genuine warning MUST survive - we are not hiding failures. */
    log_log(LOG_WARN, "src/hull/build_assets.c", 12, "something is off");
    capture_end(path);
    (void)saved;

    /* No internal implementation coordinates in polished CLI output. */
    ASSERT_TRUE(strstr(captured, "src/hull/sandbox_tool.c") == NULL);
    ASSERT_TRUE(strstr(captured, "src/hull/build_assets.c") == NULL);
    ASSERT_TRUE(strstr(captured, ":243") == NULL);
    /* The INFO-level internal chatter is gone entirely. */
    ASSERT_TRUE(strstr(captured, "tool mode applied") == NULL);
    /* The warning is still reported, and reads as tool output. */
    ASSERT_TRUE(strstr(captured, "something is off") != NULL);
    ASSERT_TRUE(strstr(captured, "hull: warn:") != NULL);

    remove(path);
}

UTEST(cli_log, default_suppresses_app_output_during_build_evaluation)
{
    FILE *saved = NULL;
    char path[512];
    capture_path(path, sizeof(path), "appphase");

    hl_cli_log_init(0);
    capture_begin(&saved, path);

    /* Outside the window, an app's own log.info is normal output. */
    log_log(LOG_INFO, "./app.lua", 25, "[app] app loaded");

    /* Inside it, the same record is an artifact of `hull build` executing the
     * entry to capture app.manifest() - not something the user asked for. */
    hl_cli_log_set_app_phase(1);
    log_log(LOG_INFO, "./app.lua", 25, "[app] evaluated during build");
    hl_cli_log_set_app_phase(0);

    capture_end(path);
    (void)saved;

    ASSERT_TRUE(strstr(captured, "app loaded") != NULL);
    ASSERT_TRUE(strstr(captured, "evaluated during build") == NULL);

    remove(path);
}

UTEST(cli_log, verbose_restores_full_diagnostics)
{
    FILE *saved = NULL;
    char path[512];
    capture_path(path, sizeof(path), "verbose");

    hl_cli_log_init(1);                       /* --verbose */
    capture_begin(&saved, path);
    log_log(LOG_INFO, "src/hull/sandbox_tool.c", 243,
            "[sandbox] tool mode applied");
    hl_cli_log_set_app_phase(1);
    log_log(LOG_INFO, "./app.lua", 25, "[app] app loaded");
    hl_cli_log_set_app_phase(0);
    capture_end(path);
    (void)saved;

    /* Rich diagnostics come back: level, file, line. */
    ASSERT_TRUE(strstr(captured, "src/hull/sandbox_tool.c:243") != NULL);
    ASSERT_TRUE(strstr(captured, "tool mode applied") != NULL);
    ASSERT_TRUE(strstr(captured, "INFO") != NULL);
    /* App output during a build is shown, but labelled so it is obvious WHY
     * application logging appears in the middle of a build. */
    ASSERT_TRUE(strstr(captured, "app loaded") != NULL);
    ASSERT_TRUE(strstr(captured, "[build-eval]") != NULL);

    hl_cli_log_init(0);                       /* restore for other suites */
    remove(path);
}

UTEST(cli_log, suspend_silences_the_cli_callback)
{
    /* `hull jobs worker` reaches hull_serve IN-PROCESS, so the dispatcher's
     * CLI policy and the server's own callback are both registered. log.c
     * invokes EVERY callback and cannot unregister one, so without the
     * suspend hand-off each surviving record printed TWICE in two formats.
     * After suspend, the CLI callback must emit nothing at all - including
     * records it would otherwise have shown. */
    FILE *saved = NULL;
    char path[512];
    capture_path(path, sizeof(path), "suspend");

    hl_cli_log_init(0);
    ASSERT_EQ(0, hl_cli_log_suspended());

    hl_cli_log_suspend();
    ASSERT_EQ(1, hl_cli_log_suspended());

    capture_begin(&saved, path);
    log_log(LOG_WARN,  "src/hull/serve.c", 10, "a warning the server owns");
    log_log(LOG_ERROR, "src/hull/serve.c", 11, "an error the server owns");
    log_log(LOG_INFO,  "./app.lua",        12, "[app] app said something");
    capture_end(path);
    (void)saved;

    /* Not "filtered differently" - completely silent. The server's callback
     * is the single emitter once ownership has transferred. */
    ASSERT_STREQ("", captured);

    /* Suspend is one-way in production, so hand the flag back explicitly -
     * otherwise this test would silence whichever tests utest happens to run
     * after it. */
    hl_cli_log_reset_for_test();
    ASSERT_EQ(0, hl_cli_log_suspended());

    remove(path);
}

/* Audit 27: app text that reaches a log call by a path other than log.*
 * (an uncaught exception's message) wrote raw newlines, so it could forge
 * records. Both callbacks escape CR / LF; a message already escaped is left
 * as it is. */
UTEST(cli_log, escape_line_writes_crlf_as_two_characters)
{
    char out[64];
    hl_log_escape_line("x\n12:00:00 INFO  forged\r", out, sizeof out);
    ASSERT_STREQ("x\\n12:00:00 INFO  forged\\r", out);
    hl_log_escape_line("already \\n escaped", out, sizeof out);
    ASSERT_STREQ("already \\n escaped", out);
    hl_log_escape_line("ab\ncd", out, 4);         /* truncated, never split */
    ASSERT_STREQ("ab", out);
    hl_log_escape_line(NULL, out, sizeof out);
    ASSERT_STREQ("", out);
    /* Audit 28: other controls (ESC, a terminal escape) as \xNN; tab stays. */
    hl_log_escape_line("a\x1b[2Jb\tc\x7f", out, sizeof out);
    ASSERT_STREQ("a\\x1b[2Jb\tc\\x7f", out);
    hl_log_escape_line("ab\x1b", out, 6);        /* \x1b would not fit whole */
    ASSERT_STREQ("ab", out);
}

/* Audit 42: the C1 controls (U+0080-U+009F, UTF-8 C2 80-9F) and stray bytes
 * 0x80-0x9F passed through raw - 0x9B is CSI and 0x9D OSC to a terminal in
 * 8-bit mode. They are \xNN per byte now; other UTF-8 is kept whole. */
UTEST(cli_log_audit42, escape_line_c1_controls)
{
    char out[128];
    hl_log_escape_line("a\xc2\x9b" "2Jb", out, sizeof out);      /* U+009B CSI */
    ASSERT_STREQ("a\\xc2\\x9b2Jb", out);
    hl_log_escape_line("\xc2\x9d" "52;c;x\xc2\x9c", out, sizeof out); /* OSC..ST */
    ASSERT_STREQ("\\xc2\\x9d52;c;x\\xc2\\x9c", out);
    hl_log_escape_line("x\x9by\x80", out, sizeof out);          /* stray bytes */
    ASSERT_STREQ("x\\x9by\\x80", out);
    /* Valid UTF-8 whose continuation bytes are in 0x80-0x9F is kept. */
    hl_log_escape_line("\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80 \xc2\xa0", out,
                       sizeof out);
    ASSERT_STREQ("\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80 \xc2\xa0", out);
    /* A truncated or overlong sequence is not UTF-8: its 0x80-0x9F bytes are
     * stray; a stray 0xA0+ byte is kept. */
    hl_log_escape_line("\xe2\x82" "z\xc0\x9b" "\xff", out, sizeof out);
    ASSERT_STREQ("\xe2\\x82z\xc0\\x9b\xff", out);
    /* A unit that does not fit ends the record: never half of one. */
    hl_log_escape_line("ab\xc2\x9b", out, 10);
    ASSERT_STREQ("ab", out);
    hl_log_escape_line("ab\xe2\x82\xac", out, 5);
    ASSERT_STREQ("ab", out);
}

UTEST(cli_log_audit42, fputs_escaped_c1_controls)
{
    char path[512];
    capture_path(path, sizeof(path), "c1");
    FILE *f = fopen(path, "w+");
    ASSERT_TRUE(f != NULL);
    hl_log_fputs_escaped(f, "PASS\xc2\x9b" "1A\x9d\xc3\xa9\n");
    fflush(f);
    rewind(f);
    char buf[128] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    remove(path);
    buf[n] = '\0';
    ASSERT_STREQ("PASS\\xc2\\x9b1A\\x9d\xc3\xa9\\n", buf);
}

/* The tool VM's writers (shared/term_escape.h) take the same rule: LF / tab
 * kept for hl_term_fwrite_escaped, CR / LF escaped for the line writer. */
UTEST(term_escape_audit42, writers_escape_c1_controls)
{
    char path[512];
    capture_path(path, sizeof(path), "term_c1");
    FILE *f = fopen(path, "w+");
    ASSERT_TRUE(f != NULL);
    const char s[] = "a\xc2\x9b" "b\n\x9c\t\xe2\x82\xac\r";
    hl_term_fwrite_escaped(f, s, sizeof s - 1);
    fputc('|', f);
    hl_term_fputs_line_escaped(f, s);
    fflush(f);
    rewind(f);
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    remove(path);
    buf[n] = '\0';
    ASSERT_STREQ("a\\xc2\\x9bb\n\\x9c\t\xe2\x82\xac\\r"
                 "|a\\xc2\\x9bb\\n\\x9c\t\xe2\x82\xac\\r", buf);
    /* hl_term_high_unit itself. */
    int esc = -1;
    ASSERT_EQ((size_t)2, hl_term_high_unit((const unsigned char *)"\xc2\x85", 2, &esc));
    ASSERT_EQ(1, esc);
    ASSERT_EQ((size_t)2, hl_term_high_unit((const unsigned char *)"\xc2\xa0", 2, &esc));
    ASSERT_EQ(0, esc);
    ASSERT_EQ((size_t)1, hl_term_high_unit((const unsigned char *)"\xc2", 1, &esc));
    ASSERT_EQ(0, esc);                       /* truncated lead, >= 0xA0: kept */
    ASSERT_EQ((size_t)1, hl_term_high_unit((const unsigned char *)"\xed\xa0\x80", 3, &esc));
    ASSERT_EQ(0, esc);                       /* surrogate form: not UTF-8 */
    ASSERT_EQ((size_t)1, hl_term_high_unit((const unsigned char *)"\x85", 1, &esc));
    ASSERT_EQ(1, esc);
}

UTEST(cli_log, a_record_with_a_newline_is_one_line)
{
    FILE *saved = NULL;
    char path[512];
    capture_path(path, sizeof(path), "crlf");
    hl_cli_log_init(0);
    capture_begin(&saved, path);
    log_log(LOG_ERROR, "./app.lua", 1, "boom\nhull: error: forged");
    capture_end(path);
    (void)saved;
    ASSERT_TRUE(strstr(captured, "boom\\nhull: error: forged\n") != NULL);
    ASSERT_TRUE(strchr(captured, '\n') == captured + strlen(captured) - 1);
    remove(path);
}

/* Audit 28: the app.main runner (serve_cli.c) left log.c's default callback,
 * which prints the message raw. It now installs the same escaped line as
 * serve.c. Last in the file: log.c cannot unregister a callback. */
UTEST(cli_log, line_callback_escapes_and_omits_file_line)
{
    FILE *saved = NULL;
    char path[512];
    capture_path(path, sizeof(path), "line");
    capture_begin(&saved, path);
    hl_log_install_line_callback(LOG_TRACE);
    log_log(LOG_ERROR, "src/hull/runtime/lua/async.c", 9,
            "task error: x\n12:00:00 ERROR [hull:sandbox] forged");
    capture_end(path);
    (void)saved;
    ASSERT_TRUE(strstr(captured,
        "ERROR task error: x\\n12:00:00 ERROR [hull:sandbox] forged\n") != NULL);
    ASSERT_TRUE(strstr(captured, "\n12:00:00 ERROR [hull:sandbox]") == NULL);
#ifndef DEBUG
    ASSERT_TRUE(strstr(captured, "async.c:9") == NULL);
#endif
    remove(path);
}

UTEST_MAIN()
