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
#include "hull/cap/db_budget.h"
#include "hull/shared/work_charge.h"   /* HL_WORK_NET_SQL_UNITS */
/* The backend itself, for its static transaction bookkeeping (the build
 * leaves cap_db_mysql.o out of this test's link). */
#include "../../../src/hull/cap/db_mysql.c"

#include <string.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

/* ── Canned server packets ────────────────────────────────────────── */

static void build_handshake_cs(HlMyWriter *w, uint8_t seq, const char *plugin,
                               uint32_t extra_caps, uint8_t charset)
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
    hl_my_put_u8(w, charset);                  /* the server's default */
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

static void build_handshake_caps(HlMyWriter *w, uint8_t seq, const char *plugin,
                                 uint32_t extra_caps)
{
    build_handshake_cs(w, seq, plugin, extra_caps, HL_MY_DEFAULT_CHARSET);
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

/* Audit 20: the client announced the server's default character set,
 * whatever it was. Under latin1 (8) UTF-8 text was stored double-encoded;
 * under gbk (28) - whose trail byte can be 0x60 - a backtick
 * hl_db_quote_ident doubled was read as part of a character, closing the
 * identifier. It is utf8mb4 (45) now, unless the server's default already
 * is a utf8mb4 collation. The DSN's scheme is matched without case. */
UTEST(mysql_conn, connection_charset_is_utf8mb4)
{
    static const struct { uint8_t server, sent; } cases[] = {
        { 8, 45 }, { 28, 45 }, { 0, 45 }, { 255, 255 }, { 224, 224 }, { 45, 45 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int sv[2];
        ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
        HlMyWriter hs; hl_my_writer_init(&hs);
        build_handshake_cs(&hs, 0, "mysql_native_password", 0, cases[i].server);
        ASSERT_TRUE(write(sv[0], hs.buf, hs.len) == (ssize_t)hs.len);
        HlMyWriter ok; hl_my_writer_init(&ok); build_ok(&ok, 2);
        ASSERT_TRUE(write(sv[0], ok.buf, ok.len) == (ssize_t)ok.len);

        HlMyDsn dsn; char err[128];
        ASSERT_EQ(0, hl_my_dsn_parse("MySQL://alice:pw@localhost/shop",
                                     &dsn, err, sizeof err));
        HlMyConn conn;
        ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

        uint8_t got[512];
        ssize_t n = read(sv[0], got, sizeof got);
        ASSERT_TRUE(n > 4);
        HlMyFrame f; size_t consumed = 0;
        ASSERT_EQ(hl_my_frame_next(got, (size_t)n, &f, &consumed), HL_MY_OK);
        HlMyCursor c; hl_my_cursor_init(&c, &f);
        (void)hl_my_get_u32(&c);                      /* client caps */
        (void)hl_my_get_u32(&c);                      /* max packet */
        EXPECT_EQ(hl_my_get_u8(&c), cases[i].sent);   /* charset */

        hl_my_conn_close(&conn);
        hl_my_writer_free(&hs); hl_my_writer_free(&ok);
        close(sv[0]);
    }
    HlMyDsn dsn; char err[128];
    EXPECT_EQ(0, hl_my_dsn_parse("MARIADB://u@h/db", &dsn, err, sizeof err));
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

/* The auth exchange is bounded and charged (audit 17): a server repeating
 * FAST_SUCCESS (or any packet that keeps the loop going) used to hold the
 * client in it for as long as it kept sending. */
static void put_fast_success(HlMyWriter *s, uint8_t seq)
{
    size_t m = hl_my_packet_begin(s, seq);
    hl_my_put_u8(s, HL_MY_AUTH_MORE_DATA);
    hl_my_put_u8(s, HL_MY_CACHING_SHA2_FAST_SUCCESS);
    hl_my_packet_end(s, m);
}

UTEST(mysql_conn, auth_exchange_is_bounded)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake_plugin(&s, 0, "caching_sha2_password");
    for (int i = 0; i < 40; i++) put_fast_success(&s, (uint8_t)(2 + i));
    build_ok(&s, 42);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://alice:pw@localhost/shop",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    EXPECT_NE(0, hl_my_conn_start(&conn, sv[1], &dsn));
    EXPECT_TRUE(strstr(conn.errmsg, "too many packets") != NULL);
    hl_my_writer_free(&s);
    close(sv[0]);
}

UTEST(mysql_conn, auth_exchange_is_charged)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake_plugin(&s, 0, "caching_sha2_password");
    for (int i = 0; i < 8; i++) put_fast_success(&s, (uint8_t)(2 + i));
    build_ok(&s, 10);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://alice:pw@localhost/shop",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    HlDbOpBudget budget = { .limit = 5, .used = 0, .tripped = 0 };
    HlDbBudgetBinding prev = hl_db_budget_swap(hl_db_op_budget_charge, &budget);
    int rc = hl_my_conn_start(&conn, sv[1], &dsn);
    hl_db_budget_restore(prev);
    EXPECT_NE(0, rc);
    EXPECT_EQ(1, budget.tripped);
    EXPECT_TRUE(strstr(conn.errmsg, "interrupted") != NULL);
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

/* A budget the row callback trips: the run went over its limit while the
 * rows were being read. */
static int my_tripped;
static int my_trip_fn(void *ud, int64_t units)
{
    (void)ud; (void)units;
    return my_tripped;
}
static int my_trip_row(void *ctx, const char *const *vals, const size_t *lens,
                       int nc)
{
    (void)vals; (void)lens; (void)nc;
    (*(int *)ctx)++;
    my_tripped = 1;
    return 0;
}

/* Once the run's budget or deadline trips, the rest of the rows are not read
 * to the end (audit 15): the connection is closed and stays broken, and the
 * query fails, so the backend reconnects on the next call. */
UTEST(mysql_conn, a_tripped_run_abandons_the_reply)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    { size_t m = hl_my_packet_begin(&s, 1);
      hl_my_put_lenenc_int(&s, 1);
      hl_my_packet_end(&s, m); }
    put_col_def(&s, 2, "x", HL_MY_TYPE_LONG);
    put_eof(&s, 3);
    for (uint8_t i = 0; i < 3; i++) {
        size_t m = hl_my_packet_begin(&s, (uint8_t)(4 + i));
        hl_my_put_lenenc_str(&s, "7", 1);
        hl_my_packet_end(&s, m);
    }
    put_eof(&s, 7);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    my_tripped = 0;
    HlDbBudgetBinding prev = hl_db_budget_swap(my_trip_fn, NULL);
    int rows = 0;
    int rc = hl_my_conn_query(&conn, "SELECT x FROM t", NULL, my_trip_row,
                              &rows, NULL);
    hl_db_budget_restore(prev);
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(1, rows);                  /* no row read past the trip */
    EXPECT_EQ(1, conn.broken);
    EXPECT_TRUE(conn.transport == NULL); /* closed, not drained */
    EXPECT_TRUE(strstr(conn.errmsg, "interrupted") != NULL);

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

/* The rows of a SECOND result set (a multi-statement COM_QUERY, a CALL) are
 * charged too (audit 17): drain_one_result discarded them unchecked, at the
 * server's pace, so `SELECT 1; SELECT * FROM huge` read the whole second
 * result inside one call. An op budget (HlDbOpBudget) of 10 units stops a
 * 40-row second result set; the connection is closed, not drained. */
UTEST(mysql_conn, second_result_set_rows_are_charged)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, HL_MY_SERVER_MORE_RESULTS);     /* statement 1: OK, more */
    { size_t m = hl_my_packet_begin(&s, 2);            /* statement 2: rows */
      hl_my_put_lenenc_int(&s, 1);
      hl_my_packet_end(&s, m); }
    put_col_def(&s, 3, "x", HL_MY_TYPE_LONG);
    put_eof(&s, 4);
    for (int i = 0; i < 40; i++) {
        size_t m = hl_my_packet_begin(&s, (uint8_t)(5 + i));
        hl_my_put_lenenc_str(&s, "7", 1);
        hl_my_packet_end(&s, m);
    }
    put_eof(&s, 45);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    HlDbOpBudget budget = { .limit = 10, .used = 0, .tripped = 0 };
    HlDbBudgetBinding prev = hl_db_budget_swap(hl_db_op_budget_charge, &budget);
    int rc = hl_my_conn_query(&conn, "UPDATE a SET x=1; SELECT x FROM t",
                              NULL, NULL, NULL, NULL);
    hl_db_budget_restore(prev);
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(1, budget.tripped);
    EXPECT_EQ(1, conn.broken);
    EXPECT_TRUE(conn.transport == NULL); /* closed, not drained */
    EXPECT_TRUE(strstr(conn.errmsg, "interrupted") != NULL);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* Every packet of a reply is charged, not only the data rows (audit 18): a
 * server answering with an endless stream of EMPTY result sets (a procedure
 * looping over a SELECT that finds nothing) sent only column counts, column
 * definitions and EOFs, which were read uncharged past the budget and the
 * run deadline. 60 empty result sets against an op budget of 40 units: it
 * trips, and the connection is closed, not drained. */
static void put_eof_status(HlMyWriter *w, uint8_t seq, uint16_t status)
{
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_u8(w, HL_MY_PKT_EOF);
    hl_my_put_u16(w, 0);        /* warnings */
    hl_my_put_u16(w, status);   /* status */
    hl_my_packet_end(w, m);
}

UTEST(mysql_conn, empty_result_sets_are_charged)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    uint8_t seq = 1;
    for (int i = 0; i < 60; i++) {
        size_t m = hl_my_packet_begin(&s, seq++);       /* column count */
        hl_my_put_lenenc_int(&s, 1);
        hl_my_packet_end(&s, m);
        put_col_def(&s, seq++, "x", HL_MY_TYPE_LONG);
        put_eof(&s, seq++);                             /* end of col defs */
        put_eof_status(&s, seq++, i < 59 ? HL_MY_SERVER_MORE_RESULTS : 0);
    }
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    HlDbOpBudget budget = { .limit = 40, .used = 0, .tripped = 0 };
    HlDbBudgetBinding prev = hl_db_budget_swap(hl_db_op_budget_charge, &budget);
    int rc = hl_my_conn_query(&conn, "CALL empty_forever()", NULL, NULL, NULL, NULL);
    hl_db_budget_restore(prev);
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(1, budget.tripped);
    EXPECT_EQ(1, conn.broken);
    EXPECT_TRUE(conn.transport == NULL); /* closed, not drained */
    EXPECT_TRUE(strstr(conn.errmsg, "interrupted") != NULL);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* The same stream through exec_multi (migrations' path). */
UTEST(mysql_conn, exec_multi_empty_result_sets_are_charged)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    uint8_t seq = 1;
    for (int i = 0; i < 60; i++)
        put_ok_more(&s, seq++, i < 59 ? HL_MY_SERVER_MORE_RESULTS : 0);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    HlDbOpBudget budget = { .limit = 40, .used = 0, .tripped = 0 };
    HlDbBudgetBinding prev = hl_db_budget_swap(hl_db_op_budget_charge, &budget);
    int rc = hl_my_conn_exec_multi(&conn, "CALL ok_forever()");
    hl_db_budget_restore(prev);
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(1, budget.tripped);
    EXPECT_TRUE(conn.transport == NULL);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A trip on the packet that ENDS a reply is not abandoned (audit 18): here
 * the OK answering a COMMIT. Nothing is left to read, and dropping the
 * connection reported a commit the server had made as failed, its
 * transaction "lost". The call succeeds, the connection stays in step and
 * usable, and the trip is still there for the run to raise. A trip on a
 * packet that more of the reply follows still abandons it (the tests
 * above). */
UTEST(mysql_conn, a_trip_on_the_final_ok_finishes_the_commit)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);                     /* auth */
    build_ok(&s, 1);                     /* COMMIT's OK: 7 bytes, 2 units */
    build_ok(&s, 1);                     /* the next statement's */
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    HlDbOpBudget budget = { .limit = 1, .used = 0, .tripped = 0 };   /* passed by the OK's charge */
    HlDbBudgetBinding prev = hl_db_budget_swap(hl_db_op_budget_charge, &budget);
    int rc = hl_my_conn_query(&conn, "COMMIT", NULL, NULL, NULL, NULL);
    int still = hl_db_budget_charge(0);
    hl_db_budget_restore(prev);
    EXPECT_EQ(0, rc);
    EXPECT_EQ(1, budget.tripped);
    EXPECT_NE(0, still);                 /* the run still sees the trip */
    EXPECT_EQ(0, conn.broken);
    EXPECT_TRUE(conn.transport != NULL);
    EXPECT_EQ(0, conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS);

    /* In step: the next statement reads its own reply. */
    EXPECT_EQ(0, hl_my_conn_query(&conn, "SELECT 1", NULL, NULL, NULL, NULL));

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A budget that trips on its @p my_trip_at'th charge (0-unit asks included).
 * The per-statement floor charged before the send (audit 41,
 * HL_WORK_NET_SQL_UNITS) is not counted: these tests place the trip inside
 * the exchange, and the floor's own refusal is tested apart. */
static int my_charges, my_trip_at;
static int my_count_trip_fn(void *ud, int64_t units)
{
    (void)ud;
    if (units == (int64_t)HL_WORK_NET_SQL_UNITS) return 0;
    return ++my_charges >= my_trip_at;
}

static int64_t my_test_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A server that says nothing (SLEEP(), a lock wait) no longer holds the call
 * in recv for the whole read timeout - here none at all - with no budget or
 * run deadline checked (audit 18): the wait is polled in slices that ask the
 * budget, and a trip closes the connection. */
UTEST(mysql_conn, a_silent_server_is_stopped_by_a_tripped_budget)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db?read_timeout=0",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));
    EXPECT_EQ(0, conn.read_timeout_ms);

    my_charges = 0;
    my_trip_at = 3;                      /* the third slice's ask trips */
    HlDbBudgetBinding prev = hl_db_budget_swap(my_count_trip_fn, NULL);
    int64_t t0 = my_test_ms();
    int rc = hl_my_conn_query(&conn, "SELECT SLEEP(3600)", NULL, NULL, NULL, NULL);
    int64_t spent = my_test_ms() - t0;
    hl_db_budget_restore(prev);
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(3, my_charges);
    EXPECT_TRUE(spent < 5000);
    EXPECT_EQ(1, conn.broken);
    EXPECT_TRUE(conn.transport == NULL);
    EXPECT_TRUE(strstr(conn.errmsg, "interrupted") != NULL);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}


/* Audit 21: a peer that sends a frame a byte at a time, each sooner than one
 * wait slice, kept the read going: the run was asked only after a QUIET slice,
 * and every wait restarted ?read_timeout=. A thread drips the frame. */
typedef struct {
    int            fd;
    const uint8_t *head;
    size_t         head_len;
    _Atomic int    stop;
} MyDrip;

static void *my_drip_main(void *p)
{
    MyDrip *d = p;
    if (write(d->fd, d->head, d->head_len) != (ssize_t)d->head_len)
        return NULL;
    for (int i = 0; i < 250 && !atomic_load(&d->stop); i++) {
        struct timespec ts = { 0, 20 * 1000000L };
        nanosleep(&ts, NULL);
        uint8_t b = 'x';
        if (write(d->fd, &b, 1) != 1)
            break;
    }
    return NULL;
}

/* A packet header announcing ~1 MB, then its body a byte every 20 ms (5 s). */
static const uint8_t my_drip_head[] = { 0xff, 0xff, 0x0f, 0x01 };

static int my_drip_conn(int sv[2], HlMyConn *conn, HlMyWriter *s, const char *dsn_s)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    hl_my_writer_init(s);
    build_handshake(s, 0);
    build_ok(s, 2);
    if (write(sv[0], s->buf, s->len) != (ssize_t)s->len) return -1;
    HlMyDsn dsn; char err[128];
    if (hl_my_dsn_parse(dsn_s, &dsn, err, sizeof err) != 0) return -1;
    return hl_my_conn_start(conn, sv[1], &dsn);
}

UTEST(mysql_conn, a_dripping_server_is_stopped_by_a_tripped_budget)
{
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    HlMyConn conn;
    HlMyWriter s;
    ASSERT_EQ(0, my_drip_conn(sv, &conn, &s, "mysql://u:p@localhost/db?read_timeout=0"));
    MyDrip d = { .fd = sv[0], .head = my_drip_head, .head_len = sizeof my_drip_head };
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, my_drip_main, &d));

    my_charges = 0;
    my_trip_at = 3;                      /* the third chunk's ask trips */
    HlDbBudgetBinding prev = hl_db_budget_swap(my_count_trip_fn, NULL);
    int64_t t0 = my_test_ms();
    int rc = hl_my_conn_query(&conn, "SELECT big", NULL, NULL, NULL, NULL);
    int64_t spent = my_test_ms() - t0;
    hl_db_budget_restore(prev);
    atomic_store(&d.stop, 1);
    pthread_join(th, NULL);
    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(spent < 2000);           /* not the 5 s of the drip */
    EXPECT_EQ(1, conn.broken);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

UTEST(mysql_conn, the_read_timeout_bounds_a_dripping_server)
{
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    HlMyConn conn;
    HlMyWriter s;
    ASSERT_EQ(0, my_drip_conn(sv, &conn, &s, "mysql://u:p@localhost/db?read_timeout=300"));
    MyDrip d = { .fd = sv[0], .head = my_drip_head, .head_len = sizeof my_drip_head };
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, my_drip_main, &d));

    int64_t t0 = my_test_ms();
    int rc = hl_my_conn_query(&conn, "SELECT big", NULL, NULL, NULL, NULL);
    int64_t spent = my_test_ms() - t0;
    atomic_store(&d.stop, 1);
    pthread_join(th, NULL);
    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(spent >= 250 && spent < 2000);
    EXPECT_EQ(1, conn.broken);
    EXPECT_TRUE(strstr(conn.errmsg, "timed out") != NULL);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* ?read_timeout= now bounds an adopted connection's wait too (it was only
 * installed as SO_RCVTIMEO by hl_my_conn_open). */
UTEST(mysql_conn, the_read_timeout_bounds_a_silent_server)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db?read_timeout=250",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    int64_t t0 = my_test_ms();
    int rc = hl_my_conn_query(&conn, "SELECT SLEEP(3600)", NULL, NULL, NULL, NULL);
    int64_t spent = my_test_ms() - t0;
    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(spent >= 200 && spent < 5000);
    EXPECT_EQ(1, conn.broken);
    EXPECT_TRUE(strstr(conn.errmsg, "timed out") != NULL);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* Audit 23: the request went out with a blocking send under SO_SNDTIMEO
 * alone (none with ?read_timeout=0), so a server that stopped reading a large
 * statement held the call with no budget or run deadline asked. The write is
 * sliced now: a tripped budget stops it and closes the connection, and
 * ?read_timeout= bounds the whole write. */
static char *my_big_sql(size_t n)
{
    char *sql = malloc(n + 1);
    if (!sql) return NULL;
    memcpy(sql, "SELECT 1", 8);
    memset(sql + 8, ' ', n - 8);
    sql[n] = '\0';
    return sql;
}

UTEST(mysql_conn, a_server_that_never_reads_is_stopped_by_a_tripped_budget)
{
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db?read_timeout=0",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));
    char *sql = my_big_sql((size_t)8 << 20);   /* far past the socket buffer */
    ASSERT_TRUE(sql != NULL);

    my_charges = 0;
    my_trip_at = 3;
    HlDbBudgetBinding prev = hl_db_budget_swap(my_count_trip_fn, NULL);
    int64_t t0 = my_test_ms();
    int rc = hl_my_conn_query(&conn, sql, NULL, NULL, NULL, NULL);
    int64_t spent = my_test_ms() - t0;
    hl_db_budget_restore(prev);
    free(sql);
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(3, my_charges);
    EXPECT_TRUE(spent < 5000);
    EXPECT_EQ(1, conn.broken);
    EXPECT_TRUE(conn.transport == NULL);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

UTEST(mysql_conn, the_read_timeout_bounds_a_server_that_never_reads)
{
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db?read_timeout=250",
                                 &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));
    char *sql = my_big_sql((size_t)8 << 20);
    ASSERT_TRUE(sql != NULL);

    int64_t t0 = my_test_ms();
    int rc = hl_my_conn_query(&conn, sql, NULL, NULL, NULL, NULL);
    int64_t spent = my_test_ms() - t0;
    free(sql);
    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(spent >= 200 && spent < 5000);
    EXPECT_EQ(1, conn.broken);

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* More values than the statement has placeholders is refused, as SQLite
 * (SQLITE_RANGE) and Postgres refuse it (audit 17): build_execute bound the
 * first num_params and dropped the rest without a word. Nothing is executed;
 * the statement is closed and the connection stays in step. */
UTEST(mysql_conn, prepared_surplus_params_refused)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    /* PREPARE_OK: stmt 1, 0 columns, 1 param */
    { size_t m = hl_my_packet_begin(&s, 1);
      hl_my_put_u8(&s, HL_MY_PKT_OK);
      hl_my_put_u32(&s, 1);
      hl_my_put_u16(&s, 0);                            /* num_columns */
      hl_my_put_u16(&s, 1);                            /* num_params */
      hl_my_put_u8(&s, 0);
      hl_my_put_u16(&s, 0);
      hl_my_packet_end(&s, m); }
    put_col_def(&s, 2, "p1", HL_MY_TYPE_LONGLONG);     /* 1 param def + EOF */
    put_eof(&s, 3);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));

    HlMyParam p[2];
    memset(p, 0, sizeof p);
    p[0].type = HL_MY_TYPE_LONGLONG; p[0].v.i = 1;
    p[1].type = HL_MY_TYPE_LONGLONG; p[1].v.i = 2;
    EXPECT_EQ(-1, hl_my_conn_query_prepared(&conn, "DELETE FROM t WHERE id = ?",
                                            p, 2, NULL, NULL, NULL, NULL));
    EXPECT_TRUE(strstr(conn.errmsg, "parameter count mismatch") != NULL);
    EXPECT_EQ(0, conn.broken);

    /* The client sent [HandshakeResponse41][COM_STMT_PREPARE][COM_STMT_CLOSE]:
     * no COM_STMT_EXECUTE. */
    uint8_t got[512];
    ssize_t n = read(sv[0], got, sizeof got);
    ASSERT_TRUE(n > 0);
    HlMyFrame f; size_t consumed = 0, off = 0;
    ASSERT_EQ(hl_my_frame_next(got, (size_t)n, &f, &consumed), HL_MY_OK);   /* handshake */
    off += consumed;
    ASSERT_EQ(hl_my_frame_next(got + off, (size_t)n - off, &f, &consumed), HL_MY_OK); /* PREPARE */
    off += consumed;
    ASSERT_EQ(hl_my_frame_next(got + off, (size_t)n - off, &f, &consumed), HL_MY_OK);
    ASSERT_TRUE(f.body_len > 0);
    EXPECT_EQ(f.body[0], HL_MY_COM_STMT_CLOSE);

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
        "ANALYZE TABLES t", "analyze no_write_to_binlog table t",
        "ANALYZE LOCAL TABLE t", "CREATE OR REPLACE TABLE t (id INT)",
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
        /* audit 29: MariaDB ANALYZE <stmt>, CREATE OR REPLACE TEMPORARY */
        "ANALYZE SELECT * FROM t", "analyze update t set x = 1",
        "ANALYZE DELETE FROM t", "ANALYZE FORMAT=JSON SELECT 1", "ANALYZE",
        "CREATE OR REPLACE TEMPORARY TABLE snap SELECT * FROM a",
        "create or /* c */ replace temporary table t (id int)",
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
    HlDbHandle hh;
    memset(&s, 0, sizeof s);
    memset(&hh, 0, sizeof hh);
    hh.ctx = &s;
    s.txn_aborted = 1;
    EXPECT_EQ(-1, my_ready(&hh, "ROLLBACK; INSERT INTO t VALUES (1)"));
    EXPECT_EQ(1, s.txn_aborted);
    EXPECT_EQ(1, my_ready(&hh, "ROLLBACK"));
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
    put_ok_more(&s, 1, 0x0002);   /* PING after the failure (audit 41) */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    EXPECT_EQ(0, mysql_exec(&h, "CREATE INDEX IF NOT EXISTS i ON t (x)", NULL, 0));
    EXPECT_EQ(-1, mysql_exec(&h, "CREATE INDEX IF NOT EXISTS i ON t (x)", NULL, 0));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* The shim rewrites exactly one CREATE [UNIQUE] INDEX IF NOT EXISTS
 * statement, matched by tokens (audit 14): app SQL that merely holds the
 * words - a procedure body, a string literal, a comment - or a
 * multi-statement text is sent as written, and a duplicate-index error in
 * one statement of a multi-statement text is a failure. */
UTEST(mysql_backend, create_index_shim_is_token_exact)
{
    HlDbMyCtx z;
    memset(&z, 0, sizeof z);
    static const char *const not_shimmed[] = {
        "CREATE PROCEDURE p() BEGIN SELECT 'index if not exists'; END",
        "CREATE TABLE t (c TEXT DEFAULT 'index if not exists')",
        "SELECT 1 /* create index if not exists */",
        "CREATE INDEX IF NOT EXISTS i ON t (x); DROP TABLE z",
        "CREATE INDEX i ON t (x) COMMENT 'if not exists'",
        "CREATE INDEXX IF NOT EXISTS i ON t (x)",
        "/*!80000 CREATE INDEX IF NOT EXISTS i ON t (x) */",
    };
    for (size_t i = 0; i < sizeof not_shimmed / sizeof not_shimmed[0]; i++) {
        int handled = 7;
        EXPECT_EQ(0, mysql_create_index_shim(&z, not_shimmed[i], &handled));
        EXPECT_EQ(0, handled);
    }

    /* A multi-statement text with a duplicate index fails as sent. */
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002);                         /* rewritten CREATE */
    build_err(&s, 1, HL_MY_ER_DUP_KEYNAME, "Duplicate key name 'i'");
    put_ok_more(&s, 1, 0x0002);   /* PING after the failure (audit 41) */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    char junk[512];
    while (recv(sv[0], junk, sizeof junk, MSG_DONTWAIT) > 0) {}  /* handshake */
    EXPECT_EQ(0, mysql_exec(&h,
        "  create unique /* c */ index  if  not exists i on t (x);", NULL, 0));
    char sent[512];
    ssize_t n = recv(sv[0], sent, sizeof sent - 1, MSG_DONTWAIT);
    ASSERT_GT(n, 4);
    sent[n] = '\0';
    EXPECT_TRUE(strstr(sent + 5, "index  i on t (x);") != NULL);
    EXPECT_TRUE(strstr(sent + 5, "not") == NULL);
    EXPECT_EQ(-1, mysql_exec(&h,
        "CREATE INDEX IF NOT EXISTS i ON t (x); CREATE TABLE u (y INT)",
        NULL, 0));

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

/* ── Audit 31: a COMMIT tripped in its wait is finished ───────────── */

/* A thread that writes @p buf to @p fd after @p delay_ms. */
typedef struct {
    int            fd;
    const uint8_t *buf;
    size_t         len;
    int            delay_ms;
} MyLate;

static void *my_late_main(void *p)
{
    MyLate *l = p;
    struct timespec ts = { l->delay_ms / 1000, (long)(l->delay_ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    if (write(l->fd, l->buf, l->len) != (ssize_t)l->len) return NULL;
    return NULL;
}

/* The run's budget trips while the OK of a lone COMMIT is awaited (it comes
 * 300 ms later). The COMMIT was wholly sent, so the server commits whatever
 * the client does: the wait used to abandon the connection with IN_TRANS
 * still set, the commit was reported failed, and the next ROLLBACK was
 * answered done unsent - a retry applied the batch twice. The reply is read
 * on (no budget asked) and its status taken; the trip stays sticky. */
UTEST(mysql_backend, a_commit_tripped_in_its_wait_is_finished)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));
    ASSERT_TRUE(my_in_trans(&h));

    HlMyWriter late; hl_my_writer_init(&late);
    put_ok_more(&late, 1, 0x0002);                      /* COMMIT: autocommit */
    put_ok_more(&late, 1, 0x0002);                      /* the next statement */
    MyLate l = { .fd = sv[0], .buf = late.buf, .len = late.len, .delay_ms = 300 };
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, my_late_main, &l));

    my_charges = 0;
    my_trip_at = 1;                      /* the first slice's ask trips */
    HlDbBudgetBinding prev = hl_db_budget_swap(my_count_trip_fn, NULL);
    int64_t t0 = my_test_ms();
    int rc = mysql_commit(&h);
    int64_t spent = my_test_ms() - t0;
    int still = hl_db_budget_charge(0);
    hl_db_budget_restore(prev);
    pthread_join(th, NULL);
    EXPECT_EQ(0, rc);                    /* the commit the server made */
    EXPECT_NE(0, still);                 /* the run still raises the trip */
    EXPECT_TRUE(spent >= 250 && spent < 2000);
    EXPECT_EQ(0, ctx.conn.broken);
    EXPECT_TRUE(ctx.conn.transport != NULL);
    EXPECT_FALSE(mysql_in_txn(&h));      /* the guard rolls nothing "back" */

    /* In step, and no longer finishing: the next statement reads its reply. */
    EXPECT_TRUE(mysql_exec(&h, "SELECT 1", NULL, 0) >= 0);
    EXPECT_EQ(0, ctx.conn.finishing);

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    hl_my_writer_free(&late);
    close(sv[0]);
}

/* The same trip, but the server never answers: the finishing read asks no
 * budget, so it is held to HL_MY_FINISH_MAX_MS (here ?read_timeout=0, no
 * other bound) and the connection is then abandoned as before audit 31 -
 * IN_TRANS still set, so calls refuse until a ROLLBACK. */
UTEST(mysql_backend, an_unanswered_finishing_commit_is_abandoned_after_the_bound)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    memset(&h, 0, sizeof h);
    memset(&ctx, 0, sizeof ctx);
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);
    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db?read_timeout=0",
                                 &dsn, err, sizeof err));
    ASSERT_EQ(0, hl_my_conn_start(&ctx.conn, sv[1], &dsn));
    h.backend = &hl_db_backend_mysql;
    h.ctx = &ctx;
    h.batch_depth = 1;
    ASSERT_EQ(0, mysql_begin(&h));

    my_charges = 0;
    my_trip_at = 1;
    HlDbBudgetBinding prev = hl_db_budget_swap(my_count_trip_fn, NULL);
    int64_t t0 = my_test_ms();
    int rc = mysql_commit(&h);
    int64_t spent = my_test_ms() - t0;
    hl_db_budget_restore(prev);
    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(spent >= HL_MY_FINISH_MAX_MS - 100 && spent < HL_MY_FINISH_MAX_MS + 2000);
    EXPECT_EQ(1, ctx.conn.broken);
    EXPECT_TRUE(ctx.conn.transport == NULL);
    EXPECT_TRUE(mysql_in_txn(&h));
    /* refused until a ROLLBACK, which the server did with the session */
    EXPECT_EQ(-1, mysql_exec(&h, "INSERT INTO a VALUES (1)", NULL, 0));
    EXPECT_EQ(0, mysql_rollback(&h));
    EXPECT_FALSE(mysql_in_txn(&h));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A statement that is not one COMMIT ("COMMIT; DELETE ...") never
 * finishes: the trip abandons it at once, as before. */
UTEST(mysql_backend, only_a_lone_commit_finishes)
{
    EXPECT_TRUE(sql_is_commit("COMMIT"));
    EXPECT_TRUE(sql_is_commit(" commit work ; "));
    EXPECT_FALSE(sql_is_commit("COMMIT; DELETE FROM t"));
    EXPECT_FALSE(sql_is_commit("/*!COMMIT*/"));
    EXPECT_FALSE(sql_is_commit("ROLLBACK"));

    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));

    my_charges = 0;
    my_trip_at = 1;
    HlDbBudgetBinding prev = hl_db_budget_swap(my_count_trip_fn, NULL);
    int64_t t0 = my_test_ms();
    int rc = mysql_exec(&h, "COMMIT; DELETE FROM t", NULL, 0);
    int64_t spent = my_test_ms() - t0;
    hl_db_budget_restore(prev);
    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(spent < 1000);           /* abandoned at the first ask */
    EXPECT_TRUE(ctx.conn.transport == NULL);

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* ── Audit 31: insert_if_absent refuses a truncated value ─────────── */

static void put_prepare_ok_1param(HlMyWriter *s)
{
    size_t m = hl_my_packet_begin(s, 1);
    hl_my_put_u8(s, HL_MY_PKT_OK);
    hl_my_put_u32(s, 1);
    hl_my_put_u16(s, 0);                            /* num_columns */
    hl_my_put_u16(s, 1);                            /* num_params */
    hl_my_put_u8(s, 0);
    hl_my_put_u16(s, 0);
    hl_my_packet_end(s, m);
    put_col_def(s, 2, "?", HL_MY_TYPE_VAR_STRING);
    put_eof(s, 3);
}

/* insert_if_absent was INSERT IGNORE, which downgrades every error to a
 * warning: an invalid utf8mb4 value or one too long for its column was
 * stored TRUNCATED and the call succeeded ('evt_1\xff' stored as 'evt_1',
 * then the genuine 'evt_1' read as a duplicate). It is a plain INSERT now:
 * ER_DUP_ENTRY is "already there" (0, no COM_PING), any other error fails
 * the call - strict mode rolled the statement back, so nothing was stored. */
UTEST(mysql_backend, insert_if_absent_is_a_plain_insert)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    put_prepare_ok_1param(&s);
    build_err(&s, 1, HL_MY_ER_DUP_ENTRY, "Duplicate entry 'k1' for key 'PRIMARY'");
    put_prepare_ok_1param(&s);
    build_err(&s, 1, 1366, "Incorrect string value: '\\xFF\\xFE' for column 'id'");
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* PING */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));

    const char *cols[] = { "id" };
    HlValue v1 = { .type = HL_TYPE_TEXT, .s = "k1", .len = 2 };
    EXPECT_EQ(0, mysql_insert_if_absent(&h, "t", cols, 1, cols, &v1, 1));
    EXPECT_EQ(0, ctx.conn.broken);
    EXPECT_EQ(0, ctx.txn_aborted);
    EXPECT_TRUE(my_in_trans(&h));        /* a duplicate keeps the transaction */

    HlValue v2 = { .type = HL_TYPE_TEXT, .s = "evt_1\xff\xfe", .len = 7 };
    EXPECT_EQ(-1, mysql_insert_if_absent(&h, "t", cols, 1, cols, &v2, 1));
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "Incorrect string value") != NULL);
    EXPECT_EQ(0, ctx.txn_aborted);       /* the ping found the transaction */
    EXPECT_TRUE(my_in_trans(&h));

    /* What the client sent: never INSERT IGNORE. */
    shutdown(sv[1], SHUT_WR);
    char got[8192];
    size_t n = 0;
    ssize_t r;
    while (n < sizeof got - 1 && (r = read(sv[0], got + n, sizeof got - 1 - n)) > 0)
        n += (size_t)r;
    got[n] = '\0';
    int saw_insert = 0, saw_ignore = 0;
    for (size_t i = 0; i + 11 <= n; i++) {
        if (memcmp(got + i, "INSERT INTO", 11) == 0) saw_insert++;
        if (memcmp(got + i, "IGNORE", 6) == 0) saw_ignore++;
    }
    EXPECT_EQ(2, saw_insert);
    EXPECT_EQ(0, saw_ignore);

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* ── Session reset (COM_RESET_CONNECTION) ──────────────────────────── */

/* Read every byte the client sent once it is done (it shuts its write side
 * down), NUL-terminated. */
static size_t my_drain_client(int sv[2], char *got, size_t cap)
{
    shutdown(sv[1], SHUT_WR);
    size_t n = 0;
    ssize_t r;
    while (n < cap - 1 && (r = read(sv[0], got + n, cap - 1 - n)) > 0)
        n += (size_t)r;
    got[n] = '\0';
    return n;
}

static int my_sent_command(const char *got, size_t n, uint8_t cmd)
{
    /* A one-byte command packet: length 1, sequence 0, the command. */
    for (size_t i = 0; i + 5 <= n; i++)
        if ((uint8_t)got[i] == 1 && got[i + 1] == 0 && got[i + 2] == 0 &&
            got[i + 3] == 0 && (uint8_t)got[i + 4] == cmd)
            return 1;
    return 0;
}

static int my_sent_text(const char *got, size_t n, const char *text)
{
    size_t tl = strlen(text);
    for (size_t i = 0; i + tl <= n; i++)
        if (memcmp(got + i, text, tl) == 0) return 1;
    return 0;
}

/* The handshake's status (autocommit) is the session's until the first OK:
 * it was left 0, which read as autocommit off. */
UTEST(mysql_conn, handshake_status_is_kept)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);
    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));
    EXPECT_TRUE(conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT);
    EXPECT_FALSE(conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS);
    EXPECT_EQ(HL_MY_DEFAULT_CHARSET, (int)conn.charset);
    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* COM_RESET_CONNECTION takes the OK's status, then the handshake's character
 * set is applied again (the reset restored the global one). */
UTEST(mysql_conn, reset_connection_restores_charset)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0000);                        /* SET autocommit = 0 */
    put_ok_more(&s, 1, 0x0002);                        /* RESET */
    put_ok_more(&s, 1, 0x0002);                        /* SET NAMES */
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);
    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));
    ASSERT_EQ(0, hl_my_conn_query(&conn, "SET autocommit = 0", NULL, NULL, NULL, NULL));
    EXPECT_FALSE(conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT);
    EXPECT_EQ(0, hl_my_conn_reset(&conn));
    EXPECT_TRUE(conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT);
    EXPECT_EQ(0, conn.broken);

    char got[8192];
    size_t n = my_drain_client(sv, got, sizeof got);
    EXPECT_TRUE(my_sent_command(got, n, HL_MY_COM_RESET_CONNECTION));
    EXPECT_TRUE(my_sent_text(got, n, "SET NAMES utf8mb4, collation_connection = 45"));

    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* MariaDB maps utf8mb4's default collation through character_set_collations
 * (11.x: utf8mb4_uca1400_ai_ci), the handshake's included: naming id 45 after
 * the reset would change the session's collation, so a plain SET NAMES. */
UTEST(mysql_conn, reset_connection_on_mariadb_names_no_collation)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002);                        /* RESET */
    put_ok_more(&s, 1, 0x0002);                        /* SET NAMES */
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);
    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));
    EXPECT_EQ(0, conn.mariadb);                        /* "8.0.35" */
    conn.mariadb = 1;
    EXPECT_EQ(0, hl_my_conn_reset(&conn));
    char got[8192];
    size_t n = my_drain_client(sv, got, sizeof got);
    EXPECT_TRUE(my_sent_text(got, n, "SET NAMES utf8mb4"));
    EXPECT_FALSE(my_sent_text(got, n, "collation_connection"));
    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A server that does not know the command answers an ERR: the call fails,
 * the connection stays in step. */
UTEST(mysql_conn, reset_connection_refused)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    build_err(&s, 1, 1047, "Unknown command");
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);
    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));
    EXPECT_EQ(-1, hl_my_conn_reset(&conn));
    EXPECT_EQ(0, conn.broken);
    EXPECT_EQ(1047, (int)conn.last_err_code);
    EXPECT_TRUE(strstr(conn.errmsg, "Unknown command") != NULL);
    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A receive buffer grown for one big packet is given back once a small
 * reply follows (it kept up to 16 MB per pooled connection). */
UTEST(mysql_conn, receive_buffer_shrinks_after_a_big_packet)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    /* SELECT big: one column, one 2 MB row */
    size_t m = hl_my_packet_begin(&s, 1);
    hl_my_put_lenenc_int(&s, 1);
    hl_my_packet_end(&s, m);
    put_col_def(&s, 2, "v", HL_MY_TYPE_VAR_STRING);
    put_eof(&s, 3);
    size_t big = (size_t)2 << 20;
    char *val = malloc(big);
    ASSERT_TRUE(val != NULL);
    memset(val, 'x', big);
    m = hl_my_packet_begin(&s, 4);
    hl_my_put_lenenc_str(&s, val, big);
    hl_my_packet_end(&s, m);
    free(val);
    put_eof_status(&s, 5, 0x0002);
    put_ok_more(&s, 1, 0x0002);                        /* SELECT 1 */
    ASSERT_EQ(0, s.err);

    MyLate l = { .fd = sv[0], .buf = s.buf, .len = s.len, .delay_ms = 0 };
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, my_late_main, &l));

    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn;
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));
    QCollect g; memset(&g, 0, sizeof g);
    ASSERT_EQ(0, hl_my_conn_query(&conn, "SELECT big", qdesc, qrow, &g, NULL));
    EXPECT_EQ(1, g.rows);
    /* the frame read after the big row gave the buffer back */
    EXPECT_EQ((size_t)HL_MY_RECV_BUF_INIT, conn.rcap);
    ASSERT_EQ(0, hl_my_conn_query(&conn, "SELECT 1", NULL, NULL, NULL, NULL));
    EXPECT_EQ((size_t)HL_MY_RECV_BUF_INIT, conn.rcap);

    pthread_join(th, NULL);
    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* The implicit-commit resume opens a new transaction under the batch: the
 * transaction generation moves (it went through mysql_txn_raw alone). */
UTEST(mysql_backend, implicit_commit_resume_moves_the_generation)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    put_ok_more(&s, 1, 0x0002);                        /* CREATE: committed */
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* resume */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    ASSERT_EQ(0, mysql_begin(&h));
    uint64_t gen = hl_db_txn_gen();
    ASSERT_TRUE(hl_db_exec(&h, "CREATE TABLE t2 (id INT)", NULL, 0) >= 0);
    EXPECT_TRUE(my_in_trans(&h));
    EXPECT_NE(gen, hl_db_txn_gen());

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}


/* ── Lost connections ─────────────────────────────────────────────── */

/* Bytes the client has sent and the server end has not read yet (without
 * blocking or shutting anything down). */
static size_t my_pending(int sv[2])
{
    char junk[4096];
    size_t n = 0;
    ssize_t r;
    while ((r = recv(sv[0], junk, sizeof junk, MSG_DONTWAIT)) > 0)
        n += (size_t)r;
    return n;
}

/* A lost connection: the session ran @p stmt
 * (answered OK with @p status), then the connection went. Its DSN names a
 * port nothing listens on, so a reconnect shows as "reconnecting failed". */
static int my_lost_after(HlDbHandle *h, HlDbMyCtx *ctx, int sv[2],
                         HlMyWriter *s, const char *stmt, uint16_t status)
{
    build_handshake(s, 0);
    build_ok(s, 2);
    put_ok_more(s, 1, status);
    if (my_backend_start(h, ctx, sv, s) != 0) return -1;
    h->batch_depth = 0;
    if (mysql_exec(h, stmt, NULL, 0) < 0) return -1;
    (void)my_pending(sv);
    ctx->dsn = strdup("mysql://u:p@127.0.0.1:1/db");
    ctx->conn.broken = 1;   /* the server killed the session */
    return ctx->dsn ? 0 : -1;
}

static void my_lost_free(HlDbMyCtx *ctx, HlMyWriter *s, int sv[2])
{
    hl_my_conn_close(&ctx->conn);
    scrub_free(ctx->dsn);
    ctx->dsn = NULL;
    hl_my_writer_free(s);
    close(sv[0]);
}

/* ── Audit convergence phase 3: refuse session SQL, reset used sessions ── */

/* Session-altering app SQL is refused before anything is sent: on a shared
 * connection the session is reset at every entry boundary, so the state
 * could not be kept anyway. */
UTEST(mysql_backend, session_sql_is_refused_before_sending)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    (void)my_pending(sv);

    EXPECT_EQ(-1, mysql_exec(&h, "LOCK TABLES t WRITE", NULL, 0));
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "LOCK TABLES") != NULL);
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "session_control") != NULL);
    EXPECT_EQ(-1, mysql_exec(&h, "SET autocommit = 0", NULL, 0));
    EXPECT_EQ(-1, mysql_query(&h, "SELECT GET_LOCK('a', 0)", NULL, 0, NULL, NULL, NULL));
    EXPECT_EQ(-1, mysql_exec_script(&h, "CREATE TABLE t (x INT);\nXA START 'x'"));
    EXPECT_EQ(0u, my_pending(sv));     /* nothing reached the server */
    EXPECT_EQ(0, ctx.used);
    EXPECT_EQ(0, ctx.conn.broken);

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* Hull's own migration lock runs with the trust flag; a session_control
 * connection takes session SQL and refuses the stdlib (ns-trusted) calls. */
UTEST(mysql_backend, trusted_lock_and_session_control)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002);                        /* GET_LOCK (trusted) */
    put_ok_more(&s, 1, 0x0002);                        /* LOCK TABLES */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;

    int prev = hl_db_session_sql_trust(1);
    EXPECT_TRUE(mysql_exec(&h, "SELECT GET_LOCK('hull_migrate', 30)", NULL, 0) >= 0);
    hl_db_session_sql_trust(prev);

    h.session_control = 1;
    EXPECT_TRUE(mysql_exec(&h, "LOCK TABLES t WRITE", NULL, 0) >= 0);
    int ns = hl_cap_db_ns_trust(1);                    /* a stdlib frame */
    EXPECT_EQ(-1, mysql_exec(&h, "SELECT 1", NULL, 0));
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "standard library") != NULL);
    hl_cap_db_ns_restore(ns);

    char got[8192];
    size_t n = my_drain_client(sv, got, sizeof got);
    EXPECT_TRUE(my_sent_text(got, n, "GET_LOCK('hull_migrate'"));
    EXPECT_TRUE(my_sent_text(got, n, "LOCK TABLES t WRITE"));
    EXPECT_FALSE(my_sent_text(got, n, "SELECT 1"));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A connection an entry used is reset at the entry boundary (and only
 * then): COM_RESET_CONNECTION, then the character set and autocommit. One
 * nobody used costs nothing. */
UTEST(mysql_backend, guard_resets_a_used_connection_only)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002);                        /* SET @v = 1 */
    put_ok_more(&s, 1, 0x0002);                        /* RESET */
    put_ok_more(&s, 1, 0x0002);                        /* SET NAMES ... */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    (void)my_pending(sv);                              /* the handshake */

    hl_db_guard_stale_txn(&h);                         /* unused: nothing */
    EXPECT_EQ(0u, my_pending(sv));
    EXPECT_EQ(0, hl_db_reset_session(&h));

    ASSERT_TRUE(mysql_exec(&h, "SET @v = 1", NULL, 0) >= 0);
    EXPECT_EQ(1, ctx.used);
    hl_db_guard_stale_txn(&h);
    EXPECT_EQ(0, ctx.used);
    EXPECT_EQ(0, ctx.conn.broken);
    hl_db_guard_stale_txn(&h);                         /* clean: nothing more */

    char got[8192];
    size_t n = my_drain_client(sv, got, sizeof got);
    EXPECT_TRUE(my_sent_command(got, n, HL_MY_COM_RESET_CONNECTION));
    EXPECT_TRUE(my_sent_text(got, n, "autocommit = 1"));
    /* exactly one reset */
    int resets = 0;
    for (size_t i = 0; i + 5 <= n; i++)
        if ((uint8_t)got[i] == 1 && got[i + 1] == 0 && got[i + 2] == 0 &&
            got[i + 3] == 0 && (uint8_t)got[i + 4] == HL_MY_COM_RESET_CONNECTION)
            resets++;
    EXPECT_EQ(1, resets);

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A stale transaction: ROLLBACK, then the reset. */
UTEST(mysql_backend, guard_rolls_back_then_resets)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* START */
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* INSERT */
    put_ok_more(&s, 1, 0x0002);                                  /* ROLLBACK */
    put_ok_more(&s, 1, 0x0002);                                  /* RESET */
    put_ok_more(&s, 1, 0x0002);                                  /* SET NAMES */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    ASSERT_EQ(0, mysql_begin(&h));
    ASSERT_TRUE(mysql_exec(&h, "INSERT INTO t VALUES (1)", NULL, 0) >= 0);
    EXPECT_TRUE(mysql_in_txn(&h));
    hl_db_guard_stale_txn(&h);
    EXPECT_FALSE(mysql_in_txn(&h));
    EXPECT_EQ(0, ctx.used);

    char got[8192];
    size_t n = my_drain_client(sv, got, sizeof got);
    EXPECT_TRUE(my_sent_text(got, n, "ROLLBACK"));
    EXPECT_TRUE(my_sent_command(got, n, HL_MY_COM_RESET_CONNECTION));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A server that refuses COM_RESET_CONNECTION has the connection abandoned
 * instead (the server ends the session with it); the next call reconnects. */
UTEST(mysql_backend, guard_abandons_when_reset_is_refused)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002);                          /* SELECT */
    build_err(&s, 1, 1047, "Unknown command");           /* RESET */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    ASSERT_TRUE(mysql_exec(&h, "DELETE FROM t", NULL, 0) >= 0);
    EXPECT_EQ(-1, hl_db_reset_session(&h));
    EXPECT_EQ(1, ctx.conn.broken);
    EXPECT_EQ(0, ctx.used);
    EXPECT_FALSE(mysql_in_txn(&h));
    EXPECT_TRUE(ctx.conn.transport == NULL);   /* closed */

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A transaction its ROLLBACK cannot end (XA on a session_control
 * connection: XAER_RMFAIL) is closed with the connection. */
UTEST(mysql_backend, guard_closes_a_transaction_rollback_cannot_end)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* XA START */
    build_err(&s, 1, 1399, "XAER_RMFAIL");                       /* ROLLBACK */
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS);   /* PING */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    h.session_control = 1;
    ASSERT_TRUE(mysql_exec(&h, "XA START 'x'", NULL, 0) >= 0);
    hl_db_guard_stale_txn(&h);
    EXPECT_EQ(1, ctx.conn.broken);
    EXPECT_FALSE(mysql_in_txn(&h));
    EXPECT_EQ(0, ctx.used);

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A pinned handle (the migration run holds GET_LOCK) is never reset. */
UTEST(mysql_backend, guard_leaves_a_pinned_session)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002);                          /* SELECT */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    ASSERT_TRUE(mysql_exec(&h, "SELECT 1", NULL, 0) >= 0);
    h.session_pinned = 1;
    hl_db_guard_stale_txn(&h);
    EXPECT_EQ(0, ctx.conn.broken);
    EXPECT_EQ(1, ctx.used);                    /* reset once unpinned */

    char got[8192];
    size_t n = my_drain_client(sv, got, sizeof got);
    EXPECT_FALSE(my_sent_command(got, n, HL_MY_COM_RESET_CONNECTION));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A lost shared connection reconnects on the next call - Hull infers no
 * session state it might have held (none can exist: the statements that
 * would set it are refused). A lost session_control connection used in this
 * entry refuses until the entry ends, then reconnects. */
UTEST(mysql_backend, lost_connection_reconnects_unless_session_control)
{
    HlMyWriter s; hl_my_writer_init(&s);
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_lost_after(&h, &ctx, sv, &s, "SELECT 1", 0x0002));
    EXPECT_EQ(-1, mysql_exec(&h, "INSERT INTO t VALUES (1)", NULL, 0));
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "reconnecting failed") != NULL);
    my_lost_free(&ctx, &s, sv);

    hl_my_writer_init(&s);
    ASSERT_EQ(0, my_lost_after(&h, &ctx, sv, &s, "SELECT 1", 0x0002));
    h.session_control = 1;
    EXPECT_EQ(-1, mysql_exec(&h, "INSERT INTO t VALUES (1)", NULL, 0));
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "session_control connection was lost") != NULL);
    hl_db_guard_stale_txn(&h);                 /* the entry ends */
    EXPECT_EQ(0, ctx.used);
    EXPECT_EQ(-1, mysql_exec(&h, "INSERT INTO t VALUES (1)", NULL, 0));
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "reconnecting failed") != NULL);
    my_lost_free(&ctx, &s, sv);
}

/* Lost inside a transaction: refused until a ROLLBACK, answered unsent. */
UTEST(mysql_backend, lost_inside_a_transaction_refuses_until_rollback)
{
    HlMyWriter s; hl_my_writer_init(&s);
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_lost_after(&h, &ctx, sv, &s, "START TRANSACTION",
                               0x0002 | HL_MY_SERVER_STATUS_IN_TRANS));
    EXPECT_TRUE(mysql_in_txn(&h));
    EXPECT_EQ(-1, mysql_exec(&h, "INSERT INTO t VALUES (1)", NULL, 0));
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "inside a transaction") != NULL);
    EXPECT_EQ(0, mysql_rollback(&h));
    EXPECT_FALSE(mysql_in_txn(&h));
    EXPECT_EQ(0u, my_pending(sv));
    my_lost_free(&ctx, &s, sv);
}

/* A CALL that failed outside a transaction after its procedure opened one:
 * the ERR carries no status, so the backend asks (COM_PING) and in_txn sees
 * it - the wait gates refuse and the guard rolls back at the entry's end. */
UTEST(mysql_backend, failed_call_outside_a_transaction_refreshes_status)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    build_err(&s, 1, 1644, "boom");                            /* CALL p() */
    put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_STATUS_IN_TRANS); /* PING */
    put_ok_more(&s, 1, 0x0002);                                /* ROLLBACK */

    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    EXPECT_EQ(-1, mysql_exec(&h, "CALL p()", NULL, 0));
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "boom") != NULL);   /* kept */
    EXPECT_TRUE(mysql_in_txn(&h));
    EXPECT_EQ(0, mysql_rollback(&h));
    EXPECT_FALSE(mysql_in_txn(&h));

    char got[8192];
    size_t n = my_drain_client(sv, got, sizeof got);
    EXPECT_TRUE(my_sent_command(got, n, HL_MY_COM_PING));

    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* A refusal by the backend itself (no server ERR this call) sends no PING. */
UTEST(mysql_backend, a_refused_call_sends_no_ping)
{
    HlMyWriter s; hl_my_writer_init(&s);
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_lost_after(&h, &ctx, sv, &s, "SELECT 1", 0x0002));
    ctx.conn.broken = 0;
    ctx.txn_aborted = 1;               /* refused by my_ready */
    ctx.conn.last_err_code = 1644;     /* stale, from an earlier call */
    EXPECT_EQ(-1, mysql_exec(&h, "INSERT INTO t VALUES (1)", NULL, 0));
    EXPECT_EQ(0, ctx.conn.last_err_code);
    EXPECT_EQ(0u, my_pending(sv));
    ctx.conn.broken = 1;
    my_lost_free(&ctx, &s, sv);
}

/* The session reset re-applies autocommit = 1 with the character set: a
 * reset restores the global default, which a server may have off. */
UTEST(mysql_conn, reset_connection_restores_autocommit)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0000);                        /* RESET */
    put_ok_more(&s, 1, 0x0002);                        /* SET NAMES */
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    ASSERT_EQ((ssize_t)s.len, write(sv[0], s.buf, s.len));
    HlMyDsn dsn; char err[128];
    ASSERT_EQ(0, hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err));
    HlMyConn conn; memset(&conn, 0, sizeof conn);
    ASSERT_EQ(0, hl_my_conn_start(&conn, sv[1], &dsn));
    ASSERT_EQ(0, hl_my_conn_reset(&conn));
    EXPECT_TRUE(conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT);
    char got[8192];
    size_t n = my_drain_client(sv, got, sizeof got);
    EXPECT_TRUE(my_sent_text(got, n, "autocommit = 1"));
    hl_my_conn_close(&conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* Audit 41: a statement pays HL_WORK_NET_SQL_UNITS before it is sent; a
 * run already over its budget sends nothing (the server below would never
 * answer it), and a ROLLBACK is charged but still goes. */
static int a41_units;
static int a41_fn(void *ud, int64_t units)
{
    (void)ud;
    a41_units += (int)units;
    return 1;
}

UTEST(mysql_backend, a_statement_pays_its_floor_before_the_send)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002);   /* the ROLLBACK's OK */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    a41_units = 0;
    HlDbBudgetBinding prev = hl_db_budget_swap(a41_fn, NULL);
    EXPECT_EQ(-1, mysql_exec(&h, "INSERT INTO a VALUES (1)", NULL, 0));
    EXPECT_TRUE(a41_units >= (int)HL_WORK_NET_SQL_UNITS);
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "instruction limit") != NULL);
    EXPECT_EQ(-1, mysql_query(&h, "SELECT 1", NULL, 0, NULL, NULL, NULL));
    EXPECT_EQ(-1, mysql_exec_script(&h, "SELECT 1; SELECT 2"));
    EXPECT_EQ(0, mysql_rollback(&h));
    hl_db_budget_restore(prev);
    EXPECT_FALSE(ctx.conn.broken);
    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* insert_if_absent builds its INSERT and sent it through the raw exec,
 * skipping the per-statement floor upsert paid (round 42): a loop of
 * insert_if_absent calls cost a round trip each for ~nothing. Both now pay
 * the floor before anything is sent. */
UTEST(mysql_backend, insert_if_absent_pays_its_floor_before_the_send)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    const char *cols[] = { "id" };
    HlValue vals[1] = { { .type = HL_TYPE_INT, .i = 1 } };
    a41_units = 0;
    HlDbBudgetBinding prev = hl_db_budget_swap(a41_fn, NULL);
    EXPECT_EQ(-1, mysql_insert_if_absent(&h, "a", cols, 1, cols, vals, 1));
    EXPECT_TRUE(a41_units >= (int)HL_WORK_NET_SQL_UNITS);
    EXPECT_TRUE(strstr(ctx.conn.errmsg, "instruction limit") != NULL);
    a41_units = 0;
    EXPECT_EQ(-1, mysql_upsert(&h, "a", cols, 1, cols, vals, 1));
    EXPECT_TRUE(a41_units >= (int)HL_WORK_NET_SQL_UNITS);
    hl_db_budget_restore(prev);
    EXPECT_FALSE(ctx.conn.broken);
    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

/* ── Audit convergence §7.1: roles, broken connections ────────────── */

/* The statement that puts the login's active roles back: MySQL's list is
 * taken as quoted (and refused in any other shape), MariaDB's bare name is
 * quoted here, NONE / NULL is SET ROLE NONE. */
UTEST(mysql_convergence_sql, role_restore_statement)
{
    char out[128];
    EXPECT_EQ(0, hl_my_role_restore_sql(0, "NONE", 4, out, sizeof out));
    EXPECT_STREQ("SET ROLE NONE", out);
    EXPECT_EQ(0, hl_my_role_restore_sql(1, NULL, 0, out, sizeof out));
    EXPECT_STREQ("SET ROLE NONE", out);
    const char *l = "`rr`@`%`,`a``b`@`h`";
    EXPECT_EQ(0, hl_my_role_restore_sql(0, l, strlen(l), out, sizeof out));
    EXPECT_STREQ("SET ROLE `rr`@`%`,`a``b`@`h`", out);
    EXPECT_EQ(0, hl_my_role_restore_sql(1, "r`x", 3, out, sizeof out));
    EXPECT_STREQ("SET ROLE `r``x`", out);
    /* not the server's quoted list: refused */
    const char *bad[] = { "rr", "`rr`", "`rr`@`%`;DROP", "`rr`@`%`,", "``@`%`",
                          "`rr`@`%", "`r`r`@`%`" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        EXPECT_EQ(-1, hl_my_role_restore_sql(0, bad[i], strlen(bad[i]),
                                             out, sizeof out));
    EXPECT_EQ(-1, hl_my_role_restore_sql(0, l, strlen(l), out, 12));
    EXPECT_EQ(-1, hl_my_role_restore_sql(1, "r\0x", 3, out, sizeof out));
    EXPECT_EQ(1, hl_my_roles_supported("8.0.35"));
    EXPECT_EQ(0, hl_my_roles_supported("5.7.44"));
    EXPECT_EQ(1, hl_my_roles_supported("5.5.5-10.6.18-MariaDB"));
    EXPECT_EQ(1, hl_my_roles_supported("11.4.2-MariaDB"));
    EXPECT_EQ(0, hl_my_roles_supported("5.5.5-10.0.4-MariaDB"));
}

/* One row of (CURRENT_ROLE(), DATABASE()) - NULL for a NULL argument -
 * starting at sequence 1; @p last_status is the final EOF's status (MORE_RESULTS
 * when another statement's result follows). */
static void put_login_row(HlMyWriter *s, const char *role, const char *db,
                          uint16_t last_status)
{
    size_t m = hl_my_packet_begin(s, 1);
    hl_my_put_lenenc_int(s, 2);
    hl_my_packet_end(s, m);
    put_col_def(s, 2, "CURRENT_ROLE()", HL_MY_TYPE_VAR_STRING);
    put_col_def(s, 3, "DATABASE()", HL_MY_TYPE_VAR_STRING);
    put_eof(s, 4);
    m = hl_my_packet_begin(s, 5);
    if (role) hl_my_put_lenenc_str(s, role, strlen(role));
    else hl_my_put_u8(s, 0xFB);
    if (db) hl_my_put_lenenc_str(s, db, strlen(db));
    else hl_my_put_u8(s, 0xFB);
    hl_my_packet_end(s, m);
    m = hl_my_packet_begin(s, 6);
    hl_my_put_u8(s, HL_MY_PKT_EOF);
    hl_my_put_u16(s, 0);
    hl_my_put_u16(s, last_status);
    hl_my_packet_end(s, m);
}

/* A connection whose login was captured (role @p login_role, database
 * @p login_db), then reset with the session at @p now_role / @p now_db.
 * @p restore: the server answers a second (restore) statement. Returns the
 * reset's result; the client's bytes in @p got. */
static int my_reset_with_login(const char *login_role, const char *login_db,
                               const char *now_role, const char *now_db,
                               int restore, char *got, size_t cap, size_t *n)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -9;
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);                            /* 8.0.35: roles */
    build_ok(&s, 2);
    put_login_row(&s, login_role, login_db, 0x0002);   /* capture */
    put_ok_more(&s, 1, 0x0002);                        /* RESET */
    put_login_row(&s, now_role, now_db,
                  0x0002 | HL_MY_SERVER_MORE_RESULTS); /* SELECT; SET NAMES */
    put_ok_more(&s, 7, 0x0002);
    if (restore) {
        put_ok_more(&s, 1, 0x0002 | HL_MY_SERVER_MORE_RESULTS);
        put_ok_more(&s, 2, 0x0002);
    }
    int rc = -9;
    if (write(sv[0], s.buf, s.len) == (ssize_t)s.len) {
        HlMyDsn dsn; char err[128];
        HlMyConn conn;
        if (hl_my_dsn_parse("mysql://u:p@localhost/db", &dsn, err, sizeof err) == 0 &&
            hl_my_conn_start(&conn, sv[1], &dsn) == 0) {
            if (hl_my_conn_capture_login(&conn) == 0 && conn.login_captured)
                rc = hl_my_conn_reset(&conn);
            *n = my_drain_client(sv, got, cap);
            hl_my_conn_close(&conn);
        }
    }
    hl_my_writer_free(&s);
    close(sv[0]);
    return rc;
}

/* COM_RESET_CONNECTION leaves the active role as it was: a role set through
 * PREPARE from a user variable, or by a CALLed procedure (both past the
 * session-statement refusal) carried into later entries. The reset reads
 * the role and database in its SET NAMES round trip and puts the login's
 * back - SET ROLE, then USE (MySQL drops the current database's privileges
 * at SET ROLE until the next USE). */
UTEST(mysql_convergence_sql, reset_restores_the_login_role)
{
    char got[8192]; size_t n = 0;
    EXPECT_EQ(0, my_reset_with_login("NONE", "hulldb", "`rr`@`%`", "hulldb", 1,
                                     got, sizeof got, &n));
    EXPECT_TRUE(my_sent_text(got, n, "SELECT CURRENT_ROLE(), DATABASE()"));
    EXPECT_TRUE(my_sent_command(got, n, HL_MY_COM_RESET_CONNECTION));
    EXPECT_TRUE(my_sent_text(got, n, "SELECT CURRENT_ROLE(), DATABASE(); SET NAMES"));
    EXPECT_TRUE(my_sent_text(got, n, "SET ROLE NONE; USE `hulldb`"));
}

/* A USE an entry ran is undone too (the reset keeps the current database). */
UTEST(mysql_convergence_sql, reset_restores_the_login_database)
{
    char got[8192]; size_t n = 0;
    EXPECT_EQ(0, my_reset_with_login("NONE", "hulldb", "NONE", "oth`er", 1,
                                     got, sizeof got, &n));
    EXPECT_FALSE(my_sent_text(got, n, "SET ROLE"));
    EXPECT_TRUE(my_sent_text(got, n, "USE `hulldb`"));
}

/* The session as the login left it: no second round trip. */
UTEST(mysql_convergence_sql, reset_with_nothing_changed_sets_nothing)
{
    char got[8192]; size_t n = 0;
    EXPECT_EQ(0, my_reset_with_login("`a`@`%`", "hulldb", "`a`@`%`", "hulldb", 0,
                                     got, sizeof got, &n));
    EXPECT_FALSE(my_sent_text(got, n, "SET ROLE"));
    EXPECT_FALSE(my_sent_text(got, n, "USE `"));
}

/* A database selected on a connection that started with none cannot be left
 * again: the reset fails (its caller abandons the connection). */
UTEST(mysql_convergence_sql, reset_refuses_a_database_it_cannot_leave)
{
    char got[8192]; size_t n = 0;
    EXPECT_EQ(-1, my_reset_with_login("NONE", NULL, "NONE", "other", 0,
                                      got, sizeof got, &n));
}

/* A connection marked broken whose socket is still open (a reply left
 * part-read) is closed at the entry boundary's reset: only forgotten, the
 * server kept its session - and any lock a procedure took - until the
 * slot's next call. */
UTEST(mysql_convergence_sql, reset_closes_a_broken_connection)
{
    HlMyWriter s; hl_my_writer_init(&s);
    build_handshake(&s, 0);
    build_ok(&s, 2);
    put_ok_more(&s, 1, 0x0002);                        /* DO 1 */
    HlDbHandle h; HlDbMyCtx ctx; int sv[2];
    ASSERT_EQ(0, my_backend_start(&h, &ctx, sv, &s));
    h.batch_depth = 0;
    ASSERT_TRUE(mysql_exec(&h, "DO 1", NULL, 0) >= 0);
    (void)my_pending(sv);
    ctx.conn.broken = 1;                               /* reply left part-read */
    ASSERT_TRUE(ctx.conn.transport != NULL);
    EXPECT_EQ(0, hl_db_reset_session(&h));
    EXPECT_TRUE(ctx.conn.transport == NULL);           /* closed */
    EXPECT_EQ(0, ctx.used);
    char b;
    EXPECT_EQ(0, (int)recv(sv[0], &b, 1, MSG_DONTWAIT)); /* the peer sees EOF */
    hl_my_conn_close(&ctx.conn);
    hl_my_writer_free(&s);
    close(sv[0]);
}

UTEST_MAIN()
