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

## 3. The single critical section: identity-search, eligibility, unlink, free

Everything happens inside ONE `shared_heap_list_lock` critical section, in this
exact order, so there is never a window between "eligible" and "gone", and no
field of a possibly-freed pointer is read before its registration is proven.

```
lock(shared_heap_list_lock)
  # (a) IDENTITY SEARCH FIRST -- compare pointers, never deref `heap` yet.
  walk shared_heap_list: find `pred` s.t. pred->next == heap (or heap == head);
                         in the same pass note if any node's chain_next == heap.
  if heap is NOT on the list:            unlock; return false   # unknown / already-destroyed
  # `heap` is now a proven-registered, live descriptor -> safe to read its fields.
  # (b) ELIGIBILITY -- read fields only now, still under the lock.
  if heap->heap_handle != NULL:          unlock; return false   # runtime-owned (out of scope)
  if heap->attached_count != 0:          unlock; return false   # still attached
  if heap->chain_next != NULL
     OR heap is some node's chain_next:  unlock; return false   # still chained
  # (c) UNLINK + FREE, all before releasing the lock.
  unlink heap from shared_heap_list (fix pred->next / head)
  heap->next = heap->chain_next = NULL; heap->base_addr = NULL; heap->size = 0
  wasm_runtime_free(heap)
unlock(shared_heap_list_lock)
return true
```

Point-by-point:

- **Identity search before any dereference (double-destroy safety).** Step (a)
  only compares `cur == heap` while walking; it never dereferences `heap`. A
  freed or foreign pointer is simply "not found" and returns false BEFORE any
  `heap->field` read, so a double-destroy cannot touch freed memory. Only after
  (a) proves `heap` is a live node on the list do we read `heap->heap_handle` /
  `attached_count` / `chain_next` in (b).
- **`attached_count` synchronization.** WAMR mutates `attached_count` ONLY under
  `shared_heap_list_lock` (`attach` +1 at `wasm_memory.c:623-625`, `detach` -1 at
  `654-656`; read under the lock by `chain`/`unchain`/`reset`). Reading it in (b)
  under the same lock is therefore consistent - it is a lock-protected read, not
  an unsynchronized precondition. No atomics are introduced.
- **`attached_count == 0` does NOT by itself prove non-retention.** Per the
  `WASMSharedHeap` comment, *only the chain head maintains a valid
  `attached_count`*; a chain **body** keeps `attached_count == 0` even while the
  chain is attached and reachable (an instance's `e->shared_heap` is the head,
  and `is_app_addr_in_shared_heap` walks `chain_next` into the body). Detach +
  `attached_count == 0` alone would let a still-referenced body be freed.
  Therefore the **unchained** check in (b) is INDEPENDENTLY necessary: `heap` may
  be destroyed only when it is neither a head with `chain_next` set nor any
  node's `chain_next`. The correct proof of non-retention is
  `attached_count == 0` (head detached) **AND** fully unchained (this node is a
  standalone list entry). Hull's teardown therefore always runs detach -> unchain
  -> destroy (§7); after `unchain` every member is a standalone heap with
  `attached_count == 0` and `chain_next == NULL`.
- **Unlink + free under the lock (no post-eligibility attachment).** Unlink and
  free happen in the SAME critical section as the eligibility read - there is no
  moment where the descriptor is both "eligible" and still reachable. `chain`
  (the only other global-list reader) is serialized by the same lock, so it can
  never re-discover a half-destroyed node.
- **`attach` is NOT gated by the global list (caller contract).** `attach`
  (`wasm_memory.c:578-628`) does not walk `shared_heap_list`; it takes the lock
  only to bump `attached_count` on a caller-supplied pointer. So unlinking does
  NOT prevent a concurrent/subsequent `attach` on the same raw pointer - that is
  a use-after-destroy and is prevented by the CALLER, exactly as use-after-free
  is: Hull serializes attach/detach/chain/destroy for a module's heaps under
  `mod->mutex` (as segments do today) and never reuses a destroyed pointer. If we
  later want WAMR-level enforcement, `attach` would need to verify the heap is
  still registered (an added O(list) walk); the initial patch documents the
  caller contract rather than paying that cost. This is called out so the
  reviewer decides explicitly.

## 4. Exact return / error behavior (every failure is total: false, no free, no mutation)

The API returns `bool` (`true` only when a descriptor was actually unlinked and
freed). Every failure path returns `false`, frees nothing, and mutates no state,
with a distinct `LOG_WARNING`:

| Case | Detected by | Return | Effect |
|---|---|---|---|
| NULL pointer | `heap == NULL` | `false` | none |
| Unknown / already-destroyed | not found in identity search (a) | `false` | none (no deref) |
| Runtime-owned (managed) heap | `heap_handle != NULL` | `false` | none |
| Still attached | `attached_count != 0` | `false` | none |
| Still chained (head or body) | `chain_next != NULL` / is a body | `false` | none |
| Eligible | all checks pass | `true` | unlinked + freed |

A `false` return is always "nothing changed", so a caller may safely retry after
fixing the precondition (detach / unchain) without risking a partial teardown.

## 5. Interpreter and AOT last-used cache invalidation - what detach actually clears

`detach` (`wasm_memory.c:642-690`) clears, on BOTH the interp and AOT sides:
`e->shared_heap = NULL` (the heap pointer), `shared_heap_start_off = UINT{64,32}_MAX`,
`shared_heap_end_off = MAX - 1`, and `shared_heap_base_addr_adj = NULL`.

**Correction (verified against the source): `detach` does NOT reset
`e->shared_heap_read_only`.** It clears the heap pointer and the address offsets
but leaves the read-only flag stale. This is currently *moot for correctness*
because a store's shared-heap gate only consults `read_only` once an address
falls inside `[start_off, end_off]`, and detach sets `start_off = MAX`,
`end_off = MAX-1` (an empty range that no `off <= end - bytes + 1` can satisfy),
so the stale `read_only` is never read before the next `attach` overwrites it.
It is nonetheless a latent cache-hygiene gap. **Recommendation for this patch:
also set `e->shared_heap_read_only = false` in `wasm_runtime_detach_shared_heap_internal`
on both back ends**, so every cache field (heap pointer, offsets, base-addr-adj,
AND read_only) is cleared by detach. It is one assignment per back end, keeps the
"detached => no stale cache state whatsoever" invariant literally true, and makes
the destroy precondition's reliance on detach airtight.

Because destroy REQUIRES `attached_count == 0` (head detached) and fully
unchained (§3), and because Hull runs detach on **every** instance that attached
the chain before destroying, every such instance has already cleared its cache
(heap pointer + offsets + base-addr-adj, and read_only once the recommendation
lands). Destroy performs no cache manipulation itself - it does not and must not
enumerate instances; the detach-first precondition is the mechanism.

## 6. Ownership scope: the process-global list and multi-runtime

`shared_heap_list` / `shared_heap_list_lock` are **file-static globals** in
`wasm_memory.c` (one per process), and WAMR is a **ref-counted single global
runtime**: `wasm_runtime_init` / `wasm_runtime_full_init` guard a
`runtime_ref_count` (`wasm_runtime_common.c`), so repeated inits ref-count ONE
runtime rather than creating independent ones. There is therefore exactly one
runtime and one shared-heap list per process; the process-global list IS the
correct ownership scope, and there is **no second runtime/global context** whose
descriptors this API could reach. The identity search (§3a) additionally proves
the pointer belongs to THIS (the only) global list before freeing, so a foreign
or bogus pointer returns false. If WAMR ever gains genuinely independent
coexisting runtimes, the list (and this API's search + teardown) would have to
become per-runtime; the patch documents that as a forward assumption so the
constraint is not silently baked in.

## 7. Runtime-teardown interaction and destroy-vs-teardown ordering (no double free)

Teardown is `wasm_runtime_memory_destroy` -> `destroy_shared_heaps`
(`wasm_memory.c:951-973`): under `shared_heap_list_lock` it grabs the head and
sets `shared_heap_list = NULL`, releases the lock, frees every node via `next`,
then **`os_mutex_destroy(&shared_heap_list_lock)`**.

- **No double free.** `destroy_shared_heap` **unlinks before it frees**, so a
  heap destroyed earlier is no longer on the list and is not freed again at
  teardown. A heap never individually destroyed stays on the list and is
  reclaimed at teardown exactly as today - so mixing the two (Hull destroys
  per-invocation spans; segments live to teardown) is safe.
- **Destroy must not run once teardown has begun (Hull ordering invariant).**
  Because teardown frees nodes AFTER releasing the lock and then DESTROYS the
  lock, a `destroy_shared_heap` that started concurrently is serialized for the
  list mutation, but calling it AFTER teardown began is undefined (the lock is
  gone). Hull's invariant: `wasm_runtime_destroy` (`hl_cap_wasm_destroy`) runs
  strictly LAST, after every span set and instance has been torn down and no
  further compute call can start. This is the same lifecycle position Hull
  already gives runtime teardown; the note makes the dependency explicit.

## 8. Async / trap / error rollback ordering (how Hull uses it)

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

## 9. Tests (WAMR-unit + Hull-level)

WAMR-unit (upstream-style, `tests/unit/shared-heap/`, globbed - no CMake change,
mirroring patch 0002's test wiring), under BOTH interpreter and AOT.

**Reclamation actually reclaims (list length + chain cost, not memory at exit).**
The load-bearing test must prove the *list* does not grow and *chain cost* stays
flat - NOT merely that memory is balanced when the process exits (teardown frees
everything regardless, so an exit-time check proves nothing). Concretely, via a
test-only debug accessor `wasm_runtime_shared_heap_count()` (a length probe added
under `#if WASM_ENABLE_SHARED_HEAP`, returning the `shared_heap_list` node count
under the lock):

- Run N (e.g. 100k) rounds of create -> attach -> detach -> unchain -> destroy;
  assert `shared_heap_count()` is **constant** (== the steady-state segment count)
  after every round, not just at the end.
- Assert **chain cost is O(steady-state)**: instrument (test-only) or bound the
  node count `chain_shared_heaps` walks, and show it does not grow with the round
  index. A leaking list would make round K's chain walk O(K).

Race and fail-closed cases (TSan for the races):

- **Destroy vs attach:** one thread `attach`es a heap while another `destroy`s it.
  Because Hull forbids this (mod->mutex), the WAMR-unit test asserts the caller
  contract by construction (serialized) AND documents that an unserialized race is
  a use-after-destroy; if we adopt the optional attach-time registration check, add
  a test that the racing attach then fails closed.
- **Destroy vs chain:** concurrent `chain` (walks the list) and `destroy` (unlinks)
  leave a consistent list; no crash, no lost/duplicated node (TSan).
- **Destroy vs a second destroy:** two threads destroy the same pointer; exactly
  one returns true (frees once), the other returns false (identity search misses).
- **Destroy vs teardown:** a destroy that begins before `wasm_runtime_destroy`
  either completes first (node gone; teardown frees the rest) or finds the list
  already nulled and returns false - and Hull's ordering invariant (§7) means
  destroy is never *initiated* after teardown starts; the test exercises the
  in-window race, ASan-clean, no double free.
- **Fail-closed matrix (§4):** NULL, unknown/already-destroyed, `heap_handle != NULL`,
  still-attached, still-chained - each returns false, frees nothing, mutates nothing.
- **Stale-address rejection:** after destroy, an instance that retained the old
  WASM address range traps on access (its cache was cleared at detach; the range is
  unattached) - host survives.
- **read_only cache clear:** with the §5 detach change, assert a detached
  instance's `shared_heap_read_only` is reset (a white-box or behavioral check that
  a re-attach of a *writable* heap into the same instance is not treated read-only).

Hull-level: exercised transitively by the checkpoint-2 lifecycle suite; this
patch's own gate is the WAMR-unit set above plus **ASan / MSan / TSan
cleanliness** and the existing writable/RO shared-heap cases (patch 0002) staying
green.

## 10. Upstreamability and relation to patches 0001 / 0002

- **Upstreamable.** `create_shared_heap` with no destroy is a genuine upstream
  gap; a symmetric `wasm_runtime_destroy_shared_heap` with fail-closed
  preconditions is a clean, self-contained addition that changes no existing
  behavior (segments and managed heaps are unaffected; the new path is opt-in).
  The §5 detach `read_only`-reset is a one-line hygiene fix also worth upstreaming
  (independently of the read-only feature, resetting a stale flag on detach is
  strictly correct). We should offer both upstream; landing out-of-tree first is
  the pragmatic path given our pinned base.
- **Carriage.** It becomes **patch 0003** in `patches/wamr/`, recorded in
  `docs/wamr_patches.md` with base commit + SHA-256, applied by
  `scripts/wamr_apply_patches.sh` after 0001 (test-harness portability) and 0002
  (read-only permission) with the same verify-base / stale-hash / offset /
  reverse-check / unexpected-source gates. It touches the same two files 0002
  does - `core/iwasm/common/wasm_memory.c` (the new `destroy_shared_heap`, the
  test-only length probe, and the one-line `read_only` reset in
  `detach_shared_heap_internal`) and `core/iwasm/include/wasm_export.h` (the
  prototype) - plus the shared-heap unit test dir. It is orthogonal to 0002's
  interp/AOT store-gate enforcement sites, so the two apply independently and
  cleanly.

## 11. Non-goals

No managed-heap (`heap_handle != NULL`) destruction; no re-pointable heap pool;
no change to `create`/`chain`/`unchain`/`attach`/`detach`/`reset` semantics; no
Hull lifecycle code (that is checkpoint 2, built after this lands). No public
spans API, SDK header, metadata query, or `compute.call({spans=...})`.

## Related

- `docs/wasm_mapped_spans_design.md` (the feature), `docs/wamr_patches.md` (the
  carriage), `patches/wamr/0002-shared-heap-readonly-permission.patch`.
- WAMR `core/iwasm/common/wasm_memory.c` (shared-heap impl),
  `core/iwasm/interpreter/wasm_runtime.h` (`WASMSharedHeap`).
