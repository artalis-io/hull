/*
 * libFuzzer harness for the PostgreSQL DSN parser (cap/pg_conn.c).
 *
 * hl_pg_dsn_parse does percent-decoding and a fair amount of pointer
 * arithmetic to split a postgres:// URL into fixed, bounded fields. Although
 * the DSN normally comes from operator config, a parser with escape decoding
 * and manual bounds is exactly the kind of code to fuzz: it must never read
 * out of bounds, overrun a field, or mishandle a truncated %-escape, and
 * every parsed field must stay a NUL-terminated string.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 * Run:
 *   ./fuzz/fuzz_pg_dsn fuzz/corpus_pg_dsn/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/pg_conn.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* The parser consumes a NUL-terminated string; give it an exact-sized
     * copy plus a terminator so ASan brackets any read past the input. */
    char *s = malloc(size + 1);
    if (!s)
        return 0;
    if (size)
        memcpy(s, data, size);
    s[size] = '\0';

    HlPgDsn d;
    char err[128];
    if (hl_pg_dsn_parse(s, &d, err, sizeof err) == 0) {
        /* On success every field must be a bounded C string. */
        (void)strlen(d.host);
        (void)strlen(d.port);
        (void)strlen(d.user);
        (void)strlen(d.password);
        (void)strlen(d.dbname);
        (void)strlen(d.sslmode);
    }
    hl_pg_dsn_scrub(&d);

    free(s);
    return 0;
}
