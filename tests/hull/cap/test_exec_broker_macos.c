/*
 * test_exec_broker_macos.c - the command broker's macOS backend
 * (cap/exec_broker_macos.c, hull/cap/exec_platform.h,
 * docs/design/exec_broker.md §6.4).
 *
 * The Seatbelt cases apply the profile in a forked child (a sandbox is for
 * the life of the process), playing the broker, and check what the kernel
 * then refuses to the commands it starts. Elsewhere one case says skipped.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/exec_platform.h"

#if defined(HL_EXEC_BROKER_SUPPORTED) && defined(__APPLE__)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

typedef struct {
    HlExecBuf       b;
    HlExecBrokerCmd cmds[4];
    int             n;
} Pol;

typedef struct {
    const char *name, *path;
    int         unix_sockets;
    const char *fs_read, *fs_write;     /* one path each, or NULL */
} Spec;

/* Build and read back a policy of @p n commands; resolve each binary as
 * the broker's cmd_load does. 0 / -1. */
static int pol_make(Pol *p, const Spec *specs, int n)
{
    memset(p, 0, sizeof *p);
    hl_exec_b_init(&p->b, NULL);
    for (int i = 0; i < n; i++) {
        const Spec *s = &specs[i];
        hl_exec_b_cmd(&p->b, s->name, strlen(s->name), s->path, strlen(s->path),
                      -1, -1, -1, -1, s->unix_sockets, -1, NULL, 0);
        hl_exec_b_count_end(&p->b, hl_exec_b_count_begin(&p->b), 0);       /* env */
        size_t m = hl_exec_b_count_begin(&p->b);
        if (s->fs_read) hl_exec_b_str(&p->b, s->fs_read, strlen(s->fs_read));
        hl_exec_b_count_end(&p->b, m, s->fs_read ? 1 : 0);
        m = hl_exec_b_count_begin(&p->b);
        if (s->fs_write) hl_exec_b_str(&p->b, s->fs_write, strlen(s->fs_write));
        hl_exec_b_count_end(&p->b, m, s->fs_write ? 1 : 0);
        hl_exec_b_count_end(&p->b, hl_exec_b_count_begin(&p->b), 0);       /* argv */
    }
    if (hl_exec_b_finish(&p->b) != 0) { fprintf(stderr, "policy: %s\n", p->b.err); return -1; }
    p->n = n;
    for (int i = 0; i < n; i++) {
        HlExecBrokerCmd *c = &p->cmds[i];
        if (hl_exec_policy_at(p->b.data, p->b.len, i, &c->c) != 0) return -1;
        if (!realpath(c->c.path, c->real)) return -1;
        snprintf(c->interp, sizeof c->interp, "/usr/lib/dyld");
        c->plat = -1;
    }
    return 0;
}

static void pol_free(Pol *p) { hl_exec_buf_free(&p->b); }

static void self_path(char *out, size_t n)
{
    char raw[PATH_MAX];
    uint32_t sz = sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) != 0 || !realpath(raw, out))
        snprintf(out, n, "/nonexistent");
}

/* fork + exec @p path quietly; its exit status, -1000 - errno when the
 * fork itself failed, or -1 on a signal. */
static int run_quiet(const char *path, char *const argv[])
{
    pid_t p = fork();
    if (p < 0) return -1000 - errno;
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execv(path, argv);
        _exit(errno == EPERM ? 77 : 78);
    }
    int st;
    if (waitpid(p, &st, 0) != p) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Run @p fn in a forked child, its return its exit status. */
static int in_child(int (*fn)(void *), void *arg)
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) _exit(fn(arg));
    int st;
    if (waitpid(p, &st, 0) != p) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

/* ── The binary check ──────────────────────────────────────────────── */

static int check_file(const char *path, char *why, size_t wsz, char *interp)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -2;
    int rc = hl_exec_plat_binary_check(fd, path, interp, PATH_MAX, why, wsz);
    close(fd);
    return rc;
}

static void write_bytes(const char *path, const void *b, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (fd < 0) return;
    if (write(fd, b, n) != (ssize_t)n) { /* the case then fails */ }
    close(fd);
}

UTEST(exec_macos, a_system_binary_is_a_mach_o_with_dyld)
{
    char why[256] = "", interp[PATH_MAX];
    ASSERT_EQ(0, check_file("/bin/ls", why, sizeof why, interp));
    EXPECT_STREQ("/usr/lib/dyld", interp);
}

UTEST(exec_macos, scripts_and_lookalikes_are_refused)
{
    char dir[] = "/tmp/hull-exec-mac-XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    char path[PATH_MAX], why[256], interp[PATH_MAX];
    snprintf(path, sizeof path, "%s/f", dir);

    write_bytes(path, "#!/bin/sh\necho x\n", 17);
    ASSERT_EQ(-1, check_file(path, why, sizeof why, interp));
    EXPECT_TRUE(strstr(why, "a script would bring its interpreter") != NULL);

    /* A Java class file starts with the universal binary's magic. */
    static const unsigned char klass[] = { 0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 0x34 };
    write_bytes(path, klass, sizeof klass);
    EXPECT_EQ(-1, check_file(path, why, sizeof why, interp));

    /* 32-bit Mach-O */
    static const unsigned char m32[] = { 0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 1 };
    write_bytes(path, m32, sizeof m32);
    EXPECT_EQ(-1, check_file(path, why, sizeof why, interp));
    EXPECT_TRUE(strstr(why, "64-bit") != NULL);

    write_bytes(path, "abc", 3);
    EXPECT_EQ(-1, check_file(path, why, sizeof why, interp));
    unlink(path);
    rmdir(dir);
}

/* ── The profile ───────────────────────────────────────────────────── */

static const char *param(char **kv, const char *key)
{
    for (char **q = kv; q[0]; q += 2)
        if (strcmp(q[0], key) == 0) return q[1];
    return NULL;
}

UTEST(exec_macos, the_profile_grants_each_binary_and_no_bare_fork)
{
    Pol p;
    Spec s[] = { { "echo", "/bin/echo", 0, "/etc/hosts", "/dev/null" },
                 { "ls", "/bin/ls", 0, "/nonexistent/x", NULL } };
    ASSERT_EQ(0, pol_make(&p, s, 2));
    char *prof = NULL, **kv = NULL;
    ASSERT_EQ(0, hl_exec_macos_profile(p.cmds, p.n, "/opt/hull/bin/hull", &prof, &kv));
    EXPECT_TRUE(strstr(prof, "(deny default)") != NULL);
    EXPECT_TRUE(strstr(prof, "(allow process-fork (process-path (param \"SELF\")))") != NULL);
    EXPECT_TRUE(strstr(prof, "(allow process-fork)") == NULL);
    EXPECT_TRUE(strstr(prof, "network") == NULL);
    EXPECT_TRUE(strstr(prof, "system-socket") == NULL);
    EXPECT_STREQ("/opt/hull/bin/hull", param(kv, "SELF"));
    EXPECT_STREQ("/bin/echo", param(kv, "B0"));
    EXPECT_STREQ("/bin/ls", param(kv, "B1"));
    /* fs paths are resolved (Seatbelt matches the real path) */
    EXPECT_STREQ("/private/etc/hosts", param(kv, "R0_0"));
    EXPECT_STREQ("/dev/null", param(kv, "W0_0"));
    /* a path that does not exist is skipped */
    EXPECT_TRUE(param(kv, "R1_0") == NULL);
    hl_exec_macos_profile_free(prof, kv);
    pol_free(&p);
}

UTEST(exec_macos, unix_sockets_opens_af_unix_only)
{
    Pol p;
    Spec s[] = { { "echo", "/bin/echo", 1, NULL, NULL } };
    ASSERT_EQ(0, pol_make(&p, s, 1));
    char *prof = NULL, **kv = NULL;
    ASSERT_EQ(0, hl_exec_macos_profile(p.cmds, p.n, "/x", &prof, &kv));
    EXPECT_TRUE(strstr(prof, "(socket-domain AF_UNIX)") != NULL);
    EXPECT_TRUE(strstr(prof, "(remote unix-socket)") != NULL);
    EXPECT_TRUE(strstr(prof, "network-inbound") == NULL);
    EXPECT_TRUE(strstr(prof, "(remote ip") == NULL);
    hl_exec_macos_profile_free(prof, kv);
    pol_free(&p);
}

UTEST(exec_macos, a_command_that_is_this_binary_is_refused)
{
    Pol p;
    Spec s[] = { { "echo", "/bin/echo", 0, NULL, NULL } };
    ASSERT_EQ(0, pol_make(&p, s, 1));
    char rep[512] = "";
    HlExecReport r = { rep, sizeof rep, 0 };
    EXPECT_EQ(-1, hl_exec_plat_lock(p.cmds, p.n, 0, "/bin/echo", &r));
    EXPECT_TRUE(strstr(rep, "this hull binary itself") != NULL);
    pol_free(&p);
}

/* ── The kernel's answers under the profile ────────────────────────── */

static int locked_broker(void *arg)
{
    Pol *p = arg;
    char self[PATH_MAX], rep[1024] = "";
    self_path(self, sizeof self);
    HlExecReport r = { rep, sizeof rep, 0 };
    if (hl_exec_plat_lock(p->cmds, p->n, 0, self, &r) != 0) {
        fprintf(stderr, "lock: %s\n", rep);
        return 10;
    }
    if (!strstr(rep, "exec: limit: no parent-death signal")) return 11;
    /* Several commands are routed, one broker each (the router tests below):
     * a broker never reports a shared sandbox any more. */
    if (strstr(rep, "ONE shared sandbox")) return 12;
    /* The broker forks, and a declared binary runs. */
    char *echo[] = { "/bin/echo", "x", NULL };
    if (run_quiet("/bin/echo", echo) != 0) return 20;
    /* An undeclared one does not. */
    char *cat[] = { "/bin/cat", "/etc/hosts", NULL };
    if (run_quiet("/bin/cat", cat) != 77) return 21;
    /* A declared one cannot fork: time(1) forks its command. */
    char *tm[] = { "/usr/bin/time", "/bin/echo", "x", NULL };
    if (run_quiet("/usr/bin/time", tm) == 0) return 22;
    /* A declared one cannot read the home directory. */
    const char *home = getenv("HOME");
    if (home && *home) {
        char hbuf[PATH_MAX];
        snprintf(hbuf, sizeof hbuf, "%s", home);
        char *ls[] = { "/bin/ls", hbuf, NULL };
        if (run_quiet("/bin/ls", ls) == 0) return 23;
    }
    /* No network for the broker either: Seatbelt refuses the connect
     * (EPERM), not the port (ECONNREFUSED). */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_in a = { 0 };
        a.sin_len = sizeof a;
        a.sin_family = AF_INET;
        a.sin_port = htons(9);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int rc = connect(s, (struct sockaddr *)&a, sizeof a), e = errno;
        close(s);
        if (rc == 0 || (e != EPERM && e != EACCES)) return 24;
    }
    return 0;
}

UTEST(exec_macos, the_profile_confines_what_the_broker_starts)
{
    Pol p;
    Spec s[] = { { "echo", "/bin/echo", 0, NULL, NULL },
                 { "time", "/usr/bin/time", 0, NULL, NULL },
                 { "ls", "/bin/ls", 0, NULL, NULL } };
    ASSERT_EQ(0, pol_make(&p, s, 3));
    EXPECT_EQ(0, in_child(locked_broker, &p));
    pol_free(&p);
}

static int no_sandbox(void *arg)
{
    Pol *p = arg;
    char rep[512] = "";
    HlExecReport r = { rep, sizeof rep, 0 };
    if (hl_exec_plat_lock(p->cmds, p->n, HL_EXEC_POLICY_NO_SANDBOX, "/x", &r) != 0)
        return 10;
    if (!strstr(rep, "exec: no sandbox")) return 11;
    char *cat[] = { "/bin/cat", "/etc/hosts", NULL };
    return run_quiet("/bin/cat", cat) == 0 ? 0 : 12;   /* nothing applied */
}

UTEST(exec_macos, no_sandbox_applies_nothing)
{
    Pol p;
    Spec s[] = { { "echo", "/bin/echo", 0, NULL, NULL } };
    ASSERT_EQ(0, pol_make(&p, s, 1));
    EXPECT_EQ(0, in_child(no_sandbox, &p));
    pol_free(&p);
}

/* ── The watches ───────────────────────────────────────────────────── */

UTEST(exec_macos, the_exit_watch_turns_readable_when_the_child_exits)
{
    pid_t p = fork();
    ASSERT_TRUE(p >= 0);
    if (p == 0) { usleep(100000); _exit(0); }
    int fd = hl_exec_plat_exit_watch(p);
    ASSERT_TRUE(fd >= 0);
    struct pollfd pf = { fd, POLLIN, 0 };
    EXPECT_EQ(1, poll(&pf, 1, 5000));
    close(fd);
    waitpid(p, NULL, 0);
    /* Gone already: no watch (the broker reaps on its tick). */
    EXPECT_EQ(-1, hl_exec_plat_exit_watch(p));
}

/* The broker's watch on the server: B (the "broker") watches A (the
 * "server"); A exits; B reports through a pipe that it saw it. */
UTEST(exec_macos, the_broker_sees_its_server_go)
{
    int pp[2];
    ASSERT_EQ(0, pipe(pp));
    pid_t a = fork();
    ASSERT_TRUE(a >= 0);
    if (a == 0) {
        int go[2];
        if (pipe(go) != 0) _exit(1);
        pid_t b = fork();
        if (b == 0) {
            close(go[1]);
            int w = -1;
            char res = 'x';
            if (hl_exec_plat_broker_init(&w) == 0 && w >= 0) {
                char c;
                if (read(go[0], &c, 1) < 0) { /* A said go */ }
                struct pollfd pf = { w, POLLIN, 0 };
                res = poll(&pf, 1, 5000) == 1 ? 'y' : 'n';
            }
            if (write(pp[1], &res, 1) < 0) { /* the case fails */ }
            _exit(0);
        }
        close(go[0]);
        usleep(100000);           /* B has its watch */
        _exit(0);                 /* closing go[1] tells B to wait */
    }
    close(pp[1]);
    waitpid(a, NULL, 0);
    char res = 0;
    struct pollfd pf = { pp[0], POLLIN, 0 };
    ASSERT_EQ(1, poll(&pf, 1, 10000));
    ASSERT_EQ(1, (int)read(pp[0], &res, 1));
    EXPECT_TRUE(res == 'y');
    close(pp[0]);
}

UTEST(exec_macos, the_child_steps_are_no_ops_here)
{
    HlExecBrokerCmd c;
    memset(&c, 0, sizeof c);
    c.plat = -1;
    EXPECT_EQ(0, hl_exec_plat_child_death());
    EXPECT_EQ(0, hl_exec_plat_child_confine(&c, 0));
    EXPECT_EQ(0, hl_exec_plat_child_filter(&c, 0));
    EXPECT_EQ(0, hl_exec_plat_leaf_check(-1, "/bin/ls", NULL, 0));
}

/* ── The keeper (exec_broker_macos_isolation.md §4.6) ──────────────── */

/* Fork a keeper (the test plays the broker): it forks a job that runs
 * behaviour @p how names (0: exit 7, 1: die by SIGTERM, 2: run until
 * killed), reports the job's pid on @p *pid_r, and waits in
 * hl_exec_plat_keeper_run. *kill_w is the call's kill pipe. */
static pid_t keeper_fork(int how, int *kill_w, int *pid_r)
{
    int kp[2], pp[2];
    if (pipe(kp) != 0 || pipe(pp) != 0) return -1;
    pid_t broker = getpid();
    pid_t k = fork();
    if (k == 0) {
        close(kp[1]);
        close(pp[0]);
        pid_t j = fork();
        if (j == 0) {
            close(kp[0]);
            switch (how) {
            case 0: usleep(50000); _exit(7);                     /* exits 7 */
            case 1: usleep(50000); raise(SIGTERM); _exit(1);     /* dies by SIGTERM */
            default: for (;;) pause();                           /* runs until killed */
            }
        }
        if (write(pp[1], &j, sizeof j) < 0) { /* the case fails */ }
        close(pp[1]);
        hl_exec_plat_keeper_run(broker, j, kp[0]);
    }
    close(kp[0]);
    close(pp[1]);
    *kill_w = kp[1];
    *pid_r = pp[0];
    return k;
}

static pid_t read_pid(int fd)
{
    pid_t j = -1;
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, 5000) != 1 || read(fd, &j, sizeof j) != (ssize_t)sizeof j) j = -1;
    close(fd);
    return j;
}

static int gone_within(pid_t p, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (kill(p, 0) != 0 && errno == ESRCH) return 1;
        usleep(10000);
    }
    return 0;
}

UTEST(exec_macos, the_keeper_exits_as_its_job_did)
{
    int kw, pr, st;
    pid_t k = keeper_fork(0, &kw, &pr);
    ASSERT_TRUE(k > 0);
    EXPECT_TRUE(read_pid(pr) > 0);
    ASSERT_EQ(k, waitpid(k, &st, 0));
    EXPECT_TRUE(WIFEXITED(st));
    EXPECT_EQ(7, WEXITSTATUS(st));
    close(kw);

    k = keeper_fork(1, &kw, &pr);
    ASSERT_TRUE(k > 0);
    EXPECT_TRUE(read_pid(pr) > 0);
    ASSERT_EQ(k, waitpid(k, &st, 0));
    EXPECT_TRUE(WIFSIGNALED(st));
    EXPECT_EQ(SIGTERM, WTERMSIG(st));
    close(kw);
}

UTEST(exec_macos, closing_the_kill_pipe_ends_the_job)
{
    int kw, pr, st;
    pid_t k = keeper_fork(2, &kw, &pr);
    ASSERT_TRUE(k > 0);
    pid_t j = read_pid(pr);
    ASSERT_TRUE(j > 0);
    usleep(50000);
    EXPECT_EQ(0, kill(j, 0));              /* it runs */
    close(kw);                             /* the broker ends the call */
    ASSERT_EQ(k, waitpid(k, &st, 0));
    EXPECT_TRUE(WIFSIGNALED(st));
    EXPECT_EQ(SIGKILL, WTERMSIG(st));
    EXPECT_TRUE(gone_within(j, 1000));     /* reaped by its keeper */
    kill(j, SIGKILL);                      /* never leave a job behind */
}

/* The broker killed with SIGKILL: B plays the broker, forks the keeper and
 * dies; the keeper sees it go and ends the job. */
UTEST(exec_macos, a_broker_killed_with_sigkill_takes_its_job)
{
    int pp[2];
    ASSERT_EQ(0, pipe(pp));
    pid_t b = fork();
    ASSERT_TRUE(b >= 0);
    if (b == 0) {
        close(pp[0]);
        int kw, pr;
        pid_t k = keeper_fork(2, &kw, &pr);
        pid_t j = k > 0 ? read_pid(pr) : -1;
        pid_t both[2] = { k, j };
        if (write(pp[1], both, sizeof both) < 0) { /* the case fails */ }
        for (;;) pause();                  /* kw stays open: only death closes it */
    }
    close(pp[1]);
    pid_t both[2] = { -1, -1 };
    struct pollfd pf = { pp[0], POLLIN, 0 };
    ASSERT_EQ(1, poll(&pf, 1, 5000));
    ASSERT_EQ((ssize_t)sizeof both, read(pp[0], both, sizeof both));
    close(pp[0]);
    ASSERT_TRUE(both[0] > 0 && both[1] > 0);
    usleep(50000);
    EXPECT_EQ(0, kill(both[1], 0));        /* the job runs */
    kill(b, SIGKILL);
    waitpid(b, NULL, 0);
    EXPECT_TRUE(gone_within(both[1], 2000));
    EXPECT_TRUE(gone_within(both[0], 2000));
    kill(both[1], SIGKILL);                /* never leave a job behind */
}

UTEST(exec_macos, the_keeper_ignores_a_group_signal)
{
    int kw, pr;
    pid_t k = keeper_fork(2, &kw, &pr);
    ASSERT_TRUE(k > 0);
    pid_t j = read_pid(pr);
    ASSERT_TRUE(j > 0);
    usleep(50000);
    kill(k, SIGTERM);
    kill(k, SIGHUP);
    kill(k, SIGINT);
    usleep(100000);
    EXPECT_EQ(0, kill(k, 0));
    EXPECT_EQ(0, kill(j, 0));
    close(kw);
    waitpid(k, NULL, 0);
    EXPECT_TRUE(gone_within(j, 1000));
    kill(j, SIGKILL);                      /* never leave a job behind */
}

/* ── The router (exec_broker_macos_isolation.md §4.2) ──────────────── */

UTEST(exec_macos, the_router_profile_has_no_exec_fork_or_file_rule)
{
    const char *p = hl_exec_macos_router_profile;
    EXPECT_TRUE(strstr(p, "(deny default)") != NULL);
    EXPECT_TRUE(strstr(p, "process-exec") == NULL);
    EXPECT_TRUE(strstr(p, "process-fork") == NULL);
    EXPECT_TRUE(strstr(p, "file-") == NULL);
    EXPECT_TRUE(strstr(p, "network") == NULL);
    EXPECT_EQ(1, hl_exec_plat_router());
}

/* In a child: lock as the router, then check it keeps its socket and its
 * child, and can open, fork and exec nothing. 0 = all as expected. */
static int locked_router(void *arg)
{
    (void)arg;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 10;
    pid_t kid = fork();
    if (kid < 0) return 11;
    if (kid == 0) { char c; if (read(sv[1], &c, 1) == 1) (void)write(sv[1], "y", 1); pause(); _exit(0); }
    char rep[512] = "";
    HlExecReport r = { rep, sizeof rep, 0 };
    if (hl_exec_plat_router_lock(&r) != 0) return 12;
    if (!strstr(rep, "exec: limit: no parent-death signal")) return 13;
    if (write(sv[0], "x", 1) != 1) return 14;                 /* its channel */
    struct pollfd pf = { sv[0], POLLIN, 0 };
    char c;
    if (poll(&pf, 1, 2000) != 1 || read(sv[0], &c, 1) != 1) return 15;
    errno = 0;
    if (open("/etc/hosts", O_RDONLY) >= 0 || errno != EPERM) return 16;
    if (open("/dev/null", O_RDWR) >= 0) return 17;
    pid_t f = fork();
    if (f == 0) _exit(0);
    if (f >= 0) return 18;
    char *av[] = { "/usr/bin/true", NULL };
    execv("/usr/bin/true", av);
    if (errno != EPERM) return 19;
    if (kill(kid, SIGKILL) != 0) return 20;                   /* its children */
    int st;
    if (waitpid(kid, &st, 0) != kid) return 21;
    if (kill(1, 0) == 0) return 22;                           /* nobody else */
    return 0;
}

UTEST(exec_macos, the_locked_router_keeps_its_channels_and_starts_nothing)
{
    EXPECT_EQ(0, in_child(locked_router, NULL));
}

#else

UTEST(exec_macos, skipped_not_macos)
{
    UTEST_SKIP("the macOS broker backend is not built on this host");
}

#endif

UTEST_MAIN()
