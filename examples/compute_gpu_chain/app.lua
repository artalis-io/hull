-- WASM → GPU chaining example — zero-copy data flow
--
-- Demonstrates the unified buffer protocol: WASM output passes directly
-- to GPU input without copying through Lua strings.
--
-- Pipeline:
--   1. WASM (compute.call) preprocesses raw data → WasmBuffer
--   2. GPU (gpu.dispatch) processes in parallel → result
--
-- Run: hull examples/compute_gpu_chain/app.lua
--      (requires: make HL_ENABLE_GPU=1 && WASM compute)
--
-- Test:
--   curl localhost:3000/health
--   curl -X POST localhost:3000/chain -d '{"values":[1,2,3,4,5,6,7,8]}'

app.manifest({ gpu = true, compute = true })

-- GPU shader: double each u32
gpu.compile("double", [[
@group(0) @binding(0) var<storage, read_write> data: array<u32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    data[id.x] = data[id.x] * 2u;
}
]])

app.get("/health", function(req, res)
    res:json({
        ok = true,
        gpu = gpu.available(),
        wasm = compute.available(),
        devices = gpu.devices(),
    })
end)

-- POST /chain — WASM preprocess → GPU compute
-- Body: { "values": [1, 2, 3, ...] }
app.post("/chain", function(req, res)
    local body = json.decode(req.body)
    if not body or not body.values then
        res:status(400):json({ error = "need values array" })
        return
    end

    -- Step 1: Pack values as u32 binary (application-level preprocessing)
    local parts = {}
    for _, v in ipairs(body.values) do
        parts[#parts + 1] = string.pack("I4", math.floor(v))
    end
    local packed = table.concat(parts)
    local count = #body.values

    -- Step 2: WASM echo (passes through — demonstrates WasmBuffer output)
    -- In a real app, WASM would do CPU-intensive preprocessing:
    -- normalization, feature extraction, encoding, etc.
    local wasm_out, wasm_err = compute.call("echo", packed, { buffer = true })
    if wasm_err then
        res:status(500):json({ error = "wasm: " .. wasm_err })
        return
    end

    -- Step 3: GPU dispatch — pass WasmBuffer directly (zero-copy!)
    -- The WasmBuffer's data pointer goes straight to wgpuQueueWriteBuffer
    -- without copying through a Lua string.
    local gpu_out, gpu_err = gpu.dispatch("double", {
        buffers = {{ data = wasm_out, size = count * 4 }},
        workgroups = { x = math.ceil(count / 64) },
        output = 1,
    })
    if gpu_err then
        res:status(500):json({ error = "gpu: " .. gpu_err })
        return
    end

    -- Step 4: Unpack results
    local results = {}
    for i = 1, count do
        results[i] = string.unpack("I4", gpu_out, (i - 1) * 4 + 1)
    end

    res:json({
        input = body.values,
        output = results,
        pipeline = "wasm_preprocess → gpu_parallel",
    })
end)

-- POST /chain-persistent — WASM → persistent GPU buffer → dispatch
-- Demonstrates keeping WASM output on GPU across requests
app.post("/index", function(req, res)
    local body = json.decode(req.body)
    if not body or not body.values then
        res:status(400):json({ error = "need values array" })
        return
    end

    local parts = {}
    for _, v in ipairs(body.values) do
        parts[#parts + 1] = string.pack("I4", math.floor(v))
    end
    local packed = table.concat(parts)

    -- WASM preprocess → WasmBuffer
    local wasm_out = compute.call("echo", packed, { buffer = true })

    -- WasmBuffer → persistent GPU buffer (zero-copy)
    gpu.buffer("indexed_data", wasm_out)

    res:json({ indexed = #body.values })
end)

app.get("/query", function(req, res)
    -- Fire-and-forget: double the indexed data in-place
    local ok, err = gpu.dispatch("double", {
        buffers = {{ name = "indexed_data" }},
        workgroups = { x = 1 },
        output = false,
    })
    if err then
        res:status(500):json({ error = err })
        return
    end

    -- Read back
    local data, rerr = gpu.buffer_read("indexed_data")
    if rerr then
        res:status(500):json({ error = rerr })
        return
    end

    local count = #data / 4
    local results = {}
    for i = 1, count do
        results[i] = string.unpack("I4", data, (i - 1) * 4 + 1)
    end
    res:json({ values = results })
end)
