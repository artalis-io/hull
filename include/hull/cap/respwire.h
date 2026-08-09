/*
 * cap/respwire.h: RESP2 / RESP3 wire-protocol codec (Redis / Valkey).
 *
 * A self-contained, allocation-light codec for the Redis Serialization
 * Protocol, split from the connection/TLS transport (cap/redis_conn.c) and the
 * backend (the valkey feature) so it can be unit-tested and fuzzed in
 * isolation: feed arbitrary bytes to the parser and it must never read out of
 * bounds, only decode or report NEED_MORE / ERR.
 *
 * The parser treats all input as UNTRUSTED (a server, or an attacker on the
 * wire, controls every length and count field). Every read is bounds-checked;
 * a hostile length or a too-deep nesting is rejected, never trusted. Requests
 * are always encoded as a RESP array of bulk strings (binary-safe); replies
 * decode into a borrowed HlRespValue tree (RESP2 + the RESP3 additions: map,
 * set, push, bool, double, null, big number, verbatim, bulk error).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_RESPWIRE_H
#define HL_CAP_RESPWIRE_H

#include <stddef.h>
#include <stdint.h>

/* ── Request writer (commands Hull SENDS) ─────────────────────────────
 *
 * A command is a RESP array of bulk strings: *argc CRLF ($len CRLF arg CRLF)*.
 * Growable buffer; on OOM/overflow the sticky `err` latches and further
 * appends become no-ops, so a caller builds a whole command and checks once. */
typedef struct HlRespWriter {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      err;
} HlRespWriter;

void hl_resp_writer_init(HlRespWriter *w);
void hl_resp_writer_free(HlRespWriter *w);
void hl_resp_writer_reset(HlRespWriter *w);   /* keep capacity, len -> 0 */

/* Begin a command of `argc` bulk-string arguments (writes "*argc\r\n"), then
 * append each argument (writes "$len\r\n<bytes>\r\n"). Binary-safe. */
void hl_resp_cmd_begin(HlRespWriter *w, size_t argc);
void hl_resp_cmd_arg(HlRespWriter *w, const void *arg, size_t len);
void hl_resp_cmd_arg_cstr(HlRespWriter *w, const char *arg);      /* strlen */
void hl_resp_cmd_arg_i64(HlRespWriter *w, int64_t v);            /* decimal */
/* Append a bulk arg = glob-escaped(prefix) followed by a literal '*', written
 * directly into the buffer (no temp allocation). Backslash-escapes each of
 * \ * ? [ ] in `prefix` so binary glob metacharacters match literally; the
 * trailing '*' is the only wildcard. Used to build a safe Redis SCAN MATCH. */
void hl_resp_cmd_arg_globprefix(HlRespWriter *w, const void *prefix, size_t plen);

/* ── Reply parser (UNTRUSTED INPUT) ───────────────────────────────────── */

typedef enum HlRespType {
    HL_RESP_NULL   = 0,   /* _ (RESP3) or $-1 / *-1 (RESP2)                 */
    HL_RESP_INT    = 1,   /* :                                             */
    HL_RESP_DOUBLE = 2,   /* , (RESP3)                                     */
    HL_RESP_BOOL   = 3,   /* # (RESP3)                                     */
    HL_RESP_STR    = 4,   /* + simple, $ bulk, = verbatim                  */
    HL_RESP_ERR    = 5,   /* - error, ! bulk error                         */
    HL_RESP_BIGNUM = 6,   /* ( big number (kept as its decimal string)     */
    HL_RESP_ARRAY  = 7,   /* * array, ~ set, > push                        */
    HL_RESP_MAP    = 8,   /* % map (items = 2*pairs, k0,v0,k1,v1,...)       */
} HlRespType;

/* A decoded value. String/error bytes BORROW into the parsed buffer (valid
 * until that buffer is mutated); array/map `items` are arena-allocated. */
typedef struct HlRespValue {
    HlRespType type;
    union {
        int64_t i;                                  /* INT                */
        double  d;                                  /* DOUBLE             */
        int     b;                                  /* BOOL (0/1)         */
        struct { const char *p; size_t len; } str;  /* STR / ERR / BIGNUM */
        struct { struct HlRespValue *items; size_t count; } arr; /* ARRAY/MAP */
    };
} HlRespValue;

typedef enum HlRespResult {
    HL_RESP_OK        =  0,   /* one complete reply decoded; *consumed set  */
    HL_RESP_NEED_MORE =  1,   /* incomplete; retry with more bytes          */
    HL_RESP_PARSE_ERR = -1,   /* malformed / hostile; the stream is unusable */
} HlRespResult;

/* Largest single bulk string / aggregate count Hull accepts from a server, and
 * the deepest reply nesting - guards against hostile length / recursion.
 * Override at compile time for a workload that legitimately needs more. */
#ifndef HL_RESP_MAX_BULK
#define HL_RESP_MAX_BULK  (256u * 1024u * 1024u)
#endif
#ifndef HL_RESP_MAX_ELEMS
#define HL_RESP_MAX_ELEMS (16u * 1024u * 1024u)
#endif
#ifndef HL_RESP_MAX_DEPTH
#define HL_RESP_MAX_DEPTH 32
#endif

/* Arena allocator hook for aggregate items. `alloc(ctx, n)` returns n zeroed
 * bytes valid until the caller's arena is reset, or NULL on OOM (-> PARSE_ERR
 * is NOT returned for OOM; the parser returns HL_RESP_NEED_MORE=... no: OOM
 * yields HL_RESP_PARSE_ERR with a distinct meaning is avoided - see note).
 * Passing NULL alloc rejects any aggregate reply (scalars still parse). */
typedef void *(*HlRespAlloc)(void *ctx, size_t n);

/*
 * Decode ONE complete reply from buf[0, avail). On HL_RESP_OK, *out is the
 * decoded value and *consumed is the total bytes of that reply. On
 * HL_RESP_NEED_MORE the buffer holds a partial reply (retry with more bytes;
 * *consumed = 0). On HL_RESP_PARSE_ERR the input is malformed or an allocation
 * for an aggregate failed; the connection must be dropped. Never dereferences
 * past avail.
 */
HlRespResult hl_resp_parse(const uint8_t *buf, size_t avail, size_t *consumed,
                           HlRespValue *out, HlRespAlloc alloc, void *alloc_ctx);

/* Convenience: is a decoded value a simple-string "OK"? */
int hl_resp_is_ok(const HlRespValue *v);

#endif /* HL_CAP_RESPWIRE_H */
