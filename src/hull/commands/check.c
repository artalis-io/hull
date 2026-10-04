/*
 * commands/check.c - hull check: modules + test + verify in one pass
 *
 * Runs each step sequentially, stopping on first failure. Intended for
 * CI validation and a single command before pushing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/check.h"
#ifdef HL_ENABLE_HTTP_SERVER
#include "hull/commands/test.h"
#endif
#include "hull/commands/verify.h"
#include "hull/commands/modules.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int hl_cmd_check(int argc, char **argv, const HlCommandEnv *env)
{
    /* One app directory for every step: the positional (as the test and
     * verify steps take it), else --app-dir. Steps 1-2 used --app-dir
     * (default ".") while 3-4 used the positional, so `hull check
     * services/api` from a monorepo root validated the ROOT app's manifest
     * and imports and passed a CI gate for the wrong app. */
    const char *app_dir = env->app_dir;
    int positional = 0;
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
