# Zero-copy host-backed mapped spans for WASM compute — design (DESIGN ONLY)

**Status:** DESIGN ONLY, for formal review. **Implementation is UNAUTHORIZED until
this review passes.** No code, SDK header, WAMR patch, or benchmark is written
yet; everything below is what the implementation PR *will* do, not evidence
already collected.

This evolves Hull's EXISTING zero-copy mechanism (`compute.segment` + WAMR shared
heaps + `HlMappedBuffer`); it does NOT add a parallel buffer subsystem. It
incorporates the reviewer's cut-1 scope decisions and eight security revisions.

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

**Scoped patch (the gating deliverable).** Make a mapped region carry a
`read_only` flag and have the store path consult it:

- Represent RO regions so WAMR knows the permission. Options, in preference order:
  (a) add a `bool read_only` to the WAMR `WASMSharedHeap` and set it after
  `create_shared_heap` (smallest struct change); or (b) a Hull-side region table
  keyed by the shared-heap range consulted by the patched store path. Prefer (a).
- **Interpreter (fast-interp) store opcodes** (`wasm_interp_fast.c` i32/i64/f32/f64
  `.store*`): after the shared-heap range resolves, if the target heap is
  `read_only`, set `EXCE_` (out-of-bounds / a new "store to read-only mapped
  region") and return — a normal recoverable WASM trap. Loads unchanged.
- **AOT store lowering** (`aot_emit_memory.c`, the store side of
  `aot_check_shared_heap_memory_overflow`): when the resolved address is in a
  `read_only` shared-heap range, branch to the existing exception-raise block
  instead of the native store. Reads unchanged (still the fast cached-range load).

**Gating rule:** the implementation PR must FIRST prove, with a test, that a WASM
`i32.store` into an RO span yields a recoverable trap (the invocation fails, the
host survives, other invocations continue) under BOTH interpreter and AOT, before
any other part is built. If this patch proves too invasive, the fallback
(evaluate `OS_ENABLE_HW_BOUND_CHECK` + a signal handler that converts the region
fault to a trap) is assessed — but that is a larger surface and interacts with
Hull's no-signal/W^X posture, so the flag-check patch is the primary plan. The
patch stays in one WAMR area, documented in `docs/` with exact upstream
files/functions so future WAMR upgrades are manageable.

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
    uint64_t         len;    /* window length in bytes (<= HL_WASM_MAX_SPAN_BYTES) */
    uint64_t         foffset;/* 64-bit LOGICAL file offset of window[0] */
} HullSpan;
```

`hull_span_ptr(s, off)` returns `(const uint8_t *)(uintptr_t)(s->base + off)` where
`base` is already the correct width — no `uint64_t`->wasm32 truncation. The HOST
never writes this struct into linear memory (which would fix a cross-ABI layout);
instead the host exposes each named span's `(base, len, foffset)` via the host_call
query, and the WASM SDK constructs the struct locally with the right widths. The
host validates on wasm32 that `base + len - 1` fits in 32 bits before attaching
(else the window is rejected — see §3.6). `len`/`foffset` are 64-bit on both
targets.

### 3.4 SDK accessors are convenience; isolation lives in WAMR

The header is ergonomics only. A hostile module may ignore it and emit raw
loads/stores at arbitrary addresses. Isolation therefore holds ENTIRELY in WAMR's
registered-range checks: a raw load outside every attached region fails the
shared-heap + linear-memory bounds check (recoverable trap); a raw store into an
RO attached region is trapped by the §3.1 patch; a raw access into a region NOT
attached to this invocation is not in range and traps. The design's security
claims are stated against raw instruction behavior, never against the header.

### 3.5 Attach/detach lifecycle — rollback, traps, reentrancy, async, concurrency, staleness

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

- A window has an explicit max, `HL_WASM_MAX_SPAN_BYTES`, independent of the file
  size: wasm32 <= 3 GiB (the current shared-region ceiling), Memory64 a larger but
  still explicit cap (proposed 32 GiB, configurable) — never "whole file by
  default". `fs.mmap({offset,length})` rejects `length > cap`.
- **Alignment arithmetic is overflow-checked.** Align the file offset DOWN to a
  page boundary and the mapping length UP, using checked math: reject if
  `offset > file_size`, if `length > cap`, if `aligned_len` overflows, or if
  `offset + length` wraps. Tests exercise offsets/lengths near `UINT64_MAX` and at
  exact page multiples. The window's WASM `base` is validated to fit the module's
  address width before attach (§3.3).

### 3.7 Multiple spans, overlap, and the cross-span access model

- Multiple named spans per invocation ARE allowed (the `spans={...}` list). Each is
  a separate mmap region at a DISTINCT high WASM-address range (the chain places
  them non-overlapping, high-address-downward, as `hl_wasm_rebuild_chain` does
  today). The host **rejects** any configuration whose ranges would overlap or
  whose total exceeds the region budget.
- **Intended model:** every span attached to an invocation IS an input to that
  invocation, so a module reaching from span A's `base` into span B's range is NOT
  a violation — both are its own inputs; it is only defense-in-depth that the SDK
  keeps them separate. What the module must NOT reach is any range NOT attached to
  this invocation (another call's spans, linear memory of another instance, Hull
  memory) — those are out-of-range and trap. Tests assert BOTH: (a) deliberate
  cross-span raw access among the invocation's own spans succeeds (allowed), and
  (b) any access outside all attached ranges traps.

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

## 4. The WAMR patch (isolated + documented)

Only §3.1 touches WAMR. Exact surface to be recorded in a dedicated
`docs/wamr_patches.md`: the `WASMSharedHeap` `read_only` field (or the region
table), the fast-interp store-opcode checks, and the AOT store-lowering branch in
`aot_emit_memory.c`. The patch is guarded so a WAMR upgrade re-applies cleanly; CI
builds both interp and AOT and runs the RO-store-trap test on each.

## 5. Test matrix (implementation PR)

Lifecycle: map/unmap, zero-length, page-aligned + unaligned offsets, logical
offsets > 4 GiB, wasm32 windowing, Memory64 larger window. Bounds/permission:
read at end, read one past end (trap), integer overflow in offset math, stale span
(prior-call address), invalid handle, unmapped buffer, **store through RO span
(trap, host survives)**, cross-region access outside attached ranges (trap),
zero-length mapping. Isolation: two instances, same buffer shared to two instances
(allowed), unrelated instances cannot reach each other's spans, multiple spans +
overlap rejection + intended cross-span access. Ordering: destruction ordering,
detach-on-trap, attach rollback, async pinning across `buffer:close()`, reentrancy
rejection. Execution: AOT and interpreter. Sanitizers: ASan/UBSan. Fuzz: span
offset/length arithmetic and window/page-alignment operations. Coherence: host
writes then WASM reads see the same bytes.

## 6. Non-goals (unchanged)

No WGPU/CUDA/Vulkan/Metal import, no persistent mmap-backed WASM heaps, no WASI
mmap, no auto-mmap of every blob, no writable/shared mappings in cut 1 (structure
allows adding later — the §3.1 store path becomes the enable point), no generic
shared-memory IPC.

## 7. Open questions for the reviewer

1. **WAMR patch shape (§3.1):** approve the `WASMSharedHeap.read_only` +
   interp/AOT store-check patch as the primary plan, with HW-bound-check+signal as
   the documented fallback only if the flag-check proves infeasible?
2. **Memory64 window cap (§3.6):** 32 GiB default acceptable, or a different
   explicit ceiling?
3. **Span metadata delivery (§3.3):** host_call query (chosen, avoids cross-ABI
   struct) vs the host writing a `HullSpan` into linear memory — confirm the query
   approach.
4. Does the reviewer want the PBF-parser workload in the core benchmark, or kept as
   an optional example (the task lists it optional)?

## Related

- `cap/wasm_data.c`, `cap/wasm.c`, `cap/fs.c`, `include/hull/cap/{wasm,fs}.h`,
  `vendor/wamr` 2.4.1, `mk/vendor/wamr.mk`.
- `docs/wamr_architecture.md` (the WASM compute design).
