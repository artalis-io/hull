/*
 * libFuzzer harness for the PostgreSQL placeholder rewriter (cap/pg_conn.c).
 *
 * hl_pg_rewrite_sql scans SQL to turn '?' into '$n' while skipping quoted
 * strings, identifiers, dollar-quoted bodies, and comments. It walks the
 * input with lookahead (p[1]) and copies into a bounded buffer, so it must
 * never read past the NUL or overrun the output, and the result must always
 * be a NUL-terminated string. Fuzz arbitrary SQL-ish bytes against it.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 * Run:
 *   ./fuzz/fuzz_pg_rewrite fuzz/corpus_pg_rewrite/ -max_total_time=60
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
    char *sql = malloc(size + 1);
    if (!sql)
        return 0;
    if (size)
        memcpy(sql, data, size);
    sql[size] = '\0';

    /* Output up to a generous multiple of the input (every '?' can expand). */
    size_t outcap = size * 4 + 64;
    char *out = malloc(outcap);
    if (out) {
        int n = -1;
        if (hl_pg_rewrite_sql(sql, out, outcap, &n) == 0) {
            (void)strlen(out);   /* must be NUL-terminated */
        }
        free(out);
    }

    free(sql);
    return 0;
}
