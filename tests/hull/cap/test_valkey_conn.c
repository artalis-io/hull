/*
 * test_valkey_conn.c: Valkey/Redis connection handshake + command round-trip.
 *
 * Drives hl_valkey_conn_start over a socketpair whose "server" end has canned
 * RESP replies pre-written (the client's requests are buffered and ignored):
 * RESP3 HELLO, AUTH inside HELLO, RESP2 fallback on an unknown HELLO, legacy
 * AUTH, SELECT, an auth failure, and a GET round-trip. No sockets to the
 * network, no TLS (-DHL_VALKEY_NO_TLS).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/valkey_conn.h"
#include "hull/cap/respwire.h"
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A minimal RESP3 HELLO reply (a 1-entry map). */
#define HELLO_MAP "%1\r\n$6\r\nserver\r\n$5\r\nredis\r\n"

static int dsn_of(const char *s, HlValkeyDsn *d) {
    char e[128];
    return hl_valkey_dsn_parse(s, d, e, sizeof e);
}

UTEST(valkey_conn, handshake_resp3) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    ASSERT_EQ((ssize_t)strlen(HELLO_MAP), write(sv[0], HELLO_MAP, strlen(HELLO_MAP)));
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://localhost", &d));
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(0, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));
    ASSERT_EQ(1, hl_valkey_conn_is_resp3(c));
    hl_valkey_conn_close(c);
    close(sv[0]);
}

UTEST(valkey_conn, handshake_with_auth_in_hello) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    /* Server accepts HELLO 3 AUTH default secret -> RESP3 map. */
    write(sv[0], HELLO_MAP, strlen(HELLO_MAP));
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://:secret@localhost", &d));
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(0, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));
    ASSERT_EQ(1, hl_valkey_conn_is_resp3(c));
    hl_valkey_conn_close(c);
    close(sv[0]);
}

UTEST(valkey_conn, resp2_fallback_on_unknown_hello) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    const char *err = "-ERR unknown command 'HELLO'\r\n";
    write(sv[0], err, strlen(err));
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://localhost", &d));   /* no pass */
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(0, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));
    ASSERT_EQ(0, hl_valkey_conn_is_resp3(c));                       /* fell back */
    hl_valkey_conn_close(c);
    close(sv[0]);
}

UTEST(valkey_conn, resp2_fallback_then_legacy_auth) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    const char *err = "-ERR unknown command 'HELLO'\r\n";
    const char *ok  = "+OK\r\n";
    write(sv[0], err, strlen(err));   /* HELLO rejected */
    write(sv[0], ok, strlen(ok));     /* AUTH secret -> +OK */
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://:secret@localhost", &d));
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(0, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));
    ASSERT_EQ(0, hl_valkey_conn_is_resp3(c));
    hl_valkey_conn_close(c);
    close(sv[0]);
}

UTEST(valkey_conn, select_db) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    const char *ok = "+OK\r\n";
    write(sv[0], HELLO_MAP, strlen(HELLO_MAP));   /* HELLO */
    write(sv[0], ok, strlen(ok));                 /* SELECT 3 -> +OK */
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://localhost/3", &d));
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(0, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));
    hl_valkey_conn_close(c);
    close(sv[0]);
}

UTEST(valkey_conn, auth_failure_rejected) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    const char *err = "-WRONGPASS invalid username-password pair\r\n";
    write(sv[0], err, strlen(err));
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://:bad@localhost", &d));
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(-1, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));  /* not unknown-cmd -> fail */
    close(sv[1]);
    close(sv[0]);
}

UTEST(valkey_conn, command_get_roundtrip) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    const char *getr = "$5\r\nhello\r\n";
    write(sv[0], HELLO_MAP, strlen(HELLO_MAP));   /* HELLO */
    write(sv[0], getr, strlen(getr));             /* GET reply */
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://localhost", &d));
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(0, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));

    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "GET");
    hl_resp_cmd_arg_cstr(&w, "k");
    HlRespValue reply;
    ASSERT_EQ(0, hl_valkey_command(c, &w, &reply));
    ASSERT_EQ((int)reply.type, (int)HL_RESP_STR);
    ASSERT_EQ(reply.str.len, (size_t)5);
    ASSERT_EQ(0, memcmp(reply.str.p, "hello", 5));
    hl_resp_writer_free(&w);

    hl_valkey_conn_close(c);
    close(sv[0]);
}

UTEST_MAIN();
