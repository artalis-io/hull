/**
 * @file stdin_secret.h
 * @brief A line read from stdin with echo off when stdin is a terminal
 *        (app.main's ctx.stdin:read_secret / ctx.stdin.readSecret, platform
 *        gap 4), as getpass reads a password.
 *
 * On a terminal: the prompt (if any) goes to stderr, echo is turned off
 * (ECHONL kept on, so the Enter still moves the cursor), one line is read,
 * and the terminal's settings are put back on every return - a stop the
 * caller's @p stopped reports (a SIGINT / SIGTERM) included, checked between
 * 100 ms waits. While echo is off, the signals that would end or stop the
 * process at their default action (SIGHUP, SIGINT, SIGQUIT, SIGTERM,
 * SIGALRM, SIGPIPE, SIGTSTP, SIGTTIN, SIGTTOU) are caught: echo is put back
 * and the signal raised again at its default action, and a stopped read that
 * is resumed turns echo off again and reads on (audit 50). A signal with a
 * handler of its own is left to it; main_signal's calls
 * hl_stdin_secret_restore_terminal before it dies. A process that exits with
 * echo still off is restored by an atexit handler. The wait is excluded from
 * the binding meter.
 *
 * Not a terminal (a pipe, a file): no prompt, no terminal calls; one line is
 * read through stdio exactly as ctx.stdin:read() reads one, so
 * `printf %s "$KEY" | tool login` works.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_STDIN_SECRET_H
#define HL_SHARED_STDIN_SECRET_H

#include <stddef.h>

/** The longest line read_secret takes (bytes, the newline excluded). */
#define HL_STDIN_SECRET_MAX 65536

enum {
    HL_STDIN_SECRET_OK          = 0,   /**< a line (maybe empty) in buf */
    HL_STDIN_SECRET_EOF         = 1,   /**< end of input before any byte */
    HL_STDIN_SECRET_ERROR       = -1,  /**< a read or a terminal call failed */
    HL_STDIN_SECRET_INTERRUPTED = -2,  /**< @p stopped said stop */
    HL_STDIN_SECRET_TOO_LONG    = -3,  /**< past @p cap; the rest of the line is consumed */
};

/**
 * Read one line into @p buf (at most @p cap bytes, no newline, not
 * NUL-terminated), its length in @p *len. @p stopped (may be NULL) is asked
 * between waits on a terminal. The caller scrubs @p buf after use.
 */
int hl_stdin_read_secret(const char *prompt, char *buf, size_t cap, size_t *len,
                         int (*stopped)(void *ud), void *ud);

/**
 * Put the terminal's settings back if a read has echo off. Async-signal-safe:
 * a signal handler about to end the process calls it.
 */
void hl_stdin_secret_restore_terminal(void);

#endif /* HL_SHARED_STDIN_SECRET_H */
