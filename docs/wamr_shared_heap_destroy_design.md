# WAMR per-heap shared-heap destruction - design (DESIGN ONLY)

**Status:** DESIGN ONLY, for review. **Implementation is UNAUTHORIZED until this
review passes.** No code, WAMR patch, or test is written yet; everything below is
what the implementation PR *will* do. This note is the prerequisite for
mapped-spans checkpoint 2 (the per-invocation attachment lifecycle): that
lifecycle creates a shared heap per span per invocation, and WAMR today has no
way to reclaim one. We land heap destruction FIRST, then build the Hull lifecycle
on top of it. **Checkpoint 2 must not be implemented with a known per-invocation
leak.**

## 0. The problem (grounded in the vendored WAMR)

WAMR `2.4.1-218-gc3a78cd1` exposes shared heaps as **create-only**
(`core/iwasm/common/wasm_memory.c`, `.../include/wasm_export.h`):

- `wasm_runtime_create_shared_heap` `runtime_malloc`s a `WASMSharedHeap` (~72 B),
  aligns/validates size, and **prepends it to a process-global singly-linked list
  `shared_heap_list`** under `shared_heap_list_lock` (`wasm_memory.c:289-297`).
- `wasm_runtime_chain_shared_heaps` **walks that global list** to validate the
  head/body are not already chained (`wasm_memory.c:325-352`) - O(list length).
- `attach`/`detach` adjust `head->attached_count` under the same lock;
  `detach` also **clears the per-instance last-used cache** on BOTH the interp and
  AOT sides (`shared_heap = NULL`, `shared_heap_start_off = UINT{64,32}_MAX`,
  `shared_heap_end_off = MAX-1`, `shared_heap_base_addr_adj = NULL`;
  `wasm_memory.c:642-690`).
- There is **no `wasm_runtime_destroy_shared_heap`**. `unchain` only unlinks
  `chain_next` pointers; `reset_shared_heap_chain` recreates managed backing but
  keeps the descriptor. The ONLY reclamation is global teardown
  (`wasm_runtime_memory_destroy`, which frees every node on `shared_heap_list`).

**Consequence.** `compute.segment` creates a handful of module-scoped heaps once,
so the create-only model is harmless there. A **per-invocation** span lifecycle
that creates a heap per span per call would:
1. **leak** ~72 B of descriptor per span per call (reclaimed only at runtime
   teardown - never, for a long-running server), and
2. grow `shared_heap_list` unboundedly, making every subsequent
   `chain_shared_heaps` **O(n) in the number of leaked heaps** - a quadratic
   cliff over the process lifetime.

Tests that init + `wasm_runtime_destroy()` a runtime do NOT show the leak
(teardown frees the list), which is exactly why this must be reasoned about
explicitly rather than caught by a single-shot ASan run.

## 1. Chosen approach: a minimal `wasm_runtime_destroy_shared_heap` patch

Per review direction, we add deterministic per-heap destruction rather than a
re-pointable heap pool. Pooling mutable descriptors risks stale cached addresses
and cross-invocation exposure; deterministic destruction is easier to reason
about and to prove fail-closed.

**Proposed public API** (`core/iwasm/include/wasm_export.h`, mirroring
`create`):

```c
/**
 * Destroy a shared heap created by wasm_runtime_create_shared_heap.
 * The heap MUST be fully detached (attached_count == 0) and NOT part of any
 * chain (neither a head with chain_next set, nor a body referenced by another
 * heap's chain_next). Frees ONLY WAMR's descriptor; a pre-allocated backing
 * buffer is owned by the caller and is NOT touched. Returns true on success,
 * false (fail-closed, no free) if the preconditions are unmet or the pointer is
 * not a currently-registered heap (double-destroy / foreign pointer).
 */
WASM_RUNTIME_API_EXTERN bool
wasm_runtime_destroy_shared_heap(wasm_shared_heap_t heap);
```

Implementation site: `core/iwasm/common/wasm_memory.c`, beside
`wasm_runtime_create_shared_heap` / `unchain`.

## 2. Ownership

- WAMR frees **only its own descriptor** (`wasm_runtime_free(heap)`), and only
  for a **pre-allocated** heap (`heap_handle == NULL`) - Hull's only case. A
  runtime-managed heap (`heap_handle != NULL`) owns its `base_addr` mmap; if we
  ever destroy one, we must call `destroy_runtime_managed_shared_heap` first, but
  Hull never creates managed heaps for spans, so the initial patch **rejects
  `heap_handle != NULL`** (fail-closed) and documents managed-heap destruction as
  out of scope.
- The **mmap backing** (`base_addr`, the page-aligned `HlMappedBuffer` window) is
  **Hull's**. `destroy_shared_heap` never `munmap`s it. Hull releases the backing
  via the checkpoint-1 borrow/`hl_cap_fs_mmap_release` path AFTER the descriptor
  is destroyed. Ordering is Hull's responsibility (§7).

## 3. Preconditions (all checked; any failure → return false, no free)

Checked under `shared_heap_list_lock`:

1. `heap != NULL`.
2. **Registered:** `heap` is currently on `shared_heap_list` (walk the list). A
   pointer not on the list is a double-destroy or a foreign pointer → fail
   closed. This walk is also how we find the unlink predecessor (§4).
3. **Detached everywhere:** `heap->attached_count == 0`. A non-zero count means
   some module instance still has it as `e->shared_heap`; destroying it would
   dangle that pointer and the instance's last-used cache. Fail closed.
4. **Not chained:** `heap->chain_next == NULL` AND no other node's `chain_next`
   points at `heap` (found during the list walk). A chained heap must be
   `unchain`ed first (which also requires `attached_count == 0`). Fail closed.
5. **Pre-allocated only:** `heap->heap_handle == NULL` (see §2).

The precondition set means: **destroy-while-attached and destroy-while-chained
both fail closed**, and a heap can only be destroyed once it is provably
unreferenced by any instance cache or chain.

## 4. Global-list unlinking, lock ordering, concurrent-lookup exclusion

- **Single lock.** `shared_heap_list_lock` guards `shared_heap_list` and every
  `attached_count`. `destroy` takes exactly this lock - the same one `create`,
  `chain`, `unchain`, `attach`, and `detach` take - so there is no new lock and
  no lock-ordering hazard to introduce.
- **Unlink:** singly-linked via `next`. Walk from `shared_heap_list`; if `heap`
  is the head, `shared_heap_list = heap->next`; else `pred->next = heap->next`.
  The list walk in step 2/4 yields both `pred` and the "is anyone's body?" check
  in one pass.
- **Free after unlink, still under lock OR immediately after release.** Once
  unlinked, no concurrent `chain` (which walks the list under the lock) can
  re-discover the descriptor. We `wasm_runtime_free(heap)` after clearing
  `heap->next/chain_next` and dropping the lock (freeing outside the lock keeps
  hold-time minimal; nothing can reach the node once unlinked). `heap->size = 0`
  and pointer-scrub before free to make any errant reuse fail loudly under ASan.
- **Concurrent lookup exclusion.** The hot per-access path
  `is_app_addr_in_shared_heap` reaches a heap ONLY through an instance's
  `e->shared_heap` (`get_shared_heap`) and its `chain_next` - never the global
  `next` list. Precondition 3 (`attached_count == 0`, i.e. no instance references
  it) therefore guarantees **no execution thread can be mid-lookup against this
  heap** while it is destroyed. `chain` is the only other global-list reader; it
  holds `shared_heap_list_lock`, so it is serialized against the unlink. Hull
  additionally serializes create/chain/attach/detach/destroy for a module under
  `mod->mutex` (as segments do today), so two Hull threads never race the same
  heap.

## 5. Interpreter and AOT last-used cache invalidation

`detach` already clears the per-instance caches on both back ends
(`wasm_memory.c:642-690`): `shared_heap = NULL`, `base_addr_adj = NULL`,
`start_off = MAX`, `end_off = MAX-1` (so the `off >= start && off <= end-bytes+1`
range test can never match). Because destroy REQUIRES `attached_count == 0`
(§3.3), every instance that ever attached this heap has already run detach and
therefore already cleared its cache. **Destroy performs no cache manipulation
itself** - it cannot, as it does not (and must not) enumerate instances - but its
attached-count precondition is exactly the invariant that guarantees no interp or
AOT `shared_heap_base_addr_adj` / `read_only` still points into the descriptor or
its backing at reclamation time. The note calls this out so the reviewer can
confirm the precondition, not a second cache-clear, is the mechanism.

## 6. Runtime-teardown interaction (no double free)

`wasm_runtime_memory_destroy` frees every node still on `shared_heap_list`.
Because `destroy_shared_heap` **unlinks before it frees**, a heap destroyed
earlier is no longer on the list and is not freed a second time at teardown.
Symmetrically, a heap never individually destroyed is still on the list and is
reclaimed at teardown exactly as today - so mixing the two (Hull destroys
per-invocation spans; segments live to teardown) is safe.

## 7. Async / trap / error rollback ordering (how Hull uses it)

The checkpoint-2 Hull lifecycle (built on this patch) tears a span set down in
**reverse order** on every exit path - success, guest trap, host error,
cancellation, and instance teardown:

1. `detach` the chain from the instance (clears caches, `attached_count--`) - on
   the owning thread only, after the WASM call has returned.
2. `unchain` the chain (requires `attached_count == 0`).
3. `destroy_shared_heap` each member (requires detached + unchained).
4. `hl_cap_fs_mmap_release` each borrowed backing (Hull owns it; only now may the
   mmap go).

Destroy sits strictly between "unchained + detached" and "backing released". For
async, detach+unchain+destroy run on the worker thread that ran the job, as part
of the job's completion path (identical for success and trap), happening-before
the completion is signalled to the event loop - so no destroy races an executing
instance. Partial-attach failure unwinds the same ladder from the last
successful step downward.

## 8. Tests (WAMR-unit + Hull-level)

WAMR-unit (upstream-style, `tests/unit/shared-heap/`, globbed - no CMake change,
mirroring patch 0002's test wiring), under BOTH interpreter and AOT:

- **No list growth:** repeated create → attach → detach → unchain → destroy in a
  loop leaves `shared_heap_list` length constant (assert via a test-only length
  probe or by observing stable RSS + a create/destroy balance counter).
- **Fail-closed:** destroy-while-attached returns false and frees nothing;
  destroy-while-chained returns false; double-destroy returns false (pointer no
  longer on the list); destroy of a `heap_handle != NULL` returns false.
- **Stale-address rejection:** after destroy, an instance that retained the old
  WASM address range (from a prior attach) traps on access (the detach precondition
  already cleared its cache; the range is unattached) - host survives.
- **Teardown safety:** create N, destroy some, then `wasm_runtime_destroy()` - no
  double free (ASan).
- **Concurrency:** a second thread attempting `chain`/`create` while one thread
  destroys is serialized by `shared_heap_list_lock` and leaves a consistent list
  (TSan clean).

Hull-level: exercised transitively by the checkpoint-2 lifecycle suite; this
patch's own gate is the WAMR-unit set above plus **ASan / MSan / TSan
cleanliness** and the existing writable/RO shared-heap cases (patch 0002) staying
green.

## 9. Upstreamability and relation to patches 0001 / 0002

- **Upstreamable.** `create_shared_heap` with no destroy is a genuine upstream
  gap; a symmetric `wasm_runtime_destroy_shared_heap` with fail-closed
  preconditions is a clean, self-contained addition that changes no existing
  behavior (segments and managed heaps are unaffected; the new path is opt-in).
  We should offer it upstream; landing it out-of-tree first is the pragmatic
  path given our pinned base.
- **Carriage.** It becomes **patch 0003** in `patches/wamr/`, recorded in
  `docs/wamr_patches.md` with base commit + SHA-256, applied by
  `scripts/wamr_apply_patches.sh` after 0001 (test-harness portability) and 0002
  (read-only permission) with the same verify-base / stale-hash / offset /
  reverse-check / unexpected-source gates. It touches the same two files 0002
  does - `core/iwasm/common/wasm_memory.c` and
  `core/iwasm/include/wasm_export.h` - plus the shared-heap unit test dir; it is
  orthogonal to 0002's enforcement sites (no overlap with the interp/AOT store
  gates), so the two apply independently and cleanly.

## 10. Non-goals

No managed-heap (`heap_handle != NULL`) destruction; no re-pointable heap pool;
no change to `create`/`chain`/`unchain`/`attach`/`detach`/`reset` semantics; no
Hull lifecycle code (that is checkpoint 2, built after this lands). No public
spans API, SDK header, metadata query, or `compute.call({spans=...})`.

## Related

- `docs/wasm_mapped_spans_design.md` (the feature), `docs/wamr_patches.md` (the
  carriage), `patches/wamr/0002-shared-heap-readonly-permission.patch`.
- WAMR `core/iwasm/common/wasm_memory.c` (shared-heap impl),
  `core/iwasm/interpreter/wasm_runtime.h` (`WASMSharedHeap`).
