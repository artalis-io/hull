/*
 * thread_affinity.h — event-loop thread-affinity assertions
 *
 * Several security-sensitive operations are correct ONLY because they run
 * on the single event-loop thread and never concurrently with a worker:
 *   - WASM shared-segment mutation (hl_cap_wasm_data_load) — its
 *     inflight_async guard is a plain int because submit/complete/check
 *     all run here.
 *   - async dispatch submit (hl_worker_{db,wasm,gpu}_submit) — where the
 *     in-flight accounting and the persistent-instance busy check live.
 *   - GPU shader compile (hl_cap_gpu_compile) — workers only look up
 *     pre-compiled pipelines, never compile.
 *
 * These helpers document those invariants in code and, in debug/sanitizer
 * builds, turn a future off-thread call into a loud abort instead of a
 * silent data race. Compiled out entirely in release (zero cost): the
 * tripwire is a dev/CI aid, not a production fail-closed mechanism.
 *
 * Enabled when HL_THREAD_AFFINITY_CHECKS is defined (DEBUG / MSAN / TSAN
 * builds — see the Makefile). When the event-loop thread has not been
 * marked (boot, unit tests, tool mode with no loop), the assert is a
 * no-op: there is no captured invariant to violate.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_THREAD_AFFINITY_H
#define HL_THREAD_AFFINITY_H

#ifdef HL_THREAD_AFFINITY_CHECKS

/* Record the calling thread as the event-loop thread. Call once, on the
 * event-loop thread, before any worker is spawned. */
void hl_event_loop_mark_current(void);

/* Abort if called from a thread other than the marked event-loop thread.
 * No-op if no thread has been marked. @what names the site for the message. */
void hl_assert_on_event_loop(const char *what);

#else

static inline void hl_event_loop_mark_current(void) {}
static inline void hl_assert_on_event_loop(const char *what) { (void)what; }

#endif /* HL_THREAD_AFFINITY_CHECKS */

#endif /* HL_THREAD_AFFINITY_H */
