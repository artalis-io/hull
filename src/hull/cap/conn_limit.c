/*
 * cap/conn_limit.c - per-client open connections and connection deadlines
 * (audit convergence §6.4). See hull/cap/conn_limit.h.
 *
 * Two tables under one mutex: an entry per descriptor (grown on demand,
 * indexed by fd) holding the connection's phase and deadline start, and an
 * open-addressing count per client key (linear probing, backward-shift
 * deletion, sized for twice the connection limit so it never fills in
 * practice; a full table admits uncounted rather than refuse a client for
 * it). Out of memory admits untracked, for the same reason.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/conn_limit.h"

#include <stdlib.h>
#include <string.h>

static uint32_t key_hash(const HlReqKey *k)
{
    uint32_t h = 2166136261u;                       /* FNV-1a */
    for (size_t i = 0; i < sizeof k->b; i++) { h ^= k->b[i]; h *= 16777619u; }
    return h;
}

/* The bucket holding @p k, or the empty one where it would go; NULL when the
 * table is full and @p k is not in it. */
static HlConnBucket *bucket_find(HlConnLimit *l, const HlReqKey *k)
{
    uint32_t mask = l->nbuckets - 1;
    uint32_t i = key_hash(k) & mask;
    for (uint32_t n = 0; n < l->nbuckets; n++, i = (i + 1) & mask) {
        HlConnBucket *b = &l->buckets[i];
        if (b->count == 0 || memcmp(&b->key, k, sizeof *k) == 0) return b;
    }
    return NULL;
}

/* One fewer connection under @p k; an emptied bucket is deleted by shifting
 * the rest of its probe run back (no tombstones). */
static void bucket_dec(HlConnLimit *l, const HlReqKey *k)
{
    HlConnBucket *b = bucket_find(l, k);
    if (!b || b->count == 0) return;
    if (--b->count > 0) return;
    uint32_t mask = l->nbuckets - 1;
    uint32_t hole = (uint32_t)(b - l->buckets);
    uint32_t j = hole;
    for (;;) {
        j = (j + 1) & mask;
        HlConnBucket *c = &l->buckets[j];
        if (c->count == 0) break;
        uint32_t home = key_hash(&c->key) & mask;
        /* c may move into the hole when its home is not in (hole, j]. */
        int move = hole <= j ? (home <= hole || home > j)
                             : (home <= hole && home > j);
        if (move) {
            l->buckets[hole] = *c;
            c->count = 0;
            hole = j;
        }
    }
}

int hl_conn_limit_init(HlConnLimit *l, int max_connections, uint32_t per_ip,
                       uint32_t header_ms, uint32_t idle_ms)
{
    if (!l) return -1;
    memset(l, 0, sizeof *l);
    if (max_connections < 1) max_connections = 1;
    uint32_t want = (uint32_t)max_connections * 2u + 16u;
    uint32_t n = 64;
    while (n < want && n < (1u << 30)) n <<= 1;
    l->buckets = (HlConnBucket *)calloc(n, sizeof(HlConnBucket));
    if (!l->buckets) return -1;
    l->nbuckets = n;
    if (pthread_mutex_init(&l->mu, NULL) != 0) {
        free(l->buckets);
        l->buckets = NULL;
        return -1;
    }
    l->mu_ok = 1;
    l->per_ip = per_ip;
    l->header_ms = header_ms;
    l->idle_ms = idle_ms;
    return 0;
}

void hl_conn_limit_free(HlConnLimit *l)
{
    if (!l) return;
    free(l->by_fd);
    free(l->buckets);
    if (l->mu_ok) pthread_mutex_destroy(&l->mu);
    memset(l, 0, sizeof *l);
}

/* The entry for @p fd, growing the table to hold it; NULL when it cannot. */
static HlConnEntry *entry_get(HlConnLimit *l, int fd, int grow)
{
    if (fd < 0 || fd >= HL_CONN_FD_MAX) return NULL;
    if (fd < l->fd_cap) return &l->by_fd[fd];
    if (!grow) return NULL;
    int cap = l->fd_cap ? l->fd_cap : 64;
    while (cap <= fd) cap *= 2;
    if (cap > HL_CONN_FD_MAX) cap = HL_CONN_FD_MAX;
    HlConnEntry *p = (HlConnEntry *)realloc(l->by_fd, (size_t)cap * sizeof *p);
    if (!p) return NULL;
    memset(p + l->fd_cap, 0, (size_t)(cap - l->fd_cap) * sizeof *p);
    l->by_fd = p;
    l->fd_cap = cap;
    return &l->by_fd[fd];
}

static void entry_drop(HlConnLimit *l, HlConnEntry *e)
{
    if (e->phase != HL_CONN_NONE && e->counted) bucket_dec(l, &e->key);
    memset(e, 0, sizeof *e);
}

int hl_conn_limit_open(HlConnLimit *l, int fd, const HlReqKey *key, uint64_t now_ms)
{
    if (!l || !l->mu_ok) return HL_CONN_ADMIT;
    pthread_mutex_lock(&l->mu);
    HlConnEntry *e = entry_get(l, fd, 1);
    if (!e) {                                   /* untracked: admitted */
        pthread_mutex_unlock(&l->mu);
        return HL_CONN_ADMIT;
    }
    entry_drop(l, e);                           /* a close we never saw */
    int counted = 0;
    if (key && l->per_ip) {
        HlConnBucket *b = bucket_find(l, key);
        if (b && b->count >= l->per_ip) {
            l->refused++;
            pthread_mutex_unlock(&l->mu);
            return HL_CONN_REFUSE;
        }
        if (b) {
            if (b->count == 0) b->key = *key;
            b->count++;
            counted = 1;
        }
    }
    e->phase = HL_CONN_HEADERS;
    e->counted = (uint8_t)counted;
    if (key) e->key = *key;
    e->since_ms = now_ms;
    pthread_mutex_unlock(&l->mu);
    return HL_CONN_ADMIT;
}

void hl_conn_limit_close(HlConnLimit *l, int fd)
{
    if (!l || !l->mu_ok) return;
    pthread_mutex_lock(&l->mu);
    HlConnEntry *e = entry_get(l, fd, 0);
    if (e) entry_drop(l, e);
    pthread_mutex_unlock(&l->mu);
}

void hl_conn_limit_bytes_in(HlConnLimit *l, int fd, uint64_t now_ms)
{
    if (!l || !l->mu_ok) return;
    pthread_mutex_lock(&l->mu);
    HlConnEntry *e = entry_get(l, fd, 0);
    if (e && e->phase == HL_CONN_IDLE) {
        e->phase = HL_CONN_HEADERS;
        e->since_ms = now_ms;
    }
    pthread_mutex_unlock(&l->mu);
}

void hl_conn_limit_request(HlConnLimit *l, int fd)
{
    if (!l || !l->mu_ok) return;
    pthread_mutex_lock(&l->mu);
    HlConnEntry *e = entry_get(l, fd, 0);
    if (e && (e->phase == HL_CONN_HEADERS || e->phase == HL_CONN_IDLE))
        e->phase = HL_CONN_BUSY;
    pthread_mutex_unlock(&l->mu);
}

void hl_conn_limit_response_done(HlConnLimit *l, int fd, int status, uint64_t now_ms)
{
    if (!l || !l->mu_ok) return;
    pthread_mutex_lock(&l->mu);
    HlConnEntry *e = entry_get(l, fd, 0);
    if (e && e->phase != HL_CONN_NONE && e->phase != HL_CONN_EXPIRED &&
        e->phase != HL_CONN_WS) {
        if (status == 101) {
            e->phase = HL_CONN_WS;
        } else {
            e->phase = HL_CONN_IDLE;
            e->since_ms = now_ms;
        }
    }
    pthread_mutex_unlock(&l->mu);
}

int hl_conn_limit_sweep(HlConnLimit *l, uint64_t now_ms, HlConnExpireFn fn, void *user)
{
    if (!l || !l->mu_ok) return 0;
    enum { BATCH = 64 };
    int fds[BATCH], whys[BATCH];
    int total = 0, start = 0;
    for (;;) {
        int n = 0, next = -1;
        pthread_mutex_lock(&l->mu);
        for (int fd = start; fd < l->fd_cap; fd++) {
            HlConnEntry *e = &l->by_fd[fd];
            int why = 0;
            if (e->phase == HL_CONN_HEADERS && l->header_ms &&
                now_ms - e->since_ms >= l->header_ms)
                why = HL_CONN_EXPIRE_HEADERS;
            else if (e->phase == HL_CONN_IDLE && l->idle_ms &&
                     now_ms - e->since_ms >= l->idle_ms)
                why = HL_CONN_EXPIRE_IDLE;
            if (!why) continue;
            if (n == BATCH) { next = fd; break; }
            e->phase = HL_CONN_EXPIRED;
            if (why == HL_CONN_EXPIRE_HEADERS) l->expired_headers++;
            else l->expired_idle++;
            fds[n] = fd;
            whys[n] = why;
            n++;
        }
        pthread_mutex_unlock(&l->mu);
        for (int i = 0; i < n; i++)
            if (fn) fn(fds[i], whys[i], user);
        total += n;
        if (next < 0) break;
        start = next;
    }
    return total;
}

uint32_t hl_conn_limit_count(HlConnLimit *l, const HlReqKey *key)
{
    if (!l || !l->mu_ok || !key) return 0;
    pthread_mutex_lock(&l->mu);
    HlConnBucket *b = bucket_find(l, key);
    uint32_t c = b ? b->count : 0;
    pthread_mutex_unlock(&l->mu);
    return c;
}

int hl_conn_limit_phase(HlConnLimit *l, int fd)
{
    if (!l || !l->mu_ok) return HL_CONN_NONE;
    pthread_mutex_lock(&l->mu);
    HlConnEntry *e = entry_get(l, fd, 0);
    int p = e ? e->phase : HL_CONN_NONE;
    pthread_mutex_unlock(&l->mu);
    return p;
}

const char *hl_conn_peer_ip(const char *ip, char *buf, size_t n)
{
    static const uint8_t v4mapped[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
    uint8_t a[16];
    if (!ip || !buf || n < 16) return ip;
    if (inet_pton(AF_INET6, ip, a) != 1 || memcmp(a, v4mapped, 12) != 0)
        return ip;
    if (!inet_ntop(AF_INET, a + 12, buf, (socklen_t)n)) return ip;
    return buf;
}
