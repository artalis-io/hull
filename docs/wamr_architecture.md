# WAMR Compute-Only Plugin Architecture

## A) Executive Summary

- **WAMR embedded in-process** as optional 7th vendored C library (~85 KB interpreter, ~50 KB AOT loader)
- **Entirely optional:** most Hull apps don't need WAMR — Lua is 10-30x faster than Python and 5-10x faster than Ruby, fast enough for HTTP handlers, business logic, and database queries. WAMR is for apps that hit a wall on CPU-bound computation.
- **Compute-only:** NO WASI imports, NO ambient I/O, NO filesystem/network/time/env/random
- **Lua remains the sole capability gate** — only Lua/host can perform I/O
- **Request flow:** HTTP → Keel C router → Lua middleware → optional WAMR plugin → Lua → response
- **Minimal ABI:** single `host_call(opcode, ptr, len)` import, length-prefixed binary framing
- **Two invocation modes:** synchronous fast call (< 10 ms) and async job with progress/cancel (future)
- **Deterministic resource limits:** memory cap per instance, instruction budget (gas metering via `WAMR_BUILD_INSTRUCTION_METERING`), max input/output size
- **Module cache:** load `.wasm` once → instantiate per-request with isolated memory
- **Optional AOT:** `wamrc` pre-compiles `.wasm` → `.aot` for near-native speed
- **BYO language:** C/C++ (clang/WASI-SDK), Rust (`wasm32-unknown-unknown`), Zig (`wasm32-freestanding`), TinyGo
- **Plugins embed** into the APE binary alongside Lua and static assets
- **Single-threaded default:** gas-metered cooperative execution integrated with Hull's event loop
- **Future:** subprocess worker model for truly long-running compute (fork + IPC + hard kill)
- **Total binary size increase:** ~85-150 KB (interpreter + AOT loader), keeping Hull under 2.5 MB
- **Maintains all Hull invariants:** single executable, auditable, sandboxed, local-first

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
║  Opcodes: LOG=1 (MVP), YIELD=2, PROGRESS=3 (future)     ║
║                                                          ║
╚═════════════════════════════════════════════════════════╝
```

**Trust boundaries:**

1. **Browser ↔ Keel:** HTTP over localhost (network boundary)
2. **Keel ↔ Lua:** C↔Lua binding layer (`lua_bindings.c`)
3. **Lua ↔ WAMR:** C bridge (`lua_wasm.c`) — WAMR linear memory is separate from Lua/host memory

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

### Opcodes (versioned)

| Opcode | Name | Behavior | MVP? |
|--------|------|----------|------|
| 0x01 | LOG | Host logs message at `ptr`/`len`. Returns 0. | Yes |
| 0x02 | YIELD | Cooperative yield (future: resume on next tick) | Future |
| 0x03 | PROGRESS | Report progress (0-100 at `ptr` as u8) | Future |

### Required plugin exports

```c
// Plugin must export exactly one of:
int32_t hull_process(int32_t input_ptr, int32_t input_len,
                     int32_t output_ptr, int32_t output_max);
// Returns: bytes written to output_ptr, or negative error code

// Optional:
int32_t hull_version(void);  // Returns ABI version (1 for MVP)
```

### Binary framing (input/output)

```
┌──────────┬──────────┬─────────────┐
│ version  │ length   │ payload     │
│ (1 byte) │ (4 bytes)│ (N bytes)   │
│ LE u8    │ LE u32   │ raw bytes   │
└──────────┴──────────┴─────────────┘
```

- **Version byte:** `0x01` for MVP
- **Length:** little-endian u32, max 16 MB (configurable)
- **Payload:** opaque bytes (JSON, MessagePack, protobuf, raw — plugin decides)

### Memory model

- Host allocates input buffer in WASM linear memory via `wasm_runtime_module_malloc()`
- Host allocates output buffer in WASM linear memory (pre-sized, configurable max)
- Plugin reads input, writes output, returns byte count
- Host copies output back to Lua string
- Host frees both buffers via `wasm_runtime_module_free()`

### Error encoding

| Return value | Meaning |
|---|---|
| >= 0 | Success — number of bytes written to output buffer |
| -1 | Generic error |
| -2 | Output buffer too small (host can retry with larger buffer) |
| -3 | Invalid input |
| -4 | Internal plugin error |

### Size limits (configurable per-plugin via opts table)

| Limit | Default | Max |
|-------|---------|-----|
| Input size | 1 MB | 16 MB |
| Output size | 1 MB | 16 MB |
| WASM heap | 1 MB | 64 MB |
| WASM stack | 64 KB | 1 MB |
| Instruction budget | 10M | 1B |

## E) Resource Limiting & Non-Blocking Execution

### Model 1: In-process cooperative (MVP — recommended default)

- WAMR's `WAMR_BUILD_INSTRUCTION_METERING=1` enables instruction counting
- `wasm_runtime_set_instruction_count_limit(exec_env, limit)` enforces gas budget
- When budget exhausted, WAMR returns error — host reports timeout to Lua
- Synchronous call: Lua blocks until plugin returns (gas-limited, so bounded)
- For short computations (< 10 ms), this is zero-overhead and simple
- Integration with Hull event loop: not needed for sync calls (they're bounded by gas)

### Async variant (future extension of Model 1)

- `plugin.start()` creates a Lua coroutine
- Coroutine calls into WAMR with a chunk of gas budget
- If gas exhausted but not done, plugin exports `hull_resume()` to continue
- Coroutine yields back to event loop, resumes on next tick
- Requires plugin cooperation (must be written to support resume)
- Complex, not MVP

### Model 2: Subprocess worker (future)

- Hull forks a worker process
- Worker embeds WAMR, loads module, runs computation
- IPC via pipe: length-prefixed messages (same framing as ABI)
- Parent sets alarm/timer, hard-kills worker on timeout
- **Advantages:** true preemption, memory isolation at OS level, crash isolation
- **Disadvantages:** fork overhead (~1 ms), IPC serialization, complexity
- **Use case:** untrusted plugins, very long computation (> 1 s)

### Recommendation

Model 1 (in-process, gas-metered) as MVP default. Model 2 as future opt-in for untrusted/long-running plugins. The gas meter handles 99% of use cases. Subprocess adds defense-in-depth for the remaining 1%.

## F) Lua Integration API

```lua
-- Synchronous call (blocking, gas-limited)
local output, err = plugin.call("score", input_bytes, {
    max_input  = 1024 * 1024,    -- 1 MB (optional, has defaults)
    max_output = 1024 * 1024,    -- 1 MB
    gas        = 10000000,       -- 10M instructions
    heap       = 1024 * 1024,    -- 1 MB WASM heap
})
if err then
    -- err is string: "timeout", "output_too_small", "invalid_input",
    --                "plugin_error", "not_found"
end

-- Asynchronous call (future — returns job handle)
local job = plugin.start("transform", input_bytes, {
    gas     = 1000000000,    -- 1B instructions (long job)
    timeout = 5000,          -- 5 second wall clock (subprocess model)
})

job:poll()                -- returns "running", "done", "error", "cancelled"
local output = job:await()  -- blocks until done (with event loop yield)
job:cancel()              -- request cancellation
job:on_progress(function(pct) ... end)  -- progress callback (future)
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

// Host import (optional — only if you need logging)
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

Run with `hull test` — same test framework as all other Hull tests.

## MVP Checklist

1. Vendor WAMR (~85 KB, interpreter mode) as optional 7th C library
2. Build WAMR with `WAMR_BUILD_INSTRUCTION_METERING=1`, no WASI, no threads
3. Implement `HullWasmCache` — load/cache `.wasm` modules
4. Implement `lua_wasm.c` — `plugin.call(name, input, opts)` C binding
5. Implement `hull_process` ABI with input/output buffers in WASM linear memory
6. Implement `host_call` with LOG opcode only
7. Implement gas metering (instruction count limit per call)
8. Implement memory limits (heap size, stack size per instance)
9. Implement input/output size limits
10. Embed `.wasm` files into APE binary during `hull build`
11. Write tests: valid plugin, gas exhaustion, memory limit, oversized I/O, missing export, ABI version check
12. Write minimal example plugins in C and Rust
13. Update docs

## Future Extensions (post-MVP)

- **AOT support:** `wamrc` pre-compilation, `.aot` file loading alongside `.wasm`
- **Async jobs:** `plugin.start()` with job handles, subprocess worker model
- **YIELD/PROGRESS opcodes:** cooperative long-running computation
- **Plugin marketplace:** curated `.wasm` plugins distributed via Hull marketplace
- **Plugin signatures:** Ed25519 signing of `.wasm` files, verified at load time
- **Multi-return:** structured output (JSON envelope with status + payload)
- **Plugin-to-plugin:** composable pipelines (output of one feeds input of another)
