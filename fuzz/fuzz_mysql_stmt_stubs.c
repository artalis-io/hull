/*
 * fuzz_mysql_stmt_stubs.c: the MySQL wire-client symbols cap/db_mysql.c
 * references, for fuzz_mysql_stmt. The fuzzed classifiers never call them;
 * the real client (cap/mysql_conn.c) needs cap/crypto (mbedTLS) and Keel's
 * TLS. No Hull header is included, so the stub signatures cannot conflict.
 * Each aborts if reached.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdlib.h>

void hl_my_conn_open(void);
void hl_my_conn_open(void) { abort(); }
void hl_my_conn_close(void);
void hl_my_conn_close(void) { abort(); }
void hl_my_conn_exec_multi(void);
void hl_my_conn_exec_multi(void) { abort(); }
void hl_my_conn_ping(void);
void hl_my_conn_ping(void) { abort(); }
void hl_my_conn_query(void);
void hl_my_conn_query(void) { abort(); }
void hl_my_conn_query_prepared(void);
void hl_my_conn_query_prepared(void) { abort(); }
void hl_my_dsn_parse(void);
void hl_my_dsn_parse(void) { abort(); }
void hl_my_dsn_scrub(void);
void hl_my_dsn_scrub(void) { abort(); }
void hl_my_conn_reset(void);
void hl_my_conn_reset(void) { abort(); }
void hl_db_txn_note(void);
void hl_db_txn_note(void) { abort(); }
