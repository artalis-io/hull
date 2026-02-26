/*
 * parse_size.h — Human-readable size string parser
 *
 * Parses strings like "128m", "512k", "1g" into byte counts.
 * Plain numbers are treated as bytes. Returns -1 on error.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_PARSE_SIZE_H
#define HL_PARSE_SIZE_H

#include <limits.h>
#include <stdlib.h>

static inline long hl_parse_size(const char *s)
{
    char *end;
    long val = strtol(s, &end, 10);
    if (end == s || val < 0)
        return -1;

    switch (*end) {
    case 'g': case 'G': if (val > LONG_MAX / (1024L*1024*1024)) return -1;
                         val *= 1024L * 1024 * 1024; end++; break;
    case 'm': case 'M': if (val > LONG_MAX / (1024L*1024)) return -1;
                         val *= 1024L * 1024; end++; break;
    case 'k': case 'K': if (val > LONG_MAX / 1024L) return -1;
                         val *= 1024L; end++; break;
    case '\0':           break;
    default:             return -1;
    }

    if (*end != '\0')
        return -1;
    return val;
}

#endif /* HL_PARSE_SIZE_H */
