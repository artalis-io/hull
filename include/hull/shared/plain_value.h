/*
 * plain_value.h - the value model a build plugin and its host exchange
 * (docs/buildcontext_design.md §9.1): nil, booleans, int64, UTF-8 text, byte
 * strings, arrays, and maps with text keys. Nothing else crosses.
 *
 * Encoding (all integers little-endian):
 *   0x00 nil      0x01 false     0x02 true
 *   0x03 int      i64
 *   0x04 text     u32 length, UTF-8 bytes
 *   0x05 bytes    u32 length, bytes
 *   0x06 array    u32 count, count values
 *   0x07 map      u32 count, count x (u32 key length, UTF-8 key, value),
 *                 keys in strictly ascending byte order
 * One encoding per value: the ascending keys make it canonical and refuse a
 * duplicate key. Depth is bounded by HL_PV_MAX_DEPTH, the whole encoding by
 * the caller's max.
 *
 * The writer is driven by each runtime's walker (runtime/lua/plain_value.c,
 * runtime/js/plain_value.c), which reads the value without running app code;
 * the reader hands the decoder one item at a time. hl_pv_check validates a
 * whole encoding before any of it is decoded.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_PLAIN_VALUE_H
#define HL_SHARED_PLAIN_VALUE_H

#include <stddef.h>
#include <stdint.h>

#define HL_PV_MAX_DEPTH 64

enum {
    HL_PV_NIL = 0, HL_PV_FALSE = 1, HL_PV_TRUE = 2, HL_PV_INT = 3,
    HL_PV_TEXT = 4, HL_PV_BYTES = 5, HL_PV_ARRAY = 6, HL_PV_MAP = 7,
};

/* ── Writer ─────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    size_t   len, cap;
    size_t   max;        /* the largest encoding allowed */
    int      err;        /* sticky: HL_PV_E_* */
} HlPvWriter;

#define HL_PV_OK          0
#define HL_PV_E_NOMEM    -1
#define HL_PV_E_TOO_BIG  -2   /* past the writer's max */
#define HL_PV_E_TEXT     -3   /* text or a key that is not UTF-8 */
#define HL_PV_E_SHAPE    -4   /* a malformed or non-canonical encoding */
#define HL_PV_E_DEPTH    -5
#define HL_PV_E_TYPE     -6   /* a value outside the model (walkers) */

void hl_pv_writer_init(HlPvWriter *w, size_t max);
void hl_pv_writer_free(HlPvWriter *w);

void hl_pv_put_nil(HlPvWriter *w);
void hl_pv_put_bool(HlPvWriter *w, int v);
void hl_pv_put_int(HlPvWriter *w, int64_t v);
void hl_pv_put_text(HlPvWriter *w, const void *s, size_t n);   /* checks UTF-8 */
void hl_pv_put_bytes(HlPvWriter *w, const void *s, size_t n);
/* A container header; its @p count items (a map: key then value, each) follow.
 * A map's keys are the walker's to sort: hl_pv_sort_map does it in place. */
void hl_pv_put_array(HlPvWriter *w, uint32_t count);
void hl_pv_put_map(HlPvWriter *w, uint32_t count);
void hl_pv_put_key(HlPvWriter *w, const void *s, size_t n);    /* checks UTF-8 */

/* Rewrite the count of the container whose header starts at @p header_off
 * (a walker that learns the count as it goes: a JS map skips null values). */
void hl_pv_set_count(HlPvWriter *w, size_t header_off, uint32_t count);

/* Sort a map the walker wrote in its own order: @p start is the offset of
 * the map's first key, @p ends[i] the offset just past entry i's value
 * (entries are consecutive). Reorders the bytes so the keys ascend; a
 * duplicate key sets HL_PV_E_SHAPE. */
void hl_pv_sort_map(HlPvWriter *w, size_t start, const size_t *ends,
                    uint32_t count);

/* ── Reader ─────────────────────────────────────────────────────────── */

typedef struct {
    int         type;     /* HL_PV_* */
    int64_t     i;        /* HL_PV_INT */
    const uint8_t *s;     /* HL_PV_TEXT / HL_PV_BYTES: borrowed */
    size_t      n;        /* their length, or a container's count */
} HlPvItem;

typedef struct {
    const uint8_t *p;
    size_t len, pos;
} HlPvReader;

void hl_pv_reader_init(HlPvReader *r, const void *buf, size_t len);
/* The next item (a value, or a container header whose items follow). 0, or
 * HL_PV_E_SHAPE. Only for an encoding hl_pv_check accepted. */
int hl_pv_next(HlPvReader *r, HlPvItem *it);
/* The next map key (borrowed). 0, or HL_PV_E_SHAPE. */
int hl_pv_next_key(HlPvReader *r, const uint8_t **s, size_t *n);

/* Whether @p buf holds exactly one well-formed, canonical value: valid tags,
 * lengths and counts within the buffer, UTF-8 text and keys, keys strictly
 * ascending, depth at most HL_PV_MAX_DEPTH, nothing after it. HL_PV_OK or an
 * HL_PV_E_* code. */
int hl_pv_check(const void *buf, size_t len);

/* The token for an HL_PV_E_* code ("too_big", "not_text", ...). */
const char *hl_pv_error_name(int err);

#endif /* HL_SHARED_PLAIN_VALUE_H */
