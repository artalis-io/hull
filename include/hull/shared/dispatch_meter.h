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
 * at least its wall time: the worst case is one call's duration before the
 * trip lands.
 *
 * Time that is not the run's own work is EXCLUDED: a binding that waits on
 * a peer or a user (a poll on a database socket, a synchronous HTTP
 * request, the terminal, stdin) or runs work metered by its own bound (a
 * WASM guest under gas / timeout_ms, a GPU device wait) brackets that wait
 * with hl_meter_exclude_begin / _end, which adds the interval to a
 * thread-local total the dispatcher subtracts. Waits that PARK (yield the
 * coroutine, return a promise) are not inside the call at all.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_DISPATCH_METER_H
#define HL_SHARED_DISPATCH_METER_H

#include <stdint.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
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

/* Sanitizer builds run every binding several times slower than the
 * release build the rate was measured on; their measured time is scaled
 * down by this factor so a run's measured cost stays what it would be in a
 * release build (tests that fit the budget there fit it here). */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || \
      __has_feature(thread_sanitizer)
#    define HL_METER_SLOWDOWN 8u
#  endif
#endif
#if !defined(HL_METER_SLOWDOWN) && (defined(__SANITIZE_ADDRESS__) || \
                                    defined(__SANITIZE_THREAD__))
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

/* The units a dispatched call owes: its wall time from @p t0 to @p t1, less
 * the excluded time (@p x0 / @p x1, readings of hl_meter_excluded), less the
 * @p charged units the run was charged meanwhile. 0 when nothing is owed. */
static inline uint64_t hl_meter_owed(uint64_t t0, uint64_t t1,
                                     uint64_t x0, uint64_t x1,
                                     int64_t charged)
{
    uint64_t wall = t1 > t0 ? t1 - t0 : 0;
    uint64_t excl = x1 > x0 ? x1 - x0 : 0;
    if (excl >= wall) return 0;
    uint64_t units = hl_meter_ticks_units(wall - excl);
    if (charged > 0) {
        if ((uint64_t)charged >= units) return 0;
        units -= (uint64_t)charged;
    }
    return units;
}

#endif /* HL_SHARED_DISPATCH_METER_H */
