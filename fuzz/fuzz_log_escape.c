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

/* Reference: escape @p in into a fresh buffer, recording each token's end. */
static size_t ref_escape(const char *in, char *out, size_t *tok_end, size_t *ntok)
{
    size_t o = 0, t = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '\n')      { out[o++] = '\\'; out[o++] = 'n'; }
        else if (*p == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (((unsigned char)*p < 0x20 && *p != '\t') || *p == 0x7f) {
            static const char hx[] = "0123456789abcdef";
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
