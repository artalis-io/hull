/*
 * libFuzzer harness for the MySQL/MariaDB wire reader (cap/mysqlwire.c).
 *
 * The reader consumes bytes straight off a socket from a server (or an on-path
 * attacker): every packet length, length-encoded int, and length-prefixed
 * string comes from the wire. A single missing bounds check would be a crash
 * or an out-of-bounds read on attacker-controlled input. This harness extracts
 * frames and drains each body through the cursor with a mix of typed reads
 * (including attacker-length lenenc strings), then runs the OK / ERR parsers.
 * An exact-sized heap copy lets ASan red-zones trap any read past the input.
 *
 * Build:  make fuzz
 * Run:    ./fuzz/fuzz_mysqlwire fuzz/corpus_mysqlwire/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/mysqlwire.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static void drain_body(const HlMyFrame *f)
{
    HlMyCursor c;
    hl_my_cursor_init(&c, f);
    while (!hl_my_cursor_err(&c) && c.remaining > 0) {
        uint8_t k = hl_my_get_u8(&c);
        switch (k % 7) {
        case 0: (void)hl_my_get_u16(&c); break;
        case 1: (void)hl_my_get_u24(&c); break;
        case 2: (void)hl_my_get_u32(&c); break;
        case 3: (void)hl_my_get_u64(&c); break;
        case 4: { int n = 0; (void)hl_my_get_lenenc_int(&c, &n); break; }
        case 5: { size_t sl = 0; (void)hl_my_get_lenenc_str(&c, &sl); break; }
        case 6: (void)hl_my_get_bytes(&c, (size_t)k); break;
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf) return 0;
    if (size) memcpy(buf, data, size);

    size_t off = 0;
    int guard = 0;
    while (off < size && guard++ < 4096) {
        HlMyFrame f;
        size_t consumed = 0;
        HlMyResult r = hl_my_frame_next(buf + off, size - off, &f, &consumed);
        if (r != HL_MY_OK) break;

        drain_body(&f);
        HlMyOk ok;   (void)hl_my_parse_ok(&f, &ok);
        HlMyErr e;   (void)hl_my_parse_err(&f, 1, &e);
        (void)hl_my_parse_err(&f, 0, &e);

        if (consumed == 0) break;   /* no forward progress */
        off += consumed;
    }

    free(buf);
    return 0;
}
