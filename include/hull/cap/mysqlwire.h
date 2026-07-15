/*
 * cap/mysqlwire.h: MySQL / MariaDB wire-protocol codec (framing + typed I/O)
 *
 * A self-contained, allocation-light codec for the MySQL client/server
 * protocol, split from the backend (cap/db_mysql.c) and any socket/TLS
 * transport so it can be unit-tested and fuzzed in isolation.
 *
 * The reader treats ALL input as UNTRUSTED (a server, or an attacker on the
 * wire, controls every length and count field). Every read is bounds-checked;
 * a hostile length is rejected, never trusted.
 *
 * Differences from the PostgreSQL codec: MySQL is LITTLE-endian, frames are a
 * 3-byte payload length + 1-byte sequence id (no per-message type tag), and
 * it uses length-encoded integers / strings.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_MYSQLWIRE_H
#define HL_CAP_MYSQLWIRE_H

#include <stddef.h>
#include <stdint.h>

/* ── Client message writer (messages Hull SENDS) ──────────────────── */

/* Growable output buffer. On allocation failure or size overflow the sticky
 * `err` flag latches and further appends become no-ops, so a caller can build
 * a whole packet and check `err` once at the end. */
typedef struct HlMyWriter {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      err;
} HlMyWriter;

void hl_my_writer_init(HlMyWriter *w);
void hl_my_writer_free(HlMyWriter *w);
void hl_my_writer_reset(HlMyWriter *w);   /* keep capacity, len -> 0 */

/* Primitive appenders (little-endian integers, matching the wire). No-ops when
 * w->err is set. */
void hl_my_put_u8(HlMyWriter *w, uint8_t v);
void hl_my_put_u16(HlMyWriter *w, uint16_t v);
void hl_my_put_u24(HlMyWriter *w, uint32_t v);
void hl_my_put_u32(HlMyWriter *w, uint32_t v);
void hl_my_put_u64(HlMyWriter *w, uint64_t v);
void hl_my_put_bytes(HlMyWriter *w, const void *p, size_t n);
void hl_my_put_cstr(HlMyWriter *w, const char *s);        /* includes the NUL */
void hl_my_put_lenenc_int(HlMyWriter *w, uint64_t v);
void hl_my_put_lenenc_str(HlMyWriter *w, const void *p, size_t n);

/*
 * Packet framing. `begin` writes a 4-byte header placeholder (3-byte length +
 * seq) and returns a marker; `end` back-patches the 3-byte length to cover the
 * payload appended since begin. The MySQL header length excludes the header
 * itself. A single logical packet whose payload reaches 0xFFFFFF must be split
 * across frames on the wire; the writer latches `err` on such an oversized
 * payload rather than emit a corrupt length (Hull's messages never approach it).
 */
size_t hl_my_packet_begin(HlMyWriter *w, uint8_t seq);
void   hl_my_packet_end(HlMyWriter *w, size_t marker);

/* ── Server message reader (UNTRUSTED INPUT) ──────────────────────── */

/* One complete packet frame. `body` points into the caller's buffer and is
 * valid until that buffer is mutated; `body_len` is validated to fit. */
typedef struct HlMyFrame {
    uint8_t        seq;
    const uint8_t *body;
    uint32_t       body_len;
} HlMyFrame;

typedef enum HlMyResult {
    HL_MY_OK        =  0,   /* one frame extracted; *consumed advanced   */
    HL_MY_NEED_MORE =  1,   /* incomplete; retry with more bytes         */
    HL_MY_ERR       = -1,   /* malformed; the stream is unusable         */
} HlMyResult;

/* Largest packet body Hull accepts from a server (a 24-bit length caps it at
 * 16 MiB - 1 anyway; this is the same value, named for symmetry with the PG
 * codec). */
#ifndef HL_MY_MAX_MSG
#define HL_MY_MAX_MSG (0x00FFFFFFu)
#endif

/*
 * Extract one packet from buf[0, avail). On HL_MY_OK, *out is filled and
 * *consumed is set to the total frame size (4-byte header + body) so the caller
 * can advance its buffer. On HL_MY_NEED_MORE, *consumed = 0. Never dereferences
 * past avail.
 */
HlMyResult hl_my_frame_next(const uint8_t *buf, size_t avail,
                            HlMyFrame *out, size_t *consumed);

/* ── Bounded cursor over a frame body (UNTRUSTED) ─────────────────── */

/* Typed, bounds-checked reads within a single packet body. On any underrun the
 * sticky `err` latches; subsequent reads return 0 / NULL. A parser reads a
 * whole packet then checks hl_my_cursor_err() once. */
typedef struct HlMyCursor {
    const uint8_t *p;
    size_t         remaining;
    int            err;
} HlMyCursor;

void           hl_my_cursor_init(HlMyCursor *c, const HlMyFrame *f);
uint8_t        hl_my_get_u8(HlMyCursor *c);
uint16_t       hl_my_get_u16(HlMyCursor *c);
uint32_t       hl_my_get_u24(HlMyCursor *c);
uint32_t       hl_my_get_u32(HlMyCursor *c);
uint64_t       hl_my_get_u64(HlMyCursor *c);
/* Length-encoded integer. On a NULL marker (0xFB) or malformed first byte,
 * latches err and returns 0; *is_null (if non-NULL) distinguishes NULL from a
 * genuine underrun. */
uint64_t       hl_my_get_lenenc_int(HlMyCursor *c, int *is_null);
/* Pointer to n contiguous bytes (advancing past them), or NULL + err. */
const uint8_t *hl_my_get_bytes(HlMyCursor *c, size_t n);
/* Length-encoded string: reads a lenenc int, then that many bytes. Returns a
 * pointer to the bytes (NOT NUL-terminated) with *out_len set, or NULL + err. */
const uint8_t *hl_my_get_lenenc_str(HlMyCursor *c, size_t *out_len);
/* NUL-terminated string within the body (advancing past the NUL), or NULL. */
const char    *hl_my_get_cstr(HlMyCursor *c);
/* The rest of the body as an EOF-terminated string (no advance-past-NUL). */
const uint8_t *hl_my_get_eof_str(HlMyCursor *c, size_t *out_len);
int            hl_my_cursor_err(const HlMyCursor *c);

/* ── OK / ERR / EOF packet parsing (UNTRUSTED) ────────────────────── */

/* First payload byte identifies these special packets. Row / result packets
 * are context-dependent, so the caller decides when to treat 0xFE as EOF (only
 * when the body is short, < 9 bytes, per the protocol). */
#define HL_MY_PKT_OK          0x00
#define HL_MY_PKT_EOF         0xFE
#define HL_MY_PKT_ERR         0xFF
#define HL_MY_PKT_LOCAL_INFILE 0xFB

typedef struct HlMyOk {
    uint64_t affected_rows;
    uint64_t last_insert_id;
    uint16_t status_flags;
    uint16_t warnings;
} HlMyOk;

typedef struct HlMyErr {
    uint16_t    code;
    char        sqlstate[6];   /* 5 chars + NUL; empty if not present */
    const char *message;       /* into the frame body; NOT NUL-terminated */
    size_t      message_len;
} HlMyErr;

/* Parse an OK packet body (first byte already known to be 0x00). Returns 0 / -1
 * (malformed). CLIENT_PROTOCOL_41 status/warnings assumed. */
int hl_my_parse_ok(const HlMyFrame *f, HlMyOk *out);
/* Parse an ERR packet body (first byte 0xFF). @p protocol_41 controls whether
 * the '#'+sqlstate block is present. Returns 0 / -1. */
int hl_my_parse_err(const HlMyFrame *f, int protocol_41, HlMyErr *out);

/* ── Handshake (UNTRUSTED server input / client response) ──────────── */

/* Parsed initial Handshake v10 packet the server sends first. */
typedef struct HlMyHandshake {
    uint8_t  protocol;              /* must be 10 */
    char     server_version[64];    /* truncated if longer */
    uint32_t conn_id;
    uint32_t capabilities;          /* lower | (upper << 16) */
    uint8_t  charset;
    uint16_t status;
    uint8_t  scramble[20];          /* auth-plugin-data (part1[8] + part2[12]) */
    int      scramble_len;          /* bytes actually filled (usually 20) */
    char     auth_plugin[64];       /* auth-plugin name, "" if not advertised */
} HlMyHandshake;

/* Parse a Handshake v10 packet body. Bounds-checked over untrusted input;
 * returns 0 / -1 (malformed or unsupported protocol version). */
int hl_my_parse_handshake(const HlMyFrame *f, HlMyHandshake *out);

/* Build a HandshakeResponse41 packet (client -> server) into @p w with sequence
 * @p seq. @p auth_resp is the plugin's auth response bytes (e.g. the 20-byte
 * native_password scramble, or empty). @p dbname / @p plugin are written only
 * when the matching capability bit is set in @p client_caps. */
void hl_my_build_handshake_response(HlMyWriter *w, uint8_t seq,
                                    uint32_t client_caps, uint8_t charset,
                                    const char *user,
                                    const uint8_t *auth_resp, size_t auth_resp_len,
                                    const char *dbname, const char *plugin);

/* ── Protocol constants (subset Hull uses) ────────────────────────── */

/* Client commands (first payload byte of a command packet). */
#define HL_MY_COM_QUIT          0x01
#define HL_MY_COM_INIT_DB       0x02
#define HL_MY_COM_QUERY         0x03
#define HL_MY_COM_PING          0x0E
#define HL_MY_COM_STMT_PREPARE  0x16
#define HL_MY_COM_STMT_EXECUTE  0x17
#define HL_MY_COM_STMT_CLOSE    0x19

/* Capability flags (subset; little-endian on the wire). */
#define HL_MY_CLIENT_LONG_PASSWORD      0x00000001u
#define HL_MY_CLIENT_LONG_FLAG          0x00000004u
#define HL_MY_CLIENT_CONNECT_WITH_DB    0x00000008u
#define HL_MY_CLIENT_PROTOCOL_41        0x00000200u
#define HL_MY_CLIENT_SSL                0x00000800u
#define HL_MY_CLIENT_TRANSACTIONS       0x00002000u
#define HL_MY_CLIENT_SECURE_CONNECTION  0x00008000u
#define HL_MY_CLIENT_MULTI_STATEMENTS   0x00010000u
#define HL_MY_CLIENT_MULTI_RESULTS      0x00020000u
#define HL_MY_CLIENT_PLUGIN_AUTH        0x00080000u
#define HL_MY_CLIENT_PLUGIN_AUTH_LENENC 0x00200000u
#define HL_MY_CLIENT_DEPRECATE_EOF      0x01000000u

#endif /* HL_CAP_MYSQLWIRE_H */
