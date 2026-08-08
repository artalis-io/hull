/*
 * cap/valkey.c: the Valkey/Redis HlKvBackend (feature-only, --with=valkey).
 *
 * Maps the narrow byte-oriented KV ops onto RESP commands over cap/valkey_conn:
 *   get -> GET     set -> SET [PX]     del -> DEL     exists -> EXISTS
 *   incr -> INCRBY (+ PEXPIRE NX for a fresh key's TTL)
 *   cas  -> SET NX (set-if-absent) or WATCH/MULTI/EXEC (optimistic, retryable)
 *   clear/scan -> cursor SCAN MATCH <prefix>* (bounded, non-atomic; never KEYS)
 * No generic command escape hatch. Returned value bytes borrow into the
 * connection's buffer (valid until the next op), per the HlKvBackend contract.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/valkey.h"
#include "hull/cap/respwire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct HlKvHandle {
    HlValkeyConn *conn;
    char          errmsg[256];
};

HlKvHandle *hl_kv_valkey_wrap(HlValkeyConn *conn) {
    HlKvHandle *h = (HlKvHandle *)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->conn = conn;
    return h;
}

static void vk_close(HlKvHandle *h) {
    if (!h) return;
    hl_valkey_conn_close(h->conn);
    free(h);
}

static const char *vk_last_error(HlKvHandle *h) { return h ? h->errmsg : ""; }

static void seterr(HlKvHandle *h, const char *m) { snprintf(h->errmsg, sizeof h->errmsg, "%s", m); }

/* Send the built command, read its reply, free the writer. On transport error
 * copies the connection's message into h->errmsg. Returns 0 / -1. */
static int run(HlKvHandle *h, HlRespWriter *w, HlRespValue *out) {
    int rc = hl_valkey_command(h->conn, w, out);
    hl_resp_writer_free(w);
    if (rc != 0) { seterr(h, hl_valkey_conn_error(h->conn)); return -1; }
    return 0;
}

/* A server -ERR reply -> copy its text into h->errmsg and return 1. */
static int is_srv_err(HlKvHandle *h, const HlRespValue *r) {
    if (r->type != HL_RESP_ERR) return 0;
    size_t n = r->str.len < sizeof h->errmsg - 1 ? r->str.len : sizeof h->errmsg - 1;
    memcpy(h->errmsg, r->str.p, n);
    h->errmsg[n] = '\0';
    return 1;
}

/* ── open ─────────────────────────────────────────────────────────────── */

static int vk_open(HlKvHandle **out, const char *dsn, int timeout_ms,
                   char *errbuf, size_t errlen) {
    HlValkeyDsn d;
    if (hl_valkey_dsn_parse(dsn, &d, errbuf, errlen) != 0) return -1;
    if (timeout_ms > 0) d.connect_timeout_ms = timeout_ms;
    HlValkeyConn *conn = NULL;
    int rc = hl_valkey_conn_open(&conn, &d, errbuf, errlen);
    hl_valkey_dsn_scrub(&d);
    if (rc != 0) return -1;
    HlKvHandle *h = hl_kv_valkey_wrap(conn);
    if (!h) { hl_valkey_conn_close(conn); if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory"); return -1; }
    *out = h;
    return 0;
}

/* ── strings ──────────────────────────────────────────────────────────── */

static int vk_get(HlKvHandle *h, const uint8_t *key, size_t klen,
                  const uint8_t **val, size_t *vlen, int *found) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "GET");
    hl_resp_cmd_arg(&w, key, klen);
    HlRespValue r;
    if (run(h, &w, &r) != 0) return -1;
    if (r.type == HL_RESP_NULL) { *found = 0; *val = NULL; *vlen = 0; return 0; }
    if (r.type == HL_RESP_STR)  { *found = 1; *val = (const uint8_t *)r.str.p; *vlen = r.str.len; return 0; }
    if (is_srv_err(h, &r)) return -1;
    seterr(h, "GET: unexpected reply type");
    return -1;
}

static int vk_set(HlKvHandle *h, const uint8_t *key, size_t klen,
                  const uint8_t *val, size_t vlen, int64_t ttl_ms) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, ttl_ms > 0 ? 5 : 3);
    hl_resp_cmd_arg_cstr(&w, "SET");
    hl_resp_cmd_arg(&w, key, klen);
    hl_resp_cmd_arg(&w, val, vlen);
    if (ttl_ms > 0) { hl_resp_cmd_arg_cstr(&w, "PX"); hl_resp_cmd_arg_i64(&w, ttl_ms); }
    HlRespValue r;
    if (run(h, &w, &r) != 0) return -1;
    if (hl_resp_is_ok(&r)) return 0;
    if (is_srv_err(h, &r)) return -1;
    seterr(h, "SET: unexpected reply");
    return -1;
}

static int vk_del(HlKvHandle *h, const uint8_t *key, size_t klen, int *deleted) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "DEL");
    hl_resp_cmd_arg(&w, key, klen);
    HlRespValue r;
    if (run(h, &w, &r) != 0) return -1;
    if (r.type == HL_RESP_INT) { *deleted = r.i > 0; return 0; }
    if (is_srv_err(h, &r)) return -1;
    seterr(h, "DEL: unexpected reply"); return -1;
}

static int vk_exists(HlKvHandle *h, const uint8_t *key, size_t klen, int *present) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "EXISTS");
    hl_resp_cmd_arg(&w, key, klen);
    HlRespValue r;
    if (run(h, &w, &r) != 0) return -1;
    if (r.type == HL_RESP_INT) { *present = r.i > 0; return 0; }
    if (is_srv_err(h, &r)) return -1;
    seterr(h, "EXISTS: unexpected reply"); return -1;
}

/* ── counters ─────────────────────────────────────────────────────────── */

static int vk_incr(HlKvHandle *h, const uint8_t *key, size_t klen,
                   int64_t by, int64_t ttl_ms, int64_t *newval) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 3);
    hl_resp_cmd_arg_cstr(&w, "INCRBY");
    hl_resp_cmd_arg(&w, key, klen);
    hl_resp_cmd_arg_i64(&w, by);
    HlRespValue r;
    if (run(h, &w, &r) != 0) return -1;
    if (r.type != HL_RESP_INT) { if (is_srv_err(h, &r)) return -1; seterr(h, "INCRBY: not an integer"); return -1; }
    *newval = r.i;
    if (ttl_ms > 0) {
        /* Apply the TTL only to a fresh key (no existing expiry): PEXPIRE .. NX.
         * Existing keys keep their TTL. Best-effort; ignore the reply value. */
        hl_resp_writer_init(&w);
        hl_resp_cmd_begin(&w, 4);
        hl_resp_cmd_arg_cstr(&w, "PEXPIRE");
        hl_resp_cmd_arg(&w, key, klen);
        hl_resp_cmd_arg_i64(&w, ttl_ms);
        hl_resp_cmd_arg_cstr(&w, "NX");
        HlRespValue r2;
        (void)run(h, &w, &r2);
    }
    return 0;
}

/* ── compare-and-swap ─────────────────────────────────────────────────── */

static HlKvCasResult set_if_absent(HlKvHandle *h, const uint8_t *key, size_t klen,
                                   const uint8_t *newv, size_t nlen, int64_t ttl_ms) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, ttl_ms > 0 ? 6 : 4);
    hl_resp_cmd_arg_cstr(&w, "SET");
    hl_resp_cmd_arg(&w, key, klen);
    hl_resp_cmd_arg(&w, newv, nlen);
    hl_resp_cmd_arg_cstr(&w, "NX");
    if (ttl_ms > 0) { hl_resp_cmd_arg_cstr(&w, "PX"); hl_resp_cmd_arg_i64(&w, ttl_ms); }
    HlRespValue r;
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;
    if (hl_resp_is_ok(&r)) return HL_KV_CAS_OK;
    if (r.type == HL_RESP_NULL) return HL_KV_CAS_MISMATCH;   /* key existed */
    is_srv_err(h, &r);
    return HL_KV_CAS_ERROR;
}

/* One WATCH/GET/compare/MULTI/SET/EXEC pass. Returns OK / MISMATCH / CONFLICT
 * (retry) / ERROR. */
static HlKvCasResult cas_attempt(HlKvHandle *h, const uint8_t *key, size_t klen,
                                 const uint8_t *expected, size_t elen,
                                 const uint8_t *newv, size_t nlen, int64_t ttl_ms) {
    HlRespWriter w; HlRespValue r;
    /* WATCH */
    hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "WATCH"); hl_resp_cmd_arg(&w, key, klen);
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;
    if (!hl_resp_is_ok(&r)) { is_srv_err(h, &r); return HL_KV_CAS_ERROR; }
    /* GET + compare (value borrows into the buffer only until the next cmd) */
    hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "GET"); hl_resp_cmd_arg(&w, key, klen);
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;
    int match = (r.type == HL_RESP_STR && r.str.len == elen &&
                 (elen == 0 || memcmp(r.str.p, expected, elen) == 0));
    if (!match) {
        hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 1); hl_resp_cmd_arg_cstr(&w, "UNWATCH");
        HlRespValue u; (void)run(h, &w, &u);
        return HL_KV_CAS_MISMATCH;
    }
    /* MULTI */
    hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 1); hl_resp_cmd_arg_cstr(&w, "MULTI");
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;
    /* SET (queued) */
    hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, ttl_ms > 0 ? 5 : 3);
    hl_resp_cmd_arg_cstr(&w, "SET"); hl_resp_cmd_arg(&w, key, klen); hl_resp_cmd_arg(&w, newv, nlen);
    if (ttl_ms > 0) { hl_resp_cmd_arg_cstr(&w, "PX"); hl_resp_cmd_arg_i64(&w, ttl_ms); }
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;   /* +QUEUED */
    /* EXEC */
    hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 1); hl_resp_cmd_arg_cstr(&w, "EXEC");
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;
    if (r.type == HL_RESP_ARRAY) return HL_KV_CAS_OK;        /* committed */
    if (r.type == HL_RESP_NULL)  return HL_KV_CAS_CONFLICT;  /* key changed -> retry */
    is_srv_err(h, &r);
    return HL_KV_CAS_ERROR;
}

static HlKvCasResult vk_cas(HlKvHandle *h, const uint8_t *key, size_t klen,
                            const uint8_t *expected, size_t elen, int has_expected,
                            const uint8_t *newv, size_t nlen, int64_t ttl_ms) {
    if (!has_expected) return set_if_absent(h, key, klen, newv, nlen, ttl_ms);
    for (int attempt = 0; attempt < 8; attempt++) {
        HlKvCasResult r = cas_attempt(h, key, klen, expected, elen, newv, nlen, ttl_ms);
        if (r != HL_KV_CAS_CONFLICT) return r;
    }
    return HL_KV_CAS_CONFLICT;
}

/* ── SCAN-based clear + scan ──────────────────────────────────────────── */

/* Run one SCAN step: SCAN <cursor> MATCH <prefix>* COUNT 128. Fills *out (an
 * array [cursor, [keys]]). Returns 0 / -1. */
static int scan_step(HlKvHandle *h, const char *cursor, const uint8_t *prefix, size_t plen,
                     HlRespValue *out) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 6);
    hl_resp_cmd_arg_cstr(&w, "SCAN");
    hl_resp_cmd_arg_cstr(&w, cursor);
    hl_resp_cmd_arg_cstr(&w, "MATCH");
    /* pattern = prefix + '*' (glob metacharacters in the prefix are the caller's
     * concern; our namespace prefixes are ASCII "kv:<ns>:"). */
    {
        HlRespWriter pw; (void)pw;
        /* Build the MATCH arg as prefix bytes followed by '*'. */
        size_t patlen = plen + 1;
        char stackbuf[256];
        char *pat = patlen <= sizeof stackbuf ? stackbuf : (char *)malloc(patlen);
        if (!pat) { hl_resp_writer_free(&w); seterr(h, "out of memory"); return -1; }
        memcpy(pat, prefix, plen); pat[plen] = '*';
        hl_resp_cmd_arg(&w, pat, patlen);
        if (pat != stackbuf) free(pat);
    }
    hl_resp_cmd_arg_cstr(&w, "COUNT");
    hl_resp_cmd_arg_cstr(&w, "128");
    if (run(h, &w, out) != 0) return -1;
    if (out->type != HL_RESP_ARRAY || out->arr.count != 2 ||
        out->arr.items[0].type != HL_RESP_STR ||
        out->arr.items[1].type != HL_RESP_ARRAY) {
        if (is_srv_err(h, out)) return -1;
        seterr(h, "SCAN: unexpected reply"); return -1;
    }
    return 0;
}

static int vk_scan(HlKvHandle *h, const uint8_t *prefix, size_t plen, size_t limit,
                   HlKvScanCb cb, void *cbctx) {
    char cursor[64]; snprintf(cursor, sizeof cursor, "0");
    size_t emitted = 0;
    do {
        HlRespValue r;
        if (scan_step(h, cursor, prefix, plen, &r) != 0) return -1;
        const HlRespValue *keys = &r.arr.items[1];
        /* copy the next cursor before any callback/command can clobber rbuf */
        char next[64];
        size_t cl = r.arr.items[0].str.len < sizeof next - 1 ? r.arr.items[0].str.len : sizeof next - 1;
        memcpy(next, r.arr.items[0].str.p, cl); next[cl] = '\0';
        for (size_t i = 0; i < keys->arr.count; i++) {
            const HlRespValue *k = &keys->arr.items[i];
            if (k->type != HL_RESP_STR) continue;
            /* strip the physical prefix; cb copies the borrowed bytes */
            const uint8_t *lk = (const uint8_t *)k->str.p + (k->str.len >= plen ? plen : k->str.len);
            size_t lklen = k->str.len >= plen ? k->str.len - plen : 0;
            if (cb(cbctx, lk, lklen) != 0) return 0;   /* caller stopped early */
            if (limit && ++emitted >= limit) return 0;
        }
        snprintf(cursor, sizeof cursor, "%s", next);
    } while (strcmp(cursor, "0") != 0);
    return 0;
}

/* clear: SCAN + batched DEL under the prefix. Bounded, non-atomic. */
static int vk_clear(HlKvHandle *h, const uint8_t *prefix, size_t plen, int64_t *removed) {
    char cursor[64]; snprintf(cursor, sizeof cursor, "0");
    int64_t total = 0;
    do {
        HlRespValue r;
        if (scan_step(h, cursor, prefix, plen, &r) != 0) return -1;
        const HlRespValue *keys = &r.arr.items[1];
        char next[64];
        size_t cl = r.arr.items[0].str.len < sizeof next - 1 ? r.arr.items[0].str.len : sizeof next - 1;
        memcpy(next, r.arr.items[0].str.p, cl); next[cl] = '\0';
        if (keys->arr.count > 0) {
            /* DEL k1 k2 ...  (cmd_arg copies each key out of rbuf before send) */
            HlRespWriter w; hl_resp_writer_init(&w);
            hl_resp_cmd_begin(&w, keys->arr.count + 1);
            hl_resp_cmd_arg_cstr(&w, "DEL");
            for (size_t i = 0; i < keys->arr.count; i++) {
                const HlRespValue *k = &keys->arr.items[i];
                if (k->type == HL_RESP_STR) hl_resp_cmd_arg(&w, k->str.p, k->str.len);
                else hl_resp_cmd_arg(&w, "", 0);
            }
            HlRespValue dr;
            if (run(h, &w, &dr) != 0) return -1;
            if (dr.type == HL_RESP_INT) total += dr.i;
        }
        snprintf(cursor, sizeof cursor, "%s", next);
    } while (strcmp(cursor, "0") != 0);
    if (removed) *removed = total;
    return 0;
}

/* ── vtable ───────────────────────────────────────────────────────────── */

static const char *const valkey_schemes[] = { "valkey", "valkeys", "redis", "rediss", NULL };

const HlKvBackend hl_kv_backend_valkey = {
    .name    = "valkey",
    .schemes = valkey_schemes,
    .caps    = HL_KV_CAP_TTL | HL_KV_CAP_ATOMIC_INCREMENT | HL_KV_CAP_COMPARE_EXCHANGE |
               HL_KV_CAP_SCAN | HL_KV_CAP_PERSISTENT | HL_KV_CAP_SHARED |
               HL_KV_CAP_EVICTION | HL_KV_CAP_TRANSACTIONS,
    .open       = vk_open,
    .close      = vk_close,
    .last_error = vk_last_error,
    .get        = vk_get,
    .set        = vk_set,
    .del        = vk_del,
    .exists     = vk_exists,
    .incr       = vk_incr,
    .cas        = vk_cas,
    .clear      = vk_clear,
    .scan       = vk_scan,
};
