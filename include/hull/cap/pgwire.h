/*
 * cap/pgwire.h: PostgreSQL v3 wire-protocol codec (framing + typed I/O)
 *
 * A self-contained, allocation-light codec for the PostgreSQL frontend/
 * backend protocol version 3. Split from the backend (cap/db_postgres.c)
 * and from any socket/TLS transport so it can be unit-tested and fuzzed in
 * isolation: feed arbitrary bytes to the reader and it must never read out
 * of bounds, only parse or report NEED_MORE / ERR.
 *
 * The reader treats all input as UNTRUSTED (a PostgreSQL server, or an
 * attacker on the wire, controls every length and count field). Every
 * read is bounds-checked; a hostile length is rejected, never trusted.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_PGWIRE_H
#define HL_CAP_PGWIRE_H

#include <stddef.h>
#include <stdint.h>

/* ── Frontend message writer (messages Hull SENDS) ────────────────── */

/* Growable output buffer. On allocation failure or size overflow the
 * sticky `err` flag latches and further appends become no-ops, so callers
 * can build a whole message and check `err` once at the end. */
typedef struct HlPgWriter {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      err;
} HlPgWriter;

void hl_pg_writer_init(HlPgWriter *w);
void hl_pg_writer_free(HlPgWriter *w);
void hl_pg_writer_reset(HlPgWriter *w);   /* keep capacity, len -> 0 */

/* Primitive appenders (big-endian for the integer forms, matching the
 * wire's network byte order). No-ops when w->err is set. */
void hl_pg_put_u8(HlPgWriter *w, uint8_t v);
void hl_pg_put_i16(HlPgWriter *w, int16_t v);
void hl_pg_put_i32(HlPgWriter *w, int32_t v);
void hl_pg_put_bytes(HlPgWriter *w, const void *p, size_t n);
void hl_pg_put_cstr(HlPgWriter *w, const char *s);   /* includes the NUL */

/*
 * Message framing. `begin` writes the 1-byte type tag (pass type == 0 for
 * the startup / SSLRequest messages, which are length-prefixed with no
 * tag) followed by a 4-byte length placeholder, and returns a marker.
 * `end` back-patches the length to cover everything appended since begin
 * (the length field counts itself but not the type tag, per the protocol).
 */
size_t hl_pg_msg_begin(HlPgWriter *w, uint8_t type);
void   hl_pg_msg_end(HlPgWriter *w, size_t marker);

/* ── Backend message reader (UNTRUSTED INPUT) ─────────────────────── */

/* One complete message frame. `body` points into the caller's buffer and
 * is valid until that buffer is mutated; `body_len` is validated to fit. */
typedef struct HlPgFrame {
    uint8_t        type;
    const uint8_t *body;
    uint32_t       body_len;
} HlPgFrame;

typedef enum HlPgResult {
    HL_PG_OK        =  0,   /* one frame extracted; *consumed advanced   */
    HL_PG_NEED_MORE =  1,   /* incomplete; retry with more bytes         */
    HL_PG_ERR       = -1,   /* malformed length; the stream is unusable  */
} HlPgResult;

/* Largest message body Hull accepts from a server, a guard against a
 * hostile length field. Override at compile time if a workload legitimately
 * streams larger single messages. */
#ifndef HL_PG_MAX_MSG
#define HL_PG_MAX_MSG (256u * 1024u * 1024u)
#endif

/*
 * Extract one frame from buf[0, avail). On HL_PG_OK, *out is filled and
 * *consumed is set to the total frame size (tag + length + body) so the
 * caller can advance its buffer. On HL_PG_NEED_MORE, *consumed = 0. On
 * HL_PG_ERR the length was out of range and the connection must be dropped.
 * Never dereferences past avail.
 */
HlPgResult hl_pg_frame_next(const uint8_t *buf, size_t avail,
                            HlPgFrame *out, size_t *consumed);

/* ── Bounded cursor over a frame body (UNTRUSTED) ─────────────────── */

/* Typed, bounds-checked reads within a single frame body. On any underrun
 * the sticky `err` latches; subsequent reads return 0 / NULL. A parser
 * reads a whole message then checks hl_pg_cursor_err() once. */
typedef struct HlPgCursor {
    const uint8_t *p;
    size_t         remaining;
    int            err;
} HlPgCursor;

void           hl_pg_cursor_init(HlPgCursor *c, const HlPgFrame *f);
uint8_t        hl_pg_get_u8(HlPgCursor *c);
int16_t        hl_pg_get_i16(HlPgCursor *c);
int32_t        hl_pg_get_i32(HlPgCursor *c);
/* Pointer to n contiguous bytes (advancing past them), or NULL + err. */
const uint8_t *hl_pg_get_bytes(HlPgCursor *c, size_t n);
/* Pointer to a NUL-terminated string within the body (advancing past the
 * NUL), or NULL + err if no NUL remains in the body. */
const char    *hl_pg_get_cstr(HlPgCursor *c);
int            hl_pg_cursor_err(const HlPgCursor *c);

/* ── Protocol constants (subset Hull uses) ────────────────────────── */

/* Backend (server -> client) message tags. */
#define HL_PG_B_AUTH             'R'
#define HL_PG_B_PARAM_STATUS     'S'
#define HL_PG_B_BACKEND_KEY      'K'
#define HL_PG_B_ROW_DESC         'T'
#define HL_PG_B_DATA_ROW         'D'
#define HL_PG_B_CMD_COMPLETE     'C'
#define HL_PG_B_ERROR            'E'
#define HL_PG_B_NOTICE           'N'
#define HL_PG_B_READY            'Z'
#define HL_PG_B_PARSE_COMPLETE   '1'
#define HL_PG_B_BIND_COMPLETE    '2'
#define HL_PG_B_CLOSE_COMPLETE   '3'
#define HL_PG_B_NO_DATA          'n'
#define HL_PG_B_PARAM_DESC       't'
#define HL_PG_B_EMPTY_QUERY      'I'

/* Frontend (client -> server) message tags. */
#define HL_PG_F_QUERY            'Q'
#define HL_PG_F_PARSE            'P'
#define HL_PG_F_BIND             'B'
#define HL_PG_F_DESCRIBE         'D'
#define HL_PG_F_EXECUTE          'E'
#define HL_PG_F_SYNC             'S'
#define HL_PG_F_PASSWORD         'p'
#define HL_PG_F_TERMINATE        'X'

/* Authentication sub-types (int32 following the 'R' tag). */
#define HL_PG_AUTH_OK             0
#define HL_PG_AUTH_CLEARTEXT      3
#define HL_PG_AUTH_MD5            5
#define HL_PG_AUTH_SASL          10
#define HL_PG_AUTH_SASL_CONTINUE 11
#define HL_PG_AUTH_SASL_FINAL    12

#endif /* HL_CAP_PGWIRE_H */
