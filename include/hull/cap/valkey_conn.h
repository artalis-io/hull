/*
 * cap/valkey_conn.h: Valkey/Redis DSN + RESP connection (transport).
 *
 * DSN parsing (pure, fuzzable) plus the blocking socket/TLS connection and the
 * HELLO 3 / AUTH handshake, split from the RESP codec (cap/respwire.c) and the
 * KV backend vtable (cap/valkey.c). The transport lives in the composed
 * `--with=valkey` feature archive; only the DSN parser is needed at test time.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_VALKEY_CONN_H
#define HL_CAP_VALKEY_CONN_H

#include <stddef.h>
#include <stdint.h>

/* Parsed DSN. Password is secret - scrub with hl_valkey_dsn_scrub after use. */
typedef struct HlValkeyDsn {
    char host[256];
    char port[16];        /* numeric string; default "6379" */
    char username[128];   /* optional ACL user */
    char password[256];   /* optional secret */
    char dbindex[16];     /* logical DB index; default "0" */
    int  tls;             /* 1 for rediss:// / valkeys:// */
    int  verify;          /* TLS chain+hostname verify (1) unless sslmode=disable/none */
    int  connect_timeout_ms;  /* default 5000 */
} HlValkeyDsn;

/*
 * Parse `scheme://[user[:pass]@]host[:port][/db][?opts]`.
 * Schemes (case-insensitive): redis / rediss / valkey / valkeys (the *s
 * variants select TLS). Query opts: connect_timeout (ms), sslmode
 * (verify-full|require|disable|none). Returns 0 on success; on failure returns
 * -1 with a human message in errbuf. Every field is bounded; oversized input
 * is an error, never a silent truncation.
 */
int hl_valkey_dsn_parse(const char *dsn, HlValkeyDsn *out, char *errbuf, size_t errlen);

/* Zero the password field (call after connecting / on teardown). */
void hl_valkey_dsn_scrub(HlValkeyDsn *dsn);

/* ── Connection (blocking; single-owner, single-thread) ───────────────────
 *
 * Opaque RESP connection. Owns the socket, an optional TLS session, a bounded
 * receive buffer, and a non-moving reply arena. NOT thread-safe. */
typedef struct HlValkeyConn HlValkeyConn;

#include "hull/cap/respwire.h"
#include "hull/utils/alloc.h"   /* HlAllocator (pluggable allocator) */

/*
 * Connect (TCP + optional rediss TLS via the shared client) and run the
 * HELLO 3 / AUTH handshake (RESP2 fallback if HELLO is unknown), then SELECT
 * the DB index. All memory (the handle, the receive buffer, the reply arena)
 * comes from `alloc` (NULL -> the process default). On success returns 0 and
 * *out; on failure -1 with a message in errbuf. The caller's dsn password
 * should be scrubbed by the caller after this returns (the connection retains
 * no plaintext credential).
 */
int hl_valkey_conn_open(HlValkeyConn **out, const HlValkeyDsn *dsn,
                        HlAllocator *alloc, char *errbuf, size_t errlen);

/*
 * Run the handshake over an ALREADY-CONNECTED plaintext fd (takes ownership of
 * fd). For tests over a socketpair; no TLS. Same return contract as open().
 */
int hl_valkey_conn_start(HlValkeyConn **out, int fd, const HlValkeyDsn *dsn,
                         HlAllocator *alloc, char *errbuf, size_t errlen);

/* The allocator this connection was opened with (for handles built over it). */
HlAllocator *hl_valkey_conn_alloc(HlValkeyConn *c);

/*
 * Send a fully-encoded command (a RESP array of bulk strings, from
 * hl_resp_cmd_*) and read exactly one reply, decoded into *out. STRING/ERROR
 * bytes in *out BORROW into the connection's receive buffer and aggregate items
 * into its reply arena; both are valid ONLY until the next command on this
 * connection. Returns 0 on a decoded reply (including a server -ERR, surfaced as
 * an HL_RESP_ERR value), -1 on a transport / protocol failure.
 */
int hl_valkey_command(HlValkeyConn *c, const HlRespWriter *cmd, HlRespValue *out);

/* Borrowed last-error string (valid until the next call on c). Never NULL. */
const char *hl_valkey_conn_error(HlValkeyConn *c);

/* Whether RESP3 was negotiated (HELLO 3 accepted). */
int hl_valkey_conn_is_resp3(HlValkeyConn *c);

/* Close the socket/TLS, free buffers, scrub residual bytes. NULL-safe. */
void hl_valkey_conn_close(HlValkeyConn *c);

#endif /* HL_CAP_VALKEY_CONN_H */
