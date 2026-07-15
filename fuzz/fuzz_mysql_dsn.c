/*
 * libFuzzer harness for the MySQL/MariaDB DSN parser (cap/mysql_conn.c).
 *
 * hl_my_dsn_parse does percent-decoding and manual pointer arithmetic to split
 * a mysql:// URL into fixed, bounded fields. Even though the DSN normally comes
 * from operator config, a parser with escape decoding and manual bounds is
 * exactly what to fuzz: it must never read out of bounds, overrun a field, or
 * mishandle a truncated %-escape, and every parsed field must stay a
 * NUL-terminated string.
 *
 * Build:  make fuzz
 * Run:    ./fuzz/fuzz_mysql_dsn fuzz/corpus_mysql_dsn/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/mysql_conn.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Exact-sized copy + NUL so ASan brackets any read past the input. */
    char *s = malloc(size + 1);
    if (!s) return 0;
    if (size) memcpy(s, data, size);
    s[size] = '\0';

    HlMyDsn d;
    char err[128];
    if (hl_my_dsn_parse(s, &d, err, sizeof err) == 0) {
        /* On success every field must be a bounded, NUL-terminated string. */
        if (memchr(d.host, 0, sizeof d.host) == NULL) abort();
        if (memchr(d.port, 0, sizeof d.port) == NULL) abort();
        if (memchr(d.user, 0, sizeof d.user) == NULL) abort();
        if (memchr(d.password, 0, sizeof d.password) == NULL) abort();
        if (memchr(d.dbname, 0, sizeof d.dbname) == NULL) abort();
        if (memchr(d.sslmode, 0, sizeof d.sslmode) == NULL) abort();
        hl_my_dsn_scrub(&d);
    }

    free(s);
    return 0;
}
