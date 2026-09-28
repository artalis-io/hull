/*
 * test_url.c - tests for the utils/url leaf: strict and form decoding, the
 * malformed-escape rule, in-place use and the capacity contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "../../src/hull/utils/url.h"

#include <string.h>

static int decodes_to(const char *s, unsigned flags, const char *want)
{
    char out[64];
    long n = hl_url_decode(s, strlen(s), out, sizeof out, flags);
    return n == (long)strlen(want) && memcmp(out, want, (size_t)n) == 0
        && out[n] == '\0';
}

static int fails(const char *s, unsigned flags)
{
    char out[64];
    return hl_url_decode(s, strlen(s), out, sizeof out, flags) == -1;
}

UTEST(url, strict_decodes_escapes_either_case)
{
    EXPECT_TRUE(decodes_to("", 0, ""));
    EXPECT_TRUE(decodes_to("plain", 0, "plain"));
    EXPECT_TRUE(decodes_to("p%40ss%2fw%2Fd", 0, "p@ss/w/d"));
    EXPECT_TRUE(decodes_to("a+b", 0, "a+b"));         /* '+' is itself */
    char nul[8];
    EXPECT_EQ(hl_url_decode("%00x", 4, nul, sizeof nul, 0), 2);  /* a NUL is a byte */
    EXPECT_EQ(nul[0], '\0');
    EXPECT_EQ(nul[1], 'x');
}

UTEST(url, strict_refuses_a_malformed_escape)
{
    EXPECT_TRUE(fails("%", 0));
    EXPECT_TRUE(fails("a%4", 0));
    EXPECT_TRUE(fails("%zz", 0));
    EXPECT_TRUE(fails("%4g", 0));
    EXPECT_TRUE(fails("ok%41%", 0));
}

UTEST(url, form_reads_plus_as_space)
{
    EXPECT_TRUE(decodes_to("a+b%2B", HL_URL_FORM, "a b+"));
    EXPECT_TRUE(decodes_to("h%C3%A9", HL_URL_FORM, "h\xc3\xa9"));
}

UTEST(url, form_keeps_a_malformed_value_as_written)
{
    /* Not half-decoded: the valid %41 is left alone too, and '+' is still
     * a space. The same rule as hull.encoding.url.decode. */
    EXPECT_TRUE(decodes_to("%zz%41", HL_URL_FORM, "%zz%41"));
    EXPECT_TRUE(decodes_to("x+%g1", HL_URL_FORM, "x %g1"));
    EXPECT_TRUE(decodes_to("100%", HL_URL_FORM, "100%"));
}

UTEST(url, works_in_place)
{
    char buf[] = "a%20b+c";
    long n = hl_url_decode(buf, strlen(buf), buf, sizeof buf, HL_URL_FORM);
    EXPECT_EQ(n, 5);
    EXPECT_STREQ(buf, "a b c");
}

UTEST(url, capacity_counts_the_nul_and_a_failure_writes_nothing)
{
    char out[4];
    memset(out, 'X', sizeof out);
    EXPECT_EQ(hl_url_decode("abcd", 4, out, sizeof out, 0), -1);
    EXPECT_EQ(out[0], 'X');
    EXPECT_EQ(hl_url_decode("abc", 3, out, sizeof out, 0), 3);
    EXPECT_EQ(hl_url_decode("%41%42%43%44", 12, out, sizeof out, 0), -1);
    EXPECT_EQ(hl_url_decode("x", 1, NULL, 4, 0), -1);
    EXPECT_EQ(hl_url_decode(NULL, 1, out, sizeof out, 0), -1);
    EXPECT_EQ(hl_url_decode(NULL, 0, out, sizeof out, 0), 0);
}

UTEST_MAIN();
