<!-- minimal -->
## GPU compute (WGSL shaders)

For large parallel workloads (ML inference, image kernels, embeddings),
Hull dispatches WGSL compute shaders via wgpu-native (Vulkan / Metal /
DX12). Compile a shader once, dispatch many times. Async-by-default -
the event loop continues serving requests while the GPU works.

Requires build with `HL_ENABLE_GPU=1` (not on by default). Check with
`hull doctor` → "Compute (GPU)" row.

```lua
-- app.lua
app.manifest({
    gpu = true,
    modules = { "hull/gpu@1", "hull/http-server@1" },
})

-- Shaders live in shaders/ as .wgsl files, auto-embedded by hull build.
gpu.load("normalize")    -- reads shaders/normalize.wgsl, caches

app.get("/normalize", function(req, res)
    local input = req:body()         -- raw bytes
    local out = gpu.dispatch("normalize", {
        buffers = {
            { data = input,     usage = "read" },         -- binding 0
            { size = #input,    usage = "readwrite" },    -- binding 1 (output)
        },
        workgroups = { x = math.ceil(#input / 64 / 4) },
        output = 2,                  -- read back buffer index 2 (1-indexed in Lua)
    })
    return res:json({ result = out })
end)
```

### When to use GPU vs WASM AOT

| Workload | Use |
|---|---|
| < 16K parallel ops | WASM AOT (`compute.call`) - sub-millisecond, no GPU overhead |
| > 16K parallel ops, regular shape | GPU - crossover ~16K vectors on Apple M1 |
| Sequential, branchy, small input | WASM AOT |
| Pixel/vertex kernels, matmul, conv | GPU |

GPU dispatch has ~2.6 ms constant overhead (submit + poll). Below
~16K parallel ops, WASM AOT wins.

<!-- compact -->
## Shader file layout

```
myapp/
  app.lua
  shaders/
    normalize.wgsl
    score.wgsl
    matmul.wgsl
```

`gpu.load("normalize")` reads `shaders/normalize.wgsl` (or embedded
VFS in built binaries) and caches the compiled pipeline. Equivalent
to `gpu.compile("normalize", file_contents)`. `hull build .`
auto-embeds every `shaders/*.wgsl`.

### Minimal WGSL example

```wgsl
// shaders/normalize.wgsl
@group(0) @binding(0) var<storage, read> input: array<f32>;
@group(0) @binding(1) var<storage, read_write> output: array<f32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if (i >= arrayLength(&input)) { return; }
    output[i] = input[i] / 255.0;
}
```

Binding layout convention: uniforms at `@binding(0)` (if any), then
storage buffers in declared order. Sampled textures pair into two
bindings (view + sampler); storage textures use one. See full doc
section for textures.

## Dispatch options

```lua
gpu.dispatch("name", {
    uniforms = packed_binary,        -- 16-byte-aligned, binding 0
    buffers = {
        { data = bytes,    usage = "read" },           -- inline upload
        { name = "vectors", usage = "read" },          -- named persistent
        { size = N, usage = "readwrite" },             -- output, allocated empty
    },
    textures = {                     -- optional
        { name = "input" },                            -- sampled (paired bindings)
        { name = "output", storage = true },           -- storage (single binding)
    },
    workgroups = { x = 64, y = 1, z = 1 },
    output = 3,                      -- 1-indexed buffer to read back (Lua)
    output_texture = 2,              -- 1-indexed texture readback
    device = -1,                     -- -1 = default device
})
```

`output = false` skips readback (fire-and-forget - persistent buffers
mutate in place, dispatch returns `true`).

## Persistent buffers

```lua
gpu.buffer("vectors", data)          -- create or replace
gpu.buffer("vectors", nil)           -- destroy
local out = gpu.buffer_read("vectors")
gpu.buffer_copy("src", "dst", { size = 1024 })   -- GPU-side, no CPU roundtrip
```

Named buffers persist across dispatches - load weights / indexes
once, reference them by name in `buffers`. `gpu.buffer_copy` runs on
the GPU; no CPU roundtrip.

## Pipelines (multi-stage)

```lua
local out = gpu.pipeline({
    { shader = "normalize", buffers = {{ name = "data", data = input }},
      workgroups = { x = 64 } },
    { shader = "score",     buffers = {{ name = "data" },
                                       { name = "results", size = N*4 }},
      uniforms = params, workgroups = { x = 64 } },
    { shader = "top_k",     buffers = {{ name = "results" }},
      workgroups = { x = 1 } },
}, {
    outputs = { { stage = 3, buffer = 1 } },
})
```

Single command-buffer submission, single poll, single readback. Named
buffers shared across stages. Use over individual `dispatch` calls
when you'd otherwise chain 2+ with no host work in between.

## Async dispatch

```lua
local out = gpu.async.dispatch("score", opts)     -- yields to event loop
local out = gpu.async.pipeline(stages, opts)
```

Submits to thread pool, yields the coroutine (Lua) or returns Promise
(JS). Other requests are served while the GPU runs. Deep-copies all
buffer data before submission for thread safety.

## Inspecting GPU state

```bash
hull agent gpu [app_dir]      # devices + shader inventory + readiness
hull agent overview [app_dir] # includes gpu_shaders count
```

<!-- full -->
## Zero-copy from mmap

```lua
app.manifest({
    gpu = true,
    fs = { read = {"embeddings.bin"} },
})

local mapped = fs.mmap("embeddings.bin")
gpu.buffer("vectors", mapped)        -- pointer → wgpuQueueWriteBuffer directly
mapped:close()
```

`gpu.buffer`, `gpu.dispatch` buffer data, and `gpu.pipeline` buffer
data all accept `MappedBuffer` (from `fs.mmap`) and `WasmBuffer`
(from `compute.call(..., {buffer = true})`) - no string round-trip.
See `compute.md` for the unified buffer protocol.

## Textures

```lua
gpu.texture("input", img)                              -- from HlImage
gpu.texture("scratch", nil, { width=512, height=512,
                              format="rgba8", storage=true })
gpu.texture("input", nil)                              -- destroy
local out_img = gpu.texture_read("scratch")
```

Sampled textures bind to TWO slots (view + sampler), storage textures
to ONE. Convention: uniforms → buffers → sampled textures → storage
textures.

```wgsl
@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var input_sampler: sampler;
@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let uv = vec2<f32>(f32(gid.x) / 512.0, f32(gid.y) / 512.0);
    let sample = textureSampleLevel(input_tex, input_sampler, uv, 0.0);
    textureStore(output_tex, vec2<i32>(gid.xy), sample * 1.2);
}
```

## Devices

```lua
gpu.devices()  -- [{id=0, name="Apple M1 Max"}, ...]
gpu.dispatch("shader", { device = 1, ... })   -- pick non-default
```

Default device is index 0. Multi-GPU is supported - pass `device`
on every dispatch / pipeline / buffer call if you want explicit
placement.

## Sandbox

When `manifest.gpu = true`:
- macOS: allows `iokit-open` and `com.apple.MTLCompilerService`
  mach-lookup
- Linux: unveils `/dev/dri` (rw) and `/proc/self` (r)

Apps without `gpu = true` in their manifest have NO access to the
`gpu` global - fail at module-load time.

## Performance reference (Apple M1 Max, cosine similarity, 128-dim)

| Vectors | Native C | WASM AOT | GPU | GPU vs AOT |
|---|---|---|---|---|
| 64    | 7 µs     | 7 µs     | 2,630 µs | 0.0× |
| 1K    | 118 µs   | 108 µs   | 2,630 µs | 0.0× |
| 16K   | 1,830 µs | 2,534 µs | 2,629 µs | 1.0× |
| 64K   | 7,270 µs | 10,969 µs| 2,653 µs | **4.1×** |

GPU has ~2.6 ms constant overhead. Use AOT below ~16K parallel ops;
use GPU above.

## Timeout

Default 5s per dispatch (configurable at compile time via
`HL_GPU_TIMEOUT_MS`). Times out → `HL_GPU_ERR_TIMEOUT`. Applies to
`dispatch`, `pipeline`, `buffer_copy`.

## Build

```bash
make fetch-wgpu                                   # one-time vendor download
make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu     # build hull with GPU
make bench-gpu HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu  # benchmark
```

Not compatible with cosmocc (wgpu-native is platform-specific). For
GPU compute on a portable binary, build native-only.

## See also

- `hull agent context --task=compute` - WASM compute (`compute.call`)
- `hull agent context --task=tools` - `hull tools install` (wamrc for AOT)
- `docs/architecture.md` - full GPU subsystem design
- `examples/` - search for `gpu.dispatch` for working examples
