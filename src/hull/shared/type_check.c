/*
 * type_check.c - the TypeScript compiler lookup and its diagnostic line
 * (include/hull/shared/type_check.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/type_check.h"
#include "hull/shared/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int hl_type_check_configured(const char *app_dir)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/tsconfig.json", app_dir);
    return n > 0 && (size_t)n < sizeof path && access(path, F_OK) == 0;
}

int hl_type_check_find(const char *app_dir, char *out, size_t outsz)
{
    static const char *const names[] = { "tsgo", "tsc" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        int n = snprintf(out, outsz, "%s/node_modules/.bin/%s", app_dir, names[i]);
        if (n > 0 && (size_t)n < outsz && access(out, X_OK) == 0) return 0;
    }
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (hl_host_find_in_path(names[i], out, outsz) == 1) return 0;
    if (outsz) out[0] = '\0';
    return -1;
}

/* Copy [s, s+n) into dst (cut to fit, NUL-terminated). */
static void cut(char *dst, size_t dsz, const char *s, size_t n)
{
    if (n >= dsz) n = dsz - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

int hl_type_check_parse_line(const char *line, size_t n, HlTypeDiag *d)
{
    memset(d, 0, sizeof *d);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) n--;
    if (n == 0) return 0;

    /* FILE(LINE,COL): SEVERITY CODE: MESSAGE. The file is up to the last
     * "(" before the first "): " - a path may hold parentheses itself. */
    const char *end = line + n;
    const char *close = NULL;
    for (const char *p = line; p + 2 < end; p++)
        if (p[0] == ')' && p[1] == ':' && p[2] == ' ') { close = p; break; }
    const char *open = NULL;
    if (close)
        for (const char *p = close; p > line; p--)
            if (p[-1] == '(') { open = p - 1; break; }
    if (open && open > line) {
        char *e1, *e2;
        long ln = strtol(open + 1, &e1, 10);
        long col = (*e1 == ',') ? strtol(e1 + 1, &e2, 10) : -1;
        const char *rest = close + 3;
        const char *sp = memchr(rest, ' ', (size_t)(end - rest));
        const char *colon = sp ? memchr(sp + 1, ':', (size_t)(end - sp - 1)) : NULL;
        if (*e1 == ',' && e2 == close && ln > 0 && col > 0 && sp && colon &&
            colon + 1 < end && colon[1] == ' ') {
            cut(d->file, sizeof d->file, line, (size_t)(open - line));
            d->line = ln;
            d->col = col;
            cut(d->severity, sizeof d->severity, rest, (size_t)(sp - rest));
            cut(d->code, sizeof d->code, sp + 1, (size_t)(colon - sp - 1));
            cut(d->message, sizeof d->message, colon + 2, (size_t)(end - colon - 2));
            d->parsed = 1;
            return 1;
        }
    }
    cut(d->message, sizeof d->message, line, n);
    return 1;
}
