# Zero-copy host-backed mapped spans for WASM compute — design (DESIGN ONLY)

**Status:** DESIGN ONLY, for formal review. **Implementation is UNAUTHORIZED until
this review passes.** No code, SDK header, WAMR patch, or benchmark is written
yet; everything below is what the implementation PR *will* do, not evidence
already collected.

This evolves Hull's EXISTING zero-copy mechanism (`compute.segment` + WAMR shared
heaps + `HlMappedBuffer`); it does NOT add a parallel buffer subsystem. It
incorporates the reviewer's cut-1 scope decisions and security revisions, and the
second-round directives: the four decisions are RESOLVED (§7), the WAMR permission
model is enumerated across EVERY shipped write path with its three insertion sites
(§3.1), the window cap is 1 GiB total per invocation on both targets (§3.6), span
metadata is a versioned linear-memory struct cached once (§3.3), and the additional
verification items (RO-reject-before-translate, `memory.copy` boundary/RO-dst,
detach-cannot-race, overflow-safe total accounting, overlap/native-dup fail-closed,
metadata non-leak, existing-writable-users preserved, upstream WAMR unit tests) are
folded into §3-§5. **Implementation stays blocked until the §3.1 permission model
is proven complete against all enumerated write paths.**

## 0. What already exists vs what is new

Hull already maps a file zero-copy and lets WASM read it at native speed:

- `fs.mmap(path)` -> `HlMappedBuffer {addr,len,closed,alloc,borrow_count,pending_free}`
  (`cap/fs.c:299`, `include/hull/cap/fs.h:135`) with a refcounted borrow-pin
  (`hl_cap_fs_mmap_borrow`/`_release`, `cap/fs.c:377`) that defers `munmap` while
  a consumer borrows.
- `compute.segment(mod, name, fs.mmap(...))` places that mmap region into the
  **high end of the WASM address space** via a WAMR **shared heap**
  (`wasm_runtime_create_shared_heap(pre_allocated_addr=...)`,
  `wasm_runtime_chain_shared_heaps`, `wasm_runtime_attach_shared_heap`;
  `cap/wasm_data.c:58,415`, addr `= UINT{32,64}_MAX - size + 1`).
- WASM asks for the region's WASM address ONCE via `host_call(0x02, seg, 0)`
  (`cap/wasm.c:143`) then reads it directly. Under **AOT** a shared-heap load
  lowers to a **cached-range check + a native load** with **no per-access host
  call** (`vendor/wamr/.../aot_emit_memory.c:385`
  `aot_check_shared_heap_memory_overflow`). WAMR is `2.4.1-218-gc3a78cd1` with
  `WASM_ENABLE_SHARED_HEAP=1`, `WASM_ENABLE_MEMORY64=1`, and **software** bounds
  checks (`mk/vendor/wamr.mk`; no `OS_ENABLE_HW_BOUND_CHECK`).

**New in this feature (the delta):**

1. A **per-invocation** span API (`compute.call(..., {spans={...}})`) that attaches
   ONLY the passed buffers for ONE call and detaches on every exit path — distinct
   from `compute.segment`'s module-scoped, shared, persistent lifetime.
2. **Windowed** `fs.mmap({offset,length})` so a >4 GiB file is processed as
   repeated invocations over fixed windows; `HullSpan.foffset` carries the 64-bit
   logical position.
3. A tiny SDK header `hull/wasm/span.h` with **overflow-safe, alignment-safe,
   inline** typed accessors (convenience only — NOT the isolation boundary).
4. A **narrow, documented WAMR patch** that makes a read-only mapped region trap a
   WASM store instead of crashing the host (§3.1) — the security linchpin.

## 1. Cut-1 scope decisions (per reviewer direction)

- **Windowing is IN cut 1, but a window is FIXED for one invocation.** There is
  NO in-WASM `hull_span_window()` — remapping requires host coordination and would
  invalidate the attached address mid-execution. The window is chosen host-side by
  `fs.mmap({offset,length})` (page-aligned internally; caller stays
  alignment-agnostic), attached for the call, and immutable during it. A parser
  over a huge file loops in the HOST, issuing repeated invocations over successive
  windows; each `HullSpan` reports its `foffset` so WASM code knows its logical
  position. Memory64 builds may accept larger windows subject to an explicit cap
  (§3.6), not "whole file by default".
- **New API alongside `compute.segment`, not a change to it.**
  ```lua
  compute.call("mod", input, { spans = { { name = "source", buffer = mapped } } })
  ```
  Attaches only those spans before the call; detaches on success/trap/error.
  `compute.segment` keeps its exact current lifetime/sharing semantics and is
  documented as the **legacy module-scoped/shared** mechanism. The `spans` option
  is also accepted by `compute.async.call` and a persistent `instance:call`.

## 2. The six architecture points (revised)

1. **Extended structures.** `HlMappedBuffer` (the "HullBuffer" owner) gains a
   windowed-mmap constructor; `HlWasmDataSegment`/`HlWasmSharedData` become the
   per-invocation attached-region metadata; `HlWasmBuffer` MMAP-kind is reused as
   the span's backing handle. No new buffer type.
2. **mmap ownership** stays in `HlMappedBuffer` (`cap/fs.c`). A span BORROWS it;
   the invocation pins it (`borrow`/`pending_free`) for the call (and, if async,
   until completion), so a span can never outlive its mapping.
3. **HullSpan representation** — no raw host pointer, ABI-explicit (§3.3). WASM
   obtains `{base,len,foffset}` per named span via a host_call query (extending
   opcode `0x02`), then builds a local `HullSpan` whose `base` is a WASM address of
   the module's own address width.
4. **WAMR change** — the fast READ path needs NO patch (shared-heap lowering
   already gives bounds-checked native reads). The ONLY WAMR change is the RO-store
   trap (§3.1), kept isolated and documented.
5. **wasm32 windowing** — §1 and §3.6.
6. **Isolation invariant** — §3.4/§3.5/§3.7: a module can address only the spans
   attached to ITS current invocation, enforced by WAMR's registered-range checks,
   not by the SDK header.

## 3. Security design (the eight required revisions)

### 3.1 Read-only mapping alone does NOT trap safely — a narrow WAMR patch is required

**Confirmed gap:** `SharedHeapInitArgs` is `{size, pre_allocated_addr}` (no
permission field) and `aot_check_shared_heap_memory_overflow` is a pure RANGE
check. A hostile WASM **store** into a `PROT_READ`-mapped shared-heap region passes
the range check, becomes a native store into read-only memory, and — because Hull
uses **software** bounds checks with **no signal handler** — raises a host SIGSEGV,
i.e. a **host process crash, not a recoverable trap**. Kernel `PROT_READ` cannot be
relied on for a controlled trap.

**The permission model (reviewer resolution 1).** Add EXPLICIT read/write
permission to shared-heap metadata and thread the ACCESS KIND (load vs store)
through WAMR's centralized memory validation, which today makes NO load/store
distinction in the shared-heap branch. Concretely:

- `WASMSharedHeap` (`core/iwasm/interpreter/wasm_runtime.h:95`) gains a
  `bool read_only`; the public `SharedHeapInitArgs`
  (`core/iwasm/include/wasm_export.h:352`) gains a `flags`/`read_only` field.
  **Default is WRITABLE (`read_only=false`)** so every existing writable user is
  byte-for-byte unchanged (see "existing users" below). RO is only coherent for
  the **pre-allocated, `heap_handle==NULL`** heap (Hull's mmap path); the managed
  malloc/free/reset paths require writable and are never marked RO.

**Complete write-path table — EVERY write path, given Hull's actually-shipped
flags (`SIMD=1`, `BULK_MEMORY=1`, `BULK_MEMORY_OPT=1`, `SHARED_MEMORY=0`, `GC=0`,
`REF_TYPES=0`, `STRINGREF=0`, software bounds checks).** Enumerating all of them
is the point; missing one is a hole.

| Write op | Ships? | Interp site | AOT site | Through shared-heap translate? |
|---|---|---|---|---|
| i32/i64.store, store8/16/32, f32/f64.store | yes | `wasm_interp_fast.c` store block via `CHECK_MEMORY_OVERFLOW` | `aot_compile_op_*_store` → `aot_check_memory_overflow` | yes |
| SIMD v128.store, store{8,16,32,64}_lane | yes | `CHECK_MEMORY_OVERFLOW(16 / w/8)` | `simd_store` → `aot_check_memory_overflow` | yes |
| memory.copy (dst) | yes | `CHECK_BULK_MEMORY_OVERFLOW` src+dst → `memmove` | `check_bulk_memory_overflow` src+dst → memmove | yes (src AND dst translated per-endpoint) |
| memory.fill (dst) | yes | `CHECK_BULK_MEMORY_OVERFLOW` dst → `memset` | `check_bulk_memory_overflow` dst → memset | yes |
| **memory.init (dst)** | yes | `CHECK_BULK_MEMORY_OVERFLOW` → `bh_memcpy_s` | **`aot_memory_init` runtime helper → `wasm_runtime_validate_app_addr` + `_addr_app_to_native`** | yes — but via a **DIFFERENT** site (C helpers, not codegen) |
| atomics: atomic.store/rmw/cmpxchg | **NO** | `#if WASM_ENABLE_SHARED_MEMORY` (off) | same guard | n/a (dead) |
| table/GC/ref/stringref writes | **NO** | compiled out | compiled out | n/a |
| memory.grow | yes | grows LINEAR memory only | same | n/a (cannot resize a shared heap) |

Confirmed: **every shipping write funnels through the shared-heap translate; no
bypass exists** in Hull's configuration. Atomics do not ship.

**The THREE insertion sites (the check must precede native pointer formation —
reviewer: RO rejection before translation/dereference).**

1. **Interpreter (fast + classic).** The single chokepoint is
   `CHECK_SHARED_HEAP_OVERFLOW` (`core/iwasm/common/wasm_memory.h`), reached by both
   `CHECK_MEMORY_OVERFLOW` and `CHECK_BULK_MEMORY_OVERFLOW`. Insert the RO check
   **between** `is_app_addr_in_shared_heap()` matching a heap and
   `shared_heap_addr_app_to_native` forming the native pointer. Because loads share
   the macro, thread the access kind via a store-specific macro variant (or return
   the matched heap's `read_only` from `is_app_addr_in_shared_heap`, which already
   locates the heap in `update_last_used_shared_heap`). Loads unaffected.
2. **AOT scalar / SIMD / memory.copy / memory.fill.** The codegen chokepoint is
   `build_get_maddr_in_cache_shared_heap` (`aot_emit_memory.c`), set up by
   `aot_check_shared_heap_memory_overflow` and
   `aot_check_bulk_memory_shared_heap_memory_overflow`. Materialize the heap's
   `read_only` bit into `func_ctx` beside the existing
   `shared_heap_base_addr_adj/start_off/end_off`, thread an `is_store` flag from the
   store call sites (i32/i64/f32/f64 store, `simd_store`, memory.copy **dst**,
   memory.fill **dst**), and branch to the exception block **before** the GEP that
   forms `maddr`. Loads pass a false flag and are unchanged.
3. **AOT `memory.init` — the DISTINCT site that is easy to miss (blocking).** AOT
   memory.init does NOT go through the codegen chokepoint; it calls the runtime
   helpers `wasm_runtime_validate_app_addr` then `wasm_runtime_addr_app_to_native`
   (`core/iwasm/common/wasm_memory.c`). Because memory.init is ALWAYS a store, add
   an UNCONDITIONAL RO rejection at the `is_app_addr_in_shared_heap`-true branch in
   `wasm_runtime_validate_app_addr` (the pre-write gate), before the native copy.
   This site is separate from (2); the review calls it out explicitly so the patch
   cannot ship covering scalar/SIMD/copy/fill while leaving AOT memory.init open.

**memory.copy correctness (reviewer: reject RO dst OR a range crossing heap
boundaries).** `is_app_addr_in_shared_heap` requires the WHOLE range inside one
heap (`off >= start && off <= end - bytes + 1`), so a boundary-crossing copy
already returns false → falls to the linear-mem bounds check → traps: **crossing
already fails closed**. src and dst are translated independently, so an RO check on
the dst endpoint catches an RO destination even when src is a different writable
heap. The RO check therefore composes with the existing whole-range gate.

**Existing writable shared-heap users retain behavior.** `read_only` defaults
false; the managed-heap malloc/free/reset (`wasm_runtime_shared_heap_malloc/_free`,
`reset_shared_heap_chain`'s `memset`), the `env.shared_heap_malloc/free` imports,
and WAMR's own `tests/unit/shared-heap` write cases (`_rmw*`, bulk-into-heap,
memory64) all run against writable heaps and are unaffected. Only Hull's
pre-allocated RO mmap heaps carry the flag.

**Gating rule.** The implementation PR must FIRST land the permission patch with
**upstream-style WAMR unit tests** (not only Hull e2e): add `TEST_F`s to
`vendor/wamr/tests/unit/shared-heap/shared_heap_test.cc` (globbed — no CMake
change) plus a write-into-RO-heap fixture in `wasm-apps/test.c`, asserting that a
scalar store, a `v128.store`, a `memory.copy` dst, a `memory.fill`, AND an AOT
`memory.init` into a read-only heap each FAULT (recoverable trap, host survives)
while a LOAD from the same heap succeeds — under BOTH interpreter and AOT. Only
after every shipped write path is proven to fail closed does the rest of the
feature proceed. The patch stays in the enumerated sites, documented in
`docs/wamr_patches.md` with exact upstream files/functions for upgrade-time
re-application. The `OS_ENABLE_HW_BOUND_CHECK` + signal-handler route is recorded
as a fallback only (larger surface; conflicts with Hull's no-signal/W^X posture).

### 3.2 Unaligned- and overflow-safe SDK accessors

The SDK header must NOT do `*(uint32_t*)(base+off)` (C alignment/aliasing UB on
unaligned data; also an unsafe wasm32 pointer cast). Accessors use **`memcpy`
into a local** (the compiler lowers it to correct, possibly-unaligned WASM loads;
no aliasing UB) with an **overflow-safe** bounds check written as
`width <= len - off` guarded by `off <= len` (never `off + width <= len`, which can
wrap):

```c
static inline int hull_span_check(const HullSpan *s, uint64_t off, uint64_t w) {
    return off <= s->len && w <= s->len - off;   /* overflow-safe */
}
static inline uint32_t hull_span_load_le32(const HullSpan *s, uint64_t off) {
    uint32_t v = 0;
    if (!hull_span_check(s, off, 4)) hull_span_trap();   /* controlled abort */
    __builtin_memcpy(&v, hull_span_ptr(s, off), 4);       /* unaligned-safe */
    return le32toh_hull(v);                                /* explicit byte order */
}
```

`le/be` accessors do the byte-order swap explicitly so binary formats (PBF,
Parquet) are correct on any host. Provide the minimal coherent set
(`u/i/f {8,16,32,64}` + `le/be {16,32,64}`); stores exist only for a future
writable mapping and are compile-time absent for RO spans. The SAME header
compiles natively (accessors over a plain `HlBufferView`) for differential tests
and benchmarks.

### 3.3 wasm32 / wasm64 ABI and struct layout — precise, no casual 64->32 casts

`HullSpan.base` is a WASM address of the MODULE's pointer width, never a truncated
host `uint64_t`. Layout is target-explicit:

```c
#if defined(__wasm64__)
  typedef uint64_t hull_wasm_addr_t;
#else
  typedef uint32_t hull_wasm_addr_t;   /* wasm32 */
#endif
typedef struct HullSpan {
    hull_wasm_addr_t base;   /* WASM address of window[0], module pointer width */
    uint64_t         len;    /* window length in bytes (<= the per-invocation cap) */
    uint64_t         foffset;/* 64-bit LOGICAL file offset of window[0] */
    uint32_t         flags;  /* bit0 = read-only (cut 1: always set) */
} HullSpan;
```

`hull_span_ptr(s, off)` returns `(const uint8_t *)(uintptr_t)(s->base + off)` where
`base` is already the correct width — no `uint64_t`->wasm32 truncation. The host
validates on wasm32 that `base + len - 1` fits in 32 bits before attaching (else
the window is rejected — see §3.6). `len`/`foffset` are 64-bit on both targets.

**Metadata delivery (reviewer resolution 3): a versioned query once per span,
written into an ABI-specific struct in linear memory, then cached — NO metadata
host calls in the scanning loop.** During invocation SETUP (not the hot loop), the
SDK calls a **versioned** host-call query ONCE per named span; the host WRITES an
`HlSpanMeta` record into a caller-provided buffer in ordinary WASM linear memory
(a normal bounds-checked `wasm_runtime_addr_app_to_native` write, exactly like
today's `host_call` LOG/CALLBACK paths). The record is ABI-specific and versioned:

```c
/* v1 wire record the host writes into linear memory; wasm32 and wasm64 layouts
 * are declared separately (fixed field widths, explicit padding) so there is no
 * cross-ABI ambiguity. version + struct_size are checked by the SDK. */
typedef struct HlSpanMetaV1 {
    uint16_t version;    /* = 1 */
    uint16_t struct_size;/* sizeof(this) for the target ABI */
    uint32_t flags;      /* bit0 = read-only */
    uint64_t base;       /* WASM address (host validated to fit the ABI width) */
    uint64_t len;
    uint64_t foffset;
} HlSpanMetaV1;
```

The SDK reads the record once, checks `version`/`struct_size`, and caches
`{base,len,foffset,flags}` in a local `HullSpan`. Every subsequent `load_*` is a
pure inline check + native read against the cached `base` — **no host call per
byte/word/element**. The query is invocation-scoped (§3.7): it can only report the
spans attached to THIS invocation and never a stale or another invocation's entry.

### 3.4 SDK accessors are convenience; isolation lives in WAMR

The header is ergonomics only. A hostile module may ignore it and emit raw
loads/stores at arbitrary addresses. Isolation therefore holds ENTIRELY in WAMR's
registered-range checks: a raw load outside every attached region fails the
shared-heap + linear-memory bounds check (recoverable trap); a raw store into an
RO attached region is trapped by the §3.1 patch; a raw access into a region NOT
attached to this invocation is not in range and traps. The design's security
claims are stated against raw instruction behavior, never against the header.

### 3.5 Attach/detach lifecycle — rollback, traps, reentrancy, async, concurrency, staleness

- **Detach cannot race active execution, async completion, a trap, or teardown
  (reviewer verification).** The invariant: detach of an instance's spans runs
  ONLY on the thread that owns the instance for that call, and ONLY after the WASM
  call has returned (normally, by trap, or by gas exhaustion) — never concurrently
  with the instance executing. For sync calls the owning thread is the event loop;
  for async calls it is the worker thread that ran the job, and detach is part of
  the job's completion (the same code path for success and trap), which happens-
  before the completion is signalled back to the event loop. Instance teardown
  (pool release / deinstantiate) is ordered AFTER detach on that same thread. WAMR's
  per-heap `attached_count` + Hull's `mod->mutex` around attach/detach/snapshot make
  the transitions atomic w.r.t. a concurrent invocation on another pooled instance.
  So there is no window in which a heap is detached while an instruction is mid-
  translate against it, and no instance is torn down with spans still attached.
- **Attach & rollback.** Before the call, attach each span's shared heap to the
  instance under `mod->mutex`. If attaching span *k* fails, detach spans `0..k-1`
  and return an error; the call does not run.
- **Detach on ALL exit paths.** Wrap the call so success, WASM trap, gas
  exhaustion, and internal error ALL run the same detach-all cleanup (mirrors the
  existing exception-clear + pool-release discipline in `cap/wasm.c`). A trapped
  call detaches before the instance is returned to the pool or destroyed.
- **Async pinning.** For `compute.async.call`, each span buffer's `borrow_count` is
  incremented for the whole async duration (the existing borrow-pin), so a
  `buffer:close()` mid-flight defers `munmap` until the worker completes; detach +
  `release` run on completion (success or trap). The module's `inflight_async`
  gate already blocks conflicting segment mutation; spans reuse it.
- **Concurrency.** Two concurrent invocations use two pooled instances; the SAME
  RO buffer may be attached to both (RO sharing is safe). Attach/detach and the
  shared-data snapshot are taken under `mod->mutex`, as segments are today.
- **Reentrancy.** A `host_call` callback that re-enters `compute.call` must target
  a DIFFERENT instance (the current one is mid-call); a reentrant call with spans on
  the same instance is rejected. Persistent instances already carry a `busy`
  atomic; spans respect it.
- **Stale-span invalidation.** Because the buffer is borrow-pinned for the call and
  detach precedes release, a span can never reference an unmapped region during
  execution. After detach, the WASM address range is no longer attached, so any
  retained address from a prior call is out-of-range on the next call -> trap
  (no use-after-unmap).

### 3.6 Window size bounded independently of file size; page-alignment near UINT64_MAX

- **Cap (reviewer resolution 2): the SAME conservative default on wasm32 AND
  Memory64 — `HL_WASM_MAX_SPAN_BYTES_TOTAL = 1 GiB` of attached span bytes PER
  INVOCATION, configurable DOWNWARD only.** Memory64 preserves 64-bit addressing
  and 64-bit `len`/`foffset`, but cut 1 does NOT authorize enormous mappings on
  either target; a larger cap requires separate RSS, address-space, latency, and
  platform evidence and is out of scope here. The cap is independent of file size:
  `fs.mmap({offset,length})` rejects `length` that would exceed it.
- **The cap is a TOTAL across ALL spans of an invocation, not per span, and the sum
  is overflow-safe.** Accumulate attached bytes with a checked add
  (`if (total > CAP - this_len) reject;`, never `total + this_len > CAP`), rejecting
  the whole invocation if the running total would exceed 1 GiB or the addition would
  wrap. A single 900 MiB span plus a 200 MiB span fails closed.
- **Alignment arithmetic is overflow-checked.** Align the file offset DOWN to a page
  boundary and the mapping length UP with checked math: reject if
  `offset > file_size`, if `length > cap`, if the page-up of `length` overflows, or
  if `offset + length` wraps. Tests exercise offsets/lengths near `UINT64_MAX`, at
  exact page multiples, and the page-up-overflow boundary. The window's WASM `base`
  is validated to fit the module's address width before attach (§3.3).

### 3.7 Multiple spans, overlap, and the cross-span access model

- Multiple named spans per invocation ARE allowed (the `spans={...}` list). Each is
  a separate mmap region at a DISTINCT high WASM-address range (the chain places
  them non-overlapping, high-address-downward, as `hl_wasm_rebuild_chain` does
  today). **Both** overlap surfaces **fail closed**: (a) overlapping WASM-visible
  ranges are rejected before attach (the chain computes non-overlapping ranges;
  the host asserts the computed ranges are strictly disjoint and aborts the
  invocation if not), and (b) duplicate / overlapping NATIVE mappings — the same
  `HlMappedBuffer` passed twice, or two windows over the same file whose byte ranges
  overlap — are rejected at setup (dedupe by backing identity + range). The total
  is bounded by the overflow-safe §3.6 accounting.
- **Intended model:** every span attached to an invocation IS an input to that
  invocation, so a module reaching from span A's `base` into span B's range is NOT
  a violation — both are its own inputs; it is only defense-in-depth that the SDK
  keeps them separate. What the module must NOT reach is any range NOT attached to
  this invocation (another call's spans, linear memory of another instance, Hull
  memory) — those are out-of-range and trap. Tests assert BOTH: (a) deliberate
  cross-span raw access among the invocation's own spans succeeds (allowed), and
  (b) any access outside all attached ranges traps.
- **Metadata query is invocation-scoped and cannot leak (reviewer verification).**
  The versioned query (§3.3) resolves names ONLY against the spans attached to the
  CURRENT invocation (the `tl_host_ctx`-style snapshot taken under `mod->mutex`,
  exactly as segment queries are today). After detach the snapshot is cleared, so a
  post-detach query returns "no such span" and can never report a stale entry or
  another invocation's spans. Tests: query an unknown name (rejected); query after
  detach (rejected); two concurrent invocations each see only their own spans.

### 3.8 Benchmark: cold first-touch faults measured separately from warm scans

Demand-paging latency must not be misattributed to compute overhead. The harness
reports COLD (first touch of a freshly-mapped window; page faults dominate) and
WARM (pages resident; steady-state throughput) as SEPARATE numbers, for each of the
four baselines (fread-into-linear-mem / chunked native->WASM memcpy / HullSpan /
native mmap) and each workload (sequential byte scan, sequential u32/u64 scan,
random reads, a PBF-like binary parser). The performance target
(<= 10-15% over native mmap, AOT) is judged on the WARM scan; the COLD number is
reported for demand-paging behavior, not gated. A separate RSS test maps a large
sparse/real file, touches a small region, and asserts RSS grows ~ with touched
pages, not logical file size (documented, not a hard CI number).

## 4. The WAMR patch (isolated, enumerated, upstream-tested)

Only §3.1 touches WAMR, at the THREE enumerated insertion sites (interp
`CHECK_SHARED_HEAP_OVERFLOW`; AOT codegen `build_get_maddr_in_cache_shared_heap`
for scalar/SIMD/copy/fill; AOT `wasm_runtime_validate_app_addr` for memory.init)
plus the `WASMSharedHeap.read_only` + `SharedHeapInitArgs` field. Recorded in a
dedicated `docs/wamr_patches.md` with exact upstream files/functions so a WAMR
upgrade re-applies cleanly. The patch is covered by **upstream-style WAMR unit
tests** in `vendor/wamr/tests/unit/shared-heap/shared_heap_test.cc` (a read-only
heap: store / `v128.store` / `memory.copy` dst / `memory.fill` / AOT `memory.init`
each fault; a load succeeds) — not only Hull e2e — run under both interp and AOT
in CI. Regression guard: the existing writable shared-heap unit tests (`_rmw*`,
bulk-into-heap, memory64) must stay green, proving default-writable is preserved.

## 5. Test matrix (implementation PR)

Lifecycle: map/unmap, zero-length, page-aligned + unaligned offsets, logical
offsets > 4 GiB, wasm32 windowing, Memory64 window at the 1 GiB cap. Bounds:
read at end, read one past end (trap), integer overflow in offset math, stale span
(prior-call address), invalid handle, unmapped buffer, cross-region access outside
attached ranges (trap), zero-length mapping. **Permission — EVERY enumerated write
path into an RO span faults (recoverable trap, host survives), under BOTH interp
and AOT:** scalar `i32/i64/f32/f64.store` (+ store8/16/32), `v128.store` and
`store*_lane`, `memory.copy` with RO destination, `memory.fill` on an RO span,
`memory.init` into an RO span (the distinct AOT route), and a `memory.copy` whose
range crosses a heap boundary (trap); a LOAD from the same RO span succeeds.
Accounting: total attached bytes > 1 GiB rejected; the sum-overflow boundary
rejected; per-span-vs-total enforced. Isolation: two instances, same buffer shared
to two instances (allowed), unrelated instances cannot reach each other's spans,
multiple spans + overlapping-WASM-range rejection + duplicate/overlapping-native-
mapping rejection + intended cross-span access. Metadata: unknown-name query
rejected, post-detach query rejected, concurrent invocations see only their own
spans, version/struct_size mismatch rejected. Ordering: destruction ordering,
detach-on-trap, attach rollback, async pinning across `buffer:close()`, reentrancy
rejection, detach-does-not-race-execution. Execution: AOT and interpreter.
Sanitizers: ASan/UBSan. Fuzz: span offset/length arithmetic, window/page-alignment
operations, and the total-span accounting sum. Coherence: host writes then WASM
reads see the same bytes. WAMR unit tests (upstream-style) for the RO-heap write
paths, plus the existing writable-heap `_rmw*`/bulk/memory64 cases staying green.

## 6. Non-goals (unchanged)

No WGPU/CUDA/Vulkan/Metal import, no persistent mmap-backed WASM heaps, no WASI
mmap, no auto-mmap of every blob, no writable/shared mappings in cut 1 (structure
allows adding later — the §3.1 permission path becomes the enable point), no
generic shared-memory IPC. **The core feature is GENERIC mapped spans; the PBF
parser is an OPTIONAL example compute plugin, not Hull core** (reviewer resolution
4) — it exercises the SDK in `examples/` and is not part of the feature's C surface
or the gating benchmark.

## 7. Resolved decisions (reviewer directives folded in)

The four prior open questions are now DECIDED and reflected above:

1. **WAMR patch (§3.1, §4):** explicit read/write permission in shared-heap
   metadata, access kind threaded through the centralized interp + AOT validation,
   with the COMPLETE write-path enumeration (scalar, SIMD, `memory.copy`/`fill`/
   `init`; atomics confirmed not shipped under `SHARED_MEMORY=0`) and the THREE
   insertion sites — including the distinct AOT `memory.init` route. Upstream-style
   WAMR unit tests gate it.
2. **Window cap (§3.6):** same conservative default on wasm32 and Memory64 — 1 GiB
   TOTAL attached span bytes per invocation, configurable downward, overflow-safe
   across all spans. Larger caps deferred pending RSS/address-space/latency/platform
   evidence.
3. **Metadata (§3.3):** versioned host-call query once per span at setup, written
   into an ABI-specific `HlSpanMeta` struct in linear memory, cached by the SDK; no
   metadata host calls in the scanning loop.
4. **PBF parser (§6):** optional example plugin only; the core is generic spans.

No open questions remain for the design; the residual risk is implementation-time
proof that the §3.1 permission patch closes EVERY enumerated write path (the
gating deliverable).

## Related

- `cap/wasm_data.c`, `cap/wasm.c`, `cap/fs.c`, `include/hull/cap/{wasm,fs}.h`,
  `vendor/wamr` 2.4.1, `mk/vendor/wamr.mk`.
- `docs/wamr_architecture.md` (the WASM compute design).
