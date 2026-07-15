/*
 * cap/mysql_conn.h: MySQL / MariaDB connection (DSN parse now; socket + auth
 * in later phases)
 *
 * Phase 1b exposes only the pure DSN parser (fuzzable, no socket). The
 * handshake, auth plugins, TLS, and query protocols land in later phases and
 * are added to cap/mysql_conn.c behind guards so this parser stays linkable
 * standalone by the fuzzers.
 *
 * The password is secret material: callers scrub the HlMyDsn after connecting.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_MYSQL_CONN_H
#define HL_CAP_MYSQL_CONN_H

#include <stddef.h>
#include <stdint.h>

/* DSN field caps (named so no bare buffer sizes leak into the struct/parser). */
#define HL_MY_DSN_HOST_MAX      256
#define HL_MY_DSN_PORT_MAX      8
#define HL_MY_DSN_USER_MAX      128
#define HL_MY_DSN_PASSWORD_MAX  256
#define HL_MY_DSN_DBNAME_MAX    128
#define HL_MY_DSN_SSLMODE_MAX   16

typedef struct HlMyDsn {
    char host[HL_MY_DSN_HOST_MAX];
    char port[HL_MY_DSN_PORT_MAX];         /* numeric string; default "3306" */
    char user[HL_MY_DSN_USER_MAX];
    char password[HL_MY_DSN_PASSWORD_MAX]; /* secret: scrub after use */
    char dbname[HL_MY_DSN_DBNAME_MAX];
    char sslmode[HL_MY_DSN_SSLMODE_MAX];   /* parsed now, enforced in the TLS phase */
} HlMyDsn;

/*
 * Parse a `mysql://` / `mariadb://` DSN into @p out. Percent-decodes user /
 * password / host / dbname; validates the port is numeric and bounded. Returns
 * 0 on success, -1 with a message in @p errbuf on any malformed / oversized
 * field. Every populated field is a NUL-terminated string on success.
 */
int hl_my_dsn_parse(const char *dsn, HlMyDsn *out, char *errbuf, size_t errlen);

/* Zero the password field (volatile) after the connection is established. */
void hl_my_dsn_scrub(HlMyDsn *dsn);

/* ── Auth plugins (crypto; compiled out under HL_MY_NO_AUTH) ───────── */
#ifndef HL_MY_NO_AUTH
#include "hull/cap/mysqlwire.h"   /* HL_MY_SCRAMBLE_LEN + wire helpers */
/*
 * mysql_native_password auth response:
 *   SHA1(pw) XOR SHA1( scramble || SHA1(SHA1(pw)) )
 * Returns the response length written (0 for an empty password,
 * HL_MY_SCRAMBLE_LEN otherwise) or -1 on a crypto error. The pure fuzzers
 * define HL_MY_NO_AUTH to stay free of the cap/crypto (mbedTLS) dependency.
 */
int hl_my_native_password_scramble(const char *password,
                                   const uint8_t scramble[HL_MY_SCRAMBLE_LEN],
                                   uint8_t out[HL_MY_SCRAMBLE_LEN]);

/* ── Connection (socket + handshake; Phase 2b) ────────────────────── */

#define HL_MY_ERRMSG_SIZE    256    /* connection error-message buffer */
#define HL_MY_RECV_BUF_INIT  8192   /* initial receive-buffer capacity */

/* An open connection, authenticated + idle once hl_my_conn_open/start returns
 * 0. Owns fd (closed by hl_my_conn_close). */
typedef struct HlMyConn {
    int      fd;
    uint8_t *rbuf;          /* receive accumulation buffer */
    size_t   rlen;          /* valid bytes in rbuf */
    size_t   rcap;          /* rbuf capacity */
    size_t   consumed;      /* bytes of the last-returned frame, compacted next */
    uint32_t capabilities;  /* client capability flags used at handshake */
    uint8_t  seq;           /* next packet sequence to send in a command */
    uint64_t last_insert_id;/* from the most recent OK packet */
    char     errmsg[HL_MY_ERRMSG_SIZE];
} HlMyConn;

/*
 * Connect to dsn->host:port and run the handshake (mysql_native_password +
 * AuthSwitch to native). On success the connection is authenticated and idle
 * (0). On failure returns -1 with conn->errmsg set and the socket closed.
 * caching_sha2_password / ed25519 land in the TLS/auth phase (§2.10 Phase 5).
 */
int  hl_my_conn_open(HlMyConn *conn, const HlMyDsn *dsn, int timeout_ms);

/*
 * Run the handshake over an already-connected, blocking socket fd (used by
 * hl_my_conn_open and by tests via socketpair). Takes ownership of fd: closed
 * by hl_my_conn_close, or immediately on handshake failure.
 */
int  hl_my_conn_start(HlMyConn *conn, int fd, const HlMyDsn *dsn);

void hl_my_conn_close(HlMyConn *conn);

/* ── Queries (COM_QUERY text protocol; no params) ─────────────────── */

/* A result column, passed to the description callback (fired once before rows).
 * @p name is valid only for the duration of the callback. */
typedef struct HlMyField {
    const char *name;
    uint8_t     type;   /* HL_MY_TYPE_* */
} HlMyField;

typedef void (*HlMyDescCb)(void *ctx, const HlMyField *fields, int nfields);

/* Row callback. values[i] is NULL for SQL NULL, else lengths[i] text bytes (not
 * NUL-terminated), valid only for this call. Return non-zero to stop early. */
typedef int (*HlMyRowCb)(void *ctx, const char *const *values,
                         const size_t *lengths, int ncols);

/*
 * Run one text-protocol query (COM_QUERY). The SQL is sent literally (no
 * parameter binding), so callers must pass only trusted / already-safe SQL;
 * parameterized queries use the binary prepared-statement protocol (Phase 3).
 * desc_cb / row_cb may be NULL for a statement with no result set; on an OK
 * response the affected row count is stored in *affected (may be NULL).
 * Returns 0 / -1 with conn->errmsg set.
 */
int hl_my_conn_query(HlMyConn *conn, const char *sql,
                     HlMyDescCb desc_cb, HlMyRowCb row_cb, void *cb_ctx,
                     int64_t *affected);

/* ── Prepared statements (binary protocol; parameterized) ─────────── */

/* One bound parameter. @p type is the MySQL wire type (HL_MY_TYPE_*); Hull
 * emits LONGLONG (integer), DOUBLE, VAR_STRING (text), BLOB, or NULL. String /
 * blob values borrow @p v.s.ptr for the length of the call. */
typedef struct HlMyParam {
    uint8_t type;
    int     is_null;              /* 1 = SQL NULL (type/value ignored) */
    union {
        int64_t i;               /* integer types */
        double  d;               /* FLOAT / DOUBLE */
        struct { const char *ptr; size_t len; } s;   /* string / blob */
    } v;
} HlMyParam;

/* A decoded binary result value handed to the binary row callback. */
typedef enum HlMyValKind {
    HL_MY_VAL_NULL = 0,
    HL_MY_VAL_INT,               /* signed integer types */
    HL_MY_VAL_DOUBLE,            /* FLOAT / DOUBLE */
    HL_MY_VAL_STR,               /* string / blob / decimal / date (borrowed) */
} HlMyValKind;

typedef struct HlMyVal {
    HlMyValKind kind;
    union {
        int64_t i;
        double  d;
        struct { const char *ptr; size_t len; } s;   /* borrowed for the call */
    } v;
    /* Backing store for values formatted on the fly (binary temporal types),
     * so v.s can point at a stable buffer for the callback's duration. */
    char tbuf[40];
} HlMyVal;

/* Binary row callback: @p vals is @p ncols decoded values, valid only for this
 * call. Return non-zero to stop early (the connection still drains to EOF). */
typedef int (*HlMyBinRowCb)(void *ctx, const HlMyVal *vals, int ncols);

/*
 * Prepare @p sql (COM_STMT_PREPARE), bind @p params (COM_STMT_EXECUTE, binary
 * protocol), stream the binary result rows, and close the statement
 * (COM_STMT_CLOSE). This is the parameterized path (no SQL injection: values
 * never touch the SQL text). @p desc_cb / @p row_cb may be NULL for a
 * no-result statement; on a plain OK the affected count is stored in *affected
 * (may be NULL). Returns 0 / -1 with conn->errmsg set.
 */
int hl_my_conn_query_prepared(HlMyConn *conn, const char *sql,
                              const HlMyParam *params, int nparams,
                              HlMyDescCb desc_cb, HlMyBinRowCb row_cb,
                              void *cb_ctx, int64_t *affected);

/*
 * Run a possibly multi-statement script as one COM_QUERY (the connection
 * advertises CLIENT_MULTI_STATEMENTS at handshake), draining every result set
 * and stopping at the first error. Result rows are discarded: this is the
 * migration / DDL path. Returns 0 / -1 with conn->errmsg set.
 */
int hl_my_conn_exec_multi(HlMyConn *conn, const char *sql);
#endif

#endif /* HL_CAP_MYSQL_CONN_H */
