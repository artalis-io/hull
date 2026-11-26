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
 *   7. stdin past the command's stdin_max      -> REFUSED
 *   8. a full POLICY head before a near-limit policy -> read, then judged
 *   9. two commands (macOS: a router, one broker each): KILL of a running
 *      call frees its slot exactly once - the command runs again, its
 *      concurrency is still held by a live call, nothing comes for the
 *      killed id, and the other command runs
 *
 * Usage: e2e_exec_frames <path to hull> [degraded]
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "hull/cap/exec.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#define SLEEP_BIN "/bin/sleep"          /* macOS has no /usr/bin/sleep */
#else
#define SLEEP_BIN "/usr/bin/sleep"
#endif

#define MAGIC   0x43455848u
#define POLICY  1u
#define READY   2u
#define RUN     3u
#define KILL    4u
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
                            int64_t stdout_max, int64_t conc, int64_t stdin_max)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    hl_exec_b_cmd(&b, name, strlen(name), path, strlen(path), 60000, stdout_max,
                  -1, conc, 0, stdin_max, NULL, 0);
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

/* A policy of two commands: "sl" (sleep 30, concurrency 1) and "e" (echo). */
static HlExecBuf policy_two(void)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    hl_exec_b_cmd(&b, "sl", 2, SLEEP_BIN, strlen(SLEEP_BIN), 60000, -1, -1, 1, 0, -1,
                  NULL, 0);
    for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    size_t m = hl_exec_b_count_begin(&b);
    hl_exec_b_fixed(&b, "30", 2);
    hl_exec_b_count_end(&b, m, 1);
    hl_exec_b_cmd(&b, "e", 1, "/bin/echo", 9, 60000, -1, -1, 2, 0, -1, NULL, 0);
    for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    m = hl_exec_b_count_begin(&b);
    hl_exec_b_fixed(&b, "hi", 2);
    hl_exec_b_count_end(&b, m, 1);
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
    /* u32 flags, then the two roots of the child manifests and the
     * protected database (u16 length + bytes each; none here). */
    uint8_t f4[10] = { 0 };
    put32(f4, g_flags);
    send_frame(sv[0], POLICY, 0, f4, 10, pol->data, pol->len);
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

/* RUN with @p arg (NULL = none) and @p in on stdin (NULL = none). */
static void run_in(int fd, uint32_t id, const char *name, const char *arg,
                   const char *in)
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
    size_t il = in ? strlen(in) : 0;
    put32(p + off, (uint32_t)il); off += 4;
    send_frame(fd, RUN, id, p, off, in, il);
}

static void run(int fd, uint32_t id, const char *name, const char *arg)
{
    run_in(fd, id, name, arg, NULL);
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
    HlExecBuf p = policy_one("sl", SLEEP_BIN, thirty, 1, 0, -1, 8, -1);
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
    p = policy_one("sl", SLEEP_BIN, thirty, 1, 0, -1, 2, -1);
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
    p = policy_one("z", "/usr/bin/head", zero, 3, 0, 16777216, 8, -1);
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
    p = policy_one("e", "/bin/echo", NULL, 0, 1, -1, 2, -1);
    b = broker_start(&p);
    run(b.fd, 1, "e", "--host=evil");
    r = expect(b.fd, 1, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == REFUSED && strstr(msg, "starts with '-'"),
          "the broker matches again (%s)", msg);
    run(b.fd, 2, "e", "ok");
    r = expect(b.fd, 2, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == DONE, "an admitted value runs");
    run_in(b.fd, 3, "e", "ok", "x");
    r = expect(b.fd, 3, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == REFUSED && strstr(msg, "takes no stdin"),
          "stdin to a command with no stdin_max is refused (%s)", msg);

    /* 6. a second POLICY ends the broker */
    uint8_t f4[10] = { 0 };
    put32(f4, g_flags);
    send_frame(b.fd, POLICY, 0, f4, 10, p.data, p.len);
    uint32_t id; uint8_t *buf; size_t len;
    r = read_frame(b.fd, &t, &id, &buf, &len, 3000);
    CHECK(r == -1, "a second POLICY ends the broker");
    broker_end(&b);
    hl_exec_buf_free(&p);

    /* 7. stdin past the command's stdin_max (no binding in the way) */
    p = policy_one("c", "/bin/cat", NULL, 0, 0, -1, 2, 4);
    b = broker_start(&p);
    run_in(b.fd, 1, "c", NULL, "hello");
    r = expect(b.fd, 1, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == REFUSED && strstr(msg, "longer than its stdin_max"),
          "stdin past stdin_max is refused by the broker (%s)", msg);
    run_in(b.fd, 2, "c", NULL, "abcd");
    r = expect(b.fd, 2, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == DONE, "stdin within stdin_max runs");
    broker_end(&b);
    hl_exec_buf_free(&p);

    /* 8. exec_frames_audit53.full_head: a POLICY whose head carries both
     * roots and HL_EXEC_MAX_PROTECT database paths of PATH_MAX - 1 bytes, in
     * front of a policy near HL_EXEC_POLICY_MAX_BYTES, is READ and judged
     * (refused here: the policy bytes are not a policy). The broker's read
     * limit had stayed at the policy + 4, so it dropped the frame and exited
     * without a word. */
    {
        size_t one = 2 + (PATH_MAX - 1);
        size_t hn = 4 + 2 + 2 + 2 + (size_t)HL_EXEC_MAX_PROTECT * one;
        size_t pn = HL_EXEC_POLICY_MAX_BYTES - 64;
        uint8_t *head = calloc(1, hn), *junk = calloc(1, pn);
        if (!head || !junk) { fprintf(stderr, "out of memory\n"); return 2; }
        put32(head, g_flags);
        size_t at = 4 + 2 + 2;               /* two empty roots */
        head[at++] = HL_EXEC_MAX_PROTECT; head[at++] = 0;
        for (int i = 0; i < HL_EXEC_MAX_PROTECT; i++) {
            size_t l = PATH_MAX - 1;
            head[at++] = (uint8_t)l; head[at++] = (uint8_t)(l >> 8);
            head[at] = '/';
            memset(head + at + 1, 'a' + i, l - 1);
            at += l;
        }
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }
        pid_t pid = fork();
        if (pid == 0) {
            if (sv[0] != 3) close(sv[0]);
            if ((sv[1] == 3 ? 3 : dup2(sv[1], 3)) != 3) _exit(126);
            if (sv[1] != 3) close(sv[1]);
            execl(g_hull, g_hull, "__hull_exec_broker", (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        (void)send_frame(sv[0], POLICY, 0, head, at, junk, pn);
        uint32_t id8; uint8_t *buf8 = NULL; size_t len8;
        r = read_frame(sv[0], &t, &id8, &buf8, &len8, 5000);
        CHECK(r == 1 && t == REFUSED,
              "a POLICY with a full head and a near-limit policy is read and judged "
              "(r %d, type %u)", r, r == 1 ? t : 0);
        free(buf8);
        Broker b8 = { sv[0], pid };
        broker_end(&b8);
        free(head);
        free(junk);
    }

    /* 9. exec_frames_router.kill_frees_once: KILL of a running call (a
     * cancelled request, a deadline) through two commands - on macOS the
     * router forwards it to the command's broker (#1218). */
    p = policy_two();
    b = broker_start(&p);
    run(b.fd, 1, "sl", NULL);
    r = expect(b.fd, 1, &t, msg, sizeof msg, 500);
    CHECK(r == 0, "a running call has no reply yet (r %d)", r);
    send_frame(b.fd, KILL, 1, NULL, 0, NULL, 0);
    usleep(300000);                          /* its broker reaps the child */
    run(b.fd, 2, "sl", NULL);
    r = expect(b.fd, 2, &t, msg, sizeof msg, 800);
    CHECK(r == 0, "after a KILL the command runs again (r %d, %s)", r, r == 1 ? msg : "");
    run(b.fd, 3, "sl", NULL);
    r = expect(b.fd, 3, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == BUSY && strstr(msg, "concurrency"),
          "...and its concurrency is held by the live call (%s)", msg);
    run(b.fd, 4, "e", NULL);
    r = expect(b.fd, 4, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == DONE, "the other command runs");
    send_frame(b.fd, KILL, 2, NULL, 0, NULL, 0);
    usleep(300000);
    run(b.fd, 5, "e", NULL);
    r = expect(b.fd, 5, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == DONE, "after both KILLs a call still gets its own reply");
    r = expect(b.fd, 1, &t, msg, sizeof msg, 300);
    CHECK(r == 0, "nothing ever comes for a killed id (r %d)", r);
    broker_end(&b);
    hl_exec_buf_free(&p);

    printf("e2e_exec_frames: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
