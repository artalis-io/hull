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
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(out, out_sz, "%s/hull-cli-log-%s.txt", tmp, tag);
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

UTEST_MAIN()
