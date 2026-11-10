/*
 * fuzz_db_wire_stubs.h: the fake transport fuzz_db_wire_stubs.c serves to
 * the message-level wire fuzzers (fuzz_pg_msg, fuzz_mysql_rows).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef FUZZ_DB_WIRE_STUBS_H
#define FUZZ_DB_WIRE_STUBS_H

#include <stddef.h>
#include <stdint.h>

struct HlDbTransport;

/* Serve @p data as the server's bytes, at most @p chunk bytes a recv. */
void fz_wire_feed(const uint8_t *data, size_t len, size_t chunk);

/* The fake transport, to hand a connection directly. */
struct HlDbTransport *fz_wire_transport(void);

#endif /* FUZZ_DB_WIRE_STUBS_H */
