/*
 * term_escape.h - write text to a terminal stream with its control bytes
 * made visible.
 *
 * The tool VM's writers (print, tool.stderr, tool.stdout) print text that
 * often carries something from the tree being built, verified or inspected:
 * a file name, a manifest string, a package.sig field. Written raw, an ESC /
 * OSC sequence in it rewrote the terminal, set its title or (OSC 52) the
 * clipboard (audits 32, 33). So they escape by default, with the rules
 * hl_log_escape_line applies to log records, except that LF and tab stay as
 * they are: the plugins' own output is multi-line and tab-separated. CR is
 * written as \r (a bare CR overwrites the line it ends), every other C0
 * control (NUL and ESC included) and DEL as \xNN. Bytes >= 0x80 pass through,
 * so UTF-8 is unchanged. The output holds no control byte but LF / tab, so
 * escaping text a second time - tool.escape's output, which is plain ASCII
 * with backslashes - changes nothing.
 *
 * Header-only (static inline) so the Lua runtime and the tool bindings share
 * it without a link-time dependency.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_TERM_ESCAPE_H
#define HL_SHARED_TERM_ESCAPE_H

#include <stddef.h>
#include <stdio.h>

static inline void hl_term_fwrite_escaped(FILE *f, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    if (!f || !s) return;
    size_t run = 0;   /* start of the pending run of bytes written as-is */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7f))
            continue;
        if (i > run) fwrite(s + run, 1, i - run, f);
        if (c == '\r') {
            fputs("\\r", f);
        } else {
            char e[4] = { '\\', 'x', hex[c >> 4], hex[c & 15] };
            fwrite(e, 1, sizeof e, f);
        }
        run = i + 1;
    }
    if (len > run) fwrite(s + run, 1, len - run, f);
}

#endif /* HL_SHARED_TERM_ESCAPE_H */
