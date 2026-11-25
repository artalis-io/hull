/*
 * hull/cap/exec_platform.h - what the command broker asks of its host
 * (docs/design/exec_broker.md §6; the broker itself: cap/exec_broker.c)
 *
 * cap/exec_broker.c is portable: the frames, admission, the job table, the
 * owner / mode / identity / pin checks, the fork, the child's common setup
 * and the server's side. Everything that differs between kernels is behind
 * this interface, one translation unit per host:
 *
 *   cap/exec_broker_linux.c   Landlock + seccomp, PR_SET_PDEATHSIG, ELF,
 *                             execveat of the checked descriptor, pidfd
 *   cap/exec_broker_macos.c   one Seatbelt profile for the broker and every
 *                             child (no nested sandbox on macOS), Mach-O,
 *                             execve of the checked path, kqueue
 *
 * Internal to the broker: nothing outside cap/exec_broker*.c includes it
 * (its unit tests do). Every function here runs in the broker process; the
 * hl_exec_plat_child_* ones run in the child between fork and exec and
 * allocate nothing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_EXEC_PLATFORM_H
#define HL_CAP_EXEC_PLATFORM_H

#include "hull/cap/exec.h"
#include "hull/cap/exec_broker.h"

#ifdef HL_EXEC_BROKER_SUPPORTED

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

/* One declared command as the broker holds it. */
typedef struct HlExecBrokerCmd {
    HlExecCmd c;
    char      real[PATH_MAX];       /* realpath of c.path */
    char      interp[PATH_MAX];     /* realpath of its loader, "" = none */
    dev_t     dev;                  /* the file found at policy load */
    ino_t     ino;
    int       plat;                 /* the host's per-command handle, -1 = none
                                       (Linux: the child's Landlock ruleset) */
    int       running;
    /* Its child manifest's fs grants (manifest.fs), resolved at policy load
     * to real paths: n_app_read then n_app_write NUL-terminated strings. */
    char     *app_paths;
    int      *app_fds;              /* the opened grants (exec_child.h), closed
                                       once the lock is applied; -1 = none */
    uint16_t  n_app_read;
    uint16_t  n_app_write;
} HlExecBrokerCmd;

/* The @p i th resolved child-manifest path of @p c (reads first, then
 * writes), or NULL past the end. */
static inline const char *hl_exec_cmd_app_path(const HlExecBrokerCmd *c, unsigned i)
{
    const char *p = c->app_paths;
    if (!p || i >= (unsigned)c->n_app_read + c->n_app_write) return NULL;
    while (i--) p += strlen(p) + 1;
    return p;
}

/* The broker's report: lines appended at @p at, never past @p size. */
typedef struct HlExecReport {
    char  *buf;
    size_t size;
    size_t at;
} HlExecReport;

/* Append one line ("exec: ...\n"); a line that does not fit is dropped. */
void hl_exec_report_line(HlExecReport *r, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* Replace the whole report with one message (a refusal). */
void hl_exec_report_set(HlExecReport *r, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* ── The broker process ────────────────────────────────────────────── */

/* First thing in hl_exec_broker_main: make the broker go when the server
 * does (Linux: PR_SET_PDEATHSIG; macOS: a kqueue watch on the server's
 * pid). *watch_fd gets a descriptor that turns readable once the server is
 * gone, or -1 when the kernel ends the broker itself. 0, or -1: the server
 * is gone already, do not serve. */
int hl_exec_plat_broker_init(int *watch_fd);

/* The leaf checks this host adds to the portable ones (owner, mode,
 * set-ID, executable bit), on the descriptor about to run. Linux refuses
 * file capabilities. 0, or -1 (@p why says). Run at load and every call. */
int hl_exec_plat_leaf_check(int fd, const char *real, char *why, size_t why_size);

/* Is the file one the kernel runs directly (not a #! script, which would
 * bring an interpreter)? @p interp gets the absolute path of the loader it
 * names, unresolved, or "" for none. 0, or -1 (@p why says). Load only. */
int hl_exec_plat_binary_check(int fd, const char *real, char *interp,
                              size_t interp_size, char *why, size_t why_size);

/* Lock the broker down once every command is loaded and checked (§6.2):
 * build what each child needs (cmd->plat) and confine this process to the
 * union of the commands' grants. @p self is this binary's real path.
 * @p flags are HL_EXEC_POLICY_*: with NO_SANDBOX, nothing is confined;
 * a guarantee this host cannot give refuses unless DEGRADED_OK. Report
 * lines (degraded guarantees, this host's limits) are appended to @p r.
 * 0, or -1 with @p r set to why. */
int hl_exec_plat_lock(HlExecBrokerCmd *cmds, int n, unsigned flags,
                      const char *self, HlExecReport *r);

/* A descriptor that turns readable once @p pid exits, so its reply goes
 * out at once; -1 when the host has none (the broker then reaps on a
 * 50 ms tick). The broker closes it. */
int hl_exec_plat_exit_watch(pid_t pid);

/* ── The child, between fork and exec ──────────────────────────────── */

/* Stage numbers the child reports when a step fails. */
#define HL_EXEC_STAGE_DEATH     1   /* die with the broker */
#define HL_EXEC_STAGE_SETSID    2
#define HL_EXEC_STAGE_PRIVS     3   /* no new privileges */
#define HL_EXEC_STAGE_CONFINE   4   /* the per-command domain */
#define HL_EXEC_STAGE_STDIO     5
#define HL_EXEC_STAGE_CHDIR     6
#define HL_EXEC_STAGE_FILTER    7   /* the per-command syscall filter */
#define HL_EXEC_STAGE_EXEC      8

/* Step 1: die with the broker (Linux: PR_SET_PDEATHSIG; macOS: nothing to
 * set - a child cannot outlive its broker's channel unnoticed, and cannot
 * fork). 0, or -1 with errno. */
int hl_exec_plat_child_death(void);

/* Step 3-4 (before stdio moves): no new privileges, then the per-command
 * domain. Returns 0, or the HL_EXEC_STAGE_* that failed (errno set). */
int hl_exec_plat_child_confine(const HlExecBrokerCmd *cmd, unsigned flags);

/* Step 7 (last before exec): the per-command syscall filter. 0, or -1. */
int hl_exec_plat_child_filter(const HlExecBrokerCmd *cmd, unsigned flags);

/* Step 8: run the checked binary, open as descriptor @p bfd. Returns only
 * on failure (errno set). */
void hl_exec_plat_child_exec(const HlExecBrokerCmd *cmd, int bfd,
                             char *const argv[], char *const envp[]);

#ifdef __APPLE__
/* The broker's Seatbelt profile (exec_broker_macos.c), exposed for its unit
 * tests: the SBPL text and its key / value parameter list (NULL-ended).
 * 0, or -1 (out of memory). Free with hl_exec_macos_profile_free. */
int  hl_exec_macos_profile(const HlExecBrokerCmd *cmds, int n, const char *self,
                           char **profile, char ***params);
void hl_exec_macos_profile_free(char *profile, char **params);
#endif

#endif /* HL_EXEC_BROKER_SUPPORTED */

#endif /* HL_CAP_EXEC_PLATFORM_H */
