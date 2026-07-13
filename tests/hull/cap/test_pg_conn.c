/*
 * test_pg_conn.c: PostgreSQL DSN parsing + startup handshake tests
 *
 * The handshake tests drive the real receive/frame/dispatch loop over a
 * socketpair: canned server bytes are queued on one end and hl_pg_conn_start
 * runs on the other, so the trust / cleartext / error paths are exercised
 * against a genuine (if local) socket without a PostgreSQL server.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/pg_conn.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* ── DSN parsing ──────────────────────────────────────────────────── */

UTEST(pg_dsn, full_url)
{
    HlPgDsn d;
    char e[128];
    ASSERT_EQ(0, hl_pg_dsn_parse(
        "postgres://alice:s3cret@db.example.com:6543/shop?sslmode=require",
        &d, e, sizeof e));
    ASSERT_STREQ(d.user, "alice");
    ASSERT_STREQ(d.password, "s3cret");
    ASSERT_STREQ(d.host, "db.example.com");
    ASSERT_STREQ(d.port, "6543");
    ASSERT_STREQ(d.dbname, "shop");
    ASSERT_STREQ(d.sslmode, "require");
}

UTEST(pg_dsn, minimal_defaults_port)
{
    HlPgDsn d;
    ASSERT_EQ(0, hl_pg_dsn_parse("postgresql://bob@localhost/mydb", &d, NULL, 0));
    ASSERT_STREQ(d.user, "bob");
    ASSERT_STREQ(d.host, "localhost");
    ASSERT_STREQ(d.port, "5432");
    ASSERT_STREQ(d.dbname, "mydb");
    ASSERT_STREQ(d.password, "");
}

UTEST(pg_dsn, percent_decoded_password)
{
    HlPgDsn d;
    ASSERT_EQ(0, hl_pg_dsn_parse("postgres://u:p%40ss%3Aword@h/db", &d, NULL, 0));
    ASSERT_STREQ(d.password, "p@ss:word");
}

UTEST(pg_dsn, rejects_bad_scheme)
{
    HlPgDsn d;
    char e[128];
    ASSERT_EQ(-1, hl_pg_dsn_parse("mysql://u@h/db", &d, e, sizeof e));
}

UTEST(pg_dsn, rejects_missing_user)
{
    HlPgDsn d;
    ASSERT_EQ(-1, hl_pg_dsn_parse("postgres://localhost/db", &d, NULL, 0));
}

UTEST(pg_dsn, rejects_nonnumeric_port)
{
    HlPgDsn d;
    ASSERT_EQ(-1, hl_pg_dsn_parse("postgres://u@h:abc/db", &d, NULL, 0));
}

UTEST(pg_dsn, scrub_clears_password)
{
    HlPgDsn d;
    ASSERT_EQ(0, hl_pg_dsn_parse("postgres://u:pw@h/db", &d, NULL, 0));
    ASSERT_STREQ(d.password, "pw");
    hl_pg_dsn_scrub(&d);
    ASSERT_EQ(d.password[0], 0);
}

/* ── Handshake over a socketpair ──────────────────────────────────── */

static int bytes_contain(const uint8_t *hay, size_t hlen,
                         const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return 1;
    return 0;
}

UTEST(pg_conn, handshake_trust_auth)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    static const uint8_t resp[] = {
        'R', 0,0,0,8,  0,0,0,0,                      /* AuthenticationOk */
        'S', 0,0,0,8,  'x',0,'y',0,                  /* ParameterStatus  */
        'K', 0,0,0,12, 0,0,0x12,0x34, 0,0,0xab,0xcd, /* BackendKeyData   */
        'Z', 0,0,0,5,  'I',                          /* ReadyForQuery    */
    };
    ASSERT_TRUE(write(sv[0], resp, sizeof resp) == (ssize_t)sizeof resp);

    HlPgDsn dsn;
    memset(&dsn, 0, sizeof dsn);
    snprintf(dsn.user, sizeof dsn.user, "%s", "app");

    HlPgConn conn;
    ASSERT_EQ(0, hl_pg_conn_start(&conn, sv[1], &dsn));
    ASSERT_EQ(conn.tx_status, (int)'I');
    ASSERT_EQ(conn.backend_pid, 0x1234);
    ASSERT_EQ(conn.backend_key, 0xabcd);

    hl_pg_conn_close(&conn);   /* closes sv[1] */
    close(sv[0]);
}

UTEST(pg_conn, handshake_cleartext_auth)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    static const uint8_t resp[] = {
        'R', 0,0,0,8, 0,0,0,3,   /* AuthenticationCleartextPassword */
        'R', 0,0,0,8, 0,0,0,0,   /* AuthenticationOk                */
        'Z', 0,0,0,5, 'I',       /* ReadyForQuery                   */
    };
    ASSERT_TRUE(write(sv[0], resp, sizeof resp) == (ssize_t)sizeof resp);

    HlPgDsn dsn;
    memset(&dsn, 0, sizeof dsn);
    snprintf(dsn.user, sizeof dsn.user, "%s", "app");
    snprintf(dsn.password, sizeof dsn.password, "%s", "secret");

    HlPgConn conn;
    ASSERT_EQ(0, hl_pg_conn_start(&conn, sv[1], &dsn));

    /* The client must have sent a PasswordMessage carrying the password. */
    uint8_t got[512];
    ssize_t n = read(sv[0], got, sizeof got);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(bytes_contain(got, (size_t)n, "secret"));

    hl_pg_conn_close(&conn);
    close(sv[0]);
}

UTEST(pg_conn, handshake_server_error)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    /* ErrorResponse with a single M (message) field "boom". */
    static const uint8_t resp[] = {
        'E', 0,0,0,11, 'M','b','o','o','m',0, 0,
    };
    ASSERT_TRUE(write(sv[0], resp, sizeof resp) == (ssize_t)sizeof resp);

    HlPgDsn dsn;
    memset(&dsn, 0, sizeof dsn);
    snprintf(dsn.user, sizeof dsn.user, "%s", "app");

    HlPgConn conn;
    ASSERT_EQ(-1, hl_pg_conn_start(&conn, sv[1], &dsn));
    ASSERT_TRUE(strstr(conn.errmsg, "boom") != NULL);

    close(sv[0]);   /* sv[1] already closed by the failed handshake */
}

UTEST_MAIN()
