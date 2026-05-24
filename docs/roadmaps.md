# Hull Roadmaps

This document captures active forward-looking design roadmaps that are too
large for the release checklist in `roadmap.md`. Each section should be
specific enough for implementation work: architecture, integration points,
security constraints, phases, tests, and non-goals.

## Typed Lua Compute Islands

### Goal

Hull should let Lua application modules define small decorated compute
functions that are compiled into WASM and invoked through generated Lua
stubs:

```lua
local compute = require("hull.compute")

@wasm({
  args = { "f64", "f64" },
  ret = "f64",
  memory = "1MB",
  deterministic = true
})
function distance(x, y)
  return sqrt(x*x + y*y)
end

print(distance(3.0, 4.0)) -- transparently calls generated WASM
```

The feature is intentionally narrow: Numba-like typed compute islands, not a
general Lua-to-WASM compiler. Lua and JS remain orchestration languages. WASM
remains the safe compute layer. The compiler accepts a restricted, typed Lua
subset, lowers it into a Hull-owned internal IR, emits WASM, caches artifacts,
loads them through the existing WAMR compute runtime, and replaces annotated
functions with generated Lua stubs.

The production invariant is important: runtime app execution must not compile
new code. `hull dev` may compile during reload before sandboxed app execution;
`hull build` must compile, optionally AOT, embed, and sign the generated
artifacts. A built application loads only sealed VFS entries covered by
`package.sig`.

### Design Constraints

- Preserve Hull's capability model: generated WASM has no ambient IO, no WASI,
  no filesystem, no network, no environment, no time, no random, and no access
  to Lua state.
- Reuse the existing `hull.compute` runtime: WAMR module cache, gas metering,
  memory caps, stack caps, input/output limits, instance pooling, async worker
  dispatch, VFS loading, AOT lookup, Memory64 handling, and audit boundaries.
- Keep compilation deterministic and local: no package manager, no remote
  compiler, no runtime plugin installation, no JIT.
- Make the compiler reject unsupported source early and loudly. Silent fallback
  to Lua execution would weaken both performance expectations and security
  review.
- Keep the first version scalar and explicit. Typed arrays, slices, and
  buffer-backed kernels are follow-up work.
- Treat `@wasm` syntax as source-transform syntax. Stock Lua 5.4 does not parse
  decorators, so Hull must transform annotated source before the normal Lua
  parser sees it. A valid-Lua API should ship first.

### User-Facing API

Phase 1 should use valid Lua syntax:

```lua
local compute = require("hull.compute")

local distance = compute.wasm({
  args = { "f64", "f64" },
  ret = "f64",
  memory = "1MB",
  deterministic = true,
}, function(x, y)
  return sqrt(x*x + y*y)
end)
```

The generated stub behaves like a normal Lua function:

```lua
local d = distance(3.0, 4.0)
```

Phase 2 may add decorator syntax through the source transformer:

```lua
@wasm({ args = { "f64", "f64" }, ret = "f64" })
function distance(x, y)
  return sqrt(x*x + y*y)
end
```

The manifest remains explicit:

```lua
app.manifest({
  modules = { "hull/compute@1" },
  compute = true,
})
```

Optional later manifest policy:

```lua
app.manifest({
  modules = { "hull/compute@1" },
  compute = true,
  compute_lua = {
    max_memory = "16MB",
    deterministic_default = true,
  },
})
```

### High-Level Architecture

```
Lua source
  |
  | 1. discover compute.wasm(...) and/or @wasm blocks
  v
Compute island records
  |
  | 2. parse restricted function body
  v
Typed AST
  |
  | 3. type check and capability-free validation
  v
Hull Compute IR
  |
  | 4. optimize and lower
  v
WASM module bytes
  |
  | 5. cache, AOT, embed in VFS
  v
Generated Lua stub
  |
  | 6. compute.call("__generated/name_hash", frame, opts)
  v
Existing WAMR runtime
```

New subsystem boundary:

- `src/hull/compute_lua/` contains compiler frontend, IR, optimizer, WASM
  emitter, cache keying, and diagnostics.
- `src/hull/runtime/lua/mod_compute.c` exposes the user-facing registration API
  and generated-call helpers, but does not compile production code at runtime.
- `src/hull/build_assets.c` / `src/hull/commands/build.c` run the compiler as
  part of build asset preparation.
- `src/hull/commands/dev.c` runs the compiler during reload and writes dev
  artifacts under `.hull/compute-cache/`.
- `src/hull/agent/validate.c`, `src/hull/agent/compute.c`, and
  `src/hull/agent/manifest.c` expose diagnostics and generated module metadata.

### Implementation Guide

#### 1. Source Discovery

Add a discovery pass that scans app Lua files before normal runtime load.
Inputs:

- `app.lua`
- first-party Lua modules embedded with the app
- any Lua files that `hull build` already treats as application code

Outputs:

```c
typedef struct {
    char source_path[HL_PATH_MAX];
    uint32_t start_line;
    uint32_t start_col;
    uint32_t end_line;
    uint32_t end_col;
    char lua_name[128];
    char module_name[256];
    char canonical_source_hash[65];
    HlComputeLuaSignature sig;
    HlComputeLuaLimits limits;
    int deterministic;
} HlComputeLuaIsland;
```

Discovery must support two syntaxes:

- valid Lua: `compute.wasm(opts, function(...) ... end)`
- transformed Lua: `@wasm(opts) function name(...) ... end`

For v1, only implement the valid-Lua form. The decorator syntax requires a
preprocessor that rewrites source before Lua parsing, so it should wait until
the compiler, cache, diagnostics, and stubs are stable.

Discovery should not use ad hoc string matching for function boundaries beyond
an initial candidate scan. Use Lua's lexer/parser where possible, or introduce
a small parser for the accepted subset. The compiler must know exact spans for
good diagnostics and stable cache keys.

#### 2. Annotation Parsing

Accepted annotation keys for v1:

| Key | Type | Required | Notes |
|-----|------|----------|-------|
| `args` | array of type strings | yes | `i32`, `i64`, `f32`, `f64`, `bool` |
| `ret` | type string or `"void"` | yes | scalar only in v1 |
| `memory` | size string or integer bytes | no | clamped by manifest/runtime ceiling |
| `stack` | size string or integer bytes | no | clamped by manifest/runtime ceiling |
| `gas` | integer | no | clamped by manifest/runtime ceiling |
| `deterministic` | boolean | no | default true for v1 |
| `name` | string | no | override generated module base name |

Reject unknown keys unless an `experimental = true` escape hatch is explicitly
set for development builds. Production builds should fail closed on unknown
annotation keys.

Canonicalize annotations into sorted JSON for cache hashing.

#### 3. Restricted Lua Subset

Supported v1 constructs:

- numeric literals
- boolean literals
- local variables with explicit inferred scalar types
- function arguments from `args`
- `return expr`
- arithmetic: `+`, `-`, `*`, `/`, unary `-`
- comparisons: `<`, `<=`, `>`, `>=`, `==`, `~=`
- boolean operators: `and`, `or`, `not`
- `if` / `elseif` / `else`
- numeric `for` loops where bounds and step are scalar numeric expressions
- `while` loops if gas metering is always present
- calls to allowlisted deterministic math intrinsics

Rejected v1 constructs:

- tables, metatables, userdata, strings, varargs
- closures or nested function declarations
- coroutines
- `require`, `dofile`, `load`, `loadstring`
- globals other than allowlisted intrinsics
- mutation of upvalues or globals
- Hull capability modules (`db`, `fs`, `http`, `env`, `time`, `crypto`, etc.)
- IO of any kind
- non-deterministic functions

Math intrinsic allowlist for v1:

| Lua name | WASM lowering |
|----------|---------------|
| `abs` | instruction or helper |
| `sqrt` | `f32.sqrt` / `f64.sqrt` |
| `floor` | `f32.floor` / `f64.floor` |
| `ceil` | `f32.ceil` / `f64.ceil` |
| `min` | compare/select |
| `max` | compare/select |

Trigonometry can be deferred unless Hull chooses to ship deterministic libm
helpers inside generated modules. Host libm imports should not be used for v1.

#### 4. Type System

Scalar types:

- `bool`
- `i32`
- `i64`
- `f32`
- `f64`

Rules:

- Function argument and return types come from the annotation.
- Locals are inferred from their initializer.
- Numeric operators require compatible numeric operands.
- Widening is explicit in v1. Do not silently coerce `i32` to `f64`.
- Add explicit conversion intrinsics later, e.g. `f64(x)` or `compute.cast`.
- Comparisons return `bool`.
- `if` branches must converge on compatible variable types.
- Loop induction variables have a declared or inferred integer type.

Diagnostics should name the source span and the required fix:

```
app.lua:18:12: compute island 'distance': unsupported global 'math'
  use allowlisted intrinsic 'sqrt(x)' instead of 'math.sqrt(x)'
```

#### 5. Internal IR

Use a Hull-owned IR rather than emitting WASM directly from Lua AST. Suggested
shape:

```c
typedef enum {
    HL_CIR_I32, HL_CIR_I64, HL_CIR_F32, HL_CIR_F64, HL_CIR_BOOL
} HlCirType;

typedef enum {
    HL_CIR_CONST,
    HL_CIR_ARG,
    HL_CIR_LOCAL_GET,
    HL_CIR_LOCAL_SET,
    HL_CIR_ADD,
    HL_CIR_SUB,
    HL_CIR_MUL,
    HL_CIR_DIV,
    HL_CIR_NEG,
    HL_CIR_CMP,
    HL_CIR_SELECT,
    HL_CIR_CALL_INTRINSIC,
    HL_CIR_BLOCK,
    HL_CIR_LOOP,
    HL_CIR_BR_IF,
    HL_CIR_RETURN,
} HlCirOp;
```

The IR should be:

- typed
- structured enough to map cleanly to WASM blocks and loops
- independent of Lua runtime internals
- serializable in debug builds for `hull agent compute --ir`
- validated before emission

Initial optimizations:

- constant folding
- dead local removal
- simple algebraic cleanup
- intrinsic lowering

Avoid clever optimization in v1. Correct rejection and stable codegen matter
more.

#### 6. WASM Emission

Use the existing compute ABI first:

```c
int32_t hull_process(int32_t input_ptr, int32_t input_len,
                     int32_t output_ptr, int32_t output_max);
int32_t hull_version(void);
```

The generated Lua stub packs scalar args into a little-endian frame:

```
u8  abi_version = 1
u8  argc
u16 flags
arg payloads...
```

Return frame:

```
u8  abi_version = 1
u8  status
u16 flags
ret payload...
```

This path reuses `compute.call` without adding a second WAMR invocation API.
Later, add a direct scalar-export fast path if benchmarks show frame overhead
dominates.

Generated modules should import only `host_call` if needed. Pure scalar v1
modules should import nothing.

WASM validation requirements:

- valid magic/version
- exactly one exported `hull_process`
- optional `hull_version` returns current ABI
- no WASI imports
- no unknown imports
- no memory declaration above annotation/runtime ceilings
- no tables unless required by future language features

#### 7. Stub Generation

Generated Lua stubs should be explicit and boring. Example internal expansion:

```lua
local function distance(x, y)
  local __frame = __hull_compute_pack_f64_f64(x, y)
  local __out, __err = compute.call(
    "__generated/app/distance_4f8c2d1a",
    __frame,
    { heap = 1048576, gas = 1000000, max_input = 64, max_output = 32 }
  )
  if __err then error("compute distance failed: " .. __err, 2) end
  return __hull_compute_unpack_f64(__out)
end
```

Packing helpers can live in the `hull.compute` Lua module or as C helpers
exposed by `mod_compute.c`. Prefer C helpers for exact endian handling and
lower allocation overhead.

Stub errors should preserve Lua call-site behavior:

- wrong argument count: Lua-style argument error
- wrong argument type: Lua-style type error
- WASM failure: `error("compute <name> failed: <err>", 2)`
- unsupported compile feature: build/validate error, not runtime fallback

#### 8. Caching

Cache key inputs:

- canonical island source
- canonical annotation JSON
- compiler version
- Hull compute ABI version
- target WASM feature set
- optimization level
- relevant manifest ceilings

Dev cache path:

```
.hull/compute-cache/<hash>.wasm
.hull/compute-cache/<hash>.json
```

Build VFS path:

```
compute/__generated/<lua-module>/<function>_<hash>.wasm
compute/__generated/<lua-module>/<function>_<hash>.aot.<arch>
```

The metadata JSON should include source path, line span, signature, limits,
compiler version, and warnings. `hull agent compute` can surface this.

Cache invalidation must be content-based. Timestamps are acceptable only as a
fast path; hash mismatch is authoritative.

#### 9. Build Integration

`hull build` pipeline:

1. Load app file list.
2. Discover compute islands.
3. Compile each island to WASM or reuse cache by hash.
4. Run WAMR load validation.
5. Run `wamrc` when available, matching current compute-module AOT behavior.
6. Embed generated `.wasm` and `.aot.<arch>` into VFS.
7. Generate transformed Lua source or sidecar stub module.
8. Include generated metadata in signature input.
9. Fail build on any compiler diagnostic with severity error.

Production runtime:

- never invokes the compiler
- loads generated modules only from VFS
- enforces existing manifest/module gates
- uses existing WAMR runtime limits

`hull dev` pipeline:

1. Watch Lua source.
2. On reload, discover and compile islands into `.hull/compute-cache/`.
3. Expose generated modules to runtime through the existing filesystem/VFS
   fallback path.
4. Write structured errors to `.hull/last_error.json`.
5. Let `hull agent errors` and `hull agent validate` report compiler messages.

#### 10. Agent and CLI Surface

New or extended commands:

| Command | Behavior |
|---------|----------|
| `hull agent validate <file>` | Includes compute-island syntax/type diagnostics |
| `hull agent compute <app>` | Lists hand-authored and generated modules |
| `hull agent compute --generated <app>` | Shows source spans, signatures, cache status |
| `hull agent compute-ir <name> <app>` | Debug-only IR dump for one generated island |
| `hull compute check-generated <app>` | Compiles and validates generated modules without full build |

JSON shape for generated modules:

```json
{
  "name": "__generated/app/distance_4f8c2d1a",
  "source": "app.lua",
  "line": 12,
  "args": ["f64", "f64"],
  "ret": "f64",
  "deterministic": true,
  "wasm_size": 314,
  "has_aot": true,
  "cache_hit": true
}
```

#### 11. Security Review Checklist

- Generated code is included in `package.sig`.
- Built apps cannot compile source into WASM at runtime.
- The compiler is not available from app code as a capability.
- Generated modules have no WASI imports.
- Unknown imports fail validation.
- Manifest still requires `hull/compute@1` and `compute = true`.
- Annotation limits are clamped by manifest/runtime ceilings.
- Gas is always applied to generated modules.
- Memory and stack are always bounded.
- No unsupported Lua construct falls back to interpreted Lua inside the island.
- The source transformer cannot rewrite non-island code in behavior-changing
  ways.
- Diagnostics do not leak host filesystem paths beyond normal build output.
- Dev cache files are not trusted during production build unless their content
  hash matches source and compiler metadata.

#### 12. Tests

Unit tests:

- annotation parser accepts valid forms and rejects unknown keys
- type checker accepts scalar arithmetic
- type checker rejects globals, tables, closures, strings, and capability calls
- IR validator catches malformed blocks and type mismatches
- WASM emitter produces loadable modules
- frame pack/unpack helpers roundtrip every scalar type
- cache key changes when source, annotation, compiler version, or ABI changes

Integration tests:

- `hull agent validate` reports precise compute diagnostics
- `hull dev --agent` compiles on reload and serves a route calling a generated
  function
- `hull build` embeds generated modules and the built app runs without source
  compiler access
- `hull agent compute` lists generated modules
- gas exhaustion returns the existing compute error shape
- memory annotation above ceiling is clamped or rejected according to policy
- deterministic math output is stable across interpreter and AOT

Security tests:

- generated module attempting WASI import fails validation
- unsupported global `db` / `fs` / `http` inside island fails compile
- tampered generated `.wasm` in dev cache is ignored on hash mismatch
- built app cannot create a new generated module at runtime
- decorator transformer rejects malformed spans and nested decorators

Benchmark tests:

- scalar function call overhead vs plain Lua
- batched scalar calls vs plain Lua
- generated WASM interpreter vs AOT
- frame ABI overhead for small functions
- module load cache hit behavior

### Roadmap

#### Phase 0: Design Lock

- Finalize v1 syntax as `compute.wasm(opts, function(...) ... end)`.
- Defer `@wasm` decorator syntax until the source transformer is proven.
- Finalize scalar type names and annotation schema.
- Decide whether unknown annotation keys always fail or fail only in production.
- Add this feature to `docs/stability.md` as experimental until v1 is shipped.

#### Phase 1: Compiler Skeleton

- Add `src/hull/compute_lua/`.
- Implement annotation discovery for valid-Lua form.
- Implement restricted AST for scalar expressions and `return`.
- Implement type checker for scalar arithmetic.
- Implement IR structures and validation.
- Add unit tests for parser/type checker/IR validator.

#### Phase 2: WASM Emission Through Existing ABI

- Emit minimal WASM modules exporting `hull_process` and `hull_version`.
- Add scalar frame pack/unpack helpers in C.
- Generate Lua stubs that call `compute.call`.
- Validate generated modules through existing `hl_cap_wasm_load`.
- Add tests for `distance(3.0, 4.0) == 5.0`.

#### Phase 3: Dev and Build Integration

- Wire compiler into `hull dev` reload.
- Store dev artifacts in `.hull/compute-cache/`.
- Wire compiler into `hull build`.
- Embed generated modules under `compute/__generated/`.
- Include generated metadata in VFS/signature inputs.
- Ensure production runtime has no source compiler path.

#### Phase 4: Diagnostics and Agent Surface

- Extend `hull agent validate`.
- Extend `hull agent compute`.
- Add debug IR dump behind an explicit debug/development flag.
- Ensure structured errors include source path, line, column, island name, and
  fix-oriented messages.

#### Phase 5: Control Flow and Intrinsics

- Add `if` / `elseif` / `else`.
- Add numeric `for` loops.
- Add `while` loops only after gas behavior is covered by tests.
- Add deterministic math intrinsic allowlist.
- Add constant folding and dead-local cleanup.

#### Phase 6: Decorator Syntax

- Add pre-Lua source transformer for `@wasm(...) function ... end`.
- Preserve source maps for diagnostics.
- Ensure transformed source is what Lua runtime loads.
- Reject malformed decorators before Lua parsing.
- Add e2e tests for decorator syntax.

#### Phase 7: Typed Buffers

- Add explicit typed buffer parameters backed by `WasmBuffer` / `MappedBuffer`.
- Support bounds-checked load/store in IR.
- Add array length arguments explicitly; do not infer hidden ambient state.
- Keep buffer mutability explicit in annotation.

Example future annotation:

```lua
local dot = compute.wasm({
  args = {
    { name = "a", type = "buffer<f32>", readonly = true },
    { name = "b", type = "buffer<f32>", readonly = true },
    { name = "n", type = "i32" },
  },
  ret = "f32",
}, function(a, b, n)
  local acc = 0.0
  for i = 0, n - 1 do
    acc = acc + a[i] * b[i]
  end
  return acc
end)
```

#### Phase 8: Direct Scalar Export Fast Path

- Benchmark frame overhead first.
- If needed, emit WASM functions with direct scalar exports.
- Add `hl_cap_wasm_call_typed` to call typed exports without frame packing.
- Keep `hull_process` ABI as the compatibility path.

### Non-Goals

- Full Lua-to-WASM compilation.
- JS-to-WASM compilation in this subsystem.
- Runtime production compilation.
- Access to Hull capabilities from generated WASM.
- WASI support.
- Host libm imports in v1.
- Silent fallback to interpreted Lua for unsupported compute-island code.
- General package management or downloading compiler components.

### Open Questions

- Should v1 allow `math.sqrt(x)` as syntax and rewrite it to `sqrt(x)`, or only
  allow bare allowlisted intrinsics?
- Should memory annotations above the manifest ceiling fail hard or clamp with a
  warning?
- Should generated module names include the source module path or only a hash?
- Should dev cache artifacts be human-visible under `.hull/compute-cache/` or
  hidden behind an agent command only?
- How much of the Lua parser should be reused versus implementing a small
  dedicated parser for the accepted subset?
- Should `deterministic = false` exist in v1, or should non-deterministic
  islands be rejected entirely until there is a concrete use case?
