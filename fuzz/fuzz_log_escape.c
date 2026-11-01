/*
 * fuzz_log_escape.c: libFuzzer harness for hl_log_escape_line (audit 27).
 *
 * Both log callbacks (serve.c's and shared/cli_log.c's) pass every formatted
 * record through hl_log_escape_line so app text that reaches a log call (an
 * uncaught exception's message, a ws / SSE error) cannot forge a second
 * record. Oracles, for every output size from 1 to past the input:
 *   - the output buffer (allocated at exactly outsz, so ASan brackets it) is
 *     NUL-terminated inside outsz;
 *   - it holds no raw CR or LF;
 *   - it equals the longest prefix of the reference escaping (CR / LF as the
 *     two characters \r / \n, other controls but tab as \xNN (audit 28),
 *     everything else as is) that ends on a token
 *     boundary and fits with its NUL - so no escape pair is ever split, and a
 *     record that fits is copied whole;
 *   - outsz == 0 writes nothing.
 * First two bytes pick outsz; the rest is the record.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/cli_log.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what)
{
    fprintf(stderr, "fuzz_log_escape: %s\n", what);
    abort();
}

/* Reference UTF-8 reader, written apart from hl_utf8_seq_len: decode the
 * code point and reject overlong, surrogate and out-of-range forms. Returns
 * the sequence length, or 0 when @p s does not start a well-formed one. */
static size_t ref_utf8(const unsigned char *s, size_t avail, unsigned *cp)
{
    size_t n;
    unsigned v, min;
    if (s[0] >= 0xF0 && s[0] <= 0xF7)      { n = 4; v = s[0] & 0x07; min = 0x10000; }
    else if (s[0] >= 0xE0 && s[0] <= 0xEF) { n = 3; v = s[0] & 0x0F; min = 0x800; }
    else if (s[0] >= 0xC0 && s[0] <= 0xDF) { n = 2; v = s[0] & 0x1F; min = 0x80; }
    else return 0;
    if (avail < n) return 0;
    for (size_t k = 1; k < n; k++) {
        if ((s[k] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[k] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return n;
}

/* True when the escaped output carries a C1 control or a stray 0x80-0x9F
 * byte raw (audit 42). */
static int has_raw_c1(const unsigned char *o, size_t len)
{
    for (size_t i = 0; i < len; ) {
        if (o[i] < 0x80) { i++; continue; }
        unsigned cp = 0;
        size_t n = ref_utf8(o + i, len - i, &cp);
        if (n) {
            if (cp <= 0x9F) return 1;
            i += n;
            continue;
        }
        if (o[i] <= 0x9F) return 1;
        i++;
    }
    return 0;
}

/* Reference: escape @p in into a fresh buffer, recording each token's end. */
static size_t ref_escape(const char *in, char *out, size_t *tok_end, size_t *ntok)
{
    static const char hx[] = "0123456789abcdef";
    size_t o = 0, t = 0, len = strlen(in);
    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x80) {
            /* audit 42: a whole UTF-8 sequence is one token, kept unless it
             * is a C1 control; a stray 0x80-0x9F is \xNN, a stray 0xA0+
             * byte is kept. */
            unsigned cp = 0;
            size_t n = ref_utf8((const unsigned char *)p,
                                len - (size_t)(p - in), &cp);
            int esc = n ? cp <= 0x9F : c <= 0x9F;
            if (!n) n = 1;
            for (size_t k = 0; k < n; k++) {
                unsigned char b = (unsigned char)p[k];
                if (esc) {
                    out[o++] = '\\'; out[o++] = 'x';
                    out[o++] = hx[b >> 4]; out[o++] = hx[b & 15];
                } else {
                    out[o++] = (char)b;
                }
            }
            p += n - 1;
            tok_end[t++] = o;
            continue;
        }
        if (*p == '\n')      { out[o++] = '\\'; out[o++] = 'n'; }
        else if (*p == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (((unsigned char)*p < 0x20 && *p != '\t') || *p == 0x7f) {
            out[o++] = '\\'; out[o++] = 'x';
            out[o++] = hx[(unsigned char)*p >> 4];
            out[o++] = hx[(unsigned char)*p & 15];
        }
        else                 { out[o++] = *p; }
        tok_end[t++] = o;
    }
    out[o] = '\0';
    *ntok = t;
    return o;
}

static void check(const char *in, size_t outsz, const char *ref,
                  const size_t *tok_end, size_t ntok)
{
    char *out = malloc(outsz ? outsz : 1);
    if (!out) return;
    memset(out, 'Z', outsz ? outsz : 1);
    hl_log_escape_line(in, out, outsz);
    if (outsz == 0) {
        if (out[0] != 'Z') fail("outsz 0 wrote to the buffer");
        free(out);
        return;
    }
    const char *nul = memchr(out, '\0', outsz);
    if (!nul) fail("output not NUL-terminated inside outsz");
    size_t olen = (size_t)(nul - out);
    for (size_t i = 0; i < olen; i++) {
        unsigned char c = (unsigned char)out[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            fail("raw control byte in escaped output");
    }
    if (has_raw_c1((const unsigned char *)out, olen))
        fail("raw C1 control or stray 0x80-0x9F byte in escaped output");
    /* Expected: tokens while the token's end + NUL fits. */
    size_t want = 0;
    for (size_t t = 0; t < ntok && tok_end[t] + 1 <= outsz; t++)
        want = tok_end[t];
    if (olen != want || memcmp(out, ref, want) != 0)
        fail("output is not the longest whole-token prefix of the escaping");
    free(out);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > 4096 + 2) return 0;
    size_t pick = (size_t)data[0] | ((size_t)data[1] << 8);
    size_t n = size - 2;
    char *in = malloc(n + 1);
    char *ref = malloc(4 * n + 1);   /* \xNN is the longest token */
    size_t *tok_end = malloc((n + 1) * sizeof *tok_end);
    if (!in || !ref || !tok_end) goto out;
    memcpy(in, data + 2, n);
    in[n] = '\0';

    size_t ntok = 0;
    size_t rlen = ref_escape(in, ref, tok_end, &ntok);

    check(in, pick % (rlen + 3), ref, tok_end, ntok);   /* includes 0 */
    check(in, rlen + 1, ref, tok_end, ntok);            /* exact fit */
    if (rlen > 0) check(in, rlen, ref, tok_end, ntok);  /* one short */
    check(in, 1024, ref, tok_end, ntok);                /* the callbacks' size */

    /* NULL input is the empty record. */
    char small[4];
    hl_log_escape_line(NULL, small, sizeof small);
    if (small[0] != '\0') fail("NULL input did not give an empty record");

    (void)hl_cli_log_is_native_source(in);
out:
    free(in); free(ref); free(tok_end);
    return 0;
}
