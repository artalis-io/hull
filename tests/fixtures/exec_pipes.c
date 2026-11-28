/*
 * exec_pipes.c - a declared command for tests/e2e_exec_stream.sh: pipe
 * patterns exec.run and exec.stream must handle. It never forks (a
 * command cannot).
 *
 *   exec_pipes pipes <ms>   write 300000 'E' to stderr (past one pipe
 *                           window and the kernel pipe buffer), wait <ms>,
 *                           then read all of stdin and print
 *                           "in=<bytes> fnv=<FNV-1a 32>" on stdout; exit 7
 *   exec_pipes early        read 10 bytes of stdin, close it, print "ok"
 *   exec_pipes tick         print "one", wait 1 s, print "two"
 *   exec_pipes echo         copy each stdin line to stdout at once
 *   exec_pipes flood        write 'x' to stdout until killed
 *   exec_pipes silent       print its pid, then wait 60 s
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void nap(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    char buf[65536];
    if (strcmp(mode, "pipes") == 0) {
        memset(buf, 'E', sizeof buf);
        size_t left = 300000;
        while (left) {
            size_t n = left < sizeof buf ? left : sizeof buf;
            if (write_all(2, buf, n) != 0) return 2;
            left -= n;
        }
        nap(argc > 2 ? atol(argv[2]) : 0);
        unsigned long long total = 0;
        unsigned h = 2166136261u;
        for (;;) {
            ssize_t r = read(0, buf, sizeof buf);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            for (ssize_t i = 0; i < r; i++) h = (h ^ (unsigned char)buf[i]) * 16777619u;
            total += (unsigned long long)r;
        }
        char out[64];
        int n = snprintf(out, sizeof out, "in=%llu fnv=%08x\n", total, h);
        write_all(1, out, (size_t)n);
        return 7;
    }
    if (strcmp(mode, "early") == 0) {
        ssize_t r = read(0, buf, 10);
        (void)r;
        close(0);
        write_all(1, "ok\n", 3);
        return 0;
    }
    if (strcmp(mode, "tick") == 0) {
        write_all(1, "one\n", 4);
        nap(1000);
        write_all(1, "two\n", 4);
        return 0;
    }
    if (strcmp(mode, "echo") == 0) {
        for (;;) {
            ssize_t r = read(0, buf, sizeof buf);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) return 0;
            if (write_all(1, buf, (size_t)r) != 0) return 2;
        }
    }
    if (strcmp(mode, "flood") == 0) {
        memset(buf, 'x', sizeof buf);
        for (;;) if (write_all(1, buf, sizeof buf) != 0) return 0;
    }
    if (strcmp(mode, "silent") == 0) {
        char out[32];
        int n = snprintf(out, sizeof out, "%ld\n", (long)getpid());
        write_all(1, out, (size_t)n);
        nap(60000);
        return 0;
    }
    return 64;
}
