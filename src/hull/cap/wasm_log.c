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
#include "hull/shared/term_escape.h"   /* hl_term_high_unit (audit 42) */

#include <string.h>

/* The escaped form of the unit of input at msg[i] in @p tmp: returns its
 * length (at most 16) and sets *@p used to the input bytes it takes - one,
 * or a whole UTF-8 sequence, which is kept together or escaped together
 * (a C1 control, C2 80-9F, as \xc2\xNN; audit 42, hl_term_high_unit). */
static size_t log_token(const char *msg, size_t len, size_t i,
                        size_t *used, char tmp[16])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char c = (unsigned char)msg[i];
    *used = 1;
    if (c == '\n') { tmp[0] = '\\'; tmp[1] = 'n'; return 2; }
    if (c == '\r') { tmp[0] = '\\'; tmp[1] = 'r'; return 2; }
    if (c >= 0x80) {
        int esc;
        size_t n = hl_term_high_unit((const unsigned char *)msg + i, len - i,
                                     &esc);
        size_t w = 0;
        for (size_t k = 0; k < n; k++) {
            unsigned char b = (unsigned char)msg[i + k];
            if (esc) {
                tmp[w++] = '\\'; tmp[w++] = 'x';
                tmp[w++] = hex[b >> 4]; tmp[w++] = hex[b & 0xf];
            } else {
                tmp[w++] = (char)b;
            }
        }
        *used = n;
        return w;
    }
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
    size_t o = 0, i = 0, used;
    char tmp[16];
    while (msg && i < len) {
        size_t n = log_token(msg, len, i, &used, tmp);
        if (o + n + 1 > outsz) break;
        memcpy(out + o, tmp, n);
        o += n;
        i += used;
    }
    if (msg && i < len) {
        /* Truncated: end with "..." where it fits, after the last WHOLE
         * token that leaves room for it - cutting back to a byte count split
         * an escape ("a\x0...", found by fuzz_wasm_log_escape). */
        size_t dots = outsz > 4 ? 3 : 0;
        if (o + dots + 1 > outsz) {
            size_t lim = outsz - 1 - dots, w = 0;
            for (size_t j = 0; j < i; j += used) {
                size_t n = log_token(msg, len, j, &used, tmp);
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
