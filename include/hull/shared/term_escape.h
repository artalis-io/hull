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
 * control (NUL and ESC included) and DEL as \xNN. Valid UTF-8 passes through
 * unchanged, except the C1 controls U+0080-U+009F (bytes C2 80-9F), written
 * as \xc2\xNN, and a stray byte 0x80-0x9F that no valid UTF-8 sequence holds,
 * written as \xNN: a terminal in 8-bit mode reads 0x9B as CSI and 0x9D as
 * OSC, and some read the UTF-8 form of U+009B the same way (audit 42; see
 * hl_term_high_unit). The output holds no control byte but LF / tab, so
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
#include <string.h>

/* Length of the well-formed UTF-8 sequence at @p s (at most @p len bytes)
 * whose first byte is >= 0x80, or 0 when the bytes there are not one (a
 * stray continuation, an overlong or surrogate form, a truncated tail). */
static inline size_t hl_utf8_seq_len(const unsigned char *s, size_t len)
{
    unsigned char c = s[0];
    size_t n;
    unsigned char lo = 0x80, hi = 0xBF;     /* range of the second byte */
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c == 0xE0) { n = 3; lo = 0xA0; }
    else if (c >= 0xE1 && c <= 0xEC) n = 3;
    else if (c == 0xED) { n = 3; hi = 0x9F; }
    else if (c >= 0xEE && c <= 0xEF) n = 3;
    else if (c == 0xF0) { n = 4; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) n = 4;
    else if (c == 0xF4) { n = 4; hi = 0x8F; }
    else return 0;
    if (len < n || s[1] < lo || s[1] > hi) return 0;
    for (size_t i = 2; i < n; i++)
        if (s[i] < 0x80 || s[i] > 0xBF) return 0;
    return n;
}

/* The escapers' rule for a byte >= 0x80 at @p s (audit 42). Returns how many
 * input bytes form one unit, and sets *@p escape when each of them is to be
 * written as \xNN: a C1 control U+0080-U+009F (C2 80-9F, 2 bytes) or a stray
 * 0x80-0x9F that no valid UTF-8 sequence holds (1 byte). Any other valid
 * UTF-8 sequence, or a stray 0xA0-0xFF, is kept as it is. Shared by
 * hl_log_escape_line / hl_log_fputs_escaped (shared/cli_log.c) and
 * hl_cap_wasm_log_escape (cap/wasm_log.c). */
static inline size_t hl_term_high_unit(const unsigned char *s, size_t len,
                                       int *escape)
{
    size_t n = hl_utf8_seq_len(s, len);
    if (n == 2 && s[0] == 0xC2 && s[1] <= 0x9F) { *escape = 1; return 2; }
    if (n) { *escape = 0; return n; }
    *escape = s[0] <= 0x9F;
    return 1;
}

static inline void hl_term_fwrite_escaped(FILE *f, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    if (!f || !s) return;
    size_t run = 0;   /* start of the pending run of bytes written as-is */
    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)s[i];
        size_t n = 1;
        if (c >= 0x80) {
            int esc;
            n = hl_term_high_unit((const unsigned char *)s + i, len - i, &esc);
            if (!esc) { i += n; continue; }
        } else if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7f)) {
            i++;
            continue;
        }
        if (i > run) fwrite(s + run, 1, i - run, f);
        if (c == '\r') {
            fputs("\\r", f);
        } else {
            for (size_t k = 0; k < n; k++) {
                unsigned char b = (unsigned char)s[i + k];
                char e[4] = { '\\', 'x', hex[b >> 4], hex[b & 15] };
                fwrite(e, 1, sizeof e, f);
            }
        }
        i += n;
        run = i;
    }
    if (len > run) fwrite(s + run, 1, len - run, f);
}

/* Write @p s to @p f with every control byte escaped, LF and CR included
 * (as hl_log_escape_line escapes a log record: CR / LF as \r / \n, tab kept,
 * every other C0 control and DEL as \xNN, a C1 control or stray 0x80-0x9F as
 * \xNN per byte). For one diagnostic LINE that
 * quotes app, request or cloned-tree text: written raw, a newline in it forged
 * a second line and an ESC / OSC sequence rewrote the terminal (audit 34). */
static inline void hl_term_fputs_line_escaped(FILE *f, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    if (!f) return;
    const char *end = s ? s + strlen(s) : NULL;
    for (const char *p = s ? s : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x80) {
            int esc;
            size_t n = hl_term_high_unit((const unsigned char *)p,
                                         (size_t)(end - p), &esc);
            for (size_t k = 0; k < n; k++) {
                unsigned char b = (unsigned char)p[k];
                if (esc) {
                    fputc('\\', f);
                    fputc('x', f);
                    fputc(hex[b >> 4], f);
                    fputc(hex[b & 15], f);
                } else {
                    fputc(b, f);
                }
            }
            p += n - 1;
            continue;
        }
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
