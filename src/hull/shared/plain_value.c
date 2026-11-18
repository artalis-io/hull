/*
 * plain_value.c - the build plugin channel's value encoding (see
 * include/hull/shared/plain_value.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/plain_value.h"
#include "hull/shared/term_escape.h"

#include <stdlib.h>
#include <string.h>

/* ── Writer ─────────────────────────────────────────────────────────── */

void hl_pv_writer_init(HlPvWriter *w, size_t max)
{
    memset(w, 0, sizeof *w);
    w->max = max;
}

void hl_pv_writer_free(HlPvWriter *w)
{
    free(w->buf);
    w->buf = NULL;
    w->len = w->cap = 0;
}

static uint8_t *room(HlPvWriter *w, size_t n)
{
    if (w->err) return NULL;
    if (n > w->max || w->len > w->max - n) {
        w->err = HL_PV_E_TOO_BIG;
        return NULL;
    }
    if (w->len + n > w->cap) {
        size_t cap = w->cap ? w->cap : 256;
        while (cap < w->len + n)
            cap = cap > SIZE_MAX / 2 ? w->len + n : cap * 2;
        uint8_t *b = realloc(w->buf, cap);
        if (!b) {
            w->err = HL_PV_E_NOMEM;
            return NULL;
        }
        w->buf = b;
        w->cap = cap;
    }
    uint8_t *p = w->buf + w->len;
    w->len += n;
    return p;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static void put_tag(HlPvWriter *w, int tag)
{
    uint8_t *p = room(w, 1);
    if (p) *p = (uint8_t)tag;
}

void hl_pv_put_nil(HlPvWriter *w) { put_tag(w, HL_PV_NIL); }

void hl_pv_put_bool(HlPvWriter *w, int v)
{
    put_tag(w, v ? HL_PV_TRUE : HL_PV_FALSE);
}

void hl_pv_put_int(HlPvWriter *w, int64_t v)
{
    uint8_t *p = room(w, 9);
    if (!p) return;
    p[0] = HL_PV_INT;
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) p[1 + i] = (uint8_t)(u >> (8 * i));
}

static void put_counted(HlPvWriter *w, int tag, const void *s, size_t n)
{
    if (n > UINT32_MAX) {
        if (!w->err) w->err = HL_PV_E_TOO_BIG;
        return;
    }
    size_t pre = tag >= 0 ? 5 : 4;
    uint8_t *p = room(w, pre + n);
    if (!p) return;
    if (tag >= 0) *p++ = (uint8_t)tag;
    put_u32(p, (uint32_t)n);
    if (n) memcpy(p + 4, s, n);
}

void hl_pv_put_text(HlPvWriter *w, const void *s, size_t n)
{
    if (!hl_utf8_valid(s, n)) {
        if (!w->err) w->err = HL_PV_E_TEXT;
        return;
    }
    put_counted(w, HL_PV_TEXT, s, n);
}

void hl_pv_put_bytes(HlPvWriter *w, const void *s, size_t n)
{
    put_counted(w, HL_PV_BYTES, s, n);
}

void hl_pv_put_key(HlPvWriter *w, const void *s, size_t n)
{
    if (!hl_utf8_valid(s, n)) {
        if (!w->err) w->err = HL_PV_E_TEXT;
        return;
    }
    put_counted(w, -1, s, n);
}

static void put_header(HlPvWriter *w, int tag, uint32_t count)
{
    uint8_t *p = room(w, 5);
    if (!p) return;
    p[0] = (uint8_t)tag;
    put_u32(p + 1, count);
}

void hl_pv_set_count(HlPvWriter *w, size_t header_off, uint32_t count)
{
    if (w->err || header_off > w->len || w->len - header_off < 5) return;
    put_u32(w->buf + header_off + 1, count);
}

typedef struct {
    size_t off, len;              /* the entry: key length, key, value */
    const uint8_t *key;
    size_t key_len;
} PvEntry;

static int entry_cmp(const void *a, const void *b)
{
    const PvEntry *x = a, *y = b;
    size_t m = x->key_len < y->key_len ? x->key_len : y->key_len;
    int c = m ? memcmp(x->key, y->key, m) : 0;
    if (c) return c;
    return (x->key_len > y->key_len) - (x->key_len < y->key_len);
}

void hl_pv_sort_map(HlPvWriter *w, size_t start, const size_t *ends,
                    uint32_t count)
{
    if (w->err || count < 2) return;
    size_t end = ends[count - 1];
    PvEntry *e = calloc(count, sizeof *e);
    uint8_t *copy = malloc(end - start);
    if (!e || !copy) {
        free(e);
        free(copy);
        w->err = HL_PV_E_NOMEM;
        return;
    }
    memcpy(copy, w->buf + start, end - start);
    size_t at = start;
    for (uint32_t i = 0; i < count; i++) {
        e[i].off = at - start;
        e[i].len = ends[i] - at;
        e[i].key_len = get_u32(copy + e[i].off);
        e[i].key = copy + e[i].off + 4;
        at = ends[i];
    }
    qsort(e, count, sizeof *e, entry_cmp);
    uint8_t *out = w->buf + start;
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0 && entry_cmp(&e[i - 1], &e[i]) == 0) w->err = HL_PV_E_SHAPE;
        memcpy(out, copy + e[i].off, e[i].len);
        out += e[i].len;
    }
    free(copy);
    free(e);
}

void hl_pv_put_array(HlPvWriter *w, uint32_t count) { put_header(w, HL_PV_ARRAY, count); }
void hl_pv_put_map(HlPvWriter *w, uint32_t count) { put_header(w, HL_PV_MAP, count); }

/* ── Reader ─────────────────────────────────────────────────────────── */

void hl_pv_reader_init(HlPvReader *r, const void *buf, size_t len)
{
    r->p = buf;
    r->len = len;
    r->pos = 0;
}

static int take(HlPvReader *r, size_t n, const uint8_t **out)
{
    if (n > r->len - r->pos) return HL_PV_E_SHAPE;
    *out = r->p + r->pos;
    r->pos += n;
    return 0;
}

int hl_pv_next(HlPvReader *r, HlPvItem *it)
{
    const uint8_t *b;
    memset(it, 0, sizeof *it);
    if (take(r, 1, &b)) return HL_PV_E_SHAPE;
    it->type = b[0];
    switch (b[0]) {
    case HL_PV_NIL: case HL_PV_FALSE: case HL_PV_TRUE:
        return 0;
    case HL_PV_INT: {
        if (take(r, 8, &b)) return HL_PV_E_SHAPE;
        uint64_t u = 0;
        for (int i = 0; i < 8; i++) u |= (uint64_t)b[i] << (8 * i);
        memcpy(&it->i, &u, sizeof u);
        return 0;
    }
    case HL_PV_TEXT: case HL_PV_BYTES:
        if (take(r, 4, &b)) return HL_PV_E_SHAPE;
        it->n = get_u32(b);
        return take(r, it->n, &it->s);
    case HL_PV_ARRAY: case HL_PV_MAP:
        if (take(r, 4, &b)) return HL_PV_E_SHAPE;
        it->n = get_u32(b);
        return 0;
    default:
        return HL_PV_E_SHAPE;
    }
}

int hl_pv_next_key(HlPvReader *r, const uint8_t **s, size_t *n)
{
    const uint8_t *b;
    if (take(r, 4, &b)) return HL_PV_E_SHAPE;
    *n = get_u32(b);
    return take(r, *n, s);
}

static int check_value(HlPvReader *r, int depth)
{
    HlPvItem it;
    if (hl_pv_next(r, &it)) return HL_PV_E_SHAPE;
    switch (it.type) {
    case HL_PV_TEXT:
        return hl_utf8_valid(it.s, it.n) ? HL_PV_OK : HL_PV_E_TEXT;
    case HL_PV_ARRAY: case HL_PV_MAP: {
        if (depth >= HL_PV_MAX_DEPTH) return HL_PV_E_DEPTH;
        /* Every item takes at least one byte (a map entry five), so a
         * count the rest of the buffer cannot hold is refused up front. */
        size_t least = it.type == HL_PV_MAP ? 5 : 1;
        if (it.n > (r->len - r->pos) / least) return HL_PV_E_SHAPE;
        const uint8_t *prev = NULL;
        size_t prev_n = 0;
        for (size_t i = 0; i < it.n; i++) {
            if (it.type == HL_PV_MAP) {
                const uint8_t *k;
                size_t kn;
                if (hl_pv_next_key(r, &k, &kn)) return HL_PV_E_SHAPE;
                if (!hl_utf8_valid(k, kn)) return HL_PV_E_TEXT;
                if (prev) {
                    size_t m = prev_n < kn ? prev_n : kn;
                    int c = m ? memcmp(prev, k, m) : 0;
                    if (c > 0 || (c == 0 && prev_n >= kn)) return HL_PV_E_SHAPE;
                }
                prev = k;
                prev_n = kn;
            }
            int rc = check_value(r, depth + 1);
            if (rc) return rc;
        }
        return HL_PV_OK;
    }
    default:
        return HL_PV_OK;
    }
}

int hl_pv_check(const void *buf, size_t len)
{
    HlPvReader r;
    hl_pv_reader_init(&r, buf, len);
    int rc = check_value(&r, 0);
    if (rc) return rc;
    return r.pos == len ? HL_PV_OK : HL_PV_E_SHAPE;
}

const char *hl_pv_error_name(int err)
{
    switch (err) {
    case HL_PV_OK:        return "ok";
    case HL_PV_E_NOMEM:   return "out_of_memory";
    case HL_PV_E_TOO_BIG: return "too_big";
    case HL_PV_E_TEXT:    return "not_text";
    case HL_PV_E_SHAPE:   return "malformed";
    case HL_PV_E_DEPTH:   return "too_deep";
    case HL_PV_E_TYPE:    return "unsupported_value";
    default:              return "invalid";
    }
}
