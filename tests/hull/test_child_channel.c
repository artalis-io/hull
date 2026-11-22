/*
 * test_child_channel.c - shared/child_channel.c: frames under deadlines
 * (a wrong magic, an oversized payload, a peer gone mid-frame, a quiet
 * peer), and a spawned child on fd 3 with a filtered environment.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/shared/child_channel.h"

#ifdef HL_CHAN_SUPPORTED

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/resource.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAGIC 0x54534554u   /* "TEST" */

/* A cosmo APE running on Windows has no channel (hl_chan_host_ok). */
#define NEED_HOST()                                                        \
    do { if (!hl_chan_host_ok()) UTEST_SKIP("no channel child on Windows"); } while (0)

static void pair(int sv[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) abort();
    hl_chan_set_nonblock(sv[0]);
    hl_chan_set_nonblock(sv[1]);
    hl_chan_set_nosigpipe(sv[0]);
    hl_chan_set_nosigpipe(sv[1]);
}

/* For a child: only the parent's end is non-blocking (the child's fd 3
 * shares its file description, as gpu_helper's does). */
static void child_pair(int sv[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) abort();
    (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    hl_chan_set_nonblock(sv[0]);
    hl_chan_set_nosigpipe(sv[0]);
}

UTEST(child_channel, frame_round_trip)
{
    NEED_HOST();
    int sv[2];
    pair(sv);
    uint64_t dl = hl_chan_mono_ms() + 2000;
    ASSERT_EQ(0, hl_chan_write_frame(sv[0], MAGIC, 7, 42, "ab", 2, "cde", 3, dl));
    ASSERT_EQ(0, hl_chan_write_frame(sv[0], MAGIC, 8, 43, NULL, 0, NULL, 0, dl));
    HlChanFrame r = { .magic = MAGIC, .max = 16 };
    ASSERT_EQ(HL_CHAN_DONE, hl_chan_frame_read(sv[1], &r, dl));
    ASSERT_EQ(7u, r.type);
    ASSERT_EQ(42u, r.id);
    ASSERT_EQ(5u, r.len);
    ASSERT_EQ(0, memcmp(r.buf, "abcde", 5));
    hl_chan_frame_reset(&r);
    ASSERT_EQ(MAGIC, r.magic);              /* kept across frames */
    ASSERT_EQ(HL_CHAN_DONE, hl_chan_frame_read(sv[1], &r, dl));
    ASSERT_EQ(8u, r.type);
    ASSERT_EQ(0u, r.len);
    hl_chan_frame_reset(&r);
    close(sv[0]);
    close(sv[1]);
}

UTEST(child_channel, frame_errors)
{
    NEED_HOST();
    int sv[2];
    uint64_t dl = hl_chan_mono_ms() + 2000;
    HlChanFrame r = { .magic = MAGIC, .max = 4 };

    pair(sv);                               /* another protocol's magic */
    ASSERT_EQ(0, hl_chan_write_frame(sv[0], MAGIC + 1, 1, 1, "x", 1, NULL, 0, dl));
    ASSERT_EQ(HL_CHAN_ERROR, hl_chan_frame_read(sv[1], &r, dl));
    hl_chan_frame_reset(&r);
    close(sv[0]); close(sv[1]);

    pair(sv);                               /* past the reader's max */
    ASSERT_EQ(0, hl_chan_write_frame(sv[0], MAGIC, 1, 1, "12345", 5, NULL, 0, dl));
    ASSERT_EQ(HL_CHAN_OVERSIZE, hl_chan_frame_read(sv[1], &r, dl));
    hl_chan_frame_reset(&r);
    close(sv[0]); close(sv[1]);

    pair(sv);                               /* the peer goes mid-frame */
    uint8_t hdr[HL_CHAN_HDR];
    hl_chan_put32(hdr, MAGIC);
    hl_chan_put32(hdr + 4, 1);
    hl_chan_put32(hdr + 8, 1);
    hl_chan_put32(hdr + 12, 4);
    ASSERT_EQ(0, hl_chan_write_all(sv[0], hdr, sizeof hdr, dl));
    ASSERT_EQ(0, hl_chan_write_all(sv[0], "ab", 2, dl));
    close(sv[0]);
    ASSERT_EQ(HL_CHAN_EOF, hl_chan_frame_read(sv[1], &r, dl));
    hl_chan_frame_reset(&r);
    close(sv[1]);

    pair(sv);                               /* a quiet peer: the deadline */
    uint64_t t0 = hl_chan_mono_ms();
    ASSERT_EQ(HL_CHAN_MORE, hl_chan_frame_read(sv[1], &r, t0 + 50));
    ASSERT_GE(hl_chan_mono_ms() - t0, 40u);
    hl_chan_frame_reset(&r);
    close(sv[0]); close(sv[1]);
}

static int keep_only_kept(const char *kv)
{
    return strncmp(kv, "HULL_CHAN_KEPT=", 15) == 0;
}

UTEST(child_channel, spawn_on_fd3)
{
    NEED_HOST();
    int sv[2];
    child_pair(sv);
    setenv("HULL_CHAN_KEPT", "yes", 1);
    setenv("HULL_CHAN_SECRET", "no", 1);
    char a0[] = "/bin/sh", a1[] = "-c",
         a2[] = "printf '%s|%s' \"$HULL_CHAN_KEPT\" \"$HULL_CHAN_SECRET\" >&3; cat <&3 >&3";
    char *argv[] = { a0, a1, a2, NULL };
    pid_t pid = hl_chan_spawn("/bin/sh", argv, sv[1], keep_only_kept);
    close(sv[1]);
    ASSERT_GT(pid, 0);
    uint64_t dl = hl_chan_mono_ms() + 5000;

    /* The environment it got: the kept name only. */
    char env[16] = { 0 };
    size_t got = 0;
    while (got < 4 && hl_chan_mono_ms() < dl) {
        ssize_t n = recv(sv[0], env + got, 4 - got, 0);
        if (n > 0) got += (size_t)n;
        else usleep(1000);
    }
    ASSERT_STREQ("yes|", env);

    /* fd 3 is the socket: a frame comes back unchanged. */
    ASSERT_EQ(0, hl_chan_write_frame(sv[0], MAGIC, 3, 9, "ping", 4, NULL, 0, dl));
    HlChanFrame r = { .magic = MAGIC, .max = 16 };
    ASSERT_EQ(HL_CHAN_DONE, hl_chan_frame_read(sv[0], &r, dl));
    ASSERT_EQ(9u, r.id);
    ASSERT_EQ(0, memcmp(r.buf, "ping", 4));
    hl_chan_frame_reset(&r);

    /* Closing our end ends it; it is reaped. */
    close(sv[0]);
    int st = -1;
    ASSERT_EQ(1, hl_chan_reap(pid, &st, 5000, NULL));
    ASSERT_TRUE(WIFEXITED(st));
    unsetenv("HULL_CHAN_KEPT");
    unsetenv("HULL_CHAN_SECRET");
}

UTEST(child_channel, reap_a_running_child_times_out)
{
    NEED_HOST();
    int sv[2];
    child_pair(sv);
    char a0[] = "/bin/sh", a1[] = "-c", a2[] = "cat <&3";
    char *argv[] = { a0, a1, a2, NULL };
    pid_t pid = hl_chan_spawn("/bin/sh", argv, sv[1], NULL);
    close(sv[1]);
    ASSERT_GT(pid, 0);
    int st;
    ASSERT_EQ(0, hl_chan_reap(pid, &st, 50, NULL));   /* still there */
    kill(pid, SIGKILL);
    ASSERT_EQ(1, hl_chan_reap(pid, &st, 5000, NULL));
    ASSERT_TRUE(WIFSIGNALED(st));
    close(sv[0]);
}

/* A descriptor the parent holds without close-on-exec does not reach the
 * child: on Linux posix_spawn passed it on (audit 49), so a build plugin could
 * use one the build host had open. */
UTEST(child_channel_audit49, spawn_closes_inherited_fds)
{
    NEED_HOST();
    int sv[2];
    child_pair(sv);
    int p[2];
    ASSERT_EQ(0, pipe(p));
    int leak = fcntl(p[1], F_DUPFD, 20);      /* high, and inheritable */
    ASSERT_GE(leak, 20);
    ASSERT_EQ(0, fcntl(leak, F_SETFD, 0));
    char script[96];
    snprintf(script, sizeof script, "if [ -e /dev/fd/%d ]; then exit 3; fi; exit 0", leak);
    char a0[] = "/bin/sh", a1[] = "-c";
    char *argv[] = { a0, a1, script, NULL };
    pid_t pid = hl_chan_spawn("/bin/sh", argv, sv[1], NULL);
    close(sv[1]);
    ASSERT_GT(pid, 0);
    int st = -1;
    ASSERT_EQ(1, hl_chan_reap(pid, &st, 5000, NULL));
    ASSERT_TRUE(WIFEXITED(st));
    EXPECT_EQ(0, WEXITSTATUS(st));            /* 3: the descriptor was there */
    close(leak); close(p[0]); close(p[1]); close(sv[0]);
}

/* Spawn /bin/sh -c @p script with @p chan as fd 3; its exit status, or -1. */
static int run_sh(const char *script, int chan)
{
    char a0[] = "/bin/sh", a1[] = "-c";
    char *argv[] = { a0, a1, (char *)(uintptr_t)script, NULL };
    pid_t pid = hl_chan_spawn("/bin/sh", argv, chan, NULL);
    if (pid <= 0) return -1;
    int st = -1;
    if (hl_chan_reap(pid, &st, 5000, NULL) != 1 || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

/* A descriptor above 4096 does not reach the child when close_range is not
 * there (a kernel before 5.9, a filter answering ENOSYS): the fallback loop
 * stopped at 4096 under a soft limit of 65536 or more, and the descriptors
 * past it were inherited (audit 50). Both fallbacks: /proc/self/fd, and the
 * loop to the limit. */
UTEST(child_channel_audit50, high_fd_closed_without_close_range)
{
    NEED_HOST();
    struct rlimit rl, saved;
    ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &saved));
    rl = saved;
    rlim_t want = 70000;
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want) want = rl.rlim_max;
    if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur < want) {
        rl.rlim_cur = want;
        (void)setrlimit(RLIMIT_NOFILE, &rl);
    }
    ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &rl));
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur <= 5000) {
        printf("  (descriptor limit %llu: no descriptor above 4096, skipped)\n",
               (unsigned long long)rl.rlim_cur);
        return;
    }
    int sv[2];
    child_pair(sv);
    int p[2];
    ASSERT_EQ(0, pipe(p));
    int high = 5000;
    ASSERT_EQ(high, dup2(p[1], high));        /* inheritable: dup2 clears CLOEXEC */
    char script[96];
    snprintf(script, sizeof script, "if [ -e /dev/fd/%d ]; then exit 3; fi; exit 0", high);
    for (int mode = 1; mode <= 2; mode++) {
        hl_chan_test_force_close_fallback(mode);
        EXPECT_EQ(0, run_sh(script, sv[1]));  /* 3: the descriptor was there */
    }
    hl_chan_test_force_close_fallback(0);
    close(high); close(p[0]); close(p[1]); close(sv[0]); close(sv[1]);
    (void)setrlimit(RLIMIT_NOFILE, &saved);
}

/* A binary that cannot be executed is reported as posix_spawn reports it:
 * -1 with errno set (audit 50: the fork path returned the pid of a child that
 * only _exit(127)ed). */
UTEST(child_channel_audit50, exec_failure_reported)
{
    NEED_HOST();
    int sv[2];
    child_pair(sv);
    char a0[] = "/nonexistent/hull-audit50";
    char *argv[] = { a0, NULL };
    errno = 0;
    pid_t pid = hl_chan_spawn("/nonexistent/hull-audit50", argv, sv[1], NULL);
    int err = errno;
    if (pid > 0) { int st; (void)hl_chan_reap(pid, &st, 5000, NULL); }
    EXPECT_EQ(-1, (int)pid);
    EXPECT_EQ(ENOENT, err);
    close(sv[0]); close(sv[1]);
}

/* A process started with stdin closed gets its socketpair on fd 0: the
 * /dev/null set-up of 0-2 replaced the channel before it was moved to fd 3,
 * and the child talked to /dev/null (audit 50). */
UTEST(child_channel_audit50, low_fd_channel_survives)
{
    NEED_HOST();
    int saved0 = fcntl(0, F_DUPFD_CLOEXEC, 10);
    ASSERT_GE(saved0, 10);
    close(0);
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    int child_end = sv[0] == 0 ? sv[0] : sv[1];
    int our_end = child_end == sv[0] ? sv[1] : sv[0];
    int st = (child_end == 0) ? run_sh("printf ok >&3", child_end) : -2;
    char buf[4] = { 0 };
    ssize_t n = st == 0 ? recv(our_end, buf, 2, MSG_DONTWAIT) : -1;
    close(sv[0]); close(sv[1]);
    ASSERT_EQ(0, dup2(saved0, 0));
    close(saved0);
    ASSERT_EQ(0, st);
    EXPECT_EQ(2, (int)n);
    EXPECT_STREQ("ok", buf);
}

#endif /* HL_CHAN_SUPPORTED */

UTEST_MAIN()
