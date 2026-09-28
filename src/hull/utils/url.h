/*
 * utils/url.h - URL percent-decoding (RFC 3986), the one home for it in C.
 *
 * Header-only, like parse_size / buffer / limits: every consumer compiles it
 * in. Its callers are the Lua and JS request-query parsers (in the composed
 * HTTP bindings) and the Postgres, MySQL and Valkey DSN parsers (composed
 * features), none of which the base links on its own - so a function in the
 * base would depend on the composed archive pulling it back out of the
 * platform library at app-link time. Inlined, there is no symbol to resolve.
 * Script code has its own single home, hull.encoding.url, and the two follow
 * the same rules.
 *
 * Deliberately NOT under include/hull/: an internal helper, not public API.
 * Consumers include it by relative path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_UTILS_URL_H
#define HULL_UTILS_URL_H

#include "hex.h"

#include <limits.h>
#include <stddef.h>

/*
 * Form mode (application/x-www-form-urlencoded, query strings): '+' is a
 * space, and a value with a malformed escape - a '%' not followed by two hex
 * digits - is kept as written (with '+' still read as a space) rather than
 * half-decoded. A query string is often not ours to reject.
 *
 * Without it (strict, for DSNs): '+' is itself, and a malformed escape fails.
 */
#define HL_URL_FORM 0x1u

/* Whether every '%' in [src, src+len) begins a two-hex-digit escape. */
static inline int hl_url__escapes_well_formed(const char *src, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (src[i] != '%') continue;
        if (i + 2 >= len
            || hl_hex_digit((unsigned char)src[i + 1]) < 0
            || hl_hex_digit((unsigned char)src[i + 2]) < 0)
            return 0;
        i += 2;
    }
    return 1;
}

/*
 * Decode `len` bytes at `src` into `dst`, NUL-terminated.
 *
 *   - `dst` may be `src`: decoding never lengthens the value, so it works in
 *     place.
 *   - `cap` must hold the decoded bytes plus the NUL; a value that does not
 *     fit fails. On failure `dst` is not written, so an in-place caller keeps
 *     its input intact.
 *   - `src` may be NULL only when `len == 0`.
 *
 * Returns the decoded length (excluding the NUL), or -1.
 */
static inline long hl_url_decode(const char *src, size_t len, char *dst,
                                 size_t cap, unsigned flags)
{
    if (!dst || cap == 0 || (len > 0 && !src))
        return -1;
    if (len >= (size_t)LONG_MAX)
        return -1;

    int form = (flags & HL_URL_FORM) != 0;
    int decode = hl_url__escapes_well_formed(src, len);
    if (!decode && !form)
        return -1;

    /* Size first, so a value that does not fit leaves dst untouched (an
     * in-place caller keeps its input). */
    size_t out = len;
    if (decode) {
        for (size_t i = 0; i < len; i++)
            if (src[i] == '%') { out -= 2; i += 2; }
    }
    if (out >= cap)
        return -1;

    /* Front to back is safe in place: the write index never passes the
     * read index. */
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (decode && c == '%') {
            c = (char)((hl_hex_digit((unsigned char)src[i + 1]) << 4)
                       | hl_hex_digit((unsigned char)src[i + 2]));
            i += 2;
        } else if (form && c == '+') {
            c = ' ';
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
    return (long)o;
}

#endif /* HULL_UTILS_URL_H */
