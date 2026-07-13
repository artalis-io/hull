/*
 * libFuzzer harness for the PostgreSQL wire-protocol reader (cap/pgwire.c).
 *
 * The reader consumes bytes straight off a socket from a PostgreSQL server
 * (or an on-path attacker): every message length, field count, and
 * length-prefixed column comes from the wire. A single missing bounds check
 * would be a crash or an out-of-bounds read on attacker-controlled input.
 * This harness treats the whole fuzz input as such a stream: it extracts
 * frames and drains each body through the cursor with a mix of typed reads,
 * including attacker-length get_bytes. An exact-sized heap copy lets ASan
 * red-zones trap any read past the input.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 * Run:
 *   ./fuzz/fuzz_pgwire fuzz/corpus_pgwire/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/pgwire.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static void drain_body(const HlPgFrame *f)
{
    HlPgCursor c;
    hl_pg_cursor_init(&c, &f[0]);
    while (!hl_pg_cursor_err(&c) && c.remaining > 0) {
        uint8_t k = hl_pg_get_u8(&c);
        switch (k & 3) {
        case 0:
            (void)hl_pg_get_i16(&c);
            break;
        case 1:
            (void)hl_pg_get_i32(&c);
            break;
        case 2: {
            const char *s = hl_pg_get_cstr(&c);
            if (s)
                (void)strlen(s);   /* must be a bounded NUL-terminated string */
            break;
        }
        case 3: {
            /* An attacker-controlled length-prefixed field (like a DataRow
             * column): the negative / oversized cases must be rejected. */
            int32_t n = hl_pg_get_i32(&c);
            if (n > 0)
                (void)hl_pg_get_bytes(&c, (size_t)n);
            break;
        }
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf)
        return 0;
    if (size)
        memcpy(buf, data, size);

    size_t off = 0;
    while (off < size) {
        HlPgFrame f;
        size_t consumed = 0;
        HlPgResult r = hl_pg_frame_next(buf + off, size - off, &f, &consumed);
        if (r != HL_PG_OK)
            break;                 /* NEED_MORE (incomplete) or ERR (bad len) */
        drain_body(&f);
        if (consumed == 0)
            break;                 /* defensive: guarantee forward progress */
        off += consumed;
    }

    free(buf);
    return 0;
}
