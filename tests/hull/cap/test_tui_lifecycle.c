/*
 * test_tui_lifecycle.c — PTY-driven tests for the cap layer's
 * lifecycle and rendering paths.
 *
 * Strategy:
 *   - hl_cap_tui_acquire requires isatty(stdin) && isatty(stdout).
 *     Tests that don't fork a PTY assert the ENOTTY rejection path.
 *   - Lifecycle tests fork+forkpty: child does acquire/render/release,
 *     parent reads the master side to verify expected ANSI output.
 *
 * The PTY-based tests are gated on forkpty availability; on platforms
 * without it (e.g. some cosmo builds) they SKIP rather than fail.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/tui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* forkpty(3) lives in different headers on different Unixes:
 *   - glibc/musl Linux: <pty.h>  (link -lutil)
 *   - macOS / FreeBSD / OpenBSD / NetBSD: <util.h>
 * Pick the right one or leave HL_HAVE_FORKPTY undefined. */
#if defined(__linux__)
#  define HL_HAVE_FORKPTY 1
#  include <pty.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  define HL_HAVE_FORKPTY 1
#  include <util.h>
#endif

#ifndef HL_HAVE_FORKPTY
#define HL_HAVE_FORKPTY 0
#endif

/* ── Test tunables ──────────────────────────────────────────────── */

#define TEST_DRAIN_BUF_BYTES    4096   /* per-test stdout drain buffer */
#define TEST_DRAIN_DEADLINE_MS  500    /* total wall-clock for drain */
#define TEST_DRAIN_POLL_MS      50     /* per-poll slice inside drain */
#define TEST_CHILD_WAIT_MS      2000   /* waitpid timeout */
#define TEST_CHILD_POLL_MS      50     /* per-poll slice in waitpid */
#define TEST_PTY_MSG_BUF        64     /* child→parent size-reporting */

/* ── Non-PTY: ENOTTY rejection ──────────────────────────────────── */

UTEST(tui_lifecycle, acquire_without_tty_errors)
{
    /* When running under utest with stdin/stdout NOT a real tty
     * (typical CI environment), acquire should refuse with ENOTTY. */
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        /* Running interactively in a real terminal — this test
         * doesn't apply. SKIP equivalent: just assert true. */
        ASSERT_TRUE(1);
        return;
    }
    HlTuiCtx *ctx = NULL;
    int rc = hl_cap_tui_acquire(&ctx);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(errno, ENOTTY);
    ASSERT_TRUE(ctx == NULL);
}

UTEST(tui_lifecycle, release_null_is_safe)
{
    int rc = hl_cap_tui_release(NULL);
    ASSERT_EQ(rc, 0);
}

UTEST(tui_lifecycle, force_leave_with_no_ctx_is_safe)
{
    /* Just call it; expect no crash, no stdout output. */
    hl_cap_tui_force_leave();
    ASSERT_TRUE(1);
}

/* ── PTY-based ──────────────────────────────────────────────────── */

#if HL_HAVE_FORKPTY

/* Fork a child that runs `child_fn` with stdin/stdout/stderr wired
 * to a fresh PTY slave. The master fd is returned in *master_out.
 * Parent reads/writes through master to drive the child. */
static pid_t spawn_pty(int *master_out, void (*child_fn)(void))
{
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child. */
        child_fn();
        _exit(0);
    }
    *master_out = master;
    return pid;
}

/* Drain all available output for up to `total_ms` total. */
static size_t drain(int fd, char *buf, size_t cap, int total_ms)
{
    size_t got = 0;
    int remaining = total_ms;
    while (got < cap && remaining > 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, TEST_DRAIN_POLL_MS);
        if (pr <= 0) { remaining -= TEST_DRAIN_POLL_MS; continue; }
        ssize_t n = read(fd, buf + got, cap - got);
        if (n <= 0) break;
        got += (size_t)n;
        remaining -= TEST_DRAIN_POLL_MS;
    }
    return got;
}

/* Wait for child with timeout (ms). Returns 1 if reaped, 0 if timeout. */
static int waitpid_timeout(pid_t pid, int *status, int timeout_ms)
{
    int remaining = timeout_ms;
    while (remaining > 0) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) return 1;
        if (r < 0)    return 0;
        usleep(TEST_CHILD_POLL_MS * 1000);
        remaining -= TEST_CHILD_POLL_MS;
    }
    return 0;
}

/* ── Child workloads ────────────────────────────────────────────── */

static void child_acquire_release(void)
{
    HlTuiCtx *ctx = NULL;
    if (hl_cap_tui_acquire(&ctx) != 0) _exit(10);
    if (!ctx) _exit(11);
    if (hl_cap_tui_release(ctx) != 0) _exit(12);
    /* Successful round-trip. */
}

static void child_double_acquire(void)
{
    HlTuiCtx *a = NULL, *b = NULL;
    if (hl_cap_tui_acquire(&a) != 0) _exit(20);
    int rc = hl_cap_tui_acquire(&b);
    if (rc != -1 || errno != EBUSY) _exit(21);
    if (b != NULL) _exit(22);
    hl_cap_tui_release(a);
}

static void child_basic_render(void)
{
    HlTuiCtx *ctx = NULL;
    if (hl_cap_tui_acquire(&ctx) != 0) _exit(30);
    hl_cap_tui_move(ctx, 1, 1);
    hl_cap_tui_write(ctx, "hello", 5);
    hl_cap_tui_flush(ctx);
    hl_cap_tui_release(ctx);
}

static void child_size_query(void)
{
    HlTuiCtx *ctx = NULL;
    if (hl_cap_tui_acquire(&ctx) != 0) _exit(40);
    int cols = 0, rows = 0;
    if (hl_cap_tui_size(ctx, &cols, &rows) != 0) _exit(41);
    /* Print the size to stderr (which is also routed through the PTY)
     * so the parent can pick it up. We write a single recognizable
     * marker so the test isn't sensitive to leftover ANSI output. */
    char msg[TEST_PTY_MSG_BUF];
    int n = snprintf(msg, sizeof msg, "\x1b]hull-tui-size:%d,%d\x07", cols, rows);
    /* We're inside alt screen; emit a known OSC the parser would
     * normally swallow, then leave. */
    write(STDOUT_FILENO, msg, (size_t)n);
    hl_cap_tui_release(ctx);
}

/* ── Tests ──────────────────────────────────────────────────────── */

UTEST(tui_lifecycle_pty, acquire_release_roundtrip)
{
    int master = -1;
    pid_t pid = spawn_pty(&master, child_acquire_release);
    ASSERT_GE(pid, 0);

    /* Drain the master so the child's writes don't fill the PTY
     * buffer and block. */
    char drainbuf[TEST_DRAIN_BUF_BYTES];
    drain(master, drainbuf, sizeof drainbuf, TEST_DRAIN_DEADLINE_MS);

    int status = 0;
    ASSERT_TRUE(waitpid_timeout(pid, &status, TEST_CHILD_WAIT_MS));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    close(master);
}

UTEST(tui_lifecycle_pty, double_acquire_returns_ebusy)
{
    int master = -1;
    pid_t pid = spawn_pty(&master, child_double_acquire);
    ASSERT_GE(pid, 0);

    char drainbuf[TEST_DRAIN_BUF_BYTES];
    drain(master, drainbuf, sizeof drainbuf, TEST_DRAIN_DEADLINE_MS);

    int status = 0;
    ASSERT_TRUE(waitpid_timeout(pid, &status, TEST_CHILD_WAIT_MS));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    close(master);
}

UTEST(tui_lifecycle_pty, basic_render_emits_expected_sequences)
{
    int master = -1;
    pid_t pid = spawn_pty(&master, child_basic_render);
    ASSERT_GE(pid, 0);

    char buf[TEST_DRAIN_BUF_BYTES * 2];
    size_t got = drain(master, buf, sizeof buf - 1, TEST_DRAIN_DEADLINE_MS);
    buf[got] = '\0';

    /* Should contain alt-screen enter, hide-cursor, the "hello"
     * literal, and alt-screen leave. */
    ASSERT_TRUE(strstr(buf, "\x1b[?1049h") != NULL);  /* alt screen */
    ASSERT_TRUE(strstr(buf, "\x1b[?25l")  != NULL);   /* hide cursor */
    ASSERT_TRUE(strstr(buf, "hello")      != NULL);
    ASSERT_TRUE(strstr(buf, "\x1b[?1049l") != NULL);  /* leave alt */
    ASSERT_TRUE(strstr(buf, "\x1b[?25h")  != NULL);   /* show cursor */

    int status = 0;
    ASSERT_TRUE(waitpid_timeout(pid, &status, TEST_CHILD_WAIT_MS));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    close(master);
}

UTEST(tui_lifecycle_pty, size_reports_pty_dimensions)
{
    int master = -1;
    pid_t pid = spawn_pty(&master, child_size_query);
    ASSERT_GE(pid, 0);

    char buf[TEST_DRAIN_BUF_BYTES * 2];
    size_t got = drain(master, buf, sizeof buf - 1, TEST_DRAIN_DEADLINE_MS);
    buf[got] = '\0';

    const char *marker = strstr(buf, "hull-tui-size:");
    ASSERT_TRUE(marker != NULL);
    int c = 0, r = 0;
    ASSERT_EQ(sscanf(marker, "hull-tui-size:%d,%d", &c, &r), 2);
    ASSERT_GT(c, 0);
    ASSERT_GT(r, 0);

    int status = 0;
    ASSERT_TRUE(waitpid_timeout(pid, &status, TEST_CHILD_WAIT_MS));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    close(master);
}

UTEST(tui_lifecycle_pty, termios_restored_after_release)
{
    /* Child grabs termios before+after acquire/release and writes a
     * delta marker to stderr. Easiest signal: c_lflag canonical bit.
     * In raw mode ICANON is cleared; after restore it should match
     * the pre-acquire state. */
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        struct termios before, after;
        tcgetattr(STDIN_FILENO, &before);
        HlTuiCtx *ctx = NULL;
        if (hl_cap_tui_acquire(&ctx) != 0) _exit(60);
        hl_cap_tui_release(ctx);
        tcgetattr(STDIN_FILENO, &after);
        int matches = (before.c_lflag == after.c_lflag)
                   && (before.c_iflag == after.c_iflag)
                   && (before.c_oflag == after.c_oflag);
        _exit(matches ? 0 : 61);
    }

    char drainbuf[TEST_DRAIN_BUF_BYTES];
    drain(master, drainbuf, sizeof drainbuf, TEST_DRAIN_DEADLINE_MS);

    int status = 0;
    ASSERT_TRUE(waitpid_timeout(pid, &status, TEST_CHILD_WAIT_MS));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    close(master);
}

#else /* HL_HAVE_FORKPTY not available */

UTEST(tui_lifecycle_pty, skip_no_forkpty)
{
    /* Build environment lacks forkpty(3); cap-layer lifecycle is
     * exercised by parser tests + the ENOTTY check above. */
    fprintf(stderr, "SKIP: no forkpty on this platform\n");
    ASSERT_TRUE(1);
}

#endif

UTEST_MAIN();
