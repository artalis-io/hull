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
    struct HlTlsClient *tls;  /* non-NULL once the TLS handshake completes */
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

/* ── TLS negotiation (SSLRequest / sslmode) ───────────────────────── */

/* DSN sslmode policy. Phase 3b.1 implements the SSLRequest negotiation and
 * the fallback decision; the mbedTLS handshake itself (for USE_TLS) lands in
 * Phase 3b.2 together with linking Keel + mbedTLS under HL_ENABLE_POSTGRES. */
typedef enum HlPgSslMode {
    HL_PG_SSLMODE_DISABLE = 0,  /* never attempt TLS (no SSLRequest)        */
    HL_PG_SSLMODE_PREFER  = 1,  /* try TLS, fall back to plaintext on 'N'   */
    HL_PG_SSLMODE_REQUIRE = 2,  /* TLS required; 'N' is a hard error        */
    HL_PG_SSLMODE_VERIFY  = 3,  /* require + verify the cert chain/hostname */
} HlPgSslMode;

typedef enum HlPgSslDecision {
    HL_PG_SSL_PLAINTEXT = 0,    /* proceed without TLS                      */
    HL_PG_SSL_USE_TLS   = 1,    /* server accepted; perform the handshake   */
    HL_PG_SSL_FAIL      = -1,   /* negotiation failed (errbuf set)          */
} HlPgSslDecision;

/* Map a DSN sslmode string to the enum. Empty defaults to DISABLE for now
 * (3b.2 will move the default to PREFER once the handshake exists). Returns
 * -1 for an unknown mode. */
int hl_pg_sslmode_parse(const char *s);

/*
 * Send an SSLRequest on @p fd and read the one-byte server reply, then apply
 * the @p mode policy: DISABLE returns PLAINTEXT without sending anything; a
 * server 'S' returns USE_TLS; a server 'N' returns PLAINTEXT under PREFER and
 * FAIL under REQUIRE/VERIFY. Exposed for tests (socketpair-drivable).
 */
HlPgSslDecision hl_pg_ssl_negotiate(int fd, HlPgSslMode mode,
                                    char *errbuf, size_t errlen);

/* ── SCRAM-SHA-256 primitives (exposed for tests) ─────────────────── */

/* Standard base64 (with '=' padding). Encode returns the encoded length and
 * NUL-terminates; both return -1 on overflow / malformed input. */
int hl_pg_b64_encode(const uint8_t *in, size_t inlen, char *out, size_t outsize);
int hl_pg_b64_decode(const char *in, size_t inlen,
                     uint8_t *out, size_t outsize, size_t *outlen);

/*
 * Compute the SCRAM-SHA-256 ClientProof and ServerSignature (RFC 5802 /
 * RFC 7677) from the password, salt, iteration count, and the full
 * AuthMessage (client-first-bare "," server-first "," client-final-no-proof).
 * Both outputs are 32 bytes. Reuses cap/crypto (PBKDF2 / HMAC-SHA256 /
 * SHA-256). Returns 0 on success, -1 on internal failure.
 */
int hl_pg_scram_proof(const char *password, size_t pw_len,
                      const uint8_t *salt, size_t salt_len, int iterations,
                      const char *auth_msg, size_t auth_msg_len,
                      uint8_t client_proof[32], uint8_t server_sig[32]);

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
