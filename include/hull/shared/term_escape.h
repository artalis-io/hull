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

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

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

/* Write @p s to @p f with every control byte escaped, LF and CR included
 * (as hl_log_escape_line escapes a log record: CR / LF as \r / \n, tab kept,
 * every other C0 control and DEL as \xNN). For one diagnostic LINE that
 * quotes app, request or cloned-tree text: written raw, a newline in it forged
 * a second line and an ESC / OSC sequence rewrote the terminal (audit 34). */
static inline void hl_term_fputs_line_escaped(FILE *f, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    if (!f) return;
    for (const char *p = s ? s : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r') {
            fputc('\\', f);
            fputc(c == '\n' ? 'n' : 'r', f);
        } else if ((c < 0x20 && c != '\t') || c == 0x7f) {
            fputc('\\', f);
            fputc('x', f);
            fputc(hex[c >> 4], f);
            fputc(hex[c & 15], f);
        } else {
            fputc(c, f);
        }
    }
}

/* printf to stderr as ONE escaped line: the formatted text is written through
 * hl_term_fputs_line_escaped and a newline appended (the format carries no
 * trailing \n). For the diagnostics Hull prints outside the log - app.main's
 * error / rejection / return value, a handler error, an app-context load
 * error - whose arguments are app, request or cloned-tree text (audit 34). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
static inline void hl_eprintf_line(const char *fmt, ...)
{
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof small) {
        hl_term_fputs_line_escaped(stderr, small);
    } else {
        char *big = (char *)malloc((size_t)n + 1);
        if (!big) {
            hl_term_fputs_line_escaped(stderr, small);   /* truncated */
        } else {
            va_start(ap, fmt);
            vsnprintf(big, (size_t)n + 1, fmt, ap);
            va_end(ap);
            hl_term_fputs_line_escaped(stderr, big);
            free(big);
        }
    }
    fputc('\n', stderr);
}

#endif /* HL_SHARED_TERM_ESCAPE_H */
