/*
 * commands/test.c — hull test subcommand
 *
 * In-process test runner: discovers test files, loads the app, wires
 * routes, executes test files with assertions. All output goes to
 * stdout in a human-readable format.
 *
 * Most of the orchestration lives in src/hull/test_runner.c (shared
 * with `hull agent test`, which uses a JSON writer). This file just
 * supplies the stdout writer and dispatches by app entry point.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/test.h"
#include "hull/app_context.h"
#include "hull/cap/test.h"
#include "hull/cap/tool.h"
#include "hull/test_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Usage ─────────────────────────────────────────────────────────── */

static void test_usage(void)
{
    fprintf(stderr, "Usage: hull test [app_dir]\n"
            "\n"
            "Discovers and runs test_*.[lua|js] files.\n");
}

/* ── Stdout writer (human-readable PASS/FAIL output) ────────────────── */

typedef struct {
    int total, passed, failed;
} StdoutWriterState;

static void stdout_on_file_start(void *user, const char *basename)
{
    (void)user;
    printf("\n--- %s ---\n", basename);
}

static void stdout_on_file_load_error(void *user, const char *err)
{
    (void)user;
    fprintf(stderr, "  ERROR: %s\n", err ? err : "unknown");
}

static void stdout_on_file_end(void *user,
                               const HlTestCaseResult *results, int count,
                               int file_total, int file_passed, int file_failed)
{
    (void)user;
    /* Print per-test PASS/FAIL lines collected by the runtime. The
     * runner stores them in results[]; we emit them here. */
    for (int i = 0; i < count; i++) {
        const HlTestCaseResult *r = &results[i];
        if (r->passed)
            printf("  PASS  %s\n", r->name);
        else
            printf("  FAIL  %s\n    %s\n", r->name, r->error);
    }
    (void)file_total;
    (void)file_passed;
    (void)file_failed;
}

static void stdout_on_summary(void *user,
                              int total, int passed, int failed)
{
    StdoutWriterState *s = user;
    s->total  = total;
    s->passed = passed;
    s->failed = failed;
    printf("\n%d/%d tests passed", passed, total);
    if (failed > 0) printf(", %d failed", failed);
    printf("\n");
}

/* Run tests for one app entry point (Lua or JS). */
static int run_tests_for_entry(const char *app_dir, const char *entry)
{
    HlAppContext *ctx = NULL;
    HlAppContextOpts opts = {
        .app_dir      = app_dir,
        .entry_point  = entry,
        .gate_modules = 1,
    };
    if (hl_app_context_init(&ctx, &opts) != 0) {
        fprintf(stderr, "hull test: failed to initialize app context\n");
        return 1;
    }

    StdoutWriterState state = {0};
    HlTestRunnerWriter writer = {
        .user                = &state,
        .on_file_start       = stdout_on_file_start,
        .on_file_load_error  = stdout_on_file_load_error,
        .on_file_end         = stdout_on_file_end,
        .on_summary          = stdout_on_summary,
    };
    int rc = hl_test_runner_run(ctx, &writer);
    hl_app_context_free(ctx);
    if (rc < 0) {
        /* Runner-level failure (no routes / no test files); load_error already emitted */
        return 1;
    }
    return state.failed > 0 ? 1 : 0;
}

/* ── Detect entry points ──────────────────────────────────────────── */

static const char *detect_lua_entry(const char *app_dir)
{
    static char lua_buf[4096];
    snprintf(lua_buf, sizeof(lua_buf), "%s/app.lua", app_dir);
    if (access(lua_buf, F_OK) == 0) return lua_buf;
    return NULL;
}

static const char *detect_js_entry(const char *app_dir)
{
    static char js_buf[4096];
    snprintf(js_buf, sizeof(js_buf), "%s/app.js", app_dir);
    if (access(js_buf, F_OK) == 0) return js_buf;
    return NULL;
}

/* ── Command entry point ───────────────────────────────────────────── */

int hl_cmd_test(int argc, char **argv, const HlCommandEnv *env)
{
    const char *app_dir = env->app_dir;
    if (argc >= 2 && argv[1][0] != '-')
        app_dir = argv[1];

    const char *lua_entry = detect_lua_entry(app_dir);
    const char *js_entry  = detect_js_entry(app_dir);

    if (!lua_entry && !js_entry) {
        fprintf(stderr, "hull test: no entry point found (app.js or app.lua) in %s\n",
                app_dir);
        test_usage();
        return 1;
    }

    int result = 0;
    int ran_any = 0;

#ifdef HL_ENABLE_LUA
    if (lua_entry) {
        char **lua_tests = hl_tool_find_files(app_dir, "test_*.lua", NULL);
        if (lua_tests && lua_tests[0]) {
            ran_any = 1;
            result |= run_tests_for_entry(app_dir, lua_entry);
        }
        if (lua_tests) {
            for (char **fp = lua_tests; *fp; fp++) free(*fp);
            free(lua_tests);
        }
    }
#endif

#ifdef HL_ENABLE_JS
    if (js_entry) {
        char **js_tests = hl_tool_find_files(app_dir, "test_*.js", NULL);
        if (js_tests && js_tests[0]) {
            ran_any = 1;
            result |= run_tests_for_entry(app_dir, js_entry);
        }
        if (js_tests) {
            for (char **fp = js_tests; *fp; fp++) free(*fp);
            free(js_tests);
        }
    }
#endif

    // cppcheck-suppress knownConditionTrueFalse
    if (!ran_any) {
        fprintf(stderr, "hull test: no test files found in %s\n", app_dir);
        return 1;
    }

    return result;
}
