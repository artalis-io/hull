/*
 * cap/pg_conn.h: PostgreSQL connection, DSN parsing, and startup handshake
 *
 * Layer above the pgwire codec: parses a postgres:// DSN, opens a blocking
 * TCP connection, and runs the v3 startup handshake (Phase 2: plaintext
 * transport with trust / cleartext-password auth). TLS and SCRAM/md5 auth
 * arrive in Phase 3, slotting in the same way SMTP layers KlTls over its fd.
 *
 * The receive path drives the untrusted-input pgwire reader; the DSN parser
 * copies into fixed, bounded fields and rejects anything oversized. The DSN
 * password is secret material: callers scrub the HlPgDsn after connecting.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_PG_CONN_H
#define HL_CAP_PG_CONN_H

#include <stddef.h>
#include <stdint.h>

/* Parsed DSN. Fixed fields keep the parser allocation-free and bound every
 * copy; an oversized component is a hard error, never a silent truncation. */
typedef struct HlPgDsn {
    char host[256];
    char port[16];
    char user[128];
    char password[256];   /* secret: scrub after use */
    char dbname[128];
    char sslmode[16];     /* parsed now, enforced in Phase 3 */
} HlPgDsn;

/*
 * Parse a "postgres://" / "postgresql://" DSN of the form
 *   postgres://[user[:password]@]host[:port][/dbname][?k=v&...]
 * Percent-encoded octets in the userinfo, host, and dbname are decoded.
 * Returns 0 on success, -1 on failure with a message in errbuf (which may
 * be NULL). Never allocates.
 */
int hl_pg_dsn_parse(const char *dsn, HlPgDsn *out, char *errbuf, size_t errlen);

/* Overwrite the password field with zeros (defense in depth). */
void hl_pg_dsn_scrub(HlPgDsn *dsn);

/* An open connection, ready for queries once hl_pg_conn_open returns 0. */
typedef struct HlPgConn {
    int      fd;
    uint8_t *rbuf;        /* receive accumulation buffer                */
    size_t   rlen;        /* valid bytes in rbuf                        */
    size_t   rcap;        /* rbuf capacity                              */
    size_t   consumed;    /* bytes of the last-returned frame, compacted
                           * at the start of the next recv             */
    int32_t  backend_pid; /* BackendKeyData, for a future CancelRequest */
    int32_t  backend_key;
    int      tx_status;   /* latest ReadyForQuery status: 'I' / 'T' / 'E' */
    char     errmsg[256];
} HlPgConn;

/*
 * Connect to dsn->host:port and run the startup handshake. On success the
 * connection is authenticated and idle (ReadyForQuery seen) and 0 is
 * returned. On failure returns -1 with conn->errmsg set and any socket
 * closed. timeout_ms bounds the TCP connect.
 */
int  hl_pg_conn_open(HlPgConn *conn, const HlPgDsn *dsn, int timeout_ms);

/*
 * Run the handshake over an already-connected, blocking socket fd (used by
 * hl_pg_conn_open and by tests via socketpair). Takes ownership of fd: it is
 * closed by hl_pg_conn_close, or immediately on handshake failure.
 */
int  hl_pg_conn_start(HlPgConn *conn, int fd, const HlPgDsn *dsn);

/* Send Terminate (best effort) and release all resources. Idempotent. */
void hl_pg_conn_close(HlPgConn *conn);

/* ── Placeholder rewriting ────────────────────────────────────────── */

/*
 * Rewrite SQLite-style '?' placeholders to PostgreSQL $1, $2, ..., skipping
 * any '?' inside single-quoted strings, double-quoted identifiers,
 * dollar-quoted strings, and -- line / block comments. Writes a
 * NUL-terminated result into out (size outsize) and sets *nparams to the
 * placeholder count. Returns 0, or -1 if the result does not fit. Pure and
 * bounded; safe to fuzz.
 */
int hl_pg_rewrite_sql(const char *sql, char *out, size_t outsize, int *nparams);

/* ── Parameterized query execution (extended protocol) ────────────── */

/* A text-format bind parameter; text == NULL means SQL NULL. */
typedef struct HlPgParam {
    const char *text;
    int32_t     len;
} HlPgParam;

/* A result column descriptor from RowDescription. */
typedef struct HlPgField {
    const char *name;   /* valid only for the duration of the desc callback */
    int32_t     oid;    /* PostgreSQL type OID */
} HlPgField;

/* RowDescription callback: fired once, before any rows. */
typedef void (*HlPgDescCb)(void *ctx, const HlPgField *fields, int nfields);

/*
 * DataRow callback. values[i] is NULL for SQL NULL, otherwise a pointer to
 * lengths[i] text-format bytes (not NUL-terminated). The pointers are valid
 * only for the duration of the call; copy anything you need to retain.
 * Return 0 to continue, non-zero to stop consuming rows.
 */
typedef int (*HlPgRowCb)(void *ctx, const char *const *values,
                         const int32_t *lengths, int ncols);

/*
 * Run one parameterized statement over the extended protocol
 * (Parse / Bind / Describe / Execute / Sync). @p sql uses '?' placeholders,
 * rewritten internally; @p params are bound in text format, so SQL injection
 * is impossible (values never touch the SQL text). desc_cb / row_cb may be
 * NULL for statements with no result set. On CommandComplete the affected
 * row count, when the tag carries one, is stored in *affected (may be NULL).
 * Returns 0 on success, -1 on error with conn->errmsg set.
 */
int hl_pg_query(HlPgConn *conn, const char *sql,
                const HlPgParam *params, int nparams,
                HlPgDescCb desc_cb, HlPgRowCb row_cb, void *cb_ctx,
                int64_t *affected);

#endif /* HL_CAP_PG_CONN_H */
