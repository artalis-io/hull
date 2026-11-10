/**
 * @file main_signal.c
 * @brief app.main observes SIGINT / SIGTERM - see shared/main_signal.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/shared/main_signal.h"
#include "hull/shared/async_backend.h"
#include "hull/runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

/* ── Handler state (process-wide: signal dispositions are) ──────────── */

/* The handler may run on any thread, so its counter is a lock-free atomic
 * (async-signal-safe), not a read-modify-write of a sig_atomic_t that two
 * threads could interleave, losing the second signal's escalation. */
#if ATOMIC_INT_LOCK_FREE != 2
#error "main_signal needs a lock-free atomic int (async-signal-safe)"
#endif

/* Written by the handler (any thread), read on the loop thread. */
static volatile sig_atomic_t g_sig;        /* the latest signal, 0 = none */
static atomic_int            g_count;      /* how many arrived */
static volatile sig_atomic_t g_delivered;  /* the first one reached the app */
/* 1 between install and remove: the handler records and wakes the loop.
 * 0 otherwise: a handler still reachable (left current, or in the chain of a
 * later installer such as the TUI, audit 44) only passes the signal on. */
static volatile sig_atomic_t g_active;

/* The self-pipe, made once and kept for the process lifetime (audit 44): a
 * handler on another thread that loaded the write end just before a remove
 * closed it would write into whatever reused the number. A stale byte is
 * drained at the next install. */
static int                   g_pipe_r = -1;
static volatile sig_atomic_t g_pipe_w = -1;

/* Per signal: whether on_signal is (or may still be, inside someone else's
 * chain) the handler for it, and what it chains to. */
enum { MS_INT, MS_TERM, MS_N };
static const int             g_signo[MS_N] = { SIGINT, SIGTERM };
static struct sigaction      g_prev[MS_N];
static volatile sig_atomic_t g_hooked[MS_N];
static int                   g_installed;

/* Loop-thread state. */
static HlAsyncBackendCtx *g_loop;
static HlRuntime         *g_rt;
static int                g_watching;
static int                g_grace_ms;
static uint64_t           g_grace_timer;
static int                g_ended;

static int slot_of(int sig) { return sig == SIGTERM ? MS_TERM : MS_INT; }

/* Pass @p sig on to the disposition saved for it. A plain sa_handler is
 * called (the TUI's, which restores the terminal); SIG_IGN does nothing;
 * SIG_DFL takes the default action (terminate) only when @p dfl_action is
 * set - the pass-through of an uninstalled handler, which must act as the
 * original disposition would have. An SA_SIGINFO handler takes arguments we
 * do not have and is not called. */
static void chain_prev(const struct sigaction *prev, int sig, int dfl_action)
{
    if (prev->sa_flags & SA_SIGINFO) return;
    if (prev->sa_handler == SIG_IGN) return;
    if (prev->sa_handler == SIG_DFL) {
        if (!dfl_action) return;
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(sig, &dfl, NULL);
        raise(sig);
        return;
    }
    prev->sa_handler(sig);
}

static void on_signal(int sig)
{
    int saved = errno;
    const struct sigaction *prev = &g_prev[slot_of(sig)];
    if (!g_active) {
        /* Uninstalled, but still reachable: act as what we replaced. */
        chain_prev(prev, sig, 1);
        errno = saved;
        return;
    }
    g_sig = sig;
    int n = atomic_fetch_add_explicit(&g_count, 1, memory_order_relaxed) + 1;
    if (n >= 2 && !g_delivered) {
        /* Twice before the loop ever got to deliver the first: main is in a
         * synchronous stretch and nothing we schedule will run. Die as
         * before this module existed. */
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(sig, &dfl, NULL);
        raise(sig);
        errno = saved;
        return;
    }
    int w = g_pipe_w;
    if (w >= 0) {
        char b = (char)sig;
        ssize_t r = write(w, &b, 1);   /* EAGAIN: a wake is already pending */
        (void)r;
    }
    chain_prev(prev, sig, 0);
    errno = saved;
}

/* ── Loop-thread side ───────────────────────────────────────────────── */

static void stop_loop(int sig)
{
    g_ended = sig;
    if (g_grace_timer && g_loop) {
        hl_async_backend()->timer_cancel(g_loop, g_grace_timer);
        g_grace_timer = 0;
    }
    if (g_loop) hl_async_backend()->stop(g_loop);
}

static void on_grace(void *user)
{
    (void)user;
    g_grace_timer = 0;
    int sig = g_sig ? (int)g_sig : SIGINT;
    log_warn("[hull:main] %s: main did not finish within %d ms; exiting",
             hl_main_signal_name(sig), g_grace_ms);
    stop_loop(sig);
}

static void on_pipe_readable(int fd, unsigned ready, void *user)
{
    (void)ready; (void)user;
    char buf[64];
    while (read(fd, buf, sizeof buf) > 0) { }
    if (g_ended || !g_active) return;

    int sig = (int)g_sig;
    if (!sig) return;

    if (!g_delivered) {
        g_delivered = 1;
        int handled = 0;
        if (g_rt && g_rt->vt && g_rt->vt->main_interrupt)
            handled = g_rt->vt->main_interrupt(g_rt, sig);
        /* Main may have settled inside the delivery (its continuation ran
         * in the drain and stopped the loop): the grace timer armed below is
         * then cancelled by hl_main_signal_remove, and main's own result
         * stands - g_ended stays 0. */
        if (!handled || g_grace_ms <= 0) {
            if (!handled)
                log_debug("[hull:main] %s: no handler registered; exiting",
                          hl_main_signal_name(sig));
            stop_loop(sig);
            return;
        }
        g_grace_timer = hl_async_backend()->timer_add(g_loop, (uint64_t)g_grace_ms,
                                                       on_grace, NULL);
        if (!g_grace_timer) {
            log_error("[hull:main] cannot arm the interrupt grace timer; exiting");
            stop_loop(sig);
        }
        /* A second signal that arrived while the handler ran. */
        if (atomic_load(&g_count) >= 2) stop_loop(sig);
        return;
    }
    /* Delivered already: a second signal during the grace. */
    stop_loop(sig);
}

static int set_cloexec_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    int fd_fl = fcntl(fd, F_GETFD);
    if (fd_fl < 0 || fcntl(fd, F_SETFD, fd_fl | FD_CLOEXEC) < 0) return -1;
    return 0;
}

/* Make the process-lifetime self-pipe on first use. */
static int pipe_ensure(void)
{
    if (g_pipe_r >= 0) return 0;
    int p[2];
    if (pipe(p) != 0) {
        log_error("[hull:main] interrupt pipe: %s", strerror(errno));
        return -1;
    }
    if (set_cloexec_nonblock(p[0]) != 0 || set_cloexec_nonblock(p[1]) != 0) {
        log_error("[hull:main] interrupt pipe flags: %s", strerror(errno));
        close(p[0]);
        close(p[1]);
        return -1;
    }
    g_pipe_r = p[0];
    g_pipe_w = p[1];
    return 0;
}

static void pipe_drain(void)
{
    char buf[64];
    if (g_pipe_r >= 0)
        while (read(g_pipe_r, buf, sizeof buf) > 0) { }
}

/* Hook signal @p i unless the process ignores it. 0 on success (hooked, or
 * deliberately left alone), -1 when sigaction failed. */
static int hook_one(int i, const struct sigaction *sa)
{
    if (g_hooked[i]) {
        /* Still ours from an earlier install: current again (a later
         * installer gave it back) or inside that installer's chain, which
         * reaches it. Re-hooking would save a handler that chains to us as
         * our own previous one - a loop. g_prev[i] is still the original. */
        return 0;
    }
    struct sigaction cur;
    if (sigaction(g_signo[i], NULL, &cur) != 0) return -1;
    /* An inherited SIG_IGN is the process's choice (a background job of a
     * shell without job control, a supervisor that ignores SIGINT): keep it
     * (audit 44). The signal then never reaches app.main. */
    if (!(cur.sa_flags & SA_SIGINFO) && cur.sa_handler == SIG_IGN) return 0;
    if (sigaction(g_signo[i], sa, &g_prev[i]) != 0) return -1;
    g_hooked[i] = 1;
    return 0;
}

/* Put back what we replaced - only while our handler is still the current
 * one (audit 44: restoring by value clobbered a handler installed after
 * ours, the TUI's, whose release then reinstalled ours for good). Otherwise
 * leave it: on_signal stays a pass-through to g_prev. */
static void unhook_one(int i)
{
    if (!g_hooked[i]) return;
    struct sigaction cur;
    if (sigaction(g_signo[i], NULL, &cur) != 0) return;
    if (!(cur.sa_flags & SA_SIGINFO) && cur.sa_handler == on_signal &&
        sigaction(g_signo[i], &g_prev[i], NULL) == 0)
        g_hooked[i] = 0;
}

int hl_main_signal_install(HlAsyncBackendCtx *loop, HlRuntime *rt, int grace_ms)
{
    if (!loop || !rt) return -1;
    if (g_installed) hl_main_signal_remove();

    if (pipe_ensure() != 0) return -1;
    pipe_drain();
    if (hl_async_backend()->watcher_add(loop, g_pipe_r, HL_ASYNC_READ,
                                        on_pipe_readable, NULL) != 0) {
        log_error("[hull:main] cannot watch the interrupt pipe");
        return -1;
    }

    g_loop        = loop;
    g_rt          = rt;
    g_watching    = 1;
    g_grace_ms    = grace_ms;
    g_grace_timer = 0;
    g_ended       = 0;
    g_sig         = 0;
    atomic_store(&g_count, 0);
    g_delivered   = 0;
    g_active      = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* Restart interrupted syscalls: the loop is woken through the pipe, not
     * by EINTR, and a blocking ctx.stdin read or a sliced send must not fail
     * because the operator pressed Ctrl-C once. */
    sa.sa_flags = SA_RESTART;
    if (hook_one(MS_INT, &sa) != 0 || hook_one(MS_TERM, &sa) != 0) {
        log_error("[hull:main] sigaction: %s", strerror(errno));
        g_active = 0;
        unhook_one(MS_INT);
        unhook_one(MS_TERM);
        hl_async_backend()->watcher_del(loop, g_pipe_r);
        g_watching = 0;
        g_loop = NULL;
        g_rt = NULL;
        return -1;
    }
    g_installed = 1;
    return 0;
}

void hl_main_signal_remove(void)
{
    if (!g_installed) return;
    g_installed = 0;
    /* Stop recording first; a handler still reachable passes signals on. */
    g_active = 0;
    unhook_one(MS_INT);
    unhook_one(MS_TERM);
    if (g_loop) {
        if (g_grace_timer) hl_async_backend()->timer_cancel(g_loop, g_grace_timer);
        if (g_watching) hl_async_backend()->watcher_del(g_loop, g_pipe_r);
    }
    g_watching = 0;
    g_grace_timer = 0;
    /* The pipe stays open for the process lifetime (see g_pipe_w). */
    pipe_drain();
    g_loop = NULL;
    g_rt = NULL;
}

int hl_main_signal_received(void)
{
    return (int)g_sig;
}

int hl_main_signal_ended(void)
{
    return g_ended;
}

const char *hl_main_signal_name(int sig)
{
    switch (sig) {
    case SIGINT:  return "SIGINT";
    case SIGTERM: return "SIGTERM";
    default:      return "SIGNAL";
    }
}

int hl_main_signal_parse_ms(const char *s, int *out)
{
    if (!s || !*s || !out) return -1;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < 0) return -1;
    if (v > 24LL * 60 * 60 * 1000) v = 24LL * 60 * 60 * 1000;
    *out = (int)v;
    return 0;
}
