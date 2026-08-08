/*
 * test_respwire.c: RESP2/RESP3 codec tests.
 *
 * Exercises the request writer and, more importantly, the untrusted-input
 * reply parser: every scalar type, RESP3 additions (map/set/bool/double/null/
 * bignum/verbatim), nested aggregates, incomplete frames (NEED_MORE), and
 * hostile input (bad lengths, over-deep nesting, junk) - never an OOB read.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/respwire.h"
#include <string.h>

/* Bump arena for aggregate items; reset before each parse. */
static uint8_t g_arena[1u << 16];
static size_t  g_arena_off;
static void *test_alloc(void *ctx, size_t n) {
    (void)ctx;
    if (n > sizeof g_arena - g_arena_off) return NULL;
    void *p = g_arena + g_arena_off;
    g_arena_off += n;
    memset(p, 0, n);
    return p;
}
#define PARSE(str) \
    g_arena_off = 0; \
    size_t consumed = 0; HlRespValue v; \
    HlRespResult r = hl_resp_parse((const uint8_t *)(str), sizeof(str) - 1, \
                                   &consumed, &v, test_alloc, NULL)

/* ── writer ───────────────────────────────────────────────────────────── */

UTEST(respwire_writer, command_encoding_binary_safe) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 3);
    hl_resp_cmd_arg_cstr(&w, "SET");
    hl_resp_cmd_arg(&w, "k", 1);
    hl_resp_cmd_arg(&w, "a\0b", 3);           /* embedded NUL */
    ASSERT_FALSE(w.err);
    static const char want[] = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\na\0b\r\n";
    ASSERT_EQ(w.len, sizeof want - 1);
    ASSERT_EQ(0, memcmp(w.buf, want, sizeof want - 1));
    hl_resp_writer_free(&w);
}

UTEST(respwire_writer, integer_arg) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "INCRBY");
    hl_resp_cmd_arg_i64(&w, -42);
    ASSERT_FALSE(w.err);
    static const char want[] = "*2\r\n$6\r\nINCRBY\r\n$3\r\n-42\r\n";
    ASSERT_EQ(0, memcmp(w.buf, want, sizeof want - 1));
    hl_resp_writer_free(&w);
}

/* ── scalar replies ───────────────────────────────────────────────────── */

UTEST(respwire_parse, simple_string) {
    PARSE("+OK\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_STR);
    ASSERT_TRUE(hl_resp_is_ok(&v));
    ASSERT_EQ(consumed, (size_t)5);
}

UTEST(respwire_parse, error) {
    PARSE("-ERR bad\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_ERR);
    ASSERT_EQ(v.str.len, (size_t)7);
    ASSERT_EQ(0, memcmp(v.str.p, "ERR bad", 7));
}

UTEST(respwire_parse, integer_neg) {
    PARSE(":-123\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_INT);
    ASSERT_EQ(v.i, (int64_t)-123);
}

UTEST(respwire_parse, bulk_string_binary_safe) {
    PARSE("$3\r\na\0b\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_STR);
    ASSERT_EQ(v.str.len, (size_t)3);
    ASSERT_EQ(0, memcmp(v.str.p, "a\0b", 3));
}

UTEST(respwire_parse, bulk_null_resp2) {
    PARSE("$-1\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_NULL);
}

UTEST(respwire_parse, null_resp3) {
    PARSE("_\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_NULL);
}

UTEST(respwire_parse, boolean_and_double) {
    { PARSE("#t\r\n"); ASSERT_EQ(r, HL_RESP_OK); ASSERT_EQ(v.type, HL_RESP_BOOL); ASSERT_EQ(v.b, 1); }
    { PARSE(",3.5\r\n"); ASSERT_EQ(r, HL_RESP_OK); ASSERT_EQ(v.type, HL_RESP_DOUBLE); ASSERT_TRUE(v.d > 3.4 && v.d < 3.6); }
}

UTEST(respwire_parse, verbatim_strips_tag) {
    PARSE("=9\r\ntxt:hello\r\n");                /* 9 bytes: "txt:" + "hello" */
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_STR);
    ASSERT_EQ(v.str.len, (size_t)5);
    ASSERT_EQ(0, memcmp(v.str.p, "hello", 5));   /* tag "txt:" stripped */
}

UTEST(respwire_parse, bignum_kept_as_text) {
    PARSE("(3492890328409238509324850943850943825024385\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_BIGNUM);
}

/* ── aggregates ───────────────────────────────────────────────────────── */

UTEST(respwire_parse, array_of_bulk) {
    PARSE("*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_ARRAY);
    ASSERT_EQ(v.arr.count, (size_t)2);
    ASSERT_EQ(0, memcmp(v.arr.items[0].str.p, "foo", 3));
    ASSERT_EQ(0, memcmp(v.arr.items[1].str.p, "bar", 3));
}

UTEST(respwire_parse, nested_array_scan_reply) {
    /* SCAN reply: [cursor, [key1, key2]] */
    PARSE("*2\r\n$1\r\n0\r\n*2\r\n$2\r\nk1\r\n$2\r\nk2\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_ARRAY);
    ASSERT_EQ(v.arr.count, (size_t)2);
    ASSERT_EQ(v.arr.items[1].type, HL_RESP_ARRAY);
    ASSERT_EQ(v.arr.items[1].arr.count, (size_t)2);
}

UTEST(respwire_parse, map_resp3) {
    PARSE("%1\r\n$1\r\nk\r\n:7\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_MAP);
    ASSERT_EQ(v.arr.count, (size_t)2);          /* 2*pairs */
    ASSERT_EQ(v.arr.items[1].type, HL_RESP_INT);
    ASSERT_EQ(v.arr.items[1].i, (int64_t)7);
}

UTEST(respwire_parse, empty_array) {
    PARSE("*0\r\n");
    ASSERT_EQ(r, HL_RESP_OK);
    ASSERT_EQ(v.type, HL_RESP_ARRAY);
    ASSERT_EQ(v.arr.count, (size_t)0);
}

/* ── untrusted-input safety ───────────────────────────────────────────── */

UTEST(respwire_parse, need_more_incomplete_header) {
    PARSE("+OK");                               /* no CRLF yet */
    ASSERT_EQ(r, HL_RESP_NEED_MORE);
    ASSERT_EQ(consumed, (size_t)0);
}

UTEST(respwire_parse, need_more_partial_bulk_body) {
    PARSE("$5\r\nhel");                          /* body shorter than declared */
    ASSERT_EQ(r, HL_RESP_NEED_MORE);
}

UTEST(respwire_parse, need_more_partial_array) {
    PARSE("*2\r\n$3\r\nfoo\r\n");                /* only one of two elements */
    ASSERT_EQ(r, HL_RESP_NEED_MORE);
}

UTEST(respwire_parse, err_bad_bulk_length) {
    PARSE("$999999999999999999999\r\n");        /* length overflows i64 */
    ASSERT_EQ(r, HL_RESP_PARSE_ERR);
}

UTEST(respwire_parse, err_bulk_missing_crlf) {
    PARSE("$3\r\nfooX\n");                       /* body not terminated by CRLF */
    ASSERT_EQ(r, HL_RESP_PARSE_ERR);
}

UTEST(respwire_parse, err_unknown_type) {
    PARSE("^nope\r\n");
    ASSERT_EQ(r, HL_RESP_PARSE_ERR);
}

UTEST(respwire_parse, err_too_deep_nesting) {
    /* 40 nested 1-element arrays exceeds HL_RESP_MAX_DEPTH (32). */
    char deep[512]; size_t o = 0;
    for (int i = 0; i < 40; i++) { memcpy(deep + o, "*1\r\n", 4); o += 4; }
    memcpy(deep + o, ":1\r\n", 4); o += 4;
    g_arena_off = 0;
    size_t consumed = 0; HlRespValue v;
    HlRespResult r = hl_resp_parse((const uint8_t *)deep, o, &consumed, &v, test_alloc, NULL);
    ASSERT_EQ(r, HL_RESP_PARSE_ERR);
}

UTEST(respwire_parse, aggregate_without_arena_rejected) {
    g_arena_off = 0;
    size_t consumed = 0; HlRespValue v;
    const char *s = "*1\r\n:1\r\n";
    HlRespResult r = hl_resp_parse((const uint8_t *)s, strlen(s), &consumed, &v, NULL, NULL);
    ASSERT_EQ(r, HL_RESP_PARSE_ERR);            /* no alloc -> reject aggregate */
}

UTEST_MAIN();
