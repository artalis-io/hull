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

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Canned server packets ────────────────────────────────────────── */

static void build_handshake_plugin(HlMyWriter *w, uint8_t seq, const char *plugin)
{
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_u8(w, 10);                       /* protocol v10 */
    hl_my_put_cstr(w, "8.0.35");               /* server version */
    hl_my_put_u32(w, 1);                       /* connection id */
    for (int i = 0; i < HL_MY_SCRAMBLE_PART1; i++)
        hl_my_put_u8(w, (uint8_t)(i + 1));     /* auth data part 1 */
    hl_my_put_u8(w, 0);                        /* filler */
    uint32_t caps = HL_MY_CLIENT_SECURE_CONNECTION | HL_MY_CLIENT_PLUGIN_AUTH
                  | HL_MY_CLIENT_PROTOCOL_41;
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

static void put_col_def(HlMyWriter *w, uint8_t seq, const char *name, uint8_t type)
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
    hl_my_put_u16(w, 0);                               /* flags */
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

UTEST_MAIN()
