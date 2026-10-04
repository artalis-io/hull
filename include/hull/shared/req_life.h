/*
 * shared/req_life.h - is the request a script object points into still live?
 *
 * A `res` object and an SSE stream object hold raw pointers into one request:
 * its KlHttpResponse, its connection's write path. A script can keep either
 * past the request - stash a stream for fan-out, keep `res` in a closure a
 * timer runs - and once the request is over those pointers name a response
 * that was sent, or a connection that was closed and freed.
 *
 * Each such object takes a reference to one HlReqLife for its request. The
 * runtime ends the life when the request's script work is over (the handler
 * returned or failed, its async continuation completed or was cancelled
 * because the client went away), and every later method call on the object
 * finds it dead and fails closed instead of touching the request.
 *
 * Single-threaded: lives are created, ended and released on the event-loop
 * thread only. Header-only so both runtimes share it without build plumbing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_REQ_LIFE_H
#define HL_SHARED_REQ_LIFE_H

#include <stdlib.h>

typedef struct HlReqLife {
    int live;   /* 1 until hl_req_life_end */
    int refs;   /* the request's own ref + one per object holding it */
    /* What currently holds the request's connection (the JS runtime keeps
     * these; 0 elsewhere). An async op in attached mode suspends the
     * connection until it completes - counted until its resume is over; a
     * multipart read parked for more body sets it reading. The two cannot
     * share a connection: each overwrites the state the other relies on. */
    int attached;
    int parked;
} HlReqLife;

/* A new live record, holding the request's reference. NULL on OOM. */
static inline HlReqLife *hl_req_life_new(void)
{
    HlReqLife *l = (HlReqLife *)malloc(sizeof *l);
    if (!l) return NULL;
    l->live = 1;
    l->refs = 1;
    l->attached = 0;
    l->parked = 0;
    return l;
}

static inline void hl_req_life_retain(HlReqLife *l)
{
    if (l) l->refs++;
}

static inline void hl_req_life_release(HlReqLife *l)
{
    if (l && --l->refs == 0) free(l);
}

/* Every holder now sees the request dead. Idempotent, keeps references:
 * any of several holders may learn first that the request is over. */
static inline void hl_req_life_kill(HlReqLife *l)
{
    if (l) l->live = 0;
}

/* The request's script work is over: kill, and drop the request's own
 * reference. Call exactly once per hl_req_life_new. */
static inline void hl_req_life_end(HlReqLife *l)
{
    hl_req_life_kill(l);
    hl_req_life_release(l);
}

/* NULL (no life tracked) counts as live, for objects made outside a request
 * (the in-process test harness). */
static inline int hl_req_life_live(const HlReqLife *l)
{
    return !l || l->live;
}

#endif /* HL_SHARED_REQ_LIFE_H */
