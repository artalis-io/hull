/*
 * include/hull/dev_state.h - `hull dev --tui` shared state.
 *
 * The hull dev command owns the child process and the log ring
 * buffer; the dev_tui Lua module renders from it. Both sides poke
 * the same struct through accessor functions exposed by
 * src/hull/runtime/lua/mod_tool.c (tool.dev_*).
 *
 * Singleton. One dev session per process.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_DEV_STATE_H
#define HL_DEV_STATE_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HL_DEV_LOG_LINES       512    /* ring capacity (lines) */
#define HL_DEV_LOG_LINE_MAX    512    /* per-line ceiling (bytes) */
#define HL_DEV_PARTIAL_MAX     (4 * 1024)

typedef struct {
    /* Lifecycle / child process. */
    pid_t  child_pid;          /* 0 when no child is running */
    int    pipe_fd;            /* read end of child's stderr+stdout, or -1 */
    int    reload_count;
    int64_t last_reload_ms;    /* monotonic clock, ms since epoch */
    int    should_quit;        /* 'q' or signal - TUI exits cleanly */
    int    pending_reload;     /* 'r' or file change - main reloads child */

    /* Spawning material - kept so we can respawn after reload. */
    const char  *hull_exe;
    const char **child_argv;   /* NULL-terminated, borrowed */
    int          child_argc;

    /* File watch - copied from the existing dev.c flow. */
    char    app_dir[PATH_MAX];
    time_t  baseline_mtime;

    /* Ring buffer of log lines. Most-recent lines at log_head - 1. */
    char  log_lines[HL_DEV_LOG_LINES][HL_DEV_LOG_LINE_MAX];
    int   log_head;            /* next write index */
    int   log_count;           /* total lines ever appended */

    /* Partial-line accumulator (bytes since last newline). */
    char    partial[HL_DEV_PARTIAL_MAX];
    size_t  partial_len;
} HlDevState;

/* Access the singleton. Returns NULL if `hull dev --tui` hasn't
 * initialized it. */
HlDevState *hl_dev_state(void);

/* Drain whatever is currently readable on the pipe into the ring
 * buffer. Returns the number of *bytes* consumed, 0 if nothing was
 * available, -1 on error. Safe to call repeatedly. */
int hl_dev_state_drain(HlDevState *s);

/* Kill the child and respawn. Increments reload_count and adds a
 * marker line to the ring buffer. Returns 0 on success. */
int hl_dev_state_reload(HlDevState *s);

/* Check whether app files have changed since the last reload.
 * Updates `baseline_mtime` and returns 1 if a change is detected,
 * 0 if not. */
int hl_dev_state_check_file_change(HlDevState *s);

#ifdef __cplusplus
}
#endif

#endif /* HL_DEV_STATE_H */
