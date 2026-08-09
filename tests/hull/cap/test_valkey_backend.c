/*
 * test_valkey_backend.c: the HlKvBackend op->RESP mapping over a socketpair.
 *
 * A fake server pre-writes canned RESP replies in the exact order the ops send
 * their commands; the test drives the vtable (get/set/del/exists/incr, CAS
 * set-if-absent + WATCH/MULTI compare, and prefix-stripping SCAN) and checks
 * the decoded results. No live server (that's e2e_valkey.sh).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/valkey.h"
#include "hull/cap/valkey_conn.h"
#include "hull/cap/kv_backend.h"
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HELLO_MAP "%1\r\n$6\r\nserver\r\n$5\r\nredis\r\n"
#define WR(fd, s) write((fd), (s), strlen(s))

/* Collect scan keys. */
struct keys { char buf[8][64]; size_t n; };
static int collect(void *ctx, const uint8_t *k, size_t klen) {
    struct keys *ks = (struct keys *)ctx;
    if (ks->n < 8 && klen < 64) { memcpy(ks->buf[ks->n], k, klen); ks->buf[ks->n][klen] = '\0'; ks->n++; }
    return 0;
}

UTEST(valkey_backend, ops_roundtrip) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    const HlKvBackend *B = &hl_kv_backend_valkey;

    /* Pre-write, in op order: HELLO, then each op's reply(s). */
    WR(sv[0], HELLO_MAP);                 /* handshake */
    WR(sv[0], "$5\r\nhello\r\n");          /* get hit    */
    WR(sv[0], "_\r\n");                    /* get miss   */
    WR(sv[0], "+OK\r\n");                  /* set        */
    WR(sv[0], ":1\r\n");                   /* del        */
    WR(sv[0], ":1\r\n");                   /* exists     */
    WR(sv[0], ":5\r\n");                   /* incr       */
    WR(sv[0], "+OK\r\n");                  /* cas set-if-absent OK */
    WR(sv[0], "_\r\n");                    /* cas set-if-absent MISMATCH */
    /* compare-CAS: WATCH, GET(=v1), MULTI, SET(QUEUED), EXEC(array) */
    WR(sv[0], "+OK\r\n"); WR(sv[0], "$2\r\nv1\r\n"); WR(sv[0], "+OK\r\n");
    WR(sv[0], "+QUEUED\r\n"); WR(sv[0], "*1\r\n+OK\r\n");
    /* scan: one page, cursor 0, two prefixed keys */
    WR(sv[0], "*2\r\n$1\r\n0\r\n*2\r\n$8\r\nkv:ns:k1\r\n$8\r\nkv:ns:k2\r\n");

    HlValkeyDsn d; char e[128];
    ASSERT_EQ(0, hl_valkey_dsn_parse("redis://localhost", &d, e, sizeof e));
    HlValkeyConn *conn = NULL;
    ASSERT_EQ(0, hl_valkey_conn_start(&conn, sv[1], &d, NULL, e, sizeof e));
    HlKvHandle *h = hl_kv_valkey_wrap(conn);
    ASSERT_TRUE(h != NULL);

    const uint8_t *val; size_t vlen; int found;
    ASSERT_EQ(0, B->get(h, (const uint8_t *)"k", 1, &val, &vlen, &found));
    ASSERT_EQ(found, 1); ASSERT_EQ(vlen, (size_t)5); ASSERT_EQ(0, memcmp(val, "hello", 5));

    ASSERT_EQ(0, B->get(h, (const uint8_t *)"m", 1, &val, &vlen, &found));
    ASSERT_EQ(found, 0);

    ASSERT_EQ(0, B->set(h, (const uint8_t *)"k", 1, (const uint8_t *)"v", 1, 0));

    int deleted = 0;
    ASSERT_EQ(0, B->del(h, (const uint8_t *)"k", 1, &deleted));
    ASSERT_EQ(deleted, 1);

    int present = 0;
    ASSERT_EQ(0, B->exists(h, (const uint8_t *)"k", 1, &present));
    ASSERT_EQ(present, 1);

    int64_t nv = 0;
    ASSERT_EQ(0, B->incr(h, (const uint8_t *)"n", 1, 5, 0, &nv));
    ASSERT_EQ(nv, (int64_t)5);

    ASSERT_EQ((int)HL_KV_CAS_OK,
              (int)B->cas(h, (const uint8_t *)"c", 1, NULL, 0, 0, (const uint8_t *)"v1", 2, 0));
    ASSERT_EQ((int)HL_KV_CAS_MISMATCH,
              (int)B->cas(h, (const uint8_t *)"c", 1, NULL, 0, 0, (const uint8_t *)"v2", 2, 0));
    ASSERT_EQ((int)HL_KV_CAS_OK,
              (int)B->cas(h, (const uint8_t *)"c", 1, (const uint8_t *)"v1", 2, 1,
                          (const uint8_t *)"v2", 2, 0));

    struct keys ks = {0};
    ASSERT_EQ(0, B->scan(h, (const uint8_t *)"kv:ns:", 6, 0, collect, &ks));
    ASSERT_EQ(ks.n, (size_t)2);
    ASSERT_STREQ(ks.buf[0], "k1");
    ASSERT_STREQ(ks.buf[1], "k2");

    B->close(h);
    close(sv[0]);
}

/* incr with a TTL on a FRESH key runs the WATCH/EXISTS/MULTI/INCRBY/PEXPIRE/
 * EXEC transaction; the returned value is EXEC's array item 0. */
UTEST(valkey_backend, incr_with_ttl_fresh_is_transactional) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    WR(sv[0], HELLO_MAP);
    WR(sv[0], "+OK\r\n");            /* WATCH   */
    WR(sv[0], ":0\r\n");             /* EXISTS -> fresh */
    WR(sv[0], "+OK\r\n");            /* MULTI   */
    WR(sv[0], "+QUEUED\r\n");        /* INCRBY  */
    WR(sv[0], "+QUEUED\r\n");        /* PEXPIRE (queued because fresh) */
    WR(sv[0], "*2\r\n:5\r\n:1\r\n"); /* EXEC -> [INCRBY=5, PEXPIRE=1] */

    HlValkeyDsn d; char e[128];
    ASSERT_EQ(0, hl_valkey_dsn_parse("redis://localhost", &d, e, sizeof e));
    HlValkeyConn *conn = NULL;
    ASSERT_EQ(0, hl_valkey_conn_start(&conn, sv[1], &d, NULL, e, sizeof e));
    HlKvHandle *h = hl_kv_valkey_wrap(conn);

    int64_t nv = 0;
    ASSERT_EQ(0, hl_kv_backend_valkey.incr(h, (const uint8_t *)"n", 1, 5, 1000, &nv));
    ASSERT_EQ(nv, (int64_t)5);

    hl_kv_backend_valkey.close(h);
    close(sv[0]);
}

UTEST(valkey_backend, caps_and_schemes) {
    const HlKvBackend *B = &hl_kv_backend_valkey;
    ASSERT_TRUE((B->caps & HL_KV_CAP_TTL) != 0);
    ASSERT_TRUE((B->caps & HL_KV_CAP_COMPARE_EXCHANGE) != 0);
    ASSERT_TRUE((B->caps & HL_KV_CAP_SHARED) != 0);
    ASSERT_STREQ(B->schemes[0], "valkey");
    ASSERT_STREQ(B->schemes[2], "redis");
}

UTEST_MAIN();
