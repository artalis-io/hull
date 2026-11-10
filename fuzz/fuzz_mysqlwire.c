/*
 * libFuzzer harness for the MySQL/MariaDB wire reader (cap/mysqlwire.c).
 *
 * The reader consumes bytes straight off a socket from a server (or an on-path
 * attacker): every packet length, length-encoded int, and length-prefixed
 * string comes from the wire. A single missing bounds check would be a crash
 * or an out-of-bounds read on attacker-controlled input. This harness extracts
 * frames and drains each body through the cursor with a mix of typed reads
 * (including attacker-length lenenc strings), then runs the OK / ERR /
 * handshake / column-definition / PREPARE_OK parsers, the version readers
 * (the _hull_* collation and roles, audit 44) and the CURRENT_ROLE() list
 * validator behind the session reset's SET ROLE.
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

/* The body read as server text: a VERSION() value (collation, roles) and a
 * CURRENT_ROLE() list (the SET ROLE the session reset would send). The
 * statement built from a value it accepts must hold no ';' outside quotes
 * and stay inside its buffer. */
static void extra_checks(const HlMyFrame *f)
{
    char v[512];
    size_t n = f->body_len < sizeof v - 1 ? f->body_len : sizeof v - 1;
    memcpy(v, f->body, n);
    v[n] = '\0';
    (void)hl_my_bin_collation(v);
    (void)hl_my_roles_supported(v);
    char out[1100];
    for (int mariadb = 0; mariadb < 2; mariadb++) {
        size_t cap = f->body_len ? 9u + (size_t)f->body[0] * 4u : sizeof out;
        if (cap > sizeof out) cap = sizeof out;
        if (hl_my_role_restore_sql(mariadb, (const char *)f->body, f->body_len,
                                   out, cap) != 0)
            continue;
        size_t ol = strnlen(out, cap);
        if (ol >= cap) abort();                     /* not terminated */
        if (strncmp(out, "SET ROLE ", 9) != 0) abort();
        char q = 0;
        for (size_t i = 9; i < ol; i++) {
            if (q) { if (out[i] == q) q = 0; continue; }
            if (out[i] == '`') q = '`';
            else if (out[i] == ';') abort();        /* a second statement */
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
        HlMyHandshake hs;
        if (hl_my_parse_handshake(&f, &hs) == 0) {
            /* the version string drives the _hull_* collation and roles */
            (void)hl_my_bin_collation(hs.server_version);
            (void)hl_my_roles_supported(hs.server_version);
        }
        HlMyColumn col;    (void)hl_my_parse_column_def(&f, &col);
        HlMyPrepareOk po;  (void)hl_my_parse_prepare_ok(&f, &po);
        extra_checks(&f);

        if (consumed == 0) break;   /* no forward progress */
        off += consumed;
    }

    free(buf);
    return 0;
}
