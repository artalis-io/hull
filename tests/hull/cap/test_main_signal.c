/*
 * test_main_signal.c - shared/main_signal.c (app.main observes SIGINT /
 * SIGTERM, platform gap 7): how it installs and removes its handler next to
 * what the process already had, and the self-pipe it writes from the handler.
 *
 * The event loop is the build's backend, never run: the module only adds /
 * drops a watcher and a timer on it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/shared/main_signal.h"
#include "hull/shared/async_backend.h"
#include "hull/runtime.h"

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

/* ── The event loop ──────────────────────────────────────────────── */

/* The build's own backend; the module only adds / drops a watcher and a
 * timer on it, and nothing runs the loop. */
static HlAsyncBackendCtx *g_loop_ctx;

static HlAsyncBackendCtx *fake_loop(void)
{
    if (!g_loop_ctx && hl_async_backend()->init(&g_loop_ctx, NULL) != 0)
        g_loop_ctx = NULL;
    return g_loop_ctx;
}

/* ── Handlers standing in for the process's own ───────────────────── */

static volatile sig_atomic_t g_prev_hits;
static void prev_handler(int sig) { (void)sig; g_prev_hits = g_prev_hits + 1; }

/* A later installer that chains, as the TUI does (cap/tui.c on_fatal_signal). */
static struct sigaction g_later_saved;
static volatile sig_atomic_t g_later_hits;
static void later_handler(int sig)
{
    g_later_hits = g_later_hits + 1;
    if (!(g_later_saved.sa_flags & SA_SIGINFO) &&
        g_later_saved.sa_handler != SIG_DFL && g_later_saved.sa_handler != SIG_IGN)
        g_later_saved.sa_handler(sig);
}

static void set_handler(int sig, void (*fn)(int), struct sigaction *old)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, old);
}

static void (*current_handler(int sig))(int)
{
    struct sigaction cur;
    sigaction(sig, NULL, &cur);
    return cur.sa_handler;
}

static int fd_open(int fd) { return fcntl(fd, F_GETFD) != -1; }

/* ── Audit 44 ─────────────────────────────────────────────────────── */

/* An inherited SIG_IGN (a background job of a shell without job control, a
 * supervisor that ignores SIGINT) is the process's choice: main_signal took
 * it over, so a Ctrl-C meant for the foreground job reached app.main. */
UTEST(main_signal_audit44, ignored_sigint_stays_ignored)
{
    HlRuntime rt;
    memset(&rt, 0, sizeof rt);
    struct sigaction saved_int, saved_term;
    set_handler(SIGINT, SIG_IGN, &saved_int);
    set_handler(SIGTERM, prev_handler, &saved_term);

    ASSERT_EQ(hl_main_signal_install(fake_loop(), &rt, 1000), 0);
    EXPECT_TRUE(current_handler(SIGINT) == SIG_IGN);
    /* SIGTERM was not ignored: main_signal handles it. */
    EXPECT_TRUE(current_handler(SIGTERM) != prev_handler);
    EXPECT_TRUE(current_handler(SIGTERM) != SIG_IGN);
    hl_main_signal_remove();
    EXPECT_TRUE(current_handler(SIGINT) == SIG_IGN);
    EXPECT_TRUE(current_handler(SIGTERM) == prev_handler);

    sigaction(SIGINT, &saved_int, NULL);
    sigaction(SIGTERM, &saved_term, NULL);
}

/* The TUI installs its handler after main_signal's and may still hold it when
 * main returns. Removing main_signal restored ITS saved handler by value,
 * clobbering the TUI's; the TUI's later release then put back main_signal's
 * handler for good. Remove now restores only when its handler is the current
 * one, and a handler left in someone's chain passes the signal on. */
UTEST(main_signal_audit44, remove_under_tui_keeps_chain)
{
    HlRuntime rt;
    memset(&rt, 0, sizeof rt);
    struct sigaction saved_int, saved_term;
    set_handler(SIGINT, prev_handler, &saved_int);
    set_handler(SIGTERM, prev_handler, &saved_term);
    g_prev_hits = 0;
    g_later_hits = 0;

    ASSERT_EQ(hl_main_signal_install(fake_loop(), &rt, 1000), 0);
    void (*ours)(int) = current_handler(SIGINT);
    ASSERT_TRUE(ours != prev_handler);

    /* A later installer (the TUI's acquire). */
    set_handler(SIGINT, later_handler, &g_later_saved);
    ASSERT_TRUE(g_later_saved.sa_handler == ours);

    hl_main_signal_remove();
    ASSERT_TRUE(current_handler(SIGINT) == later_handler);
    /* SIGTERM was ours alone: restored. */
    EXPECT_TRUE(current_handler(SIGTERM) == prev_handler);

    /* The chain still reaches the process's own handler. */
    raise(SIGINT);
    EXPECT_EQ((int)g_later_hits, 1);
    EXPECT_EQ((int)g_prev_hits, 1);

    /* The TUI's release puts back what it saved: our handler, now a
     * pass-through to the original. */
    sigaction(SIGINT, &g_later_saved, NULL);
    EXPECT_TRUE(current_handler(SIGINT) == ours);
    raise(SIGINT);
    raise(SIGINT);
    EXPECT_EQ((int)g_prev_hits, 3);

    /* Installed again over its own leftover handler: no self-chain, and a
     * remove now restores the original. */
    ASSERT_EQ(hl_main_signal_install(fake_loop(), &rt, 1000), 0);
    EXPECT_TRUE(current_handler(SIGINT) == ours);
    raise(SIGINT);
    EXPECT_EQ((int)g_prev_hits, 4);
    hl_main_signal_remove();
    EXPECT_TRUE(current_handler(SIGINT) == prev_handler);

    sigaction(SIGINT, &saved_int, NULL);
    sigaction(SIGTERM, &saved_term, NULL);
}

/* remove closed the pipe's write end while a handler on another thread could
 * still be about to write to it: the number, reused by the next open, took
 * the handler's byte. The write end now stays open for the process lifetime
 * (one pipe, reused by every install). */
UTEST(main_signal_audit44, remove_does_not_free_write_fd)
{
    HlRuntime rt;
    memset(&rt, 0, sizeof rt);
    enum { N = 256 };
    char installed[N];

    ASSERT_EQ(hl_main_signal_install(fake_loop(), &rt, 1000), 0);
    for (int fd = 0; fd < N; fd++) installed[fd] = (char)fd_open(fd);
    hl_main_signal_remove();

    /* No descriptor open while installed (the pipe among them) was freed
     * for reuse by the remove. */
    int closed = 0, open_after = 0;
    for (int fd = 0; fd < N; fd++) {
        int o = fd_open(fd);
        if (installed[fd] && !o) closed++;
        if (o) open_after++;
    }
    EXPECT_EQ(closed, 0);

    /* Another install reuses the same pipe: no descriptor per install. */
    ASSERT_EQ(hl_main_signal_install(fake_loop(), &rt, 1000), 0);
    hl_main_signal_remove();
    int now = 0;
    for (int fd = 0; fd < N; fd++) if (fd_open(fd)) now++;
    EXPECT_EQ(now, open_after);
}

UTEST_MAIN()
