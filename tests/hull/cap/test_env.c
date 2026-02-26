/*
 * test_hull_cap_env.c — Tests for shared environment variable capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/env.h"
#include <stdlib.h>
#include <string.h>

UTEST(hl_cap_env, allowed_var)
{
    setenv("HULL_TEST_VAR", "hello", 1);

    const char *allowed[] = { "HULL_TEST_VAR", NULL };
    HlEnvConfig cfg = { .allowed = allowed, .count = 1 };

    const char *val = hl_cap_env_get(&cfg, "HULL_TEST_VAR");
    ASSERT_NE(val, NULL);
    ASSERT_STREQ(val, "hello");

    unsetenv("HULL_TEST_VAR");
}

UTEST(hl_cap_env, denied_var)
{
    setenv("HULL_SECRET", "password", 1);

    const char *allowed[] = { "HULL_TEST_VAR", NULL };
    HlEnvConfig cfg = { .allowed = allowed, .count = 1 };

    /* HULL_SECRET is not in the allowlist */
    const char *val = hl_cap_env_get(&cfg, "HULL_SECRET");
    ASSERT_EQ(val, NULL);

    unsetenv("HULL_SECRET");
}

UTEST(hl_cap_env, nonexistent_var)
{
    const char *allowed[] = { "HULL_NONEXISTENT", NULL };
    HlEnvConfig cfg = { .allowed = allowed, .count = 1 };

    const char *val = hl_cap_env_get(&cfg, "HULL_NONEXISTENT");
    ASSERT_EQ(val, NULL); /* allowed but not set */
}

UTEST(hl_cap_env, null_config)
{
    const char *val = hl_cap_env_get(NULL, "PATH");
    ASSERT_EQ(val, NULL);
}

UTEST(hl_cap_env, null_name)
{
    const char *allowed[] = { "PATH", NULL };
    HlEnvConfig cfg = { .allowed = allowed, .count = 1 };

    const char *val = hl_cap_env_get(&cfg, NULL);
    ASSERT_EQ(val, NULL);
}

UTEST(hl_cap_env, empty_allowlist)
{
    HlEnvConfig cfg = { .allowed = NULL, .count = 0 };

    const char *val = hl_cap_env_get(&cfg, "PATH");
    ASSERT_EQ(val, NULL);
}

UTEST(hl_cap_env, multiple_allowed)
{
    setenv("HULL_A", "aaa", 1);
    setenv("HULL_B", "bbb", 1);

    const char *allowed[] = { "HULL_A", "HULL_B", NULL };
    HlEnvConfig cfg = { .allowed = allowed, .count = 2 };

    const char *a = hl_cap_env_get(&cfg, "HULL_A");
    const char *b = hl_cap_env_get(&cfg, "HULL_B");
    ASSERT_NE(a, NULL);
    ASSERT_NE(b, NULL);
    ASSERT_STREQ(a, "aaa");
    ASSERT_STREQ(b, "bbb");

    unsetenv("HULL_A");
    unsetenv("HULL_B");
}

UTEST_MAIN();
