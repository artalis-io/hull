/*
 * libFuzzer harness for the Postgres connection layer's message handlers
 * (cap/pg_conn.c), above the frame reader fuzz_pgwire covers.
 *
 * The bytes are the server's (or an on-path attacker's): the startup
 * exchange - AuthenticationSASL / SASLContinue (the SCRAM server-first, its
 * nonce, salt and iteration count) / SASLFinal (the server signature),
 * cleartext and MD5 requests, ParameterStatus, BackendKeyData, every
 * ErrorResponse / NoticeResponse field loop - and a query's reply:
 * RowDescription, DataRow (column counts and lengths), CommandComplete
 * (parse_affected), CopyIn / CopyOut, NotificationResponse, ReadyForQuery.
 * The real handlers run over a fake transport (fuzz_db_wire_stubs.c) that
 * serves the input in chunks whose size the input chooses, so a frame
 * arrives split across reads the way a network splits it.
 *
 * Input: byte 0 picks the mode (bit 0: startup exchange vs a query reply;
 * bit 1: extended-protocol query vs simple script; bit 2: a password in the
 * DSN; bits 3-4: sslmode disable / prefer / require / verify-full), byte 1
 * the chunk size (1..256), the rest is the server's stream.
 *
 * Build:  make fuzz
 * Run:    ./fuzz/fuzz_pg_msg fuzz/corpus_pg_msg/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/pg_conn.h"
#include "hull/cap/pgwire.h"
#include "fuzz_db_wire_stubs.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void desc_cb(void *ctx, const HlPgField *fields, int nfields)
{
    size_t *sum = ctx;
    for (int i = 0; i < nfields; i++)
        if (fields[i].name) *sum += strlen(fields[i].name) + (size_t)fields[i].oid;
}

static int row_cb(void *ctx, const char *const *values, const int32_t *lengths,
                  int ncols)
{
    size_t *sum = ctx;
    for (int i = 0; i < ncols; i++) {
        if (!values[i]) continue;
        /* touch every byte the handler says is there (ASan checks it) */
        for (int32_t k = 0; k < lengths[i]; k++) *sum += (uint8_t)values[i][k];
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2) return 0;
    uint8_t mode = data[0];
    size_t chunk = (size_t)data[1] + 1;
    size_t n = size - 2;
    uint8_t *buf = malloc(n ? n : 1);   /* exact size: ASan red zones */
    if (!buf) return 0;
    if (n) memcpy(buf, data + 2, n);
    fz_wire_feed(buf, n, chunk);

    HlPgConn conn;
    size_t sum = 0;
    if (mode & 1) {
        static const char *modes[] = { "disable", "prefer", "require", "verify-full" };
        HlPgDsn dsn;
        memset(&dsn, 0, sizeof dsn);
        snprintf(dsn.host, sizeof dsn.host, "db.example");
        snprintf(dsn.user, sizeof dsn.user, "app");
        snprintf(dsn.dbname, sizeof dsn.dbname, "app");
        if (mode & 4) snprintf(dsn.password, sizeof dsn.password, "pw");
        snprintf(dsn.sslmode, sizeof dsn.sslmode, "%s", modes[(mode >> 3) & 3]);
        if (hl_pg_conn_start(&conn, -1, &dsn) == 0)
            hl_pg_conn_close(&conn);
    } else {
        memset(&conn, 0, sizeof conn);
        conn.transport = fz_wire_transport();
        int64_t affected = 0;
        if (mode & 2) {
            HlPgParam p[2];
            memset(p, 0, sizeof p);
            (void)hl_pg_query(&conn, "SELECT ?, ?", p, 2, desc_cb, row_cb, &sum,
                              &affected);
        } else {
            (void)hl_pg_exec_simple(&conn, "SELECT 1; SELECT 2");
        }
        hl_pg_conn_close(&conn);
    }
    (void)sum;
    free(buf);
    return 0;
}
