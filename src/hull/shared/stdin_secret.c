/*
 * stdin_secret.c - one line from stdin with echo off on a terminal
 * (shared/stdin_secret.h, platform gap 4).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/stdin_secret.h"
#include "hull/shared/dispatch_meter.h"   /* the wait is the user's time */
#include "hull/utils/secure_zero.h"
#include "hull/shared/term_escape.h"
#include "hull/sandbox.h"   /* hl_sandbox_std_tty: probed before the sandbox */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* Weak default for the binaries that link this without the sandbox (unit
 * tests, embedders): no probe, so the reader asks isatty. sandbox.c's strong
 * definition wins wherever the sandbox is linked - every hull runner. */
__attribute__((weak)) int hl_sandbox_std_tty(int fd)
{
    (void)fd;
    return -1;
}

/* The settings to put back if the process exits mid-read. Loop thread only
 * (app.main's own code); the atexit handler and the signal path
 * (hl_stdin_secret_restore_terminal) read them. g_saved is written before
 * g_echo_off is set. */
static struct termios         g_saved;
static volatile sig_atomic_t  g_echo_off;
static int                    g_atexit_done;

void hl_stdin_secret_restore_terminal(void)
{
    if (g_echo_off) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
        g_echo_off = 0;
    }
}

#ifdef HL_STDIN_SECRET_TEST_HOOKS
/* Test builds only (stdin_secret_th.o): run at fixed points of the read so a
 * signal can be raised in a window no timing reaches (audit 51). */
static int (*g_test_hook)(int point);
void hl_stdin_secret_test_set_hook(int (*fn)(int point)) { g_test_hook = fn; }
#  define TEST_HOOK(pt) (g_test_hook ? g_test_hook(pt) : 0)
#else
#  define TEST_HOOK(pt) 0
#endif

static void restore_at_exit(void)
{
    hl_stdin_secret_restore_terminal();
}

/* The signals that end or stop the process while echo is off, caught for the
 * read as OpenBSD's readpassphrase catches them: the handler only records
 * the signal; the read ends, echo is put back, and the signal is raised
 * again with its default action. A stop (Ctrl-Z, a background read) is
 * resumed by turning echo off again (audit 50: killed or stopped, the
 * process left the terminal with echo off). Only a signal at its default
 * action is caught: an ignored one stays ignored, and one with a handler of
 * its own (main_signal's SIGINT / SIGTERM, which ends the read through
 * @p stopped and restores the terminal before it dies) is left to it. */
static const int g_sigs[] = {
    SIGALRM, SIGHUP, SIGINT, SIGPIPE, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU,
};
#define N_SIGS ((int)(sizeof g_sigs / sizeof g_sigs[0]))
static struct sigaction       g_prev[N_SIGS];
static int                    g_hooked[N_SIGS];
static volatile sig_atomic_t  g_caught[N_SIGS];

static void on_caught(int sig)
{
    for (int i = 0; i < N_SIGS; i++)
        if (g_sigs[i] == sig) g_caught[i] = 1;
}

static int any_caught(void)
{
    for (int i = 0; i < N_SIGS; i++)
        if (g_caught[i]) return 1;
    return 0;
}

static void hook_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_caught;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < N_SIGS; i++) sigaddset(&sa.sa_mask, g_sigs[i]);
    sa.sa_flags = 0;   /* no SA_RESTART: the wait sees EINTR */
    for (int i = 0; i < N_SIGS; i++) {
        g_caught[i] = 0;
        g_hooked[i] = 0;
        struct sigaction cur;
        if (sigaction(g_sigs[i], NULL, &cur) != 0) continue;
        if ((cur.sa_flags & SA_SIGINFO) || cur.sa_handler != SIG_DFL) continue;
        if (sigaction(g_sigs[i], &sa, &g_prev[i]) == 0) g_hooked[i] = 1;
    }
}

static void unhook_signals(void)
{
    for (int i = 0; i < N_SIGS; i++)
        if (g_hooked[i]) {
            (void)sigaction(g_sigs[i], &g_prev[i], NULL);
            g_hooked[i] = 0;
        }
}

/* Echo back on, with the caught signals blocked on this thread: a blocked
 * SIGTTOU lets a background process set the terminal, and nothing here is
 * interrupted. Another thread still records a signal through the handler. */
static void restore_echo(void)
{
    sigset_t block, prev;
    sigemptyset(&block);
    for (int i = 0; i < N_SIGS; i++) sigaddset(&block, g_sigs[i]);
    pthread_sigmask(SIG_BLOCK, &block, &prev);
    while (tcsetattr(STDIN_FILENO, TCSANOW, &g_saved) != 0 && errno == EINTR) { }
    g_echo_off = 0;
    pthread_sigmask(SIG_SETMASK, &prev, NULL);
}

/* Raise every caught signal again at its default action (the handlers are
 * gone). 1 when one of them stopped the process and it has been resumed, so
 * the read goes on; 0 when none was a stop. */
static int replay_caught(void)
{
    int resumed = 0;
    for (int i = 0; i < N_SIGS; i++) {
        if (!g_caught[i]) continue;
        g_caught[i] = 0;
        (void)raise(g_sigs[i]);
        if (g_sigs[i] == SIGTSTP || g_sigs[i] == SIGTTIN || g_sigs[i] == SIGTTOU)
            resumed = 1;
    }
    return resumed;
}

/* One line through stdio, as ctx.stdin:read() reads one. */
static int read_line_stdio(char *buf, size_t cap, size_t *len)
{
    size_t n = 0;
    int got = 0, too_long = 0;
    uint64_t mx = hl_meter_exclude_begin();
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) break;
        got = 1;
        if (c == '\n') break;
        if (n < cap) buf[n++] = (char)c;
        else too_long = 1;
    }
    hl_meter_exclude_end(mx);
    *len = n;
    if (!got) return ferror(stdin) ? HL_STDIN_SECRET_ERROR : HL_STDIN_SECRET_EOF;
    return too_long ? HL_STDIN_SECRET_TOO_LONG : HL_STDIN_SECRET_OK;
}

/* The line so far: kept across a stop and resume. */
typedef struct {
    size_t n;
    int    got, too_long;
} SecretLine;

/* One line from the terminal in canonical mode, waiting in 100 ms slices so
 * a stop is seen while the user has not answered. HL_STDIN_SECRET_ERROR with
 * a signal caught (any_caught) ends the read for the caller to replay it. */
static int read_line_tty(char *buf, size_t cap, SecretLine *ln,
                         int (*stopped)(void *), void *ud)
{
    int rc = HL_STDIN_SECRET_OK;
    for (;;) {
        if (any_caught()) { rc = HL_STDIN_SECRET_ERROR; break; }
        if (stopped && stopped(ud)) { rc = HL_STDIN_SECRET_INTERRUPTED; break; }
        struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
        uint64_t mx = hl_meter_exclude_begin();
        int pr = poll(&p, 1, 100);
        hl_meter_exclude_end(mx);
        if (pr < 0) {
            if (errno == EINTR) continue;
            rc = HL_STDIN_SECRET_ERROR;
            break;
        }
        if (pr == 0) continue;
        char chunk[512];
        mx = hl_meter_exclude_begin();
        ssize_t r = read(STDIN_FILENO, chunk, sizeof chunk);
        hl_meter_exclude_end(mx);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            rc = HL_STDIN_SECRET_ERROR;
            break;
        }
        if (r == 0) { if (!ln->got) rc = HL_STDIN_SECRET_EOF; break; }   /* Ctrl-D */
        ln->got = 1;
        int eol = 0;
        for (ssize_t i = 0; i < r; i++) {
            if (chunk[i] == '\n') { eol = 1; break; }
            if (ln->n < cap) buf[ln->n++] = chunk[i];
            else ln->too_long = 1;
        }
        hl_secure_zero(chunk, sizeof chunk);
        if (eol) break;
    }
    if (rc == HL_STDIN_SECRET_OK && ln->too_long) rc = HL_STDIN_SECRET_TOO_LONG;
    return rc;
}

int hl_stdin_read_secret(const char *prompt, char *buf, size_t cap, size_t *len,
                         int (*stopped)(void *ud), void *ud)
{
    if (!buf || !len) return HL_STDIN_SECRET_ERROR;
    *len = 0;
    /* Whether stdin is a terminal, as probed before the sandbox applied: an
     * isatty() here is an ioctl the Linux pledge refuses (fatally) on a
     * character device that is not a terminal, /dev/null included. -1 (no
     * probe ran, e.g. a unit test) asks isatty. */
    int tty = hl_sandbox_std_tty(STDIN_FILENO);
    if (tty < 0) tty = isatty(STDIN_FILENO);
    if (!tty)
        return read_line_stdio(buf, cap, len);

    if (!g_atexit_done) { g_atexit_done = 1; (void)atexit(restore_at_exit); }
    SecretLine ln = { 0, 0, 0 };
    int rc, mode = TCSAFLUSH;
    hook_signals();
    for (;;) {
        if (prompt && prompt[0]) {
            /* App text: escaped, so a prompt cannot rewrite the terminal. */
            hl_term_fwrite_escaped(stderr, prompt, strlen(prompt));
            fflush(stderr);
        }
        struct termios old, quiet;
        if (TEST_HOOK(HL_STDIN_SECRET_HOOK_TCGETATTR) ||
            tcgetattr(STDIN_FILENO, &old) != 0) { rc = HL_STDIN_SECRET_ERROR; break; }
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        quiet.c_lflag |= ECHONL;
        g_saved = old;
        if (tcsetattr(STDIN_FILENO, mode, &quiet) == 0) {
            g_echo_off = 1;
            rc = read_line_tty(buf, cap, &ln, stopped, ud);
            restore_echo();
        } else {
            rc = HL_STDIN_SECRET_ERROR;   /* a background job: SIGTTOU caught */
        }
        if (!any_caught()) break;
        /* Echo is on: the signal takes its default action now. A read the
         * signal cut short (ERROR) and that a stop paused reads on once
         * resumed, echo off again; a finished one keeps its result. */
        unhook_signals();
        int resumed = replay_caught();
        if (rc != HL_STDIN_SECRET_ERROR) break;
        if (!resumed) { rc = HL_STDIN_SECRET_INTERRUPTED; break; }
        hook_signals();
        mode = TCSANOW;
    }
    (void)TEST_HOOK(HL_STDIN_SECRET_HOOK_BEFORE_UNHOOK);
    unhook_signals();
    *len = ln.n;
    /* ECHONL moved the cursor for an answered line; a stop or a Ctrl-D did
     * not, so the next output would join the prompt's line. */
    if (rc != HL_STDIN_SECRET_OK && rc != HL_STDIN_SECRET_TOO_LONG) {
        fputc('\n', stderr);
        fflush(stderr);
    }
    return rc;
}
