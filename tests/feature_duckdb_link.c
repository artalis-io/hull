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
#include "hull/cap/db_budget.h"
#include "hull/cap/db_duckdb.h"
#include <stdio.h>

/* The archive's references into the base: duck_execute charges each DuckDB
 * task to the run's budget (cap/db_common.c, audit 15), and duck_rollback
 * runs unbound (audit 16); duck_query / duck_exec_script refuse SQL that
 * calls a dynamic-SQL function (audit 25). A composed app gets the real budget functions
 * from the platform lib (tests/e2e_feature_duckdb.sh exercises them); this
 * smoke links the archive alone, so they stand in with "no budget bound" -
 * never an interrupt - and "no dynamic SQL". */
int hl_db_budget_charge(int64_t units)
{
    (void)units;
    return 0;
}

HlDbBudgetBinding hl_db_budget_swap(HlDbBudgetFn fn, void *ud)
{
    (void)fn;
    (void)ud;
    HlDbBudgetBinding none = {0};
    return none;
}

void hl_db_budget_restore(HlDbBudgetBinding prev)
{
    (void)prev;
}

int hl_cap_db_sql_calls_dynamic_sql(const char *sql);
int hl_cap_db_sql_calls_dynamic_sql(const char *sql)
{
    (void)sql;
    return 0;
}

int hl_cap_db_sql_reads_whole_database(const char *sql);
int hl_cap_db_sql_reads_whole_database(const char *sql)
{
    (void)sql;
    return 0;
}

int main(void)
{
    void *ctx = NULL;
    if (hl_db_backend_duckdb.open(&ctx, "duckdb://:memory:", NULL) != 0) {
        fprintf(stderr, "feature link-smoke: open failed\n");
        return 1;
    }
    HlDbHandle h = {0};
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
