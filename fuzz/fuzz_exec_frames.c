/*
 * libFuzzer harness for the command broker's frame handling
 * (docs/design/exec_broker.md §5.2, docs/design/exec_stream.md §4): the
 * frames the SERVER sends - RUN, KILL, CREDIT, STDIN - which a compromised
 * app process can write with any payload.
 *
 * The broker's handlers are static and act on live children, so this drives
 * a real broker (`hull __hull_exec_broker`, ideally an ASan build) on a
 * socketpair, as tests/e2e_exec_frames.c does. One broker serves the whole
 * run; each input is decoded into a sequence of frames, and after it every
 * call is KILLed and drained and one plain call must still complete. The
 * harness aborts (so libFuzzer keeps the input) when:
 *   - the broker exits or stops answering (a crash, an ASan report, a hang);
 *   - a stream sends more output than the window allows: per call and pipe,
 *     CHUNK bytes received minus CREDIT sent never exceeds 256 KiB;
 *   - a frame arrives for an id never used, or for a call after its final
 *     frame (DONE / BUSY / REFUSED): one call's output reached another id.
 * Ids are never reused, as the server never reuses them; a KILLed call gets
 * no reply, and frames already on their way for it are ignored.
 * Framing itself stays valid (a bad magic, type or length ends the broker by
 * design); every payload byte is the fuzzer's.
 *
 * Commands (on macOS, more than one makes the broker a router, so the router
 * relays are fuzzed too; HULL_FUZZ_ONE=1 keeps one broker):
 *   "c"  stream, /bin/cat, stdin open up to 1 MiB, 10 s, stdout cap 4 MiB
 *   "y"  stream, /usr/bin/yes, 2 s, stdout cap 1 MiB
 *   "e"  plain, /bin/echo with one [a-z] slot (the liveness call)
 *   "i"  plain, /bin/cat with stdin up to 64 KiB
 *
 * Run (HULL_FUZZ_HULL = the hull to drive; HULL_FUZZ_DEGRADED=1 on a Linux
 * kernel below Landlock ABI 6):
 *   make fuzz/fuzz_exec_frames
 *   HULL_FUZZ_HULL=build/hull ./fuzz/fuzz_exec_frames fuzz/corpus_exec_frames/ \
 *       -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "hull/cap/exec.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAGIC        0x43455848u
#define T_POLICY     1u
#define T_READY      2u
#define T_RUN        3u
#define T_KILL       4u
#define T_DONE       5u
#define T_BUSY       6u
#define T_REFUSED    7u
#define T_CHUNK      9u
#define T_CREDIT    10u
#define T_STDIN     11u
#define T_STDIN_CREDIT 12u
#define WINDOW      (256u * 1024u)
#define NSLOTS      8u           /* calls the input addresses at once */
#define RING        4096u        /* states kept for the last RING ids */
#define LIVE_BASE   0x80000000u  /* ids of the liveness calls */

static const char *const CMDS[] = { "c", "y", "e", "i", "zz" };

enum { C_FREE, C_ACTIVE, C_FINAL, C_KILLED };
typedef struct {
    uint32_t id;
    int      state;
    uint64_t outstanding[2];     /* CHUNK bytes received - CREDIT sent, per pipe */
    uint64_t in_grant;           /* STDIN_CREDIT granted - STDIN sent */
} Call;

static int      g_fd = -1;
static pid_t    g_pid;
static Call     g_ring[RING];
static uint32_t g_slot_id[NSLOTS];   /* the id each slot addresses now */
static uint32_t g_next_id = 1;
static uint8_t  g_rbuf[1u << 21];
static size_t   g_rlen;
static int      g_live_done;       /* 1 DONE, 2 BUSY / REFUSED */
static uint32_t g_live_id;

static void die(const char *why)
{
    fprintf(stderr, "fuzz_exec_frames: %s\n", why);
    abort();
}

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

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* One frame from the broker: check it against the oracles. */
static void on_frame(uint32_t type, uint32_t id, const uint8_t *p, size_t len)
{
    if (id >= LIVE_BASE) {
        if (id == g_live_id && type == T_DONE) g_live_done = 1;
        else if (id == g_live_id && (type == T_BUSY || type == T_REFUSED)) g_live_done = 2;
        return;
    }
    if (id == 0 || id >= g_next_id) die("a frame for an id the harness never used");
    Call *c = &g_ring[id % RING];
    if (c->id != id || c->state == C_KILLED) return;   /* killed or long gone */
    if (c->state != C_ACTIVE) die("a frame for a call after its final frame");
    switch (type) {
    case T_CHUNK: {
        if (len < 1 || (p[0] != 1 && p[0] != 2)) die("a malformed CHUNK");
        uint64_t *o = &c->outstanding[p[0] - 1];
        *o += len - 1;
        if (*o > WINDOW) die("a stream sent past its window");
        break;
    }
    case T_STDIN_CREDIT:
        if (len != 5) die("a malformed STDIN_CREDIT");
        c->in_grant += get32(p + 1);
        break;
    case T_DONE: case T_BUSY: case T_REFUSED:
        c->state = C_FINAL;
        break;
    default:
        die("an unexpected frame type");
    }
}

/* Read whatever is there (or wait up to @p ms for something). 0 on EOF. */
static int pump_read(int ms)
{
    struct pollfd pf = { g_fd, POLLIN, 0 };
    int r = poll(&pf, 1, ms);
    if (r < 0 && errno == EINTR) return 1;
    if (r <= 0) return 1;
    if (g_rlen == sizeof g_rbuf) die("a frame larger than the harness buffer");
    ssize_t n = read(g_fd, g_rbuf + g_rlen, sizeof g_rbuf - g_rlen);
    if (n == 0) return 0;
    if (n < 0) return errno == EAGAIN || errno == EINTR;
    g_rlen += (size_t)n;
    size_t off = 0;
    while (g_rlen - off >= 16) {
        uint8_t *h = g_rbuf + off;
        if (get32(h) != MAGIC) die("a frame with a bad magic");
        size_t len = get32(h + 12);
        if (len > sizeof g_rbuf - 16) die("a frame larger than any reply");
        if (g_rlen - off < 16 + len) break;
        on_frame(get32(h + 4), get32(h + 8), h + 16, len);
        off += 16 + len;
    }
    memmove(g_rbuf, g_rbuf + off, g_rlen - off);
    g_rlen -= off;
    return 1;
}

/* Write a frame, reading while the broker's side is full (it stops reading
 * while its own output queue is full). */
static void send_frame(uint32_t type, uint32_t id, const uint8_t *p, size_t len)
{
    uint8_t *f = malloc(16 + len);
    if (!f) die("out of memory");
    put32(f, MAGIC); put32(f + 4, type); put32(f + 8, id); put32(f + 12, (uint32_t)len);
    if (len) memcpy(f + 16, p, len);
    size_t off = 0, n = 16 + len;
    uint64_t deadline = now_ms() + 10000;
    while (off < n) {
        struct pollfd pf = { g_fd, POLLIN | POLLOUT, 0 };
        if (poll(&pf, 1, 100) < 0 && errno != EINTR) die("poll");
        if (pf.revents & POLLIN) { if (!pump_read(0)) die("the broker exited"); }
        if (pf.revents & (POLLERR | POLLHUP)) die("the broker closed its channel");
        if (pf.revents & POLLOUT) {
            ssize_t w = send(g_fd, f + off, n - off, MSG_DONTWAIT
#ifdef MSG_NOSIGNAL
                             | MSG_NOSIGNAL
#endif
                             );
            if (w > 0) off += (size_t)w;
            else if (w < 0 && errno != EAGAIN && errno != EINTR) die("the broker closed its channel");
        }
        if (now_ms() > deadline) die("the broker stopped reading");
    }
    free(f);
}

static void stream_cmd(HlExecBuf *b, const char *name, const char *path, int64_t timeout,
                       int64_t out_max, int64_t in_max)
{
    hl_exec_b_cmd_stream(b, name, strlen(name), path, strlen(path), timeout, out_max, -1,
                         4, 0, in_max, NULL, 0);
    for (int k = 0; k < 3; k++) hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
    hl_exec_b_count_end(b, hl_exec_b_count_begin(b), 0);
}

static void broker_start(void)
{
    const char *hull = getenv("HULL_FUZZ_HULL");
    if (!hull || !*hull) hull = "build/hull";
    int one = getenv("HULL_FUZZ_ONE") && strcmp(getenv("HULL_FUZZ_ONE"), "1") == 0;
    HlExecBuf b;
    hl_exec_b_init(&b, NULL);
    stream_cmd(&b, "c", "/bin/cat", 10000, 4 * 1024 * 1024, 1024 * 1024);
    if (!one) {
        stream_cmd(&b, "y", "/usr/bin/yes", 2000, 1024 * 1024, -1);
        hl_exec_b_cmd(&b, "e", 1, "/bin/echo", 9, 5000, -1, -1, 4, 0, -1, NULL, 0);
        for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
        size_t m = hl_exec_b_count_begin(&b);
        hl_exec_b_slot(&b, 0, -1, -1, 0, -1, -1, NULL, 0, NULL, 0, "a-z", 3);
        hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
        hl_exec_b_count_end(&b, m, 1);
        hl_exec_b_cmd(&b, "i", 1, "/bin/cat", 8, 5000, -1, -1, 4, 0, 65536, NULL, 0);
        for (int k = 0; k < 3; k++) hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
        hl_exec_b_count_end(&b, hl_exec_b_count_begin(&b), 0);
    }
    if (hl_exec_b_finish(&b) != 0) { fprintf(stderr, "policy: %s\n", b.err); exit(2); }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(2); }
    g_pid = fork();
    if (g_pid == 0) {
        if (sv[0] != 3) close(sv[0]);
        int s = sv[1] == 3 ? 3 : dup2(sv[1], 3);
        if (s != 3) _exit(126);
        if (sv[1] != 3) close(sv[1]);
        execl(hull, hull, "__hull_exec_broker", (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    g_fd = sv[0];
    uint8_t head[10] = { 0 };
    const char *deg = getenv("HULL_FUZZ_DEGRADED");
    put32(head, deg && strcmp(deg, "1") == 0 ? 1u : 0u);
    uint8_t *pol = malloc(10 + b.len);
    memcpy(pol, head, 10);
    memcpy(pol + 10, b.data, b.len);
    /* READY is not a call frame: read it by hand before the oracles apply. */
    uint8_t h[16] = { 0 };
    {
        uint8_t fh[16];
        put32(fh, MAGIC); put32(fh + 4, T_POLICY); put32(fh + 8, 0);
        put32(fh + 12, (uint32_t)(10 + b.len));
        if (write(g_fd, fh, 16) != 16 || write(g_fd, pol, 10 + b.len) != (ssize_t)(10 + b.len))
            die("could not send the policy");
        size_t got = 0;
        while (got < 16) {
            struct pollfd pf = { g_fd, POLLIN, 0 };
            if (poll(&pf, 1, 10000) <= 0) die("no READY from the broker");
            ssize_t r = read(g_fd, h + got, 16 - got);
            if (r <= 0) die("the broker exited before READY (is HULL_FUZZ_HULL a hull?)");
            got += (size_t)r;
        }
        if (get32(h + 4) != T_READY) die("the broker refused the policy");
        size_t len = get32(h + 12), skip = 0;
        uint8_t tmp[512];
        while (skip < len) {
            ssize_t r = read(g_fd, tmp, len - skip < sizeof tmp ? len - skip : sizeof tmp);
            if (r <= 0) die("the broker exited after READY");
            skip += (size_t)r;
        }
    }
    free(pol);
    hl_exec_buf_free(&b);
}

/* A byte reader over the input. */
typedef struct { const uint8_t *p; size_t n, i; } In;
static uint8_t  in8(In *r)  { return r->i < r->n ? r->p[r->i++] : 0; }
static uint32_t in32(In *r) { uint32_t v = 0; for (int k = 0; k < 4; k++) v |= (uint32_t)in8(r) << (8 * k); return v; }

/* RUN for @p id: a well-formed one usually, else the raw bytes that follow. */
static Call *slot_call(uint32_t slot)
{
    uint32_t id = g_slot_id[slot];
    if (!id) return NULL;
    Call *c = &g_ring[id % RING];
    return c->id == id ? c : NULL;
}

static void op_run(In *r, uint32_t slot)
{
    Call *old = slot_call(slot);
    if (old && old->state == C_ACTIVE) return;   /* one live call per slot */
    uint32_t id = g_next_id++;
    if (g_next_id >= LIVE_BASE) g_next_id = 1;   /* not reached in practice */
    uint8_t mode = in8(r);
    uint8_t buf[600];
    size_t off = 0;
    if (mode & 0x80) {                  /* malformed: raw payload */
        size_t n = in8(r) % 64;
        for (size_t k = 0; k < n; k++) buf[off++] = in8(r);
    } else {
        const char *name = CMDS[in8(r) % (sizeof CMDS / sizeof CMDS[0])];
        size_t nl = strlen(name);
        buf[off++] = (uint8_t)nl; buf[off++] = 0;
        memcpy(buf + off, name, nl); off += nl;
        uint8_t nargs = in8(r) % 3;
        buf[off++] = nargs; buf[off++] = 0;
        for (uint8_t a = 0; a < nargs; a++) {
            size_t al = in8(r) % 12;
            put32(buf + off, (uint32_t)al); off += 4;
            for (size_t k = 0; k < al; k++) buf[off++] = (uint8_t)('a' + in8(r) % 30);
        }
        size_t il = in8(r) % 200;
        put32(buf + off, (uint32_t)il); off += 4;
        for (size_t k = 0; k < il; k++) buf[off++] = in8(r);
        uint8_t fl = in8(r);
        buf[off++] = (mode & 0x40) ? fl : (uint8_t)(fl & 3);
    }
    Call *c = &g_ring[id % RING];
    memset(c, 0, sizeof *c);
    c->id = id;
    c->state = C_ACTIVE;
    g_slot_id[slot] = id;
    send_frame(T_RUN, id, buf, off);
}

static void op_credit(In *r, uint32_t slot)
{
    Call *c = slot_call(slot);
    if (!c) return;
    uint32_t id = c->id;
    uint8_t mode = in8(r);
    uint8_t pl[8];
    size_t len = 5;
    uint8_t fd = (mode & 1) ? 2 : 1;
    uint32_t bytes;
    if (mode & 0x80) { fd = in8(r); bytes = in32(r); len = in8(r) % 9; }
    else bytes = (mode & 0x40) ? in32(r) : (uint32_t)c->outstanding[fd - 1];
    pl[0] = fd;
    put32(pl + 1, bytes);
    pl[5] = pl[6] = pl[7] = 0;
    /* Track only credit the broker accepts (the call ends otherwise). */
    if (len == 5 && (fd == 1 || fd == 2) && bytes <= c->outstanding[fd - 1])
        c->outstanding[fd - 1] -= bytes;
    send_frame(T_CREDIT, id, pl, len);
}

static void op_stdin(In *r, uint32_t slot)
{
    Call *c = slot_call(slot);
    if (!c) return;
    uint32_t id = c->id;
    uint8_t mode = in8(r);
    size_t n;
    if (mode & 0x80) n = ((size_t)in8(r) << 8 | in8(r)) + (mode & 1 ? 65000 : 0);
    else n = c->in_grant < 4096 ? (size_t)c->in_grant : (size_t)(in8(r) * 16u);
    if (n > 70000) n = 70000;
    uint8_t *pl = malloc(1 + n);
    if (!pl) die("out of memory");
    pl[0] = (mode & 0x40) ? in8(r) : (uint8_t)((mode >> 1) & 1);   /* EOF bit */
    for (size_t k = 0; k < n; k++) pl[1 + k] = (uint8_t)('0' + k % 64);
    if (n <= c->in_grant) c->in_grant -= n;
    send_frame(T_STDIN, id, pl, 1 + n);
    free(pl);
}

/* Read for a moment, crediting what arrived when @p credit (keeps streams
 * moving so output, the coalescing and the totals are reached). */
static void op_drain(int ms, int credit)
{
    uint64_t end = now_ms() + (uint64_t)ms;
    do {
        if (!pump_read(5)) die("the broker exited");
        if (credit)
            for (uint32_t s = 0; s < NSLOTS; s++) {
                Call *c = slot_call(s);
                if (!c || c->state != C_ACTIVE) continue;
                for (int k = 0; k < 2; k++)
                    if (c->outstanding[k] >= 65536) {
                        uint8_t pl[5];
                        pl[0] = (uint8_t)(k + 1);
                        put32(pl + 1, (uint32_t)c->outstanding[k]);
                        c->outstanding[k] = 0;
                        send_frame(T_CREDIT, c->id, pl, 5);
                    }
            }
    } while (now_ms() < end);
}

static void op_kill(uint32_t slot)
{
    Call *c = slot_call(slot);
    if (!c) return;
    if (c->state == C_ACTIVE) c->state = C_KILLED;
    send_frame(T_KILL, c->id, NULL, 0);
}

/* End every call and check the broker still serves a plain call. */
static void settle(void)
{
    for (uint32_t s = 0; s < NSLOTS; s++) op_kill(s);
    static const uint8_t live_one[] = { 1, 0, 'e', 1, 0, 2, 0, 0, 0, 'o', 'k', 0, 0, 0, 0, 0 };
    static const uint8_t live_cat[] = { 1, 0, 'c', 0, 0, 0, 0, 0, 0, 0 };
    int one = getenv("HULL_FUZZ_ONE") && strcmp(getenv("HULL_FUZZ_ONE"), "1") == 0;
    static uint32_t live_next = LIVE_BASE;
    /* Killed calls hold their slots until reaped: BUSY is retried. */
    uint64_t end = now_ms() + 10000;
    for (;;) {
        g_live_done = 0;
        g_live_id = live_next++;
        if (one) send_frame(T_RUN, g_live_id, live_cat, sizeof live_cat);
        else send_frame(T_RUN, g_live_id, live_one, sizeof live_one);
        while (!g_live_done) {
            if (now_ms() > end) die("the broker no longer completes a plain call");
            if (!pump_read(20)) die("the broker exited");
        }
        if (g_live_done == 1) break;
        if (now_ms() > end) die("the broker refuses a plain call for good");
        op_drain(20, 0);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (g_fd < 0) {
        signal(SIGPIPE, SIG_IGN);
        broker_start();
    }
    In r = { data, size, 0 };
    int ops = 0;
    while (r.i < r.n && ops++ < 64) {
        uint8_t op = in8(&r);
        uint32_t slot = (uint32_t)(in8(&r) % NSLOTS);
        switch (op % 6) {
        case 0: op_run(&r, slot); break;
        case 1: op_credit(&r, slot); break;
        case 2: op_stdin(&r, slot); break;
        case 3: op_kill(slot); break;
        case 4: op_drain(in8(&r) % 40, 1); break;
        case 5: op_drain(in8(&r) % 10, 0); break;
        }
    }
    settle();
    return 0;
}
