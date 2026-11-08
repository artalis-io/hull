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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

/* ── Handler state (process-wide: signal dispositions are) ──────────── */

/* Written by the handler (any thread), read on the loop thread. */
static volatile sig_atomic_t g_sig;        /* the latest signal, 0 = none */
static volatile sig_atomic_t g_count;      /* how many arrived */
static volatile sig_atomic_t g_delivered;  /* the first one reached the app */
static volatile sig_atomic_t g_pipe_w = -1;

static struct sigaction g_prev_int, g_prev_term;
static int              g_installed;

/* Loop-thread state. */
static HlAsyncBackendCtx *g_loop;
static HlRuntime         *g_rt;
static int                g_pipe_r = -1;
static int                g_grace_ms;
static uint64_t           g_grace_timer;
static int                g_ended;

static void chain_prev(const struct sigaction *prev, int sig)
{
    /* Only a plain sa_handler can be chained (the TUI's is one); an
     * SA_SIGINFO handler takes arguments we do not have. */
    if (prev->sa_flags & SA_SIGINFO) return;
    if (prev->sa_handler == SIG_DFL || prev->sa_handler == SIG_IGN) return;
    prev->sa_handler(sig);
}

static void on_signal(int sig)
{
    int saved = errno;
    g_sig = sig;
    g_count = g_count + 1;
    if (g_count >= 2 && !g_delivered) {
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
        ssize_t n = write(w, &b, 1);   /* EAGAIN: a wake is already pending */
        (void)n;
    }
    chain_prev(sig == SIGINT ? &g_prev_int : &g_prev_term, sig);
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
    if (g_ended) return;

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
        if (g_count >= 2) stop_loop(sig);
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

int hl_main_signal_install(HlAsyncBackendCtx *loop, HlRuntime *rt, int grace_ms)
{
    if (!loop || !rt) return -1;
    if (g_installed) hl_main_signal_remove();

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
    if (hl_async_backend()->watcher_add(loop, p[0], HL_ASYNC_READ,
                                        on_pipe_readable, NULL) != 0) {
        log_error("[hull:main] cannot watch the interrupt pipe");
        close(p[0]);
        close(p[1]);
        return -1;
    }

    g_loop        = loop;
    g_rt          = rt;
    g_pipe_r      = p[0];
    g_grace_ms    = grace_ms;
    g_grace_timer = 0;
    g_ended       = 0;
    g_sig         = 0;
    g_count       = 0;
    g_delivered   = 0;
    g_pipe_w      = p[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* Restart interrupted syscalls: the loop is woken through the pipe, not
     * by EINTR, and a blocking ctx.stdin read or a sliced send must not fail
     * because the operator pressed Ctrl-C once. */
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, &g_prev_int) != 0 ||
        sigaction(SIGTERM, &sa, &g_prev_term) != 0) {
        log_error("[hull:main] sigaction: %s", strerror(errno));
        sigaction(SIGINT, &g_prev_int, NULL);
        g_pipe_w = -1;
        hl_async_backend()->watcher_del(loop, p[0]);
        close(p[0]);
        close(p[1]);
        g_loop = NULL;
        g_rt = NULL;
        g_pipe_r = -1;
        return -1;
    }
    g_installed = 1;
    return 0;
}

void hl_main_signal_remove(void)
{
    if (!g_installed) return;
    g_installed = 0;
    /* The handler first, so it writes to no closed descriptor. */
    sigaction(SIGINT, &g_prev_int, NULL);
    sigaction(SIGTERM, &g_prev_term, NULL);
    int w = g_pipe_w;
    g_pipe_w = -1;
    if (g_loop) {
        if (g_grace_timer) hl_async_backend()->timer_cancel(g_loop, g_grace_timer);
        if (g_pipe_r >= 0) hl_async_backend()->watcher_del(g_loop, g_pipe_r);
    }
    g_grace_timer = 0;
    if (g_pipe_r >= 0) close(g_pipe_r);
    if (w >= 0) close(w);
    g_pipe_r = -1;
    g_loop = NULL;
    g_rt = NULL;
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
