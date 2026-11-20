/*
 * test_type_check.c - the TypeScript diagnostic line (shared/type_check.c):
 * what `hull dev` reads from the compiler it runs, and nothing more.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/shared/type_check.h"

#include <string.h>

static int parse(const char *s, HlTypeDiag *d)
{
    return hl_type_check_parse_line(s, strlen(s), d);
}

UTEST(type_check, a_diagnostic_line)
{
    HlTypeDiag d;
    ASSERT_EQ(parse("lib/a.ts(12,5): error TS2322: Type 'string' is not assignable to type 'number'.", &d), 1);
    ASSERT_EQ(d.parsed, 1);
    ASSERT_STREQ(d.file, "lib/a.ts");
    ASSERT_EQ(d.line, 12);
    ASSERT_EQ(d.col, 5);
    ASSERT_STREQ(d.severity, "error");
    ASSERT_STREQ(d.code, "TS2322");
    ASSERT_STREQ(d.message, "Type 'string' is not assignable to type 'number'.");
}

UTEST(type_check, a_path_with_parentheses_and_a_message_with_colons)
{
    HlTypeDiag d;
    ASSERT_EQ(parse("src/(group)/x.ts(3,1): error TS2304: Cannot find name 'a': see b(1,2): c", &d), 1);
    ASSERT_EQ(d.parsed, 1);
    ASSERT_STREQ(d.file, "src/(group)/x.ts");
    ASSERT_EQ(d.line, 3);
    ASSERT_STREQ(d.message, "Cannot find name 'a': see b(1,2): c");
}

UTEST(type_check, a_crlf_line_and_a_warning)
{
    HlTypeDiag d;
    ASSERT_EQ(parse("app.ts(1,2): warning TS6133: 'x' is declared but never used.\r", &d), 1);
    ASSERT_EQ(d.parsed, 1);
    ASSERT_STREQ(d.severity, "warning");
    ASSERT_STREQ(d.message, "'x' is declared but never used.");
}

/* Any other shape is kept whole as text: never dropped, never guessed at. */
UTEST(type_check, other_lines_are_kept_as_text)
{
    HlTypeDiag d;
    const char *const lines[] = {
        "error TS5058: The specified path does not exist: 'x'.",
        "app.ts(0,1): error TS1: zero line",
        "app.ts(a,b): error TS1: not numbers",
        "app.ts(1,2): errorTS1 no space",
        "Found 2 errors in the same file.",
    };
    for (size_t i = 0; i < sizeof lines / sizeof *lines; i++) {
        ASSERT_EQ(parse(lines[i], &d), 1);
        ASSERT_EQ(d.parsed, 0);
        ASSERT_STREQ(d.message, lines[i]);
        ASSERT_STREQ(d.file, "");
    }
}

UTEST(type_check, an_empty_line_is_nothing)
{
    HlTypeDiag d;
    ASSERT_EQ(parse("", &d), 0);
    ASSERT_EQ(parse("  \r", &d), 0);
}

UTEST(type_check, long_fields_are_cut_not_overrun)
{
    char line[4096];
    memset(line, 'a', 2000);
    memcpy(line + 2000, ".ts(1,1): error TS1: ", 21);
    memset(line + 2021, 'm', 2000);
    line[4021] = '\0';
    HlTypeDiag d;
    ASSERT_EQ(parse(line, &d), 1);
    ASSERT_EQ(d.parsed, 1);
    ASSERT_EQ(strlen(d.file), sizeof d.file - 1);
    ASSERT_EQ(strlen(d.message), sizeof d.message - 1);
}

UTEST_MAIN();
