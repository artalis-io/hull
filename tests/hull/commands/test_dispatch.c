/*
 * test_dispatch.c - Tests for subcommand dispatcher
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

/* The private installer checksum helper (src/hull/commands/asset_checksum.h is not
 * on the public -Iinclude path, so forward-declare it; the symbol is in CMD_OBJS,
 * which this test links). H1 S2a de-duplicated feature.c/flavor.c onto it. */
int hl_asset_checksum_eq(const char a[64], const char b[64]);

UTEST(asset_checksum, fixed64_equal_and_diffs)
{
    char a[64], b[64];
    memset(a, 'a', 64); memset(b, 'a', 64);
    ASSERT_EQ(hl_asset_checksum_eq(a, b), 1);   /* equal over the 64 */
    b[0] = 'b';  ASSERT_EQ(hl_asset_checksum_eq(a, b), 0);   /* differ at index 0 */
    b[0] = 'a';  b[63] = 'b';
    ASSERT_EQ(hl_asset_checksum_eq(a, b), 0);   /* differ at index 63 */
}

UTEST_MAIN();
