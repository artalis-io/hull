/**
 * @file cap/gpu_helper.h
 * @brief The shader compile helper process (audit convergence §7.3).
 *
 * A built binary whose manifest sets gpu.dynamic_shaders compiles runtime
 * WGSL source FIRST in a separate, bounded helper process. Only a source
 * the helper compiled cleanly, within its deadline, is then compiled in the
 * server (under the existing compile thread + deadline): naga is
 * deterministic, so a source that holds the helper past its deadline,
 * crashes it, recurses past its (smaller) stack or exhausts its memory is
 * refused before the server's compiler ever sees it. Design and
 * measurements: docs/design/gpu_compile_helper.md.
 *
 * Process model (all from Hull's own binary, posix_spawn, no shell):
 *
 *   server --(socketpair, spawned before the phase-1 sandbox)--> supervisor
 *   supervisor --(socketpair, one per compile)--> worker
 *
 * The supervisor parses only fixed-size frames, never WGSL; it spawns one
 * fresh worker per compile, enforces the deadlines with SIGKILL, and
 * validates the worker's reply (size caps). The worker initialises the GPU
 * backend, applies its own sandbox and rlimits, then reads the source,
 * compiles it on a thread of HL_GPU_HELPER_STACK_BYTES and replies with a
 * status. The server, sandboxed (it may no longer spawn), keeps the
 * supervisor for its whole life; if the supervisor dies, dynamic compiles
 * fail closed ("shader_helper_failed").
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_GPU_HELPER_H
#define HL_CAP_GPU_HELPER_H

#include <stddef.h>
#include <stdint.h>

/* argv[1] that runs a binary as the helper (hull_serve intercepts it before
 * parsing any argument): argv[2] is "supervisor" or "worker". Not a flag
 * spelling, so it never collides with an app's own arguments' parsing. */
#define HL_GPU_HELPER_ARGV1 "__hull_gpu_compile_helper"

/* Verdicts (the wire status and hl_gpu_helper_check's result). */
typedef enum {
    HL_GPU_HELPER_OK          = 0,  /* compiled cleanly within the deadline */
    HL_GPU_HELPER_SHADER      = 1,  /* the compiler refused the source */
    HL_GPU_HELPER_TIMEOUT     = 2,  /* killed at its deadline */
    HL_GPU_HELPER_CRASHED     = 3,  /* died (signal, exit) without a reply */
    HL_GPU_HELPER_BADREPLY    = 4,  /* reply malformed or past its size cap */
    HL_GPU_HELPER_UNAVAILABLE = 5,  /* no helper, or no GPU device in it */
    HL_GPU_HELPER_CANCELLED   = 6,  /* the calling run stopped first */
} HlGpuHelperStatus;

/* Run this process as the helper (argv[1] == HL_GPU_HELPER_ARGV1, @p role =
 * argv[2]). The caller exits with the return value; it never goes on to
 * its normal startup. */
int hl_gpu_helper_main(const char *role);

/* Spawn the supervisor. @p exe = the binary to run (NULL = this process's
 * own: /proc/self/exe on Linux, the executable path on macOS). Called by
 * serve.c before the phase-1 sandbox (which forbids spawning). 0, or -1
 * (dynamic compiles then fail closed). Idempotent. */
int hl_gpu_helper_start(const char *exe);

/* 1 when a supervisor is running. */
int hl_gpu_helper_running(void);

/* Close the supervisor's socket (it exits on EOF, killing a running worker)
 * and reap it if it has exited. Never signals it: the sandboxed server may
 * not kill another process. Idempotent. */
void hl_gpu_helper_stop(void);

/* Check @p wgsl in the helper, for device @p device (whose adapter name is
 * @p adapter, part of the verdict's cache key). Looks the verdict up in the
 * in-memory cache, then the sealed on-disk cache ("gpu-shaders", OK
 * verdicts only); on a miss charges HL_GPU_HELPER_UNITS to the calling
 * thread's run budget, then asks the supervisor and waits - polling the
 * run's deadline / budget every HL_GPU_COMPILE_POLL_MS, sending a cancel
 * when it stops - at most the helper's deadlines plus a margin. Returns a
 * HlGpuHelperStatus. Event-loop only. */
int hl_gpu_helper_check(const char *wgsl, size_t len, int device,
                        const char *adapter);

/* TEST-ONLY: drop every cached verdict (memory) and skip the on-disk cache
 * while @p off (so a test's compile reaches the helper). */
void hl_gpu_helper_test_reset_cache(int disk_off);

/* TEST-ONLY: the deadlines in ms (0 = the defaults): the worker's start-up
 * (spawn + backend init + sandbox) and its compile. */
void hl_gpu_helper_test_set_timeouts(uint32_t init_ms, uint32_t compile_ms);

/* The sealed on-disk verdict cache (cache kind "gpu-shaders",
 * HULL_NO_GPU_SHADER_CACHE): @p hex is the verdict key (64 hex chars). get:
 * 0 when an OK verdict is stored and its seal verifies, else -1. put:
 * stores an OK verdict, best effort. Defined in runtime/cache_common.c
 * (weak no-op defaults in cap/gpu_helper.c). Off under --verify-sig, like
 * the other runtime-key code caches. */
int  hl_runtime_cache_gpu_verdict_get(const char *hex);
void hl_runtime_cache_gpu_verdict_put(const char *hex);

/* Counters (tests, diagnostics): round trips to the supervisor, cache hits
 * (memory or disk), and verdicts that came from a worker whose OS sandbox
 * could not be applied. */
typedef struct {
    uint64_t round_trips;
    uint64_t cache_hits;
    uint64_t unsandboxed;   /* verdicts from a worker without its sandbox */
} HlGpuHelperStats;
void hl_gpu_helper_stats(HlGpuHelperStats *out);

#endif /* HL_CAP_GPU_HELPER_H */
