/*
 * cap/exec_broker_macos.c - the command broker's macOS backend
 * (docs/design/exec_broker.md §6.4; interface: hull/cap/exec_platform.h)
 *
 * macOS refuses a second sandbox in a sandboxed process (sandbox_init
 * fails with EPERM), so a child cannot narrow itself after fork as it does
 * on Linux. A broker applies ONE Seatbelt profile to itself, the union of
 * its commands' grants, and every child inherits it:
 *
 *   - deny by default;
 *   - process-exec of each declared binary by its real path, nothing else;
 *   - process-fork only for this binary's own image (process-path SELF):
 *     the broker forks, and anything it execs gets EPERM from fork, vfork
 *     and posix_spawn, so a command cannot leave its process group;
 *   - reads of the system trees, each command's fs.read and its binary;
 *     writes of /dev/null and each command's fs.write; AF_UNIX sockets
 *     only when a command sets unix_sockets; no other network.
 *
 * With two or more commands the broker is a ROUTER (exec_broker.c): it
 * starts one broker per command, each applying the profile below for its one
 * command, then locks itself with hl_exec_macos_router_profile. So the
 * "union" is always one command's own grants
 * (docs/design/exec_broker_macos_isolation.md).
 *
 * macOS has no PR_SET_PDEATHSIG, so each call runs under a KEEPER: the
 * broker forks it (this image, never exec'd) and it forks the job, so it is
 * the job's parent. It watches the broker (its kill pipe's EOF, a kqueue
 * watch) and kills and reaps the job when the broker goes, however it goes,
 * SIGKILL included. What is left (reported at start as an "exec: limit:"
 * line): a KEEPER killed with SIGKILL leaves its job running until it exits;
 * the job cannot fork and stays under its CPU limit.
 *
 * The child execs the checked PATH (macOS has no fexecve / execveat): the
 * file was checked on a descriptor just before, and only root can swap a
 * file under a root-owned path (§6.1).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/exec_platform.h"

#if defined(HL_EXEC_BROKER_SUPPORTED) && defined(__APPLE__)

#include <errno.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* sandbox.h is deprecated since macOS 10.8; the same two entry points
 * src/hull/sandbox.c declares. */
extern int sandbox_init_with_parameters(const char *profile, uint64_t flags,
                                        const char *const parameters[],
                                        char **errorbuf);
extern void sandbox_free_error(char *errorbuf);

/* ── The profile ───────────────────────────────────────────────────── */

/* The trees every command may read (§6.2, the macOS names): never the
 * home directories, the app directory, /Applications or /private/var
 * beyond dyld's cache. "/" itself: dyld reads it to find the shared
 * cache, and a child aborts at start without it. */
static const char PROFILE_HEAD[] =
    "(version 1)\n"
    "(deny default)\n"
    "(allow file-read-metadata)\n"
    "(allow file-read*\n"
    "    (literal \"/\")\n"
    "    (subpath \"/usr\") (subpath \"/bin\") (subpath \"/sbin\")\n"
    "    (subpath \"/System\") (subpath \"/private/etc\")\n"
    "    (subpath \"/private/var/db/dyld\")\n"
    "    (subpath \"/Library/Preferences/Logging\")\n"
    "    (literal \"/dev/urandom\") (literal \"/dev/random\") (literal \"/dev/zero\"))\n"
    "(allow file-map-executable (subpath \"/System\") (subpath \"/usr/lib\"))\n"
    "(allow file-read* file-write-data (literal \"/dev/null\")\n"
    "    (literal \"/dev/dtracehelper\"))\n"
    "(allow file-ioctl (literal \"/dev/dtracehelper\"))\n"
    "(allow sysctl-read)\n"
    /* getpwuid & co (id, whoami, ls -l) ask opendirectoryd */
    "(allow mach-lookup (global-name \"com.apple.system.opendirectoryd.libinfo\"))\n"
    /* the broker forks; nothing it execs can */
    "(allow process-fork (process-path (param \"SELF\")))\n"
    /* the broker kills each job's process group */
    "(allow signal (target children))\n";

typedef struct {
    char  *text;   size_t len, cap;
    char **keys;   char **vals;   int n, cap_p;
    int    oom;
} Prof;

static void p_put(Prof *p, const char *s)
{
    size_t k = strlen(s);
    if (p->oom) return;
    if (p->len + k + 1 > p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 4096;
        while (nc < p->len + k + 1) nc *= 2;
        char *t = realloc(p->text, nc);
        if (!t) { p->oom = 1; return; }
        p->text = t;
        p->cap = nc;
    }
    memcpy(p->text + p->len, s, k + 1);
    p->len += k;
}

/* A parameter (the path never goes into the profile text, so it needs no
 * quoting); returns its key, or NULL. */
static const char *p_param(Prof *p, const char *key, const char *val)
{
    if (p->oom) return NULL;
    if (p->n + 1 > p->cap_p) {
        int nc = p->cap_p ? p->cap_p * 2 : 64;
        char **k = realloc(p->keys, (size_t)nc * sizeof *k);
        if (!k) { p->oom = 1; return NULL; }
        p->keys = k;
        char **v = realloc(p->vals, (size_t)nc * sizeof *v);
        if (!v) { p->oom = 1; return NULL; }
        p->vals = v;
        p->cap_p = nc;
    }
    p->keys[p->n] = strdup(key);
    p->vals[p->n] = strdup(val);
    if (!p->keys[p->n] || !p->vals[p->n]) {
        free(p->keys[p->n]);
        free(p->vals[p->n]);
        p->oom = 1;
        return NULL;
    }
    return p->keys[p->n++];
}

/* Grant @p ops on @p path (resolved: Seatbelt matches the real path, so
 * /tmp/x is /private/tmp/x). A path that does not exist is skipped, as
 * Linux skips it. */
static void p_grant(Prof *p, const char *ops, const char *prefix, int i, int j,
                    const char *path)
{
    char real[PATH_MAX], name[32];
    if (!realpath(path, real)) return;
    snprintf(name, sizeof name, "%s%d_%d", prefix, i, j);
    const char *key = p_param(p, name, real);
    if (!key) return;
    char line[256];
    snprintf(line, sizeof line, "(allow %s (subpath (param \"%s\")))\n", ops, key);
    p_put(p, line);
}

/* As p_grant, for a path that is already the real path of an opened grant
 * (exec_child.c, F_GETPATH on its descriptor): not resolved again, so a
 * component swapped for a symbolic link since cannot redirect it. */
static void p_grant_real(Prof *p, const char *ops, const char *prefix, int i, int j,
                         const char *real)
{
    char name[32];
    if (!real || real[0] != '/') return;
    snprintf(name, sizeof name, "%s%d_%d", prefix, i, j);
    const char *key = p_param(p, name, real);
    if (!key) return;
    char line[256];
    snprintf(line, sizeof line, "(allow %s (subpath (param \"%s\")))\n", ops, key);
    p_put(p, line);
}

static void prof_free(Prof *p)
{
    for (int i = 0; i < p->n; i++) { free(p->keys[i]); free(p->vals[i]); }
    free(p->keys);
    free(p->vals);
    free(p->text);
    memset(p, 0, sizeof *p);
}

int hl_exec_macos_profile(const HlExecBrokerCmd *cmds, int n, const char *self,
                          char **profile, char ***params)
{
    Prof p = { 0 };
    p_put(&p, PROFILE_HEAD);
    (void)p_param(&p, "SELF", self);
    int unix_sockets = 0;
    for (int i = 0; i < n; i++) {
        const HlExecBrokerCmd *c = &cmds[i];
        char name[32];
        snprintf(name, sizeof name, "B%d", i);
        const char *key = p_param(&p, name, c->real);
        if (key) {
            char line[256];
            snprintf(line, sizeof line,
                     "(allow process-exec file-read* file-map-executable "
                     "(literal (param \"%s\")))\n", key);
            p_put(&p, line);
        }
        if (c->interp[0]) p_grant(&p, "file-read* file-map-executable", "I", i, 0,
                                  c->interp);
        const uint8_t *cur = c->c.fs_read;
        for (unsigned j = 0; j < c->c.n_fs_read; j++)
            p_grant(&p, "file-read*", "R", i, (int)j, hl_exec_next_str(&cur, NULL));
        cur = c->c.fs_write;
        for (unsigned j = 0; j < c->c.n_fs_write; j++)
            p_grant(&p, "file-read* file-write* file-ioctl", "W", i, (int)j,
                    hl_exec_next_str(&cur, NULL));
        /* The child manifest's fs, resolved to the exact paths. On macOS
         * every command shares one profile, so these join the union (§6.4). */
        for (unsigned j = 0; j < c->n_app_read; j++)
            p_grant_real(&p, "file-read*", "AR", i, (int)j, hl_exec_cmd_app_path(c, j));
        /* write, truncate and create - not unlink or rename (as on Linux,
         * where an app write grant carries no REMOVE right) */
        for (unsigned j = 0; j < c->n_app_write; j++)
            p_grant_real(&p, "file-write-data file-write-create", "AW", i, (int)j,
                    hl_exec_cmd_app_path(c, c->n_app_read + j));
        if (c->c.flags & HL_EXEC_CMD_UNIX_SOCKETS) unix_sockets = 1;
    }
    if (unix_sockets)
        p_put(&p, "(allow system-socket (socket-domain AF_UNIX))\n"
                  "(allow network-outbound (remote unix-socket))\n");
    if (p.oom) { prof_free(&p); return -1; }

    /* key, value, ..., NULL */
    char **kv = calloc((size_t)p.n * 2 + 1, sizeof *kv);
    if (!kv) { prof_free(&p); return -1; }
    for (int i = 0; i < p.n; i++) { kv[2 * i] = p.keys[i]; kv[2 * i + 1] = p.vals[i]; }
    free(p.keys);
    free(p.vals);
    *profile = p.text;
    *params = kv;
    return 0;
}

void hl_exec_macos_profile_free(char *profile, char **params)
{
    if (params) for (char **q = params; *q; q++) free(*q);
    free(params);
    free(profile);
}

/* ── The interface ─────────────────────────────────────────────────── */

int hl_exec_plat_broker_init(int *watch_fd)
{
    /* No PR_SET_PDEATHSIG: watch the server's pid. The channel's EOF says
     * the same; this catches a server that is gone while a child holds a
     * copy of nothing (every channel copy is close-on-exec). */
    *watch_fd = -1;
    pid_t server = getppid();
    if (server == 1) return -1;
    int kq = kqueue();
    if (kq < 0) return 0;               /* the channel's EOF still ends it */
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)server, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) != 0) {
        close(kq);
        return errno == ESRCH ? -1 : 0;   /* ESRCH: it is gone already */
    }
    (void)fcntl(kq, F_SETFD, FD_CLOEXEC);
    *watch_fd = kq;
    return 0;
}

int hl_exec_plat_leaf_check(int fd, const char *real, char *why, size_t why_size)
{
    (void)fd; (void)real; (void)why; (void)why_size;
    return 0;   /* no file capabilities on macOS */
}

int hl_exec_plat_binary_check(int fd, const char *real, char *interp,
                              size_t isz, char *why, size_t why_size)
{
    interp[0] = '\0';
    uint32_t m[2];
    if (pread(fd, m, sizeof m, 0) != (ssize_t)sizeof m) {
        snprintf(why, why_size, "%s is not a Mach-O executable", real);
        return -1;
    }
    if (m[0] == MH_MAGIC_64) {
        /* host order: the only order this kernel runs */
    } else if (m[0] == OSSwapHostToBigInt32(FAT_MAGIC) ||
               m[0] == OSSwapHostToBigInt32(FAT_MAGIC_64)) {
        /* A Java class file starts with the same bytes; its "count" is a
         * version number (45 and up), a universal binary's a few. */
        uint32_t nfat = OSSwapBigToHostInt32(m[1]);
        if (nfat == 0 || nfat > 16) {
            snprintf(why, why_size, "%s is not a Mach-O executable", real);
            return -1;
        }
    } else if (m[0] == MH_MAGIC || m[0] == MH_CIGAM || m[0] == MH_CIGAM_64) {
        snprintf(why, why_size, "%s is not a 64-bit Mach-O for this host", real);
        return -1;
    } else {
        snprintf(why, why_size, "%s is not a Mach-O executable (a script would "
                 "bring its interpreter)", real);
        return -1;
    }
    /* Every dynamic Mach-O names the same loader (LC_LOAD_DYLINKER). */
    snprintf(interp, isz, "/usr/lib/dyld");
    return 0;
}

int hl_exec_plat_lock(HlExecBrokerCmd *cmds, int n, unsigned flags,
                      const char *self, HlExecReport *r)
{
    for (int i = 0; i < n; i++) {
        cmds[i].plat = -1;
        /* The fork right goes with this image: a command that IS this
         * binary could fork from under it. */
        if (strcmp(cmds[i].real, self) == 0) {
            hl_exec_report_set(r, "exec.%s: %s is this hull binary itself, "
                               "which macOS lets fork; declare another command",
                               cmds[i].c.name, cmds[i].real);
            return -1;
        }
    }
    if (flags & HL_EXEC_POLICY_NO_SANDBOX) {
        hl_exec_report_line(r, "exec: no sandbox (--no-sandbox)");
        return 0;
    }
    /* Several commands under a sandbox are routed, one broker each
     * (hl_exec_plat_router); here n is 1, or the sandbox is off. */
    hl_exec_report_line(r, "exec: limit: no parent-death signal on macOS: each "
                        "call's keeper ends it when the broker goes; a keeper "
                        "killed with SIGKILL leaves its call running until it "
                        "exits (it cannot fork; its CPU limit holds)");

    char *profile = NULL, **params = NULL;
    if (hl_exec_macos_profile(cmds, n, self, &profile, &params) != 0) {
        hl_exec_report_set(r, "exec: out of memory building the broker's sandbox");
        return -1;
    }
    char *err = NULL;
    int rc = sandbox_init_with_parameters(profile, 0, (const char *const *)params,
                                          &err);
    if (rc != 0) {
        hl_exec_report_set(r, "exec: the broker's Seatbelt profile could not be "
                           "applied: %s", err ? err : strerror(errno));
        if (err) sandbox_free_error(err);
    }
    hl_exec_macos_profile_free(profile, params);
    return rc == 0 ? 0 : -1;
}

/* ── The router (docs/design/exec_broker_macos_isolation.md §4.2) ──── */

/* Everything the router uses once locked is a descriptor it holds (the
 * server's channel, each command's broker channel, kqueue watches): no file
 * rule, no exec, no fork. It may signal its children (the command brokers). */
const char hl_exec_macos_router_profile[] =
    "(version 1)\n"
    "(deny default)\n"
    "(allow signal (target children))\n";

int hl_exec_plat_router(void) { return 1; }

int hl_exec_plat_router_lock(HlExecReport *r)
{
    const char *const params[] = { NULL };
    char *err = NULL;
    if (sandbox_init_with_parameters(hl_exec_macos_router_profile, 0, params, &err) != 0) {
        hl_exec_report_set(r, "exec: the command router's Seatbelt profile could not "
                           "be applied: %s", err ? err : strerror(errno));
        if (err) sandbox_free_error(err);
        return -1;
    }
    hl_exec_report_line(r, "exec: limit: no parent-death signal on macOS: each "
                        "call's keeper ends it when the broker goes; a keeper "
                        "killed with SIGKILL leaves its call running until it "
                        "exits (it cannot fork; its CPU limit holds)");
    return 0;
}

int hl_exec_plat_exit_watch(pid_t pid)
{
    int kq = kqueue();
    if (kq < 0) return -1;
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) != 0) {   /* ESRCH: gone already */
        close(kq);
        return -1;
    }
    (void)fcntl(kq, F_SETFD, FD_CLOEXEC);
    return kq;
}

int hl_exec_plat_keeper(void) { return 1; }

/* Leave as the job left: the same exit code, or the same signal. */
static void keeper_exit_as(int st) __attribute__((noreturn));
static void keeper_exit_as(int st)
{
    if (WIFSIGNALED(st)) {
        int sig = WTERMSIG(st);
        signal(sig, SIG_DFL);
        sigset_t one;
        sigemptyset(&one);
        sigaddset(&one, sig);
        sigprocmask(SIG_UNBLOCK, &one, NULL);
        kill(getpid(), sig);
        _exit(128 + sig);                 /* a signal whose default is not to end */
    }
    _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 127);
}

/* Kill the job (when asked), wait for it, and leave as it left. */
static void keeper_end(pid_t job, int kill_it) __attribute__((noreturn));
static void keeper_end(pid_t job, int kill_it)
{
    int st = 0;
    if (kill_it) {
        kill(-job, SIGKILL);              /* its group, once it ran setsid */
        kill(job, SIGKILL);               /* and itself, in case it had not */
    }
    while (waitpid(job, &st, 0) < 0 && errno == EINTR) { }
    keeper_exit_as(st);
}

void hl_exec_plat_keeper_signals(void)
{
    /* Nothing but SIGKILL may end the keeper while its job runs: a signal
     * to the broker's process group (a Ctrl-C, a closed terminal) would
     * leave the job without anyone to end it. Faults keep their default.
     * The broker forked this process with every signal blocked, so one sent
     * before this point is pending and is discarded by SIG_IGN. */
    struct rlimit core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &core);
    for (int s = 1; s < 32; s++) {
        if (s == SIGKILL || s == SIGSTOP || s == SIGCHLD || s == SIGSEGV ||
            s == SIGBUS || s == SIGILL || s == SIGFPE || s == SIGTRAP ||
            s == SIGABRT || s == SIGSYS) continue;
        signal(s, SIG_IGN);
    }
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

void hl_exec_plat_keeper_run(pid_t broker, pid_t job, int kill_r)
{
    int kq = kqueue();
    if (kq >= 0) {
        struct kevent ev[3];
        EV_SET(&ev[0], (uintptr_t)job, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
        EV_SET(&ev[1], (uintptr_t)kill_r, EVFILT_READ, EV_ADD, 0, 0, NULL);
        EV_SET(&ev[2], (uintptr_t)broker, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
        for (int i = 0; i < 3; i++) {
            if (kevent(kq, &ev[i], 1, NULL, 0, NULL) == 0) continue;
            if (errno == ESRCH) keeper_end(job, i != 0);   /* already gone */
            if (i == 2) continue;         /* the kill pipe's EOF says it too */
            close(kq);
            kq = -1;
            break;
        }
    }
    if (getppid() != broker) keeper_end(job, 1);   /* the broker went first */
    for (;;) {
        if (kq >= 0) {
            struct kevent got;
            int n = kevent(kq, NULL, 0, &got, 1, NULL);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { close(kq); kq = -1; continue; }
            if (n == 0) continue;
            if (got.filter == EVFILT_PROC && got.ident == (uintptr_t)job) keeper_end(job, 0);
            keeper_end(job, 1);           /* the kill pipe, or the broker gone */
        }
        /* No kqueue: poll the pipe, and look at the job and the broker. */
        struct pollfd pf = { kill_r, POLLIN, 0 };
        if (poll(&pf, 1, 20) > 0) keeper_end(job, 1);
        int st;
        pid_t w = waitpid(job, &st, WNOHANG);
        if (w == job) keeper_exit_as(st);
        if (getppid() != broker) keeper_end(job, 1);
    }
}

int hl_exec_plat_child_death(void) { return 0; }

int hl_exec_plat_child_confine(const HlExecBrokerCmd *cmd, unsigned flags)
{
    (void)cmd; (void)flags;   /* the broker's profile is inherited */
    return 0;
}

int hl_exec_plat_child_filter(const HlExecBrokerCmd *cmd, unsigned flags)
{
    (void)cmd; (void)flags;
    return 0;
}

void hl_exec_plat_child_exec(const HlExecBrokerCmd *cmd, int bfd,
                             char *const argv[], char *const envp[])
{
    (void)bfd;
    execve(cmd->real, argv, envp);
}

#else
typedef int hl_exec_broker_macos_empty;   /* ISO C: no empty translation unit */
#endif /* HL_EXEC_BROKER_SUPPORTED && __APPLE__ */
