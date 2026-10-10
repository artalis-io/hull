/*
 * cap/run_watchdog.c - a wall-clock bound for every script run (audit 12)
 *
 * One process-wide thread holds the pending deadlines and raises the stop
 * flag of each one that passes. See cap/run_watchdog.h.
 *
 * Locking: g_mu guards the pending list and every HlRunWatch field but
 * `stop`, which is written under g_mu (atomically) and read by the VM's own
 * thread without it. A watch that fires is unlinked, so the list holds only
 * pending deadlines and the thread sleeps while every VM is idle. Disarm
 * takes g_mu, so once it returns the thread cannot be writing to the watch.
 *
 * Time: CLOCK_MONOTONIC everywhere, waited on the way cap/wasm_watchdog.c
 * does (a monotonic condvar where the platform has one, a relative wait on
 * Apple, short relative sleeps otherwise).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/run_watchdog.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv;
static HlRunWatch     *g_head;          /* pending watches */
static uint64_t        g_wake_ms;       /* when the thread next wakes (0 = idle) */
static int             g_started;
static int             g_start_failed;
#if !defined(__APPLE__)
static int             g_cv_monotonic;
#endif

/* The configured limit (-1 = the defaults); atomic, read by every arm on
 * any thread. Until a runner configures it, HULL_MAX_RUN_MS is read once
 * (so hull test / hull agent and every other entry honour it too). */
#define LIMIT_UNSET (-2)
static int64_t        g_limit_ms = LIMIT_UNSET;
static pthread_once_t g_env_once = PTHREAD_ONCE_INIT;

#define WATCHDOG_SLICE_MS 20u

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

void hl_run_watchdog_configure(int64_t ms)
{
    if (ms > (int64_t)HL_RUN_MAX_MS) ms = (int64_t)HL_RUN_MAX_MS;
    __atomic_store_n(&g_limit_ms, ms < 0 ? -1 : ms, __ATOMIC_RELAXED);
}

static void read_env_once(void)
{
    const char *e = getenv("HULL_MAX_RUN_MS");
    int64_t v = -1;
    if (e && *e && hl_run_watchdog_parse_ms(e, &v) != 0) {
        log_warn("[hull] HULL_MAX_RUN_MS='%s' is not a number of "
                 "milliseconds; using the defaults", e);
        v = -1;
    }
    int64_t expect = LIMIT_UNSET;
    /* A runner's --max-run-ms, configured first, wins. */
    (void)__atomic_compare_exchange_n(&g_limit_ms, &expect, v, 0,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

uint32_t hl_run_watchdog_limit_ms(HlRunKind kind)
{
    int64_t ms = __atomic_load_n(&g_limit_ms, __ATOMIC_RELAXED);
    if (ms == LIMIT_UNSET) {
        pthread_once(&g_env_once, read_env_once);
        ms = __atomic_load_n(&g_limit_ms, __ATOMIC_RELAXED);
    }
    if (ms < 0)
        return kind == HL_RUN_MAIN ? HL_RUN_MAIN_DEFAULT_MS : HL_RUN_DEFAULT_MS;
    return (uint32_t)ms;
}

int hl_run_watchdog_parse_ms(const char *s, int64_t *out)
{
    if (!s || !out || *s < '0' || *s > '9') return -1;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v < 0) return -1;
    *out = v > (long long)HL_RUN_MAX_MS ? (int64_t)HL_RUN_MAX_MS : (int64_t)v;
    return 0;
}

/* Wait on g_cv (g_mu held) until monotonic @p next, a signal, or a spurious
 * wake-up; @p next == 0 waits for a signal only. */
static void wait_locked(uint64_t next)
{
    if (next == 0) {
        pthread_cond_wait(&g_cv, &g_mu);
        return;
    }
    uint64_t now = now_ms();
    if (next <= now) return;
#if defined(__APPLE__)
    struct timespec rel = ms_to_ts(next - now);
    (void)pthread_cond_timedwait_relative_np(&g_cv, &g_mu, &rel);
#else
    if (g_cv_monotonic) {
        struct timespec abs = ms_to_ts(next);
        (void)pthread_cond_timedwait(&g_cv, &g_mu, &abs);
        return;
    }
    uint64_t rel_ms = next - now;
    if (rel_ms > WATCHDOG_SLICE_MS) rel_ms = WATCHDOG_SLICE_MS;
    struct timespec rel = ms_to_ts(rel_ms);
    pthread_mutex_unlock(&g_mu);
    while (nanosleep(&rel, &rel) != 0 && errno == EINTR) {}
    pthread_mutex_lock(&g_mu);
#endif
}

/* Caller holds g_mu. */
static void unlink_locked(HlRunWatch *w)
{
    if (!w->linked) return;
    if (w->prev) w->prev->next = w->next;
    else         g_head = w->next;
    if (w->next) w->next->prev = w->prev;
    w->next = w->prev = NULL;
    w->linked = 0;
}

/* Caller holds g_mu; w is armed and not linked. */
static void link_locked(HlRunWatch *w)
{
    w->prev = NULL;
    w->next = g_head;
    if (g_head) g_head->prev = w;
    g_head = w;
    w->linked = 1;
    if (g_wake_ms == 0 || w->deadline_ms < g_wake_ms)
        pthread_cond_signal(&g_cv);
}

static void *watchdog_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        uint64_t now = now_ms();
        uint64_t next = 0;
        HlRunWatch *w = g_head;
        while (w) {
            HlRunWatch *nx = w->next;
            if (w->deadline_ms <= now) {
                __atomic_store_n(&w->stop, 1, __ATOMIC_RELAXED);
                unlink_locked(w);           /* fired: no longer pending */
            } else if (next == 0 || w->deadline_ms < next) {
                next = w->deadline_ms;
            }
            w = nx;
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
    if (have_attr && pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0)
        g_cv_monotonic = 1;
#endif
    int rc = pthread_cond_init(&g_cv, have_attr ? &ca : NULL);
    if (have_attr) pthread_condattr_destroy(&ca);
    if (rc != 0) {
        g_start_failed = 1;
        log_error("[hull] run watchdog: condition variable failed (%d); "
                  "runs have no wall-clock limit", rc);
        return -1;
    }

    pthread_t th;
    pthread_attr_t ta;
    if (pthread_attr_init(&ta) != 0) {
        pthread_cond_destroy(&g_cv);
        g_start_failed = 1;
        log_error("[hull] run watchdog could not be started; runs have no "
                  "wall-clock limit");
        return -1;
    }
    pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&th, &ta, watchdog_main, NULL);
    pthread_attr_destroy(&ta);
    if (rc != 0) {
        pthread_cond_destroy(&g_cv);
        g_start_failed = 1;
        log_error("[hull] run watchdog thread could not be started (%d); "
                  "runs have no wall-clock limit", rc);
        return -1;
    }
    g_started = 1;
    return 0;
}

void hl_run_watch_arm(HlRunWatch *w, uint32_t ms)
{
    if (!w) return;
    pthread_mutex_lock(&g_mu);
    __atomic_store_n(&w->stop, 0, __ATOMIC_RELAXED);
    unlink_locked(w);
    w->armed = 0;
    if (ms > 0 && ensure_started_locked() == 0) {
        if (ms > HL_RUN_MAX_MS) ms = HL_RUN_MAX_MS;
        w->deadline_ms = now_ms() + ms;
        w->armed = 1;
        link_locked(w);
    }
    pthread_mutex_unlock(&g_mu);
}

void hl_run_watch_disarm(HlRunWatch *w)
{
    if (!w) return;
    pthread_mutex_lock(&g_mu);
    unlink_locked(w);
    w->armed = 0;
    __atomic_store_n(&w->stop, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_mu);
}

uint64_t hl_run_watch_save(HlRunWatch *w)
{
    if (!w) return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t d = w->armed ? w->deadline_ms : 0;
    pthread_mutex_unlock(&g_mu);
    return d;
}

void hl_run_watch_restore(HlRunWatch *w, uint64_t deadline_ms)
{
    if (!w) return;
    pthread_mutex_lock(&g_mu);
    unlink_locked(w);
    w->armed = 0;
    __atomic_store_n(&w->stop, 0, __ATOMIC_RELAXED);
    if (deadline_ms != 0) {
        w->deadline_ms = deadline_ms;
        w->armed = 1;
        if (deadline_ms <= now_ms())
            __atomic_store_n(&w->stop, 1, __ATOMIC_RELAXED);
        else if (ensure_started_locked() == 0)
            link_locked(w);
    }
    pthread_mutex_unlock(&g_mu);
}
