/*
 * test_path_normalize.c — Tests for hl_path_normalize.
 *
 * The function is exercised transitively by both runtime module
 * loaders (Lua require, JS import), but covering the contract
 * directly hardens against regressions in either consumer's path-
 * building logic.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/utils/path_normalize.h"

#include <stdio.h>
#include <string.h>

/* utest's ASSERT_* macros reference a local `utest_result` that only
 * exists inside UTEST blocks, so the helpers below are macros — they
 * splice the assertions into the caller's scope where utest_result
 * is in scope. */
#define ASSERT_NORM_OK(input, expected) do { \
    char buf__[256]; \
    snprintf(buf__, sizeof(buf__), "%s", (input)); \
    int rc__ = hl_path_normalize(buf__); \
    ASSERT_EQ(rc__, 0); \
    ASSERT_STREQ(buf__, (expected)); \
} while (0)

#define ASSERT_NORM_FAIL(input) do { \
    char buf__[256]; \
    snprintf(buf__, sizeof(buf__), "%s", (input)); \
    ASSERT_EQ(hl_path_normalize(buf__), -1); \
} while (0)

UTEST(path_normalize, null_input_fails)
{
    ASSERT_EQ(hl_path_normalize(NULL), -1);
}

UTEST(path_normalize, empty_string_is_noop)
{
    ASSERT_NORM_OK("", "");
}

UTEST(path_normalize, plain_path_unchanged)
{
    ASSERT_NORM_OK("models/user", "models/user");
}

UTEST(path_normalize, dot_segment_collapsed)
{
    ASSERT_NORM_OK("a/./b", "a/b");
    ASSERT_NORM_OK("./foo", "foo");
    ASSERT_NORM_OK("foo/.", "foo");
}

UTEST(path_normalize, dotdot_collapsed)
{
    ASSERT_NORM_OK("routes/../models/user", "models/user");
    ASSERT_NORM_OK("a/b/../c", "a/c");
}

UTEST(path_normalize, mixed_dot_and_dotdot)
{
    ASSERT_NORM_OK("./a/./../b", "b");
    ASSERT_NORM_OK("a/b/./../../c", "c");
}

UTEST(path_normalize, multiple_consecutive_slashes_collapsed)
{
    ASSERT_NORM_OK("a//b", "a/b");
    ASSERT_NORM_OK("a///b", "a/b");
}

UTEST(path_normalize, trailing_slash_stripped)
{
    ASSERT_NORM_OK("a/b/", "a/b");
}

UTEST(path_normalize, escape_past_root_rejected)
{
    /* The classic traversal attempt — must be refused. */
    ASSERT_NORM_FAIL("..");
    ASSERT_NORM_FAIL("../etc/passwd");
    ASSERT_NORM_FAIL("a/../..");
    ASSERT_NORM_FAIL("a/../../etc/passwd");
}

UTEST(path_normalize, absolute_paths_preserved)
{
    ASSERT_NORM_OK("/foo/bar", "/foo/bar");
    ASSERT_NORM_OK("/foo/./bar", "/foo/bar");
    ASSERT_NORM_OK("/foo/../bar", "/bar");
}

UTEST(path_normalize, absolute_escape_past_root_rejected)
{
    /* Even absolute paths can't pop above their starting root. */
    ASSERT_NORM_FAIL("/..");
    ASSERT_NORM_FAIL("/foo/../..");
}

UTEST(path_normalize, real_cross_directory_import_case)
{
    /* The case the JS loader regression was about: routes/users.js
     * doing `import "./../models/user.js"` resolves to
     * `routes/./../models/user.js` after the loader joins the parent
     * dir with the relative path; normalize should collapse to
     * `models/user.js`. */
    ASSERT_NORM_OK("routes/./../models/user.js", "models/user.js");
}

UTEST(path_normalize, depth_limit_segments_128)
{
    /* Up to 128 segments allowed; the 129th should be rejected. */
    char ok_buf[1024] = {0};
    char *p = ok_buf;
    for (int i = 0; i < 128; i++) {
        if (i > 0) { *p++ = '/'; }
        *p++ = 'a';
    }
    *p = '\0';
    ASSERT_EQ(hl_path_normalize(ok_buf), 0);

    /* 129 segments — should fail at the depth check. */
    char overflow_buf[2048] = {0};
    p = overflow_buf;
    for (int i = 0; i < 129; i++) {
        if (i > 0) { *p++ = '/'; }
        *p++ = 'a';
    }
    *p = '\0';
    ASSERT_EQ(hl_path_normalize(overflow_buf), -1);
}

UTEST_MAIN();
