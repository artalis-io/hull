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
 * 100 ms waits. A process that exits with echo still off (it cannot happen
 * through this call; an exit from a signal handler could) is restored by an
 * atexit handler. The wait is excluded from the binding meter.
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

#endif /* HL_SHARED_STDIN_SECRET_H */
