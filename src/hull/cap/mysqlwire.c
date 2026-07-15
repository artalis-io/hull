/*
 * cap/mysqlwire.c: MySQL / MariaDB wire-protocol codec
 *
 * Framing + typed little-endian I/O for the MySQL client/server protocol. The
 * reader half treats every input byte as untrusted (see cap/mysqlwire.h): all
 * lengths and counts come from a server or an on-path attacker, so each read is
 * bounds-checked and each size computation is overflow-guarded. No fixed-size
 * copies of server data, no strcpy/sprintf on the wire.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/mysqlwire.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Client message writer ────────────────────────────────────────── */

void hl_my_writer_init(HlMyWriter *w)
{
    w->buf = NULL; w->len = 0; w->cap = 0; w->err = 0;
}

void hl_my_writer_free(HlMyWriter *w)
{
    free(w->buf);
    w->buf = NULL; w->len = 0; w->cap = 0; w->err = 0;
}

void hl_my_writer_reset(HlMyWriter *w) { w->len = 0; w->err = 0; }

/* Ensure room for `extra` more bytes. Latches err on overflow / OOM. */
static int my_reserve(HlMyWriter *w, size_t extra)
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
    w->buf = nb; w->cap = ncap;
    return 1;
}

void hl_my_put_u8(HlMyWriter *w, uint8_t v)
{
    if (!my_reserve(w, 1)) return;
    w->buf[w->len++] = v;
}

void hl_my_put_u16(HlMyWriter *w, uint16_t v)
{
    if (!my_reserve(w, 2)) return;
    w->buf[w->len++] = (uint8_t)(v);
    w->buf[w->len++] = (uint8_t)(v >> 8);
}

void hl_my_put_u24(HlMyWriter *w, uint32_t v)
{
    if (!my_reserve(w, 3)) return;
    w->buf[w->len++] = (uint8_t)(v);
    w->buf[w->len++] = (uint8_t)(v >> 8);
    w->buf[w->len++] = (uint8_t)(v >> 16);
}

void hl_my_put_u32(HlMyWriter *w, uint32_t v)
{
    if (!my_reserve(w, 4)) return;
    w->buf[w->len++] = (uint8_t)(v);
    w->buf[w->len++] = (uint8_t)(v >> 8);
    w->buf[w->len++] = (uint8_t)(v >> 16);
    w->buf[w->len++] = (uint8_t)(v >> 24);
}

void hl_my_put_u64(HlMyWriter *w, uint64_t v)
{
    if (!my_reserve(w, 8)) return;
    for (int i = 0; i < 8; i++)
        w->buf[w->len++] = (uint8_t)(v >> (8 * i));
}

void hl_my_put_bytes(HlMyWriter *w, const void *p, size_t n)
{
    if (n == 0) return;
    if (!my_reserve(w, n)) return;
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

void hl_my_put_cstr(HlMyWriter *w, const char *s)
{
    if (s && *s) hl_my_put_bytes(w, s, strlen(s));
    hl_my_put_u8(w, 0);
}

void hl_my_put_lenenc_int(HlMyWriter *w, uint64_t v)
{
    if (v < 251) {
        hl_my_put_u8(w, (uint8_t)v);
    } else if (v < 0x10000) {
        hl_my_put_u8(w, 0xFC);
        hl_my_put_u16(w, (uint16_t)v);
    } else if (v < 0x1000000) {
        hl_my_put_u8(w, 0xFD);
        hl_my_put_u24(w, (uint32_t)v);
    } else {
        hl_my_put_u8(w, 0xFE);
        hl_my_put_u64(w, v);
    }
}

void hl_my_put_lenenc_str(HlMyWriter *w, const void *p, size_t n)
{
    hl_my_put_lenenc_int(w, (uint64_t)n);
    hl_my_put_bytes(w, p, n);
}

size_t hl_my_packet_begin(HlMyWriter *w, uint8_t seq)
{
    size_t marker = w->len;   /* offset of the 3-byte length field */
    hl_my_put_u24(w, 0);      /* placeholder, back-patched by _end */
    hl_my_put_u8(w, seq);
    return marker;
}

void hl_my_packet_end(HlMyWriter *w, size_t marker)
{
    if (w->err) return;
    /* Header is length(3) + seq(1); payload length excludes the 4-byte header. */
    if (marker + 4 > w->len) { w->err = 1; return; }
    size_t plen = w->len - marker - 4;
    if (plen > HL_MY_MAX_MSG) { w->err = 1; return; }   /* 24-bit range */
    uint32_t u = (uint32_t)plen;
    w->buf[marker]     = (uint8_t)(u);
    w->buf[marker + 1] = (uint8_t)(u >> 8);
    w->buf[marker + 2] = (uint8_t)(u >> 16);
}

/* ── Server message reader (untrusted input) ──────────────────────── */

HlMyResult hl_my_frame_next(const uint8_t *buf, size_t avail,
                            HlMyFrame *out, size_t *consumed)
{
    if (consumed) *consumed = 0;
    if (!out) return HL_MY_ERR;

    /* Header = length(3, little-endian) + seq(1). Report an incomplete header
     * before touching buf, so an empty (NULL, 0) buffer is NEED_MORE. */
    if (avail < 4) return HL_MY_NEED_MORE;
    if (!buf) return HL_MY_ERR;   /* avail >= 4 with no buffer is a caller bug */

    uint32_t plen = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                  | ((uint32_t)buf[2] << 16);
    uint8_t seq = buf[3];

    /* plen <= 0xFFFFFF (three bytes), so 4 + plen cannot overflow size_t. */
    size_t total = (size_t)4 + (size_t)plen;
    if (avail < total) return HL_MY_NEED_MORE;

    out->seq      = seq;
    out->body     = buf + 4;
    out->body_len = plen;
    if (consumed) *consumed = total;
    return HL_MY_OK;
}

/* ── Bounded cursor over a frame body (untrusted input) ───────────── */

void hl_my_cursor_init(HlMyCursor *c, const HlMyFrame *f)
{
    if (!f) { c->p = NULL; c->remaining = 0; c->err = 1; return; }
    c->p = f->body;
    c->remaining = f->body_len;
    c->err = 0;
}

uint8_t hl_my_get_u8(HlMyCursor *c)
{
    if (c->err || c->remaining < 1) { c->err = 1; return 0; }
    c->remaining -= 1;
    return *c->p++;
}

uint16_t hl_my_get_u16(HlMyCursor *c)
{
    if (c->err || c->remaining < 2) { c->err = 1; return 0; }
    uint16_t u = (uint16_t)c->p[0] | ((uint16_t)c->p[1] << 8);
    c->p += 2; c->remaining -= 2;
    return u;
}

uint32_t hl_my_get_u24(HlMyCursor *c)
{
    if (c->err || c->remaining < 3) { c->err = 1; return 0; }
    uint32_t u = (uint32_t)c->p[0] | ((uint32_t)c->p[1] << 8)
               | ((uint32_t)c->p[2] << 16);
    c->p += 3; c->remaining -= 3;
    return u;
}

uint32_t hl_my_get_u32(HlMyCursor *c)
{
    if (c->err || c->remaining < 4) { c->err = 1; return 0; }
    uint32_t u = (uint32_t)c->p[0] | ((uint32_t)c->p[1] << 8)
               | ((uint32_t)c->p[2] << 16) | ((uint32_t)c->p[3] << 24);
    c->p += 4; c->remaining -= 4;
    return u;
}

uint64_t hl_my_get_u64(HlMyCursor *c)
{
    if (c->err || c->remaining < 8) { c->err = 1; return 0; }
    uint64_t u = 0;
    for (int i = 0; i < 8; i++)
        u |= (uint64_t)c->p[i] << (8 * i);
    c->p += 8; c->remaining -= 8;
    return u;
}

uint64_t hl_my_get_lenenc_int(HlMyCursor *c, int *is_null)
{
    if (is_null) *is_null = 0;
    uint8_t fb = hl_my_get_u8(c);
    if (c->err) return 0;
    if (fb < 0xFB) return fb;
    if (fb == 0xFB) {                 /* NULL marker (row context) */
        if (is_null) *is_null = 1;
        c->err = 1;                    /* not a valid integer; caller checks null */
        return 0;
    }
    if (fb == 0xFC) return hl_my_get_u16(c);
    if (fb == 0xFD) return hl_my_get_u24(c);
    if (fb == 0xFE) return hl_my_get_u64(c);
    /* 0xFF is never a valid lenenc first byte. */
    c->err = 1;
    return 0;
}

const uint8_t *hl_my_get_bytes(HlMyCursor *c, size_t n)
{
    if (c->err) return NULL;
    if (n > c->remaining) { c->err = 1; return NULL; }
    const uint8_t *r = c->p;
    c->p += n; c->remaining -= n;
    return r;
}

const uint8_t *hl_my_get_lenenc_str(HlMyCursor *c, size_t *out_len)
{
    int is_null = 0;
    uint64_t n = hl_my_get_lenenc_int(c, &is_null);
    if (c->err) return NULL;
    if (n > c->remaining) { c->err = 1; return NULL; }   /* also catches > SIZE_MAX */
    if (out_len) *out_len = (size_t)n;
    const uint8_t *r = c->p;
    c->p += (size_t)n; c->remaining -= (size_t)n;
    return r;
}

const char *hl_my_get_cstr(HlMyCursor *c)
{
    if (c->err) return NULL;
    const uint8_t *nul = c->remaining ? memchr(c->p, 0, c->remaining) : NULL;
    if (!nul) { c->err = 1; return NULL; }
    const char *s = (const char *)c->p;
    size_t adv = (size_t)(nul - c->p) + 1;   /* include the NUL */
    c->p += adv; c->remaining -= adv;
    return s;
}

const uint8_t *hl_my_get_eof_str(HlMyCursor *c, size_t *out_len)
{
    if (c->err) return NULL;
    const uint8_t *r = c->p;
    if (out_len) *out_len = c->remaining;
    c->p += c->remaining; c->remaining = 0;
    return r;
}

int hl_my_cursor_err(const HlMyCursor *c) { return c->err; }

/* ── OK / ERR packet parsing (untrusted input) ────────────────────── */

int hl_my_parse_ok(const HlMyFrame *f, HlMyOk *out)
{
    HlMyCursor c;
    hl_my_cursor_init(&c, f);
    uint8_t hdr = hl_my_get_u8(&c);
    if (c.err || hdr != HL_MY_PKT_OK) return -1;
    HlMyOk r;
    r.affected_rows  = hl_my_get_lenenc_int(&c, NULL);
    r.last_insert_id = hl_my_get_lenenc_int(&c, NULL);
    r.status_flags   = hl_my_get_u16(&c);
    r.warnings       = hl_my_get_u16(&c);
    if (hl_my_cursor_err(&c)) return -1;   /* the info string is optional; ignore */
    if (out) *out = r;
    return 0;
}

int hl_my_parse_err(const HlMyFrame *f, int protocol_41, HlMyErr *out)
{
    HlMyCursor c;
    hl_my_cursor_init(&c, f);
    uint8_t hdr = hl_my_get_u8(&c);
    if (c.err || hdr != HL_MY_PKT_ERR) return -1;
    HlMyErr r;
    memset(&r, 0, sizeof r);
    r.code = hl_my_get_u16(&c);
    if (protocol_41) {
        uint8_t marker = hl_my_get_u8(&c);   /* '#' */
        const uint8_t *ss = hl_my_get_bytes(&c, 5);
        if (hl_my_cursor_err(&c) || marker != '#') return -1;
        memcpy(r.sqlstate, ss, 5);
        r.sqlstate[5] = '\0';
    }
    size_t mlen = 0;
    const uint8_t *msg = hl_my_get_eof_str(&c, &mlen);
    if (hl_my_cursor_err(&c)) return -1;
    r.message = (const char *)msg;
    r.message_len = mlen;
    if (out) *out = r;
    return 0;
}

/* ── Handshake ────────────────────────────────────────────────────── */

/* Bounded copy of a C string into a fixed buffer (no stdio dependency). */
static void copy_bounded(char *dst, size_t dstsz, const char *src)
{
    size_t i = 0;
    if (dstsz == 0) return;
    while (src[i] && i + 1 < dstsz) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int hl_my_parse_handshake(const HlMyFrame *f, HlMyHandshake *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof *out);

    HlMyCursor c;
    hl_my_cursor_init(&c, f);

    out->protocol = hl_my_get_u8(&c);
    if (c.err || out->protocol != 10) return -1;   /* only Handshake v10 */

    const char *sv = hl_my_get_cstr(&c);
    if (!sv) return -1;
    copy_bounded(out->server_version, sizeof out->server_version, sv);

    out->conn_id = hl_my_get_u32(&c);
    const uint8_t *ap1 = hl_my_get_bytes(&c, HL_MY_SCRAMBLE_PART1);
    if (!ap1) return -1;
    memcpy(out->scramble, ap1, HL_MY_SCRAMBLE_PART1);
    (void)hl_my_get_u8(&c);                         /* filler 0x00 */

    uint16_t cap_lower = hl_my_get_u16(&c);
    out->charset = hl_my_get_u8(&c);
    out->status  = hl_my_get_u16(&c);
    uint16_t cap_upper = hl_my_get_u16(&c);
    out->capabilities = (uint32_t)cap_lower | ((uint32_t)cap_upper << 16);

    uint8_t apdata_len = hl_my_get_u8(&c);          /* len of auth-plugin-data */
    (void)hl_my_get_bytes(&c, HL_MY_HANDSHAKE_FILLER);  /* reserved */

    /* auth-plugin-data part 2: the remaining scramble bytes (part2 = 12) plus a
     * NUL, so at least (SCRAMBLE_LEN - part1 + 1) bytes on the wire. */
    const int part2_max = HL_MY_SCRAMBLE_LEN - HL_MY_SCRAMBLE_PART1;   /* 12 */
    if (out->capabilities & HL_MY_CLIENT_SECURE_CONNECTION) {
        int part2 = apdata_len > HL_MY_SCRAMBLE_PART1
                  ? (int)apdata_len - HL_MY_SCRAMBLE_PART1 : part2_max + 1;
        if (part2 < part2_max + 1) part2 = part2_max + 1;
        const uint8_t *ap2 = hl_my_get_bytes(&c, (size_t)part2);
        if (!ap2) return -1;
        int copy2 = part2 > part2_max ? part2_max : part2;
        memcpy(out->scramble + HL_MY_SCRAMBLE_PART1, ap2, (size_t)copy2);
        out->scramble_len = HL_MY_SCRAMBLE_PART1 + copy2;
    } else {
        out->scramble_len = HL_MY_SCRAMBLE_PART1;
    }
    if (c.err) return -1;   /* everything through the scramble must parse */

    /* Trailing auth-plugin name is best-effort: some servers omit its NUL. */
    if (out->capabilities & HL_MY_CLIENT_PLUGIN_AUTH) {
        const char *pl = hl_my_get_cstr(&c);
        if (pl) copy_bounded(out->auth_plugin, sizeof out->auth_plugin, pl);
    }
    return 0;
}

int hl_my_parse_column_def(const HlMyFrame *f, HlMyColumn *out)
{
    HlMyCursor c;
    hl_my_cursor_init(&c, f);
    /* catalog, schema, table, org_table, name, org_name (lenenc strings). */
    for (int i = 0; i < 4; i++) (void)hl_my_get_lenenc_str(&c, NULL);
    size_t nl = 0;
    const uint8_t *name = hl_my_get_lenenc_str(&c, &nl);
    (void)hl_my_get_lenenc_str(&c, NULL);          /* org_name */
    (void)hl_my_get_lenenc_int(&c, NULL);          /* length of fixed fields (0x0c) */
    (void)hl_my_get_u16(&c);                       /* charset */
    (void)hl_my_get_u32(&c);                       /* column length */
    uint8_t type = hl_my_get_u8(&c);
    if (hl_my_cursor_err(&c)) return -1;
    if (out) { out->name = (const char *)name; out->name_len = nl; out->type = type; }
    return 0;
}

int hl_my_parse_prepare_ok(const HlMyFrame *f, HlMyPrepareOk *out)
{
    HlMyCursor c;
    hl_my_cursor_init(&c, f);
    if (hl_my_get_u8(&c) != HL_MY_PKT_OK) return -1;   /* status 0x00 */
    uint32_t stmt = hl_my_get_u32(&c);
    uint16_t ncol = hl_my_get_u16(&c);
    uint16_t npar = hl_my_get_u16(&c);
    (void)hl_my_get_u8(&c);                            /* reserved filler (0x00) */
    if (hl_my_cursor_err(&c)) return -1;
    uint16_t warn = hl_my_get_u16(&c);                 /* best-effort (may be absent) */
    if (out) {
        out->statement_id = stmt;
        out->num_columns  = ncol;
        out->num_params   = npar;
        out->warning_count = warn;
    }
    return 0;
}

void hl_my_build_handshake_response(HlMyWriter *w, uint8_t seq,
                                    uint32_t client_caps, uint8_t charset,
                                    const char *user,
                                    const uint8_t *auth_resp, size_t auth_resp_len,
                                    const char *dbname, const char *plugin)
{
    if (auth_resp_len > 0xFF) auth_resp_len = 0xFF; /* SECURE_CONNECTION 1-byte len */
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_u32(w, client_caps);
    hl_my_put_u32(w, HL_MY_MAX_PACKET);
    hl_my_put_u8(w, charset);
    for (int i = 0; i < HL_MY_HANDSHAKE_RESERVED; i++) hl_my_put_u8(w, 0);
    hl_my_put_cstr(w, user ? user : "");
    hl_my_put_u8(w, (uint8_t)auth_resp_len);        /* length-prefixed auth resp */
    if (auth_resp && auth_resp_len)
        hl_my_put_bytes(w, auth_resp, auth_resp_len);
    if ((client_caps & HL_MY_CLIENT_CONNECT_WITH_DB) && dbname && *dbname)
        hl_my_put_cstr(w, dbname);
    if ((client_caps & HL_MY_CLIENT_PLUGIN_AUTH) && plugin && *plugin)
        hl_my_put_cstr(w, plugin);
    hl_my_packet_end(w, m);
}

void hl_my_build_ssl_request(HlMyWriter *w, uint8_t seq,
                             uint32_t client_caps, uint8_t charset)
{
    size_t m = hl_my_packet_begin(w, seq);
    hl_my_put_u32(w, client_caps);
    hl_my_put_u32(w, HL_MY_MAX_PACKET);
    hl_my_put_u8(w, charset);
    for (int i = 0; i < HL_MY_HANDSHAKE_RESERVED; i++) hl_my_put_u8(w, 0);
    hl_my_packet_end(w, m);
}
