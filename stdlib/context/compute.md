<!-- minimal -->
## Compute (WASM)

Pure-function CPU-bound work runs in WAMR-sandboxed WASM modules. No I/O,
gas-metered, ~1.2× native speed under AOT.

```bash
hull compute new score      # scaffold compute/score/{score.c, hull_compute.h, test_fixtures.json}
hull compute build score    # source → compute/score.wasm via clang wasm32
hull compute test score     # run JSON fixtures through compute.call()
hull build .                # hull build auto-rebuilds stale .c, then AOT-compiles, then embeds
```

```lua
local out, err = compute.call("score", input_bytes, {
    gas = 10000000, heap = 256 * 1024,
})
```

Module exports: `hull_process(in, in_len, out, out_max) -> bytes_written`.
Single import: `host_call(op, ptr, len)`.

<!-- compact -->
## Authoring lifecycle

| Step | Command | Result |
|------|---------|--------|
| Scaffold | `hull compute new <name>` | `compute/<name>/{<name>.c, hull_compute.h, test_fixtures.json}` |
| Compile | `hull compute build <name>` | `compute/<name>.wasm` (clang wasm32, -O2 -flto, -nostdlib) |
| Validate | `hull compute check <name>` | Loads in WAMR + smoke test |
| Test | `hull compute test <name>` | Runs `test_fixtures.json` via tempdir `hull test` |
| Embed | `hull build .` | Auto-rebuilds stale `.c`, AOT-compiles if `wamrc` present, embeds |
| Refresh ABI | `hull compute refresh-header <name>` | Restores per-module `hull_compute.h` from the embedded canonical version |

Idle modules cost nothing. A `compute/<name>/` directory without a matching
`.wasm` will fail `compute.call` at runtime; `hull build` will compile it
automatically. Pass `--no-build-compute` to skip the auto-rebuild
(hermetic-CI builds with pre-committed `.wasm` artifacts).

## ABI surface (`hull_compute.h`)

The header is written into each module directory by `hull compute new` and
is freestanding (no libc dependency):

| API | Purpose |
|-----|---------|
| `HULL_EXPORT`, `HULL_VERSION_EXPORT` | Visibility macros - required on `hull_process` + `hull_version` |
| `HULL_OK`, `HULL_ERR_OUTPUT`, `HULL_ERR_INPUT`, `HULL_ERR_INTERNAL` | Standard return codes |
| `host_call(op, ptr, len)` | The single host import (LOG=0x01, DATA_INFO=0x02, STREAM=0x03, CALLBACK=0x10) |
| `hull_log(msg, len)` | Wrapper around `host_call(LOG, ...)` |
| `hull_segment_count()`, `hull_segment_addr(id)`, `hull_segment_size(id)` | Read shared data segments loaded via `compute.segment` |
| `hull_memcpy`, `hull_memset`, `hull_memcmp`, `hull_strlen` | Minimal libc replacements |
| `hull_alloc(n)`, `hull_alloc_reset()` | 64 KiB bump allocator scoped to one call |

## Calling from Lua / JS

```lua
-- Synchronous (blocks; gas-limited)
local out, err = compute.call("score", input, {
    max_input  = 64 * 1024,
    max_output = 64 * 1024,
    gas        = 10000000,
    heap       = 256 * 1024,
})

-- Async (yields to event loop; dispatches to worker pool)
local r = compute.async.call("score", input, opts)

-- Persistent instance (linear memory retained across calls)
local m = compute.instance("scorer", { heap = 4 * 1024 * 1024 })
m:call(query)
m:close()
```

```javascript
const out = compute.call("score", inputBytes, { maxInput: 64*1024, gas: 10_000_000 });
const buf = await compute.async.call("score", inputBytes, opts);
```

## Inspecting modules

```bash
hull agent compute <app_dir>           # list modules + AOT presence + sizes
hull agent compute-call <name> <file>  # one-shot invocation for testing
hull agent deploy <app_dir>            # per-module status in deploy readiness
```

`hull agent deploy` reports `compute_modules` with
`{name, wasm_size, has_aot, has_source, source_stale}` plus a
recommendation when any `.c` is newer than its `.wasm`.

<!-- full -->
## Shared data segments

Multi-GB read-only datasets can be loaded once and exposed to every
instance of a module. Up to 16 named segments per module.

```lua
compute.segment("router", "graph", fs.mmap("graph.bin"))
local out = compute.call("router", query)
```

```c
/* Inside the module: */
void *graph = (void *)(size_t)host_call(0x02, 0, 0);  /* segment 0 address */
int32_t graph_size = host_call(0x02, 0, 1);           /* segment 0 size */
```

Segments are page-aligned mmap regions exposed via WAMR shared heaps.
Reads are concurrent across worker threads. Adding or removing a segment
drains the instance pool and rebuilds it transparently.

## Streaming I/O

```lua
compute.stream("transform", { file = "in.csv" }, { file = "out.json" },
               { chunk_size = 65536 })
```

The module exports `hull_process_chunk(in, in_len, out, out_max, is_last)`
instead of `hull_process` and queries chunk metadata via
`host_call(0x03, sub_op, 0)` where sub_op selects `is_first`, `is_last`,
or `chunk_index`. State persists across chunks via a single persistent
instance under the hood.

## Toolchain

`hull compute build` (and the auto-rebuild step inside `hull build`)
looks for clang in this order:

1. `/opt/homebrew/opt/llvm@18/bin/clang` (macOS)
2. `/opt/homebrew/opt/llvm/bin/clang` (macOS)
3. `/usr/local/opt/llvm@18/bin/clang` (older macOS)
4. `/usr/local/opt/llvm/bin/clang`
5. `clang` in `PATH`

AOT compilation requires `wamrc` (sibling-to-hull or in PATH). Without
`wamrc`, modules run via the fast interpreter - Hull emits a runtime
warning (suppress with `HULL_QUIET_AOT=1`). `hull doctor` reports the
full toolchain status (`wasm_enabled`, `wamrc`, `clang`, `wasm_ld`,
`aot_ready`).

## Limits

| Limit | Default | Max | CLI override |
|-------|---------|-----|--------------|
| Input size | 1 MiB | 256 MiB | `--wasm-max-input` |
| Output size | 1 MiB | 256 MiB | `--wasm-max-output` |
| Heap | 2 MiB | ~4 GiB (16 GiB with Memory64) | `--wasm-heap` |
| Stack | 64 KiB | 8 MiB | `--wasm-stack` |
| Gas | 100M | 100B | `--wasm-gas` |

Per-call opts override the CLI/manifest ceilings, which override the
compile-time defaults.

## Languages

C is the only language with `hull compute new` scaffolding today. Modules
in any language work as long as they export `hull_process` and
`hull_version`. Verified to work:

- **C** (`clang --target=wasm32-unknown-unknown -nostdlib`)
- **Rust** (`wasm32-unknown-unknown` target, `#[no_mangle] extern "C"`)
- **Zig** (`wasm32-freestanding`)
- **TinyGo**, **AssemblyScript** - also supported

`--lang=rust` scaffolding is on the roadmap.

## See also

- `docs/wamr_architecture.md` - full WAMR design (ABI, gas metering,
  pooling, segments, streaming, AOT, Memory64)
- `README.md#authoring-compute-modules` - user-facing developer guide
- `examples/compute/` - eight working modules including `vector_ops`,
  `hash`, `sort`, `text`, `scoring`, `json_extract`
