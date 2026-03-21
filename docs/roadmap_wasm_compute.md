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

**What's disabled:** Memory64, shared memory, threads.
**What's enabled:** SIMD128 (AOT-first; interpreter SIMD requires SIMDe, not yet vendored).

---

## Phase 1 — SIMD128

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

- [ ] **Update `wamrc` build**
  - Ensure `make wamrc` produces a compiler that supports SIMD output
  - May need LLVM with vector extension support (should be default)

- [ ] **Cosmopolitan / cross-arch testing**
  - SIMD AOT is arch-specific — verify both x86_64 and aarch64 AOT
    work correctly under cosmocc fat binary builds
  - Interpreter SIMD fallback is graceful failure (not crash)

- [ ] **Update docs**
  - `docs/wamr_architecture.md`: document SIMD support
  - `CLAUDE.md`: note SIMD is enabled, mention compiler flags

### Risk

Low. WAMR's SIMD support is mature (used by browser engines). The main risk
is AOT relocation gaps on specific architectures — mitigated by interpreter
fallback.

---

## Phase 2 — Memory Limits & Configurable Ceiling

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

- [ ] **Update CLI flag parsing**
  - `--wasm-heap` parser: accept suffixed values (`512M`, `2G`)

- [ ] **Test with large allocations**
  - Benchmark: allocate 1 GB WASM heap, fill with data, process
  - Verify mmap input path works with 100 MB+ files
  - Check for 32-bit truncation bugs in the clamping chain

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

## Phase 3 — Memory64

**Goal:** Enable 64-bit WASM memory addressing, allowing >4 GB linear memory.

**Why:** 32-bit WASM memory tops out at 4 GB. For large ML models, high-res
image processing, or big in-memory datasets, 64-bit addressing removes the
ceiling. Memory64 is a standard WASM proposal (Phase 4 at W3C) with growing
toolchain support.

### Tasks

- [ ] **Enable Memory64 in WAMR build flags**
  - `Makefile`: add `-DWASM_ENABLE_MEMORY64=1`
  - Verify WAMR version supports Memory64 (check `vendor/wamr/` version)

- [ ] **Verify ABI compatibility**
  - `hull_process` signature uses `i32` parameters — with Memory64, pointers
    become `i64`
  - Options:
    - A) Require Memory64 modules to export `hull_process_64(i64, i64, i64, i64) → i64`
    - B) Auto-detect Memory64 flag on module and dispatch to correct ABI
    - C) Use a wrapper that truncates (only if heap stays < 4 GB)
  - Recommend **B**: detect at load time, store ABI flag in module cache

- [ ] **Update `host_call` import**
  - Memory64 modules pass 64-bit pointers — `host_call(i32, i64, i64) → i32`
  - WAMR should handle this transparently if imports are declared correctly

- [ ] **Update `wasm_runtime_module_malloc` calls**
  - Use 64-bit variants if available in WAMR's Memory64 API
  - Check `wasm_runtime_validate_app_addr` for 64-bit support

- [ ] **AOT support for Memory64**
  - Verify `wamrc` can compile Memory64 modules to AOT
  - WAMR's Memory64 AOT support may be experimental — test thoroughly

- [ ] **Toolchain documentation**
  - Document how to compile Memory64 WASM modules:
    - Rust: `wasm32-unknown-unknown` doesn't support Memory64 yet;
      `wasm64-unknown-unknown` target is experimental
    - C/C++ (clang): `-mmemory64` flag with WASI SDK
    - Zig: `wasm64-freestanding` target

- [ ] **Test with >4 GB working sets**
  - Allocate 6 GB WASM heap, process large dataset
  - Verify gas metering works correctly with 64-bit memory ops

### Risk

High. Memory64 is still maturing in both WAMR and toolchains. WAMR's support
may be incomplete for AOT or have edge cases. The ABI change for
`hull_process` needs careful design to avoid breaking existing 32-bit modules.
Plan for a compatibility shim.

### Compatibility

Existing 32-bit WASM modules must continue to work unchanged. The module
cache entry should store a `memory64` flag detected at load time. Call
dispatch selects the correct ABI based on this flag.

---

## Phase 4 — GPU Compute via WebGPU

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

### Tasks

- [ ] **Vendor wgpu-native**
  - Add pre-built static libraries (x86_64, aarch64) or build from source
  - Option A: git submodule + build via cargo (requires Rust toolchain)
  - Option B: download pre-built `.a` from wgpu-native releases (simpler)
  - Recommend B for now — avoids Rust build dependency

- [ ] **Define `HlGpuVtable` interface**
  - `include/hull/cap/gpu.h`: vtable, types, error codes
  - `HlGpuDispatchOpts`: buffer descriptors, workgroup dims, uniforms
  - Keep it minimal — don't expose the full WebGPU API

- [ ] **Implement `gpu_stub.c`**
  - No-op vtable: all methods return `HL_GPU_ERR_NOT_AVAILABLE`
  - Compiled when `HL_ENABLE_GPU=0` (default)

- [ ] **Implement `gpu_wgpu.c`**
  - Init: `wgpuCreateInstance` → adapter → device (headless, no surface)
  - Pipeline cache: compile WGSL once, cache pipeline object
  - Dispatch: create buffers → write input → dispatch → read output
  - Memory management: explicit buffer create/destroy, no leaks

- [ ] **Shader embedding via VFS**
  - `app_dir/shaders/*.wgsl` embedded alongside templates/static/compute
  - `hl_vfs_find(app_vfs, "shaders/matmul.wgsl")` for lookup

- [ ] **Lua/JS bindings**
  - `gpu.dispatch(name, opts)` — async, yields to event loop
  - `gpu.available()` → boolean (check if GPU backend is active)
  - Buffer data from Lua strings, JS ArrayBuffers, or `fs.mmap` buffers

- [ ] **Manifest integration**
  - `gpu = true` in manifest to declare GPU capability
  - Sandbox: GPU device access requires explicit opt-in

- [ ] **Build integration**
  - `make HL_ENABLE_GPU=1` links wgpu-native
  - Binary size impact: wgpu-native is ~5-15 MB (significant — document this)
  - Consider dynamic linking (`dlopen`) to avoid binary bloat when GPU
    is rarely used

- [ ] **Testing**
  - Unit tests with stub backend (no GPU required)
  - Integration tests with wgpu-native (CI needs GPU or software renderer)
  - Benchmark: GPU matmul vs WASM SIMD matmul vs native

### Risk

High. This is the largest item on the roadmap.

- **Binary size:** wgpu-native adds 5-15 MB. May warrant dynamic linking
  or a separate `hull-gpu` binary.
- **CI/testing:** GPU tests need hardware or a software Vulkan driver
  (lavapipe/SwiftShader). Adds CI complexity.
- **API design:** Exposing the right level of abstraction is hard — too low
  and it's unusable, too high and it's inflexible.
- **Platform support:** GPU availability varies. Must degrade gracefully.
- **Cosmo compatibility:** wgpu-native likely won't work with cosmocc
  (Vulkan/Metal requires platform-specific linking).

### When to Start

After Phases 1-3 are complete and validated. GPU compute is a separate
capability that doesn't block the WASM improvements.

---

## Phase 5 — Persistent WASM Instances (Future)

**Goal:** Long-lived WASM instances that retain state across calls.

**Why:** Stateful workloads (ML model weights, pre-built indexes, incremental
statistics) pay a high cost re-instantiating per call. A persistent instance
amortizes setup over many invocations.

```lua
-- Hypothetical API
local inst = compute.instance("model", { heap = 2 * 1024 * 1024 * 1024 })
inst:call("load_weights", weights_data)       -- one-time setup
local result = inst:call("predict", input)    -- fast repeated calls
inst:close()                                  -- explicit cleanup
```

### Considerations

- Breaks the "fresh instance per call" isolation model — document the
  tradeoff (performance vs isolation)
- Needs lifecycle management: create, use, close, GC/finalizer
- Memory isn't reset between calls — module is responsible for its own state
- Gas metering resets per `call()` invocation
- Thread safety: instance pinned to one thread, or mutex-guarded
- Pairs well with Memory64 for large model weights

### Not yet planned in detail — depends on real-world demand from Phases 1-3.

---

## Dependency Graph

```
Phase 1 (SIMD128)  ──────────────────────┐
                                          ├──→  Phase 3 (Memory64)
Phase 2 (Memory Limits)  ────────────────┘          │
                                                     │
                                          Phase 5 (Persistent Instances)

Phase 4 (GPU/WebGPU)  ── independent, start after Phase 3
```

Phases 1 and 2 can proceed in parallel. Phase 3 depends on the type
widening done in Phase 2. Phase 4 is independent. Phase 5 is future.

---

## Success Criteria

| Phase | Metric |
|-------|--------|
| 1 | SIMD dot-product AOT within 1.5× of native SSE/NEON |
| 2 | `compute.call` with 2 GB heap, 100 MB input works correctly |
| 3 | Memory64 module with 6 GB heap runs under AOT |
| 4 | GPU matmul 10× faster than WASM SIMD matmul for 4096×4096 |
| 5 | Persistent instance amortizes load time over 1000+ calls |
