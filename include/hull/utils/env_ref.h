/*
 * utils/env_ref.h - "$VAR" / "${VAR}" full-value environment reference parser
 *
 * A domain-free string parser shared by the manifest allowlists (databases
 * DSNs + hosts) and the host matcher. Header-only so consumers don't drag any
 * .o into their link.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_UTILS_ENV_REF_H
#define HL_UTILS_ENV_REF_H

#include <stddef.h>

/*
 * Full-value env reference: if @p s is exactly "$NAME" or "${NAME}" (with NAME
 * matching [A-Za-z_][A-Za-z0-9_]*), copy NAME into @p out (bounded by @p outsz)
 * and return 1. Otherwise return 0 (a literal value; a string that merely
 * CONTAINS '$', e.g. a DSN password, is literal). NULL-safe (returns 0). Used
 * for `databases.named` DSNs and the db / http / smtp host allowlists (resolved
 * at open / check time so secrets stay in the environment, not app source).
 */
static inline int hl_env_ref(const char *s, char *out, size_t outsz)
{
    if (!s || !out || outsz == 0 || s[0] != '$')
        return 0;
    const char *name = s + 1;
    int braced = 0;
    if (*name == '{') { braced = 1; name++; }

    /* NAME = [A-Za-z_][A-Za-z0-9_]* */
    const char *p = name;
    if (!(*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')))
        return 0;
    while (*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
           (*p >= '0' && *p <= '9'))
        p++;

    /* The reference must be the WHOLE value: end (unbraced) or "}" then end. */
    if (braced) { if (p[0] != '}' || p[1] != '\0') return 0; }
    else        { if (p[0] != '\0') return 0; }

    size_t nlen = (size_t)(p - name);
    if (nlen == 0 || nlen >= outsz)
        return 0;
    for (size_t i = 0; i < nlen; i++)   /* manual copy: no <string.h> in the header */
        out[i] = name[i];
    out[nlen] = '\0';
    return 1;
}

#endif /* HL_UTILS_ENV_REF_H */
