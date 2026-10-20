/*
 * hull/shared/offheap.h - off-heap copies counted against the VM heap limit
 *
 * An async binding copies its input into plain malloc before it submits the
 * op (the VM may move or free the original while a worker runs). That copy
 * is outside the VM heap, and a MappedBuffer / WasmBuffer / HlImage input is
 * not on the heap at all, so nothing bounded how much a run could hold:
 * N in-flight compute.async.call / gpu.async.dispatch ops on one 1 GiB
 * mapping held N copies of it (audit 20). The copy takes a hold on the VM's
 * off-heap total - the counter OWNED WasmBuffers use (hl_wasm_buffer_account,
 * `wasm_buf_bytes`), so both share one ceiling, the heap limit - and the op
 * gives it back when it is freed.
 *
 * Both calls run on the VM's (event-loop) thread: every op free path (done,
 * cancel, ctx destroy) does, and teardown drains the pool before the VM goes.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_OFFHEAP_H
#define HL_SHARED_OFFHEAP_H

#include <stddef.h>

typedef struct HlOffheapHold {
    size_t *total;      /* the VM's counter, NULL = nothing held */
    size_t  bytes;
} HlOffheapHold;

/* Add @p n bytes to @p hold (on @p total). Refuses (-1, nothing counted)
 * when that would take the total past @p limit (0 = no limit). A NULL
 * @p total holds nothing and succeeds. */
static inline int hl_offheap_take(HlOffheapHold *hold, size_t *total,
                                  size_t limit, size_t n)
{
    if (!hold || !total || n == 0) return 0;
    if (hold->total && hold->total != total) return -1;
    if (limit && (n > limit || *total > limit - n)) return -1;
    *total += n;
    hold->total = total;
    hold->bytes += n;
    return 0;
}

/* Give @p n of the bytes @p hold took back (a copy that failed after its
 * take, among others the hold keeps). */
static inline void hl_offheap_give(HlOffheapHold *hold, size_t n)
{
    if (!hold || !hold->total) return;
    if (n > hold->bytes) n = hold->bytes;
    *hold->total = *hold->total >= n ? *hold->total - n : 0;
    hold->bytes -= n;
    if (hold->bytes == 0) hold->total = NULL;
}

/* Give every byte @p hold took back. Safe to call twice. */
static inline void hl_offheap_drop(HlOffheapHold *hold)
{
    if (!hold || !hold->total) return;
    *hold->total = *hold->total >= hold->bytes ? *hold->total - hold->bytes : 0;
    hold->total = NULL;
    hold->bytes = 0;
}

#endif /* HL_SHARED_OFFHEAP_H */
