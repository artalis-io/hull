/*
 * utils/base64.c - base64 (RFC 4648) over byte buffers. See utils/base64.h for
 * the bounded contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "base64.h"

#include <limits.h>

static const char STD[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t hl_base64_encoded_len(size_t in_len, unsigned flags)
{
    if (in_len > (SIZE_MAX - 4) / 4 * 3)
        return 0;
    size_t groups = in_len / 3, rem = in_len % 3;
    size_t n = groups * 4;
    if (rem)
        n += (flags & HL_BASE64_NOPAD) ? rem + 1 : 4;
    return n;
}

int hl_base64_encode(const void *in, size_t in_len, char *out, size_t out_cap,
                     unsigned flags)
{
    if (!out || out_cap == 0)
        return -1;
    out[0] = '\0';                          /* fail-closed default */
    if (in_len > 0 && !in)
        return -1;
    size_t need = hl_base64_encoded_len(in_len, flags);
    if ((need == 0 && in_len > 0) || out_cap < need + 1 || need > (size_t)INT_MAX)
        return -1;

    const char *tab = (flags & HL_BASE64_URL) ? URL : STD;
    const uint8_t *p = (const uint8_t *)in;
    size_t i = 0, o = 0;
    for (; i + 3 <= in_len; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = tab[v & 63];
    }
    size_t rem = in_len - i;
    if (rem) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (rem == 2)
            v |= (uint32_t)p[i + 1] << 8;
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        if (rem == 2)
            out[o++] = tab[(v >> 6) & 63];
        if (!(flags & HL_BASE64_NOPAD)) {
            out[o++] = '=';
            if (rem == 1)
                out[o++] = '=';
        }
    }
    out[o] = '\0';
    return (int)o;
}

static int value_of(unsigned char c, unsigned flags)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (flags & HL_BASE64_URL) {
        if (c == '-') return 62;
        if (c == '_') return 63;
    } else {
        if (c == '+') return 62;
        if (c == '/') return 63;
    }
    return -1;
}

int hl_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
                     size_t *out_len, unsigned flags)
{
    if (!out_len || (in_len > 0 && (!in || !out)))
        return -1;
    *out_len = 0;

    /* Split off the padding: only at the end, at most two, and only when
     * the flags allow it. Anything else that is '=' is an invalid char. */
    size_t data = in_len, pad = 0;
    while (data > 0 && in[data - 1] == '=' && pad < 3) {
        data--;
        pad++;
    }
    if (pad > 0) {
        if ((flags & HL_BASE64_NOPAD) || pad > 2 || (data + pad) % 4 != 0)
            return -1;
    }
    if (data % 4 == 1)
        return -1;                          /* no input encodes to this */
    if (data / 4 * 3 + (data % 4 ? data % 4 - 1 : 0) > out_cap)
        return -1;

    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < data; i++) {
        int v = value_of((unsigned char)in[i], flags);
        if (v < 0)
            return -1;
        acc = ((acc << 6) | (uint32_t)v) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    *out_len = o;
    return 0;
}
