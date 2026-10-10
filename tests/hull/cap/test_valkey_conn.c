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
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
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

/* A failed read leaves the stream out of step: what arrives next answers the
 * earlier command. The connection refuses every later command instead of
 * handing that reply to it - here a valid GET reply is already waiting behind
 * the malformed one, and the second command must not take it. */
UTEST(valkey_conn, a_failed_reply_breaks_the_connection) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    const char *bad  = "!not a resp reply\r\n";
    const char *next = "$5\r\nhello\r\n";
    write(sv[0], HELLO_MAP, strlen(HELLO_MAP));
    write(sv[0], bad, strlen(bad));
    write(sv[0], next, strlen(next));
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://localhost", &d));
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(0, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));

    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "GET");
    hl_resp_cmd_arg_cstr(&w, "a");
    HlRespValue reply;
    EXPECT_EQ(-1, hl_valkey_command(c, &w, &reply));
    hl_resp_writer_free(&w);

    hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "GET");
    hl_resp_cmd_arg_cstr(&w, "b");
    EXPECT_EQ(-1, hl_valkey_command(c, &w, &reply));
    const char *err = hl_valkey_conn_error(c);
    EXPECT_TRUE(err && strstr(err, "reopen") != NULL);
    hl_resp_writer_free(&w);

    hl_valkey_conn_close(c);
    close(sv[0]);
}

/* ── Per-command deadline + piecewise replies (audit 12) ──────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

typedef struct {
    int fd;
    const char *data;
    size_t len;
    size_t chunk;
    int delay_ms;
    atomic_int stop;
} Dripper;

static void *drip(void *arg) {
    Dripper *d = (Dripper *)arg;
    size_t off = 0;
    while (off < d->len && !atomic_load(&d->stop)) {
        size_t n = d->len - off < d->chunk ? d->len - off : d->chunk;
        ssize_t w = write(d->fd, d->data + off, n);
        if (w <= 0) break;
        off += (size_t)w;
        if (d->delay_ms > 0) {
            struct timespec ts = { 0, (long)d->delay_ms * 1000000L };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

/* A server that sends a byte every 50 ms never trips a per-recv timeout; the
 * command's deadline (the DSN's timeout, 300 ms) ends it. */
UTEST(valkey_conn, command_deadline_bounds_a_dripping_reply) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    write(sv[0], HELLO_MAP, strlen(HELLO_MAP));
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://localhost?connect_timeout=300", &d));
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(0, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));

    static char body[64];
    memset(body, 'a', sizeof body);
    memcpy(body, "$100000\r\n", 9);
    Dripper dr = { .fd = sv[0], .data = body, .len = sizeof body,
                   .chunk = 1, .delay_ms = 50 };
    atomic_init(&dr.stop, 0);
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, drip, &dr));

    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "GET");
    hl_resp_cmd_arg_cstr(&w, "k");
    HlRespValue reply;
    uint64_t t0 = now_ms();
    int rc = hl_valkey_command(c, &w, &reply);
    uint64_t took = now_ms() - t0;
    hl_resp_writer_free(&w);
    atomic_store(&dr.stop, 1);
    pthread_join(th, NULL);

    EXPECT_EQ(-1, rc);
    EXPECT_TRUE(strstr(hl_valkey_conn_error(c), "timed out") != NULL);
    EXPECT_TRUE(took >= 250);
    EXPECT_TRUE(took < 2500);   /* the drip alone runs 64 x 50 ms = 3.2 s */

    hl_valkey_conn_close(c);
    close(sv[0]);
}

/* A large aggregate arriving 7 bytes at a time still decodes whole: the
 * re-parse is deferred while more bytes are waiting, never skipped once the
 * reply is complete. */
UTEST(valkey_conn, piecewise_aggregate_reply_decodes) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    write(sv[0], HELLO_MAP, strlen(HELLO_MAP));
    HlValkeyDsn d; ASSERT_EQ(0, dsn_of("redis://localhost", &d));
    HlValkeyConn *c = NULL; char e[128];
    ASSERT_EQ(0, hl_valkey_conn_start(&c, sv[1], &d, NULL, e, sizeof e));

    enum { N = 2000 };
    static char big[16 + N * 9];
    size_t len = (size_t)snprintf(big, sizeof big, "*%d\r\n", N);
    for (int i = 0; i < N; i++) { memcpy(big + len, "$3\r\nabc\r\n", 9); len += 9; }
    Dripper dr = { .fd = sv[0], .data = big, .len = len, .chunk = 7, .delay_ms = 0 };
    atomic_init(&dr.stop, 0);
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, drip, &dr));

    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "LRANGE");
    hl_resp_cmd_arg_cstr(&w, "k");
    HlRespValue reply;
    int rc = hl_valkey_command(c, &w, &reply);
    hl_resp_writer_free(&w);
    pthread_join(th, NULL);

    ASSERT_EQ(0, rc);
    ASSERT_EQ((int)reply.type, (int)HL_RESP_ARRAY);
    ASSERT_EQ((size_t)reply.arr.count, (size_t)N);
    ASSERT_EQ(reply.arr.items[N - 1].str.len, (size_t)3);
    ASSERT_EQ(0, memcmp(reply.arr.items[N - 1].str.p, "abc", 3));

    hl_valkey_conn_close(c);
    close(sv[0]);
}

UTEST_MAIN();
