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
static clockid_t       g_clock = CLOCK_REALTIME;

/* The watch whose instantiation is in progress on this thread, for the WAMR
 * post-instantiate hook. */
static _Thread_local HlWasmWatch *tl_pending;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(g_clock, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint32_t hl_wasm_timeout_resolve(uint64_t ms)
{
    if (ms == 0) ms = HL_WASM_DEFAULT_TIMEOUT_MS;
    if (ms > HL_WASM_MAX_TIMEOUT_MS) ms = HL_WASM_MAX_TIMEOUT_MS;
    return (uint32_t)ms;
}

/* Caller holds g_mu. */
static void terminate_locked(HlWasmWatch *w)
{
    if (w->inst && !w->terminated) {
        wasm_runtime_terminate((wasm_module_inst_t)w->inst);
        w->terminated = 1;
    }
}

static void *watchdog_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        uint64_t now = now_ms();
        uint64_t next = 0;
        for (HlWasmWatch *w = g_head; w; w = w->next) {
            if (w->expired) continue;
            if (w->deadline_ms <= now) {
                w->expired = 1;
                terminate_locked(w);   /* unbound: hl_wasm_watch_bind does it */
            } else if (next == 0 || w->deadline_ms < next) {
                next = w->deadline_ms;
            }
        }
        g_wake_ms = next;
        if (next == 0) {
            pthread_cond_wait(&g_cv, &g_mu);
        } else {
            struct timespec ts;
            ts.tv_sec  = (time_t)(next / 1000u);
            ts.tv_nsec = (long)((next % 1000u) * 1000000u);
            (void)pthread_cond_timedwait(&g_cv, &g_mu, &ts);
        }
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
    /* A monotonic deadline: a wall-clock step must neither fire every armed
     * watch early nor postpone them. macOS has no condattr clock. */
    if (have_attr && pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0)
        g_clock = CLOCK_MONOTONIC;
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
    w->terminated  = 0;
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
     * the instance before it runs at all. */
    if (w->expired) terminate_locked(w);
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
