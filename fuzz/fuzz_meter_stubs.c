/*
 * fuzz_meter_stubs.c: the thread-locals behind hull/shared/dispatch_meter.h's
 * inline exclude brackets (hl_meter_exclude_begin / _end, hl_meter_fsync).
 * cap/http.c brackets its synchronous waits and cap/fs.c its fsyncs with
 * them; the real definitions live in cap/run_watchdog.c, which pulls in the
 * watchdog thread and the runtime clocks, so the harnesses that link http.c
 * or fs.c (fuzz_http_guard, fuzz_span_window) link these instead.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdint.h>

_Thread_local uint64_t hl_meter_excluded;
_Thread_local uint32_t hl_meter_exclude_depth;
