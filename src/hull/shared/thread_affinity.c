/*
 * thread_affinity.c - event-loop thread-affinity assertions
 *
 * Only meaningful under HL_THREAD_AFFINITY_CHECKS (debug/sanitizer
 * builds); compiles to an empty TU otherwise (the header provides
 * static-inline no-ops for callers).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/thread_affinity.h"

#ifdef HL_THREAD_AFFINITY_CHECKS

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_t       s_loop_tid;
static atomic_bool     s_loop_set = false;

void hl_event_loop_mark_current(void)
{
    s_loop_tid = pthread_self();
    /* Release so any thread that later observes s_loop_set == true also
     * observes the s_loop_tid write (pairs with the acquire load below).
     * Marking happens before the worker pool is created, so workers also
     * get a happens-before edge via pthread_create. */
    atomic_store_explicit(&s_loop_set, true, memory_order_release);
}

void hl_assert_on_event_loop(const char *what)
{
    if (!atomic_load_explicit(&s_loop_set, memory_order_acquire))
        return; /* no loop captured (boot / tests / tool mode) */
    if (!pthread_equal(pthread_self(), s_loop_tid)) {
        fprintf(stderr,
                "FATAL: %s called off the event-loop thread "
                "(thread-affinity invariant violated)\n",
                what ? what : "(unknown)");
        abort();
    }
}

#endif /* HL_THREAD_AFFINITY_CHECKS */
