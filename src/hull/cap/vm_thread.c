/*
 * cap/vm_thread.c - the app VM's own thread (--vm-thread; design §6.3
 * phase 1). See include/hull/cap/vm_thread.h for the model.
 *
 * Locking: t->mu guards the job hand-off (fn / ud / has_job / done / stop /
 * txn_gen). The loop thread holds the baton from hl_vmt_run's entry until it
 * posts the job, the VM thread from taking the job until it marks it done;
 * the mutex is the happens-before edge between them in both directions.
 * Standby touches no shared state but t->mu, the run watch (through its own
 * lock), the stats (atomics), the captured request socket and the listener.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/vm_thread.h"
#include "hull/cap/db_backend.h"   /* hl_db_txn_gen / hl_db_txn_gen_set */
#include "hull/shared/thread_affinity.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* How often the loop thread re-reads the run's deadline while it waits. */
#define VMT_WAIT_SLICE_MS     100u
/* Standby: how long one poll of the listener waits before the job state and
 * the ceiling are looked at again. */
#define VMT_STANDBY_SLICE_MS   25
/* Standby: connections answered per poll wake-up (the rest wait for the next
 * slice, so the done check is never starved). */
#define VMT_STANDBY_BATCH      16
/* Standby: per-connection socket timeout for reading the request head and
 * writing the answer. */
#define VMT_CONN_IO_MS        100

struct HlVmThread {
    pthread_t        tid;
    int              started;
    pthread_mutex_t  mu;
    pthread_cond_t   cv_job;
    pthread_cond_t   cv_done;
#if !defined(__APPLE__)
    int              cv_monotonic;
#endif
    /* The job (under mu). */
    void           (*fn)(void *);
    void            *ud;
    int              has_job;
    int              done;
    int              stop;
    uint64_t         txn_gen;      /* handed across with the baton */

    HlVmThreadConfig cfg;

    /* Stats (atomic). */
    uint64_t         n_jobs;
    uint64_t         n_abandoned;
    uint64_t         n_503;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t hl_vmt_effective_deadline(int armed_this_job,
                                   uint64_t watch_deadline_ms,
                                   uint64_t job_start_ms,
                                   uint32_t fallback_ms)
{
    if (armed_this_job)
        return watch_deadline_ms;            /* 0: the run armed no deadline */
    if (fallback_ms == 0) return 0;
    return job_start_ms + fallback_ms;
}

/* Wait on @p cv (t->mu held) for at most @p ms. */
static void timed_wait(HlVmThread *t, pthread_cond_t *cv, uint32_t ms)
{
#if defined(__APPLE__)
    struct timespec rel = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    (void)pthread_cond_timedwait_relative_np(cv, &t->mu, &rel);
#else
    struct timespec abs;
    clock_gettime(t->cv_monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &abs);
    abs.tv_sec  += (time_t)(ms / 1000u);
    abs.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    (void)pthread_cond_timedwait(cv, &t->mu, &abs);
#endif
}

static void set_txn_gen(uint64_t g)
{
#ifdef HL_ENABLE_DB
    hl_db_txn_gen_set(g);
#else
    (void)g;
#endif
}

static void *vm_thread_main(void *arg)
{
    HlVmThread *t = (HlVmThread *)arg;
    /* The asynchronous signals are blocked on this thread, so the SIGTERM /
     * SIGINT Keel handles go to the loop thread and never interrupt a system
     * call of a run. The synchronous ones (SIGBUS for the mmap guard,
     * SIGSEGV, ...) stay deliverable. Blocked here rather than inherited
     * from a mask the creating thread changes around pthread_create, which
     * leaves the loop thread's own mask untouched. */
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    sigaddset(&block, SIGQUIT);
    sigaddset(&block, SIGPIPE);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGUSR2);
    sigaddset(&block, SIGCHLD);
    sigaddset(&block, SIGALRM);
    (void)pthread_sigmask(SIG_BLOCK, &block, NULL);
    pthread_mutex_lock(&t->mu);
    for (;;) {
        while (!t->has_job && !t->stop)
            pthread_cond_wait(&t->cv_job, &t->mu);
        if (!t->has_job) break;              /* stop, nothing pending */
        void (*fn)(void *) = t->fn;
        void *ud = t->ud;
        uint64_t gen = t->txn_gen;
        t->has_job = 0;
        pthread_mutex_unlock(&t->mu);

        /* The baton is ours: the per-thread state that outlives a run, then
         * the runtime's own (QuickJS's stack top). */
        set_txn_gen(gen);
        if (t->cfg.thread_switch) t->cfg.thread_switch(t->cfg.switch_ud);
        hl_event_loop_baton_enter();
        fn(ud);
        hl_event_loop_baton_leave();
        gen = hl_db_txn_gen();

        pthread_mutex_lock(&t->mu);
        t->txn_gen = gen;
        t->done = 1;
        pthread_cond_signal(&t->cv_done);
    }
    pthread_mutex_unlock(&t->mu);
    return NULL;
}

int hl_vmt_create(HlVmThread **out, const HlVmThreadConfig *cfg)
{
    if (!out || !cfg || !cfg->watch) return -1;
    *out = NULL;
    HlVmThread *t = calloc(1, sizeof *t);
    if (!t) return -1;
    t->cfg = *cfg;
    t->txn_gen = hl_db_txn_gen();   /* destroy hands it back unchanged */
    if (t->cfg.grace_ms == 0) t->cfg.grace_ms = HL_VMT_DEFAULT_GRACE_MS;
    if (t->cfg.grace_ms > HL_VMT_MAX_MS) t->cfg.grace_ms = HL_VMT_MAX_MS;
    if (t->cfg.ceiling_ms > HL_VMT_MAX_MS) t->cfg.ceiling_ms = HL_VMT_MAX_MS;

    if (pthread_mutex_init(&t->mu, NULL) != 0) { free(t); return -1; }
    pthread_condattr_t ca;
    int have_attr = pthread_condattr_init(&ca) == 0;
#if !defined(__APPLE__)
    if (have_attr && pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0)
        t->cv_monotonic = 1;
#endif
    int rc1 = pthread_cond_init(&t->cv_job, have_attr ? &ca : NULL);
    int rc2 = pthread_cond_init(&t->cv_done, have_attr ? &ca : NULL);
    if (have_attr) pthread_condattr_destroy(&ca);
    if (rc1 != 0 || rc2 != 0) {
        if (rc1 == 0) pthread_cond_destroy(&t->cv_job);
        if (rc2 == 0) pthread_cond_destroy(&t->cv_done);
        pthread_mutex_destroy(&t->mu);
        free(t);
        return -1;
    }

    pthread_attr_t ta;
    int rc = pthread_attr_init(&ta);
    if (rc == 0) {
        (void)pthread_attr_setstacksize(&ta, HL_VMT_STACK_BYTES);
        rc = pthread_create(&t->tid, &ta, vm_thread_main, t);
        pthread_attr_destroy(&ta);
    }
    if (rc != 0) {
        log_error("[hull:vm-thread] the VM thread could not be started (%d)", rc);
        pthread_cond_destroy(&t->cv_job);
        pthread_cond_destroy(&t->cv_done);
        pthread_mutex_destroy(&t->mu);
        free(t);
        return -1;
    }
    t->started = 1;
    *out = t;
    return 0;
}

void hl_vmt_destroy(HlVmThread *t)
{
    if (!t) return;
    if (t->started) {
        pthread_mutex_lock(&t->mu);
        t->stop = 1;
        pthread_cond_signal(&t->cv_job);
        pthread_mutex_unlock(&t->mu);
        pthread_join(t->tid, NULL);
        /* The baton comes home for good: teardown runs VM code here. */
        set_txn_gen(t->txn_gen);
        if (t->cfg.thread_switch) t->cfg.thread_switch(t->cfg.switch_ud);
    }
    pthread_cond_destroy(&t->cv_job);
    pthread_cond_destroy(&t->cv_done);
    pthread_mutex_destroy(&t->mu);
    free(t);
}

int hl_vmt_on_vm_thread(const HlVmThread *t)
{
    return t && t->started && pthread_equal(pthread_self(), t->tid);
}

void hl_vmt_stats(const HlVmThread *t, HlVmtStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!t) return;
    out->jobs        = __atomic_load_n(&t->n_jobs, __ATOMIC_RELAXED);
    out->abandoned   = __atomic_load_n(&t->n_abandoned, __ATOMIC_RELAXED);
    out->standby_503 = __atomic_load_n(&t->n_503, __ATOMIC_RELAXED);
}

/* ── Standby ────────────────────────────────────────────────────────── */

static const char k_503[] =
    "HTTP/1.1 503 Service Unavailable\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 20\r\n"
    "Retry-After: 1\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Service Unavailable\n";

static void send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void set_io_timeouts(int fd, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* Answer one connection standby accepted: read its request head (so closing
 * does not reset the connection before the client reads the answer), write
 * the 503, close. Bounded by VMT_CONN_IO_MS per step. */
static void answer_503(int c)
{
    int fl = fcntl(c, F_GETFL);
    if (fl >= 0) (void)fcntl(c, F_SETFL, fl & ~O_NONBLOCK);
    (void)fcntl(c, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int on = 1;
    (void)setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    set_io_timeouts(c, VMT_CONN_IO_MS);
    char buf[4096];
    size_t got = 0;
    for (int i = 0; i < 4 && got < sizeof buf - 1; i++) {
        ssize_t r = recv(c, buf + got, sizeof buf - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        buf[got] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    send_all(c, k_503, sizeof k_503 - 1);
    (void)shutdown(c, SHUT_WR);
    for (int i = 0; i < 2; i++) {             /* let the FIN cross */
        if (recv(c, buf, sizeof buf, 0) <= 0) break;
    }
    close(c);
}

static void accept_batch(HlVmThread *t, int lfd)
{
    for (int i = 0; i < VMT_STANDBY_BATCH; i++) {
        /* Keel's listener is non-blocking, but standby never relies on it:
         * accept only what poll says is there. */
        if (i > 0) {
            struct pollfd p = { lfd, POLLIN, 0 };
            if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
        }
        int c = accept(lfd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            return;                          /* EAGAIN, or the listener is gone */
        }
        answer_503(c);
        __atomic_fetch_add(&t->n_503, 1, __ATOMIC_RELAXED);
    }
}

static int job_done(HlVmThread *t)
{
    pthread_mutex_lock(&t->mu);
    int d = t->done;
    pthread_mutex_unlock(&t->mu);
    return d;
}

/* Keel was asked to stop (SIGTERM / SIGINT): draining, or stopped. */
static int server_stopping(const HlVmThread *t)
{
    if (t->cfg.server_draining &&
        __atomic_load_n(t->cfg.server_draining, __ATOMIC_RELAXED) != 0)
        return 1;
    return t->cfg.server_running &&
           __atomic_load_n(t->cfg.server_running, __ATOMIC_RELAXED) == 0;
}

static void die(int status, const char *why, uint64_t stuck_ms)
{
    log_fatal("[hull:vm-thread] %s: the VM thread has been stuck in one "
              "native call for %llu ms; exiting without teardown (status %d) "
              "so a supervisor restarts the process",
              why, (unsigned long long)stuck_ms, status);
    fflush(stderr);
    fflush(stdout);
    _exit(status);
}

/* The run outlived its deadline + grace. Answer what can be answered without
 * touching anything the VM thread may be using, until the job is done (or the
 * ceiling ends the process). */
static void standby(HlVmThread *t, const HlVmtHttp *http, int *early_sent,
                    uint64_t deadline)
{
    uint64_t start = now_ms();
    __atomic_fetch_add(&t->n_abandoned, 1, __ATOMIC_RELAXED);
    log_warn("[hull:vm-thread] run abandoned: still running %llu ms past its "
             "deadline, inside a native call that does not stop; answering "
             "503 until it returns",
             (unsigned long long)(start > deadline ? start - deadline : 0));

    if (http && http->early_ok && http->fd >= 0) {
        send_all(http->fd, k_503, sizeof k_503 - 1);
        (void)shutdown(http->fd, SHUT_WR);
        if (early_sent) *early_sent = 1;
    }

    /* Read on this (the loop) thread, which is the one Keel binds it on. */
    int lfd = t->cfg.listen_fd ? (int)*t->cfg.listen_fd : -1;
    uint64_t stop_seen = 0;
    while (!job_done(t)) {
        uint64_t now = now_ms();
        if (t->cfg.ceiling_ms && now - start >= t->cfg.ceiling_ms)
            die(HL_VMT_EXIT_STUCK, "hard ceiling reached", now - start);
        if (server_stopping(t)) {
            if (!stop_seen) stop_seen = now;
            else if (now - stop_seen >= t->cfg.drain_ms)
                die(1, "stop requested and the drain timeout passed",
                    now - start);
        }
        if (lfd >= 0) {
            struct pollfd p = { lfd, POLLIN, 0 };
            int r = poll(&p, 1, VMT_STANDBY_SLICE_MS);
            if (r > 0 && (p.revents & POLLIN))
                accept_batch(t, lfd);
        } else {
            struct timespec ts = { 0, VMT_STANDBY_SLICE_MS * 1000000L };
            (void)nanosleep(&ts, NULL);
        }
    }
    log_warn("[hull:vm-thread] the abandoned run returned after %llu ms in "
             "standby; serving again",
             (unsigned long long)(now_ms() - start));
}

/* ── Running an entry ───────────────────────────────────────────────── */

int hl_vmt_run(HlVmThread *t, void (*fn)(void *), void *ud,
               const HlVmtHttp *http, int *early_sent)
{
    if (early_sent) *early_sent = 0;
    if (!t || !t->started || hl_vmt_on_vm_thread(t)) {
        fn(ud);
        return HL_VMT_DONE;
    }

    uint64_t seq0 = 0;
    hl_run_watch_peek(t->cfg.watch, NULL, &seq0);
    uint64_t start = now_ms();
    /* The deadline the run's own arm will set: an HTTP entry's limit when
     * this is one (audit convergence section 6.2), else every other run's. */
    uint32_t fallback = hl_run_watchdog_limit_ms(http ? HL_RUN_HTTP
                                                      : HL_RUN_ENTRY);

    pthread_mutex_lock(&t->mu);
    t->fn = fn;
    t->ud = ud;
    t->txn_gen = hl_db_txn_gen();
    t->done = 0;
    t->has_job = 1;
    pthread_cond_signal(&t->cv_job);
    __atomic_fetch_add(&t->n_jobs, 1, __ATOMIC_RELAXED);

    int abandoned = 0;
    while (!t->done) {
        timed_wait(t, &t->cv_done, VMT_WAIT_SLICE_MS);
        if (t->done) break;
        pthread_mutex_unlock(&t->mu);
        uint64_t dl = 0, seq = 0;
        hl_run_watch_peek(t->cfg.watch, &dl, &seq);
        uint64_t eff = hl_vmt_effective_deadline(seq != seq0, dl, start, fallback);
        if (eff != 0 && now_ms() >= eff + t->cfg.grace_ms && !job_done(t)) {
            abandoned = 1;
            standby(t, http, early_sent, eff);
        }
        pthread_mutex_lock(&t->mu);
    }
    uint64_t gen = t->txn_gen;
    t->done = 0;
    t->fn = NULL;
    t->ud = NULL;
    pthread_mutex_unlock(&t->mu);

    /* The baton is back: this thread's per-thread state, then the
     * runtime's. */
    set_txn_gen(gen);
    if (t->cfg.thread_switch) t->cfg.thread_switch(t->cfg.switch_ud);
    return abandoned ? HL_VMT_ABANDONED : HL_VMT_DONE;
}

/* ── Test hook ──────────────────────────────────────────────────────── */

int hl_vmt_test_hook_enabled(void)
{
    const char *e = getenv("HULL_TEST_NATIVE_SLEEP");
    return e && strcmp(e, "1") == 0;
}

void hl_vmt_test_native_sleep(uint32_t ms)
{
    if (ms > HL_VMT_TEST_SLEEP_MAX_MS) ms = HL_VMT_TEST_SLEEP_MAX_MS;
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}
