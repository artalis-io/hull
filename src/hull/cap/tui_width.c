/*
 * src/hull/cap/tui_width.c - TUI cell-width lookup over the
 * generated Unicode width table.
 *
 * The table (vendor/unicode/eaw.h) is a sorted array of
 * (lo, hi, width) ranges. Width 1 is the default and is NOT stored
 * - any codepoint not covered by an explicit range returns 1.
 *
 * Binary search; O(log n) per lookup, ~10 comparisons for the
 * ~900-range table.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/tui_width.h"

#include <stddef.h>
#include <stdint.h>

/* The generated table lives in vendor/unicode/. The Makefile's
 * include path covers vendor/, so this resolves at compile time. */
#include "unicode/eaw.h"

int hl_tui_cp_width(uint32_t cp)
{
    if (cp > 0x10FFFF) return 1;

    size_t lo = 0;
    size_t hi = hl_eaw_ranges_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const HlEawRange *r = &hl_eaw_ranges[mid];
        if (cp < r->lo) {
            hi = mid;
        } else if (cp > r->hi) {
            lo = mid + 1;
        } else {
            return r->width;
        }
    }
    return 1; /* default */
}

int hl_tui_utf8_decode(const char *s, size_t len, uint32_t *out_cp)
{
    if (len == 0 || !s) { if (out_cp) *out_cp = 0xFFFD; return 0; }

    const unsigned char *p = (const unsigned char *)s;
    unsigned char c0 = p[0];

    if (c0 < 0x80) {
        if (out_cp) *out_cp = c0;
        return 1;
    }

    int trailing;
    uint32_t cp;
    uint32_t min_cp;
    if ((c0 & 0xE0) == 0xC0) {
        trailing = 1; cp = c0 & 0x1F; min_cp = 0x80;
    } else if ((c0 & 0xF0) == 0xE0) {
        trailing = 2; cp = c0 & 0x0F; min_cp = 0x800;
    } else if ((c0 & 0xF8) == 0xF0) {
        trailing = 3; cp = c0 & 0x07; min_cp = 0x10000;
    } else {
        if (out_cp) *out_cp = 0xFFFD;
        return 1; /* skip the bad lead byte */
    }

    if (len < (size_t)(trailing + 1)) {
        if (out_cp) *out_cp = 0xFFFD;
        return 1;
    }

    for (int i = 1; i <= trailing; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            if (out_cp) *out_cp = 0xFFFD;
            return 1; /* skip just the bad lead, let caller resync */
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }

    /* Reject overlong encodings and surrogates. */
    if (cp < min_cp || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        if (out_cp) *out_cp = 0xFFFD;
        return trailing + 1;
    }

    if (out_cp) *out_cp = cp;
    return trailing + 1;
}

int hl_tui_str_width(const char *s, size_t len)
{
    if (!s || len == 0) return 0;
    int total = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int n = hl_tui_utf8_decode(s + i, len - i, &cp);
        if (n <= 0) break;
        if (cp != 0xFFFD) {
            int w = hl_tui_cp_width(cp);
            if (w > 0) total += w;
        }
        i += (size_t)n;
    }
    return total;
}
