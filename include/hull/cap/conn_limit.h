/*
 * hull/cap/conn_limit.h - per-client open connections and connection deadlines
 *
 * Audit convergence §6.4 (confirming audit, M1). The request limiter
 * (hull/shared/req_limit.h) counts admitted requests only: it runs after the
 * headers arrive. Idle keep-alive connections, connections still sending
 * headers and upgraded WebSockets counted for nothing, and Keel's own limit
 * (max_connections) is global, so one client that opened that many
 * connections - each idle after one request, or each dripping its headers a
 * byte at a time inside the read timeout - refused every other client.
 *
 * This table bounds that, at accept time, from serve.c's socket provider:
 *
 *   - at most `per_ip` connections open per client key (IPv4 as itself, an
 *     IPv4-mapped IPv6 address as the IPv4, IPv6 by /64 - the request
 *     limiter's key). A connection over the cap is closed as soon as it is
 *     accepted. A connection is open from accept until its socket is closed,
 *     an upgraded WebSocket included. Peers in limits.trusted_proxies are
 *     tracked but not counted (open with a NULL key).
 *   - a header deadline: a connection must deliver a complete request head
 *     within `header_ms` of its first byte (of its accept, for the first
 *     request - a TLS handshake included). Keel's read timeout restarts on
 *     every byte, so it never stopped a client that keeps dripping.
 *   - an idle keep-alive deadline: a connection that finished a response and
 *     sent nothing more is closed after `idle_ms`.
 *
 * While a request is being handled (headers complete until its response is
 * sent), and once the connection is a WebSocket, neither deadline applies:
 * Keel's body timeout, the run deadlines and the WebSocket's own lifetime
 * govern it.
 *
 * Phases are driven by serve.c: open / close / bytes_in from the socket
 * provider, request from the first pre-body middleware, response_done from
 * the access-log hook. Expiry is reported by sweep(); the caller shuts the
 * socket down, Keel reads the end of stream and closes it through the
 * provider (close()).
 *
 * Thread-safe (one mutex): the provider's calls come from the event loop,
 * but nothing here assumes so.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_CONN_LIMIT_H
#define HL_CAP_CONN_LIMIT_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "hull/shared/req_limit.h"   /* HlReqKey */

/* Connection phases (HlConnEntry.phase). */
#define HL_CONN_NONE     0   /* not tracked */
#define HL_CONN_HEADERS  1   /* waiting for / reading a request head: header deadline */
#define HL_CONN_BUSY     2   /* a request is being handled: no deadline here */
#define HL_CONN_IDLE     3   /* response sent, nothing since: idle deadline */
#define HL_CONN_WS       4   /* upgraded (101): no deadline here */
#define HL_CONN_EXPIRED  5   /* reported by sweep, waiting for its close */

/* Why sweep() reports a connection. */
#define HL_CONN_EXPIRE_HEADERS 1
#define HL_CONN_EXPIRE_IDLE    2

/* Outcome of hl_conn_limit_open. */
#define HL_CONN_ADMIT   0
#define HL_CONN_REFUSE  1

/* Descriptors above this are admitted untracked (the table is by fd). */
#define HL_CONN_FD_MAX  (1 << 20)

typedef struct HlConnEntry {
    uint8_t   phase;
    uint8_t   counted;     /* holds a count under `key` */
    HlReqKey  key;
    uint64_t  since_ms;    /* start of the current deadline */
} HlConnEntry;

typedef struct HlConnBucket {
    HlReqKey  key;
    uint32_t  count;       /* 0 = empty */
} HlConnBucket;

typedef struct HlConnLimit {
    pthread_mutex_t mu;
    int           mu_ok;
    HlConnEntry  *by_fd;
    int           fd_cap;
    HlConnBucket *buckets;     /* open addressing, linear probing */
    uint32_t      nbuckets;    /* power of two */
    uint32_t      per_ip;      /* 0 = no per-client cap */
    uint32_t      header_ms;   /* 0 = no header deadline */
    uint32_t      idle_ms;     /* 0 = no idle deadline */
    uint64_t      refused;     /* counters, for logs and tests */
    uint64_t      expired_headers;
    uint64_t      expired_idle;
} HlConnLimit;

/* Size for @p max_connections open connections. 0 / -1 (out of memory). */
int  hl_conn_limit_init(HlConnLimit *l, int max_connections, uint32_t per_ip,
                        uint32_t header_ms, uint32_t idle_ms);
void hl_conn_limit_free(HlConnLimit *l);

/* A connection was accepted on @p fd at @p now_ms. @p key NULL = not counted
 * (a trusted proxy, a peer with no IP address), but still under the
 * deadlines. HL_CONN_REFUSE: the client already holds per_ip connections;
 * the caller closes @p fd without calling close(). A stale entry left on
 * @p fd is dropped first. */
int  hl_conn_limit_open(HlConnLimit *l, int fd, const HlReqKey *key, uint64_t now_ms);
/* The socket on @p fd is being closed (no-op for an untracked fd). */
void hl_conn_limit_close(HlConnLimit *l, int fd);
/* Bytes arrived on @p fd: an idle connection starts a request head. */
void hl_conn_limit_bytes_in(HlConnLimit *l, int fd, uint64_t now_ms);
/* A complete request head arrived on @p fd. */
void hl_conn_limit_request(HlConnLimit *l, int fd);
/* The response on @p fd was sent (@p status 101 = upgraded). */
void hl_conn_limit_response_done(HlConnLimit *l, int fd, int status, uint64_t now_ms);

/* Report every connection past its deadline once (fd, HL_CONN_EXPIRE_*),
 * outside the lock; returns how many. */
typedef void (*HlConnExpireFn)(int fd, int why, void *user);
int  hl_conn_limit_sweep(HlConnLimit *l, uint64_t now_ms, HlConnExpireFn fn, void *user);

/* Open, counted connections under @p key. */
uint32_t hl_conn_limit_count(HlConnLimit *l, const HlReqKey *key);
/* The phase of @p fd (HL_CONN_NONE if untracked). */
int  hl_conn_limit_phase(HlConnLimit *l, int fd);

#endif /* HL_CAP_CONN_LIMIT_H */
