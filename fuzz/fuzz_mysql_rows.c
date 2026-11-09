/*
 * libFuzzer harness for the MySQL / MariaDB connection layer's reply
 * handlers (cap/mysql_conn.c), above the packet reader fuzz_mysqlwire covers.
 *
 * The bytes are the server's (or an on-path attacker's): the Handshake v10
 * greeting and the auth exchange (AuthSwitchRequest, caching_sha2
 * AuthMoreData, OK / ERR); a text-protocol reply (column count, column
 * definitions, text rows of length-encoded cells, EOF / OK chains with
 * MORE_RESULTS); and a prepared statement's (COM_STMT_PREPARE OK, parameter
 * and column definitions, binary rows: the NULL bitmap and every type's
 * decoder, temporal formatting included). The real handlers run over a fake
 * transport (fuzz_db_wire_stubs.c) that serves the input in chunks whose
 * size the input chooses.
 *
 * Input: byte 0 picks the mode (0: handshake + auth, 1: text query, 2:
 * prepared statement, 3: multi-statement script; bit 2 a password in the
 * DSN; bit 3 sslmode=disable), byte 1 the chunk size (1..256), the rest is
 * the server's stream.
 *
 * Build:  make fuzz
 * Run:    ./fuzz/fuzz_mysql_rows fuzz/corpus_mysql_rows/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/mysql_conn.h"
#include "hull/cap/mysqlwire.h"
#include "fuzz_db_wire_stubs.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void desc_cb(void *ctx, const HlMyField *fields, int nfields)
{
    size_t *sum = ctx;
    for (int i = 0; i < nfields; i++)
        if (fields[i].name) *sum += strlen(fields[i].name) + fields[i].type;
}

static int text_row(void *ctx, const char *const *values, const size_t *lengths,
                    int ncols)
{
    size_t *sum = ctx;
    for (int i = 0; i < ncols; i++) {
        if (!values[i]) continue;
        for (size_t k = 0; k < lengths[i]; k++) *sum += (uint8_t)values[i][k];
    }
    return 0;
}

static int bin_row(void *ctx, const HlMyVal *vals, int ncols)
{
    size_t *sum = ctx;
    for (int i = 0; i < ncols; i++) {
        switch (vals[i].kind) {
        case HL_MY_VAL_INT:    *sum += (size_t)vals[i].v.i; break;
        case HL_MY_VAL_DOUBLE: *sum += vals[i].v.d > 0; break;
        case HL_MY_VAL_STR:
            for (size_t k = 0; k < vals[i].v.s.len; k++)
                *sum += (uint8_t)vals[i].v.s.ptr[k];
            break;
        default: break;
        }
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

    HlMyConn conn;
    size_t sum = 0;
    int64_t affected = 0;
    if ((mode & 3) == 0) {
        HlMyDsn dsn;
        memset(&dsn, 0, sizeof dsn);
        snprintf(dsn.host, sizeof dsn.host, "db.example");
        snprintf(dsn.port, sizeof dsn.port, "3306");
        snprintf(dsn.user, sizeof dsn.user, "app");
        snprintf(dsn.dbname, sizeof dsn.dbname, "app");
        if (mode & 4) snprintf(dsn.password, sizeof dsn.password, "pw");
        snprintf(dsn.sslmode, sizeof dsn.sslmode, "%s",
                 (mode & 8) ? "disable" : "prefer");
        if (hl_my_conn_start(&conn, -1, &dsn) == 0) {
            /* the post-auth round trips read the same stream on */
            if (hl_my_conn_session_init(&conn) == 0)
                (void)hl_my_conn_capture_login(&conn);
            hl_my_conn_close(&conn);
        }
    } else {
        memset(&conn, 0, sizeof conn);
        conn.transport = fz_wire_transport();
        conn.capabilities = HL_MY_CLIENT_PROTOCOL_41 | HL_MY_CLIENT_MULTI_RESULTS;
        if ((mode & 3) == 1) {
            (void)hl_my_conn_query(&conn, "SELECT 1", desc_cb, text_row, &sum,
                                   &affected);
        } else if ((mode & 3) == 2) {
            HlMyParam p[2];
            memset(p, 0, sizeof p);
            p[0].type = HL_MY_TYPE_LONGLONG; p[0].v.i = 7;
            p[1].is_null = 1;
            (void)hl_my_conn_query_prepared(&conn, "SELECT ?, ?", p, 2, desc_cb,
                                            bin_row, &sum, &affected);
        } else {
            (void)hl_my_conn_exec_multi(&conn, "SELECT 1; SELECT 2");
        }
        hl_my_conn_close(&conn);
    }
    (void)sum;
    free(buf);
    return 0;
}
