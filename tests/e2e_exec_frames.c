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
 *  10. macOS router: the killed call's broker dies first (the KILL's write
 *      fails) while another command's call is live, and a RUN follows in the
 *      same read - the live call completes, the new call gets its own reply,
 *      the router takes more calls, the lost command is refused (review of
 *      #1218: the killed slot stayed queued and freed the next call)
 *  11. macOS router: one command's broker stops reading (SIGSTOP) with a
 *      RUN larger than the socket buffer queued for it - another command's
 *      call still completes on time, and once the broker reads again (after
 *      the 5 s the old blocking write allowed) its call completes too, not
 *      "the command broker is gone"; a KILL of a call whose RUN never went
 *      out yields no reply and frees its slot (#1234)
 *  12. streams (docs/design/exec_stream.md), through the router on macOS and
 *      the one broker on Linux: a stream RUN to a non-stream command, an open
 *      stdin without a stream, exec.run of a stream command, the credit
 *      window both ways (output stops at one window, credit lets exactly that
 *      much more through, credit past the window ends the call), stdin
 *      without an open stdin or past its credit, the total cap, the timeout
 *      (read or not), KILLed unread streams freeing their slots, output before
 *      the end, a stdin round trip and EOF, and the throughput of 200 MiB
 *      through the window (macOS also without the router)
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
#define CHUNK   9u
#define CREDIT 10u
#define STDIN  11u
#define STDIN_CREDIT 12u
#define RUN_STREAM 1u
#define RUN_STDIN_OPEN 2u
#define WINDOW  (256u * 1024u)

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

#ifdef __APPLE__
/* Commands "sl" (sleep 30, concurrency 1) and "s1" (sleep 1, concurrency 4). */
static HlExecBuf policy_sl_s1(void)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    static const char *const names[] = { "sl", "s1" }, *const args[] = { "30", "1" };
    static const int conc[] = { 1, 4 };
    for (int i = 0; i < 2; i++) {
        hl_exec_b_cmd(&b, names[i], 2, SLEEP_BIN, strlen(SLEEP_BIN), 60000, -1, -1,
                      conc[i], 0, -1, NULL, 0);
        for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
        size_t m = hl_exec_b_count_begin(&b);
        hl_exec_b_fixed(&b, args[i], strlen(args[i]));
        hl_exec_b_count_end(&b, m, 1);
    }
    if (hl_exec_b_finish(&b) != 0) { fprintf(stderr, "policy: %s\n", b.err); exit(2); }
    return b;
}

/* Commands "c" (cat, stdin up to 1 MiB, stdout 2 MiB, concurrency 4) and
 * "s1" (sleep 1, concurrency 4), in that order. */
static HlExecBuf policy_cat_s1(void)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    hl_exec_b_cmd(&b, "c", 1, "/bin/cat", 8, 60000, 2 * 1024 * 1024, -1, 4, 0,
                  1024 * 1024, NULL, 0);
    for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    hl_exec_b_cmd(&b, "s1", 2, SLEEP_BIN, strlen(SLEEP_BIN), 60000, -1, -1, 4, 0, -1,
                  NULL, 0);
    for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    size_t m = hl_exec_b_count_begin(&b);
    hl_exec_b_fixed(&b, "1", 1);
    hl_exec_b_count_end(&b, m, 1);
    if (hl_exec_b_finish(&b) != 0) { fprintf(stderr, "policy: %s\n", b.err); exit(2); }
    return b;
}

/* The children of @p pid (pgrep -P), up to @p max; their count. */
static int children_of(pid_t pid, pid_t *out, int max)
{
    char cmd[64];
    snprintf(cmd, sizeof cmd, "pgrep -P %d", (int)pid);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    int n = 0, v;
    while (n < max && fscanf(f, "%d", &v) == 1) out[n++] = (pid_t)v;
    pclose(f);
    return n;
}
#endif

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

/* RUN with @p arg (NULL = none), @p in on stdin (NULL = none) and RUN
 * flags @p flags. */
static void run_flags(int fd, uint32_t id, const char *name, const char *arg,
                      const char *in, unsigned flags)
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
    uint8_t fl = (uint8_t)flags;
    uint8_t *all = malloc(off + il + 1);
    memcpy(all, p, off);
    if (il) memcpy(all + off, in, il);
    all[off + il] = fl;
    send_frame(fd, RUN, id, all, off + il + 1, NULL, 0);
    free(all);
}

static void run_in(int fd, uint32_t id, const char *name, const char *arg,
                   const char *in)
{
    run_flags(fd, id, name, arg, in, 0);
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

/* ── Streams (docs/design/exec_stream.md) ──────────────────────────── */

/* One stream command: no slots, its fixed argv. */
static void stream_cmd(HlExecBuf *b, const char *name, const char *path,
                       const char *const *fixed, int nfixed, int64_t timeout,
                       int64_t out_max, int64_t in_max)
{
    hl_exec_b_cmd_stream(b, name, strlen(name), path, strlen(path), timeout, out_max, -1,
                         4, 0, in_max, NULL, 0);
    for (int k = 0; k < 3; k++) hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
    size_t m = hl_exec_b_count_begin(b);
    for (int i = 0; i < nfixed; i++) hl_exec_b_fixed(b, fixed[i], strlen(fixed[i]));
    hl_exec_b_count_end(b, m, (size_t)nfixed);
}

/* The stream suite's commands: "y" yes (no limits), "yt" yes (1 s), "yc"
 * yes (stdout_max 100000), "b" bash reading one line, "c" cat (stdin up to
 * 1 MiB), and "e", echo, which is not a stream command. */
static HlExecBuf policy_streams(void)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    stream_cmd(&b, "y", "/usr/bin/yes", NULL, 0, 0, -1, -1);
    stream_cmd(&b, "yt", "/usr/bin/yes", NULL, 0, 1000, -1, -1);
    stream_cmd(&b, "yc", "/usr/bin/yes", NULL, 0, 10000, 100000, -1);
    static const char *const bash_argv[] = { "-c", "echo a; read x; echo \"got $x\"" };
    stream_cmd(&b, "b", "/bin/bash", bash_argv, 2, 10000, -1, 1024 * 1024);
    stream_cmd(&b, "c", "/bin/cat", NULL, 0, 30000, -1, 1024 * 1024);
    hl_exec_b_cmd(&b, "e", 1, "/bin/echo", 9, 60000, -1, -1, 2, 0, -1, NULL, 0);
    for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    size_t m = hl_exec_b_count_begin(&b);
    hl_exec_b_fixed(&b, "hi", 2);
    hl_exec_b_count_end(&b, m, 1);
    if (hl_exec_b_finish(&b) != 0) { fprintf(stderr, "policy: %s\n", b.err); exit(2); }
    return b;
}

/* A policy of "y" alone (macOS: no router). */
static HlExecBuf policy_yes_only(void)
{
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    stream_cmd(&b, "y", "/usr/bin/yes", NULL, 0, 0, -1, -1);
    if (hl_exec_b_finish(&b) != 0) { fprintf(stderr, "policy: %s\n", b.err); exit(2); }
    return b;
}

static void credit(int fd, uint32_t id, uint8_t pipe, uint32_t bytes)
{
    uint8_t p[5];
    p[0] = pipe;
    put32(p + 1, bytes);
    send_frame(fd, CREDIT, id, p, 5, NULL, 0);
}

static void stdin_send(int fd, uint32_t id, uint8_t flags, const char *data, size_t n)
{
    send_frame(fd, STDIN, id, &flags, 1, data, n);
}

/* What came for call @p id until its last frame or @p ms of quiet. */
typedef struct {
    size_t   bytes[3];          /* CHUNK bytes per pipe (1, 2) */
    char     text[256];         /* the first stdout bytes */
    size_t   text_len;
    uint32_t stdin_credit;      /* STDIN_CREDIT bytes */
    int      stdin_closed;
    uint32_t last;              /* DONE / REFUSED / BUSY, 0 = none */
    uint8_t  done[64];          /* the DONE (or REFUSED) payload */
    size_t   done_len;
    int      chunks_after_last; /* frames for the id after its last frame */
} Got;

/* Read frames for @p id: return on its last frame, after @p ms of quiet,
 * or once @p until stdout bytes came (0 = no such stop). Credit is given
 * back for every CHUNK when @p give. */
static void collect(int fd, uint32_t id, Got *g, int ms, size_t until, int give)
{
    for (;;) {
        uint32_t t, rid; uint8_t *buf; size_t len;
        int r = read_frame(fd, &t, &rid, &buf, &len, ms);
        if (r != 1) return;
        if (rid != id) { free(buf); continue; }
        if (g->last) { g->chunks_after_last++; free(buf); continue; }
        if (t == CHUNK && len >= 1) {
            uint8_t pipe = buf[0];
            if (pipe <= 2) g->bytes[pipe] += len - 1;
            if (pipe == 1)
                for (size_t i = 1; i < len && g->text_len < sizeof g->text - 1; i++)
                    g->text[g->text_len++] = (char)buf[i];
            g->text[g->text_len] = 0;
            if (give) credit(fd, id, pipe, (uint32_t)(len - 1));
        } else if (t == STDIN_CREDIT && len == 5) {
            g->stdin_credit += get32(buf + 1);
            g->stdin_closed |= buf[0] & 1;
        } else if (t == DONE || t == REFUSED || t == BUSY) {
            g->last = t;
            g->done_len = len < sizeof g->done ? len : sizeof g->done;
            memcpy(g->done, buf, g->done_len);
            free(buf);
            return;
        }
        free(buf);
        if (until && g->bytes[1] >= until) return;
    }
}

static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 200 MiB of `yes` through the window, credit given back for each CHUNK:
 * the throughput (MiB/s), printed for the window / chunk sizing. */
static void stream_throughput(Broker *b, uint32_t id, const char *label)
{
    const size_t total = 200u * 1024u * 1024u;
    double t0 = now_s();
    run_flags(b->fd, id, "y", NULL, NULL, RUN_STREAM);
    Got g = { 0 };
    collect(b->fd, id, &g, 5000, total, 1);
    double dt = now_s() - t0;
    send_frame(b->fd, KILL, id, NULL, 0, NULL, 0);
    CHECK(g.bytes[1] >= total, "%s: 200 MiB streamed through a 256 KiB window "
          "(%zu MiB in %.2f s, %.0f MiB/s)", label, g.bytes[1] >> 20, dt,
          (double)g.bytes[1] / (1024.0 * 1024.0) / dt);
}

static void stream_cases(const char *mode)
{
    uint32_t t;
    char msg[512];
    int r;
    HlExecBuf p = policy_streams();
    Broker b = broker_start(&p);
    printf("== streams (%s)\n", mode);

    /* exec_frames_stream.not_a_stream_command */
    run_flags(b.fd, 1, "e", NULL, NULL, RUN_STREAM);
    r = expect(b.fd, 1, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == REFUSED && strstr(msg, "not a stream command"),
          "a stream RUN to a non-stream command is refused (%s)", msg);
    /* exec_frames_stream.stdin_open_needs_stream */
    run_flags(b.fd, 2, "c", NULL, NULL, RUN_STDIN_OPEN);
    r = expect(b.fd, 2, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == REFUSED, "an open stdin without a stream is refused (%s)", msg);
    /* exec.run of a stream command still runs, under exec.run's limits. */
    run_flags(b.fd, 3, "yt", NULL, NULL, 0);
    {
        Got g = { 0 };
        collect(b.fd, 3, &g, 5000, 0, 0);
        CHECK(g.last == DONE && g.done_len >= 20 && get32(g.done + 16) == 16u << 20 &&
              g.done[9], "exec.run of a stream command with no output cap buffers up "
              "to exec.run's 16 MiB ceiling (%u bytes, cut %d)", g.done_len >= 20 ? get32(g.done + 16) : 0,
              g.done_len >= 20 ? g.done[9] : -1);
    }

    /* exec_frames_stream.window: no credit back -> exactly one window, then
     * nothing; credit -> exactly that much more. */
    run_flags(b.fd, 4, "y", NULL, NULL, RUN_STREAM);
    {
        Got g = { 0 };
        collect(b.fd, 4, &g, 800, 0, 0);
        CHECK(g.bytes[1] == WINDOW && !g.last, "with no credit back the stream stops "
              "at one window (%zu bytes)", g.bytes[1]);
        credit(b.fd, 4, 1, 65536);
        collect(b.fd, 4, &g, 800, 0, 0);
        CHECK(g.bytes[1] == WINDOW + 65536 && !g.last, "64 KiB of credit lets exactly "
              "64 KiB more through (%zu bytes)", g.bytes[1]);
        send_frame(b.fd, KILL, 4, NULL, 0, NULL, 0);
        collect(b.fd, 4, &g, 400, 0, 0);
        CHECK(!g.last, "a killed stream gets no reply");
    }
    /* exec_frames_stream.credit_past_window */
    run_flags(b.fd, 5, "y", NULL, NULL, RUN_STREAM);
    {
        Got g = { 0 };
        collect(b.fd, 5, &g, 500, 0, 0);
        credit(b.fd, 5, 1, WINDOW + 1);
        collect(b.fd, 5, &g, 3000, 0, 0);
        CHECK(g.last == REFUSED, "credit past the window ends the call (last %u)", g.last);
    }
    /* exec_frames_stream.stdin_unopened */
    run_flags(b.fd, 6, "y", NULL, NULL, RUN_STREAM);
    {
        Got g = { 0 };
        stdin_send(b.fd, 6, 0, "x", 1);
        collect(b.fd, 6, &g, 3000, 0, 1);
        CHECK(g.last == REFUSED, "STDIN to a call that did not open stdin ends it");
    }
    /* exec_frames_stream.total_cap: yc's 100000 bytes, then the command is
     * killed and DONE says so. */
    run_flags(b.fd, 7, "yc", NULL, NULL, RUN_STREAM);
    {
        Got g = { 0 };
        collect(b.fd, 7, &g, 5000, 0, 1);
        CHECK(g.last == DONE && g.bytes[1] == 100000 && g.done_len >= 32 &&
              g.done[11] == 1 && g.done[9] == 1 && get64(g.done + 16) == 100000 &&
              get32(g.done + 4) == SIGKILL && g.chunks_after_last == 0,
              "the total cap: 100000 bytes, then killed, cut, counted in DONE "
              "(%zu bytes, signal %u)", g.bytes[1], g.done_len >= 8 ? get32(g.done + 4) : 0);
    }
    /* exec_frames_stream.timeout */
    run_flags(b.fd, 8, "yt", NULL, NULL, RUN_STREAM);
    {
        Got g = { 0 };
        double t0 = now_s();
        collect(b.fd, 8, &g, 4000, 0, 1);
        double dt = now_s() - t0;
        CHECK(g.last == DONE && g.done[8] == 1 && dt > 0.8 && dt < 3.0,
              "a stream times out by its own timeout (%.2f s, timed_out %d)", dt,
              g.done_len > 8 ? g.done[8] : -1);
    }
    /* exec_frames_stream.timeout_unread: a consumer that never gives credit
     * back still gets DONE at the timeout (the rest is dropped, cut). */
    run_flags(b.fd, 15, "yt", NULL, NULL, RUN_STREAM);
    {
        Got g = { 0 };
        collect(b.fd, 15, &g, 4000, 0, 0);
        CHECK(g.last == DONE && g.done[8] == 1 && g.done[9] == 1 && g.bytes[1] == WINDOW,
              "an unread stream still ends at its timeout, cut at one window "
              "(%zu bytes, timed_out %d)", g.bytes[1], g.done_len > 8 ? g.done[8] : -1);
    }
    /* exec_frames_stream.kill_unread: a KILL frees the slot of a stream the
     * app stopped reading (y has concurrency 4: four more run). */
    for (uint32_t id = 16; id < 20; id++) run_flags(b.fd, id, "y", NULL, NULL, RUN_STREAM);
    usleep(300000);
    for (uint32_t id = 16; id < 20; id++) send_frame(b.fd, KILL, id, NULL, 0, NULL, 0);
    usleep(300000);
    {
        /* One at a time, each left running unread (it then sends nothing
         * more), so all four are in flight together. */
        int ok = 1;
        for (uint32_t id = 20; id < 24; id++) {
            run_flags(b.fd, id, "y", NULL, NULL, RUN_STREAM);
            Got g = { 0 };
            collect(b.fd, id, &g, 800, 0, 0);
            ok &= g.bytes[1] == WINDOW && !g.last;
        }
        for (uint32_t id = 20; id < 24; id++) send_frame(b.fd, KILL, id, NULL, 0, NULL, 0);
        CHECK(ok, "KILLed unread streams free their slots: four more run");
    }
    usleep(300000);
    /* exec_frames_stream.stdin_round_trip: bash prints, waits for a line on
     * its stdin, then prints it: output before the end, stdin as it goes. */
    run_flags(b.fd, 9, "b", NULL, NULL, RUN_STREAM | RUN_STDIN_OPEN);
    {
        Got g = { 0 };
        collect(b.fd, 9, &g, 1000, 2, 1);
        CHECK(g.text_len == 2 && memcmp(g.text, "a\n", 2) == 0 && !g.last &&
              g.stdin_credit == WINDOW, "output arrives before the command ends, with "
              "the stdin window granted ('%s', credit %u)", g.text, g.stdin_credit);
        stdin_send(b.fd, 9, 0, "hello\n", 6);
        collect(b.fd, 9, &g, 5000, 0, 1);
        CHECK(g.last == DONE && strstr(g.text, "got hello") && get32(g.done) == 0,
              "a line written to its stdin reaches it ('%s')", g.text);
    }
    /* exec_frames_stream.stdin_eof: cat echoes what it gets, then EOF ends it;
     * the credit for the streamed bytes comes back. */
    run_flags(b.fd, 10, "c", NULL, "init-", RUN_STREAM | RUN_STDIN_OPEN);
    {
        Got g = { 0 };
        while (!g.stdin_credit && !g.last) collect(b.fd, 10, &g, 3000, 0, 1);
        stdin_send(b.fd, 10, 1, "rest", 4);   /* within the grant, as a server waits for */
        collect(b.fd, 10, &g, 5000, 0, 1);
        CHECK(g.last == DONE && g.text_len == 9 && memcmp(g.text, "init-rest", 9) == 0 &&
              get32(g.done) == 0 && g.stdin_credit == WINDOW + 4,
              "the RUN's stdin, then streamed stdin, then EOF: cat ends ('%s', credit %u)",
              g.text, g.stdin_credit);
    }
    /* exec_frames_stream.stdin_backpressure: cat with no output credit
     * returned stops reading its stdin once its stdout is full, so the stdin
     * credit stops too. */
    run_flags(b.fd, 12, "c", NULL, NULL, RUN_STREAM | RUN_STDIN_OPEN);
    {
        Got g = { 0 };
        char *big = malloc(65536);
        memset(big, 'q', 65536);
        uint32_t sent = 0;
        collect(b.fd, 12, &g, 300, 0, 0);         /* the first grant */
        for (int round = 0; round < 40; round++) {
            while (sent + 65536 <= g.stdin_credit) {
                stdin_send(b.fd, 12, 0, big, 65536);
                sent += 65536;
            }
            collect(b.fd, 12, &g, 200, 0, 0);
        }
        CHECK(!g.last && g.bytes[1] == WINDOW && sent < 4u * 1024 * 1024,
              "a child that cannot write stops taking stdin: %u bytes sent, %zu out",
              sent, g.bytes[1]);
        /* exec_frames_stream.stdin_past_credit: the credit left, and a byte. */
        uint32_t left = g.stdin_credit - sent;
        while (left > 0) {
            uint32_t n = left > 65536 ? 65536 : left;
            stdin_send(b.fd, 12, 0, big, n);
            left -= n;
        }
        stdin_send(b.fd, 12, 0, big, 1);
        free(big);
        collect(b.fd, 12, &g, 3000, 0, 0);
        CHECK(g.last == REFUSED, "stdin past its credit ends the call (last %u)", g.last);
    }
    /* After all that the broker takes calls. */
    run_flags(b.fd, 13, "e", NULL, NULL, 0);
    r = expect(b.fd, 13, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == DONE, "the broker takes calls afterwards");
    stream_throughput(&b, 14, mode);
    broker_end(&b);
    hl_exec_buf_free(&p);
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

#ifdef __APPLE__
    /* 10. exec_frames_router.kill_after_broker_lost */
    p = policy_sl_s1();
    b = broker_start(&p);
    pid_t subs[4];
    int nsub = children_of(b.pid, subs, 4);
    CHECK(nsub == 2, "two commands: the router has one broker each (%d)", nsub);
    run(b.fd, 1, "sl", NULL);                /* the call to be killed */
    run(b.fd, 2, "s1", NULL);                /* the live call (1 s) */
    usleep(300000);
    pid_t target = -1;                       /* sl's broker: the one with a child */
    for (int i = 0; i < nsub; i++) {
        pid_t keepers[2], jobs[2];
        /* broker -> keeper -> command; s1's sleep may still run too: sl's
         * command runs 30 s. */
        int nk = children_of(subs[i], keepers, 2);
        for (int k = 0; k < nk && target < 0; k++) {
            if (children_of(keepers[k], jobs, 2) < 1) continue;
            char cmd[96];
            snprintf(cmd, sizeof cmd, "ps -o args= -p %d | grep -q ' 30$'", (int)jobs[0]);
            if (system(cmd) == 0) target = subs[i];
        }
    }
    CHECK(target > 0, "found the killed call's broker (%d)", (int)target);
    /* Hold the router, so the KILL, the next RUN and the broker's death all
     * reach it in one poll pass. The frames go first: macOS's poll, resumed,
     * reports the event that fired first, and the server's channel is read
     * first. The KILL's write then finds the broker dead (EPIPE) and marks
     * it lost while the RUN is read in the same loop. */
    kill(b.pid, SIGSTOP);
    usleep(50000);
    send_frame(b.fd, KILL, 1, NULL, 0, NULL, 0);
    run(b.fd, 3, "s1", NULL);
    usleep(50000);
    if (target > 0) kill(target, SIGKILL);
    usleep(200000);
    kill(b.pid, SIGCONT);
    r = expect(b.fd, 2, &t, msg, sizeof msg, 4000);
    CHECK(r == 1 && t == DONE, "the live call completes normally (r %d, type %u)", r,
          r == 1 ? t : 0);
    r = expect(b.fd, 3, &t, msg, sizeof msg, 4000);
    CHECK(r == 1 && t == DONE, "the call read with the KILL gets its own reply "
          "(r %d, type %u)", r, r == 1 ? t : 0);
    run(b.fd, 4, "s1", NULL);
    r = expect(b.fd, 4, &t, msg, sizeof msg, 4000);
    CHECK(r == 1 && t == DONE, "the router takes another call afterwards");
    run(b.fd, 5, "sl", NULL);
    r = expect(b.fd, 5, &t, msg, sizeof msg, 3000);
    CHECK(r == 1 && t == REFUSED && strstr(msg, "command broker is gone"),
          "a call to the lost command is refused (%s)", msg);
    broker_end(&b);
    hl_exec_buf_free(&p);

    /* 11. exec_frames_router.stopped_broker_holds_nothing (#1234) */
    p = policy_cat_s1();
    b = broker_start(&p);
    nsub = children_of(b.pid, subs, 4);
    CHECK(nsub == 2, "two commands: the router has one broker each (%d)", nsub);
    pid_t cat_broker = nsub == 2 ? (subs[0] < subs[1] ? subs[0] : subs[1]) : -1;
    size_t big = 512 * 1024;                 /* far past the socket buffer */
    char *in = malloc(big + 1);
    memset(in, 'x', big);
    in[big] = 0;
    if (cat_broker > 0) kill(cat_broker, SIGSTOP);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    run_in(b.fd, 1, "c", NULL, in);          /* waits in the router's queue */
    run_in(b.fd, 2, "c", NULL, in);          /* never starts on the wire... */
    send_frame(b.fd, KILL, 2, NULL, 0, NULL, 0);   /* ...and is killed */
    run(b.fd, 3, "s1", NULL);
    r = expect(b.fd, 3, &t, msg, sizeof msg, 3000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(r == 1 && t == DONE && ms < 2500,
          "another command's call completes while one broker is stopped (r %d, %ld ms)",
          r, ms);
    usleep(5000000);                         /* past the old 5 s write deadline */
    if (cat_broker > 0) kill(cat_broker, SIGCONT);
    {
        uint32_t ty, id; uint8_t *buf; size_t len;
        int got1 = 0, got2 = 0;
        size_t out_len = 0;
        for (;;) {
            int rr = read_frame(b.fd, &ty, &id, &buf, &len, 4000);
            if (rr != 1) break;
            if (id == 1) { got1 = 1; t = ty; if (ty == DONE && len >= 20) out_len = get32(buf + 16); }
            if (id == 2) got2 = 1;
            free(buf);
            if (got1) break;
        }
        CHECK(got1 && t == DONE && out_len == big,
              "the stopped broker's call completes once it reads again (type %u, %zu bytes)",
              got1 ? t : 0, out_len);
        r = expect(b.fd, 2, &t, msg, sizeof msg, 500);
        CHECK(!got2 && r == 0, "the killed call's RUN never went out: no reply");
    }
    run_in(b.fd, 4, "c", NULL, "hi");
    r = expect(b.fd, 4, &t, msg, sizeof msg, 4000);
    CHECK(r == 1 && t == DONE, "the command takes calls afterwards");
    free(in);
    broker_end(&b);
    hl_exec_buf_free(&p);
#endif

#ifdef __APPLE__
    stream_cases("router: six commands, one broker each");
    {
        HlExecBuf py = policy_yes_only();
        Broker by = broker_start(&py);
        stream_throughput(&by, 1, "one command, no router");
        broker_end(&by);
        hl_exec_buf_free(&py);
    }
#else
    stream_cases("one broker");
#endif

    printf("e2e_exec_frames: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
