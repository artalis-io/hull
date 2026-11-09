/*
 * json_term_safe.h - write JSON text with the bytes a terminal acts on
 * escaped, as the same JSON value.
 *
 * Agent / MCP output is JSON a human may read in a terminal, and it often
 * carries text from the app tree (a data.db value or column name, a
 * queries.json key, an eval result). A JSON encoder escapes only C0 inside a
 * string, so DEL and the C1 controls (U+0080-U+009F, UTF-8 C2 80-9F) went out
 * raw - a terminal in 8-bit mode reads 0x9B as CSI and 0x9D as OSC. This
 * writer rewrites, in valid JSON text:
 *   - every C0 control but the tab / CR / LF whitespace between tokens, and
 *     DEL, as \u00NN (inside a string an encoder escaped the whitespace, so a
 *     raw one is between tokens; any other raw control can only be in a
 *     string, where its \u escape is the same value);
 *   - a C1 control (C2 80-9F) as \u0080-\u009f, the same value;
 *   - a stray byte 0x80-0x9F that no valid UTF-8 sequence holds as �
 *     (it was not a character; the decoder's replacement).
 * Every other byte is written as it is. Outside a string valid JSON holds
 * only ASCII structural bytes and whitespace, so the output is the same JSON
 * value. Shared by `hull agent` (output_result, inspect), agent eval
 * (audit 35 / 37) and `hull mcp` (audit 44).
 *
 * Header-only (static inline).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_JSON_TERM_SAFE_H
#define HL_SHARED_JSON_TERM_SAFE_H

#include <stddef.h>
#include <stdio.h>

#include "hull/shared/term_escape.h"   /* hl_term_high_unit */

typedef int (*HlJsonSafeWriteFn)(void *ctx, const char *data, size_t len);

/* Write @p json (@p len bytes) through @p write. Returns 0, or the first
 * non-zero value @p write returned. */
static inline int hl_json_write_terminal_safe(HlJsonSafeWriteFn write,
                                              void *ctx, const char *json,
                                              size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t run = 0;
    int rc = 0;
    if (!json) return 0;
    for (size_t i = 0; i < len && rc == 0; ) {
        unsigned char c = (unsigned char)json[i];
        char esc[6] = { '\\', 'u', '0', '0', 0, 0 };
        size_t n = 1;
        if (c >= 0x80) {
            int e;
            n = hl_term_high_unit((const unsigned char *)json + i, len - i, &e);
            if (!e) { i += n; continue; }
            if (n == 2) {                 /* C2 80-9F: U+0080-U+009F */
                unsigned char b = (unsigned char)json[i + 1];
                esc[4] = hex[b >> 4];
                esc[5] = hex[b & 15];
            } else {                      /* stray 0x80-0x9F */
                esc[2] = 'f'; esc[3] = 'f'; esc[4] = 'f'; esc[5] = 'd';
            }
        } else if (c < 0x20 || c == 0x7f) {
            if (c == '\t' || c == '\n' || c == '\r') { i++; continue; }
            esc[4] = hex[c >> 4];
            esc[5] = hex[c & 15];
        } else {
            i++;
            continue;
        }
        if (i > run) rc = write(ctx, json + run, i - run);
        if (rc == 0) rc = write(ctx, esc, sizeof esc);
        i += n;
        run = i;
    }
    if (rc == 0 && len > run) rc = write(ctx, json + run, len - run);
    return rc;
}

static inline int hl_json_safe_fwrite_fn(void *ctx, const char *data,
                                         size_t len)
{
    return fwrite(data, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

/* Write @p json to @p f (see above). */
static inline int hl_json_fwrite_terminal_safe(FILE *f, const char *json,
                                               size_t len)
{
    if (!f) return -1;
    return hl_json_write_terminal_safe(hl_json_safe_fwrite_fn, f, json, len);
}

#endif /* HL_SHARED_JSON_TERM_SAFE_H */
