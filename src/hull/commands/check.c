/*
 * commands/check.c — hull check: test + verify in one pass
 *
 * Runs test and verify sequentially, stopping on first failure.
 * Uses env->app_dir for both commands. Intended for CI validation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/check.h"
#include "hull/commands/test.h"
#include "hull/commands/verify.h"

#include <stdio.h>

int hl_cmd_check(int argc, char **argv, const HlCommandEnv *env)
{
    fprintf(stderr, "[hull:check] running tests...\n");
    int rc = hl_cmd_test(argc, argv, env);
    if (rc != 0) {
        fprintf(stderr, "[hull:check] tests failed\n");
        return rc;
    }

    fprintf(stderr, "[hull:check] running verify...\n");
    rc = hl_cmd_verify(argc, argv, env);
    if (rc != 0) {
        fprintf(stderr, "[hull:check] verify failed\n");
        return rc;
    }

    fprintf(stderr, "[hull:check] all checks passed\n");
    return 0;
}
