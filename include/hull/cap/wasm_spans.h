/*
 * cap/wasm_spans.h - internal per-invocation mapped-span attachment lifecycle.
 *
 * This is the C-level lifecycle that attaches a set of read-only mmap windows
 * (HlMappedBuffer) to a WASM instance for ONE invocation, then reverse-order
 * tears them down on every exit
 * path. It is DELIBERATELY internal: there is NO public `spans={}` API, NO
 * metadata host_call, NO SDK header, and NO Lua/JS binding here -- those are held
 * until this lifecycle is reviewed. It is distinct from `compute.segment`
 * (cap/wasm_data.c), which stays module-scoped/shared/persistent and unchanged.
 *
 * Ownership/reclamation: each span BORROWS its HlMappedBuffer (pinned via the
 * checkpoint-1 borrow/release refcount for the set's lifetime, so a close()
 * mid-flight defers munmap) and OWNS a read-only WAMR shared heap it creates over
 * the buffer's page-aligned region. Teardown detaches, unchains, DESTROYS each
 * heap (WAMR patch 0003, wasm_runtime_destroy_shared_heap -> the shared-heap list
 * count returns to baseline every invocation), and releases each borrow -- in
 * reverse order.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_CAP_WASM_SPANS_H
#define HL_CAP_WASM_SPANS_H

#ifdef HL_ENABLE_WASM

#include "hull/cap/fs.h" /* HlMappedBuffer */

#include <pthread.h>
#include <stdint.h>

/* Max spans attached to one invocation. */
#ifndef HL_WASM_MAX_SPANS
#define HL_WASM_MAX_SPANS 16
#endif

/* Aggregate cap on the LOGICAL span bytes attached to ONE invocation (1 GiB),
 * matching HL_FS_MMAP_MAX_WINDOW_BYTES / the design 3.6. This is the TOTAL across
 * all spans of the set, accumulated overflow-safe. */
#ifndef HL_WASM_MAX_SPAN_BYTES_TOTAL
#define HL_WASM_MAX_SPAN_BYTES_TOTAL ((uint64_t)1 << 30)
#endif

/* One attached span: a borrowed read-only buffer + the shared heap over it.
 * Design B (WAMR patch 0004): the shared heap is created over the whole
 * page-aligned mapping [map_base, map_base+map_len) as the RESERVED region, and
 * carries a valid sub-window (valid_offset = addr - map_base, valid_size = len)
 * so the guest can reach only the caller's logical window [addr, addr+len). */
typedef struct HlWasmSpan {
    HlMappedBuffer *buf;         /**< Borrowed input; pinned while the set lives. */
    void           *shared_heap; /**< wasm_shared_heap_t (read-only) over buf's
                                      page-aligned [map_base, map_base+map_len). */
    uint64_t        wasm_addr;   /**< Guest LOGICAL window base = the heap's
                                      reserved WASM base + valid_offset (slop), so
                                      it maps natively to buf->addr. Reported to the
                                      guest as the span's `base` (metadata query). */
    char            name[64];    /**< Span name, NUL-terminated (1..63 bytes,
                                      unique within the set). Identifies the span to
                                      the guest metadata query. */
} HlWasmSpan;

/* An invocation-scoped set of spans. Zero-initialised by hl_wasm_span_set_init;
 * all state lives here (no globals), so distinct invocations on distinct pooled
 * instances are independent. */
typedef struct HlWasmSpanSet {
    HlWasmSpan spans[HL_WASM_MAX_SPANS];
    int        count;          /**< Spans successfully added (heaps created + borrowed). */
    uint64_t   total_logical;  /**< Sum of buf->len (logical); <= HL_WASM_MAX_SPAN_BYTES_TOTAL. */
    uint64_t   total_reserved; /**< Sum of buf->map_len (page-aligned reserved);
                                    tracked separately, kept under the address ceiling. */
    void      *chain_head;      /**< wasm_shared_heap_t chain head, or NULL. */
    void      *inst;            /**< Attached instance (wasm_module_inst_t), or NULL. */
    int        is_memory64;    /**< Address-ceiling selection for the chain. */
    pthread_t  owner;          /**< Thread that owns the set (add/attach/teardown). */
    int        owner_set;      /**< 1 iff `owner` has been recorded. */
} HlWasmSpanSet;

/**
 * @brief Begin an empty invocation-scoped span set on the calling thread.
 *
 * Records the calling thread as the owner; all subsequent add/attach/teardown
 * MUST run on this thread (the instance is single-threaded per call). For async,
 * the owning thread is the worker that runs the job and also tears the set down.
 */
void hl_wasm_span_set_init(HlWasmSpanSet *set, int is_memory64);

/**
 * @brief Add one read-only span named `name`, backed by `buf` (a windowed
 *        HlMappedBuffer).
 *
 * Validates, then borrows the buffer and creates its read-only shared heap:
 *  - count < HL_WASM_MAX_SPANS (else "too_many_spans");
 *  - `name` non-NULL, 1..63 bytes (strlen), and unique within the set (else
 *    "bad_name" / "duplicate_name"). `name` is a NUL-terminated C string; the
 *    binding layer rejects an embedded NUL before calling here;
 *  - overflow-safe aggregate: total_logical + buf->len <= 1 GiB (else "span_cap");
 *  - no duplicate backing: `buf` / buf->map_base not already in the set, and no
 *    native byte-range overlap with an existing span (else "duplicate_span");
 *  - buf->map_len page-aligned and <= UINT32_MAX (a windowed buffer; else the
 *    heap create fails -> "heap_create").
 *
 * On success the buffer is pinned (borrow), its RO heap is recorded, and `name`
 * is copied into the span. On failure NOTHING for this span is left behind (no
 * borrow, no heap); prior spans remain -- the caller rolls the whole set back
 * with hl_wasm_span_set_teardown. Owning-thread only.
 *
 * @return 0 on success; -1 with *err set on failure.
 */
int hl_wasm_span_set_add(HlWasmSpanSet *set, HlMappedBuffer *buf,
                         const char *name, const char **err);

/**
 * @brief Transactionally chain the added spans and attach them to `inst`.
 *
 * Chains the per-span heaps into one WAMR shared-heap chain at distinct,
 * non-overlapping high WASM addresses (asserted strictly disjoint), then attaches
 * the chain to `inst`. Must be called once, after all adds, before the WASM call.
 * If chaining or attach fails, the set is left UN-attached and the caller tears it
 * down (which unchains + destroys + releases). Rejects a set already attached
 * (reentrancy) and a foreign owning thread. Owning-thread only.
 *
 * @return 0 on success; -1 with *err set on failure.
 */
int hl_wasm_span_set_attach(HlWasmSpanSet *set, void *inst, const char **err);

/**
 * @brief Reverse-order teardown on EVERY exit path (and partial-failure rollback).
 *
 * Runs on the owning thread after the WASM call has returned (success, WASM trap,
 * gas exhaustion, host error, cancellation) or to unwind a partial build: detach
 * (if attached) -> unchain -> destroy each heap (patch 0003) -> release each
 * borrow, in REVERSE order of addition. Idempotent and safe on a partially-built
 * set.
 *
 * @return 0 when the set is FULLY torn down: the shared-heap list count and every
 *         borrow return to their pre-build baseline and the set is zeroed (a stale
 *         handle then rejects access). -1 when one or more heaps could NOT be
 *         destroyed (unexpected after detach + unchain): those spans are RETAINED
 *         (heap handle + mmap borrow kept, so nothing WAMR still references is
 *         freed) and COMPACTED to the front of the set, leaving it live and
 *         reachable. The caller may retry (call teardown again once the block
 *         clears -- an already-destroyed span is never re-destroyed and its
 *         backing never re-released) or hand the still-populated set to an
 *         explicit runtime-teardown cleanup path. Never silently discards a
 *         retained pin.
 */
int hl_wasm_span_set_teardown(HlWasmSpanSet *set);

#endif /* HL_ENABLE_WASM */
#endif /* HL_CAP_WASM_SPANS_H */
