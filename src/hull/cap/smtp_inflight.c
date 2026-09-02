/*
 * smtp_inflight.c - per-server registry of in-flight async SMTP ops.
 * See include/hull/cap/smtp_inflight.h and docs/smtp_keel_slice2c_plan.md sec 10.
 *
 * Event-loop-thread only (no lock): submit adds, the resume path removes, the
 * post-pool_free shutdown sweep drains the remainder. Intrusive circular list.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/smtp_inflight.h"

#include <stddef.h>   /* NULL */

void hl_smtp_inflight_init(HlSmtpInflight *r)
{
    r->head.prev = &r->head;
    r->head.next = &r->head;
    r->head.reg  = r;
    r->head.owner = NULL;
    r->head.release = NULL;
    r->head.linked = 0;   /* the sentinel is never "linked" data */
    r->count = 0;
}

void hl_smtp_inflight_add(HlSmtpInflight *r, HlSmtpInflightNode *n,
                          void *owner, HlSmtpInflightRelease release)
{
    n->owner   = owner;
    n->release = release;
    n->reg     = r;
    /* link at head: head <-> n <-> head.next */
    n->prev = &r->head;
    n->next = r->head.next;
    r->head.next->prev = n;
    r->head.next = n;
    n->linked = 1;
    r->count++;
}

void hl_smtp_inflight_remove(HlSmtpInflightNode *n)
{
    if (!n->linked)
        return;
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->linked = 0;
    if (n->reg)
        n->reg->count--;
    n->prev = n->next = NULL;
}

int hl_smtp_inflight_count(const HlSmtpInflight *r)
{
    return r->count;
}

void hl_smtp_inflight_for_each(HlSmtpInflight *r,
                               void (*fn)(void *owner, void *user), void *user)
{
    /* Snapshot next before the callback so a (contract-violating) unlink of the
     * current node cannot derail the walk; the contract is no mutation, this is
     * just defensive. */
    for (HlSmtpInflightNode *n = r->head.next; n != &r->head; ) {
        HlSmtpInflightNode *next = n->next;
        if (fn)
            fn(n->owner, user);
        n = next;
    }
}

int hl_smtp_inflight_sweep(HlSmtpInflight *r)
{
    int swept = 0;
    while (r->head.next != &r->head) {
        HlSmtpInflightNode *n = r->head.next;
        /* Unlink BEFORE release: the release callback may free the storage that
         * embeds this node, so it must already be off the list. */
        HlSmtpInflightRelease release = n->release;
        void *owner = n->owner;
        hl_smtp_inflight_remove(n);
        if (release)
            release(owner);
        swept++;
    }
    return swept;
}
