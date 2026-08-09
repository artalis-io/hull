/*
 * cap/kv.c: app-facing KV connection capability (base-resident).
 *
 * Resolves a composed backend by DSN scheme (hl_kv_backend_select), opens a
 * handle, and forwards the narrow op set through the HlKvBackend vtable. A
 * cap-gated op the backend lacks (incr / cas / scan left NULL) fails closed with
 * an "unsupported" message rather than dereferencing a NULL slot. The runtime
 * bindings (runtime/{lua,js}/mod_kv.c) call only through here; they never touch
 * a backend vtable directly.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/kv.h"

#include <stdio.h>
#include <string.h>

#include "hull/utils/alloc.h"

struct HlKvConn {
    const HlKvBackend *b;
    HlKvHandle        *h;
    HlAllocator       *alloc;   /* NULL -> process default; owns this struct */
    char               err[256];/* "unsupported"/dispatch-side messages */
};

int hl_cap_kv_open(HlKvConn **out, const char *dsn, int timeout_ms,
                   HlAllocator *alloc, char *errbuf, size_t errlen) {
    if (out) *out = NULL;
    if (!out || !dsn) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "kv: null argument");
        return -1;
    }
    const HlKvBackend *b = hl_kv_backend_select(dsn);
    if (!b) {
        if (errbuf && errlen)
            snprintf(errbuf, errlen,
                     "no KV backend for this DSN scheme; compose it with "
                     "'hull build --with=valkey' (hull feature install valkey)");
        return -1;
    }
    HlKvHandle *h = NULL;
    if (b->open(&h, dsn, timeout_ms, alloc, errbuf, errlen) != 0)
        return -1;   /* open filled errbuf */

    HlKvConn *c = hl_alloc_calloc(alloc, 1, sizeof *c);
    if (!c) {
        b->close(h);
        if (errbuf && errlen) snprintf(errbuf, errlen, "kv: out of memory");
        return -1;
    }
    c->b = b;
    c->h = h;
    c->alloc = alloc;
    *out = c;
    return 0;
}

void hl_cap_kv_close(HlKvConn *c) {
    if (!c) return;
    c->b->close(c->h);
    hl_alloc_free(c->alloc, c, sizeof *c);
}

const char *hl_cap_kv_error(HlKvConn *c) {
    if (!c) return "kv: null connection";
    if (c->err[0]) return c->err;          /* a dispatch-side error takes priority */
    return c->b->last_error(c->h);         /* else the backend's last message */
}

uint32_t hl_cap_kv_caps(HlKvConn *c) { return c ? c->b->caps : 0u; }

const char *hl_cap_kv_backend_name(HlKvConn *c) {
    return (c && c->b->name) ? c->b->name : "";
}

/* Record an op-layer error (used for the cap-gate "unsupported" path so it
 * survives hl_cap_kv_error even though the backend never ran). */
static int kv_unsupported(HlKvConn *c, const char *op) {
    snprintf(c->err, sizeof c->err, "kv: backend '%s' does not support %s",
             c->b->name ? c->b->name : "?", op);
    return -1;
}

/* Clear the dispatch-side error before a real backend op so a stale
 * "unsupported" never masks the backend's own last_error. */
static void kv_clear_err(HlKvConn *c) { c->err[0] = '\0'; }

int hl_cap_kv_get(HlKvConn *c, const uint8_t *key, size_t klen,
                  const uint8_t **val, size_t *vlen, int *found) {
    if (!c) return -1;
    kv_clear_err(c);
    return c->b->get(c->h, key, klen, val, vlen, found);
}

int hl_cap_kv_set(HlKvConn *c, const uint8_t *key, size_t klen,
                  const uint8_t *val, size_t vlen, int64_t ttl_ms) {
    if (!c) return -1;
    if (ttl_ms > 0 && !(c->b->caps & HL_KV_CAP_TTL)) return kv_unsupported(c, "ttl");
    kv_clear_err(c);
    return c->b->set(c->h, key, klen, val, vlen, ttl_ms);
}

int hl_cap_kv_del(HlKvConn *c, const uint8_t *key, size_t klen, int *deleted) {
    if (!c) return -1;
    kv_clear_err(c);
    return c->b->del(c->h, key, klen, deleted);
}

int hl_cap_kv_exists(HlKvConn *c, const uint8_t *key, size_t klen, int *present) {
    if (!c) return -1;
    kv_clear_err(c);
    return c->b->exists(c->h, key, klen, present);
}

int hl_cap_kv_incr(HlKvConn *c, const uint8_t *key, size_t klen,
                   int64_t by, int64_t ttl_ms, int64_t *newval) {
    if (!c) return -1;
    if (!c->b->incr) return kv_unsupported(c, "increment");
    kv_clear_err(c);
    return c->b->incr(c->h, key, klen, by, ttl_ms, newval);
}

HlKvCasResult hl_cap_kv_cas(HlKvConn *c, const uint8_t *key, size_t klen,
                            const uint8_t *expected, size_t elen, int has_expected,
                            const uint8_t *newv, size_t nlen, int64_t ttl_ms) {
    if (!c) return HL_KV_CAS_ERROR;
    if (!c->b->cas) { kv_unsupported(c, "compare-and-swap"); return HL_KV_CAS_ERROR; }
    kv_clear_err(c);
    return c->b->cas(c->h, key, klen, expected, elen, has_expected, newv, nlen, ttl_ms);
}

int hl_cap_kv_clear(HlKvConn *c, const uint8_t *prefix, size_t plen, int64_t *removed) {
    if (!c) return -1;
    kv_clear_err(c);
    return c->b->clear(c->h, prefix, plen, removed);
}

int hl_cap_kv_scan(HlKvConn *c, const uint8_t *prefix, size_t plen, size_t limit,
                   HlKvScanCb cb, void *cbctx) {
    if (!c) return -1;
    if (!c->b->scan) return kv_unsupported(c, "scan");
    kv_clear_err(c);
    return c->b->scan(c->h, prefix, plen, limit, cb, cbctx);
}
