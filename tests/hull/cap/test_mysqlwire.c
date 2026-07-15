/*
 * test_mysqlwire.c: MySQL/MariaDB wire codec (cap/mysqlwire.c) + DSN parser
 * (cap/mysql_conn.c). Pure functions, no socket.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/mysqlwire.h"
#include "hull/cap/mysql_conn.h"
#include "hull/cap/crypto.h"
#include <string.h>

/* ── Writer / reader round-trips ──────────────────────────────────── */

UTEST(mysqlwire, int_roundtrip_little_endian)
{
    HlMyWriter w; hl_my_writer_init(&w);
    size_t m = hl_my_packet_begin(&w, 7);
    hl_my_put_u8(&w, 0xAB);
    hl_my_put_u16(&w, 0x1234);
    hl_my_put_u24(&w, 0x563412);
    hl_my_put_u32(&w, 0x89ABCDEFu);
    hl_my_put_u64(&w, 0x1122334455667788ull);
    hl_my_packet_end(&w, m);
    ASSERT_FALSE(w.err);

    HlMyFrame f; size_t consumed = 0;
    ASSERT_EQ(hl_my_frame_next(w.buf, w.len, &f, &consumed), HL_MY_OK);
    ASSERT_EQ(consumed, w.len);
    ASSERT_EQ(f.seq, 7);

    HlMyCursor c; hl_my_cursor_init(&c, &f);
    ASSERT_EQ(hl_my_get_u8(&c), 0xAB);
    ASSERT_EQ(hl_my_get_u16(&c), 0x1234);
    ASSERT_EQ(hl_my_get_u24(&c), 0x563412u);
    ASSERT_EQ(hl_my_get_u32(&c), 0x89ABCDEFu);
    ASSERT_TRUE(hl_my_get_u64(&c) == 0x1122334455667788ull);
    ASSERT_FALSE(hl_my_cursor_err(&c));
    ASSERT_EQ(c.remaining, 0u);
    hl_my_writer_free(&w);
}

/* lenenc integers across all four encodings + the boundary values. */
UTEST(mysqlwire, lenenc_int_boundaries)
{
    uint64_t vals[] = { 0, 250, 251, 0xFFFF, 0x10000, 0xFFFFFF, 0x1000000,
                        0xFFFFFFFFFFFFFFFFull };
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        HlMyWriter w; hl_my_writer_init(&w);
        size_t m = hl_my_packet_begin(&w, 0);
        hl_my_put_lenenc_int(&w, vals[i]);
        hl_my_packet_end(&w, m);
        ASSERT_FALSE(w.err);

        HlMyFrame f; size_t consumed = 0;
        ASSERT_EQ(hl_my_frame_next(w.buf, w.len, &f, &consumed), HL_MY_OK);
        HlMyCursor c; hl_my_cursor_init(&c, &f);
        int is_null = 0;
        uint64_t got = hl_my_get_lenenc_int(&c, &is_null);
        ASSERT_FALSE(hl_my_cursor_err(&c));
        ASSERT_FALSE(is_null);
        ASSERT_TRUE(got == vals[i]);
        hl_my_writer_free(&w);
    }
}

UTEST(mysqlwire, lenenc_str_and_cstr)
{
    HlMyWriter w; hl_my_writer_init(&w);
    size_t m = hl_my_packet_begin(&w, 0);
    hl_my_put_lenenc_str(&w, "hello", 5);
    hl_my_put_cstr(&w, "world");
    hl_my_packet_end(&w, m);

    HlMyFrame f; size_t consumed = 0;
    ASSERT_EQ(hl_my_frame_next(w.buf, w.len, &f, &consumed), HL_MY_OK);
    HlMyCursor c; hl_my_cursor_init(&c, &f);
    size_t slen = 0;
    const uint8_t *s = hl_my_get_lenenc_str(&c, &slen);
    ASSERT_EQ(slen, 5u);
    ASSERT_EQ(memcmp(s, "hello", 5), 0);
    const char *cs = hl_my_get_cstr(&c);
    ASSERT_STREQ(cs, "world");
    ASSERT_FALSE(hl_my_cursor_err(&c));
    hl_my_writer_free(&w);
}

/* ── Untrusted-input safety ───────────────────────────────────────── */

UTEST(mysqlwire, frame_truncation_need_more)
{
    /* Header claims 10-byte body but only 4 header bytes present. */
    uint8_t hdr[4] = { 10, 0, 0, 0 };
    HlMyFrame f; size_t consumed = 99;
    ASSERT_EQ(hl_my_frame_next(hdr, 4, &f, &consumed), HL_MY_NEED_MORE);
    ASSERT_EQ(consumed, 0u);
    /* Empty buffer is NEED_MORE, not a crash. */
    ASSERT_EQ(hl_my_frame_next(NULL, 0, &f, &consumed), HL_MY_NEED_MORE);
}

UTEST(mysqlwire, cursor_underrun_latches)
{
    uint8_t body[3] = { 1, 2, 3 };
    HlMyFrame f = { .seq = 0, .body = body, .body_len = 3 };
    HlMyCursor c; hl_my_cursor_init(&c, &f);
    (void)hl_my_get_u16(&c);              /* ok: 2 bytes */
    ASSERT_FALSE(hl_my_cursor_err(&c));
    (void)hl_my_get_u32(&c);              /* underrun: only 1 byte left */
    ASSERT_TRUE(hl_my_cursor_err(&c));
    /* Further reads stay safe + return 0. */
    ASSERT_EQ(hl_my_get_u8(&c), 0);
}

UTEST(mysqlwire, lenenc_str_hostile_length)
{
    /* A lenenc string claiming 0xFFFFFF bytes with no payload must latch err,
     * never read past the body. */
    uint8_t body[4] = { 0xFD, 0xFF, 0xFF, 0xFF };   /* lenenc int = 0xFFFFFF */
    HlMyFrame f = { .seq = 0, .body = body, .body_len = 4 };
    HlMyCursor c; hl_my_cursor_init(&c, &f);
    size_t slen = 0;
    ASSERT_TRUE(hl_my_get_lenenc_str(&c, &slen) == NULL);
    ASSERT_TRUE(hl_my_cursor_err(&c));
}

UTEST(mysqlwire, lenenc_int_null_marker)
{
    uint8_t body[1] = { 0xFB };   /* NULL marker */
    HlMyFrame f = { .seq = 0, .body = body, .body_len = 1 };
    HlMyCursor c; hl_my_cursor_init(&c, &f);
    int is_null = 0;
    (void)hl_my_get_lenenc_int(&c, &is_null);
    ASSERT_TRUE(is_null);
}

/* ── OK / ERR packet parsing ──────────────────────────────────────── */

UTEST(mysqlwire, parse_ok_packet)
{
    HlMyWriter w; hl_my_writer_init(&w);
    size_t m = hl_my_packet_begin(&w, 1);
    hl_my_put_u8(&w, HL_MY_PKT_OK);
    hl_my_put_lenenc_int(&w, 5);          /* affected_rows */
    hl_my_put_lenenc_int(&w, 42);         /* last_insert_id */
    hl_my_put_u16(&w, 0x0002);            /* status_flags */
    hl_my_put_u16(&w, 0);                 /* warnings */
    hl_my_packet_end(&w, m);

    HlMyFrame f; size_t consumed = 0;
    hl_my_frame_next(w.buf, w.len, &f, &consumed);
    HlMyOk ok;
    ASSERT_EQ(hl_my_parse_ok(&f, &ok), 0);
    ASSERT_TRUE(ok.affected_rows == 5);
    ASSERT_TRUE(ok.last_insert_id == 42);
    ASSERT_EQ(ok.status_flags, 0x0002);
    hl_my_writer_free(&w);
}

UTEST(mysqlwire, parse_err_packet_protocol41)
{
    HlMyWriter w; hl_my_writer_init(&w);
    size_t m = hl_my_packet_begin(&w, 2);
    hl_my_put_u8(&w, HL_MY_PKT_ERR);
    hl_my_put_u16(&w, 1045);              /* error code: access denied */
    hl_my_put_u8(&w, '#');
    hl_my_put_bytes(&w, "28000", 5);      /* sqlstate */
    hl_my_put_bytes(&w, "Access denied", 13);
    hl_my_packet_end(&w, m);

    HlMyFrame f; size_t consumed = 0;
    hl_my_frame_next(w.buf, w.len, &f, &consumed);
    HlMyErr e;
    ASSERT_EQ(hl_my_parse_err(&f, 1, &e), 0);
    ASSERT_EQ(e.code, 1045);
    ASSERT_STREQ(e.sqlstate, "28000");
    ASSERT_EQ(e.message_len, 13u);
    ASSERT_EQ(memcmp(e.message, "Access denied", 13), 0);
    hl_my_writer_free(&w);
}

/* ── DSN parser ───────────────────────────────────────────────────── */

UTEST(mysql_dsn, full)
{
    HlMyDsn d; char err[128];
    ASSERT_EQ(hl_my_dsn_parse("mysql://alice:s3cret@db.example.com:3307/shop"
                              "?sslmode=require", &d, err, sizeof err), 0);
    ASSERT_STREQ(d.user, "alice");
    ASSERT_STREQ(d.password, "s3cret");
    ASSERT_STREQ(d.host, "db.example.com");
    ASSERT_STREQ(d.port, "3307");
    ASSERT_STREQ(d.dbname, "shop");
    ASSERT_STREQ(d.sslmode, "require");
}

UTEST(mysql_dsn, mariadb_scheme_and_defaults)
{
    HlMyDsn d; char err[128];
    ASSERT_EQ(hl_my_dsn_parse("mariadb://root@localhost/app", &d, err, sizeof err), 0);
    ASSERT_STREQ(d.user, "root");
    ASSERT_STREQ(d.host, "localhost");
    ASSERT_STREQ(d.port, "3306");         /* default */
    ASSERT_STREQ(d.dbname, "app");
    ASSERT_STREQ(d.password, "");
}

UTEST(mysql_dsn, percent_decode)
{
    HlMyDsn d; char err[128];
    ASSERT_EQ(hl_my_dsn_parse("mysql://u%40corp:p%3Aw@h/db", &d, err, sizeof err), 0);
    ASSERT_STREQ(d.user, "u@corp");
    ASSERT_STREQ(d.password, "p:w");
}

UTEST(mysql_dsn, rejects)
{
    HlMyDsn d; char err[128];
    ASSERT_NE(hl_my_dsn_parse("postgres://u@h/db", &d, err, sizeof err), 0);   /* wrong scheme */
    ASSERT_NE(hl_my_dsn_parse("mysql://h/db", &d, err, sizeof err), 0);        /* missing user */
    ASSERT_NE(hl_my_dsn_parse("mysql://u@h:99x/db", &d, err, sizeof err), 0);  /* non-numeric port */
    ASSERT_NE(hl_my_dsn_parse("mysql://u@/db", &d, err, sizeof err), 0);       /* empty host */
    ASSERT_NE(hl_my_dsn_parse(NULL, &d, err, sizeof err), 0);
}

/* ── Handshake parse + response build ─────────────────────────────── */

UTEST(mysqlwire, parse_handshake_v10)
{
    HlMyWriter w; hl_my_writer_init(&w);
    size_t m = hl_my_packet_begin(&w, 0);
    hl_my_put_u8(&w, 10);                          /* protocol v10 */
    hl_my_put_cstr(&w, "8.0.35");                  /* server version */
    hl_my_put_u32(&w, 12345);                      /* connection id */
    for (int i = 0; i < 8; i++) hl_my_put_u8(&w, (uint8_t)(i + 1));   /* ap1 */
    hl_my_put_u8(&w, 0);                           /* filler */
    uint32_t caps = HL_MY_CLIENT_SECURE_CONNECTION | HL_MY_CLIENT_PLUGIN_AUTH
                  | HL_MY_CLIENT_PROTOCOL_41;
    hl_my_put_u16(&w, (uint16_t)(caps & 0xFFFF));  /* cap lower */
    hl_my_put_u8(&w, 45);                          /* charset */
    hl_my_put_u16(&w, 2);                          /* status */
    hl_my_put_u16(&w, (uint16_t)(caps >> 16));     /* cap upper */
    hl_my_put_u8(&w, 21);                          /* auth-plugin-data len (8+13) */
    for (int i = 0; i < 10; i++) hl_my_put_u8(&w, 0);                 /* reserved */
    for (int i = 0; i < 12; i++) hl_my_put_u8(&w, (uint8_t)(i + 9));  /* ap2 */
    hl_my_put_u8(&w, 0);                           /* ap2 NUL (13th) */
    hl_my_put_cstr(&w, "mysql_native_password");   /* plugin name */
    hl_my_packet_end(&w, m);
    ASSERT_FALSE(w.err);

    HlMyFrame f; size_t consumed = 0;
    ASSERT_EQ(hl_my_frame_next(w.buf, w.len, &f, &consumed), HL_MY_OK);
    HlMyHandshake hs;
    ASSERT_EQ(hl_my_parse_handshake(&f, &hs), 0);
    ASSERT_EQ(hs.protocol, 10);
    ASSERT_STREQ(hs.server_version, "8.0.35");
    ASSERT_EQ(hs.conn_id, 12345u);
    ASSERT_EQ(hs.scramble_len, 20);
    ASSERT_EQ(hs.scramble[0], 1);                  /* ap1[0] */
    ASSERT_EQ(hs.scramble[8], 9);                  /* ap2[0] */
    ASSERT_STREQ(hs.auth_plugin, "mysql_native_password");
    ASSERT_TRUE((hs.capabilities & HL_MY_CLIENT_PLUGIN_AUTH) != 0);
    hl_my_writer_free(&w);
}

UTEST(mysqlwire, parse_handshake_rejects_bad_protocol)
{
    uint8_t body[1] = { 9 };   /* protocol != 10 */
    HlMyFrame f = { .seq = 0, .body = body, .body_len = 1 };
    HlMyHandshake hs;
    ASSERT_EQ(hl_my_parse_handshake(&f, &hs), -1);
}

UTEST(mysqlwire, build_handshake_response_roundtrip)
{
    HlMyWriter w; hl_my_writer_init(&w);
    uint8_t auth[20]; for (int i = 0; i < 20; i++) auth[i] = (uint8_t)i;
    uint32_t caps = HL_MY_CLIENT_PROTOCOL_41 | HL_MY_CLIENT_SECURE_CONNECTION
                  | HL_MY_CLIENT_CONNECT_WITH_DB | HL_MY_CLIENT_PLUGIN_AUTH;
    hl_my_build_handshake_response(&w, 1, caps, 45, "alice", auth, 20,
                                   "shop", "mysql_native_password");
    ASSERT_FALSE(w.err);

    HlMyFrame f; size_t consumed = 0;
    ASSERT_EQ(hl_my_frame_next(w.buf, w.len, &f, &consumed), HL_MY_OK);
    ASSERT_EQ(f.seq, 1);
    HlMyCursor c; hl_my_cursor_init(&c, &f);
    ASSERT_EQ(hl_my_get_u32(&c), caps);
    (void)hl_my_get_u32(&c);                       /* max packet size */
    ASSERT_EQ(hl_my_get_u8(&c), 45);
    (void)hl_my_get_bytes(&c, 23);                 /* reserved */
    ASSERT_STREQ(hl_my_get_cstr(&c), "alice");
    ASSERT_EQ(hl_my_get_u8(&c), 20);               /* auth-response length */
    const uint8_t *a = hl_my_get_bytes(&c, 20);
    ASSERT_EQ(a[5], 5);
    ASSERT_STREQ(hl_my_get_cstr(&c), "shop");
    ASSERT_STREQ(hl_my_get_cstr(&c), "mysql_native_password");
    ASSERT_FALSE(hl_my_cursor_err(&c));
    hl_my_writer_free(&w);
}

UTEST(mysqlwire, build_ssl_request)
{
    HlMyWriter w; hl_my_writer_init(&w);
    uint32_t caps = HL_MY_CLIENT_PROTOCOL_41 | HL_MY_CLIENT_SECURE_CONNECTION
                  | HL_MY_CLIENT_SSL;
    hl_my_build_ssl_request(&w, 1, caps, 45);
    ASSERT_FALSE(w.err);

    HlMyFrame f; size_t consumed = 0;
    ASSERT_EQ(hl_my_frame_next(w.buf, w.len, &f, &consumed), HL_MY_OK);
    ASSERT_EQ(f.seq, 1);
    /* SSLRequest body is exactly caps(4) + max_packet(4) + charset(1) + 23. */
    ASSERT_EQ(f.body_len, (uint32_t)(4 + 4 + 1 + 23));
    HlMyCursor c; hl_my_cursor_init(&c, &f);
    ASSERT_EQ(hl_my_get_u32(&c), caps);
    (void)hl_my_get_u32(&c);                       /* max packet size */
    ASSERT_EQ(hl_my_get_u8(&c), 45);
    (void)hl_my_get_bytes(&c, 23);                 /* reserved */
    ASSERT_FALSE(hl_my_cursor_err(&c));
    hl_my_writer_free(&w);
}

/* ── mysql_native_password scramble (crypto) ──────────────────────── */

UTEST(mysql_auth, native_password_scramble)
{
    uint8_t scramble[20];
    for (int i = 0; i < 20; i++) scramble[i] = (uint8_t)(i * 7 + 1);
    uint8_t out[20];

    /* Empty password -> zero-length response. */
    ASSERT_EQ(hl_my_native_password_scramble("", scramble, out), 0);

    /* Non-empty -> 20 bytes matching SHA1(pw) XOR SHA1(scramble||SHA1(SHA1(pw))). */
    ASSERT_EQ(hl_my_native_password_scramble("s3cret", scramble, out), 20);
    uint8_t h1[20], h2[20], cat[40], h3[20], exp[20];
    ASSERT_EQ(hl_cap_crypto_sha1("s3cret", 6, h1), 0);
    ASSERT_EQ(hl_cap_crypto_sha1(h1, 20, h2), 0);
    memcpy(cat, scramble, 20);
    memcpy(cat + 20, h2, 20);
    ASSERT_EQ(hl_cap_crypto_sha1(cat, 40, h3), 0);
    for (int i = 0; i < 20; i++) exp[i] = (uint8_t)(h1[i] ^ h3[i]);
    ASSERT_EQ(memcmp(out, exp, 20), 0);
}

UTEST_MAIN()
