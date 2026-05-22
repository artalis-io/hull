/*
 * include/hull/cap/tui_width.h — TUI cell-width lookup.
 *
 * Returns the number of terminal cells a Unicode codepoint occupies.
 * Backed by an embedded table generated from Unicode 16.0.0 data
 * (vendor/unicode/eaw.h); identical behavior across platforms,
 * independent of host wcwidth(3).
 *
 *   0  zero-width (combining marks, format, control, surrogates,
 *      soft hyphen)
 *   1  normal printable (default for unmapped codepoints)
 *   2  East Asian Wide or Fullwidth
 *
 * Refresh the underlying table via `make fetch-unicode`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TUI_WIDTH_H
#define HL_CAP_TUI_WIDTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return cell width for a single codepoint. Always returns 0, 1, or 2. */
int hl_tui_cp_width(uint32_t cp);

/* Convenience: walk a UTF-8 string and return the total display
 * width in cells. Invalid UTF-8 sequences contribute width 0 (the
 * byte is skipped). Combining marks attach to the preceding
 * character — they don't add to width but are accounted for in the
 * advance. */
int hl_tui_str_width(const char *s, size_t len);

/* Decode one UTF-8 codepoint starting at `s`. Returns the number of
 * bytes consumed (1–4) and stores the codepoint in *out_cp. Returns
 * 0 if the buffer is empty or the sequence is invalid (in which
 * case *out_cp is set to 0xFFFD, the replacement character). */
int hl_tui_utf8_decode(const char *s, size_t len, uint32_t *out_cp);

#ifdef __cplusplus
}
#endif

#endif /* HL_CAP_TUI_WIDTH_H */
