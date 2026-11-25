/*
 * exec_probe.c - a declared command for tests/e2e_exec_broker.sh: it runs
 * as the exec'd program under the broker's child sandbox and reports what
 * that sandbox allows (docs/design/exec_broker.md §5.3, §7).
 *
 *   exec_probe probe   one line per check: "<name>=<ok|errno name>"
 *   exec_probe ready   requires PR_GET_PDEATHSIG == SIGKILL, prints
 *                      HULL_EXEC_PROBE_READY, then sleeps 30 s
 *   exec_probe sleep   sleeps 30 s
 *   exec_probe spew    writes 4 MiB to stdout and 1 MiB to stderr
 *   exec_probe unix    makes an AF_UNIX socket: "unix=<ok|errno name>"
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <grp.h>

static const char *res(long rc)
{
    if (rc >= 0) return "ok";
    switch (errno) {
    case EPERM:  return "EPERM";
    case EACCES: return "EACCES";
    case ENOSYS: return "ENOSYS";
    default:     return strerrorname_np(errno) ? strerrorname_np(errno) : "error";
    }
}

static void *thread_fn(void *p) { return p; }

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "probe";
    if (strcmp(mode, "probe") == 0) {
        /* Process creation: refused (a grandchild would escape PDEATHSIG). */
        pid_t p = fork();
        if (p == 0) _exit(0);
        if (p > 0) waitpid(p, NULL, 0);
        printf("fork=%s\n", res(p));
#ifdef SYS_clone3
        long c3 = syscall(SYS_clone3, NULL, 0);
        printf("clone3=%s\n", res(c3));
#endif
        /* Threads: allowed. */
        pthread_t t;
        int pr = pthread_create(&t, NULL, thread_fn, NULL);
        if (pr == 0) pthread_join(t, NULL);
        else errno = pr;
        printf("thread=%s\n", res(pr == 0 ? 0 : -1));
        /* No network socket, ever. */
        int s = socket(AF_INET, SOCK_STREAM, 0);
        printf("inet=%s\n", res(s));
        if (s >= 0) close(s);
        /* Nothing that clears or replaces PDEATHSIG. */
        printf("set_pdeathsig=%s\n", res(prctl(PR_SET_PDEATHSIG, 0)));
        uid_t u = getuid();
        printf("setresuid=%s\n", res(setresuid(u, u, u)));
        printf("setgroups=%s\n", res(setgroups(0, NULL)));
        int sig = 0;
        prctl(PR_GET_PDEATHSIG, &sig);
        printf("pdeathsig=%d\n", sig);
        return 0;
    }
    if (strcmp(mode, "unix") == 0) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        printf("unix=%s\n", res(s));
        return 0;
    }
    if (strcmp(mode, "ready") == 0) {
        int sig = 0;
        if (prctl(PR_GET_PDEATHSIG, &sig) != 0 || sig != SIGKILL) {
            printf("pdeathsig=%d (expected %d)\n", sig, SIGKILL);
            return 3;
        }
        printf("HULL_EXEC_PROBE_READY\n");
        fflush(stdout);
        sleep(30);
        return 0;
    }
    if (strcmp(mode, "sleep") == 0) { sleep(30); return 0; }
    if (strcmp(mode, "spew") == 0) {
        static char buf[65536];
        memset(buf, 'x', sizeof buf);
        for (int i = 0; i < 64; i++) fwrite(buf, 1, sizeof buf, stdout);
        for (int i = 0; i < 16; i++) fwrite(buf, 1, sizeof buf, stderr);
        return 0;
    }
    return 2;
}
