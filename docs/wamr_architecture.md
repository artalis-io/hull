# WAMR Compute Architecture (Shipped)

> **Status: shipped and in production use.** This document is the
> implemented design, not a proposal. Toggle the subsystem with
> `HL_ENABLE_WASM` (default `1`; `make HL_ENABLE_WASM=0` drops WAMR
> entirely and saves ~256 KB). For the data-parallel sibling see GPU
> compute in [`../README.md#gpu-compute`](../README.md#gpu-compute) and
> the remaining roadmap items in [`roadmap.md`](roadmap.md).

## A) Executive Summary

- **WAMR embedded in-process** as an optional vendored C library (~256 KB compiled with interpreter + AOT loader + shared-heap support, included in the ~5 MB default Hull binary).
- **Entirely optional:** most Hull apps don't need WAMR. Lua is 10-30× faster than Python and 5-10× faster than Ruby, fast enough for HTTP handlers, business logic, and database queries. WAMR is for apps that hit a wall on CPU-bound computation; GPU compute (`HL_ENABLE_GPU=1`) covers the data-parallel side.
- **Compute-only:** NO WASI imports, NO ambient I/O, NO filesystem/network/time/env/random.
- **Capability layer remains the sole I/O gate**. Only Lua/JS/host code can perform I/O via `hl_cap_*`. A WASM module is a pure function from input bytes to output bytes.
- **Request flow:** HTTP → Keel C router → Lua/JS middleware → optional WAMR plugin → Lua/JS → response.
- **Minimal ABI:** single `host_call(opcode, ptr, len) -> int` import, length-prefixed binary framing. Opcodes: `LOG=0x01`, `DATA_INFO=0x02`, `STREAM=0x03`, `CALLBACK=0x10`.
- **Two invocation modes shipped:** synchronous `compute.call` (gas-limited, blocking) and asynchronous `compute.async.call` (dispatches to the thread pool, yields to the event loop, resolves via Lua coroutine / JS Promise).
- **Deterministic resource limits:** memory cap per instance (default 2 MiB heap, configurable to ~4 GiB on WASM32 or 16 GiB on Memory64), instruction budget via WAMR's instruction metering (default 100M per call), max input/output size. Each is configurable at compile time, manifest, and per-call.
- **Module cache + instance pool:** load `.wasm` (or `.aot`) once; per-module instance pool (size 8, heap ≤ 4 MB) reuses linear memory across `compute.call`s and cuts per-call overhead from ~2.5 ms to near-zero.
- **AOT shipped:** `wamrc` pre-compiles `.wasm` → `.aot.<arch>` for near-native speed. `hull build` auto-AOT-compiles when `wamrc` is available; for cosmocc fat binaries both `x86_64` and `aarch64` AOTs are emitted.
- **Memory64 (cap-layer dispatch + mapped spans shipped; `hull build` plugin path tracked in [#336](https://github.com/artalis-io/hull/issues/336)):** `(memory i64 N)` modules are detected automatically and dispatched with the 8-cell `(i64,i64,i64,i64)` calling convention; Memory64 is AOT-only (the fast interpreter doesn't support it) and `wamrc` auto-detects it from the module's `(memory i64)` type (there is no `--enable-memory64` wamrc flag). Detection uses the public accessor `wasm_runtime_memory_is_memory64` (WAMR patch 0005), so `cap/wasm.c` carries no WAMR-internal header and no WAMR compile-time config. Detection, the `memory64_requires_aot` guard, and the 8-cell AOT dispatch/readback are CI-gated (`memory64_aot_dispatch`, must-not-skip; #318). **Mapped spans under Memory64 are validated** ([#334](https://github.com/artalis-io/hull/issues/334), CI-gated `memory64_span_readback`): a `(memory i64)` guest reads a span window above `UINT32_MAX` through the SPAN_INFO record's 64-bit `base` (WAMR's guarded-subrange RO shared-heap addressing is memory64-correct under AOT). See [memory64_dispatch_design.md](memory64_dispatch_design.md), [memory64_spans_design.md](memory64_spans_design.md). NOT yet supported: the `hull build` AOT path for a Memory64 compute *plugin* — #336.
- **SIMD128 shipped:** `-DWASM_ENABLE_SIMD=1`, AOT maps WASM SIMD to native SSE4.1 / NEON. Interpreter cannot load v128 modules (graceful `HL_WASM_ERR_LOAD`).
- **Shared data segments shipped:** `compute.segment(module, name, bytes)` loads up to 16 named read-only segments per module via WAMR's shared-heap mechanism. Multi-GB datasets readable at native speed by every instance (pooled and persistent).
- **Persistent instances shipped:** `compute.instance(name, opts)` creates a long-lived WASM instance that retains linear memory across calls. Not pooled. Exclusively owned by the caller. Used for stateful workloads (ML weights, pre-built indexes).
- **Streaming I/O shipped:** `compute.stream(name, input, output, opts)` processes datasets larger than memory in fixed-size chunks. Persistent instance internally; state preserved between chunks; modules query chunk metadata via `host_call(0x03)`.
- **Unified buffer protocol:** WASM input accepts strings, JS `ArrayBuffer`s, `WasmBuffer` (output of a previous `compute.call`), and `MappedBuffer` (`fs.mmap`). All zero-copy where the source layout permits.
- **BYO language:** C/C++ (clang `--target=wasm32 -nostdlib`), Rust (`wasm32-unknown-unknown`), Zig (`wasm32-freestanding`), TinyGo, AssemblyScript.
- **Plugins embed** into the APE binary alongside Lua/JS modules and static assets via the unified VFS (`compute/*.wasm` + `compute/*.aot.<arch>`, sorted, O(log n) lookup).
- **Single-threaded interpreter, multi-threaded async dispatch:** synchronous calls run on the event loop; `compute.async.call` dispatches to the thread pool.
- **Maintains all Hull invariants:** single executable, auditable, capability-gated, local-first.

## B) Architecture Diagram

```
┌─────────────────────────────────────────────────────────┐
│                    Browser (HTML5/JS)                     │
└──────────────────────────┬──────────────────────────────┘
                           │ HTTP (localhost)
┌──────────────────────────┴──────────────────────────────┐
│                    Keel C Router                         │
│              route match → middleware chain               │
└──────────────────────────┬──────────────────────────────┘
                           │
┌──────────────────────────┴──────────────────────────────┐
│                    Lua Runtime                           │
│         routes, middleware, business logic, I/O           │
│                                                          │
│  ┌─────────────────────────────────────────────────┐    │
│  │  plugin.call("score", input)                     │    │
│  │  plugin.start("transform", input, {timeout=5000})│    │
│  └───────────────────────┬─────────────────────────┘    │
└──────────────────────────┼──────────────────────────────┘
                           │ C bridge (lua_wasm.c)
╔══════════════════════════╧══════════════════════════════╗
║              TRUST BOUNDARY (WAMR sandbox)               ║
╠═════════════════════════════════════════════════════════╣
║                                                          ║
║  ┌──────────────┐    ┌──────────────────────────┐       ║
║  │ Module Cache  │    │  Per-Request Instance     │       ║
║  │              │───▸│  - isolated linear memory  │       ║
║  │ score.wasm   │    │  - gas-metered execution   │       ║
║  │ transform.wasm    │  - memory-capped heap      │       ║
║  │ dedupe.aot   │    │  - no host imports (I/O)   │       ║
║  └──────────────┘    └──────────────────────────┘       ║
║                                                          ║
║  Only import: host_call(opcode, ptr, len) → status       ║
║  Opcodes: LOG=0x01, DATA_INFO=0x02, STREAM=0x03,         ║
║          CALLBACK=0x10                                    ║
║                                                          ║
╚═════════════════════════════════════════════════════════╝
```

**Trust boundaries:**

1. **Browser ↔ Keel:** HTTP over localhost (network boundary)
2. **Keel ↔ Lua:** C↔Lua binding layer (`lua_bindings.c`)
3. **Lua ↔ WAMR:** C bridge (`lua_wasm.c`). WAMR linear memory is separate from Lua/host memory

## C) Compute-Only Plugin Contract

### What a plugin CAN do

- Pure functions: `transform(input) → output`
- Read and write its own linear memory
- Call `host_call(LOG, ptr, len)` to emit debug messages (host decides whether to log)
- Return a status code + output buffer

### What a plugin CANNOT do

- Access filesystem, network, environment variables, random, time
- Allocate host memory
- Call any WASI function (no WASI imports registered)
- Interact with other plugins or Lua state
- Spawn threads or processes

### Invocation modes

| Mode | API | Use case | Blocking? |
|------|-----|----------|-----------|
| Synchronous | `plugin.call(name, input, opts)` | Fast transforms < 10 ms | Yes (gas-limited) |
| Asynchronous | `plugin.start(name, input, opts)` | Long compute, cancellable | No (returns job handle) |

### Typical use cases

- Header computation (e.g., content hash, ETag generation)
- Scoring / ranking (relevance, priority, risk)
- Deduplication keys (fingerprinting, similarity hashing)
- Request/response transformation (compression, encoding, format conversion)
- Feature flag evaluation (complex rule engine)
- Optimization heuristics (scheduling, allocation)
- PDF layout computation (text flow, table layout)
- Statistical calculations (financial modeling, Monte Carlo)
- Image processing (resize, thumbnail, format conversion)
- CSV/Excel parsing (millions of rows)

## D) Minimal ABI Spec

### Single host import

```c
// The only function a plugin can import from host
int32_t host_call(int32_t opcode, int32_t ptr, int32_t len);
```

### Opcodes (shipped)

| Opcode | Name | Behavior |
|--------|------|----------|
| 0x01 | LOG | Host logs message at `ptr`/`len`. Returns 0. |
| 0x02 | DATA_INFO | Query shared data segments: `(0x02, seg_id, 0)` returns WASM address; `(0x02, seg_id, 1)` returns size; `(0x02, -1, 0)` returns segment count. |
| 0x03 | STREAM | Query streaming-chunk metadata: `is_first()`, `is_last()`, `chunk_index()` for the `compute.stream` API. |
| 0x10 | CALLBACK | Invoke a host-provided callback (used by `db.udf` aggregate UDFs and similar gateways). |

### Required plugin exports

```c
// Plugin must export exactly one of:
int32_t hull_process(int32_t input_ptr, int32_t input_len,
                     int32_t output_ptr, int32_t output_max);
// Returns: bytes written to output_ptr, or negative error code

// Optional:
int32_t hull_version(void);  // Returns ABI version (currently 1)
```

### Binary framing (input/output)

```
┌──────────┬──────────┬─────────────┐
│ version  │ length   │ payload     │
│ (1 byte) │ (4 bytes)│ (N bytes)   │
│ LE u8    │ LE u32   │ raw bytes   │
└──────────┴──────────┴─────────────┘
```

- **Version byte:** `0x01` (current ABI)
- **Length:** little-endian u32, max 16 MB (configurable)
- **Payload:** opaque bytes (JSON, MessagePack, protobuf, raw. Plugin decides)

### Memory model

- Host allocates input buffer in WASM linear memory via `wasm_runtime_module_malloc()`
- Host allocates output buffer in WASM linear memory (pre-sized, configurable max)
- Plugin reads input, writes output, returns byte count
- Host copies output back to Lua string
- Host frees both buffers via `wasm_runtime_module_free()`

### Error encoding

| Return value | Meaning |
|---|---|
| >= 0 | Success. Number of bytes written to output buffer |
| -1 | Generic error |
| -2 | Output buffer too small (host can retry with larger buffer) |
| -3 | Invalid input |
| -4 | Internal plugin error |

### Size limits (configurable per-call, per-manifest, and at compile time)

| Limit | Default | Max | CLI flag |
|-------|---------|-----|----------|
| Input size | 1 MB | 256 MB | `--wasm-max-input` |
| Output size | 1 MB | 256 MB | `--wasm-max-output` |
| WASM heap | 2 MB | ~4 GB | `--wasm-heap` |
| WASM stack | 64 KB | 8 MB | `--wasm-stack` |
| Instruction budget | 10M | 100B | `--wasm-gas` |

Three-tier resolution: **per-call opts > CLI/manifest ceiling > compile-time default**.
Compile-time maximums are `#ifndef`-guarded. Override via `make HL_WASM_MAX_HEAP_MB=512`.

## E) Resource Limiting & Non-Blocking Execution

### Model 1: In-process cooperative (default)

- WAMR's `WAMR_BUILD_INSTRUCTION_METERING=1` enables instruction counting
- `wasm_runtime_set_instruction_count_limit(exec_env, limit)` enforces gas budget
- When budget exhausted, WAMR returns error. Host reports timeout to Lua
- Synchronous call: Lua blocks until plugin returns (gas-limited, so bounded)
- For short computations (< 10 ms), this is zero-overhead and simple
- Integration with Hull event loop: not needed for sync calls (they're bounded by gas)

### Async variant (shipped. Model 1b)

- `compute.async.call(name, input, opts)` dispatches the WASM execution to
  the thread pool (`--workers N`, default 4).
- The handler's Lua coroutine / JS Promise yields back to the event loop so
  the loop keeps serving other requests while the WASM job runs.
- On completion the worker thread resumes the handler with the result bytes
  (or error). Persistent instance + streaming variants follow the same
  pattern.
- No `hull_resume` callback is required from the plugin. The thread
  isolation does the cooperation for us.

### Model 2: Subprocess worker (not implemented; explicit non-goal)

A subprocess model (fork + IPC + hard kill) was considered for untrusted
or very-long-running plugins. We decided against it: the in-process
gas-metered model + the thread pool covers every realistic workload, and
subprocesses break the single-binary distribution story (you can't fork
a Cosmopolitan APE binary cleanly on every supported OS). Apps that need
process-level isolation should run a separate Hull binary behind an
HTTP boundary.

## F) Lua / JS Integration API (shipped)

```lua
-- Lua: synchronous call (blocking, gas-limited)
local output, err = compute.call("score", input_bytes, {
    max_input  = 1024 * 1024,    -- 1 MB (optional, has defaults)
    max_output = 1024 * 1024,    -- 1 MB
    gas        = 10000000,       -- 10M instructions
    heap       = 1024 * 1024,    -- 1 MB WASM heap
})
if err then
    -- err is string: "gas_exhausted", "output_too_small",
    --                "input_too_large", "call_failed",
    --                "not_found", "internal_error"
end

-- Lua: async call (dispatches to thread pool, yields to event loop)
local r = compute.async.call("transform", input_bytes, opts)
-- r.result is bytes on success; r.error is a string on failure.

-- Lua: persistent instance for stateful workloads
local inst = compute.instance("model", { heap = 64 * 1024 * 1024 })
inst:call(query_1)
inst:call(query_2)
inst:close()

-- Lua: streaming
compute.stream("transform", { file = "input.csv" }, { file = "output.json" },
               { chunk_size = 65536 })
```

```javascript
// JS: same API in camelCase
import { compute } from "hull:compute";

const out = compute.call("score", inputBytes, {
    maxInput: 1024 * 1024,
    maxOutput: 1024 * 1024,
    gas: 10000000,
});

const buf = await compute.async.call("transform", inputBytes, opts);

const inst = compute.instance("model", { heap: 64 * 1024 * 1024 });
inst.call(query1);
inst.close();
```

### C bridge (`lua_wasm.c`)

Binding through Keel's C layer:

```c
// Registered as Lua C function
static int l_plugin_call(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    size_t input_len;
    const char *input = luaL_checklstring(L, 2, &input_len);
    // Parse opts table from arg 3 (optional)

    // 1. Lookup cached module by name
    // 2. Instantiate with configured heap/stack limits
    // 3. Set instruction count limit
    // 4. Allocate input/output buffers in WASM memory
    // 5. Copy input, call hull_process
    // 6. Copy output back to Lua string
    // 7. Deinstantiate
    // 8. Return (output, nil) or (nil, error_string)
}
```

The `plugin` table is registered in Lua's global namespace by `lua_wasm.c` during Hull startup, same pattern as `db`, `json`, `session`, etc.

## G) C Routing Middleware Integration

### Where plugins can be invoked (all via Lua middleware)

| Phase | Example | Pattern |
|-------|---------|---------|
| Pre-route | Rate limit scoring | `app.use("*", "/*", function(req, res) ... plugin.call("rate_score", ...) end)` |
| Auth | Token validation/decoding | `app.use("*", "/api/*", function(req, res) ... plugin.call("jwt_verify", ...) end)` |
| Transform | Request body processing | Inside route handler, before business logic |
| Response | Output encoding/compression | Inside route handler, before `res:json()`/`res:html()` |

Plugins are always invoked FROM Lua. They are never registered directly as Keel C middleware. This maintains Lua as the single orchestration layer.

### Module caching

```c
typedef struct {
    char name[256];
    wasm_module_t module;       // Parsed WASM bytecode (shared, read-only)
    uint8_t *aot_buf;           // AOT buffer if loaded from .aot file
    size_t aot_size;
    uint32_t abi_version;       // From hull_version() export
} HullWasmModule;

typedef struct {
    HullWasmModule *modules;
    int count;
    int capacity;
    KlAllocator *alloc;
} HullWasmCache;
```

- Modules loaded on first `plugin.call()` or at startup via `plugin.preload("name")`
- Module stays cached for process lifetime (WASM bytecode is immutable)
- Per-invocation: fresh `wasm_module_inst_t` + `wasm_exec_env_t` (isolated memory)
- Optional AOT: if `plugins/name.aot` exists alongside `plugins/name.wasm`, load AOT
- In production: `.wasm` files embedded in APE binary alongside Lua and static assets

## H) Developer Guide

### Supported languages

| Language | Target | Toolchain | Notes |
|----------|--------|-----------|-------|
| C/C++ | `wasm32` | clang `--target=wasm32` or WASI-SDK (no WASI imports) | Smallest output, most control |
| Rust | `wasm32-unknown-unknown` | `cargo build --target wasm32-unknown-unknown` | Use `#![no_std]`, no WASI |
| Zig | `wasm32-freestanding` | `zig build -Dtarget=wasm32-freestanding` | Excellent WASM support |
| Go | wasm | TinyGo `tinygo build -target=wasm` | Larger output (~100 KB+), GC overhead |

### Minimal example plugin (C)

```c
// plugins/score.c
// Compile: clang --target=wasm32 -nostdlib -O2 -o score.wasm score.c

// Host import (optional. Only if you need logging)
extern int host_call(int opcode, int ptr, int len);

// Required export: process input → output
__attribute__((export_name("hull_process")))
int hull_process(const char *input, int input_len,
                 char *output, int output_max) {
    // Example: compute a simple score byte from input
    int score = 0;
    for (int i = 0; i < input_len; i++) {
        score += (unsigned char)input[i];
    }
    score = score % 101;  // 0-100

    if (output_max < 1) return -2;  // output too small
    output[0] = (char)score;
    return 1;  // wrote 1 byte
}

// Optional: declare ABI version
__attribute__((export_name("hull_version")))
int hull_version(void) { return 1; }
```

### Build pipeline

1. Write plugin in any supported language
2. Compile to `.wasm` targeting `wasm32` with no WASI imports
3. Place in `plugins/` directory
4. Optional: pre-compile to AOT with `wamrc -o plugin.aot plugin.wasm`
5. In Lua: `local out = plugin.call("score", input_data)`
6. `hull build` embeds `.wasm` (and `.aot` if present) into the APE binary

### Testing locally

```lua
-- tests/test_score.lua
local out, err = plugin.call("score", "hello")
assert(not err, "plugin error: " .. tostring(err))
assert(#out == 1, "expected 1 byte output")
print("score: " .. string.byte(out, 1))
```

Run with `hull test`. Same test framework as all other Hull tests.

## Implementation Status (all shipped)

| Item | Source |
|------|--------|
| WAMR vendored, interpreter + AOT + shared-heap | `vendor/wamr/` (git submodule) |
| Build flags: instruction metering, no WASI, no threads, SIMD128, Memory64 | `Makefile` |
| Module cache + per-module instance pool | `src/hull/cap/wasm.c` (`HlWasmCache`) |
| `compute.call` / `compute.async.call` / `compute.instance` / `compute.stream` | `src/hull/runtime/{lua,js}/mod_compute.c` |
| `hull_process` ABI + `host_call(LOG / DATA_INFO / STREAM / CALLBACK)` | `src/hull/cap/wasm.c`, `cap/wasm_data.c`, `cap/wasm_stream.c` |
| Gas metering, memory limits, input/output limits | `HlWasmCallOpts`, `wasm_config` resolution (compile-time → manifest → per-call) |
| `.wasm` + `.aot.<arch>` embedded via VFS at `hull build` time | `build.lua` + `src/hull/build.c` |
| Unit tests | `tests/hull/test_wasm.c` (55 cases), `test_wasm_buffer.c` (12 cases) |
| Example plugins | `examples/compute/`, `examples/compute_gpu_chain/` |

## I) SIMD128 Support

WAMR is built with `-DWASM_ENABLE_SIMD=1`, enabling 128-bit SIMD vector operations.

**Compiler flags for plugins:**
- C/C++: `clang --target=wasm32 -msimd128 -O2`
- Rust: `#[target_feature(enable = "simd128")]`
- Zig: `-mcpu=generic+simd128`

**Execution modes:**
- **AOT (recommended):** WAMR maps WASM SIMD to native instructions. SSE4.1 on x86_64, NEON on aarch64. Near-native vector throughput.
- **Interpreter:** Cannot load modules with `v128` types (graceful `HL_WASM_ERR_LOAD`, no crash). SIMDe vendoring would enable interpreter SIMD but is not yet done.

**Performance (aarch64 AOT benchmarks):**
- Dot product 1M elements: SIMD AOT 4.5× native, scalar AOT 5.0× → **1.12× SIMD speedup**
- Matmul 256×256: SIMD AOT **0.94× native** (faster than scalar C), **1.11× SIMD speedup**
- SIMD benefit is modest (~1.1×) for these workloads due to memory bandwidth limits and gather overhead in the matmul inner loop. Workloads with better data locality (image filters, FFT) will see larger gains.

**Plugin example with SIMD:**
```c
#include <wasm_simd128.h>

int32_t hull_process(const void *in, int32_t in_len, void *out, int32_t out_max) {
    v128_t sum = wasm_f32x4_splat(0.0f);
    const float *data = (const float *)in;
    for (int i = 0; i < in_len / 16; i++) {
        v128_t v = wasm_v128_load(&data[i * 4]);
        sum = wasm_f32x4_add(sum, v);
    }
    wasm_v128_store(out, sum);
    return 16;
}
```

## J) Instance Pooling

Every `compute.call()` previously created a fresh WASM instance (~2.5ms allocation). Instance pooling reuses instances between calls, amortizing instantiation to near-zero.

- Per-module pool of `(instance, exec_env, process_fn)` tuples, keyed by `(heap_size, stack_size)`
- Pool max: `HL_WASM_POOL_MAX` (default 8) per module
- Instances with heap > `HL_WASM_POOL_HEAP_THRESHOLD` (4 MB) are never pooled
- Failed calls destroy the instance. Never pooled
- Single `pthread_mutex_t` guards all pool operations; WASM execution is outside the lock
- Linear memory is not reset between calls (safe: compute plugins are pure functions)

## K) Zero-Copy Output Buffers

`HlWasmBuffer` wraps compute output without copying. Three backing kinds:

| Kind | Backing | Use case |
|------|---------|----------|
| OWNED | `malloc`'d bytes | Default, non-poolable instances |
| MMAP | Kernel mapping via `fs.mmap` | Large file input |
| WASM | Pointer into pooled instance linear memory | Zero-copy chaining |

Opt in via `{ buffer = true }` in `compute.call()` / `compute.async.call()`:

```lua
local buf = compute.call("transform", input, { buffer = true })
local output = buf:bytes()   -- materialize to string
buf:close()                  -- explicit release (or GC)
```

WASM-backed buffers keep the pooled instance checked out until `close()` / GC. Non-poolable instances eagerly copy to OWNED.

## Future Extensions

| Item | Status |
|------|--------|
| AOT support | ✅ Shipped. `wamrc`, `.aot.<arch>` loading, auto-AOT during `hull build`, multi-arch under cosmocc |
| Async dispatch | ✅ Shipped. `compute.async.call` via thread pool |
| Persistent instances | ✅ Shipped. `compute.instance` |
| Shared data segments | ✅ Shipped. `compute.segment` (up to 16 per module via WAMR shared heaps) |
| Streaming I/O | ✅ Shipped. `compute.stream` with chunk metadata via `host_call(0x03)` |
| Memory64 cap dispatch + mapped spans | ✅ Shipped. Detection via the public accessor `wasm_runtime_memory_is_memory64` (patch 0005); `memory64_requires_aot` guard + 8-cell AOT dispatch/readback CI-gated ([#318](https://github.com/artalis-io/hull/issues/318)); SPAN_INFO mapped spans on a 64-bit guest read a window >`UINT32_MAX` through the record's 64-bit `base`, CI-gated `memory64_span_readback` ([#334](https://github.com/artalis-io/hull/issues/334)). NOT yet: the standard `hull build` AOT path for a Memory64 *plugin* — [#336](https://github.com/artalis-io/hull/issues/336) |
| Plugin scaffolding (`hull compute new` / `build` / `test` / `check`) | ✅ Shipped. Full dev lifecycle including auto-rebuild during `hull build` and per-module enumeration in `hull agent deploy`. See [`../README.md#authoring-compute-modules`](../README.md#authoring-compute-modules). |
| Sample compute modules | ✅ Shipped (initial set). `examples/compute/` covers vector_ops, sort, hash, json_extract, scoring, text. |
| Rust scaffolding (`hull compute new --lang=rust`) | Planned ([`roadmap.md`](roadmap.md)). Manually-authored Rust modules work today (only need `hull_process` + `hull_version` exports); only the scaffolding shortcut is C-only. |
| Plugin signatures | Considered. `package.sig` already covers embedded `.wasm`/`.aot` bytes; per-module signing is a separable concern |
| Plugin-to-plugin composition | Available today. `WasmBuffer` outputs flow into the next `compute.call` zero-copy via the unified buffer protocol |
