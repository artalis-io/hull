/*
 * cap/wasm_watchdog.h - wall-clock bound for WASM compute (audit 5 M2)
 *
 * Gas (WAMR instruction metering) bounds INTERPRETED code only: WAMR's AOT
 * code is never metered, and the functions an instantiation runs itself (the
 * start function, __post_instantiate, __wasm_call_ctors) run with no limit at
 * all. The watchdog bounds every guest execution by time instead: a caller
 * arms a watch on the instance it is about to run, and one process-wide
 * thread calls wasm_runtime_terminate() on it at the deadline. WAMR patch 0007
 * makes that land: the fast interpreter polls the instance's exception every
 * 4096 instructions and AOT code checks it at every loop header, so a
 * terminated instance traps out of any loop - the same unwind as gas or an
 * out-of-bounds access.
 *
 * Instantiation is covered through the patch's post-instantiate hook: a watch
 * made PENDING on the calling thread (hl_wasm_watch_instantiate) is bound to
 * the new instance while its start / ctor functions run.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_WASM_WATCHDOG_H
#define HL_CAP_WASM_WATCHDOG_H

#ifdef HL_ENABLE_WASM

#include <stdint.h>

/* One armed deadline. Caller-owned (lives on the caller's stack for the span
 * of one instantiate + call); linked into the watchdog's list while armed. */
typedef struct HlWasmWatch {
    struct HlWasmWatch *next;
    struct HlWasmWatch *prev;
    void     *inst;        /* wasm_module_inst_t to terminate; NULL = none bound */
    uint64_t  deadline_ms; /* monotonic */
    int       armed;
    int       expired;     /* the deadline passed: the bound instance is
                            * terminated, and terminated again every
                            * HL_WASM_WATCHDOG_REASSERT_MS until unbound */
} HlWasmWatch;

/* Install the WAMR post-instantiate hook. Called once by hl_cap_wasm_init,
 * after wasm_runtime_full_init. The watchdog thread itself starts on the first
 * arm. */
void hl_wasm_watchdog_install(void);

/* Arm @p w with a deadline @p timeout_ms from now (0 = the default), bound to
 * @p inst (may be NULL: bound later). 0, or -1 when the watchdog thread could
 * not be started - the caller must then refuse to run guest code. */
int hl_wasm_watch_arm(HlWasmWatch *w, uint32_t timeout_ms, void *inst);

/* Bind @p w to @p inst (NULL unbinds). An already expired watch terminates
 * the instance at once, and keeps re-terminating it until it is unbound. */
void hl_wasm_watch_bind(HlWasmWatch *w, void *inst);

/* Disarm @p w. Once this returns, the watchdog never touches the instance
 * again. Returns 1 when the deadline passed while it was armed (a failed call
 * then failed BECAUSE of the timeout), else 0. */
int hl_wasm_watch_disarm(HlWasmWatch *w);

/* wasm_runtime_instantiate under the armed watch @p w: the instance's own
 * start / ctor functions are bounded by the same deadline. */
void *hl_wasm_watch_instantiate(HlWasmWatch *w, void *module,
                                uint32_t stack_size, uint32_t heap_size,
                                char *error_buf, uint32_t error_buf_size);

/* Effective timeout: @p ms when non-zero, else the default, clamped to the
 * compile-time maximum. */
uint32_t hl_wasm_timeout_resolve(uint64_t ms);

#endif /* HL_ENABLE_WASM */
#endif /* HL_CAP_WASM_WATCHDOG_H */
