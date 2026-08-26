/*
 * test_tui_width.c - Tests for the embedded Unicode width table.
 *
 * Validates that the table generated from `vendor/unicode/` matches
 * expected widths for a sample of well-known codepoints across:
 *   - ASCII (control + printable)
 *   - Latin-1 supplement (soft hyphen edge case)
 *   - Combining marks (Mn / Me)
 *   - Format characters (ZWJ, ZWNJ, soft hyphen, BOM)
 *   - East Asian Wide (CJK Unified, Hiragana, Hangul)
 *   - Fullwidth forms
 *   - Emoji
 *   - Supplementary plane edge cases
 *
 * Also validates the UTF-8 decoder and the string-width helper.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/tui_width.h"

#include <string.h>

/* ── codepoint width ─────────────────────────────────────────────── */

UTEST(tui_width, ascii_control_is_zero)
{
    ASSERT_EQ(hl_tui_cp_width(0x00), 0);    /* NUL */
    ASSERT_EQ(hl_tui_cp_width(0x07), 0);    /* BEL */
    ASSERT_EQ(hl_tui_cp_width(0x09), 0);    /* TAB */
    ASSERT_EQ(hl_tui_cp_width(0x1B), 0);    /* ESC */
    ASSERT_EQ(hl_tui_cp_width(0x1F), 0);    /* US */
}

UTEST(tui_width, ascii_printable_is_one)
{
    ASSERT_EQ(hl_tui_cp_width(' '),  1);
    ASSERT_EQ(hl_tui_cp_width('A'),  1);
    ASSERT_EQ(hl_tui_cp_width('~'),  1);
    ASSERT_EQ(hl_tui_cp_width('0'),  1);
}

UTEST(tui_width, del_and_c1_control_are_zero)
{
    ASSERT_EQ(hl_tui_cp_width(0x7F), 0);    /* DEL */
    ASSERT_EQ(hl_tui_cp_width(0x80), 0);    /* C1 begin */
    ASSERT_EQ(hl_tui_cp_width(0x9F), 0);    /* C1 end */
}

UTEST(tui_width, soft_hyphen_is_zero)
{
    /* U+00AD: rendered invisibly until line-break point. We treat it
     * as zero-width so the shadow buffer doesn't reserve a cell. */
    ASSERT_EQ(hl_tui_cp_width(0x00AD), 0);
}

UTEST(tui_width, combining_marks_are_zero)
{
    ASSERT_EQ(hl_tui_cp_width(0x0300), 0);  /* combining grave accent */
    ASSERT_EQ(hl_tui_cp_width(0x0301), 0);  /* combining acute accent */
    ASSERT_EQ(hl_tui_cp_width(0x036F), 0);  /* end of combining range */
}

UTEST(tui_width, zwj_and_zwnj_are_zero)
{
    ASSERT_EQ(hl_tui_cp_width(0x200B), 0);  /* zero-width space */
    ASSERT_EQ(hl_tui_cp_width(0x200C), 0);  /* ZWNJ */
    ASSERT_EQ(hl_tui_cp_width(0x200D), 0);  /* ZWJ */
    ASSERT_EQ(hl_tui_cp_width(0xFEFF), 0);  /* BOM */
}

UTEST(tui_width, variation_selectors_are_zero)
{
    ASSERT_EQ(hl_tui_cp_width(0xFE00), 0);  /* VS1 */
    ASSERT_EQ(hl_tui_cp_width(0xFE0F), 0);  /* VS16 (emoji presentation) */
    ASSERT_EQ(hl_tui_cp_width(0xE0100), 0); /* VS17 */
}

UTEST(tui_width, cjk_unified_is_wide)
{
    ASSERT_EQ(hl_tui_cp_width(0x4E00), 2);  /* 一 (one) */
    ASSERT_EQ(hl_tui_cp_width(0x4E2D), 2);  /* 中 (middle) */
    ASSERT_EQ(hl_tui_cp_width(0x9FFF), 2);  /* end of CJK Unified */
}

UTEST(tui_width, hiragana_katakana_are_wide)
{
    ASSERT_EQ(hl_tui_cp_width(0x3042), 2);  /* あ */
    ASSERT_EQ(hl_tui_cp_width(0x30A2), 2);  /* ア */
}

UTEST(tui_width, hangul_syllables_are_wide)
{
    ASSERT_EQ(hl_tui_cp_width(0xAC00), 2);  /* 가 */
    ASSERT_EQ(hl_tui_cp_width(0xD7A3), 2);  /* 힣 */
}

UTEST(tui_width, fullwidth_ascii_is_wide)
{
    ASSERT_EQ(hl_tui_cp_width(0xFF21), 2);  /* fullwidth A */
    ASSERT_EQ(hl_tui_cp_width(0xFF5E), 2);  /* fullwidth tilde */
}

UTEST(tui_width, emoji_are_wide)
{
    ASSERT_EQ(hl_tui_cp_width(0x1F600), 2); /* 😀 */
    ASSERT_EQ(hl_tui_cp_width(0x1F44D), 2); /* 👍 */
    ASSERT_EQ(hl_tui_cp_width(0x1F4A9), 2); /* 💩 */
}

UTEST(tui_width, latin_supplement_is_narrow)
{
    ASSERT_EQ(hl_tui_cp_width(0x00E9), 1);  /* é */
    ASSERT_EQ(hl_tui_cp_width(0x00FC), 1);  /* ü */
}

UTEST(tui_width, out_of_range_returns_default)
{
    /* Above max Unicode codepoint should default to 1 (caller error). */
    ASSERT_EQ(hl_tui_cp_width(0x110000), 1);
    ASSERT_EQ(hl_tui_cp_width(0xFFFFFFFFu), 1);
}

UTEST(tui_width, supplementary_private_use)
{
    /* SPUA-A starts at U+F0000. Width 1 (best guess). */
    ASSERT_EQ(hl_tui_cp_width(0xF0000), 1);
}

/* ── UTF-8 decode ───────────────────────────────────────────────── */

UTEST(tui_utf8, decode_ascii)
{
    uint32_t cp = 0;
    int n = hl_tui_utf8_decode("A", 1, &cp);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(cp, (uint32_t)'A');
}

UTEST(tui_utf8, decode_two_byte)
{
    uint32_t cp = 0;
    int n = hl_tui_utf8_decode("\xC3\xA9", 2, &cp);  /* é */
    ASSERT_EQ(n, 2);
    ASSERT_EQ(cp, 0x00E9u);
}

UTEST(tui_utf8, decode_three_byte)
{
    uint32_t cp = 0;
    int n = hl_tui_utf8_decode("\xE4\xB8\xAD", 3, &cp);  /* 中 */
    ASSERT_EQ(n, 3);
    ASSERT_EQ(cp, 0x4E2Du);
}

UTEST(tui_utf8, decode_four_byte)
{
    uint32_t cp = 0;
    int n = hl_tui_utf8_decode("\xF0\x9F\x98\x80", 4, &cp);  /* 😀 */
    ASSERT_EQ(n, 4);
    ASSERT_EQ(cp, 0x1F600u);
}

UTEST(tui_utf8, decode_empty)
{
    uint32_t cp = 0;
    int n = hl_tui_utf8_decode("", 0, &cp);
    ASSERT_EQ(n, 0);
    ASSERT_EQ(cp, 0xFFFDu);
}

UTEST(tui_utf8, decode_truncated_sequence)
{
    uint32_t cp = 0;
    int n = hl_tui_utf8_decode("\xE4\xB8", 2, &cp);  /* truncated 3-byte */
    ASSERT_EQ(n, 1);
    ASSERT_EQ(cp, 0xFFFDu);
}

UTEST(tui_utf8, decode_invalid_continuation)
{
    uint32_t cp = 0;
    int n = hl_tui_utf8_decode("\xE4\x41\xAD", 3, &cp);  /* bad cont byte */
    ASSERT_EQ(n, 1);
    ASSERT_EQ(cp, 0xFFFDu);
}

UTEST(tui_utf8, decode_overlong_rejected)
{
    uint32_t cp = 0;
    /* 'A' encoded as 2 bytes (overlong) */
    int n = hl_tui_utf8_decode("\xC1\x81", 2, &cp);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(cp, 0xFFFDu);
}

UTEST(tui_utf8, decode_surrogate_rejected)
{
    uint32_t cp = 0;
    /* U+D800 as 3-byte UTF-8 = ED A0 80 - must reject */
    int n = hl_tui_utf8_decode("\xED\xA0\x80", 3, &cp);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(cp, 0xFFFDu);
}

/* ── string width ───────────────────────────────────────────────── */

UTEST(tui_str_width, empty_is_zero)
{
    ASSERT_EQ(hl_tui_str_width(NULL, 0), 0);
    ASSERT_EQ(hl_tui_str_width("", 0), 0);
}

UTEST(tui_str_width, ascii_one_per_char)
{
    ASSERT_EQ(hl_tui_str_width("hello", 5), 5);
}

UTEST(tui_str_width, cjk_two_per_char)
{
    /* "中文" = U+4E2D U+6587 - two CJK chars, total 4 cells */
    const char *s = "\xE4\xB8\xAD\xE6\x96\x87";
    ASSERT_EQ(hl_tui_str_width(s, 6), 4);
}

UTEST(tui_str_width, combining_marks_dont_count)
{
    /* 'e' + U+0301 (combining acute) - visual "é", 1 cell */
    const char *s = "e\xCC\x81";
    ASSERT_EQ(hl_tui_str_width(s, 3), 1);
}

UTEST(tui_str_width, mixed_ascii_and_emoji)
{
    /* "hi 👍" = 'h' 'i' ' ' 😀 = 1+1+1+2 = 5 */
    const char *s = "hi \xF0\x9F\x91\x8D";
    ASSERT_EQ(hl_tui_str_width(s, 7), 5);
}

UTEST_MAIN();
