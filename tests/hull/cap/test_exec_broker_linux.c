/*
 * test_exec_broker_linux.c - the command broker's Linux backend
 * (cap/exec_broker_linux.c, hull/cap/exec_platform.h,
 * docs/design/exec_broker.md §6.2 - §6.5).
 *
 * The confinement cases run in a forked child (NO_NEW_PRIVS, a Landlock
 * domain and a seccomp filter are for the life of the process). The whole
 * broker, end to end, is tests/e2e_exec_broker.sh. Elsewhere one case says
 * skipped.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "utest.h"
#include "hull/cap/exec_platform.h"

#if defined(HL_EXEC_BROKER_SUPPORTED) && defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

static int check_file(const char *path, char *why, size_t wsz, char *interp)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -2;
    int rc = hl_exec_plat_binary_check(fd, path, interp, PATH_MAX, why, wsz);
    close(fd);
    return rc;
}

static void write_bytes(const char *path, const void *b, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    if (fd < 0) return;
    if (write(fd, b, n) != (ssize_t)n) { /* the case then fails */ }
    close(fd);
}

/* Run @p fn in a forked child; its return is the exit status. */
static int in_child(int (*fn)(void *), void *arg)
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) _exit(fn(arg));
    int st;
    if (waitpid(p, &st, 0) != p) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static int landlock_abi(void)
{
    long abi = syscall(444 /* landlock_create_ruleset */, NULL, 0, 1u);
    return abi > 0 ? (int)abi : 0;
}

/* ── The binary checks ─────────────────────────────────────────────── */

UTEST(exec_linux, a_system_binary_is_an_elf_with_its_loader)
{
    char real[PATH_MAX], why[256] = "", interp[PATH_MAX];
    ASSERT_TRUE(realpath("/usr/bin/true", real) != NULL);
    ASSERT_EQ(0, check_file(real, why, sizeof why, interp));
    /* Dynamically linked on every distribution CI runs: a loader path. */
    EXPECT_TRUE(interp[0] == '/');
}

UTEST(exec_linux, scripts_and_foreign_files_are_refused)
{
    char dir[] = "/tmp/hull-exec-linux-XXXXXX";
    ASSERT_TRUE(mkdtemp(dir) != NULL);
    char path[PATH_MAX], why[256], interp[PATH_MAX];
    snprintf(path, sizeof path, "%s/f", dir);

    write_bytes(path, "#!/bin/sh\necho x\n", 17);
    EXPECT_EQ(-1, check_file(path, why, sizeof why, interp));
    EXPECT_TRUE(strstr(why, "a script would bring its interpreter") != NULL);

    /* a 32-bit ELF */
    unsigned char e32[64] = { 0x7f, 'E', 'L', 'F', 1, 1, 1 };
    write_bytes(path, e32, sizeof e32);
    EXPECT_EQ(-1, check_file(path, why, sizeof why, interp));
    EXPECT_TRUE(strstr(why, "64-bit") != NULL);

    /* a 64-bit header with a nonsense program header table */
    unsigned char e64[64] = { 0x7f, 'E', 'L', 'F', 2, 1, 1 };
    e64[54] = 1;   /* e_phentsize = 1 */
    write_bytes(path, e64, sizeof e64);
    EXPECT_EQ(-1, check_file(path, why, sizeof why, interp));
    EXPECT_TRUE(strstr(why, "malformed") != NULL);

    unlink(path);
    rmdir(dir);
}

UTEST(exec_linux, an_ordinary_binary_carries_no_file_capabilities)
{
    char real[PATH_MAX], why[256] = "";
    ASSERT_TRUE(realpath("/usr/bin/true", real) != NULL);
    int fd = open(real, O_RDONLY | O_CLOEXEC);
    ASSERT_TRUE(fd >= 0);
    EXPECT_EQ(0, hl_exec_plat_leaf_check(fd, real, why, sizeof why));
    close(fd);
}

/* ── The child's steps ─────────────────────────────────────────────── */

static int filtered_child(void *arg)
{
    (void)arg;
    HlExecBrokerCmd c;
    memset(&c, 0, sizeof c);
    c.plat = -1;                                      /* no Landlock ruleset */
    if (hl_exec_plat_child_confine(&c, 0) != 0) return 10;
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) return 11;
    if (hl_exec_plat_child_filter(&c, 0) != 0) return 12;
    pid_t p = fork();
    if (p == 0) _exit(0);
    if (p >= 0 || errno != EPERM) return 13;          /* no process creation */
    if (socket(AF_UNIX, SOCK_STREAM, 0) >= 0 || errno != EACCES) return 14;
    if (prctl(PR_SET_PDEATHSIG, 0, 0, 0, 0) == 0) return 15;
    if (setuid(getuid()) == 0 || errno != EPERM) return 16;
    return 0;
}

UTEST(exec_linux, the_child_filter_refuses_fork_sockets_and_credentials)
{
    EXPECT_EQ(0, in_child(filtered_child, NULL));
}

static int unix_child(void *arg)
{
    (void)arg;
    HlExecBrokerCmd c;
    memset(&c, 0, sizeof c);
    c.plat = -1;
    c.c.flags = HL_EXEC_CMD_UNIX_SOCKETS;
    if (hl_exec_plat_child_confine(&c, 0) != 0) return 10;
    if (hl_exec_plat_child_filter(&c, 0) != 0) return 12;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return 13;
    close(s);
    return 0;
}

UTEST(exec_linux, unix_sockets_keeps_af_unix)
{
    EXPECT_EQ(0, in_child(unix_child, NULL));
}

static int death_child(void *arg)
{
    (void)arg;
    if (hl_exec_plat_child_death() != 0) return 10;
    int sig = 0;
    if (prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) != 0 || sig != SIGKILL) return 11;
    return 0;
}

UTEST(exec_linux, the_child_dies_with_the_broker)
{
    EXPECT_EQ(0, in_child(death_child, NULL));
}

static int no_sandbox_child(void *arg)
{
    (void)arg;
    HlExecBrokerCmd c;
    memset(&c, 0, sizeof c);
    c.plat = -1;
    if (hl_exec_plat_child_filter(&c, HL_EXEC_POLICY_NO_SANDBOX) != 0) return 10;
    pid_t p = fork();
    if (p < 0) return 11;                             /* nothing applied */
    if (p == 0) _exit(0);
    waitpid(p, NULL, 0);
    return 0;
}

UTEST(exec_linux, no_sandbox_adds_no_filter)
{
    EXPECT_EQ(0, in_child(no_sandbox_child, NULL));
}

/* ── The broker's lock ─────────────────────────────────────────────── */

typedef struct {
    HlExecBuf       b;
    HlExecBrokerCmd cmd;
} One;

static int one_make(One *o)
{
    memset(o, 0, sizeof *o);
    hl_exec_b_init(&o->b, NULL);
    hl_exec_b_cmd(&o->b, "t", 1, "/usr/bin/true", 13, -1, -1, -1, -1, 0, -1, NULL, 0);
    for (int k = 0; k < 4; k++)
        hl_exec_b_count_end(&o->b, hl_exec_b_count_begin(&o->b), 0);
    if (hl_exec_b_finish(&o->b) != 0) return -1;
    if (hl_exec_policy_at(o->b.data, o->b.len, 0, &o->cmd.c) != 0) return -1;
    if (!realpath("/usr/bin/true", o->cmd.real)) return -1;
    o->cmd.plat = -1;
    return 0;
}

static int locked_broker(void *arg)
{
    One *o = arg;
    char rep[2048] = "";
    HlExecReport r = { rep, sizeof rep, 0 };
    if (hl_exec_plat_lock(&o->cmd, 1, HL_EXEC_POLICY_DEGRADED_OK, o->cmd.real, &r) != 0)
        return 10;
    if (o->cmd.plat < 0) return 11;                   /* the child's ruleset */
    /* The broker reads only the trees the commands may: not /root, not
     * the user's home. */
    const char *home = getenv("HOME");
    if (home && *home && strncmp(home, "/usr", 4) != 0 &&
        open(home, O_RDONLY | O_DIRECTORY | O_CLOEXEC) >= 0) return 12;
    if (open("/usr/bin/true", O_RDONLY | O_CLOEXEC) < 0) return 13;
    /* The ceiling: no ptrace, no namespaces, AF_INET refused. */
    if (socket(AF_INET, SOCK_STREAM, 0) >= 0 || errno != EACCES) return 14;
    if (unshare(CLONE_NEWUSER) == 0) return 15;
    return 0;
}

UTEST(exec_linux, the_lock_confines_the_broker)
{
    if (landlock_abi() < 1) UTEST_SKIP("no Landlock on this kernel");
    One o;
    ASSERT_EQ(0, one_make(&o));
    EXPECT_EQ(0, in_child(locked_broker, &o));
    hl_exec_buf_free(&o.b);
}

static int lock_refused(void *arg)
{
    One *o = arg;
    char rep[2048] = "";
    HlExecReport r = { rep, sizeof rep, 0 };
    int rc = hl_exec_plat_lock(&o->cmd, 1, 0, o->cmd.real, &r);
    if (landlock_abi() >= 6) return rc == 0 ? 0 : 10;
    /* An older kernel: refused without DEGRADED_OK, naming the flag. */
    return rc != 0 && strstr(rep, "--allow-degraded-sandbox") ? 0 : 11;
}

UTEST(exec_linux, a_missing_landlock_right_refuses_without_the_flag)
{
    One o;
    ASSERT_EQ(0, one_make(&o));
    EXPECT_EQ(0, in_child(lock_refused, &o));
    hl_exec_buf_free(&o.b);
}

/* ── The exit watch ────────────────────────────────────────────────── */

UTEST(exec_linux, the_exit_watch_turns_readable_when_the_child_exits)
{
    pid_t p = fork();
    ASSERT_TRUE(p >= 0);
    if (p == 0) { usleep(100000); _exit(0); }
    int fd = hl_exec_plat_exit_watch(p);
    if (fd < 0) {
        waitpid(p, NULL, 0);
        UTEST_SKIP("no pidfd_open on this kernel (the broker reaps on a tick)");
    }
    struct pollfd pf = { fd, POLLIN, 0 };
    EXPECT_EQ(1, poll(&pf, 1, 5000));
    close(fd);
    waitpid(p, NULL, 0);
}

UTEST(exec_linux, the_broker_init_sets_its_death_signal)
{
    int w = 7;
    pid_t p = fork();
    ASSERT_TRUE(p >= 0);
    if (p == 0) {
        int sig = 0;
        if (hl_exec_plat_broker_init(&w) != 0 || w != -1) _exit(1);
        if (prctl(PR_GET_PDEATHSIG, &sig, 0, 0, 0) != 0 || sig != SIGKILL) _exit(2);
        _exit(0);
    }
    int st;
    ASSERT_EQ(p, waitpid(p, &st, 0));
    EXPECT_TRUE(WIFEXITED(st));
    EXPECT_EQ(0, WEXITSTATUS(st));
}

#else

UTEST(exec_linux, skipped_not_linux)
{
    UTEST_SKIP("the Linux broker backend is not built on this host");
}

#endif

UTEST_MAIN()
