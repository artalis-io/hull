/*
 * log_lock.c — process-global lock making vendored log.c thread-safe.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/log_lock.h"
#include "log.h"

#include <pthread.h>
#include <stdbool.h>

/* Static storage with PTHREAD_MUTEX_INITIALIZER: default attributes, no
 * runtime pthread_mutex_init/_destroy, process-lifetime. Same pattern as
 * runtime/cache_common.c's g_singleton_mutex. */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void log_lock_cb(bool lock, void *udata)
{
    (void)udata;
    if (lock)
        pthread_mutex_lock(&g_log_mutex);
    else
        pthread_mutex_unlock(&g_log_mutex);
}

void hl_log_make_threadsafe(void)
{
    /* Idempotent: re-registering the same callback is harmless. log_log
     * wraps its entire body (callback list + stream write) in this
     * lock/unlock pair, so concurrent log calls serialize. */
    log_set_lock(log_lock_cb, NULL);
}
