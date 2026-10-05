/*
 * cap/wasm_watchdog.c - wall-clock bound for WASM compute (audit 5 M2)
 *
 * One process-wide thread holds every armed deadline and terminates the
 * instance of each one that passes. See cap/wasm_watchdog.h.
 *
 * Locking: g_mu guards the armed list and every HlWasmWatch field. The
 * terminate itself runs under g_mu, and disarm takes g_mu, so once disarm
 * returns the thread can no longer be touching that instance - the caller may
 * then pool, reuse or destroy it.
 *
 * A passed deadline is not a one-shot: while the instance stays bound (guest
 * code still running on it), the thread terminates it again every
 * HL_WASM_WATCHDOG_REASSERT_MS. wasm_runtime_terminate only writes the
 * instance's exception, and a host call the guest makes can overwrite that
 * with its own and clear it (round-6 M1), so one terminate could be lost.
 *
 * Time: deadlines are CLOCK_MONOTONIC everywhere, so a wall-clock step neither
 * postpones nor fires them. The wait follows the clock: an absolute monotonic
 * wait where the condvar takes that clock, a relative wait on Apple (no
 * condattr clock there), and otherwise short relative sleeps.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm_watchdog.h"
#include "hull/limits/wasm.h"
#include "log.h"
#include "wasm_export.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv;
static HlWasmWatch    *g_head;          /* armed watches */
static uint64_t        g_wake_ms;       /* when the thread next wakes (0 = idle) */
static int             g_started;       /* thread running (under g_mu) */
static int             g_start_failed;
#if !defined(__APPLE__)
static int             g_cv_monotonic;  /* g_cv's timed wait uses CLOCK_MONOTONIC */
#endif

/* Longest sleep, when the condvar cannot wait on the monotonic clock: a new
 * earlier deadline is then seen within this. */
#define WATCHDOG_SLICE_MS 20u

/* The watch whose instantiation is in progress on this thread, for the WAMR
 * post-instantiate hook. */
static _Thread_local HlWasmWatch *tl_pending;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static struct timespec ms_to_ts(uint64_t ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    return ts;
}

/* Wait on g_cv (g_mu held) until monotonic time @p next, a signal, or a
 * spurious wake-up; @p next == 0 waits for a signal only. */
static void wait_locked(uint64_t next)
{
    if (next == 0) {
        pthread_cond_wait(&g_cv, &g_mu);
        return;
    }
    uint64_t now = now_ms();
    if (next <= now) return;
#if defined(__APPLE__)
    /* No condattr clock on macOS: a RELATIVE wait is immune to a wall-clock
     * step, where an absolute CLOCK_REALTIME one is postponed by it. */
    struct timespec rel = ms_to_ts(next - now);
    (void)pthread_cond_timedwait_relative_np(&g_cv, &g_mu, &rel);
#else
    if (g_cv_monotonic) {
        struct timespec abs = ms_to_ts(next);
        (void)pthread_cond_timedwait(&g_cv, &g_mu, &abs);
        return;
    }
    /* A realtime condvar would carry a clock step into the deadline: sleep in
     * short relative slices instead (an arm's signal is then not awaited, but
     * the next slice sees the new deadline). */
    uint64_t rel_ms = next - now;
    if (rel_ms > WATCHDOG_SLICE_MS) rel_ms = WATCHDOG_SLICE_MS;
    struct timespec rel = ms_to_ts(rel_ms);
    pthread_mutex_unlock(&g_mu);
    while (nanosleep(&rel, &rel) != 0 && errno == EINTR) {}
    pthread_mutex_lock(&g_mu);
#endif
}

uint32_t hl_wasm_timeout_resolve(uint64_t ms)
{
    if (ms == 0) ms = HL_WASM_DEFAULT_TIMEOUT_MS;
    if (ms > HL_WASM_MAX_TIMEOUT_MS) ms = HL_WASM_MAX_TIMEOUT_MS;
    return (uint32_t)ms;
}

/* Caller holds g_mu. Terminates the bound instance, if any; harmless when it
 * already carries the trap. */
static void terminate_locked(HlWasmWatch *w)
{
    if (w->inst)
        wasm_runtime_terminate((wasm_module_inst_t)w->inst);
}

static void *watchdog_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        uint64_t now = now_ms();
        uint64_t next = 0;
        int running_expired = 0;
        for (HlWasmWatch *w = g_head; w; w = w->next) {
            if (!w->expired) {
                if (w->deadline_ms > now) {
                    if (next == 0 || w->deadline_ms < next)
                        next = w->deadline_ms;
                    continue;
                }
                w->expired = 1;
            }
            /* Expired and still bound: guest code is running on it, so
             * terminate it (again - the last one may have been erased).
             * Unbound, hl_wasm_watch_bind terminates it when bound. */
            if (w->inst) {
                terminate_locked(w);
                running_expired = 1;
            }
        }
        if (running_expired) {
            uint64_t again = now + HL_WASM_WATCHDOG_REASSERT_MS;
            if (next == 0 || again < next) next = again;
        }
        g_wake_ms = next;
        wait_locked(next);
    }
    return NULL;
}

/* Caller holds g_mu. */
static int ensure_started_locked(void)
{
    if (g_started) return 0;
    if (g_start_failed) return -1;

    pthread_condattr_t ca;
    int have_attr = pthread_condattr_init(&ca) == 0;
#if !defined(__APPLE__)
    /* Deadlines are monotonic; let the timed wait take the same clock where
     * the platform allows it (wait_locked falls back otherwise). */
    if (have_attr && pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0)
        g_cv_monotonic = 1;
#endif
    int rc = pthread_cond_init(&g_cv, have_attr ? &ca : NULL);
    if (have_attr) pthread_condattr_destroy(&ca);
    if (rc != 0) {
        g_start_failed = 1;
        return -1;
    }

    pthread_t th;
    pthread_attr_t ta;
    if (pthread_attr_init(&ta) != 0) {
        pthread_cond_destroy(&g_cv);
        g_start_failed = 1;
        return -1;
    }
    pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&th, &ta, watchdog_main, NULL);
    pthread_attr_destroy(&ta);
    if (rc != 0) {
        pthread_cond_destroy(&g_cv);
        g_start_failed = 1;
        log_error("[wasm] watchdog thread could not be started (%d); "
                  "compute calls are refused", rc);
        return -1;
    }
    g_started = 1;
    return 0;
}

int hl_wasm_watch_arm(HlWasmWatch *w, uint32_t timeout_ms, void *inst)
{
    if (!w) return -1;
    pthread_mutex_lock(&g_mu);
    if (ensure_started_locked() != 0) {
        pthread_mutex_unlock(&g_mu);
        w->armed = 0;
        return -1;
    }
    w->inst        = inst;
    w->deadline_ms = now_ms() + hl_wasm_timeout_resolve(timeout_ms);
    w->armed       = 1;
    w->expired     = 0;
    w->prev        = NULL;
    w->next        = g_head;
    if (g_head) g_head->prev = w;
    g_head = w;
    /* Wake the thread only when this deadline comes before its next wake-up;
     * otherwise it will see this one when it wakes anyway. */
    if (g_wake_ms == 0 || w->deadline_ms < g_wake_ms)
        pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return 0;
}

void hl_wasm_watch_bind(HlWasmWatch *w, void *inst)
{
    if (!w || !w->armed) return;
    pthread_mutex_lock(&g_mu);
    w->inst = inst;
    /* Expired while unbound (e.g. between instantiation and the call): stop
     * the instance before it runs at all, and wake the thread so it keeps
     * re-asserting the stop while the instance stays bound. */
    if (w->expired && inst) {
        terminate_locked(w);
        pthread_cond_signal(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
}

int hl_wasm_watch_disarm(HlWasmWatch *w)
{
    if (!w || !w->armed) return 0;
    pthread_mutex_lock(&g_mu);
    if (w->prev) w->prev->next = w->next;
    else         g_head = w->next;
    if (w->next) w->next->prev = w->prev;
    w->next = w->prev = NULL;
    w->armed = 0;
    w->inst  = NULL;
    /* The deadline may have passed without the thread having woken yet. */
    int expired = w->expired || now_ms() >= w->deadline_ms;
    pthread_mutex_unlock(&g_mu);
    return expired;
}

/* WAMR patch 0007 hook: bind the calling thread's pending watch to the new
 * instance while its start / ctor functions run. */
static void post_instantiate_hook(wasm_module_inst_t inst, bool entering)
{
    HlWasmWatch *w = tl_pending;
    if (w) hl_wasm_watch_bind(w, entering ? (void *)inst : NULL);
}

void hl_wasm_watchdog_install(void)
{
    wasm_runtime_set_post_instantiate_hook(post_instantiate_hook);
}

void *hl_wasm_watch_instantiate(HlWasmWatch *w, void *module,
                                uint32_t stack_size, uint32_t heap_size,
                                char *error_buf, uint32_t error_buf_size)
{
    HlWasmWatch *saved = tl_pending;
    tl_pending = w;
    wasm_module_inst_t inst = wasm_runtime_instantiate(
        (wasm_module_t)module, stack_size, heap_size, error_buf, error_buf_size);
    tl_pending = saved;
    return inst;
}

#endif /* HL_ENABLE_WASM */
