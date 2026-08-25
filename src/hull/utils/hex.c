/*
 * utils/hex.c - lowercase hex encoding of a byte buffer. See utils/hex.h for
 * the bounded contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hex.h"

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
