# WASM Compute — Improvement Roadmap

Findings from architectural review of the compute module. Ordered by severity.

---

## 1. Thread Safety: attach_shared_data without mutex ✅

**Where:** `wasm.c` — `hl_cap_wasm_call_buf()` and `hl_cap_wasm_instance_create()`

**Problem:** `attach_shared_data(mod, inst)` reads `mod->shared_data->chain_head`
after pool_mutex was released. A concurrent `hl_cap_wasm_data_load()` could be
rebuilding the chain (freeing/replacing chain_head). Same issue with
`tl_host_ctx.shared_data = mod->shared_data` — torn read of the pointer.

**Fix:** `pool_acquire()` now snapshots `shared_data` and `chain_head` under the
mutex. `hl_cap_wasm_instance_create()` snapshots during the cache_find lock.
Snapshotted values passed to `attach_shared_heap()` and `tl_host_ctx`.

---

## 2. Thread Safety: hl_cap_wasm_destroy without mutex ✅

**Where:** `wasm.c`

**Problem:** `hl_cap_wasm_destroy()` calls `pool_drain()` and `free_shared_data()`
without holding pool_mutex. Race with in-flight `hl_wasm_pool_release()`.

**Fix:** Lock pool_mutex around the destroy loop, unlock before mutex_destroy.

---

## 3. MappedBuffer GC pin in Lua compute.segment ✅

**Where:** `lua/modules.c` — `lua_compute_data()`

**Problem:** MappedBuffer addr is passed as `pre_alloc` to `hl_cap_wasm_data_load()`.
If Lua GC collects the MappedBuffer, the backing is munmap'd while the shared heap
still references it → use-after-free.

**Fix:** Per-module registry table `__hull_data_<module>` maps segment names to
MappedBuffer userdata references. Pinned on add, released on remove/remove-all.

---

## 4. Chain rebuild failure leaves inconsistent state ✅

**Where:** `wasm.c` — `hl_cap_wasm_data_load()`

**Problem:** If `rebuild_chain()` fails after the segment was added to the array,
the state is inconsistent.

**Fix:** On rebuild failure, rollback: free the segment, decrement count, free
shared_data struct if empty. Return error with mutex still held (no unlock gap).

---

## 5. JS compute.segment missing MappedBuffer support ✅

**Where:** `js/modules.c` — `js_compute_data()`

**Problem:** Only accepted string or ArrayBuffer. No zero-copy path from mmap.

**Fix:** Added MappedBuffer check via `js_mmap_class_id`. Sets `pre_alloc` for
zero-copy. Note: JS MappedBuffer GC pin not yet implemented (needs the same
registry pattern as Lua, but via QuickJS prevent-GC mechanism).

---

## 6. JS async paths missing MappedBuffer input ✅

**Where:** `js/modules.c` — `js_compute_async_call()`, instance call/async paths

**Problem:** MappedBuffer accepted as input for sync `compute.call()` but not for
`compute.async.call()`, `inst.call()`, or `inst.async.call()`.

**Fix:** Added MappedBuffer check to all four JS call paths.

---

## 7. Deduplicate wasm_clamp_opts ✅

**Where:** `lua/modules.c`, `js/modules.c`

**Problem:** Identical clamping logic duplicated in both runtimes.

**Fix:** Moved to `hl_cap_wasm_clamp_opts()` in `wasm.c`. Both runtimes delegate
to the shared implementation via thin wrappers.

---

## 8. Split shared data into wasm_data.c ✅

**Where:** `wasm.c` — ~250 lines of segment/chain/mmap management

**Problem:** Shared data management (page-aligned mmap, WAMR shared heap creation,
chain building, segment lifecycle) is mixed with call dispatch. Different lifecycle
and locking semantics.

**Fix:** Extracted to `src/hull/cap/wasm_data.c` + `include/hull/cap/wasm_data.h`.
Public API and internal helpers moved. `hl_wasm_pool_drain` exported from wasm.c.

---

## 9. Per-module mutex (pool_mutex contention) ✅

**Where:** `wasm.c` — single `cache->pool_mutex`

**Problem:** One mutex guards the module cache array, all per-module pools, and all
per-module shared data. Loading shared data for module A blocks pool acquisition
for module B.

**Fix:** Added `pthread_mutex_t mutex` to `HlWasmModule`. `cache->pool_mutex` now
only guards module insertion/lookup. Per-module mutex guards pool and shared data.

---

## 10. Document compute.buffer() in CLAUDE.md ✅

**Where:** CLAUDE.md WASM section

**Problem:** `compute.buffer(input)` creates a WasmBuffer from a string. Implemented
in both Lua and JS but not documented in the API listing.

**Fix:** Added to both Lua and JS API code blocks.
