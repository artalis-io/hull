/*
 * utils/url.c - URL percent-decoding. See utils/url.h for the contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "url.h"
#include "hex.h"

#include <limits.h>

/* Whether every '%' in [src, src+len) begins a two-hex-digit escape. */
static int escapes_well_formed(const char *src, size_t len)
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

long hl_url_decode(const char *src, size_t len, char *dst, size_t cap,
                   unsigned flags)
{
    if (!dst || cap == 0 || (len > 0 && !src))
        return -1;
    if (len >= (size_t)LONG_MAX)
        return -1;

    int form = (flags & HL_URL_FORM) != 0;
    int decode = escapes_well_formed(src, len);
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
