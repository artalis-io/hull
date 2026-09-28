/*
 * utils/hex.c - lowercase hex encoding of a byte buffer, and decoding. See utils/hex.h for
 * the bounded contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hex.h"

#include <limits.h>

int hl_hex_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    static const char digits[] = "0123456789abcdef";

    if (!out || out_cap == 0)
        return -1;
    out[0] = '\0';                         /* fail-closed default */
    if (in_len > 0 && !in)
        return -1;
    if (in_len > (SIZE_MAX - 1) / 2)       /* in_len*2 + 1 would overflow */
        return -1;
    if (out_cap < in_len * 2 + 1)
        return -1;

    for (size_t i = 0; i < in_len; i++) {
        out[i * 2]     = digits[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[in[i] & 0xF];
    }
    out[in_len * 2] = '\0';
    return 0;
}

int hl_hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hl_hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_cap)
{
    if (hex_len == 0)
        return 0;
    if (!hex || !out || hex_len % 2 != 0 || out_cap < hex_len / 2)
        return -1;
    if (hex_len / 2 > (size_t)INT_MAX)
        return -1;
    for (size_t i = 0; i < hex_len / 2; i++) {
        int hi = hl_hex_digit((unsigned char)hex[2 * i]);
        int lo = hl_hex_digit((unsigned char)hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(hex_len / 2);
}
