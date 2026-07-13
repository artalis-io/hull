/*
 * cap/pgwire.c: PostgreSQL v3 wire-protocol codec
 *
 * Framing + typed I/O for the PostgreSQL frontend/backend protocol. The
 * reader half treats every input byte as untrusted (see cap/pgwire.h): all
 * lengths and counts come from a server or an on-path attacker, so each read
 * is bounds-checked and each size computation is overflow-guarded. There are
 * no fixed-size copies of server data and no strcpy/sprintf on the wire.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/pgwire.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Frontend message writer ──────────────────────────────────────── */

void hl_pg_writer_init(HlPgWriter *w)
{
    w->buf = NULL;
    w->len = 0;
    w->cap = 0;
    w->err = 0;
}

void hl_pg_writer_free(HlPgWriter *w)
{
    free(w->buf);
    w->buf = NULL;
    w->len = 0;
    w->cap = 0;
    w->err = 0;
}

void hl_pg_writer_reset(HlPgWriter *w)
{
    w->len = 0;
    w->err = 0;
}

/* Ensure room for `extra` more bytes. Latches err on overflow / OOM. */
static int pg_reserve(HlPgWriter *w, size_t extra)
{
    if (w->err) return 0;
    if (extra > SIZE_MAX - w->len) { w->err = 1; return 0; }
    size_t need = w->len + extra;
    if (need <= w->cap) return 1;

    size_t ncap = w->cap ? w->cap : 64;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) { ncap = need; break; }
        ncap *= 2;
    }
    uint8_t *nb = realloc(w->buf, ncap);
    if (!nb) { w->err = 1; return 0; }
    w->buf = nb;
    w->cap = ncap;
    return 1;
}

void hl_pg_put_u8(HlPgWriter *w, uint8_t v)
{
    if (!pg_reserve(w, 1)) return;
    w->buf[w->len++] = v;
}

void hl_pg_put_i16(HlPgWriter *w, int16_t v)
{
    if (!pg_reserve(w, 2)) return;
    uint16_t u = (uint16_t)v;
    w->buf[w->len++] = (uint8_t)(u >> 8);
    w->buf[w->len++] = (uint8_t)(u);
}

void hl_pg_put_i32(HlPgWriter *w, int32_t v)
{
    if (!pg_reserve(w, 4)) return;
    uint32_t u = (uint32_t)v;
    w->buf[w->len++] = (uint8_t)(u >> 24);
    w->buf[w->len++] = (uint8_t)(u >> 16);
    w->buf[w->len++] = (uint8_t)(u >> 8);
    w->buf[w->len++] = (uint8_t)(u);
}

void hl_pg_put_bytes(HlPgWriter *w, const void *p, size_t n)
{
    if (n == 0) return;
    if (!pg_reserve(w, n)) return;
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

void hl_pg_put_cstr(HlPgWriter *w, const char *s)
{
    if (s && *s) hl_pg_put_bytes(w, s, strlen(s));
    hl_pg_put_u8(w, 0);
}

size_t hl_pg_msg_begin(HlPgWriter *w, uint8_t type)
{
    if (type != 0) hl_pg_put_u8(w, type);
    size_t marker = w->len;   /* offset of the 4-byte length field */
    hl_pg_put_i32(w, 0);      /* placeholder, back-patched by _end */
    return marker;
}

void hl_pg_msg_end(HlPgWriter *w, size_t marker)
{
    if (w->err) return;
    /* Length counts itself + the body, not the type tag. `marker` is the
     * offset of the length field, so len = current_len - marker. */
    if (marker + 4 > w->len) { w->err = 1; return; }
    size_t len = w->len - marker;
    if (len > 0x7fffffffu) { w->err = 1; return; }   /* int32 range */
    uint32_t u = (uint32_t)len;
    w->buf[marker]     = (uint8_t)(u >> 24);
    w->buf[marker + 1] = (uint8_t)(u >> 16);
    w->buf[marker + 2] = (uint8_t)(u >> 8);
    w->buf[marker + 3] = (uint8_t)(u);
}

/* ── Frontend message builders ────────────────────────────────────── */

void hl_pg_build_startup(HlPgWriter *w, const char *user, const char *database)
{
    size_t m = hl_pg_msg_begin(w, 0);   /* startup has no type tag */
    hl_pg_put_i32(w, 196608);           /* protocol version 3.0 */
    hl_pg_put_cstr(w, "user");
    hl_pg_put_cstr(w, user ? user : "");
    if (database && *database) {
        hl_pg_put_cstr(w, "database");
        hl_pg_put_cstr(w, database);
    }
    hl_pg_put_u8(w, 0);                 /* terminating empty parameter name */
    hl_pg_msg_end(w, m);
}

void hl_pg_build_password(HlPgWriter *w, const char *password)
{
    size_t m = hl_pg_msg_begin(w, HL_PG_F_PASSWORD);
    hl_pg_put_cstr(w, password ? password : "");
    hl_pg_msg_end(w, m);
}

void hl_pg_build_terminate(HlPgWriter *w)
{
    size_t m = hl_pg_msg_begin(w, HL_PG_F_TERMINATE);
    hl_pg_msg_end(w, m);
}

/* ── Backend message reader (untrusted input) ─────────────────────── */

HlPgResult hl_pg_frame_next(const uint8_t *buf, size_t avail,
                            HlPgFrame *out, size_t *consumed)
{
    if (consumed) *consumed = 0;
    if (!out) return HL_PG_ERR;

    /* A backend message is tag(1) + length(4, big-endian) + body. Report an
     * incomplete header before touching buf, so an empty (NULL, 0) buffer is
     * NEED_MORE rather than a spurious error on the first read. */
    if (avail < 5) return HL_PG_NEED_MORE;
    if (!buf) return HL_PG_ERR;   /* avail >= 5 with no buffer is a caller bug */

    uint8_t type = buf[0];
    uint32_t ulen = ((uint32_t)buf[1] << 24) | ((uint32_t)buf[2] << 16)
                  | ((uint32_t)buf[3] << 8)  | ((uint32_t)buf[4]);

    /* The length field covers itself (>= 4) and is capped so a hostile
     * value cannot drive an allocation or a wait for gigabytes. Because
     * ulen <= HL_PG_MAX_MSG, `1 + ulen` cannot overflow size_t. */
    if (ulen < 4 || ulen > HL_PG_MAX_MSG) return HL_PG_ERR;

    size_t total = (size_t)1 + (size_t)ulen;
    if (avail < total) return HL_PG_NEED_MORE;

    out->type     = type;
    out->body     = buf + 5;
    out->body_len = ulen - 4;   /* body excludes the 4 length bytes */
    if (consumed) *consumed = total;
    return HL_PG_OK;
}

/* ── Bounded cursor over a frame body (untrusted input) ───────────── */

void hl_pg_cursor_init(HlPgCursor *c, const HlPgFrame *f)
{
    if (!f) { c->p = NULL; c->remaining = 0; c->err = 1; return; }
    c->p = f->body;
    c->remaining = f->body_len;
    c->err = 0;
}

uint8_t hl_pg_get_u8(HlPgCursor *c)
{
    if (c->err || c->remaining < 1) { c->err = 1; return 0; }
    c->remaining -= 1;
    return *c->p++;
}

int16_t hl_pg_get_i16(HlPgCursor *c)
{
    if (c->err || c->remaining < 2) { c->err = 1; return 0; }
    uint16_t u = ((uint16_t)c->p[0] << 8) | (uint16_t)c->p[1];
    c->p += 2;
    c->remaining -= 2;
    return (int16_t)u;
}

int32_t hl_pg_get_i32(HlPgCursor *c)
{
    if (c->err || c->remaining < 4) { c->err = 1; return 0; }
    uint32_t u = ((uint32_t)c->p[0] << 24) | ((uint32_t)c->p[1] << 16)
               | ((uint32_t)c->p[2] << 8)  | (uint32_t)c->p[3];
    c->p += 4;
    c->remaining -= 4;
    return (int32_t)u;
}

const uint8_t *hl_pg_get_bytes(HlPgCursor *c, size_t n)
{
    if (c->err) return NULL;
    if (n > c->remaining) { c->err = 1; return NULL; }
    const uint8_t *r = c->p;
    c->p += n;
    c->remaining -= n;
    return r;
}

const char *hl_pg_get_cstr(HlPgCursor *c)
{
    if (c->err) return NULL;
    const uint8_t *nul = c->remaining ? memchr(c->p, 0, c->remaining) : NULL;
    if (!nul) { c->err = 1; return NULL; }
    const char *s = (const char *)c->p;
    size_t adv = (size_t)(nul - c->p) + 1;   /* include the NUL */
    c->p += adv;
    c->remaining -= adv;
    return s;
}

int hl_pg_cursor_err(const HlPgCursor *c)
{
    return c->err;
}
