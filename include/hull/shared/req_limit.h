/*
 * hull/shared/req_limit.h - per-client and per-route request concurrency
 *
 * Audit convergence §6.4. The server admits a request (a pre-body Keel
 * middleware in serve.c) only while its client has fewer than `per_ip`
 * requests in flight, and its route fewer than that route's limit. A request
 * is in flight from admission until Keel reports its response sent (the
 * access-log hook) or the same connection starts its next request. A
 * connection that closes while its response is still being written leaves its
 * entry behind; it is swept when the connection is gone (`alive` callback)
 * the next time the cap would refuse someone, and replaced when the
 * connection slot takes another request.
 *
 * Clients are keyed by address: an IPv4 address as itself (an IPv4-mapped
 * IPv6 address as the IPv4 one), an IPv6 address by its /64 - one host
 * usually holds a whole /64, as the stdlib's per-client limits assume.
 *
 * Header-only, no Keel: serve.c supplies the connection identity and the
 * liveness callback, and the unit tests drive it directly.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_REQ_LIMIT_H
#define HL_SHARED_REQ_LIMIT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <sys/socket.h>   /* AF_INET, AF_INET6 */
#include <arpa/inet.h>    /* inet_pton */

#include "hull/limits/input.h"

/* A client key: 16 bytes, IPv4 as ::ffff:a.b.c.d, IPv6 masked to /64. */
typedef struct HlReqKey {
    uint8_t b[16];
} HlReqKey;

/* 0 + *out from a numeric address ("192.0.2.1", "2001:db8::1",
 * "::ffff:192.0.2.1"); -1 if @p ip is not one. */
static inline int hl_req_key_from_ip(const char *ip, HlReqKey *out)
{
    uint8_t buf[16];
    if (!ip || !out) return -1;
    memset(out, 0, sizeof *out);
    if (inet_pton(AF_INET, ip, buf) == 1) {
        out->b[10] = 0xff; out->b[11] = 0xff;
        memcpy(out->b + 12, buf, 4);
        return 0;
    }
    if (inet_pton(AF_INET6, ip, buf) == 1) {
        static const uint8_t v4mapped[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
        if (memcmp(buf, v4mapped, 12) == 0) {
            memcpy(out->b, buf, 16);          /* the IPv4 address it carries */
        } else {
            memcpy(out->b, buf, 8);           /* the /64 */
        }
        return 0;
    }
    return -1;
}

/* The client address behind a trusted proxy: the LAST entry of the
 * X-Forwarded-For value @p xff (whitespace trimmed), as the stdlib's
 * `_request.client_ip(req, true)` takes it - proxies append the peer they
 * saw, so everything to its left is whatever the client wrote. Copies it into
 * @p out and returns 1 when that entry is a numeric address, else 0. */
static inline int hl_req_xff_last(const char *xff, size_t len, char *out, size_t n)
{
    if (!xff || !out || n < 2) return 0;
    const char *end = xff + len;
    const char *start = xff;
    for (const char *p = xff; p < end; p++)
        if (*p == ',') start = p + 1;
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t l = (size_t)(end - start);
    if (l == 0 || l >= n) return 0;
    memcpy(out, start, l);
    out[l] = '\0';
    HlReqKey k;
    return hl_req_key_from_ip(out, &k) == 0;
}

/* 1 when @p path is served by route @p pattern for the concurrency cap:
 * an exact match (a trailing slash tolerated either way), or a prefix
 * pattern ending in slash-star ("/files/" + anything, and "/files" itself). */
static inline int hl_req_route_match(const char *pattern, const char *path, size_t path_len)
{
    size_t pl = strlen(pattern);
    if (pl >= 2 && pattern[pl - 2] == '/' && pattern[pl - 1] == '*') {
        size_t base = pl - 2;                     /* "/files" */
        if (path_len < base || memcmp(path, pattern, base) != 0) return 0;
        return path_len == base || path[base] == '/';
    }
    while (pl > 1 && pattern[pl - 1] == '/') pl--;
    while (path_len > 1 && path[path_len - 1] == '/') path_len--;
    return pl == path_len && memcmp(pattern, path, pl) == 0;
}

typedef struct HlReqSlot {
    const void *conn;     /* NULL = free */
    HlReqKey    key;
    int         route;    /* index into the route limits, -1 = none */
} HlReqSlot;

typedef int (*HlReqAliveFn)(const void *conn, void *user);

typedef struct HlReqLimiter {
    HlReqSlot  *slots;    /* one per connection the server can hold */
    int         cap;
    uint32_t    per_ip;   /* 0 = no per-client cap */
    const uint32_t *route_max;   /* borrowed, route_count entries */
    int         route_count;
    HlReqAliveFn alive;   /* NULL = every entry counts */
    void       *alive_user;
    uint64_t    refused_ip;      /* counters, for logs and tests */
    uint64_t    refused_route;
} HlReqLimiter;

/* Outcome of hl_req_limit_admit. */
#define HL_REQ_ADMIT          0
#define HL_REQ_REFUSE_CLIENT  1   /* the client is at per_ip: 429 */
#define HL_REQ_REFUSE_ROUTE   2   /* the route is at its limit: 503 */

static inline int hl_req_limit_init(HlReqLimiter *l, int cap, uint32_t per_ip,
                                    const uint32_t *route_max, int route_count)
{
    memset(l, 0, sizeof *l);
    if (cap < 1) cap = 1;
    l->slots = (HlReqSlot *)calloc((size_t)cap, sizeof(HlReqSlot));
    if (!l->slots) return -1;
    l->cap = cap;
    l->per_ip = per_ip;
    l->route_max = route_max;
    l->route_count = route_count;
    return 0;
}

static inline void hl_req_limit_free(HlReqLimiter *l)
{
    if (!l) return;
    free(l->slots);
    l->slots = NULL;
    l->cap = 0;
}

/* The request on @p conn has ended: forget it. */
static inline void hl_req_limit_release(HlReqLimiter *l, const void *conn)
{
    if (!l || !l->slots || !conn) return;
    for (int i = 0; i < l->cap; i++)
        if (l->slots[i].conn == conn) { l->slots[i].conn = NULL; return; }
}

/* Count the live entries for @p key and for @p route; with @p sweep, drop
 * the entries of connections that are gone first. */
static inline void hl_req_limit_count_(HlReqLimiter *l, const HlReqKey *key,
                                       int route, int sweep,
                                       uint32_t *n_key, uint32_t *n_route)
{
    *n_key = 0; *n_route = 0;
    for (int i = 0; i < l->cap; i++) {
        HlReqSlot *s = &l->slots[i];
        if (!s->conn) continue;
        int k = memcmp(&s->key, key, sizeof *key) == 0;
        int r = route >= 0 && s->route == route;
        if (!k && !r) continue;
        if (sweep && l->alive && !l->alive(s->conn, l->alive_user)) {
            s->conn = NULL;
            continue;
        }
        if (k) (*n_key)++;
        if (r) (*n_route)++;
    }
}

/* Admit (record) or refuse a request from @p key for route index @p route
 * (-1 = no route limit) on connection @p conn. A connection holds at most
 * one request at a time, so whatever @p conn had recorded has ended. */
static inline int hl_req_limit_admit(HlReqLimiter *l, const void *conn,
                                     const HlReqKey *key, int route)
{
    if (!l || !l->slots || !conn || !key) return HL_REQ_ADMIT;
    hl_req_limit_release(l, conn);
    if (route >= l->route_count) route = -1;
    uint32_t rmax = route >= 0 ? l->route_max[route] : 0;

    uint32_t nk, nr;
    hl_req_limit_count_(l, key, route, 0, &nk, &nr);
    if ((l->per_ip && nk >= l->per_ip) || (rmax && nr >= rmax))
        hl_req_limit_count_(l, key, route, 1, &nk, &nr);   /* sweep, recount */
    if (l->per_ip && nk >= l->per_ip) { l->refused_ip++; return HL_REQ_REFUSE_CLIENT; }
    if (rmax && nr >= rmax) { l->refused_route++; return HL_REQ_REFUSE_ROUTE; }

    for (int i = 0; i < l->cap; i++) {
        if (!l->slots[i].conn) {
            l->slots[i].conn = conn;
            l->slots[i].key = *key;
            l->slots[i].route = route;
            return HL_REQ_ADMIT;
        }
    }
    /* Every slot taken (more live connections than the table was sized
     * for): admitted, uncounted, rather than refuse a client for it. */
    return HL_REQ_ADMIT;
}

#endif /* HL_SHARED_REQ_LIMIT_H */
