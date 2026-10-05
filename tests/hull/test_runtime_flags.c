/*
 * test_runtime_flags.c - the --hull-<name> rewrite and the built-binary
 * downgrade rule (include/hull/runtime_flags.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/runtime_flags.h"

#include <stdio.h>
#include <string.h>

/* A long --hull-ca-bundle=PATH used to overflow the 96-byte rewrite pool:
 * the function returned 0 and left "--hull-..." in place, which a built
 * binary then took as the start of the app's arguments. */
UTEST(runtime_flags, long_value_is_rewritten_not_dropped)
{
    char arg[700];
    char path[600];
    memset(path, 'a', sizeof path - 1);
    path[0] = '/';
    path[sizeof path - 1] = '\0';
    snprintf(arg, sizeof arg, "--hull-ca-bundle=%s", path);
    char *a = arg;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), 1);
    ASSERT_EQ(strncmp(a, "--ca-bundle=", 12), 0);
    ASSERT_STREQ(a + 12, path);
}

/* The pool held 32 rewrites; the 33rd was silently left prefixed. */
UTEST(runtime_flags, many_rewrites_all_taken)
{
    for (int i = 0; i < 100; i++) {
        char buf[] = "--hull-no-sandbox";
        char *a = buf;
        ASSERT_EQ(hl_runtime_flag_unprefix(&a), 1);
        ASSERT_STREQ(a, "--no-sandbox");
    }
}

UTEST(runtime_flags, one_letter_names)
{
    char buf[] = "--hull-d";
    char *a = buf;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), 1);
    ASSERT_STREQ(a, "-d");

    /* There is no "-d=x" form: refuse rather than produce an option that
     * matches nothing (and so became an app argument). */
    char buf2[] = "--hull-d=x.db";
    a = buf2;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), -1);
}

UTEST(runtime_flags, not_prefixed)
{
    char b1[] = "--no-sandbox";
    char *a = b1;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), 0);
    ASSERT_STREQ(a, "--no-sandbox");
    char b2[] = "--hull-";
    a = b2;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), 0);
    char b3[] = "--hull-=x";
    a = b3;
    ASSERT_EQ(hl_runtime_flag_unprefix(&a), -1);
}

UTEST(runtime_flags, downgrades_need_the_prefix_in_a_built_binary)
{
    static const char *const down[] = {
        "--no-sandbox", "--ca-bundle=/x", "--max-instructions",
        "-b", "-d", "-m", "-M", "-s", "--tls-cert", "--tls-key",
        "--wasm-gas", "--wasm-heap", "--wasm-stack", "--wasm-max-input",
        "--wasm-max-output", "--body-max-size",
        /* Raise the WASM wall-clock bound (to 1 h) / the connection cap. */
        "--wasm-timeout-ms", "--wasm-timeout-ms=3600000",
        "--max-connections", NULL
    };
    for (int i = 0; down[i]; i++) {
        EXPECT_EQ(hl_runtime_flag_is_downgrade(down[i]), 1);
        EXPECT_EQ(hl_runtime_flag_check(down[i], 0, 1, NULL), -1);   /* bare, built */
        EXPECT_EQ(hl_runtime_flag_check(down[i], 1, 1, NULL), 0);    /* prefixed */
        EXPECT_EQ(hl_runtime_flag_check(down[i], 0, 0, NULL), 0);    /* under hull */
    }
    static const char *const ok[] = {
        "-p", "-l", "--no-migrate", "--verify-sig", "--audit", "-dx",
        "--bogus", "--help", NULL
    };
    for (int i = 0; ok[i]; i++)
        EXPECT_EQ(hl_runtime_flag_is_downgrade(ok[i]), 0);
}

/* The app.main runner (serve_cli.c) implements only some downgrade options.
 * The rest are a built CLI tool's own arguments: refusing `./tool -s pat`
 * protected nothing (the runner has no -s) and left no spelling that reached
 * the app. The ones it does implement stay reserved. */
UTEST(runtime_flags, cli_runner_reserves_only_what_it_implements)
{
    const char *const *cli = hl_runtime_flag_cli_taken();
    static const char *const app_owned[] = {
        "-s", "-m", "-M", "-b", "--tls-cert", "--tls-key", "--wasm-gas",
        "--wasm-heap", "--wasm-stack", "--wasm-timeout-ms", "--wasm-max-input",
        "--wasm-max-output", "--body-max-size", "--max-connections",
        "--agent-api", NULL
    };
    for (int i = 0; app_owned[i]; i++) {
        EXPECT_EQ(hl_runtime_flag_check(app_owned[i], 0, 1, cli), 0);
        /* serve.c implements them all, so there they stay reserved. */
        EXPECT_EQ(hl_runtime_flag_check(app_owned[i], 0, 1, NULL), -1);
    }
    static const char *const reserved[] = {
        "--no-sandbox", "--allow-degraded-sandbox", "--no-ca-bundle",
        "--skip-ca-bundle", "--ca-bundle", "--ca-bundle=/x",
        "--no-verify-platform", "--max-instructions", "-d", NULL
    };
    for (int i = 0; reserved[i]; i++) {
        EXPECT_EQ(hl_runtime_flag_check(reserved[i], 0, 1, cli), -1);
        EXPECT_EQ(hl_runtime_flag_check(reserved[i], 1, 1, cli), 0);
        EXPECT_EQ(hl_runtime_flag_is_downgrade(reserved[i]), 1);
    }
}

UTEST(runtime_flags, unknown_prefixed_is_an_error)
{
    EXPECT_EQ(hl_runtime_flag_unknown("--bogus"), -1);
    EXPECT_EQ(hl_runtime_flag_unknown("-q"), -1);
}

UTEST_MAIN();
