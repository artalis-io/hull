/*
 * cap/wasm_log.c - escaping a guest's HL_WASM_OP_LOG message (audit 26)
 *
 * A leaf of its own (no WAMR) so fuzz/fuzz_wasm_log_escape links it alone.
 * See hl_cap_wasm_log_escape in cap/wasm.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"

#include <string.h>

/* One input byte's escaped form in @p tmp; returns its length (1, 2 or 4). */
static size_t log_token(unsigned char c, char tmp[4])
{
    static const char hex[] = "0123456789abcdef";
    if (c == '\n') { tmp[0] = '\\'; tmp[1] = 'n'; return 2; }
    if (c == '\r') { tmp[0] = '\\'; tmp[1] = 'r'; return 2; }
    if (c < 0x20 || c == 0x7f) {
        tmp[0] = '\\'; tmp[1] = 'x';
        tmp[2] = hex[c >> 4]; tmp[3] = hex[c & 0xf];
        return 4;
    }
    tmp[0] = (char)c;
    return 1;
}

size_t hl_cap_wasm_log_escape(const char *msg, size_t len,
                              char *out, size_t outsz)
{
    if (!out || outsz == 0) return 0;
    size_t o = 0, i = 0;
    char tmp[4];
    for (; msg && i < len; i++) {
        size_t n = log_token((unsigned char)msg[i], tmp);
        if (o + n + 1 > outsz) break;
        memcpy(out + o, tmp, n);
        o += n;
    }
    if (msg && i < len) {
        /* Truncated: end with "..." where it fits, after the last WHOLE
         * token that leaves room for it - cutting back to a byte count split
         * an escape ("a\x0...", found by fuzz_wasm_log_escape). */
        size_t dots = outsz > 4 ? 3 : 0;
        if (o + dots + 1 > outsz) {
            size_t lim = outsz - 1 - dots, w = 0;
            for (size_t j = 0; j < i; j++) {
                size_t n = log_token((unsigned char)msg[j], tmp);
                if (w + n > lim) break;
                w += n;
            }
            o = w;
        }
        memcpy(out + o, "...", dots);
        o += dots;
    }
    out[o] = '\0';
    return o;
}

#endif /* HL_ENABLE_WASM */
