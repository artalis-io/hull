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
 *   REQ    deadline_ms u32 | device i32 | stack_mib u32 | mem_mib u32 |
 *          WGSL bytes                               (<= 16 + 1 MiB)
 *   CANCEL (empty)                                  server -> supervisor
 *   READY  (empty)                                  worker -> supervisor
 *   RESP   status i32 | flags u32 | detail          (<= HL_GPU_HELPER_REPLY_MAX)
 * (flags bit 0: the worker's OS sandbox was applied.) The supervisor's RESP
 * to the server adds peak_kib u32: the worker's peak memory it observed.
 * stack_mib / mem_mib are the server's asks (0 = the defaults); the
 * supervisor clamps both to HL_GPU_HELPER_STACK_BYTES / _MEM_BYTES.
 * A frame of another type, or longer than its cap, is a protocol error.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/gpu_helper.h"
#include "hull/cap/gpu.h"
#include "hull/cap/crypto.h"
#include "hull/limits/gpu.h"
#include "hull/shared/child_channel.h"
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
#include <libproc.h>
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
static uint64_t g_test_mem_cap;

void hl_gpu_helper_test_set_timeouts(uint32_t init_ms, uint32_t compile_ms)
{
    g_test_init_ms = init_ms;
    g_test_compile_ms = compile_ms;
}

void hl_gpu_helper_test_set_mem_cap(uint64_t bytes)
{
    g_test_mem_cap = bytes;
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

#define HGC_MAGIC   0x32434748u   /* "HGC2" little-endian */
#define HGC_REQ     1u
#define HGC_CANCEL  2u
#define HGC_RESP    3u
#define HGC_READY   4u
#define HGC_REQ_PRE 16u           /* deadline, device, stack, memory */
#define HGC_REQ_MAX ((uint32_t)(HGC_REQ_PRE + HL_GPU_WGSL_MAX_BYTES))
#define HGC_FLAG_SANDBOXED 0x1u

static int write_frame(int fd, uint32_t type, uint32_t id,
                       const void *a, size_t alen, const void *b, size_t blen,
                       uint64_t deadline)
{
    return hl_chan_write_frame(fd, HGC_MAGIC, type, id, a, alen, b, blen,
                               deadline);
}

/* ── Spawning ─────────────────────────────────────────────────────── */

/* The environment a helper gets: what the GPU stack reads (Vulkan / Mesa /
 * wgpu knobs, the cache and runtime dirs, the loader path) and, in a build
 * with the test hooks, the hook - never the server's secrets
 * (manifest.secrets, DSN passwords). */
static int env_allowed(const char *kv)
{
    static const char *const prefixes[] = { "VK_", "MESA_", "WGPU_", "MTL_",
                                            "__GLX_", NULL };
    static const char *const names[] = { "HOME", "TMPDIR", "XDG_CACHE_HOME",
                                         "XDG_RUNTIME_DIR", "LD_LIBRARY_PATH",
#ifdef HL_GPU_HELPER_TEST_HOOKS
                                         "HULL_TEST_GPU_HELPER",
#endif
                                         NULL };
    for (int i = 0; prefixes[i]; i++)
        if (strncmp(kv, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    for (int i = 0; names[i]; i++) {
        size_t n = strlen(names[i]);
        if (strncmp(kv, names[i], n) == 0 && kv[n] == '=') return 1;
    }
    return 0;
}

/* Spawn @p exe as the helper @p role with @p fd as its fd 3, stdin /
 * stdout / stderr on /dev/null. The pid, or -1. */
static pid_t spawn_role(const char *exe, const char *role, int fd)
{
    /* posix_spawn takes char *const argv[]: owned copies. */
    // cppcheck-suppress variableScope
    char a0[PATH_MAX], a1[] = HL_GPU_HELPER_ARGV1, a2[16];
    if (strlen(exe) >= sizeof a0 || strlen(role) >= sizeof a2) return -1;
    memcpy(a0, exe, strlen(exe) + 1);
    memcpy(a2, role, strlen(role) + 1);
    char *argv[] = { a0, a1, a2, NULL };
    return hl_chan_spawn(exe, argv, fd, env_allowed);
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
        // cppcheck-suppress knownConditionTrueFalse
        if (hl_chan_self_exe(self, sizeof self) != 0) {
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
    hl_chan_set_nonblock(sv[0]);
    hl_chan_set_nosigpipe(sv[0]);
    g_fd = sv[0];
    g_pid = pid;
    return 0;
}

int hl_gpu_helper_running(void)
{
    return g_fd >= 0;
}

/* A stopped supervisor not reaped within hl_gpu_helper_stop's moment (C
 * audit L16): Hull installs no SIGCHLD handler, so it stayed a zombie for
 * the server's life. Reaped without waiting at the next check. */
static pid_t g_pid_exiting = -1;

static void helper_reap_exiting(void)
{
    if (g_pid_exiting > 0) {
        int st;
        if (hl_chan_reap(g_pid_exiting, &st, 0, NULL)) g_pid_exiting = -1;
    }
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
         * moment, then reap it later (helper_reap_exiting). */
        if (!hl_chan_reap(g_pid, &st, 200, NULL)) {
            helper_reap_exiting();         /* an older one, if any */
            if (g_pid_exiting <= 0) g_pid_exiting = g_pid;
        }
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
 * wgpu / naga's -, the backend, the arch, the worker's stack and memory
 * cap). */
static int verdict_key(const char *wgsl, size_t len, int device,
                       const char *adapter, uint32_t stack_mib,
                       uint32_t mem_mib, uint8_t out[32])
{
    uint8_t src[32];
    if (hl_cap_crypto_sha256(wgsl, len, src) != 0) return -1;
    char hex[65];
    hex32(src, hex);
    char buf[1024];
    const HlGpuBackend *be = helper_backend();
    int n = snprintf(buf, sizeof buf,
                     "hull-gpu-verdict-v2|%s|%s|%s|%d|%s|%u|%u|%s",
                     HL_VERSION, be ? be->name : "-", HL_HELPER_ARCH,
                     device, adapter ? adapter : "-", (unsigned)stack_mib,
                     (unsigned)mem_mib, hex);
    if (n < 0 || (size_t)n >= sizeof buf) return -1;
    return hl_cap_crypto_sha256(buf, (size_t)n, out);
}

static int cache_lookup(const uint8_t key[32], const char hex[65])
{
    for (size_t i = 0; i < HL_GPU_HELPER_CACHE_ENTRIES; i++)
        if (g_cache[i].used && memcmp(g_cache[i].key, key, 32) == 0)
            return g_cache[i].status;
    // cppcheck-suppress knownConditionTrueFalse
    if (!g_disk_off && hl_runtime_cache_gpu_verdict_get(hex) == 0)
        return HL_GPU_HELPER_OK;
    return -1;
}

static void cache_store(const uint8_t key[32], int status)
{
    Verdict *v = &g_cache[g_cache_next++ % HL_GPU_HELPER_CACHE_ENTRIES];
    memcpy(v->key, key, 32);
    v->status = status;
    v->used = 1;
    /* Memory only (C audit L6). Every source reaching the helper is runtime
     * source a built binary opted into, possibly request text: writing its
     * OK verdict to the sealed disk cache minted one file per distinct
     * source, without bound - the shape audit 39 closed for
     * template.render_string. The disk side is read (cache_lookup) but no
     * longer written from here. */
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

static int round_trip(const char *wgsl, size_t len, int device,
                      uint32_t stack_mib, uint32_t mem_mib)
{
    if (g_fd < 0) return HL_GPU_HELPER_UNAVAILABLE;
    uint32_t id = g_next_id++;
    if (g_next_id == 0) g_next_id = 1;
    uint32_t compile_ms = helper_compile_ms();
    uint64_t deadline = hl_chan_mono_ms() + helper_init_ms() + compile_ms +
                        HL_GPU_HELPER_MARGIN_MS;
    uint8_t pre[HGC_REQ_PRE];
    hl_chan_put32(pre, compile_ms);
    hl_chan_put32(pre + 4, (uint32_t)device);
    hl_chan_put32(pre + 8, stack_mib);
    hl_chan_put32(pre + 12, mem_mib);
    if (write_frame(g_fd, HGC_REQ, id, pre, sizeof pre, wgsl, len,
                    deadline) != 0) {
        helper_lost("request not sent");
        return HL_GPU_HELPER_UNAVAILABLE;
    }
    g_stats.round_trips++;
    HlChanFrame r = { .magic = HGC_MAGIC, .max = 8u + HL_GPU_HELPER_REPLY_MAX };
    int cancelled = 0;
    for (;;) {
        /* Wait in poll slices, asking the run between them: its deadline
         * or a tripped budget sends a cancel (the supervisor kills the
         * worker and answers CANCELLED). */
        uint64_t slice = hl_chan_mono_ms() + HL_GPU_COMPILE_POLL_MS;
        if (slice > deadline) slice = deadline;
        int rc = hl_chan_frame_read(g_fd, &r, slice);
        if (rc == HL_CHAN_DONE) {
            int keep = r.type == HGC_RESP && r.len >= 8;
            uint32_t rid = r.id;
            int status = keep ? (int)hl_chan_get32(r.buf) : -1;
            uint32_t flags = keep ? hl_chan_get32(r.buf + 4) : 0;
            uint64_t peak = keep && r.len >= 12
                ? (uint64_t)hl_chan_get32(r.buf + 8) * 1024u : 0;
            hl_chan_frame_reset(&r);
            if (!keep) {
                helper_lost("malformed reply");
                return HL_GPU_HELPER_UNAVAILABLE;
            }
            if (rid != id) continue;    /* a cancelled request's reply */
            g_stats.last_peak_bytes = peak;
            if (peak > g_stats.peak_bytes) g_stats.peak_bytes = peak;
            if (status < HL_GPU_HELPER_OK || status > HL_GPU_HELPER_MEMORY)
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
        if (rc != HL_CHAN_MORE) {
            hl_chan_frame_reset(&r);
            helper_lost(rc == HL_CHAN_EOF ? "it exited" : "bad frame");
            return HL_GPU_HELPER_UNAVAILABLE;
        }
        if (hl_chan_mono_ms() >= deadline) {
            hl_chan_frame_reset(&r);
            helper_lost("no reply within its deadline");
            return HL_GPU_HELPER_UNAVAILABLE;
        }
        // cppcheck-suppress knownConditionTrueFalse
        if (!cancelled && run_stopped()) {
            cancelled = 1;
            if (write_frame(g_fd, HGC_CANCEL, id, NULL, 0, NULL, 0,
                            hl_chan_mono_ms() + HL_GPU_HELPER_MARGIN_MS) != 0) {
                hl_chan_frame_reset(&r);
                helper_lost("cancel not sent");
                return HL_GPU_HELPER_CANCELLED;
            }
            /* Wait for the CANCELLED reply (bounded by the deadline), so
             * the next request does not queue behind a running worker. */
        }
    }
}

/* The worker's stack / memory in MiB: the caller's ask, never above the
 * compiled caps (the supervisor clamps them again). */
static uint32_t helper_stack_mib(size_t stack_bytes)
{
    size_t b = stack_bytes && stack_bytes < HL_GPU_HELPER_STACK_BYTES
        ? stack_bytes : HL_GPU_HELPER_STACK_BYTES;
    return (uint32_t)(b >> 20);
}

static uint32_t helper_mem_mib(void)
{
    uint64_t b = g_test_mem_cap && g_test_mem_cap < HL_GPU_HELPER_MEM_BYTES
        ? g_test_mem_cap : HL_GPU_HELPER_MEM_BYTES;
    return (uint32_t)(b >> 20);
}

int hl_gpu_helper_check(const char *wgsl, size_t len, int device,
                        const char *adapter, size_t stack_bytes)
{
    if (!wgsl || len > HL_GPU_WGSL_MAX_BYTES)
        return HL_GPU_HELPER_BADREPLY;
    uint32_t stack_mib = helper_stack_mib(stack_bytes);
    uint32_t mem_mib = helper_mem_mib();
    if (stack_mib == 0)
        return HL_GPU_HELPER_UNAVAILABLE;   /* no stack worth checking on */
    uint8_t key[32];
    char hex[65];
    helper_reap_exiting();
    if (verdict_key(wgsl, len, device, adapter, stack_mib, mem_mib, key) != 0)
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
    int status = round_trip(wgsl, len, device, stack_mib, mem_mib);
    if (status == HL_GPU_HELPER_OK || status == HL_GPU_HELPER_SHADER ||
        status == HL_GPU_HELPER_TIMEOUT || status == HL_GPU_HELPER_CRASHED ||
        status == HL_GPU_HELPER_BADREPLY || status == HL_GPU_HELPER_MEMORY)
        cache_store(key, status);
    return status;
}

/* ── Supervisor ──────────────────────────────────────────────────── */

typedef struct {
    pid_t    pid;
    int      fd;
    uint64_t mem_cap;   /* bytes: killed above it (macOS polling) */
    uint64_t peak;      /* the largest memory seen: footprint / RSS, bytes */
} Worker;

/* The worker's memory as the supervisor can see it while it runs: on macOS
 * its peak physical footprint (dirty memory, compressed included); 0 where
 * it cannot be read (Linux relies on RLIMIT_DATA in the worker and reads
 * the peak RSS when it reaps it). */
static uint64_t worker_mem_now(pid_t pid)
{
#if defined(__APPLE__)
    struct rusage_info_v4 ri;
    if (pid > 0 &&
        proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&ri) == 0)
        return ri.ri_lifetime_max_phys_footprint;
#else
    (void)pid;
#endif
    return 0;
}

static void worker_note_rss(Worker *w, uint64_t rss)
{
    if (rss > w->peak) w->peak = rss;
}

/* sup_wait_worker's "a frame arrived": outside every HlGpuHelperStatus. */
#define SUP_FRAME 100

/* How long the supervisor waits for a killed worker to be reaped before it
 * answers the server (C audit L7). The server's deadline is init + compile +
 * HL_GPU_HELPER_MARGIN_MS; a worker slow to die after SIGKILL (inside a
 * driver call) used to be reaped for up to 2 s - 2.5 s on the crash path -
 * BEFORE the verdict was written, so the reply could miss the server's
 * deadline, which then dropped the helper for the process's life. A worker
 * not reaped by now is reaped later (sup_reap_pending), after the reply. */
#define SUP_REAP_QUICK_MS 50u
#define SUP_REAP_PENDING_MAX 8

static pid_t g_reap_pending[SUP_REAP_PENDING_MAX];
static int   g_reap_pending_n;

/* Test hook: a worker that is slow to die (the quick reap never sees it). */
static int sup_slowdie(void)
{
#ifdef HL_GPU_HELPER_TEST_HOOKS
    const char *hook = getenv("HULL_TEST_GPU_HELPER");
    return hook && strcmp(hook, "slowdie") == 0;
#else
    return 0;
#endif
}

/* Reap the killed workers still pending: without waiting (@p block 0), or
 * each for up to 2 s. */
static void sup_reap_pending(int block)
{
    int keep = 0;
    for (int i = 0; i < g_reap_pending_n; i++) {
        int st;
        pid_t pid = g_reap_pending[i];
        // cppcheck-suppress knownConditionTrueFalse
        if (block && sup_slowdie()) {
            struct timespec ts = { 2, 500 * 1000000L };   /* its slow death */
            nanosleep(&ts, NULL);
        }
        if (!hl_chan_reap(pid, &st, block ? 2000u : 0u, NULL))
            g_reap_pending[keep++] = pid;
    }
    g_reap_pending_n = keep;
}

/* Reap a worker that was just killed, for at most SUP_REAP_QUICK_MS; one
 * that is not gone by then is queued for sup_reap_pending. */
static void worker_reap_quick(Worker *w)
{
    int st;
    uint64_t rss = 0;
    // cppcheck-suppress knownConditionTrueFalse
    int reaped = !sup_slowdie() &&
                 hl_chan_reap(w->pid, &st, SUP_REAP_QUICK_MS, &rss);
    worker_note_rss(w, rss);
    if (!reaped) {
        if (g_reap_pending_n == SUP_REAP_PENDING_MAX) sup_reap_pending(1);
        if (g_reap_pending_n < SUP_REAP_PENDING_MAX)
            g_reap_pending[g_reap_pending_n++] = w->pid;
        /* else left to init when this supervisor exits */
    }
    w->pid = -1;
}

static void worker_kill(Worker *w)
{
    if (w->pid > 0) {
        worker_note_rss(w, worker_mem_now(w->pid));
        (void)kill(w->pid, SIGKILL);
        worker_reap_quick(w);
    }
    if (w->fd >= 0) {
        close(w->fd);
        w->fd = -1;
    }
}

/* The status a worker that closed its socket without a reply gets. */
static int worker_gone(Worker *w)
{
    if (w->pid > 0) {
        worker_note_rss(w, worker_mem_now(w->pid));
        (void)kill(w->pid, SIGKILL);   /* no-op when it has exited */
        worker_reap_quick(w);
    }
    close(w->fd);
    w->fd = -1;
    return HL_GPU_HELPER_CRASHED;
}

/* Wait for one frame from the worker, until @p deadline, watching the
 * server's socket for a cancel of request @p id or its EOF, and (macOS)
 * the worker's memory every HL_GPU_HELPER_MEM_POLL_MS. Returns SUP_FRAME
 * (a frame in *wr), or a status: TIMEOUT, CRASHED, BADREPLY, CANCELLED,
 * MEMORY; *server_gone = 1 on the server's EOF / a protocol error. */
static int sup_wait_worker(int sfd, Worker *w, HlChanFrame *wr, uint32_t id,
                           uint64_t deadline, int *server_gone)
{
    HlChanFrame sr = { .magic = HGC_MAGIC, .max = 0 };   /* only CANCEL (empty) is taken while busy */
    for (;;) {
        struct pollfd pf[2] = {
            { .fd = w->fd, .events = POLLIN },
            { .fd = sfd,   .events = POLLIN },
        };
        int left = hl_chan_ms_left(deadline);
        if (left == 0) {
            worker_kill(w);
            return HL_GPU_HELPER_TIMEOUT;
        }
#if defined(__APPLE__)
        /* No data / address-space rlimit is enforced on macOS: watch the
         * footprint instead, often enough that an allocation burst between
         * two looks stays small next to the cap. */
        if (left < 0 || left > HL_GPU_HELPER_MEM_POLL_MS)
            left = HL_GPU_HELPER_MEM_POLL_MS;
        uint64_t mem = worker_mem_now(w->pid);
        worker_note_rss(w, mem);
        if (w->mem_cap && mem > w->mem_cap) {
            worker_kill(w);
            return HL_GPU_HELPER_MEMORY;
        }
#endif
        int pr = poll(pf, 2, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) {
            worker_kill(w);
            return HL_GPU_HELPER_CRASHED;
        }
        if (pf[1].revents) {
            int rc = hl_chan_frame_read_some(sfd, &sr);
            if (rc == HL_CHAN_DONE) {
                int is_cancel = sr.type == HGC_CANCEL;
                int ours = is_cancel && sr.id == id;
                hl_chan_frame_reset(&sr);
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
            } else if (rc != HL_CHAN_MORE) {
                hl_chan_frame_reset(&sr);
                *server_gone = 1;
                worker_kill(w);
                return HL_GPU_HELPER_CANCELLED;
            }
        }
        if (pf[0].revents) {
            int rc = hl_chan_frame_read_some(w->fd, wr);
            if (rc == HL_CHAN_DONE) return SUP_FRAME;
            if (rc == HL_CHAN_OVERSIZE || rc == HL_CHAN_ERROR) {
                hl_chan_frame_reset(wr);
                worker_kill(w);
                return HL_GPU_HELPER_BADREPLY;
            }
            if (rc == HL_CHAN_EOF) {
                hl_chan_frame_reset(wr);
                return worker_gone(w);
            }
        }
    }
}

/* One compile: spawn a worker, wait for it to be ready, hand it the source,
 * wait for its verdict. */
static int sup_compile_w(int sfd, Worker *w, uint32_t id, uint8_t *req,
                         uint32_t req_len, uint32_t compile_ms,
                         int *server_gone, uint32_t *flags);

static int sup_compile(int sfd, const char *exe, uint32_t id,
                       uint8_t *req, uint32_t req_len, int *server_gone,
                       uint32_t *flags, uint64_t *peak)
{
    uint32_t compile_ms = hl_chan_get32(req);
    if (compile_ms == 0 || compile_ms > 600000u) compile_ms = HL_GPU_COMPILE_TIMEOUT_MS;
    /* The stack and memory the worker gets: the server's asks, never above
     * the compiled caps; rewritten in the request the worker reads. */
    uint32_t stack_mib = hl_chan_get32(req + 8);
    uint32_t mem_mib = hl_chan_get32(req + 12);
    if (stack_mib == 0 || stack_mib > (uint32_t)(HL_GPU_HELPER_STACK_BYTES >> 20))
        stack_mib = (uint32_t)(HL_GPU_HELPER_STACK_BYTES >> 20);
    if (mem_mib == 0 || mem_mib > (uint32_t)(HL_GPU_HELPER_MEM_BYTES >> 20))
        mem_mib = (uint32_t)(HL_GPU_HELPER_MEM_BYTES >> 20);
    hl_chan_put32(req + 8, stack_mib);
    hl_chan_put32(req + 12, mem_mib);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return HL_GPU_HELPER_UNAVAILABLE;
    (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    Worker w = { .pid = spawn_role(exe, "worker", sv[1]), .fd = sv[0],
                 .mem_cap = (uint64_t)mem_mib << 20, .peak = 0 };
    close(sv[1]);
    if (w.pid < 0) {
        close(sv[0]);
        return HL_GPU_HELPER_UNAVAILABLE;
    }
    hl_chan_set_nonblock(w.fd);
    hl_chan_set_nosigpipe(w.fd);
    sup_reap_pending(0);   /* earlier workers that were slow to die */
    int rc = sup_compile_w(sfd, &w, id, req, req_len, compile_ms,
                           server_gone, flags);
    *peak = w.peak;
    return rc;
}

static int sup_compile_w(int sfd, Worker *w, uint32_t id, uint8_t *req,
                         uint32_t req_len, uint32_t compile_ms,
                         int *server_gone, uint32_t *flags)
{
    HlChanFrame wr = { .magic = HGC_MAGIC, .max = HL_GPU_HELPER_REPLY_MAX };
    /* Start-up: spawn + backend init + sandbox, until READY. The init
     * deadline is the server's (it knows the configured value) less the
     * compile deadline and margin it budgeted; the supervisor uses the
     * default. */
    int rc = sup_wait_worker(sfd, w, &wr, id,
                             hl_chan_mono_ms() + helper_init_ms(), server_gone);
    if (rc != SUP_FRAME) return rc;
    int ready = wr.type == HGC_READY && wr.len == 0;
    hl_chan_frame_reset(&wr);
    if (!ready) {
        worker_kill(w);
        return HL_GPU_HELPER_BADREPLY;
    }
    /* The source, then the compile deadline. */
    uint64_t deadline = hl_chan_mono_ms() + compile_ms;
    if (write_frame(w->fd, HGC_REQ, id, req, req_len, NULL, 0, deadline) != 0) {
        worker_kill(w);
        return hl_chan_mono_ms() >= deadline ? HL_GPU_HELPER_TIMEOUT
                                     : HL_GPU_HELPER_CRASHED;
    }
    rc = sup_wait_worker(sfd, w, &wr, id, deadline, server_gone);
    if (rc != SUP_FRAME) return rc;
    int status = HL_GPU_HELPER_BADREPLY;
    if (wr.type == HGC_RESP && wr.id == id && wr.len >= 8) {
        int st = (int)hl_chan_get32(wr.buf);
        if (st == HL_GPU_HELPER_OK || st == HL_GPU_HELPER_SHADER ||
            st == HL_GPU_HELPER_UNAVAILABLE) {
            status = st;
            *flags = hl_chan_get32(wr.buf + 4) & HGC_FLAG_SANDBOXED;
        }
    }
    hl_chan_frame_reset(&wr);
    worker_kill(w);   /* it exits after replying; reaped (or killed) here */
    /* macOS polls the footprint only while it waits: an allocation burst
     * between the last look and the reply may have passed the cap, which
     * the reap's footprint (worker_kill) shows. A clean verdict from a
     * worker over its cap is not clean (it would not fit the server's
     * compile either). Linux enforces the cap in the worker (RLIMIT_DATA)
     * and its peak is the reap's RSS, shared pages included, so the
     * recheck is macOS's. */
#if defined(__APPLE__)
    if (status == HL_GPU_HELPER_OK && w->mem_cap && w->peak > w->mem_cap)
        return HL_GPU_HELPER_MEMORY;
#endif
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
    hl_chan_close_extra_fds();
    struct rlimit core0 = { 0, 0 };
    (void)setrlimit(RLIMIT_CORE, &core0);
    char exe[PATH_MAX];
    // cppcheck-suppress knownConditionTrueFalse
    if (hl_chan_self_exe(exe, sizeof exe) != 0) return 2;
    int sfd = 3;
    hl_chan_set_nonblock(sfd);
    hl_chan_set_nosigpipe(sfd);
    HlChanFrame r = { .magic = HGC_MAGIC, .max = HGC_REQ_MAX };
    for (;;) {
        int rc = hl_chan_frame_read(sfd, &r, 0);
        if (rc != HL_CHAN_DONE) {
            hl_chan_frame_reset(&r);
            return rc == HL_CHAN_EOF ? 0 : 2;
        }
        if (r.type == HGC_CANCEL) {      /* nothing running: ignore */
            hl_chan_frame_reset(&r);
            continue;
        }
        if (r.type != HGC_REQ || r.len < HGC_REQ_PRE) {
            hl_chan_frame_reset(&r);
            return 2;
        }
        int gone = 0;
        uint32_t flags = 0;
        uint64_t peak = 0;
        int status = sup_compile(sfd, exe, r.id, r.buf, r.len, &gone, &flags,
                                 &peak);
        uint32_t id = r.id;
        hl_chan_frame_reset(&r);
        if (gone) return 0;
        uint8_t st[12];
        hl_chan_put32(st, (uint32_t)status);
        hl_chan_put32(st + 4, flags);
        uint64_t kib = peak >> 10;
        hl_chan_put32(st + 8, kib > UINT32_MAX ? UINT32_MAX : (uint32_t)kib);
        if (write_frame(sfd, HGC_RESP, id, st, sizeof st, NULL, 0, 0) != 0)
            return 0;
        /* The verdict is out: now the workers that were slow to die may
         * take their time (C audit L7). */
        sup_reap_pending(1);
    }
}

/* ── Worker ──────────────────────────────────────────────────────── */

/* The worker's sandbox, applied after the GPU backend is up (the device
 * and its driver connection are open) and before it reads the source. */
#if defined(__APPLE__)
extern void sandbox_free_error(char *errorbuf);
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
        if (err) sandbox_free_error(err);
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
/* ABI 4 added handled_access_net; an older kernel takes the first field
 * only (the size passed says which). */
struct hl_ll_ruleset_attr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;
};
#define HL_LL_NET_BIND_TCP    (1ull << 0)
#define HL_LL_NET_CONNECT_TCP (1ull << 1)
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup    425
#define __NR_io_uring_enter    426
#define __NR_io_uring_register 427
#endif
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
        /* ABI 4+: TCP bind and connect handled with no rule granting
         * either, so both are refused (seccomp below refuses non-local
         * sockets too; this also holds for a socket made some other way). */
        struct hl_ll_ruleset_attr attr = { mask, 0 };
        size_t attr_size = sizeof attr.handled_access_fs;
        if (abi >= 4) {
            attr.handled_access_net = HL_LL_NET_BIND_TCP |
                                      HL_LL_NET_CONNECT_TCP;
            attr_size = sizeof attr;
        }
        int fd = (int)syscall(__NR_landlock_create_ruleset, &attr,
                              attr_size, 0u);
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
    /* seccomp: no exec, no non-local socket, no io_uring (a compiler needs
     * none of them; IORING_OP_SOCKET would make a socket without the
     * socket() call the filter checks). */
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
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 7, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execve, 6, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execveat, 5, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_setup, 4, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_enter, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_register, 2, 0),
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

/* Compile on a thread of @p stack bytes, the request's (smaller fallbacks if
 * that cannot be reserved: a smaller stack only makes the verdict
 * stricter). */
static int worker_compile(CompileArg *a, size_t stack)
{
    const size_t sizes[] = { stack, (size_t)256 << 20, (size_t)64 << 20 };
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        if (k > 0 && sizes[k] >= stack) continue;
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

#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#    define HL_GPU_HELPER_SANITIZED 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define HL_GPU_HELPER_SANITIZED 1
#endif

/* Lower the worker's RLIMIT_DATA to @p bytes (Linux, non-sanitizer builds;
 * a no-op elsewhere). Only ever lowers: never above the hard limit the
 * worker was started with. */
static void worker_limit_data(uint64_t bytes)
{
#if defined(__linux__) && !defined(HL_GPU_HELPER_SANITIZED)
    struct rlimit cur, rl;
    if (getrlimit(RLIMIT_DATA, &cur) != 0) return;
    rlim_t want = (rlim_t)bytes;
    if (cur.rlim_max != RLIM_INFINITY && cur.rlim_max < want)
        want = cur.rlim_max;
    rl.rlim_cur = rl.rlim_max = want;
    (void)setrlimit(RLIMIT_DATA, &rl);
#else
    (void)bytes;
#endif
}

static void worker_reply(uint32_t id, int status, const void *extra,
                         size_t extra_len)
{
    uint8_t st[8];
    hl_chan_put32(st, (uint32_t)status);
    hl_chan_put32(st + 4, g_worker_flags);
    (void)write_frame(3, HGC_RESP, id, st, sizeof st, extra, extra_len, 0);
}

static int worker_main(void)
{
    signal(SIGPIPE, SIG_IGN);
#if defined(__linux__)
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    hl_chan_close_extra_fds();
    struct rlimit rl = { 0, 0 };
    (void)setrlimit(RLIMIT_CORE, &rl);
    /* CPU: the start-up and compile deadlines, rounded up, plus a second;
     * the supervisor kills at the wall-clock deadline first. */
    rlim_t cpu = (rlim_t)((HL_GPU_HELPER_INIT_TIMEOUT_MS +
                           HL_GPU_HELPER_COMPILE_TIMEOUT_MS) / 1000u + 2u);
    rl.rlim_cur = cpu;
    rl.rlim_max = cpu + 1;
    (void)setrlimit(RLIMIT_CPU, &rl);
    /* Memory (HL_GPU_HELPER_MEM_BYTES): on Linux RLIMIT_DATA, the cap plus
     * the compile stack (a thread stack is a private writable mapping,
     * counted as data); lowered to the request's values once it arrives.
     * RLIMIT_DATA counts every private mapping, so it refuses an ASan /
     * MSan worker its shadow-memory reservation and the worker dies at
     * start; sanitizer builds (tests only) run without it. macOS enforces
     * no data rlimit: the supervisor watches the footprint instead. */
    worker_limit_data((uint64_t)HL_GPU_HELPER_MEM_BYTES +
                      (uint64_t)HL_GPU_HELPER_STACK_BYTES);
    int fd = 3;
    hl_chan_set_nosigpipe(fd);

    /* The GPU backend, before the sandbox (it opens the device). A failure
     * is reported after the request arrives, as UNAVAILABLE. */
    const HlGpuBackend *be = helper_backend();
    HlGpuCtx *ctx = calloc(1, sizeof *ctx);
    int have_dev = be && ctx && hl_cap_gpu_init(ctx, be) == HL_GPU_OK &&
                   ctx->device_count > 0;
    /* A sandbox that could not be applied (or only in part: no Landlock,
     * no seccomp) is reported in the reply's flags, and the server warns
     * once; the process boundary, the deadline and the rlimits still hold. */
    // cppcheck-suppress knownConditionTrueFalse
    if (worker_sandbox() == 0)
        g_worker_flags |= HGC_FLAG_SANDBOXED;
    if (write_frame(fd, HGC_READY, 0, NULL, 0, NULL, 0, 0) != 0)
        _exit(2);
    HlChanFrame r = { .magic = HGC_MAGIC, .max = HGC_REQ_MAX };
    if (hl_chan_frame_read(fd, &r, 0) != HL_CHAN_DONE || r.type != HGC_REQ ||
        r.len < HGC_REQ_PRE)
        _exit(2);
    uint32_t id = r.id;
    int device = (int)hl_chan_get32(r.buf + 4);
    /* The supervisor clamped both to the compiled caps. */
    size_t stack = (size_t)hl_chan_get32(r.buf + 8) << 20;
    uint64_t mem = (uint64_t)hl_chan_get32(r.buf + 12) << 20;
    if (stack == 0 || stack > HL_GPU_HELPER_STACK_BYTES)
        stack = HL_GPU_HELPER_STACK_BYTES;
    if (mem == 0 || mem > HL_GPU_HELPER_MEM_BYTES)
        mem = HL_GPU_HELPER_MEM_BYTES;
    worker_limit_data(mem + (uint64_t)stack);

#ifdef HL_GPU_HELPER_TEST_HOOKS
    /* Test hooks (HULL_TEST_GPU_HELPER), compiled only into test builds
     * (-DHL_GPU_HELPER_TEST_HOOKS: the unit tests and the GPU e2e): they
     * exercise the supervision without an adversarial shader, and one of
     * them ("accept") vouches for any source, so a release binary must not
     * have them. Read here, after the sandbox, so they run on hosts with
     * no GPU too. */
    const char *hook = getenv("HULL_TEST_GPU_HELPER");
    if (hook && strcmp(hook, "crash") == 0) {
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
        // cppcheck-suppress unreachableCode
        _exit(3);
    }
    if (hook && (strcmp(hook, "slow") == 0 || strcmp(hook, "slowdie") == 0)) {
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
#if defined(__linux__)
        /* io_uring would make a socket (IORING_OP_SOCKET) without the
         * socket() call seccomp checks. */
        unsigned char params[120];
        memset(params, 0, sizeof params);
        long ur = syscall(__NR_io_uring_setup, 1u, params);
        if (ur >= 0) { leaked = 1; close((int)ur); }
#endif
        worker_reply(id, leaked ? HL_GPU_HELPER_SHADER : HL_GPU_HELPER_OK,
                     NULL, 0);
        _exit(0);
    }
    if (hook && strcmp(hook, "accept") == 0) {
        /* Unit tests whose in-process backend is a mock. */
        worker_reply(id, HL_GPU_HELPER_OK, NULL, 0);
        _exit(0);
    }
    if (hook && strcmp(hook, "alloc") == 0) {
        /* Memory past the cap, no shader involved: touched 16 MiB blocks
         * until an allocation fails (Linux, RLIMIT_DATA: abort, as an
         * allocator that cannot allocate does) or the supervisor kills the
         * worker (macOS). Reaching 4x the cap means nothing stopped it. */
        uint64_t total = 0;
        while (total < 4 * (mem + (uint64_t)stack)) {
            /* Touched through a volatile pointer: an unread malloc'd
             * block may otherwise be optimised away. */
            volatile char *blk = malloc((size_t)16 << 20);
            if (!blk) abort();
            for (size_t off = 0; off < ((size_t)16 << 20); off += 4096)
                blk[off] = (char)0xa5;
            total += (uint64_t)16 << 20;
        }
        worker_reply(id, HL_GPU_HELPER_OK, NULL, 0);
        _exit(0);
    }
#endif

    if (!have_dev) {
        worker_reply(id, HL_GPU_HELPER_UNAVAILABLE, NULL, 0);
        _exit(0);
    }
    if (device < 0 || device >= ctx->device_count) device = 0;
    CompileArg a = { be, &ctx->devices[device], (const char *)r.buf + HGC_REQ_PRE,
                     r.len - HGC_REQ_PRE, HL_GPU_ERR_INTERNAL };
    int crc = worker_compile(&a, stack);
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
                        const char *adapter, size_t stack_bytes)
{
    (void)wgsl; (void)len; (void)device; (void)adapter; (void)stack_bytes;
    return HL_GPU_HELPER_UNAVAILABLE;
}
void hl_gpu_helper_test_reset_cache(int disk_off) { (void)disk_off; }
void hl_gpu_helper_stats(HlGpuHelperStats *out)
{
    if (out) memset(out, 0, sizeof *out);
}

#endif
