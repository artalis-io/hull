# Plan: Phase 3 — Memory64 (WASM64 Linear Memory)

## Context

WASM32 modules are limited to ~4 GB linear memory. For workloads that need
larger heaps (planet-scale routing graphs, large ML models), Memory64 extends
WASM addresses to 64-bit, allowing heaps up to terabytes.

Phase 5b shared data covers the read-only case (multi-GB datasets mapped into
shared heaps). Memory64 covers the mutable case — where the WASM plugin itself
needs >4 GB of writable linear memory.

## Key Constraint: Fast Interpreter Incompatibility

**WAMR's Memory64 is mutually exclusive with the fast interpreter.**

From `vendor/wamr/build-scripts/unsupported_combination.cmake`:
```
MEMORY64 + FAST_INTERP = unsupported
MEMORY64 + FAST_JIT = unsupported
MEMORY64 + LLVM_JIT = unsupported
```

Memory64 is only supported in: **classic interpreter** and **AOT**.

Hull currently uses `WASM_ENABLE_FAST_INTERP=1`. Switching to the classic
interpreter would regress all WASM32 module performance ~2x in dev mode.

### Solution: Memory64 = AOT-only

Keep `WASM_ENABLE_FAST_INTERP=1`. Enable `WASM_ENABLE_MEMORY64=1`. The fast
interpreter handles WASM32 modules (dev mode). Memory64 modules **must** be
AOT-compiled — interpreter fallback is not available.

- `hull dev` with a Memory64 `.wasm` file (no AOT) → clear error:
  "Memory64 modules require AOT compilation (run hull build or wamrc)"
- `hull build` AOT-compiles Memory64 modules with `wamrc --enable-memory64`
- Production: AOT binary runs at native speed, no interpreter involved

This matches the SIMD pattern — SIMD modules technically load in interpreter
but only perform well as AOT. Memory64 is stricter: AOT or nothing.

## ABI

Memory64 changes the hull_process signature. Pointers become i64:

```
WASM32: hull_process(i32 in_ptr, i32 in_len, i32 out_ptr, i32 out_max) -> i32
WASM64: hull_process(i64 in_ptr, i64 in_len, i64 out_ptr, i64 out_max) -> i32
```

The return value stays i32 (bytes written or error code — always fits in 32 bits).

`wasm_runtime_call_wasm` uses `uint32_t argv[]` for all modules. For i64 args,
two consecutive uint32 cells encode one i64 (little-endian on LE platforms):
```c
// WASM32: 4 argv cells
uint32_t argv[4] = { (uint32_t)in_ptr, (uint32_t)in_len,
                      (uint32_t)out_ptr, max_output };

// WASM64: 8 argv cells (each i64 = 2 × uint32)
uint32_t argv[8];
argv[0] = (uint32_t)(in_ptr);       argv[1] = (uint32_t)(in_ptr >> 32);
argv[2] = (uint32_t)(input_len);    argv[3] = (uint32_t)(input_len >> 32);
argv[4] = (uint32_t)(out_ptr);      argv[5] = (uint32_t)(out_ptr >> 32);
argv[6] = (uint32_t)(max_output);   argv[7] = (uint32_t)(max_output >> 32);
// result in argv[0] (i32, single cell)
```

`wasm_runtime_module_malloc` already returns `uint64_t`. For WASM32, the value
is ≤ UINT32_MAX. For Memory64, it can be larger.

## Type Widening

### HlWasmCallOpts

`heap_size` stays `uint32_t` — even with Memory64, heap size in bytes is passed
to `wasm_runtime_instantiate` as a `uint32_t`. WAMR uses page count internally
(64 KB pages), so 4 GB = 65536 pages fits in uint32. Larger heaps are specified
via the module's memory section (min/max pages), not via the runtime API.

`max_input` and `max_output` widen to `uint64_t`:
```c
typedef struct {
    uint64_t max_input;     /* default: 1 MB, max: 16 GB */
    uint64_t max_output;    /* default: 1 MB, max: 16 GB */
    uint32_t heap_size;     /* default: 2 MB, max: ~4 GB (WAMR limit) */
    uint32_t stack_size;    /* default: 64 KB, max: 8 MB */
    int64_t  gas;
} HlWasmCallOpts;
```

This is a **source-compatible** change for existing callers — uint32 values
assigned to uint64 fields work without casts. But it's an **ABI break** for
any code that reads the struct layout directly (none external — Hull-internal
only).

### Limits

```c
/* limits.h */
#ifndef HL_WASM_MAX_IO_SIZE
#define HL_WASM_MAX_IO_SIZE   ((uint64_t)16 * 1024 * 1024 * 1024)  /* 16 GB */
#endif
```

The old 256 MB uint32 limit was WASM32-era. Memory64 modules can handle much
larger I/O.

## Detection

WAMR stores `is_memory64` on the memory instance. To check after loading a
module, instantiate a temporary instance and inspect:

```c
static int module_is_memory64(wasm_module_t module)
{
    char err[128];
    wasm_module_inst_t tmp = wasm_runtime_instantiate(module, 8192, 8192, err, sizeof(err));
    if (!tmp) return 0;
    WASMMemoryInstance *mem = wasm_get_default_memory((WASMModuleInstance *)tmp);
    int is64 = mem ? mem->is_memory64 : 0;
    wasm_runtime_deinstantiate(tmp);
    return is64;
}
```

Store `is_memory64` on `HlWasmModule` alongside `is_aot`.

## Files to Modify

| File | Changes |
|------|---------|
| `Makefile` | Add `-DWASM_ENABLE_MEMORY64=1` to `WAMR_CFLAGS` |
| `include/hull/limits.h` | Widen `HL_WASM_MAX_IO_SIZE` to uint64, add `HL_WASM64_DEFAULT_MAX_INPUT/OUTPUT` |
| `include/hull/cap/wasm.h` | Widen `max_input`/`max_output` to uint64 in `HlWasmCallOpts`, add `is_memory64` to `HlWasmModule` |
| `src/hull/cap/wasm.c` | Dual ABI dispatch (4-cell vs 8-cell argv), detect Memory64 on load, reject non-AOT Memory64, widen module_malloc handling |
| `src/hull/cap/wasm_data.c` | Update shared heap offset selection (`start_off_mem64` vs `start_off_mem32`) |
| `src/hull/runtime/lua/modules.c` | Accept Lua integers for max_input/max_output (already int64-capable) |
| `src/hull/runtime/js/modules.c` | Accept JS numbers for max_input/max_output |
| `tests/hull/cap/test_wasm.c` | Memory64 detection test, AOT-only enforcement test |
| `tests/fixtures/compute/echo64.wat` | Memory64 echo module (memory type with `i64` flag) |
| `bench/wasm/bench_wasm.c` | Memory64 AOT benchmark if wamrc available |
| `docs/roadmap_wasm_compute.md` | Mark Phase 3 status |
| `CLAUDE.md` | Document Memory64 support |

## Step-by-step

### Step 1: Enable Memory64 in WAMR build

Add to `WAMR_CFLAGS` in Makefile:
```makefile
-DWASM_ENABLE_MEMORY64=1
```

Verify compilation with `make clean && make`. The fast interpreter code has
`#if WASM_ENABLE_MEMORY64 != 0` guards in a few places (dummy variables for
shared heap). These should compile harmlessly. If not, wrap them.

### Step 2: Widen types

In `limits.h`:
```c
#ifndef HL_WASM_MAX_IO_SIZE
#define HL_WASM_MAX_IO_SIZE  ((uint64_t)16 * 1024 * 1024 * 1024)  /* 16 GB */
#endif
#define HL_WASM64_DEFAULT_MAX_IO  (1ULL * 1024 * 1024 * 1024)  /* 1 GB */
```

In `wasm.h`, widen `HlWasmCallOpts`:
```c
uint64_t max_input;
uint64_t max_output;
```

Add to `HlWasmModule`:
```c
int is_memory64;   /* 1 = Memory64 module (i64 addresses) */
```

### Step 3: Detect Memory64 on module load

In `hl_cap_wasm_load()`, after the ABI version probe (which already creates a
temporary instance), also check `is_memory64`:

```c
/* Probe ABI version + Memory64 flag */
uint32_t abi_version = 0;
int detected_memory64 = 0;
{
    wasm_module_inst_t tmp = wasm_runtime_instantiate(...);
    if (tmp) {
        /* ABI version probe ... (existing code) ... */

        /* Memory64 detection */
        WASMMemoryInstance *mem = wasm_get_default_memory(tmp);
        if (mem && mem->is_memory64)
            detected_memory64 = 1;

        wasm_runtime_deinstantiate(tmp);
    }
}
```

Store on the cached module: `cached->is_memory64 = detected_memory64;`

### Step 4: Reject non-AOT Memory64 modules

In `hl_cap_wasm_call_buf()` and `hl_cap_wasm_instance_create()`, after
lazy-loading the module:

```c
if (mod->is_memory64 && !mod->is_aot) {
    if (err_msg) *err_msg = "memory64_requires_aot";
    return HL_WASM_ERR_LOAD;
}
```

Define new error string. This gives a clear message in dev mode: "this module
uses Memory64 and must be AOT-compiled."

### Step 5: Dual ABI call dispatch

In `hl_cap_wasm_call_buf()`, branch on `mod->is_memory64`:

```c
if (mod->is_memory64) {
    /* Memory64 ABI: hull_process(i64, i64, i64, i64) -> i32
     * Each i64 arg = 2 × uint32 cells in argv */
    uint32_t argv[9]; /* 4 × 2 cells + 1 result cell */
    argv[0] = (uint32_t)(wasm_in_ptr);
    argv[1] = (uint32_t)(wasm_in_ptr >> 32);
    argv[2] = (uint32_t)(input_len);
    argv[3] = (uint32_t)((uint64_t)input_len >> 32);
    argv[4] = (uint32_t)(wasm_out_ptr);
    argv[5] = (uint32_t)(wasm_out_ptr >> 32);
    argv[6] = (uint32_t)(max_output);
    argv[7] = (uint32_t)((uint64_t)max_output >> 32);
    if (!wasm_runtime_call_wasm(exec_env, process_fn, 8, argv))
        goto call_failed;
    result = (int32_t)argv[0];
} else {
    /* WASM32 ABI (existing code) */
    assert(wasm_in_ptr <= UINT32_MAX);
    assert(wasm_out_ptr <= UINT32_MAX);
    uint32_t argv[4] = { ... };
    ...
}
```

Same pattern in `hl_cap_wasm_instance_call_buf()`.

### Step 6: host_call for Memory64

The `host_call` import signature stays `(iii)i` — opcode, ptr, len are
semantic values (segment IDs, sizes), not memory addresses. No change needed.

If a future host_call opcode needs to pass memory addresses, it would need
a `(III)i` variant (i64 params). Cross that bridge when needed.

### Step 7: Shared heap Memory64 support

In `wasm_data.c`, `hl_wasm_rebuild_chain()` currently computes `wasm_addr`
using `alloc_size` against `UINT32_MAX`. For Memory64 modules, use
`UINT64_MAX`:

```c
/* Address computation depends on module memory width */
uint64_t addr_max = is_memory64 ? UINT64_MAX : (uint64_t)UINT32_MAX;
uint64_t addr = addr_max + 1;
for (int i = sd->count - 1; i >= 0; i--) {
    addr -= sd->segments[i].alloc_size;
    sd->segments[i].wasm_addr = addr;  /* uint64_t now */
}
```

`HlWasmDataSegment.wasm_addr` widens from `uint32_t` to `uint64_t`.

The DATA_INFO opcode returns `int32_t` which can't hold 64-bit addresses.
For Memory64, add a new sub-opcode or use two calls:
```
host_call(0x02, seg_id, 0) → low 32 bits of address
host_call(0x02, seg_id, 2) → high 32 bits of address
```

### Step 8: wamrc Memory64 AOT compilation

In `build.lua` (hull build), detect Memory64 modules and pass
`--enable-memory64` to wamrc:

```lua
local is_mem64 = detect_memory64(wasm_path)
if is_mem64 then
    wamrc_args = wamrc_args .. " --enable-memory64"
end
```

Detection: check the WASM binary's memory section for the Memory64 flag
(byte 0x04 in the limits flags field).

### Step 9: Test fixture

Create `tests/fixtures/compute/echo64.wat`:
```wat
(module
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))
  (memory (export "memory") i64 1)  ;; Memory64 flag
  (func $hull_process (export "hull_process")
    (param $in_ptr i64) (param $in_len i64)
    (param $out_ptr i64) (param $out_max i64)
    (result i32)
    ;; Same echo logic but with i64 addresses
    ...
  )
)
```

Compile with: `wat2wasm --enable-memory64 echo64.wat`

### Step 10: Unit tests

1. `memory64_detection` — load echo64.wasm, verify `mod->is_memory64 == 1`
2. `memory64_rejects_interpreter` — load non-AOT Memory64 module, verify error
3. `memory64_aot_echo` — if wamrc available, AOT-compile echo64, call, verify
4. `memory64_shared_data` — shared data with Memory64 module (if AOT available)

### Step 11: CLI flag

Add `--wasm-memory64` flag to force-allow interpreter fallback for Memory64
during development (slower but functional). Default: AOT-required.

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| WAMR compile failure with FAST_INTERP+MEMORY64 | High | Build test first. If fails, conditionally enable MEMORY64 only when building AOT modules |
| Performance regression for WASM32 | None | Fast interpreter unchanged for WASM32 |
| ABI confusion (32 vs 64 plugin) | Medium | Clear error messages, `hull_version()` can encode memory width |
| Shared heap address overflow | Low | Widen `wasm_addr` to uint64, test with fixture |
| wamrc doesn't support `--enable-memory64` | Low | Check wamrc version, skip if unavailable |

## Verification

1. `make clean && make` — compiles with MEMORY64=1
2. `make test` — all existing WASM32 tests still pass (zero regression)
3. Memory64 detection unit test passes
4. Non-AOT Memory64 module correctly rejected
5. (If wamrc available) AOT Memory64 echo test passes
6. `make e2e` — all E2E tests still pass

## Non-Goals

- Classic interpreter support in production (AOT is the only path)
- Automatic WASM32→WASM64 module conversion
- Mixed 32/64 module chains (each module is one or the other)
- Heap sizes >4 GB via `HlWasmCallOpts.heap_size` (module declares its own memory limits)
