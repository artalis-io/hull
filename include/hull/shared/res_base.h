/*
 * hull/shared/res_base.h - the per-runtime list of "the headers a script entry
 * started with" (HlResBaseList), kept by the response's error answer.
 *
 * Split out of res_headers.h so the runtime state (HlJS / HlLua) can embed the
 * list and the base runtime objects (runtime.c, async.c) can clear it and
 * forget entries without pulling Keel's response header in: nothing here
 * dereferences a KlHttpResponse. Taking a snapshot (hl_res_base_begin) and
 * restoring one (hl_res_base_error_reset) need the response's header buffer
 * and live in res_headers.h, on the HTTP side of the seam.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_RES_BASE_H
#define HL_SHARED_RES_BASE_H

#include <stddef.h>
#include <stdlib.h>   /* free */

struct KlHttpResponse;

/* At most this many entries, and this many header bytes over all of them
 * (audit 12: the count alone allowed 256 x 64 KB). Past either the oldest
 * goes, and that response's error answer drops every header. */
#define HL_RES_BASE_MAX       256
#define HL_RES_BASE_BYTES_MAX (1024 * 1024)

typedef struct HlResBase {
    const struct KlHttpResponse *res;
    const void                  *conn;   /* its connection (NULL: none known) */
    char                        *hdrs;   /* malloc'd copy */
    size_t                       len;
    struct HlResBase            *prev, *next;   /* newest first */
} HlResBase;

typedef struct HlResBaseList {
    HlResBase *head, *tail;
    size_t     count;
    size_t     bytes;   /* sum of every entry's len */
} HlResBaseList;

static inline void hl_res_base_unlink(HlResBaseList *l, HlResBase *b)
{
    if (b->prev) b->prev->next = b->next; else l->head = b->next;
    if (b->next) b->next->prev = b->prev; else l->tail = b->prev;
    l->count--;
    l->bytes -= b->len;
}

static inline void hl_res_base_drop(HlResBaseList *l, HlResBase *b)
{
    hl_res_base_unlink(l, b);
    free(b->hdrs);
    free(b);
}

static inline HlResBase *hl_res_base_find(HlResBaseList *l,
                                          const struct KlHttpResponse *res)
{
    for (HlResBase *b = l ? l->head : NULL; b; b = b->next)
        if (b->res == res) return b;
    return NULL;
}

/* Forget the entry for @p res, if any. */
static inline void hl_res_base_forget(HlResBaseList *l,
                                      const struct KlHttpResponse *res)
{
    HlResBase *b = hl_res_base_find(l, res);
    if (b) hl_res_base_drop(l, b);
}

/* Forget the entry made for a request on @p conn (its connection is gone: an
 * async cancel). NULL: nothing. */
static inline void hl_res_base_forget_conn(HlResBaseList *l, const void *conn)
{
    if (!l || !conn) return;
    for (HlResBase *b = l->head; b; b = b->next) {
        if (b->conn == conn) {
            hl_res_base_drop(l, b);
            return;
        }
    }
}

/* Free every entry (the runtime is going away). */
static inline void hl_res_base_clear(HlResBaseList *l)
{
    while (l && l->head) hl_res_base_drop(l, l->head);
}

#endif /* HL_SHARED_RES_BASE_H */
