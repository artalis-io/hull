/*
 * test_parse_size.c — Tests for hl_parse_size()
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/parse_size.h"

/* ── Basic byte values ─────────────────────────────────────────────── */

UTEST(parse_size, plain_bytes)
{
    ASSERT_EQ(hl_parse_size("0"), 0);
    ASSERT_EQ(hl_parse_size("1"), 1);
    ASSERT_EQ(hl_parse_size("1024"), 1024);
    ASSERT_EQ(hl_parse_size("67108864"), 67108864L);
}

/* ── Suffix parsing ────────────────────────────────────────────────── */

UTEST(parse_size, kilobytes)
{
    ASSERT_EQ(hl_parse_size("1k"), 1024L);
    ASSERT_EQ(hl_parse_size("1K"), 1024L);
    ASSERT_EQ(hl_parse_size("512k"), 512L * 1024);
}

UTEST(parse_size, megabytes)
{
    ASSERT_EQ(hl_parse_size("1m"), 1024L * 1024);
    ASSERT_EQ(hl_parse_size("1M"), 1024L * 1024);
    ASSERT_EQ(hl_parse_size("64m"), 64L * 1024 * 1024);
    ASSERT_EQ(hl_parse_size("128M"), 128L * 1024 * 1024);
}

UTEST(parse_size, gigabytes)
{
    ASSERT_EQ(hl_parse_size("1g"), 1024L * 1024 * 1024);
    ASSERT_EQ(hl_parse_size("1G"), 1024L * 1024 * 1024);
    ASSERT_EQ(hl_parse_size("2g"), 2L * 1024 * 1024 * 1024);
}

/* ── Error cases ───────────────────────────────────────────────────── */

UTEST(parse_size, empty_string)
{
    ASSERT_EQ(hl_parse_size(""), -1);
}

UTEST(parse_size, negative)
{
    ASSERT_EQ(hl_parse_size("-1"), -1);
    ASSERT_EQ(hl_parse_size("-64m"), -1);
}

UTEST(parse_size, invalid_suffix)
{
    ASSERT_EQ(hl_parse_size("10x"), -1);
    ASSERT_EQ(hl_parse_size("10mb"), -1);
    ASSERT_EQ(hl_parse_size("10kg"), -1);
}

UTEST(parse_size, no_digits)
{
    ASSERT_EQ(hl_parse_size("m"), -1);
    ASSERT_EQ(hl_parse_size("abc"), -1);
}

UTEST(parse_size, trailing_junk)
{
    ASSERT_EQ(hl_parse_size("64m!"), -1);
    ASSERT_EQ(hl_parse_size("10k "), -1);
}

UTEST_MAIN();
