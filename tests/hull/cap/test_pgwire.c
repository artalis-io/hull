/*
 * test_pgwire.c: PostgreSQL v3 wire-protocol codec tests
 *
 * Exercises the writer (framing + big-endian appenders) and, more
 * importantly, the untrusted-input reader: incomplete frames, malformed
 * lengths, exact boundaries, and cursor underruns must be handled without
 * ever reading out of bounds.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/pgwire.h"
#include <string.h>

/* ── Writer primitives ────────────────────────────────────────────── */

UTEST(pgwire_writer, big_endian_integers)
{
    HlPgWriter w;
    hl_pg_writer_init(&w);
    hl_pg_put_u8(&w, 0xAB);
    hl_pg_put_i16(&w, (int16_t)0x0102);
    hl_pg_put_i32(&w, 0x01020304);
    ASSERT_FALSE(w.err);
    ASSERT_EQ(w.len, (size_t)7);
    static const uint8_t want[] = { 0xAB, 0x01, 0x02, 0x01, 0x02, 0x03, 0x04 };
    ASSERT_EQ(0, memcmp(w.buf, want, sizeof want));
    hl_pg_writer_free(&w);
}

UTEST(pgwire_writer, cstring_includes_nul)
{
    HlPgWriter w;
    hl_pg_writer_init(&w);
    hl_pg_put_cstr(&w, "hi");
    ASSERT_EQ(w.len, (size_t)3);
    ASSERT_EQ(w.buf[0], 'h');
    ASSERT_EQ(w.buf[1], 'i');
    ASSERT_EQ(w.buf[2], 0);
    hl_pg_writer_free(&w);
}

UTEST(pgwire_writer, tagged_message_backpatches_length)
{
    HlPgWriter w;
    hl_pg_writer_init(&w);
    size_t m = hl_pg_msg_begin(&w, HL_PG_F_QUERY);   /* 'Q' */
    hl_pg_put_cstr(&w, "hi");                          /* h i \0 */
    hl_pg_msg_end(&w, m);
    ASSERT_FALSE(w.err);
    /* 'Q', len=7 (4 length bytes + 3 body), then body. */
    static const uint8_t want[] = { 'Q', 0, 0, 0, 7, 'h', 'i', 0 };
    ASSERT_EQ(w.len, sizeof want);
    ASSERT_EQ(0, memcmp(w.buf, want, sizeof want));
    hl_pg_writer_free(&w);
}

UTEST(pgwire_writer, startup_has_no_tag)
{
    HlPgWriter w;
    hl_pg_writer_init(&w);
    size_t m = hl_pg_msg_begin(&w, 0);   /* startup: length-prefixed, no tag */
    hl_pg_put_i32(&w, 196608);           /* protocol 3.0 */
    hl_pg_msg_end(&w, m);
    ASSERT_FALSE(w.err);
    /* length = 4 (len) + 4 (payload) = 8, no leading tag byte. */
    static const uint8_t want[] = { 0, 0, 0, 8, 0, 0x03, 0, 0 };
    ASSERT_EQ(w.len, sizeof want);
    ASSERT_EQ(0, memcmp(w.buf, want, sizeof want));
    hl_pg_writer_free(&w);
}

/* ── Reader: framing over untrusted bytes ─────────────────────────── */

UTEST(pgwire_reader, need_more_when_short)
{
    HlPgFrame f;
    size_t consumed = 123;
    /* Fewer than the 5-byte header. */
    const uint8_t hdr[] = { 'D', 0, 0 };
    ASSERT_EQ(HL_PG_NEED_MORE, hl_pg_frame_next(hdr, 3, &f, &consumed));
    ASSERT_EQ(consumed, (size_t)0);

    /* Header says body of 4 but only 2 present. */
    const uint8_t part[] = { 'D', 0, 0, 0, 8, 0xAA, 0xBB };
    ASSERT_EQ(HL_PG_NEED_MORE, hl_pg_frame_next(part, sizeof part, &f, &consumed));
    ASSERT_EQ(consumed, (size_t)0);
}

UTEST(pgwire_reader, rejects_malformed_length)
{
    HlPgFrame f;
    size_t consumed;
    /* Length < 4 is malformed (must at least cover itself). */
    const uint8_t tiny[] = { 'D', 0, 0, 0, 3, 0 };
    ASSERT_EQ(HL_PG_ERR, hl_pg_frame_next(tiny, sizeof tiny, &f, &consumed));

    /* Length above the cap must be rejected, not waited on. */
    const uint8_t huge[] = { 'D', 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    ASSERT_EQ(HL_PG_ERR, hl_pg_frame_next(huge, sizeof huge, &f, &consumed));
}

UTEST(pgwire_reader, extracts_exact_frame)
{
    HlPgFrame f;
    size_t consumed = 0;
    const uint8_t msg[] = { 'Q', 0, 0, 0, 7, 'h', 'i', 0 };
    ASSERT_EQ(HL_PG_OK, hl_pg_frame_next(msg, sizeof msg, &f, &consumed));
    ASSERT_EQ(f.type, (uint8_t)'Q');
    ASSERT_EQ(f.body_len, (uint32_t)3);
    ASSERT_EQ(consumed, (size_t)8);
    ASSERT_EQ(f.body[0], 'h');
}

UTEST(pgwire_reader, consumes_frames_in_sequence)
{
    /* Two 'Z' (ReadyForQuery) frames: tag + len=5 + 1 status byte. */
    const uint8_t stream[] = {
        'Z', 0, 0, 0, 5, 'I',
        'Z', 0, 0, 0, 5, 'T',
    };
    HlPgFrame f;
    size_t consumed = 0, off = 0;

    ASSERT_EQ(HL_PG_OK, hl_pg_frame_next(stream + off, sizeof stream - off,
                                         &f, &consumed));
    ASSERT_EQ(f.body[0], 'I');
    off += consumed;

    ASSERT_EQ(HL_PG_OK, hl_pg_frame_next(stream + off, sizeof stream - off,
                                         &f, &consumed));
    ASSERT_EQ(f.body[0], 'T');
    off += consumed;
    ASSERT_EQ(off, sizeof stream);
}

/* ── Cursor: bounded typed reads ──────────────────────────────────── */

UTEST(pgwire_cursor, typed_reads)
{
    /* body: u8=0x7F, i16=0x0102, i32=0x03040506, cstr "ok" */
    const uint8_t body[] = { 0x7F, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                             'o', 'k', 0 };
    HlPgFrame f = { .type = 'D', .body = body, .body_len = sizeof body };
    HlPgCursor c;
    hl_pg_cursor_init(&c, &f);
    ASSERT_EQ(hl_pg_get_u8(&c), (uint8_t)0x7F);
    ASSERT_EQ(hl_pg_get_i16(&c), (int16_t)0x0102);
    ASSERT_EQ(hl_pg_get_i32(&c), (int32_t)0x03040506);
    const char *s = hl_pg_get_cstr(&c);
    ASSERT_TRUE(s != NULL);
    ASSERT_STREQ(s, "ok");
    ASSERT_FALSE(hl_pg_cursor_err(&c));
}

UTEST(pgwire_cursor, underrun_latches_error)
{
    const uint8_t body[] = { 0x01, 0x02 };
    HlPgFrame f = { .type = 'D', .body = body, .body_len = sizeof body };
    HlPgCursor c;
    hl_pg_cursor_init(&c, &f);
    (void)hl_pg_get_i16(&c);            /* consumes both bytes */
    ASSERT_FALSE(hl_pg_cursor_err(&c));
    (void)hl_pg_get_u8(&c);             /* underrun */
    ASSERT_TRUE(hl_pg_cursor_err(&c));
    /* Once latched, further reads stay in error and return zero. */
    ASSERT_EQ(hl_pg_get_i32(&c), 0);
    ASSERT_TRUE(hl_pg_cursor_err(&c));
}

UTEST(pgwire_cursor, cstring_without_nul_errors)
{
    const uint8_t body[] = { 'n', 'o', 'n', 'u', 'l' };   /* no terminator */
    HlPgFrame f = { .type = 'D', .body = body, .body_len = sizeof body };
    HlPgCursor c;
    hl_pg_cursor_init(&c, &f);
    ASSERT_TRUE(hl_pg_get_cstr(&c) == NULL);
    ASSERT_TRUE(hl_pg_cursor_err(&c));
}

UTEST(pgwire_cursor, get_bytes_over_read_errors)
{
    const uint8_t body[] = { 1, 2, 3 };
    HlPgFrame f = { .type = 'D', .body = body, .body_len = sizeof body };
    HlPgCursor c;
    hl_pg_cursor_init(&c, &f);
    ASSERT_TRUE(hl_pg_get_bytes(&c, 2) != NULL);
    ASSERT_TRUE(hl_pg_get_bytes(&c, 2) == NULL);   /* only 1 left */
    ASSERT_TRUE(hl_pg_cursor_err(&c));
}

/* ── Round trip: writer output parses back through the reader ─────── */

UTEST(pgwire_roundtrip, write_then_read)
{
    HlPgWriter w;
    hl_pg_writer_init(&w);
    size_t m = hl_pg_msg_begin(&w, HL_PG_B_ROW_DESC);   /* 'T' */
    hl_pg_put_i16(&w, 1);            /* one field */
    hl_pg_put_cstr(&w, "id");        /* field name */
    hl_pg_put_i32(&w, 0);            /* table oid */
    hl_pg_put_i16(&w, 0);            /* column */
    hl_pg_put_i32(&w, 23);           /* type oid (int4) */
    hl_pg_put_i16(&w, 4);            /* typlen */
    hl_pg_put_i32(&w, -1);           /* typmod */
    hl_pg_put_i16(&w, 0);            /* format */
    hl_pg_msg_end(&w, m);
    ASSERT_FALSE(w.err);

    HlPgFrame f;
    size_t consumed = 0;
    ASSERT_EQ(HL_PG_OK, hl_pg_frame_next(w.buf, w.len, &f, &consumed));
    ASSERT_EQ(f.type, (uint8_t)'T');
    ASSERT_EQ(consumed, w.len);

    HlPgCursor c;
    hl_pg_cursor_init(&c, &f);
    ASSERT_EQ(hl_pg_get_i16(&c), (int16_t)1);
    ASSERT_STREQ(hl_pg_get_cstr(&c), "id");
    ASSERT_EQ(hl_pg_get_i32(&c), 0);
    ASSERT_EQ(hl_pg_get_i16(&c), (int16_t)0);
    ASSERT_EQ(hl_pg_get_i32(&c), 23);
    ASSERT_EQ(hl_pg_get_i16(&c), (int16_t)4);
    ASSERT_EQ(hl_pg_get_i32(&c), -1);
    ASSERT_EQ(hl_pg_get_i16(&c), (int16_t)0);
    ASSERT_FALSE(hl_pg_cursor_err(&c));

    hl_pg_writer_free(&w);
}

UTEST_MAIN()
