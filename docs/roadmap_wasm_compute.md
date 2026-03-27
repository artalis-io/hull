# WASM Compute Roadmap — Numeric Heavy Workloads

Status: **Draft** | Created: 2026-03-20

This roadmap tracks the work needed to make Hull's WASM compute layer a
capable platform for numeric-heavy workloads: linear algebra, signal
processing, ML inference, image/audio pipelines, large dataset transforms.

---

## Current State

Hull ships a compute-only WASM sandbox built on WAMR with:

- Interpreter + fast-interp + AOT (near-native via `wamrc`)
- Gas metering (instruction counting)
- Per-call instance isolation, no WASI, no ambient I/O
- `fs.mmap` zero-copy input path
- Async dispatch to thread pool (`compute.async.call`)
- Three-tier config: compile-time → manifest → per-call
- ~4 GB max heap (uint32_t ceiling), 256 MB max I/O, 100B max instructions
- Configurable compile-time ceilings (`make HL_WASM_MAX_HEAP_MB=N`)

**What's disabled:** Shared memory, threads.
**What's enabled:** SIMD128 (AOT-first; interpreter SIMD requires SIMDe, not yet vendored).

---

## Phase 1 — SIMD128 ✅

**Status:** Complete.

**Goal:** Enable 128-bit SIMD vector operations in WASM modules.

**Why:** SIMD128 gives 2-8× throughput for vectorizable numeric code. Rust/C++
compilers with `-msimd128` / `#[target_feature(enable = "simd128")]` emit
these automatically. In AOT mode, WAMR maps WASM SIMD to native SSE4.1
(x86_64) or NEON (aarch64) — near-native vector throughput.

### Tasks

- [x] **Enable SIMD in WAMR build flags**
  - `Makefile`: changed `-DWASM_ENABLE_SIMD=0` → `-DWASM_ENABLE_SIMD=1`
  - SIMD support in loader and AOT runtime is gated by `#if WASM_ENABLE_SIMD`
    inside existing compiled files — no source additions needed

- [x] **Verify AOT relocation support**
  - SIMD AOT on aarch64 verified: dot product + matmul SIMD modules compile
    via `wamrc --enable-simd`, load as AOT, produce byte-correct output
  - WAMR handles SIMD relocations transparently on aarch64 (NEON)
  - x86_64 (SSE4.1) not yet tested (requires x86_64 CI runner)

- [x] **Add SIMD benchmark workloads**
  - `simd_dot_product.c`: f32 dot product with `wasm_simd128.h` intrinsics
  - `simd_matmul.c`: f32 matrix multiply with 4-wide SIMD inner loop
  - Benchmark sizes: 1K-1M elements (dot), 16-256 matrices (matmul)
  - Results (aarch64 AOT):
    - Dot product 1M: SIMD AOT 4.5× native, scalar AOT 5.0× → **1.12× SIMD speedup**
    - Matmul 256×256: SIMD AOT **0.94× native** (faster than scalar C!), **1.11× SIMD speedup**
  - SIMD benefit is modest (~1.1×) because: dot product is memory-bound at
    large sizes; matmul SIMD has B-column gather overhead
  - Interpreter cannot load v128 modules (graceful HL_WASM_ERR_LOAD, no crash)

- [x] **wamrc SIMD output verified**
  - x86_64 Linux CI (scan-build + unit tests) compiles and runs SIMD modules
  - aarch64 macOS local: SIMD AOT modules produce correct output
  - `wamrc --enable-simd` is already part of the AOT build pipeline

- [x] **Cosmopolitan / cross-arch SIMD testing**
  - `e2e-compute` added to Cosmo CI job — validates compute.call on x86_64
    under cosmocc fat binary (APE)
  - Unit tests include SIMD interpreter fallback test (graceful error)
  - aarch64 AOT verified locally; x86_64 AOT verified via Linux CI

- [x] **Docs updated**
  - `docs/wamr_architecture.md`: SIMD128 section (compiler flags, AOT mapping,
    benchmark results), instance pooling, zero-copy buffers, updated size limits
  - `CLAUDE.md`: SIMD, pooling, memory limits notes in WASM section

### Risk

Low. WAMR's SIMD support is mature (used by browser engines). The main risk
is AOT relocation gaps on specific architectures — mitigated by interpreter
fallback.

---

## Phase 2 — Memory Limits & Configurable Ceiling ✅

**Status:** Complete.

**Goal:** Support large working sets (up to 4 GB) and make the compile-time
maximum configurable without editing source.

**Why:** Current max heap is 64 MB. A 4096×4096 f32 matrix is 64 MB alone.
ML model weights, image buffers, and large datasets need more room. The
compile-time ceiling should be tunable for different deployment contexts
(embedded vs server).

### Tasks

- [x] **Raise compile-time maximums in `limits.h`**
  - `HL_WASM_MAX_HEAP`: 64 MB → ~4 GB (uint32_t max)
  - `HL_WASM_MAX_STACK`: 1 MB → 8 MB
  - `HL_WASM_MAX_IO_SIZE`: 16 MB → 256 MB
  - `HL_WASM_MAX_GAS`: 1B → 100B instructions

- [ ] **Widen types for large limits** (deferred — WAMR API is uint32_t,
  type widening only needed for Memory64 in Phase 3)

- [x] **Make ceiling configurable via Makefile**
  - `make HL_WASM_MAX_HEAP_MB=512` / `HL_WASM_MAX_STACK_MB=4` / `HL_WASM_MAX_IO_MB=64`
  - Uses `-D` overrides with `#ifndef` guards in `limits.h`

- [x] **Guard `limits.h` with `#ifndef`**
  - All WASM max limits wrapped in `#ifndef` so Makefile `-D` overrides work

- [x] **Raise default I/O sizes**
  - `HL_WASM_DEFAULT_MAX_INPUT`: 64 KB → 1 MB
  - `HL_WASM_DEFAULT_MAX_OUTPUT`: 64 KB → 1 MB

- [x] **CLI flag parsing**
  - `--wasm-heap`, `--wasm-stack`, `--wasm-gas`, `--wasm-max-input`,
    `--wasm-max-output` all implemented with `hl_parse_size()` (k/m/g suffixes)
  - Three-tier resolution: CLI > manifest > compile-time defaults
  - Documented in `hull -h` help text

- [x] **Test with large allocations**
  - 256 MB heap instantiation: succeeds (33ms)
  - 32 MB echo I/O (128 MB heap): byte-exact verified (63ms)
  - 128 MB echo I/O (512 MB heap): byte-exact verified (254ms)
  - 512 MB I/O request: silently clamped to 256 MB, succeeds
  - UINT32_MAX heap request: graceful failure (no crash/truncation)
  - Full uint32_t clamping chain verified: no truncation bugs found

### Risk

Medium. The type widening touches multiple layers (limits.h → manifest →
main.c → bindings → wasm.c). Needs careful review of all uint32_t → uint64_t
transitions. WAMR itself uses `uint32_t` for memory size in 32-bit mode —
Memory64 (Phase 3) is required for >4 GB.

---

## Phase 2.5 — Instance Pooling

**Goal:** Eliminate per-call WASM instantiation overhead by reusing instances.

**Status:** Complete.

Every `compute.call` previously created a fresh WASM instance (~2.5ms allocation
overhead), used it, then destroyed it. Instance pooling reuses instances between
calls, amortizing instantiation cost to near-zero for repeated calls.

### Design

- Per-module pool of `(instance, exec_env, process_fn)` tuples, keyed by `(heap_size, stack_size)`
- Pool max size: `HL_WASM_POOL_MAX` (default 8) per module
- Instances with heap > `HL_WASM_POOL_HEAP_THRESHOLD` (4 MB) are never pooled
- Failed calls (gas exhaustion, call failure) destroy the instance — never pooled
- Single `pthread_mutex_t` guards all pool operations; WASM execution outside lock
- No API changes — `compute.call` and `compute.async.call` work identically
- Linear memory contents are not reset (safe because compute plugins are pure functions)

### Files Changed

- `include/hull/limits.h`: Pool constants (`HL_WASM_POOL_MAX`, `HL_WASM_POOL_HEAP_THRESHOLD`)
- `include/hull/cap/wasm.h`: `HlWasmPoolEntry`, `HlWasmPool` types; pool in `HlWasmModule`; mutex in `HlWasmCache`
- `src/hull/cap/wasm.c`: `pool_acquire()`, `pool_release()`, `pool_drain()` statics; modified init/destroy/call
- `include/hull/worker_wasm.h`: `wasm_cache` field in `HlWorkerWasmOp`
- `src/hull/worker_wasm.c`: Pool acquire/release in async worker path
- `src/hull/runtime/{lua,js}/modules.c`: Wire `wasm_cache` pointer to async ops

---

## Phase 2.6 — HlWasmBuffer (Zero-Copy Output)

**Goal:** Eliminate redundant memcpy round-trips between WASM linear memory,
host buffers, and Lua/JS string allocations. Enable zero-copy chaining of
compute calls.

**Status:** Complete.

**Design:** `HlWasmBuffer` is a managed buffer handle backed by one of three
kinds: OWNED (malloc'd bytes), MMAP (kernel mapping via `fs.mmap`), or WASM
(pointer into pooled WASM instance linear memory). For WASM-backed buffers,
the pooled instance stays checked out — `pool_release` is deferred to
`hl_wasm_buffer_destroy()`. Non-poolable instances (heap > 4MB threshold or
pool full) eagerly copy to OWNED so the instance can be destroyed.

**API:** Opt-in via `{ buffer = true }` (Lua) / `{ buffer: true }` (JS) in
`compute.call` and `compute.async.call`. Returns a WasmBuffer userdata/object
with `buf:bytes()` / `buf.bytes()` to materialize to string/ArrayBuffer, and
`buf:close()` / `buf.close()` for explicit release (GC handles it otherwise).
`compute.buffer(str)` creates an OWNED buffer from a string for use as input.

**Files changed:**
- `include/hull/cap/wasm_buffer.h`: `HlWasmBuffer` struct, `HlWasmBufKind` enum, C API
- `src/hull/cap/wasm_buffer.c`: All `hl_wasm_buffer_*` implementations
- `include/hull/cap/wasm.h`: `hl_cap_wasm_call_buf()`, `hl_wasm_pool_release()` exports
- `src/hull/cap/wasm.c`: `hl_wasm_pool_release()` (renamed from static), `hl_cap_wasm_call_buf()`
- `include/hull/worker_wasm.h`: `want_buffer`, `output_buf` fields
- `src/hull/worker_wasm.c`: Buffer-mode path in async worker
- `src/hull/runtime/{lua,js}/modules.c`: WasmBuffer metatable/class, input detection, buffer option
- `tests/hull/cap/test_wasm_buffer.c`: 12 unit tests
- `tests/e2e_compute.sh`: Buffer-mode E2E tests for Lua and JS

---

## Phase 3 — Memory64 ✅

**Status:** Complete.

**Goal:** Enable 64-bit WASM memory addressing, allowing >4 GB linear memory.

### Delivered

- [x] **Memory64 enabled in WAMR** — `Makefile`: `-DWASM_ENABLE_MEMORY64=1`
- [x] **ABI auto-detection** — Option B implemented: `is_memory64` flag detected at module load time via WAMR internal API, stored in module cache
- [x] **Dual ABI dispatch** — `hull_process` called with `(i64, i64, i64, i64) → i32` for Memory64 modules, `(i32, i32, i32, i32) → i32` for WASM32
- [x] **host_call updated** — Memory64 modules use `host_call(i32, i64, i64) → i32` with widened pointer/length parameters
- [x] **AOT support** — `hull build` passes `--enable-memory64` to `wamrc` when Memory64 module detected
- [x] **Backward compatibility** — existing 32-bit modules work unchanged; ABI flag selects correct calling convention

---

## Phase 4 — GPU Compute via WebGPU ✅

**Status:** Complete.

**Goal:** Optional GPU acceleration for massively parallel numeric workloads
(matrix ops, convolutions, reductions) through a capability-mediated WebGPU
backend.

**Why:** GPU compute delivers 10-100× throughput for data-parallel workloads.
Hull's vtable pattern allows pluggable backends without coupling the core to
any GPU library. WebGPU's standardized C API (`webgpu.h`) makes this feasible
as an optional, vendor-neutral capability.

### C-Native WebGPU Implementations

| Library | Language | C API | Headless Compute | Notes |
|---------|----------|-------|------------------|-------|
| **wgpu-native** | Rust → C FFI | `webgpu.h` + `wgpu.h` | Yes | Best fit for C projects. Pure C API, opaque pointers. Pre-built static libs available. |
| **Dawn** | C++ | `webgpu.h` | Yes | Google/Chromium's impl. More mature but C++ dependency. |

**Recommendation: wgpu-native.** Pure C API surface, no C++ in the build
chain, supports headless compute-only operation, actively maintained.

### Architecture: Vtable-Based GPU Backend

Hull already uses vtables for runtime dispatch (`HlRuntimeVtable`) and async
continuations (`HlAsyncCont`). GPU compute follows the same pattern — a
`HlGpuVtable` that abstracts the GPU backend, with wgpu-native as the first
(and possibly only) implementation.

```
Application (Lua/JS)
    │
    │  gpu.dispatch("shader.wgsl", input, opts)
    │
Capability Layer (cap/gpu.c)
    │
    │  HlGpuVtable → init / create_pipeline / dispatch / read_back / destroy
    │
GPU Backend (cap/gpu_wgpu.c)          ← or gpu_stub.c (no-op when disabled)
    │
    │  wgpuDeviceCreateComputePipeline()
    │  wgpuQueueWriteBuffer()
    │  wgpuComputePassSetPipeline()
    │  wgpuQueueSubmit()
    │  wgpuBufferMapAsync() → read back
    │
wgpu-native (libwgpu_native.a)
    │
    │  Vulkan / Metal / DX12
    │
GPU Hardware
```

### Design Principles

1. **Optional dependency.** GPU support is off by default (`HL_ENABLE_GPU=0`).
   When disabled, `gpu_stub.c` provides a vtable that returns
   `HL_GPU_ERR_NOT_AVAILABLE`. Zero binary size impact when off.

2. **Capability-mediated.** `gpu.dispatch()` goes through `hl_cap_gpu_*()` —
   same pattern as `hl_cap_db_*()`, `hl_cap_fs_*()`. Audit logging,
   manifest gating, all apply.

3. **Shader as data.** WGSL compute shaders live in `app_dir/shaders/` and
   are embedded via VFS, same as templates and WASM modules. No runtime
   shader compilation from user strings.

4. **Synchronous-looking, async underneath.** GPU dispatch is inherently async
   (submit → fence → readback). The Lua/JS API yields to the event loop
   during GPU work, same as `compute.async.call()` and `db.async.query()`.

5. **No WASM↔GPU bridge.** GPU compute is a separate capability, not an
   extension of WASM compute. WASM plugins remain CPU-only with isolated
   linear memory. Orchestration code (Lua/JS) decides whether to use WASM
   or GPU for a given workload.

### Proposed API

```lua
-- Lua
local result = gpu.dispatch("matmul.wgsl", {
    buffers = {
        { data = matrix_a, usage = "read" },
        { data = matrix_b, usage = "read" },
        { size = output_size, usage = "write" },
    },
    workgroups = { 16, 16, 1 },
    uniforms = { rows = 1024, cols = 1024, k = 1024 },
})
-- result = output buffer bytes
```

```javascript
// JavaScript
const result = await gpu.dispatch("matmul.wgsl", {
    buffers: [
        { data: matrixA, usage: "read" },
        { data: matrixB, usage: "read" },
        { size: outputSize, usage: "write" },
    ],
    workgroups: [16, 16, 1],
    uniforms: { rows: 1024, cols: 1024, k: 1024 },
});
```

### Vtable Definition

```c
typedef struct HlGpuVtable {
    int   (*init)(void **ctx);
    void  (*destroy)(void *ctx);
    int   (*create_pipeline)(void *ctx, const char *wgsl, size_t wgsl_len,
                             HlGpuPipeline **out);
    void  (*destroy_pipeline)(void *ctx, HlGpuPipeline *pipeline);
    int   (*dispatch)(void *ctx, HlGpuPipeline *pipeline,
                      const HlGpuDispatchOpts *opts,
                      void *output, size_t *output_len);
    const char *name;   /* "wgpu", "stub", etc. */
} HlGpuVtable;
```

### Delivered

- [x] **Vendor wgpu-native** — Pre-built v27 static libraries in `vendor/wgpu/`
- [x] **Backend vtable** — `HlGpuBackend` in `gpu.h`, `gpu_wgpu.c` implementation
- [x] **Stub backend** — `gpu.c` returns `HL_GPU_ERR_NOT_AVAILABLE` when `HL_ENABLE_GPU=0`
- [x] **wgpu-native backend** — Full implementation: device enumeration, WGSL compilation with caching, sync dispatch with timeout, staging buffer readback, fire-and-forget mode
- [x] **Multi-stage pipelines** — `gpu.pipeline()` with named buffer sharing across stages, single command buffer submission
- [x] **Persistent GPU buffers** — `gpu.buffer(name, data)`, `gpu.buffer_read()`, `gpu.buffer_copy()` (GPU-side copy)
- [x] **Async dispatch** — `gpu.async.dispatch()`, `gpu.async.pipeline()` via thread pool
- [x] **Shader loading from VFS** — `gpu.load(name)` reads `shaders/<name>.wgsl`, embedded in built binaries
- [x] **Lua/JS bindings** — Full parity: dispatch, pipeline, buffer, async, fire-and-forget
- [x] **Manifest integration** — `gpu = true` or `gpu = { devices = {0} }` with per-device restriction
- [x] **Sandbox** — macOS: iokit-open + MTLCompilerService. Linux: per-device `/dev/dri/renderDN` unveil
- [x] **Per-dispatch timeout** — `{ timeout = 1000 }` in dispatch/pipeline opts (min 10ms, default 5s)
- [x] **Unified buffer protocol** — MappedBuffer and WasmBuffer accepted as GPU input (zero-copy)
- [x] **Build integration** — `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu`, `make fetch-wgpu` for download
- [x] **Testing** — 13 unit tests (real GPU tests skip if no adapter), GPU benchmark, macOS Metal CI job
- [x] **Error codes** — `HlGpuError` enum with 9 specific codes + `err_msg` out-param

### Notes

GPU compute shipped before Memory64 (Phase 3). The two are independent —
GPU operates on its own buffers, not WASM linear memory.

---

## Phase 5a — Persistent WASM Instances ✅

**Status:** Complete.

**Goal:** Long-lived WASM instances that retain state across calls.

**Why:** Stateful workloads (ML model weights, pre-built indexes, incremental
statistics) pay a high cost re-instantiating per call. A persistent instance
amortizes setup over many invocations.

### API

**Lua:**
```lua
local inst, err = compute.instance("model", {
    heap = 256 * 1024 * 1024,   -- immutable after creation
    stack = 64 * 1024,           -- immutable after creation
    gas = 100000000,             -- default gas per call (overridable)
})

local output, err = inst:call(input)
local output, err = inst:call(input, { gas = 50000000 })
local buf, err = inst:call(input, { buffer = true })
local r = inst:async_call(input)  -- yields to event loop

inst:name()    -- "model"
inst:closed()  -- false
inst:close()   -- explicit cleanup (or GC)
```

**JavaScript:**
```javascript
const inst = compute.instance("model", {
    heap: 256 * 1024 * 1024,
    gas: 100000000,
});

const output = inst.call(input);
const buf = inst.call(input, { buffer: true });
const output2 = await inst.async.call(input);

inst.name;    // "model" (getter)
inst.closed;  // false (getter)
inst.close();
```

### Key properties

- Instance NOT pooled — exclusively owned by userdata until close/GC
- Linear memory preserved across calls (globals, heap, data segments)
- Gas reset per call
- `heap`/`stack` immutable; `gas`/`max_input`/`max_output`/`buffer` overridable per call
- Exceptions cleared at start of each call (reusable after gas exhaustion)
- Supports sync and async dispatch
- Instance has `busy` flag — async dispatch while in-flight returns error

### Tradeoffs

- Breaks the "fresh instance per call" isolation model — module is responsible
  for its own state management
- Memory isn't reset between calls
- Single-threaded: one instance can only be used by one caller at a time

---

## Phase 5b — Shared WASM Data via WAMR Shared Heaps ✅

**Goal:** Load named data segments that multiple WASM instances read concurrently
at native speed via WAMR's shared heap feature. Enables multi-GB read-only
datasets (spatial indexes, CSR graphs) for tile servers and routing engines.

**Implementation:** `compute.segment(module, segment, data)` API loads named segments
backed by page-aligned mmap regions. WAMR shared heaps chain segments into
contiguous high-address WASM32 space. WASM plugins query segments via
`host_call(0x02, segment_id, sub)` and read at native speed via normal `i32.load`.

### Key properties

- **Per-module, multi-segment**: up to 16 named segments per module
- **Auto-attach**: every `compute.call()` / `compute.instance()` gets all segments
- **Concurrent reads**: multiple thread pool workers read the same backing memory
- **Zero-copy mmap**: `fs.mmap()` + `compute.segment()` avoids copying multi-GB files
- **Named segments**: plugin queries by index via `host_call(0x02, segment_id, 0/1)`
- **Replaceable**: adding/removing segments drains pool and rebuilds chain
- **WASM32 limit**: ~3 GB total across all segments

### Files changed

- `Makefile`: `WASM_ENABLE_SHARED_HEAP=1`, `shared_heap_wrapper.c`, platform-specific `invokeNative`
- `include/hull/cap/wasm.h`: `HlWasmDataSegment`, `HlWasmSharedData`, `hl_cap_wasm_data_load/unload()`
- `include/hull/limits.h`: `HL_WASM_MAX_SHARED_DATA`, `HL_WASM_MAX_DATA_SEGMENTS`
- `src/hull/cap/wasm.c`: shared heap creation, chaining, attach, host_call opcode 0x02
- `src/hull/runtime/lua/modules.c`: `compute.segment()` Lua binding
- `src/hull/runtime/js/modules.c`: `compute.segment()` JS binding
- 8 unit tests + 2 E2E tests (Lua + JS)

---

## Dependency Graph

```
Phase 1 (SIMD128)  ──────────────────────┐
                                          ├──→  Phase 3 (Memory64)
Phase 2 (Memory Limits)  ────────────────┘          │
                                                     │
                                          Phase 5a (Persistent) ✅ ──→ Phase 5b (Shared Data) ✅

Phase 4 (GPU/WebGPU)  ── independent, start after Phase 3
```

Phases 1 and 2 can proceed in parallel. Phase 3 depends on the type
widening done in Phase 2. Phase 4 is independent. Phase 5a is complete;
Phase 5b is complete.

---

## Phase 6 — Developer Tooling (Adoption Blocker)

**Goal:** Lower the barrier to creating WASM compute modules from "expert C/Rust
developer who knows the ABI" to "any developer with a C compiler."

**Why:** The runtime is excellent but getting code into it is painful. Users must
hand-write C, manually invoke clang with the right flags, know the `hull_process`
ABI, and have no standard library. This is the #1 adoption barrier.

### Tasks

- [ ] **`hull compute new <name> --lang=c|rust` scaffolding command**
  - Generates a working module skeleton: source file, Makefile/build.sh,
    correct ABI exports (`hull_process`, `hull_version`), and a test fixture
  - C template: clang invocation with `--target=wasm32 -nostdlib -O2`
  - Rust template: `wasm32-unknown-unknown` target, `#[no_mangle]` exports
  - Output: `compute/<name>/` directory with source + build script

- [ ] **`hull compute test <name>` module testing**
  - Run a WASM module with sample inputs and validate outputs
  - No HTTP app needed — standalone module testing
  - Input/output from files or inline hex/base64
  - Reports: pass/fail, gas used, execution time, output bytes

- [ ] **C standard library shim (`compute/hull_libc.h`)**
  - Minimal libc subset: `memcpy`, `memset`, `strlen`, `memcmp`
  - Simple bump allocator for `malloc`/`free` using WASM linear memory
  - Single-header, include in any module

---

## Phase 7 — Sample Compute Modules

**Goal:** Provide reference implementations for common compute patterns so users
don't start from zero.

**Why:** Only `echo.wasm` exists as a fixture. Developers need working examples
of real workloads to learn the ABI, understand performance characteristics, and
copy-paste as starting points.

### Modules (in `stdlib/compute/` or `examples/compute/modules/`)

| Module | Description | SIMD? |
|--------|-------------|-------|
| `vector_ops` | Dot product, cosine similarity, L2 distance (f32 arrays) | Yes |
| `sort` | In-place quicksort of typed arrays (int32, float32) | No |
| `hash` | SHA-256, FNV-1a, xxHash of input bytes | No |
| `json_transform` | Parse JSON, extract/rename fields, emit JSON | No |
| `scoring` | Feature weighting, normalization, softmax | Yes |
| `text` | Tokenization, lowercase, fuzzy match (Levenshtein) | No |

Each module ships with:
- C source + optional Rust source
- Pre-compiled `.wasm` (interpreter) + `.aot.x86_64` + `.aot.aarch64`
- Test fixture (input bytes → expected output bytes)
- README with usage example

---

## Phase 8 — Compute Result Caching

**Goal:** Memoize `compute.call()` results to avoid redundant re-execution.

**Why:** Repeated calls on identical input re-execute from scratch. For
deterministic compute modules (which all should be — they're pure functions),
caching is safe and often a 100× speedup for repeated queries.

### Design

- **Key:** `SHA-256(module_name + input_bytes + opts_hash)`
- **Storage:** In-memory LRU cache, configurable max size (default 64 MB)
- **API:** `compute.call(name, input, { cache = true, cache_ttl = 60 })`
- **Invalidation:** On module reload, segment change, TTL expiry, or explicit
  `compute.cache_clear(name)`
- **Scope:** Per-process (shared across requests), thread-safe

### Tasks

- [ ] **LRU cache implementation** (`src/hull/cap/wasm_cache.c`)
- [ ] **Cache key computation** (SHA-256 of module + input + serialized opts)
- [ ] **Lua/JS binding** (`cache` and `cache_ttl` in call opts)
- [ ] **Invalidation hooks** in module reload and segment change paths
- [ ] **Cache stats** via `compute.cache_stats()` → `{ hits, misses, size, entries }`

---

## Phase 9 — SQLite UDF Integration (Differentiator)

**Goal:** Register WASM modules as SQL functions callable from queries.

**Why:** This is the killer feature. `SELECT *, hull_score(embedding) FROM items
ORDER BY hull_score(embedding) DESC` where `hull_score` runs a WASM module per
row. Brings compute to the data instead of data to the compute.

### Design

```lua
-- Register a WASM module as a SQL function
compute.register_udf("score", "score_module")

-- Use in queries — executed per-row by SQLite
local results = db.query(
    "SELECT *, hull_score(embedding) AS score FROM items ORDER BY score DESC"
)
```

- WASM function receives column value as input bytes, returns scalar or bytes
- Executed per-row via `sqlite3_create_function_v2`
- Instance pooling reused (one pool per UDF module)
- Gas metering per-row (configurable, prevents runaway UDFs)
- Result type: `SQLITE_INTEGER`, `SQLITE_FLOAT`, `SQLITE_TEXT`, or `SQLITE_BLOB`
  based on WASM output format

### Tasks

- [ ] **`compute.register_udf(sql_name, module_name, opts)` API**
- [ ] **SQLite function callback** that dispatches to pooled WASM instance
- [ ] **Type marshaling** (SQLite value → WASM input bytes → SQLite result)
- [ ] **Per-row gas limit** (`opts.gas_per_row`, default 100K instructions)
- [ ] **Namespace protection** (UDF names prefixed with `hull_` to avoid collisions)
- [ ] **Deterministic flag** for SQLite optimizer (pure WASM functions are deterministic)

---

## Phase 10 — GPU Texture/Image Support

**Goal:** Add 2D texture binding to `gpu.dispatch` for image processing workloads.

**Why:** Currently GPU only supports 1D storage buffers. Image processing (resize,
blur, convolution, compositing, heatmaps) requires 2D texture sampling with
hardware-accelerated filtering and boundary handling.

### Design

```lua
gpu.dispatch("blur", {
    textures = {
        { data = image_bytes, width = 1920, height = 1080, format = "rgba8", usage = "read" },
    },
    buffers = {
        { size = 1920 * 1080 * 4, usage = "write" },
    },
    workgroups = { x = 120, y = 68 },
})
```

- WGSL shader accesses via `texture_2d<f32>` + `textureSample`/`textureLoad`
- Supported formats: `rgba8`, `rgba16float`, `r32float`, `rg32float`
- Sampler configuration: nearest, linear, clamp, repeat

### Tasks

- [ ] **Texture descriptor type** (`HlGpuTextureDesc`)
- [ ] **wgpu texture creation + upload** in `gpu_wgpu.c`
- [ ] **Bind group layout** with texture + sampler entries
- [ ] **Lua/JS binding** for `textures` array in dispatch opts
- [ ] **Sample shaders** (gaussian blur, edge detection, resize)

---

## Phase 11 — Streaming I/O

**Goal:** Process datasets larger than memory through WASM in fixed-size chunks.

**Why:** All compute is currently "load entire input, produce entire output."
Can't process a 1 GB file without 1 GB of memory. Streaming enables bounded
memory usage for arbitrarily large inputs.

### Design

```lua
-- Lua
compute.stream("transform", input_file, output_file, {
    chunk_size = 64 * 1024,  -- 64 KB chunks
    gas = 1000000,           -- per-chunk gas limit
})
```

- WASM module exports `hull_process_chunk(in_ptr, in_len, out_ptr, out_max, is_last) → bytes_written`
- Hull reads input in chunks, calls module per-chunk, writes output
- Persistent instance reused across chunks (state preserved)
- Final chunk signaled via `is_last = 1` for flush/finalize

### Tasks

- [ ] **Chunk ABI** (`hull_process_chunk` export alongside `hull_process`)
- [ ] **`compute.stream()` API** (Lua + JS)
- [ ] **File reader/writer integration** with `hl_cap_fs_*`
- [ ] **Accumulator pattern** for reduce operations across chunks

---

## Dependency Graph (Updated)

```
Phase 1 (SIMD128) ✅ ──────────────────┐
                                        ├──→  Phase 3 (Memory64) ✅
Phase 2 (Memory Limits) ✅ ────────────┘
Phase 2.5 (Instance Pooling) ✅
Phase 2.6 (WasmBuffer) ✅
Phase 4 (GPU/WebGPU) ✅
Phase 5a (Persistent Instances) ✅
Phase 5b (Shared Data Segments) ✅

Phase 6 (Developer Tooling)  ←── no deps, start anytime
Phase 7 (Sample Modules)     ←── after Phase 6 (uses scaffolding)
Phase 8 (Result Caching)     ←── no deps, start anytime
Phase 9 (SQLite UDFs)        ←── after Phase 2.5 (uses instance pooling)
Phase 10 (GPU Textures)      ←── after Phase 4 (extends GPU backend)
Phase 11 (Streaming I/O)     ←── after Phase 5a (uses persistent instances)
```

---

## Success Criteria

| Phase | Metric |
|-------|--------|
| 1 | SIMD dot-product AOT within 1.5× of native SSE/NEON ✅ |
| 2 | `compute.call` with 2 GB heap, 100 MB input works correctly ✅ |
| 3 | Memory64 module with 6 GB heap runs under AOT ✅ |
| 4 | GPU matmul 10× faster than WASM SIMD matmul for 4096×4096 ✅ |
| 5a | Persistent instance amortizes load time over 1000+ calls ✅ |
| 5b | Shared data segments readable from pooled + persistent instances ✅ |
| 6 | `hull compute new score --lang=c` produces a buildable, testable module |
| 7 | 6 sample modules with source, .wasm, tests, and documentation |
| 8 | Cached `compute.call` returns in <1µs for repeated identical input |
| 9 | `SELECT hull_score(col) FROM t` executes WASM per-row at >100K rows/sec |
| 10 | GPU blur shader processes 1080p image in <5ms |
| 11 | `compute.stream` processes 1 GB file with <64 MB peak memory |
