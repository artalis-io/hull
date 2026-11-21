/*
 * commands/check.c - hull check: modules + test + verify in one pass
 *
 * Runs each step sequentially, stopping on first failure. Intended for
 * CI validation and a single command before pushing. `--types` adds a
 * type check first, with a TypeScript compiler already installed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/check.h"
#ifdef HL_ENABLE_HTTP_SERVER
#include "hull/commands/test.h"
#endif
#include "hull/commands/verify.h"
#include "hull/commands/modules.h"
#include "hull/cap/tool.h"
#include "hull/shared/type_check.h"
#include "hull/shared/term_escape.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* `--types`: the compiler over the app's tsconfig.json (`hull types`
 * writes it), then tests/tsconfig.json when there is one, with nothing
 * emitted. Run before every other step: the ones that load the app go
 * through the tool sandbox, which stays applied to this process, and a
 * compiler under ~/.nvm or the app's node_modules could not run there. */
static int check_types(const char *app_dir)
{
    if (!hl_type_check_configured(app_dir)) {
        hl_eprintf_line("[hull:check] --types: no tsconfig.json in %s; "
                        "install the declarations with `hull types %s`", app_dir, app_dir);
        return 1;
    }
    char checker[PATH_MAX];
    if (hl_type_check_find(app_dir, 1, checker, sizeof checker) != 0) {
        fprintf(stderr, "[hull:check] --types: no TypeScript compiler found - install one "
                "in the app (npm install --save-dev typescript) or on PATH (tsgo or tsc)\n");
        return 1;
    }
    hl_eprintf_line("[hull:check] type-checking with %s", checker);
    const char *argv[] = { checker, HL_TYPE_CHECK_ARGS, "-p", app_dir, NULL };
    int rc = hl_tool_spawn(argv);
    if (rc != 0) return rc < 0 ? 1 : rc;

    char tests[PATH_MAX];
    int n = snprintf(tests, sizeof tests, "%s/tests", app_dir);
    if (n < 0 || (size_t)n >= sizeof tests) return 1;
    if (hl_type_check_configured(tests)) {
        const char *targv[] = { checker, HL_TYPE_CHECK_ARGS, "-p", tests, NULL };
        rc = hl_tool_spawn(targv);
        if (rc != 0) return rc < 0 ? 1 : rc;
    }
    return 0;
}

int hl_cmd_check(int argc, char **argv, const HlCommandEnv *env)
{
    /* One app directory for every step: the positional (as the test and
     * verify steps take it), else --app-dir. Steps 1-2 used --app-dir
     * (default ".") while 3-4 used the positional, so `hull check
     * services/api` from a monorepo root validated the ROOT app's manifest
     * and imports and passed a CI gate for the wrong app. */
    const char *app_dir = env->app_dir;
    int positional = 0;
    int types = 0;
    /* The options hull check forwards to verify that take a value, which
     * is not the app directory (`hull check --developer-key k.pub api`). */
    static const char *const value_flags[] = {
        "--platform-key", "--developer-key", "--gethull-key", "--binary",
        "--app-dir", NULL
    };
    char *verify_argv[16];
    int vc = 0;
    verify_argv[vc++] = (char *)(uintptr_t)"verify";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') {
            if (positional) {
                fprintf(stderr, "hull check: unexpected argument '%s' "
                        "(one app directory)\n", a);
                return 2;
            }
            app_dir = a;
            positional = 1;
            continue;
        }
        int takes = 0;
        for (int k = 0; value_flags[k]; k++)
            if (strcmp(a, value_flags[k]) == 0) { takes = 1; break; }
        if (takes && i + 1 >= argc) {
            fprintf(stderr, "hull check: %s needs a value\n", a);
            return 2;
        }
        if (strcmp(a, "--app-dir") == 0) { i++; continue; }  /* env has it */
        if (strcmp(a, "--types") == 0) { types = 1; continue; }
        if (strcmp(a, "--verbose") == 0 || strcmp(a, "--json") == 0 ||
            strncmp(a, "--app-dir=", 10) == 0)
            continue;   /* global flags, read by the dispatcher */
        /* The rest are verify's (--no-verify-platform, the keys). */
        if (vc + (takes ? 2 : 1) >= (int)(sizeof verify_argv / sizeof verify_argv[0]) - 1) {
            fprintf(stderr, "hull check: too many options\n");
            return 2;
        }
        verify_argv[vc++] = argv[i];
        if (takes) verify_argv[vc++] = argv[++i];
    }
    verify_argv[vc++] = (char *)(uintptr_t)app_dir;   /* verify takes the LAST positional */
    verify_argv[vc] = NULL;

    /* Step 0 (--types): the type check, before anything applies the tool
     * sandbox (check_types). */
    if (types) {
        fprintf(stderr, "[hull:check] checking types...\n");
        int rc = check_types(app_dir);
        if (rc != 0) {
            fprintf(stderr, "[hull:check] type check failed\n");
            return rc;
        }
    }

    /* Step 1: load the app's manifest and print declared modules.
     * Surfaces top-level load errors (unparseable manifest, missing
     * entry point) before the test runner has to. */
    fprintf(stderr, "[hull:check] validating manifest...\n");
    {
        const char *list_argv[3] = { "check", "list", app_dir };
        int rc = hl_cmd_modules(3, (char **)(uintptr_t)list_argv, env);  /* hl_cmd_modules does not modify argv */
        if (rc != 0) {
            fprintf(stderr, "[hull:check] manifest validation failed\n");
            return rc;
        }
    }

    /* Step 2: static import/require analysis. Walks source files for
     * `require("hull.X")` / `import "hull:X"` calls and compares
     * against the declared modules. Exits non-zero on undeclared
     * imports (would otherwise fail at runtime when the code path
     * executes); unused declarations are advisory only. */
    fprintf(stderr, "[hull:check] analyzing imports...\n");
    {
        const char *analyze_argv[3] = { "check", "analyze", app_dir };
        int rc = hl_cmd_modules(3, (char **)(uintptr_t)analyze_argv, env);  /* hl_cmd_modules does not modify argv */
        if (rc != 0) {
            fprintf(stderr, "[hull:check] import analysis failed\n");
            return rc;
        }
    }

#ifdef HL_ENABLE_HTTP_SERVER
    fprintf(stderr, "[hull:check] running tests...\n");
    /* Explicit argv for each step, naming the app directory resolved above:
     * hl_cmd_test takes argv[1] only, so forwarding the raw argv
     * (`hull check --no-verify-platform services/api`) tested the ROOT
     * app, and verify ignored --app-dir. */
    const char *test_argv[3] = { "test", app_dir, NULL };
    int rc = hl_cmd_test(2, (char **)(uintptr_t)test_argv, env);  /* does not modify argv */
    if (rc != 0) {
        fprintf(stderr, "[hull:check] tests failed\n");
        return rc;
    }
#else
    int rc = 0;
    fprintf(stderr, "[hull:check] (test runner unavailable on HTTP=0 builds)\n");
#endif

    fprintf(stderr, "[hull:check] running verify...\n");
    rc = hl_cmd_verify(vc, verify_argv, env);
    if (rc != 0) {
        fprintf(stderr, "[hull:check] verify failed\n");
        return rc;
    }

    fprintf(stderr, "[hull:check] all checks passed\n");
    return 0;
}
