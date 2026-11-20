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

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAGIC 0x54534554u   /* "TEST" */

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

#endif /* HL_CHAN_SUPPORTED */

UTEST_MAIN()
