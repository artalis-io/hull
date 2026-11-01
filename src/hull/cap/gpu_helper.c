/*
 * gpu_helper.c - the shader compile helper process (audit convergence §7.3).
 *
 * See include/hull/cap/gpu_helper.h for the model and
 * docs/design/gpu_compile_helper.md for the design, the choice of "the
 * helper validates, the server then compiles" and the measurements.
 *
 * Three roles live here, all in Hull's own binary:
 *
 *   client      the server: hl_gpu_helper_start / _check / _stop
 *   supervisor  spawned before the server's sandbox; one per server
 *   worker      spawned by the supervisor, one per compile
 *
 * Wire format (every frame, both sockets): a 16-byte header
 *   magic "HGC1" | type | id | payload length     (u32 little-endian each)
 * then the payload:
 *   REQ    deadline_ms u32 | device i32 | WGSL bytes  (<= 8 + 1 MiB)
 *   CANCEL (empty)                                  server -> supervisor
 *   READY  (empty)                                  worker -> supervisor
 *   RESP   status i32 | flags u32 | detail          (<= HL_GPU_HELPER_REPLY_MAX)
 * (flags bit 0: the worker's OS sandbox was applied.)
 * A frame of another type, or longer than its cap, is a protocol error.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/gpu_helper.h"
#include "hull/cap/gpu.h"
#include "hull/cap/crypto.h"
#include "hull/limits/gpu.h"
#include "hull/shared/host.h"
#ifdef HL_ENABLE_DB
#include "hull/cap/db_budget.h"
#endif
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(__COSMOPOLITAN__) && !defined(_WIN32)
#define HL_GPU_HELPER_SUPPORTED 1
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__linux__)
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/audit.h>
#include <stddef.h>
#endif

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

/* The sealed on-disk verdict cache ("gpu-shaders") lives with the other
 * sealed runtime caches in runtime/cache_common.c, which overrides these
 * weak no-op defaults wherever it is linked (hull, every built app); an
 * embedder or a unit test without it keeps verdicts in memory only. */
__attribute__((weak)) int hl_runtime_cache_gpu_verdict_get(const char *hex)
{
    (void)hex;
    return -1;
}

__attribute__((weak)) void hl_runtime_cache_gpu_verdict_put(const char *hex)
{
    (void)hex;
}

extern char **environ;

static uint32_t g_test_init_ms, g_test_compile_ms;

void hl_gpu_helper_test_set_timeouts(uint32_t init_ms, uint32_t compile_ms)
{
    g_test_init_ms = init_ms;
    g_test_compile_ms = compile_ms;
}

#ifdef HL_GPU_HELPER_SUPPORTED

/* ── Shared: the backend, the deadlines ──────────────────────────────── */

static const HlGpuBackend *helper_backend(void)
{
    size_t n = 0;
    const HlGpuBackend *const *feats = hl_gpu_feature_backends(&n);
    if (n > 0 && feats && feats[0])
        return feats[0];
#ifdef HL_ENABLE_GPU
    return &hl_gpu_backend_wgpu;
#else
    return NULL;
#endif
}

static uint32_t helper_init_ms(void)
{
    return g_test_init_ms ? g_test_init_ms : HL_GPU_HELPER_INIT_TIMEOUT_MS;
}

static uint32_t helper_compile_ms(void)
{
    return g_test_compile_ms ? g_test_compile_ms
                             : HL_GPU_HELPER_COMPILE_TIMEOUT_MS;
}

/* ── Frames ────────────────────────────────────────────────────────── */

#define HGC_MAGIC   0x31434748u   /* "HGC1" little-endian */
#define HGC_HDR     16u
#define HGC_REQ     1u
#define HGC_CANCEL  2u
#define HGC_RESP    3u
#define HGC_READY   4u
#define HGC_REQ_MAX ((uint32_t)(8u + HL_GPU_WGSL_MAX_BYTES))
#define HGC_FLAG_SANDBOXED 0x1u

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

#ifdef MSG_NOSIGNAL
#define HGC_SEND_FLAGS MSG_NOSIGNAL
#else
#define HGC_SEND_FLAGS 0
#endif

/* Milliseconds left until @p deadline (0 = no deadline: -1, poll forever). */
static int ms_left(uint64_t deadline)
{
    if (!deadline) return -1;
    uint64_t now = mono_ms();
    if (now >= deadline) return 0;
    uint64_t left = deadline - now;
    return left > INT_MAX ? INT_MAX : (int)left;
}

/* Write @p n bytes to a non-blocking socket, waiting for room at most until
 * @p deadline (0 = none). 0, or -1 (error, peer gone, deadline). */
static int write_all(int fd, const void *buf, size_t n, uint64_t deadline)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, HGC_SEND_FLAGS);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            int left = ms_left(deadline);
            if (left == 0) return -1;
            int pr = poll(&pf, 1, left);
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0) return -1;
            if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int write_frame(int fd, uint32_t type, uint32_t id,
                       const void *a, size_t alen, const void *b, size_t blen,
                       uint64_t deadline)
{
    if (alen + blen > UINT32_MAX) return -1;
    uint8_t hdr[HGC_HDR];
    put32(hdr, HGC_MAGIC);
    put32(hdr + 4, type);
    put32(hdr + 8, id);
    put32(hdr + 12, (uint32_t)(alen + blen));
    if (write_all(fd, hdr, sizeof hdr, deadline) != 0) return -1;
    if (alen && write_all(fd, a, alen, deadline) != 0) return -1;
    if (blen && write_all(fd, b, blen, deadline) != 0) return -1;
    return 0;
}

/* An incremental frame reader over a non-blocking socket. */
typedef struct {
    uint8_t  hdr[HGC_HDR];
    size_t   got;        /* bytes of header + payload read so far */
    uint32_t type, id, len;
    uint8_t *buf;        /* the payload (len bytes), malloc'd */
    uint32_t max;        /* the largest payload taken */
} FrameRd;

#define FR_MORE      0
#define FR_DONE      1
#define FR_EOF      -1
#define FR_ERROR    -2
#define FR_OVERSIZE -3

static void frame_reset(FrameRd *r)
{
    free(r->buf);
    uint32_t max = r->max;
    memset(r, 0, sizeof *r);
    r->max = max;
}

/* Read what the socket has. FR_DONE when a whole frame is in @p r (the
 * caller takes it, then frame_reset), FR_MORE when it needs more, else an
 * error (FR_EOF before a whole frame, FR_ERROR, FR_OVERSIZE past r->max). */
static int frame_read_some(int fd, FrameRd *r)
{
    for (;;) {
        if (r->got < HGC_HDR) {
            ssize_t n = recv(fd, r->hdr + r->got, HGC_HDR - r->got, 0);
            if (n == 0) return FR_EOF;
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return FR_MORE;
                return FR_ERROR;
            }
            r->got += (size_t)n;
            if (r->got < HGC_HDR) continue;
            if (get32(r->hdr) != HGC_MAGIC) return FR_ERROR;
            r->type = get32(r->hdr + 4);
            r->id   = get32(r->hdr + 8);
            r->len  = get32(r->hdr + 12);
            if (r->len > r->max) return FR_OVERSIZE;
            if (r->len > 0) {
                r->buf = malloc(r->len);
                if (!r->buf) return FR_ERROR;
            }
        }
        size_t have = r->got - HGC_HDR;
        if (have == r->len) return FR_DONE;
        ssize_t n = recv(fd, r->buf + have, r->len - have, 0);
        if (n == 0) return FR_EOF;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return FR_MORE;
            return FR_ERROR;
        }
        r->got += (size_t)n;
    }
}

/* Read one whole frame, waiting at most until @p deadline (0 = none).
 * FR_DONE, FR_MORE on the deadline, or an error. */
static int frame_read(int fd, FrameRd *r, uint64_t deadline)
{
    for (;;) {
        int rc = frame_read_some(fd, r);
        if (rc != FR_MORE) return rc;
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int left = ms_left(deadline);
        if (left == 0) return FR_MORE;
        int pr = poll(&pf, 1, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) return FR_ERROR;
        if (pr == 0) return FR_MORE;
    }
}

/* ── Spawning ─────────────────────────────────────────────────────── */

/* The environment a helper gets: what the GPU stack reads (Vulkan / Mesa /
 * wgpu knobs, the cache and runtime dirs, the loader path) and the test
 * hook - never the server's secrets (manifest.secrets, DSN passwords). */
static int env_allowed(const char *kv)
{
    static const char *const prefixes[] = { "VK_", "MESA_", "WGPU_", "MTL_",
                                            "__GLX_", NULL };
    static const char *const names[] = { "HOME", "TMPDIR", "XDG_CACHE_HOME",
                                         "XDG_RUNTIME_DIR", "LD_LIBRARY_PATH",
                                         "HULL_TEST_GPU_HELPER", NULL };
    for (int i = 0; prefixes[i]; i++)
        if (strncmp(kv, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    for (int i = 0; names[i]; i++) {
        size_t n = strlen(names[i]);
        if (strncmp(kv, names[i], n) == 0 && kv[n] == '=') return 1;
    }
    return 0;
}

static char **helper_env(void)
{
    size_t n = 0;
    for (char **e = environ; e && *e; e++) n++;
    char **out = calloc(n + 1, sizeof *out);
    if (!out) return NULL;
    size_t k = 0;
    for (char **e = environ; e && *e; e++)
        if (env_allowed(*e)) out[k++] = *e;
    out[k] = NULL;
    return out;
}

/* Spawn @p exe as the helper @p role with @p fd as its fd 3, stdin /
 * stdout / stderr on /dev/null. The pid, or -1. */
static pid_t spawn_role(const char *exe, const char *role, int fd)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    if (posix_spawn_file_actions_init(&fa) != 0) return -1;
    if (posix_spawnattr_init(&at) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return -1;
    }
    int ok = 1;
    ok = ok && posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0) == 0;
    ok = ok && posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0) == 0;
    ok = ok && posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0) == 0;
    ok = ok && posix_spawn_file_actions_adddup2(&fa, fd, 3) == 0;
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    /* Apple: every descriptor not named above is closed in the child. */
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    ok = ok && posix_spawnattr_setflags(&at, flags) == 0;
    ok = ok && posix_spawnattr_setsigmask(&at, &none) == 0;
    ok = ok && posix_spawnattr_setsigdefault(&at, &all) == 0;
    char **env = ok ? helper_env() : NULL;
    pid_t pid = -1;
    if (ok && env) {
        /* posix_spawn takes char *const argv[]: owned copies. */
        char a0[PATH_MAX], a1[] = HL_GPU_HELPER_ARGV1, a2[16];
        if (strlen(exe) < sizeof a0 && strlen(role) < sizeof a2) {
            memcpy(a0, exe, strlen(exe) + 1);
            memcpy(a2, role, strlen(role) + 1);
            char *argv[] = { a0, a1, a2, NULL };
            if (posix_spawn(&pid, exe, &fa, &at, argv, env) != 0)
                pid = -1;
        }
    }
    free(env);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return pid;
}

/* This process's binary, as a path posix_spawn takes. On Linux
 * /proc/self/exe itself: it runs the image this process runs even if the
 * file was replaced since. */
static int self_exe(char *out, size_t sz)
{
#if defined(__linux__)
    if (sz < sizeof "/proc/self/exe") return -1;
    memcpy(out, "/proc/self/exe", sizeof "/proc/self/exe");
    return 0;
#elif defined(__APPLE__)
    char raw[PATH_MAX];
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0) return -1;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return -1;
    if (strlen(real) >= sz) return -1;
    memcpy(out, real, strlen(real) + 1);
    return 0;
#else
    (void)out; (void)sz;
    return -1;
#endif
}

/* Close every descriptor above 3 (the helper inherits only 0-3). */
static void close_extra_fds(void)
{
#if defined(__linux__) && defined(__NR_close_range)
    if (syscall(__NR_close_range, 4u, ~0u, 0u) == 0)
        return;
#endif
    struct rlimit rl;
    long max = 4096;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        rl.rlim_cur < 65536)
        max = (long)rl.rlim_cur;
    for (long fd = 4; fd < max; fd++)
        (void)close((int)fd);
}

/* Reap @p pid within @p ms (it was killed, or is exiting). 1 = reaped (its
 * status in *status), 0 = still there. */
static int reap(pid_t pid, int *status, uint32_t ms)
{
    uint64_t deadline = mono_ms() + ms;
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) return 1;
        if (r < 0 && errno != EINTR) return 1;   /* not ours any more */
        if (mono_ms() >= deadline) return 0;
        struct timespec ts = { 0, 5 * 1000000L };
        nanosleep(&ts, NULL);
    }
}

/* ── Client (the server) ──────────────────────────────────────────── */

static int      g_fd = -1;          /* the supervisor's socket */
static pid_t    g_pid = -1;
static uint32_t g_next_id = 1;
static HlGpuHelperStats g_stats;

int hl_gpu_helper_start(const char *exe)
{
    if (g_fd >= 0) return 0;
    char self[PATH_MAX];
    if (!exe) {
        if (self_exe(self, sizeof self) != 0) {
            log_warn("[hull:gpu] compile helper: cannot resolve this binary");
            return -1;
        }
        exe = self;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    pid_t pid = spawn_role(exe, "supervisor", sv[1]);
    close(sv[1]);
    if (pid < 0) {
        close(sv[0]);
        log_warn("[hull:gpu] compile helper: spawn failed: %s",
                 strerror(errno));
        return -1;
    }
    set_nonblock(sv[0]);
    set_nosigpipe(sv[0]);
    g_fd = sv[0];
    g_pid = pid;
    return 0;
}

int hl_gpu_helper_running(void)
{
    return g_fd >= 0;
}

void hl_gpu_helper_stop(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    if (g_pid > 0) {
        int st;
        /* It exits on the EOF, killing a running worker first; give it a
         * moment, then leave it (it will exit; init reaps it). */
        (void)reap(g_pid, &st, 200);
        g_pid = -1;
    }
}

/* The supervisor misbehaved or vanished: drop it; dynamic compiles fail
 * closed from now on (the sandboxed server cannot spawn another). */
static void helper_lost(const char *why)
{
    log_error("[hull:gpu] compile helper lost (%s): dynamic shader source "
              "is refused from now on", why);
    hl_gpu_helper_stop();
}

/* ── Verdict cache ───────────────────────────────────────────────── */

typedef struct {
    uint8_t key[32];
    int     status;
    int     used;
} Verdict;

static Verdict  g_cache[HL_GPU_HELPER_CACHE_ENTRIES];
static unsigned g_cache_next;
static int      g_disk_off;

void hl_gpu_helper_test_reset_cache(int disk_off)
{
    memset(g_cache, 0, sizeof g_cache);
    g_cache_next = 0;
    g_disk_off = disk_off ? 1 : 0;
}

void hl_gpu_helper_stats(HlGpuHelperStats *out)
{
    if (out) *out = g_stats;
}

static void hex32(const uint8_t in[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i]     = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[64] = '\0';
}

#if defined(__x86_64__)
#define HL_HELPER_ARCH "x86_64"
#elif defined(__aarch64__)
#define HL_HELPER_ARCH "aarch64"
#else
#define HL_HELPER_ARCH "other"
#endif

/* The verdict's key: the source, the device it was checked for and
 * everything that decides how it compiles (Hull's version - and with it
 * wgpu / naga's -, the backend, the arch). */
static int verdict_key(const char *wgsl, size_t len, int device,
                       const char *adapter, uint8_t out[32])
{
    uint8_t src[32];
    if (hl_cap_crypto_sha256(wgsl, len, src) != 0) return -1;
    char hex[65];
    hex32(src, hex);
    char buf[1024];
    const HlGpuBackend *be = helper_backend();
    int n = snprintf(buf, sizeof buf, "hull-gpu-verdict-v1|%s|%s|%s|%d|%s|%s",
                     HL_VERSION, be ? be->name : "-", HL_HELPER_ARCH,
                     device, adapter ? adapter : "-", hex);
    if (n < 0 || (size_t)n >= sizeof buf) return -1;
    return hl_cap_crypto_sha256(buf, (size_t)n, out);
}

static int cache_lookup(const uint8_t key[32], const char hex[65])
{
    for (size_t i = 0; i < HL_GPU_HELPER_CACHE_ENTRIES; i++)
        if (g_cache[i].used && memcmp(g_cache[i].key, key, 32) == 0)
            return g_cache[i].status;
    if (!g_disk_off && hl_runtime_cache_gpu_verdict_get(hex) == 0)
        return HL_GPU_HELPER_OK;
    return -1;
}

static void cache_store(const uint8_t key[32], const char hex[65], int status)
{
    Verdict *v = &g_cache[g_cache_next++ % HL_GPU_HELPER_CACHE_ENTRIES];
    memcpy(v->key, key, 32);
    v->status = status;
    v->used = 1;
    /* Only a clean compile is kept across restarts: a timeout or a crash
     * may be the host's load, not the source. */
    if (status == HL_GPU_HELPER_OK && !g_disk_off)
        hl_runtime_cache_gpu_verdict_put(hex);
}

/* ── The round trip ──────────────────────────────────────────────── */

static int run_stopped(void)
{
#ifdef HL_ENABLE_DB
    return hl_db_budget_charge(0) != 0;
#else
    return 0;
#endif
}

static int round_trip(const char *wgsl, size_t len, int device)
{
    if (g_fd < 0) return HL_GPU_HELPER_UNAVAILABLE;
    uint32_t id = g_next_id++;
    if (g_next_id == 0) g_next_id = 1;
    uint32_t compile_ms = helper_compile_ms();
    uint64_t deadline = mono_ms() + helper_init_ms() + compile_ms +
                        HL_GPU_HELPER_MARGIN_MS;
    uint8_t pre[8];
    put32(pre, compile_ms);
    put32(pre + 4, (uint32_t)device);
    if (write_frame(g_fd, HGC_REQ, id, pre, sizeof pre, wgsl, len,
                    deadline) != 0) {
        helper_lost("request not sent");
        return HL_GPU_HELPER_UNAVAILABLE;
    }
    g_stats.round_trips++;
    FrameRd r = { .max = 8u + HL_GPU_HELPER_REPLY_MAX };
    int cancelled = 0;
    for (;;) {
        /* Wait in poll slices, asking the run between them: its deadline
         * or a tripped budget sends a cancel (the supervisor kills the
         * worker and answers CANCELLED). */
        uint64_t slice = mono_ms() + HL_GPU_COMPILE_POLL_MS;
        if (slice > deadline) slice = deadline;
        int rc = frame_read(g_fd, &r, slice);
        if (rc == FR_DONE) {
            int keep = r.type == HGC_RESP && r.len >= 8;
            uint32_t rid = r.id;
            int status = keep ? (int)get32(r.buf) : -1;
            uint32_t flags = keep ? get32(r.buf + 4) : 0;
            frame_reset(&r);
            if (!keep) {
                helper_lost("malformed reply");
                return HL_GPU_HELPER_UNAVAILABLE;
            }
            if (rid != id) continue;    /* a cancelled request's reply */
            if (status < HL_GPU_HELPER_OK || status > HL_GPU_HELPER_CANCELLED)
                status = HL_GPU_HELPER_BADREPLY;
            if ((status == HL_GPU_HELPER_OK || status == HL_GPU_HELPER_SHADER)
                && !(flags & HGC_FLAG_SANDBOXED)) {
                g_stats.unsandboxed++;
                if (g_stats.unsandboxed == 1)
                    log_warn("[hull:gpu] the compile helper could not apply "
                             "its OS sandbox on this host: it still runs in "
                             "its own process under its deadline and "
                             "rlimits");
            }
            return cancelled ? HL_GPU_HELPER_CANCELLED : status;
        }
        if (rc != FR_MORE) {
            frame_reset(&r);
            helper_lost(rc == FR_EOF ? "it exited" : "bad frame");
            return HL_GPU_HELPER_UNAVAILABLE;
        }
        if (mono_ms() >= deadline) {
            frame_reset(&r);
            helper_lost("no reply within its deadline");
            return HL_GPU_HELPER_UNAVAILABLE;
        }
        if (!cancelled && run_stopped()) {
            cancelled = 1;
            if (write_frame(g_fd, HGC_CANCEL, id, NULL, 0, NULL, 0,
                            mono_ms() + HL_GPU_HELPER_MARGIN_MS) != 0) {
                frame_reset(&r);
                helper_lost("cancel not sent");
                return HL_GPU_HELPER_CANCELLED;
            }
            /* Wait for the CANCELLED reply (bounded by the deadline), so
             * the next request does not queue behind a running worker. */
        }
    }
}

int hl_gpu_helper_check(const char *wgsl, size_t len, int device,
                        const char *adapter)
{
    if (!wgsl || len > HL_GPU_WGSL_MAX_BYTES)
        return HL_GPU_HELPER_BADREPLY;
    uint8_t key[32];
    char hex[65];
    if (verdict_key(wgsl, len, device, adapter, key) != 0)
        return HL_GPU_HELPER_UNAVAILABLE;
    hex32(key, hex);
    int hit = cache_lookup(key, hex);
    if (hit >= 0) {
        g_stats.cache_hits++;
        return hit;
    }
    if (g_fd < 0)
        return HL_GPU_HELPER_UNAVAILABLE;
#ifdef HL_ENABLE_DB
    /* The floor, before the round trip; the wait itself is not charged
     * (it is the helper's time) - see HL_GPU_HELPER_UNITS. */
    if (hl_db_budget_charge((int64_t)HL_GPU_HELPER_UNITS) != 0)
        return HL_GPU_HELPER_CANCELLED;
#endif
    int status = round_trip(wgsl, len, device);
    if (status == HL_GPU_HELPER_OK || status == HL_GPU_HELPER_SHADER ||
        status == HL_GPU_HELPER_TIMEOUT || status == HL_GPU_HELPER_CRASHED ||
        status == HL_GPU_HELPER_BADREPLY)
        cache_store(key, hex, status);
    return status;
}

/* ── Supervisor ──────────────────────────────────────────────────── */

typedef struct {
    pid_t pid;
    int   fd;
} Worker;

/* sup_wait_worker's "a frame arrived": outside every HlGpuHelperStatus. */
#define SUP_FRAME 100

static void worker_kill(Worker *w)
{
    if (w->pid > 0) {
        (void)kill(w->pid, SIGKILL);
        int st;
        (void)reap(w->pid, &st, 2000);
        w->pid = -1;
    }
    if (w->fd >= 0) {
        close(w->fd);
        w->fd = -1;
    }
}

/* The status a worker that closed its socket without a reply gets. */
static int worker_gone(Worker *w)
{
    int st = 0;
    int reaped = w->pid > 0 && reap(w->pid, &st, 500);
    if (w->pid > 0 && !reaped) (void)kill(w->pid, SIGKILL);
    if (!reaped && w->pid > 0) (void)reap(w->pid, &st, 2000);
    w->pid = -1;
    close(w->fd);
    w->fd = -1;
    return HL_GPU_HELPER_CRASHED;
}

/* Wait for one frame from the worker, until @p deadline, watching the
 * server's socket for a cancel of request @p id or its EOF. Returns
 * SUP_FRAME (a frame in *wr), or a status: TIMEOUT, CRASHED, BADREPLY,
 * CANCELLED; *server_gone = 1 on the server's EOF / a protocol error. */
static int sup_wait_worker(int sfd, Worker *w, FrameRd *wr, uint32_t id,
                           uint64_t deadline, int *server_gone)
{
    FrameRd sr = { .max = 0 };   /* only CANCEL (empty) is taken while busy */
    for (;;) {
        struct pollfd pf[2] = {
            { .fd = w->fd, .events = POLLIN },
            { .fd = sfd,   .events = POLLIN },
        };
        int left = ms_left(deadline);
        if (left == 0) {
            worker_kill(w);
            return HL_GPU_HELPER_TIMEOUT;
        }
        int pr = poll(pf, 2, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) {
            worker_kill(w);
            return HL_GPU_HELPER_CRASHED;
        }
        if (pf[1].revents) {
            int rc = frame_read_some(sfd, &sr);
            if (rc == FR_DONE) {
                int is_cancel = sr.type == HGC_CANCEL;
                int ours = is_cancel && sr.id == id;
                frame_reset(&sr);
                if (ours) {
                    worker_kill(w);
                    return HL_GPU_HELPER_CANCELLED;
                }
                /* A stale cancel is harmless; anything else is not. */
                if (!is_cancel) {
                    *server_gone = 1;
                    worker_kill(w);
                    return HL_GPU_HELPER_CANCELLED;
                }
            } else if (rc != FR_MORE) {
                frame_reset(&sr);
                *server_gone = 1;
                worker_kill(w);
                return HL_GPU_HELPER_CANCELLED;
            }
        }
        if (pf[0].revents) {
            int rc = frame_read_some(w->fd, wr);
            if (rc == FR_DONE) return SUP_FRAME;
            if (rc == FR_OVERSIZE || rc == FR_ERROR) {
                frame_reset(wr);
                worker_kill(w);
                return HL_GPU_HELPER_BADREPLY;
            }
            if (rc == FR_EOF) {
                frame_reset(wr);
                return worker_gone(w);
            }
        }
    }
}

/* One compile: spawn a worker, wait for it to be ready, hand it the source,
 * wait for its verdict. */
static int sup_compile(int sfd, const char *exe, uint32_t id,
                       const uint8_t *req, uint32_t req_len, int *server_gone,
                       uint32_t *flags)
{
    uint32_t compile_ms = get32(req);
    if (compile_ms == 0 || compile_ms > 600000u) compile_ms = HL_GPU_COMPILE_TIMEOUT_MS;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return HL_GPU_HELPER_UNAVAILABLE;
    (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    Worker w = { .pid = spawn_role(exe, "worker", sv[1]), .fd = sv[0] };
    close(sv[1]);
    if (w.pid < 0) {
        close(sv[0]);
        return HL_GPU_HELPER_UNAVAILABLE;
    }
    set_nonblock(w.fd);
    set_nosigpipe(w.fd);

    FrameRd wr = { .max = HL_GPU_HELPER_REPLY_MAX };
    /* Start-up: spawn + backend init + sandbox, until READY. The init
     * deadline is the server's (it knows the configured value) less the
     * compile deadline and margin it budgeted; the supervisor uses the
     * default. */
    int rc = sup_wait_worker(sfd, &w, &wr, id,
                             mono_ms() + helper_init_ms(), server_gone);
    if (rc != SUP_FRAME) return rc;
    int ready = wr.type == HGC_READY && wr.len == 0;
    frame_reset(&wr);
    if (!ready) {
        worker_kill(&w);
        return HL_GPU_HELPER_BADREPLY;
    }
    /* The source, then the compile deadline. */
    uint64_t deadline = mono_ms() + compile_ms;
    if (write_frame(w.fd, HGC_REQ, id, req, req_len, NULL, 0, deadline) != 0) {
        worker_kill(&w);
        return mono_ms() >= deadline ? HL_GPU_HELPER_TIMEOUT
                                     : HL_GPU_HELPER_CRASHED;
    }
    rc = sup_wait_worker(sfd, &w, &wr, id, deadline, server_gone);
    if (rc != SUP_FRAME) return rc;
    int status = HL_GPU_HELPER_BADREPLY;
    if (wr.type == HGC_RESP && wr.id == id && wr.len >= 8) {
        int st = (int)get32(wr.buf);
        if (st == HL_GPU_HELPER_OK || st == HL_GPU_HELPER_SHADER ||
            st == HL_GPU_HELPER_UNAVAILABLE) {
            status = st;
            *flags = get32(wr.buf + 4) & HGC_FLAG_SANDBOXED;
        }
    }
    frame_reset(&wr);
    worker_kill(&w);   /* it exits after replying; reaped (or killed) here */
    return status;
}

static int supervisor_main(void)
{
    signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
    /* The server going away takes the supervisor (and so its worker)
     * with it even if the socket is somehow kept open. */
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
    (void)prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#endif
    close_extra_fds();
    struct rlimit core0 = { 0, 0 };
    (void)setrlimit(RLIMIT_CORE, &core0);
    char exe[PATH_MAX];
    if (self_exe(exe, sizeof exe) != 0) return 2;
    int sfd = 3;
    set_nonblock(sfd);
    set_nosigpipe(sfd);
    FrameRd r = { .max = HGC_REQ_MAX };
    for (;;) {
        int rc = frame_read(sfd, &r, 0);
        if (rc == FR_EOF) return 0;
        if (rc != FR_DONE) return 2;
        if (r.type == HGC_CANCEL) {      /* nothing running: ignore */
            frame_reset(&r);
            continue;
        }
        if (r.type != HGC_REQ || r.len < 8) return 2;
        int gone = 0;
        uint32_t flags = 0;
        int status = sup_compile(sfd, exe, r.id, r.buf, r.len, &gone, &flags);
        uint32_t id = r.id;
        frame_reset(&r);
        if (gone) return 0;
        uint8_t st[8];
        put32(st, (uint32_t)status);
        put32(st + 4, flags);
        if (write_frame(sfd, HGC_RESP, id, st, sizeof st, NULL, 0, 0) != 0)
            return 0;
    }
}

/* ── Worker ──────────────────────────────────────────────────────── */

/* The worker's sandbox, applied after the GPU backend is up (the device
 * and its driver connection are open) and before it reads the source. */
#if defined(__APPLE__)
extern int sandbox_init_with_parameters(const char *profile, uint64_t flags,
                                        const char **parameters,
                                        char **errorbuf);

static int worker_sandbox(void)
{
    /* Allow-default with the authority a compiler has no use for denied:
     * the network, spawning, and writing files (the Metal compiler's own
     * cache under the per-user temp tree aside); reading the user's home
     * is denied too. A deny-default profile would have to enumerate what
     * Metal and its XPC compiler service read, which changes between
     * macOS releases. */
    const char *profile =
        "(version 1)\n"
        "(allow default)\n"
        "(deny network*)\n"
        "(deny process-fork)\n"
        "(deny process-exec*)\n"
        "(deny file-write*)\n"
        "(allow file-write* (subpath \"/private/var/folders\"))\n"
        "(allow file-write-data (literal \"/dev/null\"))\n"
        "(deny file-read* (subpath (param \"HOME\")))\n";
    const char *home = hl_host_home();
    char rhome[PATH_MAX];
    if (!home || home[0] != '/' || !realpath(home, rhome))
        snprintf(rhome, sizeof rhome, "/nonexistent-home");
    const char *params[] = { "HOME", rhome, NULL };
    char *err = NULL;
    if (sandbox_init_with_parameters(profile, 0, params, &err) != 0) {
        free(err);
        return -1;
    }
    return 0;
}
#elif defined(__linux__)
#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#define __NR_landlock_add_rule       445
#define __NR_landlock_restrict_self  446
#endif
struct hl_ll_ruleset_attr { uint64_t handled_access_fs; };
struct hl_ll_path_beneath {
    uint64_t allowed_access;
    int32_t  parent_fd;
} __attribute__((packed));
#define HL_LL_RULE_PATH_BENEATH 1
#define HL_LL_EXECUTE    (1ull << 0)
#define HL_LL_WRITE_FILE (1ull << 1)
#define HL_LL_READ_FILE  (1ull << 2)
#define HL_LL_READ_DIR   (1ull << 3)
#define HL_LL_IOCTL_DEV  (1ull << 15)

static void ll_allow(int ruleset, const char *path, uint64_t access)
{
    int pfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pfd < 0) return;
    struct hl_ll_path_beneath pb = { access, pfd };
    (void)syscall(__NR_landlock_add_rule, ruleset, HL_LL_RULE_PATH_BENEATH,
                  &pb, 0u);
    close(pfd);
}

static int worker_sandbox(void)
{
    int rc = 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) rc = -1;
    /* Landlock: every filesystem right this ABI knows is handled; the only
     * grants are reading the system trees a GPU driver may still read
     * while it compiles (libraries, ICD / driver configuration, /proc,
     * /sys) and reading and writing existing device nodes. Nothing can be
     * created or removed anywhere, and the user's home (and the app's
     * files) cannot be read. It covers the calling thread and the compile
     * thread it starts (threads the backend started during init are not:
     * Landlock domains are per thread without TSYNC). */
    int abi = (int)syscall(__NR_landlock_create_ruleset, NULL, 0, 1u);
    if (abi >= 1) {
        uint64_t mask = (1ull << 13) - 1;          /* ABI 1: 13 rights */
        if (abi >= 2) mask |= 1ull << 13;          /* REFER */
        if (abi >= 3) mask |= 1ull << 14;          /* TRUNCATE */
        if (abi >= 5) mask |= HL_LL_IOCTL_DEV;
        struct hl_ll_ruleset_attr attr = { mask };
        int fd = (int)syscall(__NR_landlock_create_ruleset, &attr,
                              sizeof attr, 0u);
        if (fd >= 0) {
            static const char *const ro[] = { "/usr", "/lib", "/lib64",
                                              "/etc", "/opt", "/proc",
                                              "/sys", NULL };
            uint64_t rd = (HL_LL_EXECUTE | HL_LL_READ_FILE | HL_LL_READ_DIR)
                          & mask;
            for (int i = 0; ro[i]; i++)
                ll_allow(fd, ro[i], rd);
            ll_allow(fd, "/dev", (rd | HL_LL_WRITE_FILE | HL_LL_IOCTL_DEV)
                                 & mask);
        }
        if (fd < 0 || syscall(__NR_landlock_restrict_self, fd, 0u) != 0)
            rc = -1;
        if (fd >= 0) close(fd);
    } else {
        rc = -1;
    }
    /* seccomp: no exec, no non-local socket (a compiler needs neither). */
#if defined(__x86_64__)
#define HL_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define HL_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif
#ifdef HL_AUDIT_ARCH
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        /* Another ABI (i386 / compat): the worker uses none - refused. */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, HL_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        /* x32 numbers carry bit 30 on the x86_64 arch: refused. */
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 4, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execve, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execveat, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 2, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
    };
    struct sock_fprog prog = { (unsigned short)(sizeof f / sizeof f[0]), f };
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) rc = -1;
#endif
    /* No new processes for this user from here (fork / clone of a process
     * fails; threads are not counted against it). */
    struct rlimit np = { 0, 0 };
    (void)setrlimit(RLIMIT_NPROC, &np);
    return rc;
}
#else
static int worker_sandbox(void) { return -1; }
#endif

typedef struct {
    const HlGpuBackend *be;
    HlGpuDevice        *dev;
    const char         *wgsl;
    size_t              len;
    int                 rc;
} CompileArg;

static void *worker_compile_main(void *p)
{
    CompileArg *a = p;
    HlGpuPipeline out;
    memset(&out, 0, sizeof out);
    a->rc = a->be->compile(a->dev, "helper", a->wgsl, a->len, &out);
    if (a->rc == HL_GPU_OK && a->be->pipeline_destroy)
        a->be->pipeline_destroy(a->dev, &out);
    return NULL;
}

/* Compile on a thread of HL_GPU_HELPER_STACK_BYTES (smaller fallbacks if
 * that cannot be reserved). */
static int worker_compile(CompileArg *a)
{
    static const size_t sizes[] = { HL_GPU_HELPER_STACK_BYTES,
                                    (size_t)256 << 20, (size_t)64 << 20 };
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) continue;
        pthread_t th;
        int ok = pthread_attr_setstacksize(&attr, sizes[k]) == 0 &&
                 pthread_create(&th, &attr, worker_compile_main, a) == 0;
        pthread_attr_destroy(&attr);
        if (ok) {
            pthread_join(th, NULL);
            return a->rc;
        }
    }
    return HL_GPU_ERR_INTERNAL;
}

static uint32_t g_worker_flags;

static void worker_reply(uint32_t id, int status, const void *extra,
                         size_t extra_len)
{
    uint8_t st[8];
    put32(st, (uint32_t)status);
    put32(st + 4, g_worker_flags);
    (void)write_frame(3, HGC_RESP, id, st, sizeof st, extra, extra_len, 0);
}

static int worker_main(void)
{
    signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    close_extra_fds();
    struct rlimit rl = { 0, 0 };
    (void)setrlimit(RLIMIT_CORE, &rl);
    /* CPU: the start-up and compile deadlines, rounded up, plus a second;
     * the supervisor kills at the wall-clock deadline first. */
    rlim_t cpu = (rlim_t)((HL_GPU_HELPER_INIT_TIMEOUT_MS +
                           HL_GPU_HELPER_COMPILE_TIMEOUT_MS) / 1000u + 2u);
    rl.rlim_cur = cpu;
    rl.rlim_max = cpu + 1;
    (void)setrlimit(RLIMIT_CPU, &rl);
#if defined(__linux__)
    struct rlimit cur;
    if (getrlimit(RLIMIT_DATA, &cur) == 0) {
        rlim_t want = (rlim_t)HL_GPU_HELPER_DATA_BYTES;
        if (cur.rlim_max != RLIM_INFINITY && cur.rlim_max < want)
            want = cur.rlim_max;
        rl.rlim_cur = rl.rlim_max = want;
        (void)setrlimit(RLIMIT_DATA, &rl);
    }
#endif
    int fd = 3;
    set_nosigpipe(fd);

    /* The GPU backend, before the sandbox (it opens the device). A failure
     * is reported after the request arrives, as UNAVAILABLE. */
    const HlGpuBackend *be = helper_backend();
    HlGpuCtx *ctx = calloc(1, sizeof *ctx);
    int have_dev = be && ctx && hl_cap_gpu_init(ctx, be) == HL_GPU_OK &&
                   ctx->device_count > 0;
    /* A sandbox that could not be applied (or only in part: no Landlock,
     * no seccomp) is reported in the reply's flags, and the server warns
     * once; the process boundary, the deadline and the rlimits still hold. */
    if (worker_sandbox() == 0)
        g_worker_flags |= HGC_FLAG_SANDBOXED;
    if (write_frame(fd, HGC_READY, 0, NULL, 0, NULL, 0, 0) != 0)
        _exit(2);
    FrameRd r = { .max = HGC_REQ_MAX };
    if (frame_read(fd, &r, 0) != FR_DONE || r.type != HGC_REQ || r.len < 8)
        _exit(2);
    uint32_t id = r.id;
    int device = (int)get32(r.buf + 4);

    /* Test hooks (HULL_TEST_GPU_HELPER): exercise the supervision without
     * an adversarial shader. Read here, after the sandbox, so they run on
     * hosts with no GPU too. */
    const char *hook = getenv("HULL_TEST_GPU_HELPER");
    if (hook && strcmp(hook, "crash") == 0) {
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
        _exit(3);
    }
    if (hook && strcmp(hook, "slow") == 0) {
        for (;;) pause();
    }
    if (hook && strcmp(hook, "oversize") == 0) {
        static char big[HL_GPU_HELPER_REPLY_MAX + 64];
        memset(big, 'x', sizeof big);
        worker_reply(id, HL_GPU_HELPER_OK, big, sizeof big);
        _exit(0);
    }
    if (hook && strcmp(hook, "silent") == 0)
        _exit(0);
    if (hook && strcmp(hook, "probe") == 0) {
        /* What the sandbox denies: reading the user's home, reaching the
         * network. OK when both are refused, SHADER when either worked. */
        int leaked = 0;
        const char *home = hl_host_home();
        if (home && home[0] == '/') {
            int hfd = open(home, O_RDONLY | O_DIRECTORY);
            if (hfd >= 0) { leaked = 1; close(hfd); }
        }
        int so = socket(AF_INET, SOCK_STREAM, 0);
        if (so >= 0) {
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons(9);
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(so, (struct sockaddr *)&sa, sizeof sa) == 0 ||
                errno == ECONNREFUSED)
                leaked = 1;      /* the connect reached the stack */
            close(so);
        }
        worker_reply(id, leaked ? HL_GPU_HELPER_SHADER : HL_GPU_HELPER_OK,
                     NULL, 0);
        _exit(0);
    }
    if (hook && strcmp(hook, "accept") == 0) {
        /* Unit tests whose in-process backend is a mock. */
        worker_reply(id, HL_GPU_HELPER_OK, NULL, 0);
        _exit(0);
    }

    if (!have_dev) {
        worker_reply(id, HL_GPU_HELPER_UNAVAILABLE, NULL, 0);
        _exit(0);
    }
    if (device < 0 || device >= ctx->device_count) device = 0;
    CompileArg a = { be, &ctx->devices[device], (const char *)r.buf + 8,
                     r.len - 8, HL_GPU_ERR_INTERNAL };
    int crc = worker_compile(&a);
    worker_reply(id, crc == HL_GPU_OK ? HL_GPU_HELPER_OK : HL_GPU_HELPER_SHADER,
                 NULL, 0);
    /* No teardown: the process is about to go, and freeing the backend can
     * take longer than the compile. */
    _exit(0);
}

int hl_gpu_helper_main(const char *role)
{
    if (role && strcmp(role, "supervisor") == 0)
        return supervisor_main();
    if (role && strcmp(role, "worker") == 0)
        return worker_main();
    return 2;
}

#else /* !HL_GPU_HELPER_SUPPORTED: cosmo / Windows (GPU is native-only) */

int hl_gpu_helper_main(const char *role) { (void)role; return 2; }
int hl_gpu_helper_start(const char *exe) { (void)exe; return -1; }
int hl_gpu_helper_running(void) { return 0; }
void hl_gpu_helper_stop(void) {}
int hl_gpu_helper_check(const char *wgsl, size_t len, int device,
                        const char *adapter)
{
    (void)wgsl; (void)len; (void)device; (void)adapter;
    return HL_GPU_HELPER_UNAVAILABLE;
}
void hl_gpu_helper_test_reset_cache(int disk_off) { (void)disk_off; }
void hl_gpu_helper_stats(HlGpuHelperStats *out)
{
    if (out) memset(out, 0, sizeof *out);
}

#endif
