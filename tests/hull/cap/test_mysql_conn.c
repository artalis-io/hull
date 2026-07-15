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

static void build_handshake(HlMyWriter *w, uint8_t seq)
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
    hl_my_put_cstr(w, "mysql_native_password");
    hl_my_packet_end(w, m);
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

UTEST_MAIN()
