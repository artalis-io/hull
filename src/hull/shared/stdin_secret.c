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
 * (app.main's own code); the atexit handler reads them at exit. */
static struct termios g_saved;
static int            g_echo_off;
static int            g_atexit_done;

static void restore_at_exit(void)
{
    if (g_echo_off)
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
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

/* One line from the terminal in canonical mode, waiting in 100 ms slices so
 * a stop is seen while the user has not answered. */
static int read_line_tty(char *buf, size_t cap, size_t *len,
                         int (*stopped)(void *), void *ud)
{
    size_t n = 0;
    int got = 0, too_long = 0, rc = HL_STDIN_SECRET_OK;
    for (;;) {
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
        if (r == 0) { if (!got) rc = HL_STDIN_SECRET_EOF; break; }   /* Ctrl-D */
        got = 1;
        int eol = 0;
        for (ssize_t i = 0; i < r; i++) {
            if (chunk[i] == '\n') { eol = 1; break; }
            if (n < cap) buf[n++] = chunk[i];
            else too_long = 1;
        }
        hl_secure_zero(chunk, sizeof chunk);
        if (eol) break;
    }
    *len = n;
    if (rc == HL_STDIN_SECRET_OK && too_long) rc = HL_STDIN_SECRET_TOO_LONG;
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

    if (prompt && prompt[0]) {
        /* App text: escaped, so a prompt cannot rewrite the terminal. */
        hl_term_fwrite_escaped(stderr, prompt, strlen(prompt));
        fflush(stderr);
    }
    struct termios old, quiet;
    if (tcgetattr(STDIN_FILENO, &old) != 0)
        return HL_STDIN_SECRET_ERROR;
    quiet = old;
    quiet.c_lflag &= ~(tcflag_t)ECHO;
    quiet.c_lflag |= ECHONL;
    g_saved = old;
    if (!g_atexit_done) { g_atexit_done = 1; (void)atexit(restore_at_exit); }
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) != 0)
        return HL_STDIN_SECRET_ERROR;
    g_echo_off = 1;

    int rc = read_line_tty(buf, cap, len, stopped, ud);

    (void)tcsetattr(STDIN_FILENO, TCSANOW, &old);
    g_echo_off = 0;
    /* ECHONL moved the cursor for an answered line; a stop or a Ctrl-D did
     * not, so the next output would join the prompt's line. */
    if (rc != HL_STDIN_SECRET_OK && rc != HL_STDIN_SECRET_TOO_LONG) {
        fputc('\n', stderr);
        fflush(stderr);
    }
    return rc;
}
