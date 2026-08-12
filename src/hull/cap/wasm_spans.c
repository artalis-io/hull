/*
 * cap/wasm_spans.c - internal per-invocation mapped-span attachment lifecycle.
 *
 * See include/hull/cap/wasm_spans.h. Internal only: no public spans API, no
 * metadata host_call, no SDK header, no bindings (held for review). Distinct from
 * cap/wasm_data.c (compute.segment), which is unchanged.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm_spans.h"
#include "hull/cap/fs.h"
#include "log.h"
#include "wasm_export.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* Overflow-safe native byte-range overlap: does [a, a+alen) meet [b, b+blen)?
 * alen/blen are > 0 and bounded (<= 1 GiB), and real mmap addresses sit far below
 * the top of the address space, so a+alen does not wrap. */
static int
ranges_overlap(const void *a, size_t alen, const void *b, size_t blen)
{
    uintptr_t a0 = (uintptr_t)a, a1 = a0 + alen;
    uintptr_t b0 = (uintptr_t)b, b1 = b0 + blen;
    return a0 < b1 && b0 < a1;
}

static int
owner_ok(const HlWasmSpanSet *set)
{
    return set->owner_set && pthread_equal(set->owner, pthread_self());
}

void
hl_wasm_span_set_init(HlWasmSpanSet *set, int is_memory64)
{
    if (!set)
        return;
    memset(set, 0, sizeof(*set));
    set->is_memory64 = is_memory64 ? 1 : 0;
    set->owner = pthread_self();
    set->owner_set = 1;
}

int
hl_wasm_span_set_add(HlWasmSpanSet *set, HlMappedBuffer *buf, const char **err)
{
    if (!set || !buf) {
        if (err) *err = "internal_error";
        return -1;
    }
    if (!owner_ok(set)) {
        if (err) *err = "wrong_thread";
        return -1;
    }
    if (set->inst) { /* the set is attached; no mutation until teardown */
        if (err) *err = "already_attached";
        return -1;
    }
    if (set->count >= HL_WASM_MAX_SPANS) {
        if (err) *err = "too_many_spans";
        return -1;
    }

    /* the buffer must be a live, page-aligned window usable as a shared heap.
     * (A whole-file mmap whose map_len is not page-aligned is not a valid span;
     * spans use the windowed constructor.) */
    if (buf->closed || !buf->map_base || buf->map_len == 0) {
        if (err) *err = "bad_buffer";
        return -1;
    }
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    if ((uint64_t)buf->map_len % (uint64_t)pg != 0
        || (uint64_t)buf->map_len > (uint64_t)UINT32_MAX) {
        if (err) *err = "bad_buffer";
        return -1;
    }

    /* Design B validity: the logical window [addr, addr+len) must sit inside the
     * page-aligned mapping. valid_offset = addr - map_base (the intra-page slop);
     * valid_offset + len <= map_len always holds for a windowed buffer, but assert
     * it (fail closed) so a malformed HlMappedBuffer can never widen the guest. */
    uint64_t voff = (uint64_t)((uintptr_t)buf->addr - (uintptr_t)buf->map_base);
    if ((uintptr_t)buf->addr < (uintptr_t)buf->map_base
        || voff + (uint64_t)buf->len > (uint64_t)buf->map_len) {
        if (err) *err = "bad_buffer";
        return -1;
    }

    /* overflow-safe aggregate cap on LOGICAL bytes (the caller window, buf->len):
     * total + len <= CAP, written to never wrap. */
    if ((uint64_t)buf->len > HL_WASM_MAX_SPAN_BYTES_TOTAL
        || set->total_logical > HL_WASM_MAX_SPAN_BYTES_TOTAL - (uint64_t)buf->len) {
        if (err) *err = "span_cap";
        return -1;
    }

    /* Account RESERVED (page-aligned map_len) bytes separately, and keep the sum
     * under the WASM address-space ceiling: the shared-heap chain occupies the top
     * of the address space, so sum(reserved) must fit below UINT32_MAX (wasm32) /
     * UINT64_MAX (memory64). Overflow-safe: total + map_len <= ceil. */
    uint64_t addr_ceil = set->is_memory64 ? UINT64_MAX : (uint64_t)UINT32_MAX;
    if ((uint64_t)buf->map_len > addr_ceil
        || set->total_reserved > addr_ceil - (uint64_t)buf->map_len) {
        if (err) *err = "addr_space";
        return -1;
    }

    /* reject duplicate backing (same buffer / same mapping) and any native
     * byte-range overlap with an already-added span. */
    for (int i = 0; i < set->count; i++) {
        HlMappedBuffer *o = set->spans[i].buf;
        if (o == buf || o->map_base == buf->map_base
            || ranges_overlap(buf->map_base, buf->map_len, o->map_base,
                              o->map_len)) {
            if (err) *err = "duplicate_span";
            return -1;
        }
    }

    /* pin the buffer for the set's lifetime, then create its read-only heap over
     * the whole page-aligned mapping (RESERVED) with the valid sub-window carved
     * to [valid_offset, valid_offset+len) (Design B, WAMR patch 0004). */
    hl_cap_fs_mmap_borrow(buf);

    SharedHeapInitArgs args;
    memset(&args, 0, sizeof(args));
    args.pre_allocated_addr = buf->map_base;
    args.size = (uint32_t)buf->map_len;
    args.read_only = true;
    args.valid_offset = voff;
    args.valid_size = (uint64_t)buf->len;
    wasm_shared_heap_t heap = wasm_runtime_create_shared_heap(&args);
    if (!heap) {
        hl_cap_fs_mmap_release(buf); /* undo the borrow: nothing left behind */
        if (err) *err = "heap_create";
        return -1;
    }

    set->spans[set->count].buf = buf;
    set->spans[set->count].shared_heap = heap;
    set->spans[set->count].wasm_addr = 0; /* computed on attach */
    set->count++;
    set->total_logical += (uint64_t)buf->len;
    set->total_reserved += (uint64_t)buf->map_len;
    return 0;
}

int
hl_wasm_span_set_attach(HlWasmSpanSet *set, void *inst, const char **err)
{
    if (!set || !inst) {
        if (err) *err = "internal_error";
        return -1;
    }
    if (!owner_ok(set)) {
        if (err) *err = "wrong_thread";
        return -1;
    }
    if (set->inst) {
        if (err) *err = "already_attached";
        return -1;
    }
    if (set->count == 0) {
        if (err) *err = "no_spans";
        return -1;
    }

    uint64_t addr_ceil =
        set->is_memory64 ? UINT64_MAX : (uint64_t)UINT32_MAX;

    /* Chain right-to-left (last span highest). On a partial-chain failure, the
     * spans already chained live in `chain`; record it so teardown unchains them. */
    wasm_shared_heap_t chain =
        (wasm_shared_heap_t)set->spans[set->count - 1].shared_heap;
    for (int i = set->count - 2; i >= 0; i--) {
        wasm_shared_heap_t next = wasm_runtime_chain_shared_heaps(
            (wasm_shared_heap_t)set->spans[i].shared_heap, chain);
        if (!next) {
            set->chain_head = chain; /* partial chain (spans i+1..end) */
            if (err) *err = "chain_failed";
            return -1;
        }
        chain = next;
    }
    set->chain_head = chain;

    /* Compute the RESERVED WASM slots from the ceiling downward (map_len each,
     * mirroring how WAMR's create + chain place them), and record each span's
     * guest LOGICAL base = reserved_base + valid_offset (slop). That logical base
     * maps natively to buf->addr; the guest can reach only [wasm_addr, +len). */
    uint64_t next_start = addr_ceil;
    uint64_t reserved_base[HL_WASM_MAX_SPANS];
    for (int i = set->count - 1; i >= 0; i--) {
        uint64_t alloc = (uint64_t)set->spans[i].buf->map_len;
        reserved_base[i] = next_start - alloc + 1;
        uint64_t voff = (uint64_t)((uintptr_t)set->spans[i].buf->addr
                                   - (uintptr_t)set->spans[i].buf->map_base);
        set->spans[i].wasm_addr = reserved_base[i] + voff;
        if (i > 0)
            next_start = reserved_base[i] - 1;
    }

    /* Fail-closed: assert the RESERVED WASM slots are STRICTLY DISJOINT (the chain
     * places them non-overlapping; this catches any math regression). The logical
     * windows are sub-ranges of disjoint reserved slots, so they are disjoint too.
     * Inclusive ends avoid the wrap at addr_ceil + 1. */
    for (int i = 0; i < set->count; i++) {
        uint64_t ai0 = reserved_base[i];
        uint64_t ai1 = ai0 + (uint64_t)set->spans[i].buf->map_len - 1;
        for (int j = i + 1; j < set->count; j++) {
            uint64_t aj0 = reserved_base[j];
            uint64_t aj1 = aj0 + (uint64_t)set->spans[j].buf->map_len - 1;
            if (ai0 <= aj1 && aj0 <= ai1) {
                if (err) *err = "wasm_overlap";
                return -1;
            }
        }
    }

    if (!wasm_runtime_attach_shared_heap((wasm_module_inst_t)inst,
                                         (wasm_shared_heap_t)set->chain_head)) {
        /* leave un-attached; the caller tears down (unchain + destroy + release). */
        if (err) *err = "attach_failed";
        return -1;
    }
    set->inst = inst;
    return 0;
}

int
hl_wasm_span_set_teardown(HlWasmSpanSet *set)
{
    if (!set)
        return 0;

    /* Reverse order on every exit path (success / trap / error / cancel) and
     * partial-failure rollback: detach -> unchain -> destroy -> release.
     * Returns 0 when the set is fully torn down (list + borrows back to
     * baseline, set zeroed), or -1 when one or more heaps could NOT be destroyed:
     * those spans are RETAINED, live, and COMPACTED to the front of the set so
     * the caller can retry (call teardown again once the block clears) or hand
     * the still-populated set to a runtime-teardown cleanup path. Idempotent:
     * detach/unchain no-op on a retry (inst/chain already cleared). */

    /* 1. detach the chain from the instance (clears the interp/AOT caches and
     *    drops attached_count to 0). */
    if (set->inst) {
        wasm_runtime_detach_shared_heap((wasm_module_inst_t)set->inst);
        set->inst = NULL;
    }

    /* 2. unchain (requires attached_count == 0, just satisfied). Only when a
     *    chain was actually formed (count > 1); a lone heap is already standalone. */
    if (set->chain_head && set->count > 1) {
        wasm_runtime_unchain_shared_heaps((wasm_shared_heap_t)set->chain_head,
                                          true);
    }
    set->chain_head = NULL;

    /* 3+4. destroy each heap (patch 0003) then release each borrow, in REVERSE
     *      order of addition. Destroy frees only WAMR's descriptor; release drops
     *      the borrow pin so the mmap can finally go.
     *
     *      On destroy FAILURE (unexpected after detach + unchain): WAMR may still
     *      reference this heap's backing memory, so RETAIN BOTH the heap handle
     *      and the mmap borrow -- releasing the borrow could munmap memory WAMR
     *      still points at (use-after-free). The retained span stays in the set
     *      (see compaction below) so it is reachable for a retry or an explicit
     *      cleanup path, NOT silently discarded into a permanent pin. Only a
     *      SUCCESSFUL destroy releases the borrow and NULLs the span; an
     *      already-destroyed span (buf/heap NULL) is skipped, so a retry never
     *      re-destroys a heap or re-releases backing. */
    for (int i = set->count - 1; i >= 0; i--) {
        if (!set->spans[i].shared_heap)
            continue; /* already destroyed on a prior pass; do not touch */
        if (wasm_runtime_destroy_shared_heap(
                (wasm_shared_heap_t)set->spans[i].shared_heap)) {
            set->spans[i].shared_heap = NULL;
            if (set->spans[i].buf) {
                hl_cap_fs_mmap_release(set->spans[i].buf);
                set->spans[i].buf = NULL;
            }
        }
        else {
            log_error("[wasm] span shared-heap destroy failed; "
                      "retaining heap handle + mmap borrow (retry-able)");
        }
    }

    /* Compact any RETAINED spans (destroy failed -> heap still set) to the front,
     * recompute count + logical/reserved totals from what survives, and clear the
     * rest. If nothing survives, zero the whole set so a stale handle is rejected.
     * The retained set stays valid for a subsequent teardown retry. */
    int w = 0;
    uint64_t logical = 0, reserved = 0;
    for (int i = 0; i < set->count; i++) {
        if (!set->spans[i].shared_heap)
            continue;
        if (w != i)
            set->spans[w] = set->spans[i];
        if (set->spans[w].buf) {
            logical += (uint64_t)set->spans[w].buf->len;
            reserved += (uint64_t)set->spans[w].buf->map_len;
        }
        w++;
    }
    for (int i = w; i < set->count; i++)
        memset(&set->spans[i], 0, sizeof(set->spans[i]));

    if (w == 0) {
        int is_mem64 = set->is_memory64;
        memset(set, 0, sizeof(*set));
        set->is_memory64 = is_mem64;
        return 0;
    }
    set->count = w;
    set->total_logical = logical;
    set->total_reserved = reserved;
    return -1;
}

#endif /* HL_ENABLE_WASM */
