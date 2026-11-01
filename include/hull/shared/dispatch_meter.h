/*
 * shared/dispatch_meter.h - measured-time charging for runtime bindings
 * (audit convergence phase 2, docs/design/audit_convergence_dispatcher.md)
 *
 * Every app-facing binding is called through one dispatcher per runtime
 * (Lua: budget.c's trampolines, registered by hl_lua_setfuncs /
 * hl_lua_pushcclosure; JS: the embedder call hooks of QuickJS HULL PATCH
 * 0011). The dispatcher reads a monotonic clock before and after the call
 * and charges the run's budget for the time the call took, at
 * HL_WORK_UNITS_PER_US units per microsecond, minus whatever the run was
 * charged while the call ran (the binding's own explicit charges, SQL
 * progress, the instructions of any script it called back into, a nested
 * dispatcher). A binding with a missing or too-small floor therefore costs
 * at least the time it took (see hl_meter_settle for how time off the CPU -
 * preemption on a loaded host - is bounded).
 *
 * Time that is not the run's own work is EXCLUDED: a binding that waits on
 * a peer or a user (a poll on a database socket, a synchronous HTTP
 * request, the terminal, stdin) or runs work metered by its own bound (a
 * WASM guest under gas / timeout_ms, a GPU device wait, an fsync) brackets that wait
 * with hl_meter_exclude_begin / _end, which adds the interval to a
 * thread-local total the dispatcher subtracts. Waits that PARK (yield the
 * coroutine, return a promise) are not inside the call at all.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_DISPATCH_METER_H
#define HL_SHARED_DISPATCH_METER_H

#include <errno.h>
#include <stdint.h>
#include <unistd.h>   /* fsync */

#include <time.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include "hull/shared/work_charge.h"   /* HL_WORK_UNITS_PER_US */

/* A raw monotonic reading: mach_absolute_time on Apple (a few ns, no
 * syscall), CLOCK_MONOTONIC in nanoseconds elsewhere (vDSO on Linux). */
static inline uint64_t hl_meter_ticks(void)
{
#if defined(__APPLE__)
    return mach_absolute_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

/* Instrumented builds run every binding several times slower than the
 * release build the rate was measured on, while the instructions a script
 * runs are counted, not timed; their measured time is divided by this
 * factor so a run's measured cost stays about what it would be in a
 * release build (tests that fit the budget there fit it here). A release
 * build is 1: production charging is unchanged. Slowdowns measured on the
 * PBKDF2-heavy TOTP tests (lua_stdlib.totp_enroll_confirm_verify) against
 * the release build on the same host:
 *
 *   MSan + UBSan -O1   x86_64 GitHub runner 19x (13.8 s vs 0.71 s),
 *                      aarch64 2.4x (mbedTLS is built without asm and
 *                      MSan shadows every SHA-256 round)   -> 64
 *   ASan + UBSan -O0   x86_64 2.3x                          -> 8
 *   TSan -O1                                                -> 16
 *   coverage (-O0 --coverage, atomic counters; the Makefile
 *   passes -DHL_METER_SLOWDOWN=8, the compiler names no feature) -> 8
 *
 * Each factor leaves at least 3x of margin over the worst slowdown seen
 * (PR #981: MSan at 8 tripped the default budget on ordinary work). A
 * -DHL_METER_SLOWDOWN=N on the command line wins. */
#if !defined(HL_METER_SLOWDOWN) && defined(__has_feature)
#  if __has_feature(memory_sanitizer)
#    define HL_METER_SLOWDOWN 64u
#  elif __has_feature(thread_sanitizer)
#    define HL_METER_SLOWDOWN 16u
#  elif __has_feature(address_sanitizer)
#    define HL_METER_SLOWDOWN 8u
#  endif
#endif
#if !defined(HL_METER_SLOWDOWN) && defined(__SANITIZE_THREAD__)
#  define HL_METER_SLOWDOWN 16u
#endif
#if !defined(HL_METER_SLOWDOWN) && defined(__SANITIZE_ADDRESS__)
#  define HL_METER_SLOWDOWN 8u
#endif
#ifndef HL_METER_SLOWDOWN
#  define HL_METER_SLOWDOWN 1u
#endif

/* Units of budget for @p ticks of wall time (cap/run_watchdog.c, which
 * caches the Apple timebase), already divided by HL_METER_SLOWDOWN. */
uint64_t hl_meter_ticks_units(uint64_t ticks);

/* Excluded time on this thread, in ticks, and the nesting depth of the
 * exclusion brackets (only the outermost one counts). Defined in
 * cap/run_watchdog.c. */
extern _Thread_local uint64_t hl_meter_excluded;
extern _Thread_local uint32_t hl_meter_exclude_depth;

/* Bracket a wait that is not the run's work. Returns a token for _end. */
static inline uint64_t hl_meter_exclude_begin(void)
{
    return hl_meter_exclude_depth++ == 0 ? hl_meter_ticks() : 0;
}

static inline void hl_meter_exclude_end(uint64_t token)
{
    if (hl_meter_exclude_depth == 0) return;
    if (--hl_meter_exclude_depth == 0 && token != 0)
        hl_meter_excluded += hl_meter_ticks() - token;
}

/* fsync(2) with the device flush excluded: waiting for the disk is not
 * the run's work, and on a slow or busy disk (a CI runner) it is
 * milliseconds that vary run to run. A binding that fsyncs pays for it
 * through its floor (HL_WORK_FS_WRITE_UNITS and the blob floors, sized
 * from the measured cost of the whole write, flush included). errno is
 * fsync's. */
static inline int hl_meter_fsync(int fd)
{
    uint64_t mx = hl_meter_exclude_begin();
    int rc = fsync(fd);
    int e = errno;
    hl_meter_exclude_end(mx);
    errno = e;
    return rc;
}

/* A call that owes more than this past its own charges is SLOW and puts the
 * VM in CPU mode (hl_meter_settle): 256 us, 16384 units. */
#define HL_METER_SLOW_UNITS (256u * HL_WORK_UNITS_PER_US)

/* Off-CPU time a call is charged at most in CPU mode: 32 us, 2048 units -
 * small, since on a loaded host many calls of a run are preempted. */
#define HL_METER_OFFCPU_MAX_UNITS (32u * HL_WORK_UNITS_PER_US)

/* This thread's CPU time in nanoseconds, or 0 when the host has no
 * per-thread CPU clock. A syscall (~100-200 ns), so it is read only once a
 * run is in CPU mode (below). */
static inline uint64_t hl_meter_cpu_ns(void)
{
#if defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    return ns ? ns : 1;
#else
    return 0;
#endif
}

/* Units of budget for @p ns nanoseconds of CPU time, already divided by
 * HL_METER_SLOWDOWN (cap/run_watchdog.c). */
uint64_t hl_meter_ns_units(uint64_t ns);

/* One call in progress: clock, excluded time, and - in CPU mode - this
 * thread's CPU time when it started. */
typedef struct HlMeterMark {
    uint64_t t0;    /* hl_meter_ticks() */
    uint64_t x0;    /* hl_meter_excluded */
    uint64_t c0;    /* hl_meter_cpu_ns(), or 0 */
    int      cpu;   /* the run was in CPU mode when the call started */
} HlMeterMark;

/* A VM's meter state: the mode (0 = WALL, else the tick of the last slow
 * call = CPU; hl_meter_settle) and the thread's CPU time when a run was last
 * armed with no binding call in progress (hl_meter_arm), or 0. */
typedef struct HlMeterState {
    uint64_t mode;
    uint64_t cpu_ref;
} HlMeterState;

/* A run is armed: the CPU reference for a slow call in WALL mode. Call it
 * only when no binding call of this VM is in progress. */
static inline void hl_meter_arm(HlMeterState *st)
{
    st->cpu_ref = hl_meter_cpu_ns();
}

/* Start measuring a call; @p cpu_mode is the run's mode. The wall clock is
 * read last, so the readings before it are not the call's time. */
static inline void hl_meter_begin(HlMeterMark *m, int cpu_mode)
{
    m->x0  = hl_meter_excluded;
    m->cpu = cpu_mode ? 1 : 0;
    m->c0  = cpu_mode ? hl_meter_cpu_ns() : 0;
    m->t0  = hl_meter_ticks();
}

/* What an explicit charge is worth against measured time (below): 2x. */
#define HL_METER_PAID_WEIGHT 2u

/* How long a run stays in CPU mode after its last slow call: 100 ms of
 * wall time, in units (below). */
#define HL_METER_CPU_WINDOW_UNITS (100000u * HL_WORK_UNITS_PER_US)

/* The units a dispatched call owes, given the @p charged units the run was
 * charged while it ran (the binding's explicit charges, nested script, SQL
 * progress, ...). The rule (docs/design/audit_convergence_dispatcher.md):
 *
 *     owed = max(0, measured - HL_METER_PAID_WEIGHT x charged)
 *
 * so an explicit charge or floor is a down payment against measured time,
 * never added to it, and counts double: floors were sized from the cost
 * measured on an idle machine, and the same work takes up to about twice as
 * long on a slower or loaded one (another process on the core's siblings, a
 * low-power core). The dispatcher exists to catch a floor that is missing
 * or far too small - the audits found 0 units for work worth thousands -
 * so a call is charged at least half its measured time, and a calibrated
 * floor is not second-guessed by machine-to-machine variance. The measured time is the call's wall time less the
 * waits it bracketed as excluded (hl_meter_exclude_begin / _end: peer and
 * user waits, device flushes, WASM / GPU work under its own bound).
 *
 * Wall time also counts time the thread did not run: preempted by another
 * process on a loaded host, or blocked in a wait nobody bracketed. The
 * VM's meter mode (@p st, HlMeterState) decides how that is treated:
 *
 * - In WALL mode (two cheap clock reads per call) a call is charged its
 *   measured wall time. A call that owes more than HL_METER_SLOW_UNITS is
 *   "slow": it switches the VM to CPU mode, and is charged at most the
 *   thread's CPU time since the run was armed (which bounds its own CPU
 *   work from above) plus HL_METER_OFFCPU_MAX_UNITS.
 * - In CPU mode every call also reads the thread's CPU clock and is
 *   measured as  min(wall - excluded, cpu + HL_METER_OFFCPU_MAX_UNITS):
 *   all the CPU work it did, plus at most 32 us of time off the CPU. Each
 *   slow call keeps the VM in CPU mode; HL_METER_CPU_WINDOW_UNITS of wall
 *   time after the last one it goes back to WALL mode (the CPU clock is a
 *   syscall, so only a VM that is being preempted pays for it).
 *
 * So a call is never charged less than half the CPU work it did (all of it
 * when it charged nothing) nor more than its wall time; a preemption is
 * charged only as far as the run's CPU time so far covers it. A host
 * without a per-thread CPU clock measures wall time. hl_meter_begin takes
 * (st->mode != 0). */
static inline uint64_t hl_meter_settle(const HlMeterMark *m, int64_t charged,
                                       HlMeterState *st)
{
    uint64_t t1 = hl_meter_ticks();
    uint64_t c1 = m->c0 ? hl_meter_cpu_ns() : 0;
    uint64_t x1 = hl_meter_excluded;
    uint64_t wall = t1 > m->t0 ? t1 - m->t0 : 0;
    uint64_t excl = x1 > m->x0 ? x1 - m->x0 : 0;
    uint64_t units = excl < wall ? hl_meter_ticks_units(wall - excl) : 0;
    uint64_t paid = charged > 0 ? (uint64_t)charged : 0;
    paid = paid > UINT64_MAX / HL_METER_PAID_WEIGHT
               ? UINT64_MAX : paid * HL_METER_PAID_WEIGHT;
    int slow = units > paid + HL_METER_SLOW_UNITS;
    uint64_t cpu = 0;
    if (m->cpu && m->c0 && c1 >= m->c0) {
        cpu = c1 - m->c0;
    } else if (!m->cpu && slow && st && st->cpu_ref) {
        /* No CPU reading for this call: the thread's CPU time since the
         * run was armed (before the call began) bounds it from above. */
        uint64_t cn = hl_meter_cpu_ns();
        /* cppcheck reads the stub hl_meter_cpu_ns (no CPU clock: 0). */
        // cppcheck-suppress unsignedLessThanZero
        cpu = cn > st->cpu_ref ? cn - st->cpu_ref : 0;
        // cppcheck-suppress knownConditionTrueFalse
        if (cpu == 0) cpu = UINT64_MAX;    /* no usable reading */
    }
    if (cpu && cpu != UINT64_MAX) {
        uint64_t u = hl_meter_ns_units(cpu) + HL_METER_OFFCPU_MAX_UNITS;
        if (u < units) units = u;
    }
    if (st) {
        if (slow)
            st->mode = t1 ? t1 : 1;
        else if (st->mode && t1 > st->mode &&
                 hl_meter_ticks_units(t1 - st->mode) > HL_METER_CPU_WINDOW_UNITS)
            st->mode = 0;
    }
    return units > paid ? units - paid : 0;
}

#endif /* HL_SHARED_DISPATCH_METER_H */
