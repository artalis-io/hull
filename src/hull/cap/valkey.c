/*
 * cap/valkey.c: the Valkey/Redis HlKvBackend (feature-only, --with=valkey).
 *
 * Maps the narrow byte-oriented KV ops onto RESP commands over cap/valkey_conn:
 *   get -> GET     set -> SET [PX]     del -> DEL     exists -> EXISTS
 *   incr -> INCRBY; with a TTL, a WATCH/EXISTS/MULTI/INCRBY[/PEXPIRE]/EXEC
 *     transaction so the expiry lands ONLY on a freshly-created key (an existing
 *     key keeps its own TTL) - atomic, not a racy INCRBY-then-PEXPIRE.
 *   cas  -> SET NX (set-if-absent) or WATCH/MULTI/EXEC (optimistic, retryable ->
 *     the four-state HlKvCasResult OK/MISMATCH/CONFLICT/ERROR; a GET error is an
 *     ERROR, never a mismatch; every abort leaves the connection clean).
 *   clear/scan -> cursor SCAN MATCH <glob-escaped-prefix>* (bounded, non-atomic;
 *     never blocking KEYS; iteration-capped; scan strips the prefix before cb).
 * No generic command escape hatch. All memory is the connection's pluggable
 * HlAllocator.
 *
 * Invariants (validated in the pre-2b backend review):
 *  - Returned value bytes BORROW into the connection buffer, valid only until
 *    the next op; every internal borrow (SCAN keys, cursor) is copied first.
 *  - No auto-reconnect: a transport failure fails the op and poisons the handle
 *    (the caller reopens). Non-idempotent ops (incr, EXEC) are NEVER replayed.
 *  - Physical keys/prefixes come from the caller and are treated opaquely. The
 *    milestone-1 wiring (Phase 2b) makes them collision-free + glob-free by
 *    hex-encoding the namespace; the SCAN pattern is glob-escaped here so the
 *    backend is safe for ANY prefix bytes it is handed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/valkey.h"
#include "hull/cap/respwire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KV_VALKEY_ERRMSG 256u   /* matches the connection's error-buffer size */

struct HlKvHandle {
    HlValkeyConn *conn;
    HlAllocator  *alloc;
    char          errmsg[KV_VALKEY_ERRMSG];
};

HlKvHandle *hl_kv_valkey_wrap(HlValkeyConn *conn) {
    HlAllocator *alloc = hl_valkey_conn_alloc(conn);
    HlKvHandle *h = (HlKvHandle *)hl_alloc_calloc(alloc, 1, sizeof *h);
    if (!h) return NULL;
    h->conn = conn;
    h->alloc = alloc;
    return h;
}

static void vk_close(HlKvHandle *h) {
    if (!h) return;
    HlAllocator *alloc = h->alloc;
    hl_valkey_conn_close(h->conn);
    hl_alloc_free(alloc, h, sizeof *h);
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
                   HlAllocator *alloc, char *errbuf, size_t errlen) {
    HlValkeyDsn d;
    if (hl_valkey_dsn_parse(dsn, &d, errbuf, errlen) != 0) return -1;
    if (timeout_ms > 0) d.connect_timeout_ms = timeout_ms;
    HlValkeyConn *conn = NULL;
    int rc = hl_valkey_conn_open(&conn, &d, alloc, errbuf, errlen);
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

/* Best-effort UNWATCH / DISCARD to leave the connection clean after aborting a
 * transaction on a still-alive connection (skip when the conn is already dead,
 * i.e. a prior run() returned -1). */
static void kv_unwatch(HlKvHandle *h) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 1); hl_resp_cmd_arg_cstr(&w, "UNWATCH");
    HlRespValue r; (void)run(h, &w, &r);
}

static int incr_simple(HlKvHandle *h, const uint8_t *key, size_t klen,
                       int64_t by, int64_t *newval) {
    HlRespWriter w; hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, 3);
    hl_resp_cmd_arg_cstr(&w, "INCRBY");
    hl_resp_cmd_arg(&w, key, klen);
    hl_resp_cmd_arg_i64(&w, by);
    HlRespValue r;
    if (run(h, &w, &r) != 0) return -1;
    if (r.type != HL_RESP_INT) { if (is_srv_err(h, &r)) return -1; seterr(h, "INCRBY: not an integer"); return -1; }
    *newval = r.i;
    return 0;
}

static int vk_incr(HlKvHandle *h, const uint8_t *key, size_t klen,
                   int64_t by, int64_t ttl_ms, int64_t *newval) {
    if (ttl_ms <= 0) return incr_simple(h, key, klen, by, newval);

    /* TTL requested: apply it ONLY to a freshly-created key (an existing key
     * keeps its own expiry, including "no expiry"), atomically. WATCH the key,
     * decide fresh-vs-existing under that watch, then MULTI { INCRBY [PEXPIRE] }
     * EXEC. EXEC aborts (null) if the key changed since WATCH -> retry. This is
     * the transactional handling a separate INCRBY + PEXPIRE cannot provide. */
    for (int attempt = 0; attempt < 8; attempt++) {
        HlRespWriter w; HlRespValue r;
        hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 2);
        hl_resp_cmd_arg_cstr(&w, "WATCH"); hl_resp_cmd_arg(&w, key, klen);
        if (run(h, &w, &r) != 0) return -1;
        if (!hl_resp_is_ok(&r)) { is_srv_err(h, &r); return -1; }

        hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 2);
        hl_resp_cmd_arg_cstr(&w, "EXISTS"); hl_resp_cmd_arg(&w, key, klen);
        if (run(h, &w, &r) != 0) return -1;
        if (r.type != HL_RESP_INT) { is_srv_err(h, &r); kv_unwatch(h); return -1; }
        int exists = r.i > 0;

        hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 1); hl_resp_cmd_arg_cstr(&w, "MULTI");
        if (run(h, &w, &r) != 0) return -1;

        hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 3);
        hl_resp_cmd_arg_cstr(&w, "INCRBY"); hl_resp_cmd_arg(&w, key, klen); hl_resp_cmd_arg_i64(&w, by);
        if (run(h, &w, &r) != 0) return -1;                 /* +QUEUED */
        if (!exists) {
            hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 3);
            hl_resp_cmd_arg_cstr(&w, "PEXPIRE"); hl_resp_cmd_arg(&w, key, klen); hl_resp_cmd_arg_i64(&w, ttl_ms);
            if (run(h, &w, &r) != 0) return -1;             /* +QUEUED */
        }
        hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 1); hl_resp_cmd_arg_cstr(&w, "EXEC");
        if (run(h, &w, &r) != 0) return -1;
        if (r.type == HL_RESP_NULL) continue;               /* watched key changed -> retry */
        if (r.type == HL_RESP_ARRAY && r.arr.count >= 1 && r.arr.items[0].type == HL_RESP_INT) {
            *newval = r.arr.items[0].i;                      /* INCRBY result */
            return 0;
        }
        if (is_srv_err(h, &r)) return -1;
        seterr(h, "INCRBY(txn): unexpected EXEC reply"); return -1;
    }
    seterr(h, "incr: contended past retry budget");
    return -1;
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
 * (retry) / ERROR. Every non-transport abort leaves the connection clean
 * (UNWATCH after a mismatch; EXEC itself clears WATCH+MULTI). A GET error is an
 * ERROR, never a mismatch. */
static HlKvCasResult cas_attempt(HlKvHandle *h, const uint8_t *key, size_t klen,
                                 const uint8_t *expected, size_t elen,
                                 const uint8_t *newv, size_t nlen, int64_t ttl_ms) {
    HlRespWriter w; HlRespValue r;
    /* WATCH */
    hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "WATCH"); hl_resp_cmd_arg(&w, key, klen);
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;
    if (!hl_resp_is_ok(&r)) { is_srv_err(h, &r); return HL_KV_CAS_ERROR; }

    /* GET + compare (the value borrows into the buffer only until the next cmd,
     * so we compare BEFORE issuing any further command). */
    hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 2);
    hl_resp_cmd_arg_cstr(&w, "GET"); hl_resp_cmd_arg(&w, key, klen);
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;
    if (r.type == HL_RESP_ERR) {          /* e.g. WRONGTYPE - a real error, not a mismatch */
        is_srv_err(h, &r); kv_unwatch(h); return HL_KV_CAS_ERROR;
    }
    /* STR compares by bytes; NULL (absent key) fails to match a provided value. */
    int match = (r.type == HL_RESP_STR && r.str.len == elen &&
                 (elen == 0 || memcmp(r.str.p, expected, elen) == 0));
    if (!match) { kv_unwatch(h); return HL_KV_CAS_MISMATCH; }

    /* MULTI { SET } EXEC */
    hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 1); hl_resp_cmd_arg_cstr(&w, "MULTI");
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;
    hl_resp_writer_init(&w);
    hl_resp_cmd_begin(&w, ttl_ms > 0 ? 5 : 3);
    hl_resp_cmd_arg_cstr(&w, "SET"); hl_resp_cmd_arg(&w, key, klen); hl_resp_cmd_arg(&w, newv, nlen);
    if (ttl_ms > 0) { hl_resp_cmd_arg_cstr(&w, "PX"); hl_resp_cmd_arg_i64(&w, ttl_ms); }
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;   /* +QUEUED (or -ERR queued -> EXECABORT) */
    hl_resp_writer_init(&w); hl_resp_cmd_begin(&w, 1); hl_resp_cmd_arg_cstr(&w, "EXEC");
    if (run(h, &w, &r) != 0) return HL_KV_CAS_ERROR;
    if (r.type == HL_RESP_ARRAY) return HL_KV_CAS_OK;        /* committed */
    if (r.type == HL_RESP_NULL)  return HL_KV_CAS_CONFLICT;  /* key changed -> retry */
    is_srv_err(h, &r);                                        /* EXECABORT / error */
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
    /* Glob-safe "<prefix>*" written straight into the command (the prefix's
     * binary glob metacharacters are escaped so they match literally; only the
     * trailing '*' is a wildcard). No temp allocation. */
    hl_resp_cmd_arg_globprefix(&w, prefix, plen);
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

/* Cursor iterations before we give up on a server that never returns "0". */
#define VK_SCAN_MAX_ITERS (1u << 20)
/* A Redis SCAN cursor is an opaque uint64 (<= 20 decimal digits). */
#define KV_VALKEY_CURSOR_MAX 32u

/* Copy the reply's cursor (which borrows into the buffer) into `out` before any
 * further command reuses that buffer. -1 on a malformed / oversized cursor. */
static int copy_cursor(HlKvHandle *h, const HlRespValue *cur, char out[KV_VALKEY_CURSOR_MAX]) {
    if (cur->type != HL_RESP_STR || cur->str.len >= KV_VALKEY_CURSOR_MAX) {
        seterr(h, "SCAN: malformed cursor"); return -1;
    }
    memcpy(out, cur->str.p, cur->str.len);
    out[cur->str.len] = '\0';
    return 0;
}

static int vk_scan(HlKvHandle *h, const uint8_t *prefix, size_t plen, size_t limit,
                   HlKvScanCb cb, void *cbctx) {
    char cursor[KV_VALKEY_CURSOR_MAX]; snprintf(cursor, sizeof cursor, "0");
    size_t emitted = 0;
    unsigned iters = 0;
    do {
        if (++iters > VK_SCAN_MAX_ITERS) { seterr(h, "SCAN: cursor did not terminate"); return -1; }
        HlRespValue r;
        if (scan_step(h, cursor, prefix, plen, &r) != 0) return -1;
        const HlRespValue *keys = &r.arr.items[1];
        /* copy the next cursor before any callback/command can clobber the buffer */
        char next[KV_VALKEY_CURSOR_MAX];
        if (copy_cursor(h, &r.arr.items[0], next) != 0) return -1;
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
    char cursor[KV_VALKEY_CURSOR_MAX]; snprintf(cursor, sizeof cursor, "0");
    int64_t total = 0;
    unsigned iters = 0;
    do {
        if (++iters > VK_SCAN_MAX_ITERS) { seterr(h, "SCAN: cursor did not terminate"); return -1; }
        HlRespValue r;
        if (scan_step(h, cursor, prefix, plen, &r) != 0) return -1;
        const HlRespValue *keys = &r.arr.items[1];
        char next[KV_VALKEY_CURSOR_MAX];
        if (copy_cursor(h, &r.arr.items[0], next) != 0) return -1;
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
