/*
 * test_stdin_secret.c - shared/stdin_secret.c on a terminal: echo is back on
 * after a signal ends or stops the process while a read_secret waits
 * (audit 50), and a stopped read that is resumed reads on.
 *
 * Each case forks a child on a PTY that plays the shell: the reader runs in a
 * process group of its own in the foreground (a stop is discarded in an
 * orphaned process group, the PTY child's own), and the shell reports its
 * stops and its end and stays until killed (the terminal is revoked once its
 * session leader exits). The parent reads the terminal's settings through
 * the slave it opens itself.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/shared/stdin_secret.h"
#include "hull/shared/main_signal.h"
#include "hull/shared/async_backend.h"
#include "hull/runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) && !defined(__COSMOPOLITAN__)
#  define HAVE_FORKPTY 1
#  include <pty.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  define HAVE_FORKPTY 1
#  include <util.h>
#endif

#ifdef HAVE_FORKPTY

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Read what the child wrote, so it never waits on the PTY (TCSAFLUSH waits
 * until the prompt has been read). */
static void drain(int master)
{
    char b[256];
    int fl = fcntl(master, F_GETFL);
    (void)fcntl(master, F_SETFL, fl | O_NONBLOCK);
    while (read(master, b, sizeof b) > 0) { }
    (void)fcntl(master, F_SETFL, fl);
}

/* 1 when the terminal echoes, 0 when not, -1 on error. */
static int echo_on(int slave)
{
    struct termios t;
    if (tcgetattr(slave, &t) != 0) return -1;
    return (t.c_lflag & ECHO) ? 1 : 0;
}

/* Read exactly @p n bytes from the non-blocking @p fd within 5 s. */
static int read_full(int fd, void *out, size_t n)
{
    size_t got = 0;
    for (int i = 0; i < 500 && got < n; i++) {
        ssize_t r = read(fd, (char *)out + got, n - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return 0;
        if (errno != EAGAIN && errno != EINTR) return 0;
        sleep_ms(10);
    }
    return got == n;
}

/* The reader: one secret; exits 0 when it is "abc", 2 otherwise. With
 * @p main_signal, main_signal is installed first (and the read has no stop
 * callback, as a synchronous stretch would not see one). */
static void child_read(int main_signal)
{
    struct rlimit no_core = { 0, 0 };
    (void)setrlimit(RLIMIT_CORE, &no_core);
    if (main_signal) {
        static HlRuntime rt;
        HlAsyncBackendCtx *loop = NULL;
        if (hl_async_backend()->init(&loop, NULL) != 0 ||
            hl_main_signal_install(loop, &rt, 1000) != 0)
            _exit(9);
    }
    char buf[64];
    size_t n = 0;
    int rc = hl_stdin_read_secret("Key: ", buf, sizeof buf, &n, NULL, NULL);
    _exit(rc == HL_STDIN_SECRET_OK && n == 3 && memcmp(buf, "abc", 3) == 0 ? 0 : 2);
}

/* The shell, on the PTY: reports the reader's pid, 'S' per stop, then 'E'
 * and its wait status on @p rep; then waits to be killed. */
static void child_shell(int rep, int main_signal)
{
    pid_t gc = fork();
    if (gc == 0) {
        (void)setpgid(0, 0);
        while (tcgetpgrp(STDIN_FILENO) != getpid()) sleep_ms(5);
        child_read(main_signal);
    }
    if (gc < 0) _exit(9);
    (void)setpgid(gc, gc);
    signal(SIGTTOU, SIG_IGN);
    (void)tcsetpgrp(STDIN_FILENO, gc);
    if (write(rep, &gc, sizeof gc) != (ssize_t)sizeof gc) _exit(9);
    for (;;) {
        int st;
        if (waitpid(gc, &st, WUNTRACED) != gc) _exit(9);
        if (WIFSTOPPED(st)) {
            char c = 'S';
            if (write(rep, &c, 1) != 1) _exit(9);
            continue;
        }
        char c = 'E';
        if (write(rep, &c, 1) != 1 || write(rep, &st, sizeof st) != (ssize_t)sizeof st)
            _exit(9);
        for (;;) pause();
    }
}

typedef struct {
    pid_t shell, reader;
    int   master, slave, rep;
} Pty;

static int pty_start(Pty *p, int main_signal)
{
    int rep[2];
    if (pipe(rep) != 0) return -1;
    char name[128];
    p->shell = forkpty(&p->master, name, NULL, NULL);
    if (p->shell == 0) { close(rep[0]); child_shell(rep[1], main_signal); }
    close(rep[1]);
    if (p->shell < 0) { close(rep[0]); return -1; }
    p->rep = rep[0];
    (void)fcntl(p->rep, F_SETFL, fcntl(p->rep, F_GETFL) | O_NONBLOCK);
    p->slave = open(name, O_RDWR | O_NOCTTY);
    if (p->slave < 0 || !read_full(p->rep, &p->reader, sizeof p->reader)) return -1;
    return 0;
}

/* Wait (up to 3 s) until echo is @p want. */
static int pty_wait_echo(Pty *p, int want)
{
    for (int i = 0; i < 300; i++) {
        drain(p->master);
        if (echo_on(p->slave) == want) return 1;
        sleep_ms(10);
    }
    return 0;
}

/* The next report: 'S', or 'E' with the reader's wait status in *st. */
static char pty_report(Pty *p, int *st)
{
    char c = 0;
    for (int i = 0; i < 500 && c == 0; i++) {
        drain(p->master);
        if (read(p->rep, &c, 1) == 1) break;
        c = 0;
        sleep_ms(10);
    }
    if (c == 'E' && !read_full(p->rep, st, sizeof *st)) return 0;
    return c;
}

static void pty_end(Pty *p)
{
    kill(p->reader, SIGKILL);
    kill(p->shell, SIGKILL);
    int st;
    (void)waitpid(p->shell, &st, 0);
    close(p->rep);
    close(p->slave);
    close(p->master);
}

/* A signal at its default action that kills the reader while echo is off:
 * only a return from the read or exit() restored the terminal, so it was
 * left with echo off. */
static void killed_restores_echo(int *utest_result, int sig)
{
    Pty p;
    ASSERT_EQ(0, pty_start(&p, 0));
    ASSERT_TRUE(pty_wait_echo(&p, 0));
    kill(p.reader, sig);
    int st = 0;
    EXPECT_EQ('E', pty_report(&p, &st));
    EXPECT_TRUE(WIFSIGNALED(st) && WTERMSIG(st) == sig);
    EXPECT_EQ(1, echo_on(p.slave));
    pty_end(&p);
}

UTEST(stdin_secret_audit50, sigquit_restores_echo)
{
    killed_restores_echo(utest_result, SIGQUIT);   /* Ctrl-\ */
}

UTEST(stdin_secret_audit50, sighup_restores_echo)
{
    killed_restores_echo(utest_result, SIGHUP);
}

/* SIGINT / SIGTERM with no handler (main_signal not installed). */
UTEST(stdin_secret_audit50, sigterm_restores_echo)
{
    killed_restores_echo(utest_result, SIGTERM);
}

/* Ctrl-Z stopped the process with echo off, so the shell it went back to
 * did not echo. Now echo is on while it is stopped, off again once it is
 * resumed, and the read goes on. */
UTEST(stdin_secret_audit50, sigtstp_restores_echo_and_resumes)
{
    Pty p;
    ASSERT_EQ(0, pty_start(&p, 0));
    ASSERT_TRUE(pty_wait_echo(&p, 0));
    kill(p.reader, SIGTSTP);
    int st = -1;
    EXPECT_EQ('S', pty_report(&p, &st));
    EXPECT_EQ(1, echo_on(p.slave));
    kill(p.reader, SIGCONT);
    EXPECT_TRUE(pty_wait_echo(&p, 0));
    ASSERT_EQ(4, (int)write(p.master, "abc\n", 4));
    EXPECT_EQ('E', pty_report(&p, &st));
    EXPECT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);   /* it read "abc" */
    EXPECT_EQ(1, echo_on(p.slave));
    pty_end(&p);
}

/* With main_signal installed, a second SIGINT before the first was
 * delivered takes the default action from its handler: that path died with
 * echo off. */
UTEST(stdin_secret_audit50, main_signal_second_sigint_restores_echo)
{
    Pty p;
    ASSERT_EQ(0, pty_start(&p, 1));
    ASSERT_TRUE(pty_wait_echo(&p, 0));
    kill(p.reader, SIGINT);
    sleep_ms(50);
    kill(p.reader, SIGINT);
    int st = 0;
    EXPECT_EQ('E', pty_report(&p, &st));
    EXPECT_TRUE(WIFSIGNALED(st) && WTERMSIG(st) == SIGINT);
    EXPECT_EQ(1, echo_on(p.slave));
    pty_end(&p);
}

#else
UTEST(stdin_secret_audit50, skip_no_forkpty)
{
    fprintf(stderr, "SKIP: no forkpty on this platform\n");
}
#endif

UTEST_MAIN()
