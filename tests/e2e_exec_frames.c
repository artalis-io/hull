/*
 * e2e_exec_frames.c - the command broker's admission, driven by raw frames
 * (docs/design/exec_broker.md §5.2; run by tests/e2e_exec_broker.sh).
 *
 * The server's own checks are a fast path: a compromised app process can
 * write any frame to the broker. So this program spawns the broker itself
 * (`hull __hull_exec_broker` on fd 3), sends it a POLICY, and then RUN
 * frames the binding would never send, checking the broker refuses each:
 *   1. a request id already in use              -> BUSY
 *   2. a 9th call in flight                     -> BUSY
 *   3. past a command's concurrency             -> BUSY
 *   4. past the buffered-output cap, held by an unread 16 MB reply -> BUSY
 *   5. arguments the slot does not admit        -> REFUSED
 *   6. a second POLICY                          -> the broker exits
 *
 * Usage: e2e_exec_frames <path to hull> [degraded]
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "hull/cap/exec.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAGIC   0x43455848u
#define POLICY  1u
#define READY   2u
#define RUN     3u
#define DONE    5u
#define BUSY    6u
#define REFUSED 7u

static const char *g_hull;
static unsigned g_flags;
static int g_fail;

#define CHECK(c, ...) do { if (c) printf("  PASS: " __VA_ARGS__); \
                           else { printf("  FAIL: " __VA_ARGS__); g_fail++; } \
                           printf("\n"); } while (0)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static int write_all(int fd, const void *b, size_t n)
{
    const uint8_t *p = b;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int send_frame(int fd, uint32_t type, uint32_t id, const void *a, size_t alen,
                      const void *b, size_t blen)
{
    uint8_t h[16];
    put32(h, MAGIC); put32(h + 4, type); put32(h + 8, id);
    put32(h + 12, (uint32_t)(alen + blen));
    if (write_all(fd, h, 16) || (alen && write_all(fd, a, alen)) ||
        (blen && write_all(fd, b, blen))) return -1;
    return 0;
}

/* One frame within @p ms: 1 (type / id set, payload in *buf), 0 timeout,
 * -1 EOF. */
static int read_frame(int fd, uint32_t *type, uint32_t *id, uint8_t **buf,
                      size_t *len, int ms)
{
    uint8_t h[16];
    size_t got = 0;
    while (got < 16) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, ms) <= 0) return 0;
        ssize_t r = read(fd, h + got, 16 - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    *type = get32(h + 4);
    *id = get32(h + 8);
    *len = get32(h + 12);
    *buf = malloc(*len + 1);
    got = 0;
    while (got < *len) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 5000) <= 0) return 0;
        ssize_t r = read(fd, *buf + got, *len - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    (*buf)[*len] = 0;
    return 1;
}

/* A policy of one command. */
static HlExecBuf policy_one(const char *name, const char *path,
                            const char *const *fixed, int nfixed, int slot,
                            int64_t stdout_max, int64_t conc)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    hl_exec_b_cmd(&b, name, strlen(name), path, strlen(path), 60000, stdout_max,
                  -1, conc, 0);
    for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    size_t m = hl_exec_b_count_begin(&b);
    for (int i = 0; i < nfixed; i++) hl_exec_b_fixed(&b, fixed[i], strlen(fixed[i]));
    if (slot) {
        hl_exec_b_slot(&b, 0, -1, -1, 0, -1, -1, NULL, 0, NULL, 0, "a-z", 3);
        hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    }
    hl_exec_b_count_end(&b, m, (size_t)nfixed + (slot ? 1 : 0));
    if (hl_exec_b_finish(&b) != 0) { fprintf(stderr, "policy: %s\n", b.err); exit(2); }
    return b;
}

typedef struct { int fd; pid_t pid; } Broker;

static Broker broker_start(const HlExecBuf *pol)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(2); }
    pid_t pid = fork();
    if (pid == 0) {
        /* sv[0] may itself be fd 3: close it before the dup lands there. */
        if (sv[0] != 3) close(sv[0]);
        int s = sv[1] == 3 ? 3 : dup2(sv[1], 3);
        if (s != 3) _exit(126);
        if (sv[1] != 3) close(sv[1]);
        execl(g_hull, g_hull, "__hull_exec_broker", (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    uint8_t f4[4];
    put32(f4, g_flags);
    send_frame(sv[0], POLICY, 0, f4, 4, pol->data, pol->len);
    uint32_t t, id; uint8_t *buf; size_t len;
    int r = read_frame(sv[0], &t, &id, &buf, &len, 5000);
    if (r != 1 || t != READY) {
        fprintf(stderr, "broker did not get READY (%d, type %u): %s\n", r, r == 1 ? t : 0,
                r == 1 ? (char *)buf : "");
        exit(2);
    }
    free(buf);
    return (Broker){ sv[0], pid };
}

static void broker_end(Broker *b)
{
    close(b->fd);
    int st;
    for (int i = 0; i < 50 && waitpid(b->pid, &st, WNOHANG) == 0; i++) usleep(20000);
    kill(b->pid, SIGKILL);
    waitpid(b->pid, &st, 0);
}

static void run(int fd, uint32_t id, const char *name, const char *arg)
{
    uint8_t p[256];
    size_t nl = strlen(name), off = 0;
    p[off++] = (uint8_t)nl; p[off++] = 0;
    memcpy(p + off, name, nl); off += nl;
    p[off++] = arg ? 1 : 0; p[off++] = 0;
    if (arg) {
        put32(p + off, (uint32_t)strlen(arg)); off += 4;
        memcpy(p + off, arg, strlen(arg)); off += strlen(arg);
    }
    send_frame(fd, RUN, id, p, off, NULL, 0);
}

/* The first frame for @p want_id within @p ms (others skipped). */
static int expect(int fd, uint32_t want_id, uint32_t *type, char *msg, size_t msz, int ms)
{
    for (;;) {
        uint32_t t, id; uint8_t *buf; size_t len;
        int r = read_frame(fd, &t, &id, &buf, &len, ms);
        if (r != 1) return r;
        if (id == want_id) {
            *type = t;
            snprintf(msg, msz, "%s", t == DONE ? "" : (char *)buf);
            free(buf);
            return 1;
        }
        free(buf);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <hull> [degraded]\n", argv[0]); return 2; }
    g_hull = argv[1];
    if (argc > 2 && strcmp(argv[2], "degraded") == 0) g_flags = 1;
    signal(SIGPIPE, SIG_IGN);
    const char *thirty[] = { "30" };
    uint32_t t;
    char msg[512];

    /* 1. a request id already in use */
    HlExecBuf p = policy_one("sl", "/usr/bin/sleep", thirty, 1, 0, -1, 8);
    Broker b = broker_start(&p);
    run(b.fd, 7, "sl", NULL);
    run(b.fd, 7, "sl", NULL);
    int r = expect(b.fd, 7, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == BUSY && strstr(msg, "already in use"),
          "a request id in use is refused (%s)", msg);
    broker_end(&b);

    /* 2. a 9th call in flight */
    b = broker_start(&p);
    for (uint32_t id = 1; id <= HL_EXEC_MAX_INFLIGHT; id++) run(b.fd, id, "sl", NULL);
    run(b.fd, 99, "sl", NULL);
    r = expect(b.fd, 99, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == BUSY && strstr(msg, "too many calls in flight"),
          "a 9th call in flight is refused (%s)", msg);
    broker_end(&b);
    hl_exec_buf_free(&p);

    /* 3. past a command's concurrency */
    p = policy_one("sl", "/usr/bin/sleep", thirty, 1, 0, -1, 2);
    b = broker_start(&p);
    run(b.fd, 1, "sl", NULL);
    run(b.fd, 2, "sl", NULL);
    run(b.fd, 3, "sl", NULL);
    r = expect(b.fd, 3, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == BUSY && strstr(msg, "concurrency"),
          "past the command's concurrency is refused (%s)", msg);
    broker_end(&b);
    hl_exec_buf_free(&p);

    /* 4. past the buffered-output cap: a 16 MB reply this side does not read
     * holds its reservation, so a second call would exceed 32 MiB. */
    /* stdout_max at its 16 MiB ceiling + the 16 KiB stderr default: two
     * calls reserve 33,587,200 bytes, past HL_EXEC_MAX_BUFFERED (32 MiB). */
    const char *zero[] = { "-c", "16777216", "/dev/zero" };
    p = policy_one("z", "/usr/bin/head", zero, 3, 0, 16777216, 8);
    b = broker_start(&p);
    run(b.fd, 1, "z", NULL);
    usleep(1500000);                         /* it ran; its reply is stuck */
    run(b.fd, 2, "z", NULL);
    r = expect(b.fd, 2, &t, msg, sizeof msg, 10000);
    CHECK(r == 1 && t == BUSY && strstr(msg, "too much output"),
          "an unread reply holds its reservation (%s)", msg);
    broker_end(&b);
    hl_exec_buf_free(&p);

    /* 5. arguments the slot does not admit (no binding in the way) */
    p = policy_one("e", "/bin/echo", NULL, 0, 1, -1, 2);
    b = broker_start(&p);
    run(b.fd, 1, "e", "--host=evil");
    r = expect(b.fd, 1, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == REFUSED && strstr(msg, "starts with '-'"),
          "the broker matches again (%s)", msg);
    run(b.fd, 2, "e", "ok");
    r = expect(b.fd, 2, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == DONE, "an admitted value runs");

    /* 6. a second POLICY ends the broker */
    uint8_t f4[4];
    put32(f4, g_flags);
    send_frame(b.fd, POLICY, 0, f4, 4, p.data, p.len);
    uint32_t id; uint8_t *buf; size_t len;
    r = read_frame(b.fd, &t, &id, &buf, &len, 3000);
    CHECK(r == -1, "a second POLICY ends the broker");
    broker_end(&b);
    hl_exec_buf_free(&p);

    printf("e2e_exec_frames: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
