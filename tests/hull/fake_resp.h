/*
 * fake_resp.h - a minimal RESP3 server on a loopback port, for the runtime
 * tests that drive the kv bindings end to end (audit 41). One thread, one
 * connection at a time: HELLO gets a server map, SET "+OK", GET a one-byte
 * bulk, anything else ":1". Each read is taken as one command, which holds
 * for the bindings: they send a command and wait for its reply.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_TEST_FAKE_RESP_H
#define HL_TEST_FAKE_RESP_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int       lfd;
    int       port;
    pthread_t th;
    volatile int stop;
} HlFakeResp;

static inline void fake_resp_serve(int fd)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n <= 0) return;
        buf[n] = '\0';
        const char *r = ":1\r\n";
        if (strstr(buf, "HELLO"))
            r = "%1\r\n$6\r\nserver\r\n$5\r\nredis\r\n";
        else if (strstr(buf, "SET"))
            r = "+OK\r\n";
        else if (strstr(buf, "GET"))
            r = "$1\r\nx\r\n";
        if (write(fd, r, strlen(r)) < 0) return;
    }
}

static inline void *fake_resp_main(void *ud)
{
    HlFakeResp *f = ud;
    while (!f->stop) {
        int fd = accept(f->lfd, NULL, NULL);
        if (fd < 0) break;
        fake_resp_serve(fd);
        close(fd);
    }
    return NULL;
}

/* 0 with f->port set, -1 when no socket could be made. */
static inline int fake_resp_start(HlFakeResp *f)
{
    memset(f, 0, sizeof *f);
    f->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->lfd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(f->lfd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(f->lfd, 8) != 0 ||
        getsockname(f->lfd, (struct sockaddr *)&a, &al) != 0) {
        close(f->lfd);
        return -1;
    }
    f->port = ntohs(a.sin_port);
    if (pthread_create(&f->th, NULL, fake_resp_main, f) != 0) {
        close(f->lfd);
        return -1;
    }
    return 0;
}

/* Every client connection must be closed first (the thread serves one at a
 * time). A dummy connection wakes the accept. */
static inline void fake_resp_stop(HlFakeResp *f)
{
    f->stop = 1;
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c >= 0) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)f->port);
        (void)connect(c, (struct sockaddr *)&a, sizeof a);
        close(c);
    }
    pthread_join(f->th, NULL);
    close(f->lfd);
}

#endif /* HL_TEST_FAKE_RESP_H */
