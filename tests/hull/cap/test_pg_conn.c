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
#include "hull/cap/pgwire.h"

#include <stdint.h>
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

/* ── Placeholder rewriting ────────────────────────────────────────── */

UTEST(pg_rewrite, placeholders_and_literals)
{
    char out[512];
    int n;

    ASSERT_EQ(0, hl_pg_rewrite_sql("a = ? AND b = ?", out, sizeof out, &n));
    ASSERT_STREQ(out, "a = $1 AND b = $2");
    ASSERT_EQ(n, 2);

    ASSERT_EQ(0, hl_pg_rewrite_sql("SELECT '?' , ?", out, sizeof out, &n));
    ASSERT_STREQ(out, "SELECT '?' , $1");
    ASSERT_EQ(n, 1);

    ASSERT_EQ(0, hl_pg_rewrite_sql("SELECT \"c?\" , ?", out, sizeof out, &n));
    ASSERT_STREQ(out, "SELECT \"c?\" , $1");
    ASSERT_EQ(n, 1);

    ASSERT_EQ(0, hl_pg_rewrite_sql("SELECT 'a''?b', ?", out, sizeof out, &n));
    ASSERT_STREQ(out, "SELECT 'a''?b', $1");
    ASSERT_EQ(n, 1);

    ASSERT_EQ(0, hl_pg_rewrite_sql("x -- ? c\n, ?", out, sizeof out, &n));
    ASSERT_STREQ(out, "x -- ? c\n, $1");
    ASSERT_EQ(n, 1);

    ASSERT_EQ(0, hl_pg_rewrite_sql("x /* ? */ , ?", out, sizeof out, &n));
    ASSERT_STREQ(out, "x /* ? */ , $1");
    ASSERT_EQ(n, 1);

    ASSERT_EQ(0, hl_pg_rewrite_sql("x $$ ? $$ , ?", out, sizeof out, &n));
    ASSERT_STREQ(out, "x $$ ? $$ , $1");
    ASSERT_EQ(n, 1);

    ASSERT_EQ(0, hl_pg_rewrite_sql("x $tag$ ? $tag$, ?", out, sizeof out, &n));
    ASSERT_STREQ(out, "x $tag$ ? $tag$, $1");
    ASSERT_EQ(n, 1);

    ASSERT_EQ(0, hl_pg_rewrite_sql("SELECT 1", out, sizeof out, &n));
    ASSERT_STREQ(out, "SELECT 1");
    ASSERT_EQ(n, 0);
}

UTEST(pg_rewrite, overflow_fails_closed)
{
    char small[4];
    int n;
    ASSERT_EQ(-1, hl_pg_rewrite_sql("SELECT ?", small, sizeof small, &n));
}

/* ── Query exchange over a socketpair ─────────────────────────────── */

struct qcollect {
    int  nrows;
    char v[8][2][64];
    int  isnull[8][2];
    int  nfields;
    char field0[32];
    char field1[32];
};

static void q_desc(void *ctx, const HlPgField *f, int nf)
{
    struct qcollect *co = ctx;
    co->nfields = nf;
    if (nf > 0) snprintf(co->field0, sizeof co->field0, "%s", f[0].name);
    if (nf > 1) snprintf(co->field1, sizeof co->field1, "%s", f[1].name);
}

static int q_row(void *ctx, const char *const *vals, const int32_t *lens, int nc)
{
    struct qcollect *co = ctx;
    if (co->nrows >= 8) return 1;
    int r = co->nrows++;
    for (int i = 0; i < nc && i < 2; i++) {
        if (!vals[i]) {
            co->isnull[r][i] = 1;
        } else {
            int l = lens[i] < 63 ? lens[i] : 63;
            memcpy(co->v[r][i], vals[i], (size_t)l);
            co->v[r][i][l] = '\0';
        }
    }
    return 0;
}

UTEST(pg_query, select_rows_and_affected)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlPgWriter s;
    hl_pg_writer_init(&s);
    size_t m;
    m = hl_pg_msg_begin(&s, '1'); hl_pg_msg_end(&s, m);   /* ParseComplete */
    m = hl_pg_msg_begin(&s, '2'); hl_pg_msg_end(&s, m);   /* BindComplete  */
    m = hl_pg_msg_begin(&s, 'T');                          /* RowDescription */
    hl_pg_put_i16(&s, 2);
    hl_pg_put_cstr(&s, "id");
    hl_pg_put_i32(&s, 0); hl_pg_put_i16(&s, 0); hl_pg_put_i32(&s, 23);
    hl_pg_put_i16(&s, 4); hl_pg_put_i32(&s, -1); hl_pg_put_i16(&s, 0);
    hl_pg_put_cstr(&s, "name");
    hl_pg_put_i32(&s, 0); hl_pg_put_i16(&s, 0); hl_pg_put_i32(&s, 25);
    hl_pg_put_i16(&s, -1); hl_pg_put_i32(&s, -1); hl_pg_put_i16(&s, 0);
    hl_pg_msg_end(&s, m);
    m = hl_pg_msg_begin(&s, 'D');                          /* DataRow 1 */
    hl_pg_put_i16(&s, 2);
    hl_pg_put_i32(&s, 1); hl_pg_put_bytes(&s, "1", 1);
    hl_pg_put_i32(&s, 5); hl_pg_put_bytes(&s, "alice", 5);
    hl_pg_msg_end(&s, m);
    m = hl_pg_msg_begin(&s, 'D');                          /* DataRow 2, NULL name */
    hl_pg_put_i16(&s, 2);
    hl_pg_put_i32(&s, 1); hl_pg_put_bytes(&s, "2", 1);
    hl_pg_put_i32(&s, -1);
    hl_pg_msg_end(&s, m);
    m = hl_pg_msg_begin(&s, 'C'); hl_pg_put_cstr(&s, "SELECT 2"); hl_pg_msg_end(&s, m);
    m = hl_pg_msg_begin(&s, 'Z'); hl_pg_put_u8(&s, 'I'); hl_pg_msg_end(&s, m);
    ASSERT_FALSE(s.err);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);
    hl_pg_writer_free(&s);

    HlPgConn conn;
    memset(&conn, 0, sizeof conn);
    conn.fd = sv[1];

    struct qcollect co;
    memset(&co, 0, sizeof co);
    HlPgParam p = { .text = "7", .len = 1 };
    int64_t affected = -1;
    ASSERT_EQ(0, hl_pg_query(&conn, "SELECT id, name FROM t WHERE id > ?",
                             &p, 1, q_desc, q_row, &co, &affected));

    ASSERT_EQ(co.nfields, 2);
    ASSERT_STREQ(co.field0, "id");
    ASSERT_STREQ(co.field1, "name");
    ASSERT_EQ(co.nrows, 2);
    ASSERT_STREQ(co.v[0][0], "1");
    ASSERT_STREQ(co.v[0][1], "alice");
    ASSERT_STREQ(co.v[1][0], "2");
    ASSERT_EQ(co.isnull[1][1], 1);
    ASSERT_EQ(affected, (int64_t)2);

    /* The client sent the rewritten SQL (with $1) and the bound value. */
    uint8_t got[512];
    ssize_t n = read(sv[0], got, sizeof got);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(bytes_contain(got, (size_t)n, "id > $1"));

    hl_pg_conn_close(&conn);
    close(sv[0]);
}

UTEST(pg_query, server_error_then_ready)
{
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    HlPgWriter s;
    hl_pg_writer_init(&s);
    size_t m;
    m = hl_pg_msg_begin(&s, '1'); hl_pg_msg_end(&s, m);    /* ParseComplete */
    m = hl_pg_msg_begin(&s, 'E');                           /* ErrorResponse */
    hl_pg_put_u8(&s, 'M');
    hl_pg_put_cstr(&s, "syntax error");
    hl_pg_put_u8(&s, 0);                                    /* field terminator */
    hl_pg_msg_end(&s, m);
    m = hl_pg_msg_begin(&s, 'Z'); hl_pg_put_u8(&s, 'E'); hl_pg_msg_end(&s, m);
    ASSERT_FALSE(s.err);
    ASSERT_TRUE(write(sv[0], s.buf, s.len) == (ssize_t)s.len);
    hl_pg_writer_free(&s);

    HlPgConn conn;
    memset(&conn, 0, sizeof conn);
    conn.fd = sv[1];

    HlPgParam p = { .text = "7", .len = 1 };
    ASSERT_EQ(-1, hl_pg_query(&conn, "SELECT ?", &p, 1, NULL, NULL, NULL, NULL));
    ASSERT_TRUE(strstr(conn.errmsg, "syntax error") != NULL);

    hl_pg_conn_close(&conn);
    close(sv[0]);
}

/* ── TLS negotiation (SSLRequest / sslmode) ───────────────────────── */

UTEST(pg_ssl, sslmode_parse)
{
    ASSERT_EQ(hl_pg_sslmode_parse(NULL), (int)HL_PG_SSLMODE_DISABLE);
    ASSERT_EQ(hl_pg_sslmode_parse(""), (int)HL_PG_SSLMODE_DISABLE);
    ASSERT_EQ(hl_pg_sslmode_parse("disable"), (int)HL_PG_SSLMODE_DISABLE);
    ASSERT_EQ(hl_pg_sslmode_parse("prefer"), (int)HL_PG_SSLMODE_PREFER);
    ASSERT_EQ(hl_pg_sslmode_parse("require"), (int)HL_PG_SSLMODE_REQUIRE);
    ASSERT_EQ(hl_pg_sslmode_parse("verify-full"), (int)HL_PG_SSLMODE_VERIFY);
    ASSERT_EQ(hl_pg_sslmode_parse("bogus"), -1);
}

static HlPgSslDecision negotiate_with(HlPgSslMode mode, int have_resp, char resp)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return HL_PG_SSL_FAIL;
    if (have_resp) { ssize_t w = write(sv[0], &resp, 1); (void)w; }
    char e[128] = {0};
    HlPgSslDecision d = hl_pg_ssl_negotiate(sv[1], mode, e, sizeof e);
    close(sv[0]);
    close(sv[1]);
    return d;
}

UTEST(pg_ssl, negotiation_matrix)
{
    /* disable: no SSLRequest sent, immediate plaintext. */
    ASSERT_EQ(negotiate_with(HL_PG_SSLMODE_DISABLE, 0, 0), HL_PG_SSL_PLAINTEXT);
    /* prefer: 'S' uses TLS, 'N' falls back to plaintext. */
    ASSERT_EQ(negotiate_with(HL_PG_SSLMODE_PREFER, 1, 'S'), HL_PG_SSL_USE_TLS);
    ASSERT_EQ(negotiate_with(HL_PG_SSLMODE_PREFER, 1, 'N'), HL_PG_SSL_PLAINTEXT);
    /* require: 'S' uses TLS, 'N' is a hard error. */
    ASSERT_EQ(negotiate_with(HL_PG_SSLMODE_REQUIRE, 1, 'S'), HL_PG_SSL_USE_TLS);
    ASSERT_EQ(negotiate_with(HL_PG_SSLMODE_REQUIRE, 1, 'N'), HL_PG_SSL_FAIL);
    /* verify: 'N' is a hard error too. */
    ASSERT_EQ(negotiate_with(HL_PG_SSLMODE_VERIFY, 1, 'N'), HL_PG_SSL_FAIL);
    /* unexpected reply byte is an error. */
    ASSERT_EQ(negotiate_with(HL_PG_SSLMODE_PREFER, 1, 'X'), HL_PG_SSL_FAIL);
}

/* ── SCRAM-SHA-256 ────────────────────────────────────────────────── */

UTEST(pg_scram, base64_roundtrip)
{
    /* RFC 4648 test vectors. */
    static const char *in[]  = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *out[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==",
                                 "Zm9vYmE=", "Zm9vYmFy" };
    for (int i = 0; i < 7; i++) {
        char enc[32];
        int el = hl_pg_b64_encode((const uint8_t *)in[i], strlen(in[i]),
                                  enc, sizeof enc);
        ASSERT_TRUE(el >= 0);
        ASSERT_STREQ(enc, out[i]);

        uint8_t dec[32];
        size_t dl = 0;
        ASSERT_EQ(0, hl_pg_b64_decode(enc, strlen(enc), dec, sizeof dec, &dl));
        ASSERT_EQ(dl, strlen(in[i]));
        if (dl) ASSERT_EQ(0, memcmp(dec, in[i], dl));
    }
}

UTEST(pg_scram, rfc7677_vector)
{
    /* RFC 7677 SCRAM-SHA-256 worked example (user "user", password "pencil"). */
    uint8_t salt[64];
    size_t salt_len = 0;
    ASSERT_EQ(0, hl_pg_b64_decode("W22ZaJ0SNY7soEsUEjb6gQ==", 24,
                                  salt, sizeof salt, &salt_len));

    const char *auth_msg =
        "n=user,r=rOprNGfwEbeRWgbNEkqO,"
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
        "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096,"
        "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0";

    uint8_t proof[32], server_sig[32];
    ASSERT_EQ(0, hl_pg_scram_proof("pencil", 6, salt, salt_len, 4096,
                                   auth_msg, strlen(auth_msg), proof, server_sig));

    char proof_b64[64], sig_b64[64];
    ASSERT_TRUE(hl_pg_b64_encode(proof, 32, proof_b64, sizeof proof_b64) >= 0);
    ASSERT_TRUE(hl_pg_b64_encode(server_sig, 32, sig_b64, sizeof sig_b64) >= 0);
    ASSERT_STREQ(proof_b64, "dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=");
    ASSERT_STREQ(sig_b64, "6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=");
}

UTEST_MAIN()
