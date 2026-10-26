/*
 * fuzz_wasm_log_escape.c: libFuzzer harness for hl_cap_wasm_log_escape
 * (audit 26).
 *
 * A guest's HL_WASM_OP_LOG message reaches the log through this escaper, so a
 * guest cannot write a log line of its own. Oracles, for every output size
 * from 0 to past the input:
 *   - the output buffer (allocated at exactly outsz, so ASan brackets it) is
 *     NUL-terminated inside outsz and the return value is its length;
 *   - it holds no raw control byte (C0, tab included, or DEL);
 *   - a message whose escaping fits is copied whole; one that does not is the
 *     longest whole-token prefix of the reference escaping that leaves room
 *     for "..." (when outsz > 4) and the NUL, followed by "..." - so no
 *     escape is ever split;
 *   - outsz == 0 writes nothing, and a NULL message is the empty string.
 * First two bytes pick outsz; the rest is the message (embedded NULs kept).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/wasm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what)
{
    fprintf(stderr, "fuzz_wasm_log_escape: %s\n", what);
    abort();
}

/* Reference: escape @p n bytes into @p out, recording each token's end. */
static size_t ref_escape(const uint8_t *in, size_t n, char *out, size_t *tok_end)
{
    static const char hx[] = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (c == '\n')                 { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r')            { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c < 0x20 || c == 0x7f) {
            out[o++] = '\\'; out[o++] = 'x';
            out[o++] = hx[c >> 4]; out[o++] = hx[c & 15];
        }
        else                           { out[o++] = (char)c; }
        tok_end[i] = o;
    }
    out[o] = '\0';
    return o;
}

static void check(const uint8_t *in, size_t n, size_t outsz, const char *ref,
                  size_t rlen, const size_t *tok_end)
{
    char *out = malloc(outsz ? outsz : 1);
    if (!out) return;
    memset(out, 'Z', outsz ? outsz : 1);
    size_t r = hl_cap_wasm_log_escape((const char *)in, n, out, outsz);
    if (outsz == 0) {
        if (r != 0 || out[0] != 'Z') fail("outsz 0 wrote to the buffer");
        free(out);
        return;
    }
    const char *nul = memchr(out, '\0', outsz);
    if (!nul) fail("output not NUL-terminated inside outsz");
    size_t olen = (size_t)(nul - out);
    if (r != olen) fail("return value is not the output length");
    for (size_t i = 0; i < olen; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c < 0x20 || c == 0x7f) fail("raw control byte in escaped output");
    }
    if (rlen + 1 <= outsz) {
        if (olen != rlen || memcmp(out, ref, rlen) != 0)
            fail("a message that fits was not copied whole");
    } else {
        size_t dots = outsz > 4 ? 3 : 0;
        size_t lim = outsz - 1 - dots, want = 0;
        for (size_t t = 0; t < n && tok_end[t] <= lim; t++)
            want = tok_end[t];
        if (olen != want + dots || memcmp(out, ref, want) != 0 ||
            memcmp(out + want, "...", dots) != 0)
            fail("truncation is not the longest whole-token prefix + \"...\"");
    }
    free(out);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > 4096 + 2) return 0;
    size_t pick = (size_t)data[0] | ((size_t)data[1] << 8);
    const uint8_t *in = data + 2;
    size_t n = size - 2;
    char *ref = malloc(4 * n + 1);   /* \xNN is the longest token */
    size_t *tok_end = malloc((n + 1) * sizeof *tok_end);
    if (!ref || !tok_end) goto out;

    size_t rlen = ref_escape(in, n, ref, tok_end);

    check(in, n, pick % (rlen + 3), ref, rlen, tok_end);   /* includes 0 */
    check(in, n, rlen + 1, ref, rlen, tok_end);            /* exact fit */
    if (rlen > 0) check(in, n, rlen, ref, rlen, tok_end);  /* one short */
    for (size_t s = 1; s <= 8; s++)                         /* the dots edge */
        check(in, n, s, ref, rlen, tok_end);
    check(in, n, 4096, ref, rlen, tok_end);                /* the host's size */

    char small[4];
    if (hl_cap_wasm_log_escape(NULL, 5, small, sizeof small) != 0 || small[0])
        fail("NULL message did not give an empty string");
out:
    free(ref); free(tok_end);
    return 0;
}
