/*
 * exec_probe_macos.c - a declared command for tests/e2e_exec_broker_macos.sh:
 * it runs as the exec'd program under the broker's Seatbelt profile and
 * reports what that profile allows (docs/design/exec_broker.md §6.4).
 *
 *   exec_probe_macos probe HOME  one line per check: "<name>=<ok|errno name>"
 *   exec_probe_macos ready       prints HULL_EXEC_PROBE_READY, sleeps 30 s
 *   exec_probe_macos sleep       sleeps 30 s
 *   exec_probe_macos spew        4 MiB to stdout, 1 MiB to stderr
 *   exec_probe_macos unix        connects to a system AF_UNIX socket:
 *                                "unix=<ok|errno name>"
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char *res(long rc)
{
    if (rc >= 0) return "ok";
    switch (errno) {
    case EPERM:        return "EPERM";
    case EACCES:       return "EACCES";
    case ECONNREFUSED: return "ECONNREFUSED";
    case ENOENT:       return "ENOENT";
    default:           return "error";
    }
}

static void *thread_fn(void *a) { return a; }

static int probe(const char *home)
{
    /* process creation: fork, vfork's replacement, posix_spawn */
    pid_t p = fork();
    if (p == 0) _exit(0);
    if (p > 0) waitpid(p, NULL, 0);
    printf("fork=%s\n", res(p));
    char *argv[] = { "/bin/echo", "x", NULL };
    pid_t q;
    int rc = posix_spawn(&q, "/bin/echo", NULL, NULL, argv, environ);
    if (rc == 0) waitpid(q, NULL, 0);
    errno = rc;
    printf("posix_spawn=%s\n", res(rc == 0 ? 0 : -1));
    /* the network: Seatbelt refuses the connect (EPERM), not the port */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    long c = -1;
    if (s >= 0) {
        struct sockaddr_in a = { 0 };
        a.sin_len = sizeof a;
        a.sin_family = AF_INET;
        a.sin_port = htons(9);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        c = connect(s, (struct sockaddr *)&a, sizeof a);
        int e = errno;
        close(s);
        errno = e;
    }
    printf("inet=%s\n", res(c));
    /* the home directory */
    int fd = open(home, O_RDONLY | O_DIRECTORY);
    printf("home=%s\n", res(fd));
    if (fd >= 0) close(fd);
    /* a system file */
    fd = open("/etc/hosts", O_RDONLY);
    printf("etc=%s\n", res(fd));
    if (fd >= 0) close(fd);
    /* threads still work */
    pthread_t t;
    rc = pthread_create(&t, NULL, thread_fn, NULL);
    if (rc == 0) pthread_join(t, NULL);
    errno = rc;
    printf("thread=%s\n", res(rc == 0 ? 0 : -1));
    return 0;
}

static int unix_conn(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    long c = s;
    if (s >= 0) {
        struct sockaddr_un a = { 0 };
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof a.sun_path, "/private/var/run/mDNSResponder");
        c = connect(s, (struct sockaddr *)&a, sizeof a);
        int e = errno;
        close(s);
        errno = e;
    }
    printf("unix=%s\n", res(c));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (strcmp(argv[1], "probe") == 0 && argc > 2) return probe(argv[2]);
    if (strcmp(argv[1], "unix") == 0) return unix_conn();
    if (strcmp(argv[1], "ready") == 0) {
        printf("HULL_EXEC_PROBE_READY\n");
        fflush(stdout);
        sleep(30);
        return 0;
    }
    if (strcmp(argv[1], "sleep") == 0) { sleep(30); return 0; }
    if (strcmp(argv[1], "spew") == 0) {
        static char buf[65536];
        memset(buf, 'o', sizeof buf);
        for (int i = 0; i < 64; i++) fwrite(buf, 1, sizeof buf, stdout);
        memset(buf, 'e', sizeof buf);
        for (int i = 0; i < 16; i++) fwrite(buf, 1, sizeof buf, stderr);
        return 0;
    }
    return 2;
}
