/*
 * fuzz_encoding.c: libFuzzer harness for the C codecs (utils/hex, utils/base64).
 *
 * The decoders read untrusted text: SCRAM server messages, DSN escapes,
 * signatures and, through hull.encoding, anything an app decodes. Beyond
 * "no crash, no over-read" under ASan+UBSan, two properties are asserted:
 *
 *   1. Canonical: text a decoder ACCEPTS re-encodes to exactly itself (hex
 *      compared case-insensitively, since either case decodes). Strict
 *      decoding promises each value has one encoding; a second accepted
 *      spelling is a malleability bug, not a style issue.
 *   2. Round trip: any bytes, encoded under each flag set, decode back to the
 *      same bytes.
 *
 * Every decode runs into an exactly-sized heap buffer, so a write past the
 * documented bound is an ASan report rather than a silent overrun.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../src/hull/utils/base64.h"
#include "../src/hull/utils/hex.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const unsigned FLAG_SETS[] = {
    0, HL_BASE64_NOPAD, HL_BASE64_URL, HL_BASE64_URL | HL_BASE64_NOPAD,
};

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static void check_hex(const uint8_t *data, size_t size)
{
    size_t cap = size / 2;
    uint8_t *out = malloc(cap ? cap : 1);
    if (!out) return;
    int got = hl_hex_decode((const char *)data, size, out, cap);
    if (got >= 0) {
        if ((size_t)got != size / 2) abort();
        char *back = malloc(size + 1);
        if (!back) { free(out); return; }
        if (hl_hex_encode(out, (size_t)got, back, size + 1) != 0) abort();
        for (size_t i = 0; i < size; i++)
            if (lower(data[i]) != back[i]) abort();
        free(back);
    }
    free(out);
}

static void check_base64_decode(const uint8_t *data, size_t size, unsigned flags)
{
    /* The documented bound: n characters decode to at most n * 3 / 4 bytes. */
    size_t cap = size * 3 / 4;
    uint8_t *out = malloc(cap ? cap : 1);
    if (!out) return;
    size_t got = 0;
    if (hl_base64_decode((const char *)data, size, out, cap, &got, flags) == 0) {
        if (got > cap) abort();
        /* Re-encode in the input's own padding form. */
        unsigned enc = flags & HL_BASE64_URL;
        if (!(size > 0 && data[size - 1] == '='))
            enc |= HL_BASE64_NOPAD;
        size_t len = hl_base64_encoded_len(got, enc);
        char *back = malloc(len + 1);
        if (!back) { free(out); return; }
        if (hl_base64_encode(out, got, back, len + 1, enc) != (int)len) abort();
        if (len != size || memcmp(back, data, size) != 0) abort();
        free(back);
    }
    free(out);
}

static void check_round_trip(const uint8_t *data, size_t size, unsigned flags)
{
    size_t len = hl_base64_encoded_len(size, flags);
    char *text = malloc(len + 1);
    uint8_t *back = malloc(size ? size : 1);
    if (!text || !back) { free(text); free(back); return; }
    if (hl_base64_encode(data, size, text, len + 1, flags) != (int)len) abort();
    size_t got = 0;
    if (hl_base64_decode(text, len, back, size, &got, flags) != 0) abort();
    if (got != size || memcmp(back, data, size) != 0) abort();
    free(text);
    free(back);

    char *hex = malloc(size * 2 + 1);
    uint8_t *hback = malloc(size ? size : 1);
    if (!hex || !hback) { free(hex); free(hback); return; }
    if (hl_hex_encode(data, size, hex, size * 2 + 1) != 0) abort();
    if (hl_hex_decode(hex, size * 2, hback, size) != (int)size) abort();
    if (memcmp(hback, data, size) != 0) abort();
    free(hex);
    free(hback);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    check_hex(data, size);
    for (size_t i = 0; i < sizeof FLAG_SETS / sizeof FLAG_SETS[0]; i++) {
        check_base64_decode(data, size, FLAG_SETS[i]);
        check_round_trip(data, size, FLAG_SETS[i]);
    }
    return 0;
}
