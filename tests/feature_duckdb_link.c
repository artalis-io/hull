/*
 * feature_duckdb_link.c - link-smoke for the DuckDB feature archive.
 *
 * Compiled + linked against ONLY libhull_feature-duckdb.a (plus the C++ runtime
 * and libc), with NO linker --start-group. It proves the combined feature
 * archive is self-contained: bundling the DuckDB static libs into one archive
 * turns their circular refs intra-archive, so ld resolves them by iterating a
 * single archive to a fixed point -- no group needed at the composing link.
 *
 * Exercises the real backend vtable (hl_db_backend_duckdb), not just symbol
 * presence: open :memory:, create a table, bind + insert a row, close.
 *
 * Not a utest (it links a special archive, not the default test objects); the
 * CI DuckDB job builds + runs it. Compile with -DHL_ENABLE_DUCKDB.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/db_backend.h"
#include "hull/cap/db_duckdb.h"
#include <stdio.h>

int main(void)
{
    void *ctx = NULL;
    if (hl_db_backend_duckdb.open(&ctx, "duckdb://:memory:", NULL) != 0) {
        fprintf(stderr, "feature link-smoke: open failed\n");
        return 1;
    }
    HlDbHandle h;
    h.backend = &hl_db_backend_duckdb;
    h.ctx = ctx;

    if (hl_db_exec(&h, "CREATE TABLE t (x INTEGER)", NULL, 0) != 0) {
        fprintf(stderr, "feature link-smoke: create failed\n");
        return 1;
    }
    HlValue v = { .type = HL_TYPE_INT, .i = 42 };
    if (hl_db_exec(&h, "INSERT INTO t VALUES (?)", &v, 1) != 0) {
        fprintf(stderr, "feature link-smoke: insert failed\n");
        return 1;
    }
    hl_db_backend_duckdb.close(&h);

    printf("FEATURE DUCKDB LINK OK\n");
    return 0;
}
