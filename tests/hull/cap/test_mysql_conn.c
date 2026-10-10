/*
 * test_mysql_conn.c: MySQL/MariaDB handshake over a socketpair.
 *
 * Drives the real hl_my_conn_start receive/frame/auth loop against canned
 * server bytes queued on one end of a socketpair, with no MySQL server.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/mysql_conn.h"
#include "hull/cap/mysqlwire.h"
/* The backend itself, for its static transaction bookkeeping (the build
 * leaves cap_db_mysql.o out of this test's link). */
#include "../../../src/hull/cap/db_mysql.c"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Canned server packets ────────────────────────────────────────── */

static void build_handshake_caps(HlMyWriter *w, uint8_t seq, const char *plugin,
                                 uint32_t extra_caps)
{
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_u8(w, 10);                       /* protocol v10 */
    hl_my_put_cstr(w, "8.0.35");               /* server version */
    hl_my_put_u32(w, 1);                       /* connection id */
    for (int i = 0; i < HL_MY_SCRAMBLE_PART1; i++)
        hl_my_put_u8(w, (uint8_t)(i + 1));     /* auth data part 1 */
    hl_my_put_u8(w, 0);                        /* filler */
    uint32_t caps = HL_MY_CLIENT_SECURE_CONNECTION | HL_MY_CLIENT_PLUGIN_AUTH
                  | HL_MY_CLIENT_PROTOCOL_41 | extra_caps;
    hl_my_put_u16(w, (uint16_t)(caps & 0xFFFF));
    hl_my_put_u8(w, HL_MY_DEFAULT_CHARSET);
    hl_my_put_u16(w, 2);                       /* status */
    hl_my_put_u16(w, (uint16_t)(caps >> 16));
    hl_my_put_u8(w, 21);                       /* auth-plugin-data length */
    for (int i = 0; i < HL_MY_HANDSHAKE_FILLER; i++) hl_my_put_u8(w, 0);
    for (int i = 0; i < HL_MY_SCRAMBLE_LEN - HL_MY_SCRAMBLE_PART1; i++)
        hl_my_put_u8(w, (uint8_t)(i + 9));     /* auth data part 2 */
    hl_my_put_u8(w, 0);                        /* part-2 NUL */
    hl_my_put_cstr(w, plugin);
    hl_my_packet_end(w, m);
}

static void build_handshake_plugin(HlMyWriter *w, uint8_t seq, const char *plugin)
{
    build_handshake_caps(w, seq, plugin, 0);
}

static void build_handshake(HlMyWriter *w, uint8_t seq)
{
    build_handshake_plugin(w, seq, "mysql_native_password");
}

static void build_ok(HlMyWriter *w, uint8_t seq)
{
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_u8(w, HL_MY_PKT_OK);
    hl_my_put_lenenc_int(w, 0);   /* affected rows */
    hl_my_put_lenenc_int(w, 0);   /* last insert id */
    hl_my_put_u16(w, 2);          /* status flags */
    hl_my_put_u16(w, 0);          /* warnings */
    hl_my_packet_end(w, m);
}

static void build_err(HlMyWriter *w, uint8_t seq, uint16_t code, const char *msg)
{
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_u8(w, HL_MY_PKT_ERR);
    hl_my_put_u16(w, code);
    hl_my_put_u8(w, '#');
    hl_my_put_bytes(w, "28000", 5);
    hl_my_put_bytes(w, msg, strlen(msg));
    hl_my_packet_end(w, m);
}

/* ── Handshake over a socketpair ──────────────────────────────────── */

UTEST(mysql_conn, handshake_native_ok)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter hs; hl_my_writer_init(&hs); build_handshake(&hs, 0);
    ASSERT_TRUE(write(sv[0], hs.buf, hs.len) == (ssize_t)hs.len);
    HlMyWriter ok; hl_my_writer_init(&ok); build_ok(&ok, 2);
    ASSERT_TRUE(write(sv[0], ok.buf, ok.len) == (ssize_t)ok.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://alice:pw@localhost/shop",
                                 &dsn, err, sizeof err));

    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));   /* authenticated */

    /* The client's HandshakeResponse41 reached the server end; check the user. */
    uint8_t got[512];
    ssize_t n = read(sv[0], got, sizeof got);
    ASSERT_TRUE(n > 4);
    HlMyFrame f; size_t consumed = 0;
    ASSERT_EQ(hl_my_frame_next(got, (size_t)n, &f, &consumed), HL_MY_OK);
    HlMyCursor c; hl_my_cursor_init(&c, &f);
    (void)hl_my_get_u32(&c);                          /* client caps */
    (void)hl_my_get_u32(&c);                          /* max packet */
    (void)hl_my_get_u8(&c);                           /* charset */
    (void)hl_my_get_bytes(&c, HL_MY_HANDSHAKE_RESERVED);
    ASSERT_STREQ(hl_my_get_cstr(&c), "alice");

    hl_my_conn_close(&conn);
    hl_my_writer_free(&hs); hl_my_writer_free(&ok);
    close(sv[0]);
}

UTEST(mysql_conn, handshake_auth_error)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter hs; hl_my_writer_init(&hs); build_handshake(&hs, 0);
    ASSERT_TRUE(write(sv[0], hs.buf, hs.len) == (ssize_t)hs.len);
    HlMyWriter er; hl_my_writer_init(&er);
    build_err(&er, 2, 1045, "Access denied for user 'bob'");
    ASSERT_TRUE(write(sv[0], er.buf, er.len) == (ssize_t)er.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://bob:bad@localhost/db",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(-1, hl_my_conn_start(&conn, sv[1], &dsn));
    ASSERT_TRUE(strstr(conn.errmsg, "Access denied") != NULL);

    hl_my_writer_free(&hs); hl_my_writer_free(&er);
    close(sv[0]);   /* sv[1] already closed by the failed handshake */
}

/* ── TLS negotiation: fail closed, no silent downgrade ─────────────────
 * The default server handshake (build_handshake) advertises no CLIENT_SSL.
 * sslmode=prefer (handshake_native_ok) legitimately falls back to plaintext;
 * sslmode=require / verify-* MUST refuse rather than continue unencrypted, and an
 * unknown sslmode is rejected before any credential reaches the wire. Each case
 * fails at the sslmode gate, before the client sends its HandshakeResponse41, so
 * no OK/ERR reply needs to be queued.
 *
 * Drive hl_my_conn_start against the no-CLIENT_SSL handshake with @p dsn_str;
 * return its rc and copy the resulting errmsg into @p out. */
static int drive_sslmode(const char *dsn_str, char *out, size_t outsz)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -2;

    HlMyWriter hs; hl_my_writer_init(&hs); build_handshake(&hs, 0);
    ssize_t w = write(sv[0], hs.buf, hs.len); (void)w;

    HlMyDsn dsn; char err[128];
    if (hl_my_dsn_parse(dsn_str, &dsn, err, sizeof err) != 0) {
        hl_my_writer_free(&hs); close(sv[0]); close(sv[1]); return -3;
    }

    HlMyConn conn;
    int rc = hl_my_conn_start(&conn, sv[1], &dsn);
    if (out && outsz) { strncpy(out, conn.errmsg, outsz - 1); out[outsz - 1] = 0; }
    if (rc == 0) hl_my_conn_close(&conn);   /* not expected; retire the transport */

    hl_my_writer_free(&hs);
    close(sv[0]);   /* sv[1] is closed by the failed start */
    return rc;
}

/* Bytes that follow the greeting before TLS (an on-path attacker's forged OK
 * in the same segment) are refused, not parsed later as if they came over
 * TLS (audit 4 H1). */
UTEST(mysql_conn, tls_refuses_bytes_buffered_before_handshake)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    HlMyWriter hs; hl_my_writer_init(&hs);
    build_handshake_caps(&hs, 0, "mysql_native_password", HL_MY_CLIENT_SSL);
    build_ok(&hs, 2);                         /* the forged OK, same write */
    ssize_t w = write(sv[0], hs.buf, hs.len); (void)w;

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://a:pw@localhost/db?sslmode=require",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    int rc = hl_my_conn_start(&conn, sv[1], &dsn);
    char e[256];
    snprintf(e, sizeof e, "%s", conn.errmsg);
    if (rc == 0) hl_my_conn_close(&conn);
    hl_my_writer_free(&hs);
    close(sv[0]);
    ASSERT_EQ(-1, rc);
    ASSERT_TRUE(strstr(e, "before the TLS handshake") != NULL);
}

UTEST(mysql_conn, tls_require_no_downgrade)
{
    char e[128] = {0};
    ASSERT_EQ(-1, drive_sslmode("mysql://a:pw@localhost/db?sslmode=require",
                                e, sizeof e));
    ASSERT_TRUE(strstr(e, "server does not support TLS") != NULL);
}

UTEST(mysql_conn, tls_verify_no_downgrade)
{
    char e[128] = {0};
    ASSERT_EQ(-1, drive_sslmode("mysql://a:pw@localhost/db?sslmode=verify-full",
                                e, sizeof e));
    ASSERT_TRUE(strstr(e, "server does not support TLS") != NULL);
}

UTEST(mysql_conn, sslmode_unknown_rejected)
{
    char e[128] = {0};
    ASSERT_EQ(-1, drive_sslmode("mysql://a:pw@localhost/db?sslmode=bogus",
                                e, sizeof e));
    ASSERT_TRUE(strstr(e, "unknown sslmode") != NULL);
}

/* caching_sha2_password fast path: server names the plugin, then sends an
 * AuthMoreData(fast_success) followed by OK. Asserts the client selected
 * caching_sha2 (32-byte auth response + plugin name in its response). */
UTEST(mysql_conn, caching_sha2_fast_auth)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake_plugin(&s, 0, "caching_sha2_password");
    { size_t m = hl_my_packet_begin(&s, 2);
      hl_my_put_u8(&s, HL_MY_AUTH_MORE_DATA);
      hl_my_put_u8(&s, HL_MY_CACHING_SHA2_FAST_SUCCESS);
      hl_my_packet_end(&s, m); }
    build_ok(&s, 3);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://alice:pw@localhost/shop",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));   /* authenticated */

    /* Inspect the client's HandshakeResponse41: plugin + 32-byte auth resp. */
    uint8_t got[512];
    ssize_t n = read(sv[0], got, sizeof got);
    ASSERT_TRUE(n > 4);
    HlMyFrame f; size_t consumed = 0;
    ASSERT_EQ(hl_my_frame_next(got, (size_t)n, &f, &consumed), HL_MY_OK);
    HlMyCursor c; hl_my_cursor_init(&c, &f);
    (void)hl_my_get_u32(&c);                          /* client caps */
    (void)hl_my_get_u32(&c);                          /* max packet */
    (void)hl_my_get_u8(&c);                           /* charset */
    (void)hl_my_get_bytes(&c, HL_MY_HANDSHAKE_RESERVED);
    ASSERT_STREQ(hl_my_get_cstr(&c), "alice");
    ASSERT_EQ(hl_my_get_u8(&c), HL_MY_CACHING_SHA2_DIGEST_LEN);  /* 32-byte resp */
    (void)hl_my_get_bytes(&c, HL_MY_CACHING_SHA2_DIGEST_LEN);
    ASSERT_STREQ(hl_my_get_cstr(&c), "shop");                    /* database */
    ASSERT_STREQ(hl_my_get_cstr(&c), "caching_sha2_password");   /* plugin */

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* An AuthSwitchRequest to client_ed25519 (MariaDB) is rejected with a hint
 * pointing at a supported plugin (ed25519 is deferred). */
UTEST(mysql_conn, ed25519_unsupported)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);                            /* offers native */
    /* AuthSwitchRequest: 0xFE, plugin name, 32-byte scramble */
    { size_t m = hl_my_packet_begin(&s, 2);
      hl_my_put_u8(&s, HL_MY_PKT_EOF);
      hl_my_put_cstr(&s, "client_ed25519");
      for (int i = 0; i < 32; i++) hl_my_put_u8(&s, (uint8_t)i);
      hl_my_packet_end(&s, m); }
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(-1, hl_my_conn_start(&conn, sv[1], &dsn));
    ASSERT_TRUE(strstr(conn.errmsg, "ed25519") != NULL);

    hl_my_writer_free(&s);
    close(sv[0]);   /* sv[1] closed by the failed handshake */
}

/* ── COM_QUERY result set over a socketpair ───────────────────────── */

typedef struct { int rows; int ncols; char first[64]; char col0[64]; } QCollect;

static void qdesc(void *ctx, const HlMyField *fields, int nf)
{
    QCollect *g = ctx;
    g->ncols = nf;
    if (nf >= 1 && fields[0].name)
        snprintf(g->col0, sizeof g->col0, "%s", fields[0].name);
}

static int qrow(void *ctx, const char *const *vals, const size_t *lens, int nc)
{
    QCollect *g = ctx;
    g->rows++;
    if (nc >= 1 && vals[0]) {
        size_t n = lens[0] < sizeof g->first - 1 ? lens[0] : sizeof g->first - 1;
        memcpy(g->first, vals[0], n);
        g->first[n] = '\0';
    }
    return 0;
}

static void put_eof(HlMyWriter *w, uint8_t seq)
{
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_u8(w, HL_MY_PKT_EOF);
    hl_my_put_u16(w, 0);   /* warnings */
    hl_my_put_u16(w, 0);   /* status */
    hl_my_packet_end(w, m);
}

static void put_col_def_flags(HlMyWriter *w, uint8_t seq, const char *name,
                              uint8_t type, uint16_t flags);

static void put_col_def(HlMyWriter *w, uint8_t seq, const char *name, uint8_t type)
{
    put_col_def_flags(w, seq, name, type, 0);
}

static void put_col_def_flags(HlMyWriter *w, uint8_t seq, const char *name,
                              uint8_t type, uint16_t flags)
{
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_lenenc_str(w, "def", 3);
    hl_my_put_lenenc_str(w, "", 0);
    hl_my_put_lenenc_str(w, "", 0);
    hl_my_put_lenenc_str(w, "", 0);
    hl_my_put_lenenc_str(w, name, strlen(name));
    hl_my_put_lenenc_str(w, name, strlen(name));
    hl_my_put_lenenc_int(w, 0x0c);
    hl_my_put_u16(w, 63);                              /* charset */
    hl_my_put_u32(w, 11);                              /* column length */
    hl_my_put_u8(w, type);
    hl_my_put_u16(w, flags);                           /* flags */
    hl_my_put_u8(w, 0);                                /* decimals */
    hl_my_put_u16(w, 0);                               /* filler */
    hl_my_packet_end(w, m);
}

UTEST(mysql_conn, com_query_result_set)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);                                   /* auth OK */

    /* column count = 1 */
    { size_t m = hl_my_packet_begin(&s, 1);
      hl_my_put_lenenc_int(&s, 1);
      hl_my_packet_end(&s, m); }
    /* ColumnDefinition41 for "id" (LONG) */
    { size_t m = hl_my_packet_begin(&s, 2);
      hl_my_put_lenenc_str(&s, "def", 3);
      hl_my_put_lenenc_str(&s, "", 0);
      hl_my_put_lenenc_str(&s, "", 0);
      hl_my_put_lenenc_str(&s, "", 0);
      hl_my_put_lenenc_str(&s, "id", 2);
      hl_my_put_lenenc_str(&s, "id", 2);
      hl_my_put_lenenc_int(&s, 0x0c);
      hl_my_put_u16(&s, 63);                           /* charset */
      hl_my_put_u32(&s, 11);                           /* column length */
      hl_my_put_u8(&s, HL_MY_TYPE_LONG);
      hl_my_put_u16(&s, 0);                            /* flags */
      hl_my_put_u8(&s, 0);                             /* decimals */
      hl_my_put_u16(&s, 0);                            /* filler */
      hl_my_packet_end(&s, m); }
    put_eof(&s, 3);                                    /* end of column defs */
    /* one row: "42" */
    { size_t m = hl_my_packet_begin(&s, 4);
      hl_my_put_lenenc_str(&s, "42", 2);
      hl_my_packet_end(&s, m); }
    put_eof(&s, 5);                                    /* end of rows */

    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    QCollect got; memset(&got, 0, sizeof got);
    ASSERT_EQ(0, hl_my_conn_query(&conn, "SELECT id", qdesc, qrow, &got, NULL));
    ASSERT_EQ(got.ncols, 1);
    ASSERT_EQ(got.rows, 1);
    ASSERT_STREQ(got.col0, "id");
    ASSERT_STREQ(got.first, "42");

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* ── Prepared statement (binary protocol) over a socketpair ───────── */

typedef struct {
    int rows; int ncols; int64_t first_int; char first_str[64]; char col0[64];
} BCollect;

static void bdesc(void *ctx, const HlMyField *fields, int nf)
{
    BCollect *g = ctx;
    g->ncols = nf;
    if (nf >= 1 && fields[0].name)
        snprintf(g->col0, sizeof g->col0, "%s", fields[0].name);
}

static int brow(void *ctx, const HlMyVal *vals, int nc)
{
    BCollect *g = ctx;
    g->rows++;
    if (nc >= 1 && vals[0].kind == HL_MY_VAL_INT)
        g->first_int = vals[0].v.i;
    if (nc >= 1 && vals[0].kind == HL_MY_VAL_STR) {
        size_t n = vals[0].v.s.len < sizeof g->first_str - 1
                   ? vals[0].v.s.len : sizeof g->first_str - 1;
        memcpy(g->first_str, vals[0].v.s.ptr, n);
        g->first_str[n] = '\0';
    }
    return 0;
}

UTEST(mysql_conn, prepared_statement)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);                                   /* auth OK */

    /* COM_STMT_PREPARE_OK: stmt 1, 1 column, 1 param */
    { size_t m = hl_my_packet_begin(&s, 1);
      hl_my_put_u8(&s, HL_MY_PKT_OK);
      hl_my_put_u32(&s, 1);                            /* statement_id */
      hl_my_put_u16(&s, 1);                            /* num_columns */
      hl_my_put_u16(&s, 1);                            /* num_params */
      hl_my_put_u8(&s, 0);                             /* filler */
      hl_my_put_u16(&s, 0);                            /* warnings */
      hl_my_packet_end(&s, m); }
    put_col_def(&s, 2, "?", HL_MY_TYPE_LONGLONG);      /* param def + EOF */
    put_eof(&s, 3);
    put_col_def(&s, 4, "id", HL_MY_TYPE_LONG);         /* column def + EOF */
    put_eof(&s, 5);

    /* COM_STMT_EXECUTE response: 1 column, one binary row (id = 42) */
    { size_t m = hl_my_packet_begin(&s, 1);
      hl_my_put_lenenc_int(&s, 1);
      hl_my_packet_end(&s, m); }
    put_col_def(&s, 2, "id", HL_MY_TYPE_LONG);
    put_eof(&s, 3);
    { size_t m = hl_my_packet_begin(&s, 4);
      hl_my_put_u8(&s, 0x00);                          /* binary row header */
      hl_my_put_u8(&s, 0x00);                          /* null bitmap (1 byte, none) */
      hl_my_put_u32(&s, 42);                           /* LONG value, LE */
      hl_my_packet_end(&s, m); }
    put_eof(&s, 5);

    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    HlMyParam p;
    memset(&p, 0, sizeof p);
    p.type = HL_MY_TYPE_LONGLONG;
    p.v.i = 7;

    BCollect got; memset(&got, 0, sizeof got);
    ASSERT_EQ(0, hl_my_conn_query_prepared(&conn, "SELECT id WHERE x = ?",
                                           &p, 1, bdesc, brow, &got, NULL));
    ASSERT_EQ(got.ncols, 1);
    ASSERT_EQ(got.rows, 1);
    ASSERT_STREQ(got.col0, "id");
    ASSERT_EQ((int)got.first_int, 42);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* Binary integer values of UNSIGNED columns are zero-extended (audit 9 M3:
 * TINYINT UNSIGNED 200 read as -56), a signed one is still sign-extended,
 * and a BIGINT UNSIGNED above INT64_MAX comes back as its decimal text. */
typedef struct { int n; HlMyVal v[8]; char s[8][32]; } UCollect;

static int urow(void *ctx, const HlMyVal *vals, int nc)
{
    UCollect *g = ctx;
    for (int i = 0; i < nc && i < 8; i++) {
        g->v[i] = vals[i];
        if (vals[i].kind == HL_MY_VAL_STR)
            snprintf(g->s[i], sizeof g->s[i], "%.*s", (int)vals[i].v.s.len,
                     vals[i].v.s.ptr);
    }
    g->n = nc;
    return 0;
}

UTEST(mysql_conn, prepared_unsigned_columns)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    { size_t m = hl_my_packet_begin(&s, 1);           /* PREPARE_OK: 6 cols */
      hl_my_put_u8(&s, HL_MY_PKT_OK);
      hl_my_put_u32(&s, 1);
      hl_my_put_u16(&s, 6);
      hl_my_put_u16(&s, 0);
      hl_my_put_u8(&s, 0);
      hl_my_put_u16(&s, 0);
      hl_my_packet_end(&s, m); }
    for (int i = 0; i < 6; i++) put_col_def(&s, (uint8_t)(2 + i), "c", HL_MY_TYPE_TINY);
    put_eof(&s, 8);

    { size_t m = hl_my_packet_begin(&s, 1);
      hl_my_put_lenenc_int(&s, 6);
      hl_my_packet_end(&s, m); }
    put_col_def_flags(&s, 2, "tu", HL_MY_TYPE_TINY, HL_MY_FLAG_UNSIGNED);
    put_col_def_flags(&s, 3, "ts", HL_MY_TYPE_TINY, 0);
    put_col_def_flags(&s, 4, "su", HL_MY_TYPE_SHORT, HL_MY_FLAG_UNSIGNED);
    put_col_def_flags(&s, 5, "lu", HL_MY_TYPE_LONG, HL_MY_FLAG_UNSIGNED);
    put_col_def_flags(&s, 6, "bu", HL_MY_TYPE_LONGLONG, HL_MY_FLAG_UNSIGNED);
    put_col_def_flags(&s, 7, "bs", HL_MY_TYPE_LONGLONG, HL_MY_FLAG_UNSIGNED);
    put_eof(&s, 8);
    { size_t m = hl_my_packet_begin(&s, 9);
      hl_my_put_u8(&s, 0x00);                          /* binary row header */
      hl_my_put_u8(&s, 0x00);                          /* null bitmap: 6+2 bits */
      hl_my_put_u8(&s, 200);                           /* tu */
      hl_my_put_u8(&s, 200);                           /* ts */
      hl_my_put_u16(&s, 65535);                        /* su */
      hl_my_put_u32(&s, 4000000000u);                  /* lu */
      hl_my_put_u32(&s, 0xFFFFFFFFu);                  /* bu = 2^64 - 1 */
      hl_my_put_u32(&s, 0xFFFFFFFFu);
      hl_my_put_u32(&s, 7);                            /* bs = 7 (fits) */
      hl_my_put_u32(&s, 0);
      hl_my_packet_end(&s, m); }
    put_eof(&s, 10);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    UCollect got; memset(&got, 0, sizeof got);
    ASSERT_EQ(0, hl_my_conn_query_prepared(&conn, "SELECT u", NULL, 0,
                                           NULL, urow, &got, NULL));
    ASSERT_EQ(6, got.n);
    EXPECT_EQ((int)HL_MY_VAL_INT, (int)got.v[0].kind);
    EXPECT_EQ(200, (int)got.v[0].v.i);
    EXPECT_EQ(-56, (int)got.v[1].v.i);
    EXPECT_EQ(65535, (int)got.v[2].v.i);
    EXPECT_EQ(4000000000LL, (long long)got.v[3].v.i);
    EXPECT_EQ((int)HL_MY_VAL_STR, (int)got.v[4].kind);
    EXPECT_STREQ("18446744073709551615", got.s[4]);
    EXPECT_EQ((int)HL_MY_VAL_INT, (int)got.v[5].kind);
    EXPECT_EQ(7, (int)got.v[5].v.i);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* Prepared SELECT returning a binary DATETIME, exercising temporal decode. */
UTEST(mysql_conn, prepared_datetime)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    /* PREPARE_OK: stmt 1, 1 column, 0 params */
    { size_t m = hl_my_packet_begin(&s, 1);
      hl_my_put_u8(&s, HL_MY_PKT_OK);
      hl_my_put_u32(&s, 1);
      hl_my_put_u16(&s, 1);                            /* num_columns */
      hl_my_put_u16(&s, 0);                            /* num_params */
      hl_my_put_u8(&s, 0);
      hl_my_put_u16(&s, 0);
      hl_my_packet_end(&s, m); }
    put_col_def(&s, 2, "ts", HL_MY_TYPE_DATETIME);     /* column def + EOF */
    put_eof(&s, 3);

    /* EXECUTE response: 1 column, one binary row (2024-01-15 12:30:45) */
    { size_t m = hl_my_packet_begin(&s, 1);
      hl_my_put_lenenc_int(&s, 1);
      hl_my_packet_end(&s, m); }
    put_col_def(&s, 2, "ts", HL_MY_TYPE_DATETIME);
    put_eof(&s, 3);
    { size_t m = hl_my_packet_begin(&s, 4);
      hl_my_put_u8(&s, 0x00);                          /* binary row header */
      hl_my_put_u8(&s, 0x00);                          /* null bitmap */
      hl_my_put_u8(&s, 7);                             /* temporal length */
      hl_my_put_u16(&s, 2024);                         /* year */
      hl_my_put_u8(&s, 1);                             /* month */
      hl_my_put_u8(&s, 15);                            /* day */
      hl_my_put_u8(&s, 12);                            /* hour */
      hl_my_put_u8(&s, 30);                            /* minute */
      hl_my_put_u8(&s, 45);                            /* second */
      hl_my_packet_end(&s, m); }
    put_eof(&s, 5);

    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    BCollect got; memset(&got, 0, sizeof got);
    ASSERT_EQ(0, hl_my_conn_query_prepared(&conn, "SELECT ts", NULL, 0,
                                           bdesc, brow, &got, NULL));
    ASSERT_EQ(got.rows, 1);
    ASSERT_STREQ(got.first_str, "2024-01-15 12:30:45");

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A params array shorter than the statement's parameter count (Lua drops
 * trailing nils) must pad the tail as NULL in COM_STMT_EXECUTE. Decodes the
 * client's execute packet and asserts the null bitmap + single bound value. */
UTEST(mysql_conn, prepared_trailing_null_pad)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    /* PREPARE_OK: stmt 1, 0 columns, 2 params */
    { size_t m = hl_my_packet_begin(&s, 1);
      hl_my_put_u8(&s, HL_MY_PKT_OK);
      hl_my_put_u32(&s, 1);
      hl_my_put_u16(&s, 0);                            /* num_columns */
      hl_my_put_u16(&s, 2);                            /* num_params */
      hl_my_put_u8(&s, 0);
      hl_my_put_u16(&s, 0);
      hl_my_packet_end(&s, m); }
    put_col_def(&s, 2, "p1", HL_MY_TYPE_LONGLONG);     /* 2 param defs + EOF */
    put_col_def(&s, 3, "p2", HL_MY_TYPE_LONGLONG);
    put_eof(&s, 4);
    build_ok(&s, 1);                                   /* execute response: OK */
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    /* Bind ONE param (42) for a two-param statement; slot 1 must become NULL. */
    HlMyParam p;
    memset(&p, 0, sizeof p);
    p.type = HL_MY_TYPE_LONGLONG;
    p.v.i = 42;
    ASSERT_EQ(0, hl_my_conn_query_prepared(&conn, "INSERT ... VALUES (?, ?)",
                                           &p, 1, NULL, NULL, NULL, NULL));

    /* The client's traffic on sv[0] is
     * [HandshakeResponse41][COM_STMT_PREPARE][COM_STMT_EXECUTE][CLOSE]; walk to
     * the third frame (EXECUTE). */
    uint8_t got[512];
    ssize_t n = read(sv[0], got, sizeof got);
    ASSERT_TRUE(n > 0);
    HlMyFrame f; size_t consumed = 0, off = 0;
    ASSERT_EQ(hl_my_frame_next(got, (size_t)n, &f, &consumed), HL_MY_OK);   /* handshake */
    off += consumed;
    ASSERT_EQ(hl_my_frame_next(got + off, (size_t)n - off, &f, &consumed), HL_MY_OK); /* PREPARE */
    off += consumed;
    ASSERT_EQ(hl_my_frame_next(got + off, (size_t)n - off, &f, &consumed), HL_MY_OK); /* EXECUTE */

    HlMyCursor c; hl_my_cursor_init(&c, &f);
    ASSERT_EQ(hl_my_get_u8(&c), HL_MY_COM_STMT_EXECUTE);
    (void)hl_my_get_u32(&c);                           /* statement id */
    (void)hl_my_get_u8(&c);                            /* flags */
    (void)hl_my_get_u32(&c);                           /* iteration count */
    ASSERT_EQ(hl_my_get_u8(&c), 0x02);                 /* null bitmap: slot 1 NULL */
    ASSERT_EQ(hl_my_get_u8(&c), 1);                    /* new-params-bound */
    ASSERT_EQ(hl_my_get_u8(&c), HL_MY_TYPE_LONGLONG);  /* param 0 type */
    (void)hl_my_get_u8(&c);
    ASSERT_EQ(hl_my_get_u8(&c), HL_MY_TYPE_NULL);      /* param 1 padded to NULL */
    (void)hl_my_get_u8(&c);
    ASSERT_EQ(hl_my_get_u64(&c), 42u);                 /* only the one value */
    ASSERT_FALSE(hl_my_cursor_err(&c));

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* ── Multi-statement script (exec_multi) over a socketpair ────────── */

static void put_ok_more(HlMyWriter *w, uint8_t seq, uint16_t status)
{
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_u8(w, HL_MY_PKT_OK);
    hl_my_put_lenenc_int(w, 1);       /* affected rows */
    hl_my_put_lenenc_int(w, 0);       /* last insert id */
    hl_my_put_u16(w, status);         /* status flags */
    hl_my_put_u16(w, 0);              /* warnings */
    hl_my_packet_end(w, m);
}

UTEST(mysql_conn, exec_multi_statement)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);                                   /* auth OK */
    /* Two statements: first OK carries MORE_RESULTS, second terminates. */
    put_ok_more(&s, 1, HL_MY_SERVER_MORE_RESULTS);
    put_ok_more(&s, 2, 0);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    ASSERT_EQ(0, hl_my_conn_exec_multi(&conn,
        "CREATE TABLE a (id INT); CREATE TABLE b (id INT)"));

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A multi-statement COM_QUERY whose SECOND statement fails: the ERR used to
 * be read past and the call reported success. It fails, and the connection
 * stays in step (the reply was read to its end). */
UTEST(mysql_conn, query_later_statement_error_fails)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);                                   /* auth OK */
    put_ok_more(&s, 1, HL_MY_SERVER_MORE_RESULTS);     /* statement 1: OK, more */
    build_err(&s, 2, 1146, "Table 'db.nope' doesn't exist");   /* statement 2 */
    put_ok_more(&s, 1, 0);                             /* the next command */
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    EXPECT_NE(0, hl_my_conn_query(&conn, "UPDATE a SET x=1; UPDATE nope SET x=1",
                                  NULL, NULL, NULL, NULL));
    EXPECT_TRUE(strstr(conn.errmsg, "nope") != NULL);
    EXPECT_EQ(0, conn.broken);
    /* still usable: the next reply is read normally */
    EXPECT_EQ(0, hl_my_conn_query(&conn, "SELECT 1", NULL, NULL, NULL, NULL));

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A server ERR records its code (the backend reads ER_LOCK_DEADLOCK from it to
 * learn that InnoDB rolled the whole transaction back), and a new command
 * clears it. */
UTEST(mysql_conn, query_error_records_server_code)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);                                   /* auth OK */
    build_err(&s, 1, HL_MY_ER_LOCK_DEADLOCK,
              "Deadlock found when trying to get lock");
    put_ok_more(&s, 1, 0);                             /* the next command */
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    EXPECT_NE(0, hl_my_conn_query(&conn, "UPDATE a SET x=1", NULL, NULL, NULL, NULL));
    EXPECT_EQ(HL_MY_ER_LOCK_DEADLOCK, (int)conn.last_err_code);
    EXPECT_EQ(0, conn.broken);
    EXPECT_EQ(0, hl_my_conn_query(&conn, "SELECT 1", NULL, NULL, NULL, NULL));
    EXPECT_EQ(0, (int)conn.last_err_code);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A deadlock in a LATER statement of a multi-statement COM_QUERY records its
 * code too (audit 6 L2): it is what tells the backend the whole transaction
 * was rolled back. */
UTEST(mysql_conn, later_statement_error_records_server_code)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);                                   /* auth OK */
    put_ok_more(&s, 1, HL_MY_SERVER_MORE_RESULTS |
                       HL_MY_SERVER_STATUS_IN_TRANS);  /* statement 1 */
    build_err(&s, 2, HL_MY_ER_LOCK_DEADLOCK,
              "Deadlock found when trying to get lock");   /* statement 2 */
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    EXPECT_NE(0, hl_my_conn_query(&conn, "UPDATE a SET x=1; UPDATE b SET x=1",
                                  NULL, NULL, NULL, NULL));
    EXPECT_EQ(HL_MY_ER_LOCK_DEADLOCK, (int)conn.last_err_code);
    EXPECT_EQ(0, conn.broken);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* An ERR carries no status flags, so after one the connection still reads
 * "in a transaction" from the last OK - though MySQL committed it before a
 * failing DDL statement. COM_PING learns the real status, and keeps the
 * failed statement's message and code (audit 6 L1). */
UTEST(mysql_conn, ping_refreshes_status_after_error)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);                                   /* auth OK */
    put_ok_more(&s, 1, HL_MY_SERVER_STATUS_IN_TRANS);  /* START TRANSACTION */
    build_err(&s, 1, 1050, "Table 't' already exists");   /* CREATE TABLE */
    put_ok_more(&s, 1, 0x0002);                        /* PING: autocommit */
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    ASSERT_EQ(0, hl_my_conn_query(&conn, "START TRANSACTION", NULL, NULL, NULL, NULL));
    EXPECT_TRUE(conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS);
    EXPECT_NE(0, hl_my_conn_query(&conn, "CREATE TABLE t (id INT)",
                                  NULL, NULL, NULL, NULL));
    EXPECT_TRUE(conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS);   /* stale */
    EXPECT_EQ(0, hl_my_conn_ping(&conn));
    EXPECT_FALSE(conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS);
    EXPECT_TRUE(strstr(conn.errmsg, "already exists") != NULL);
    EXPECT_EQ(1050, (int)conn.last_err_code);
    EXPECT_EQ(0, conn.broken);

    /* The PING went out as a one-byte COM_PING after the two queries. */
    uint8_t got[512];
    ssize_t n = read(sv[0], got, sizeof got);
    ASSERT_TRUE(n >= 5);
    EXPECT_EQ(HL_MY_COM_PING, got[n - 1]);
    EXPECT_EQ(1, got[n - 5]);                          /* payload length 1 */

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* caching_sha2 full authentication sends the password itself: never without
 * a verified TLS session. (Here there is no TLS at all; the unverified-TLS
 * case - sslmode prefer / require against a forged certificate - takes the
 * same branch, since only a verify-* handshake sets tls_verified.) */
UTEST(mysql_conn, caching_sha2_full_auth_needs_verified_tls)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake_plugin(&s, 0, "caching_sha2_password");
    { size_t m = hl_my_packet_begin(&s, 2);
      hl_my_put_u8(&s, HL_MY_AUTH_MORE_DATA);
      hl_my_put_u8(&s, HL_MY_CACHING_SHA2_FULL_AUTH);
      hl_my_packet_end(&s, m); }
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://alice:Sup3rSecretPw@localhost/shop",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(-1, hl_my_conn_start(&conn, sv[1], &dsn));
    EXPECT_TRUE(strstr(conn.errmsg, "verify-full") != NULL);

    /* Only the HandshakeResponse41 (a scramble, not the password) went out. */
    uint8_t got[1024];
    ssize_t n = read(sv[0], got, sizeof got);
    ASSERT_TRUE(n > 0);
    int leaked = 0;
    for (ssize_t i = 0; i + 13 <= n; i++)
        if (memcmp(got + i, "Sup3rSecretPw", 13) == 0) leaked = 1;
    EXPECT_EQ(0, leaked);

    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A single-statement script must stop after one result (no MORE_RESULTS). */
UTEST(mysql_conn, exec_multi_single)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    ASSERT_EQ(0, hl_my_conn_exec_multi(&conn, "CREATE TABLE a (id INT)"));

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* ── The backend (db_mysql.c, compiled into this test) ─────────────── */

/* Start a backend handle on a socketpair whose server side has @p srv
 * queued after the handshake. */
static int my_backend_start(HlDbHandle *h, HlDbMyCtx *ctx, int sv[2],
                            HlMyWriter *srv)
{
    memset(h, 0, sizeof *h);
    memset(ctx, 0, sizeof *ctx);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    if (write(sv[0], srv->buf, srv->len) != (ssize_t)srv->len) return -1;
    HlMyDsn dsn; char err[128];
    if (hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err) != 0)
        return -1;
    if (hl_my_conn_start(&ctx->conn, sv[1], &dsn) != 0) return -1;
    h->backend = &hl_db_backend_mysql;
    h->ctx = ctx;
    h->batch_depth = 1;   /* inside a db.batch */
    return 0;
}

/* A failed non-DDL statement after which the transaction is gone was a
 * server rollback (a lock-wait timeout under innodb_rollback_on_timeout),
 * not a DDL implicit commit: the resume opened a new transaction and the
 * batch committed what followed without what came before (audit 7 L1). */
UTEST(mysql_backend, rollback_under_a_failed_statement_is_not_resumed)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);                                   /* auth OK */
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    build_err(&s, 1, 1205, "Lock wait timeout exceeded");        /* UPDATE */
    put_ok_more(&s, 1, 0x0002);                        /* PING: autocommit */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));
    EXPECT_EQ(-1, mysql_exec(&h, "UPDATE a SET x = 1", NULL, 0));
    EXPECT_EQ(1, ctx.txn_aborted);
    EXPECT_TRUE(mysql_in_txn(&h));
    /* Refused without a round trip (nothing more is queued) */
    EXPECT_EQ(-1, mysql_exec(&h, "INSERT INTO a VALUES (1)", NULL, 0));
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "rolled this transaction back") != NULL);
    EXPECT_EQ(0, mysql_rollback(&h));
    EXPECT_EQ(0, ctx.txn_aborted);
    EXPECT_FALSE(mysql_in_txn(&h));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A failed DDL statement still committed the transaction first: that one
 * resumes, so the rest of the batch is transactional again. */
UTEST(mysql_backend, failed_ddl_commit_is_resumed)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    build_err(&s, 1, 1050, "Table 't' already exists");          /* CREATE */
    put_ok_more(&s, 1, 0x0002);                        /* PING: autocommit */
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* resume */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));
    EXPECT_EQ(-1, mysql_exec(&h, "/* c */ CREATE TABLE t (id INT)", NULL, 0));
    EXPECT_EQ(0, ctx.txn_aborted);
    EXPECT_TRUE(my_in_trans(&h));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* The implicit-commit classifier reads past the first keyword (audit 8
 * c_db L2): CREATE / DROP TEMPORARY TABLE and LOAD DATA commit nothing, so
 * a transaction gone after one failed was a server rollback. */
UTEST(mysql_backend, implicit_commit_classifier)
{
    static const char *const commits[] = {
        "CREATE TABLE t (id INT)", "create index i on t (x)",
        "CREATE DATABASE d", "CREATE USER u", "CREATE OR REPLACE VIEW v AS SELECT 1",
        "DROP TABLE t", "drop view v", "DROP USER u",
        "ALTER TABLE t ADD c INT", "ALTER USER u IDENTIFIED BY 'x'",
        "RENAME TABLE a TO b", "TRUNCATE TABLE t", "GRANT SELECT ON t TO u",
        "REVOKE SELECT ON t FROM u", "SET PASSWORD = 'x'",
        "LOCK TABLES t WRITE", "lock table t read", "UNLOCK TABLES",
        "ANALYZE TABLE t", "CHECK TABLE t", "OPTIMIZE TABLE t",
        "REPAIR TABLE t", "CACHE INDEX t IN c", "LOAD INDEX INTO CACHE t",
        "FLUSH PRIVILEGES", "RESET MASTER", "INSTALL PLUGIN p SONAME 'x.so'",
        "UNINSTALL PLUGIN p", "START REPLICA", "STOP SLAVE",
        "CHANGE MASTER TO MASTER_HOST = 'h'",
        "CHANGE REPLICATION SOURCE TO SOURCE_HOST = 'h'",
        "/* c */ create table t (id int)",
    };
    static const char *const no_commit[] = {
        "CREATE TEMPORARY TABLE snap SELECT * FROM accounts",
        "create /* c */ temporary table t (id int)",
        "DROP TEMPORARY TABLE IF EXISTS snap",
        "LOAD DATA INFILE 'x' INTO TABLE t", "LOAD XML INFILE 'x' INTO TABLE t",
        "RESET PERSIST", "RESET", "START GROUP_REPLICATION",
        "CHANGE REPLICATION FILTER REPLICATE_DO_DB = (d)",
        "SET autocommit = 0", "PURGE BINARY LOGS TO 'x'", "CHECKSUM TABLE t",
        "INSTALL COMPONENT 'file://c'", "LOCK INSTANCE FOR BACKUP",
        "UPDATE a SET x = 1", "INSERT INTO t VALUES (1)", "SELECT 1", "", NULL,
    };
    for (size_t i = 0; i < sizeof commits / sizeof commits[0]; i++)
        EXPECT_EQ_MSG(1, my_sql_commits_implicitly(commits[i]), commits[i]);
    for (size_t i = 0; i < sizeof no_commit / sizeof no_commit[0]; i++)
        EXPECT_EQ_MSG(0, my_sql_commits_implicitly(no_commit[i]),
                      no_commit[i] ? no_commit[i] : "(null)");
}

/* Text protocol: a BIGINT UNSIGNED above INT64_MAX keeps its text instead
 * of saturating to INT64_MAX (audit 9 M3). */
UTEST(mysql_backend, text_bigint_unsigned_out_of_range_is_text)
{
    HlValue v;
    decode_my_value(HL_MY_TYPE_LONGLONG, "18446744073709551615", 20, &v);
    EXPECT_EQ((int)HL_TYPE_TEXT, (int)v.type);
    EXPECT_EQ(20, (int)v.len);
    decode_my_value(HL_MY_TYPE_LONGLONG, "-5", 2, &v);
    EXPECT_EQ((int)HL_TYPE_INT, (int)v.type);
    EXPECT_EQ(-5, (int)v.i);
}

/* MySQL runs an executable comment (slash-star-bang) and skips a '#' comment,
 * where the shared reader does the opposite: such a statement is never read
 * as an implicit commit nor as a plain statement to resume after (audit 9
 * L1). */
UTEST(mysql_backend, executable_and_hash_comments_are_unrecognised)
{
    static const char *const unreadable[] = {
        "/*! COMMIT */", "/*!50000 COMMIT */", "/*M! COMMIT */",
        "# c\nCOMMIT", "CREATE /*!99999 TEMPORARY */ TABLE t (id INT)",
        "CREATE # c\n TEMPORARY TABLE t (id INT)",
    };
    for (size_t i = 0; i < sizeof unreadable / sizeof unreadable[0]; i++) {
        EXPECT_EQ_MSG(0, my_sql_commits_implicitly(unreadable[i]), unreadable[i]);
        EXPECT_EQ_MSG((int)HL_SQL_TXN_OTHER, (int)my_sql_txn_kind(unreadable[i]),
                      unreadable[i]);
    }
    EXPECT_EQ((int)HL_SQL_TXN_COMMIT, (int)my_sql_txn_kind("/* c */ COMMIT"));
    EXPECT_EQ((int)HL_SQL_TXN_NONE, (int)my_sql_txn_kind("UPDATE a SET x = 1"));
}

/* Audit 10: the connection runs multi-statement texts, and the classifiers
 * read the first statement only - only ONE statement is ever taken for an
 * implicit commit (and resumed after). */
UTEST(mysql_backend, only_a_single_statement_commits_implicitly)
{
    static const char *const single[] = {
        "CREATE TABLE t (x INT)", "CREATE TABLE t (x INT);",
        "CREATE TABLE t (x INT) ; -- done\n", "CREATE TABLE t (x INT); /* c */",
        "CREATE TABLE t (c TEXT DEFAULT ';')", "CREATE TABLE `a;b` (x INT)",
        "CREATE TABLE t (c TEXT DEFAULT 'it''s')",
    };
    static const char *const not_single[] = {
        "CREATE TABLE t (x INT); COMMIT", "CREATE TABLE t (x INT);COMMIT;",
        "SELECT 1; CREATE TABLE t (x INT)",
        "CREATE TABLE t (c TEXT DEFAULT 'x\\'); COMMIT; -- ')",
        "CREATE TABLE t (x INT) /* unterminated", "CREATE TABLE t (c TEXT DEFAULT 'x)",
        "CREATE TABLE t (x INT) --x\n", "CREATE TABLE t (x INT); # c",
    };
    for (size_t i = 0; i < sizeof single / sizeof single[0]; i++)
        EXPECT_EQ_MSG(1, my_sql_commits_implicitly(single[i]), single[i]);
    for (size_t i = 0; i < sizeof not_single / sizeof not_single[0]; i++)
        EXPECT_EQ_MSG(0, my_sql_commits_implicitly(not_single[i]), not_single[i]);
}

/* MySQL block comments do not nest: the first star-slash ends one, so the
 * reader must not see this COMMIT as commented out (audit 10). */
UTEST(mysql_backend, block_comments_do_not_nest)
{
    EXPECT_EQ((int)HL_SQL_TXN_COMMIT, (int)my_sql_txn_kind("/* /* */ COMMIT"));
    EXPECT_EQ((int)HL_SQL_TXN_ROLLBACK, (int)my_sql_txn_kind("/* /* */ ROLLBACK"));
    EXPECT_EQ((int)HL_SQL_TXN_NONE, (int)hl_sql_txn_kind("/* /* */ COMMIT */"));
    EXPECT_EQ(1, my_sql_commits_implicitly("/* /* */ CREATE TABLE t (x INT)"));
}

/* The ROLLBACK a lost / aborted connection answers without sending must be
 * the whole text, read as MySQL reads it: a statement after it would be
 * reported done without running (audit 12). */
UTEST(mysql_backend, rollback_shortcut_is_single_statement_only)
{
    EXPECT_EQ(1, sql_is_rollback("ROLLBACK"));
    EXPECT_EQ(1, sql_is_rollback("rollback work;"));
    EXPECT_EQ(1, sql_is_rollback("/* c */ ROLLBACK -- done\n"));
    EXPECT_EQ(0, sql_is_rollback("ROLLBACK; INSERT INTO t VALUES (1)"));
    EXPECT_EQ(0, sql_is_rollback("ROLLBACK --x; INSERT INTO t VALUES (1)"));
    EXPECT_EQ(0, sql_is_rollback("ROLLBACK # x"));
    EXPECT_EQ(0, sql_is_rollback("ROLLBACK TO SAVEPOINT s"));

    /* A deadlock-aborted transaction: only a lone ROLLBACK is answered. */
    HlDbMyCtx s;
    memset(&s, 0, sizeof s);
    s.txn_aborted = 1;
    EXPECT_EQ(-1, my_ready(&s, "ROLLBACK; INSERT INTO t VALUES (1)", 0));
    EXPECT_EQ(1, s.txn_aborted);
    EXPECT_EQ(1, my_ready(&s, "ROLLBACK", 0));
    EXPECT_EQ(0, s.txn_aborted);
}

/* "SELECT 1; COMMIT" ended the batch's transaction by its second statement:
 * no transaction is opened under it (the batch reports its loss). */
UTEST(mysql_backend, multi_statement_commit_is_not_resumed)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    put_ok_more(&s, 1, 0x0002);                        /* the text */
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* a resume */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));
    EXPECT_LE(0, mysql_exec(&h, "CREATE TABLE t (x INT); COMMIT", NULL, 0));
    EXPECT_FALSE(my_in_trans(&h));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* An executable-comment COMMIT ends the batch's transaction: none is opened under
 * it (the batch reports its transaction lost instead). */
UTEST(mysql_backend, executable_comment_commit_is_not_resumed)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    put_ok_more(&s, 1, 0x0002);                        /* the COMMIT */
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* a resume */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));
    EXPECT_LE(0, mysql_exec(&h, "/*! COMMIT */", NULL, 0));   /* rows affected */
    /* No START TRANSACTION was sent (its queued reply is never read). */
    EXPECT_FALSE(my_in_trans(&h));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A failed statement with an executable comment whose transaction is gone
 * is refused until ROLLBACK, like any unrecognised statement - not resumed
 * as DDL (CREATE disguised by the comment) and not let through to
 * autocommit the rest of the batch. */
UTEST(mysql_backend, failed_executable_comment_statement_is_refused)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    build_err(&s, 1, 1205, "Lock wait timeout exceeded");        /* CREATE */
    put_ok_more(&s, 1, 0x0002);                        /* PING: autocommit */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));
    EXPECT_EQ(-1, mysql_exec(&h,
        "CREATE /*!99999 TEMPORARY */ TABLE snap SELECT * FROM a", NULL, 0));
    EXPECT_EQ(1, ctx.txn_aborted);
    EXPECT_EQ(0, mysql_rollback(&h));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* The CREATE INDEX IF NOT EXISTS shim takes a duplicate index by the server's
 * error code (ER_DUP_KEYNAME), not its message (audit 9 L2): a localized
 * message is still a duplicate, and another error whose text mentions
 * "Duplicate key name" is still a failure. */
UTEST(mysql_backend, create_index_shim_uses_error_code)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    build_err(&s, 1, HL_MY_ER_DUP_KEYNAME, "Doppelter Schluesselname 'i'");
    build_err(&s, 1, 1146, "Table 'Duplicate key name' doesn't exist");

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    EXPECT_EQ(0, mysql_exec(&h, "CREATE INDEX IF NOT EXISTS i ON t (x)", NULL, 0));
    EXPECT_EQ(-1, mysql_exec(&h, "CREATE INDEX IF NOT EXISTS i ON t (x)", NULL, 0));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A lock-wait timeout in CREATE TEMPORARY TABLE ... SELECT (shared locks on
 * the source rows) under innodb_rollback_on_timeout took the transaction:
 * refused until ROLLBACK, not resumed as DDL. */
UTEST(mysql_backend, temporary_table_rollback_is_not_resumed)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    build_err(&s, 1, 1205, "Lock wait timeout exceeded");        /* CREATE */
    put_ok_more(&s, 1, 0x0002);                        /* PING: autocommit */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));
    EXPECT_EQ(-1, mysql_exec(&h,
        "CREATE TEMPORARY TABLE snap SELECT * FROM accounts", NULL, 0));
    EXPECT_EQ(1, ctx.txn_aborted);
    EXPECT_TRUE(mysql_in_txn(&h));
    EXPECT_EQ(0, mysql_rollback(&h));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

UTEST_MAIN()
