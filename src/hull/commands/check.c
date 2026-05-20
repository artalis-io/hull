/*
 * commands/check.c — hull check: modules + test + verify in one pass
 *
 * Runs each step sequentially, stopping on first failure. Intended for
 * CI validation and a single command before pushing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/check.h"
#ifdef HL_ENABLE_HTTP
#include "hull/commands/test.h"
#endif
#include "hull/commands/verify.h"
#include "hull/commands/modules.h"

#include <stdio.h>
#include <string.h>

int hl_cmd_check(int argc, char **argv, const HlCommandEnv *env)
{
    /* Step 1: load the app's manifest and print declared modules.
     * Surfaces top-level load errors (unparseable manifest, missing
     * entry point) before the test runner has to. */
    fprintf(stderr, "[hull:check] validating manifest...\n");
    {
        const char *list_argv[3] = { "check", "list", env->app_dir };
        int rc = hl_cmd_modules(3, (char **)list_argv, env);
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
        const char *analyze_argv[3] = { "check", "analyze", env->app_dir };
        int rc = hl_cmd_modules(3, (char **)analyze_argv, env);
        if (rc != 0) {
            fprintf(stderr, "[hull:check] import analysis failed\n");
            return rc;
        }
    }

#ifdef HL_ENABLE_HTTP
    fprintf(stderr, "[hull:check] running tests...\n");
    int rc = hl_cmd_test(argc, argv, env);
    if (rc != 0) {
        fprintf(stderr, "[hull:check] tests failed\n");
        return rc;
    }
#else
    int rc = 0;
    fprintf(stderr, "[hull:check] (test runner unavailable on HTTP=0 builds)\n");
#endif

    fprintf(stderr, "[hull:check] running verify...\n");
    rc = hl_cmd_verify(argc, argv, env);
    if (rc != 0) {
        fprintf(stderr, "[hull:check] verify failed\n");
        return rc;
    }

    fprintf(stderr, "[hull:check] all checks passed\n");
    return 0;
}
