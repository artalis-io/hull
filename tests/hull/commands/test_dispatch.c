/*
 * test_dispatch.c — Tests for subcommand dispatcher
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/commands/dispatch.h"

/* ── Dispatch tests ───────────────────────────────────────────────── */

UTEST(dispatch, no_args_returns_neg1)
{
    char *argv[] = { "hull" };
    int rc = hl_command_dispatch(1, argv);
    ASSERT_EQ(rc, -1);
}

UTEST(dispatch, unknown_command_returns_neg1)
{
    char *argv[] = { "hull", "nonexistent" };
    int rc = hl_command_dispatch(2, argv);
    ASSERT_EQ(rc, -1);
}

UTEST(dispatch, server_args_return_neg1)
{
    /* Server flags like -p 3000 should not match any command */
    char *argv[] = { "hull", "-p", "3000" };
    int rc = hl_command_dispatch(3, argv);
    ASSERT_EQ(rc, -1);
}

UTEST(dispatch, entry_point_returns_neg1)
{
    /* A .lua or .js file should not match a command */
    char *argv[] = { "hull", "app.lua" };
    int rc = hl_command_dispatch(2, argv);
    ASSERT_EQ(rc, -1);
}

/*
 * We can't easily test that known commands dispatch correctly without
 * side effects, but we can verify the negative cases above which confirm
 * the dispatcher correctly falls through for non-command args.
 */

UTEST_MAIN();
