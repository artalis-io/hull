-- GPU vector similarity search example
--
-- Demonstrates GPU compute shaders for real-time similarity search.
-- Index a set of embedding vectors, then query for nearest neighbors
-- using cosine similarity computed on the GPU.
--
-- Run:  hull examples/gpu_search/app.lua
--       (requires: make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu)
--
-- Test: curl localhost:3000/health
--       curl -X POST localhost:3000/index -d '{"dimensions":4,"vectors":[[1,0,0,0],[0,1,0,0],[0.7,0.7,0,0]]}'
--       curl -X POST localhost:3000/search -d '{"query":[0.8,0.6,0,0],"k":2}'

app.manifest({ gpu = true })

-- WGSL compute shader: cosine similarity between query and N candidates
--
-- Binding layout (matches Hull gpu.dispatch with uniforms):
--   binding 0 = uniform Params { dimensions, count }
--   binding 1 = storage read: query vector
--   binding 2 = storage read: candidate vectors (flat)
--   binding 3 = storage read_write: similarity scores (output)
local SHADER = [[
struct Params {
    dimensions: u32,
    count: u32,
}

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> query: array<f32>;
@group(0) @binding(2) var<storage, read> candidates: array<f32>;
@group(0) @binding(3) var<storage, read_write> results: array<f32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let idx = gid.x;
    if (idx >= params.count) { return; }

    let offset = idx * params.dimensions;
    var dot_prod: f32 = 0.0;
    var norm_q: f32 = 0.0;
    var norm_c: f32 = 0.0;

    for (var i: u32 = 0u; i < params.dimensions; i = i + 1u) {
        let q = query[i];
        let c = candidates[offset + i];
        dot_prod = dot_prod + q * c;
        norm_q = norm_q + q * q;
        norm_c = norm_c + c * c;
    }

    let denom = sqrt(norm_q) * sqrt(norm_c);
    if (denom > 0.0) {
        results[idx] = dot_prod / denom;
    } else {
        results[idx] = 0.0;
    }
}
]]

-- Compile shader at startup
if gpu.available() then
    local _, err = gpu.compile("cosine_similarity", SHADER)
    if err then
        log.error("shader compile failed: " .. err)
    end
else
    log.warn("GPU not available — search endpoints will return errors")
end

-- Index state
local dimensions = 0
local vector_count = 0

-- Health check
app.get("/health", function(req, res)
    res:json({
        ok = true,
        gpu = gpu.available(),
        devices = gpu.devices(),
        indexed = vector_count,
    })
end)

-- List GPU devices
app.get("/devices", function(req, res)
    res:json(gpu.devices())
end)

-- Index vectors: POST /index
-- Body: { "dimensions": 128, "vectors": [[0.1, 0.2, ...], ...] }
app.post("/index", function(req, res)
    local body = json.decode(req.body)
    if not body or not body.vectors or not body.dimensions then
        res:status(400):json({ error = "need dimensions and vectors" })
        return
    end

    dimensions = body.dimensions
    vector_count = #body.vectors

    -- Pack all vectors as contiguous f32 binary
    local parts = {}
    for _, vec in ipairs(body.vectors) do
        for _, v in ipairs(vec) do
            parts[#parts + 1] = string.pack("f", v)
        end
    end
    local data = table.concat(parts)

    -- Upload to persistent GPU buffer
    local _, err = gpu.buffer("candidates", data)
    if err then
        res:status(500):json({ error = err })
        return
    end

    res:json({ indexed = vector_count, dimensions = dimensions })
end)

-- Search: POST /search
-- Body: { "query": [0.1, 0.2, ...], "k": 5 }
app.post("/search", function(req, res)
    if vector_count == 0 then
        res:status(400):json({ error = "no vectors indexed" })
        return
    end

    local body = json.decode(req.body)
    if not body or not body.query then
        res:status(400):json({ error = "need query" })
        return
    end

    local k = body.k or 5

    -- Pack query as f32 binary
    local qparts = {}
    for _, v in ipairs(body.query) do
        qparts[#qparts + 1] = string.pack("f", v)
    end
    local query_data = table.concat(qparts)

    -- Pack uniforms: { dimensions: u32, count: u32 }
    -- Padded to 16 bytes (WebGPU uniform alignment requirement)
    local uniforms = string.pack("I4I4I4I4", dimensions, vector_count, 0, 0)

    -- Dispatch: ceil(count / 64) workgroups
    local wg_x = math.ceil(vector_count / 64)

    local output, err = gpu.dispatch("cosine_similarity", {
        uniforms = uniforms,
        buffers = {
            { data = query_data, usage = "read" },             -- binding 1: query
            { name = "candidates", usage = "read" },           -- binding 2: persistent
            { size = vector_count * 4, usage = "readwrite" },  -- binding 3: results
        },
        workgroups = { x = wg_x, y = 1, z = 1 },
        output = 3,  -- 1-indexed: read back the 3rd buffer (results)
    })

    if err then
        res:status(500):json({ error = err })
        return
    end

    -- Unpack f32 scores and find top-K
    local scores = {}
    for i = 1, vector_count do
        local score = string.unpack("f", output, (i - 1) * 4 + 1)
        scores[#scores + 1] = { index = i - 1, score = score }
    end
    table.sort(scores, function(a, b) return a.score > b.score end)

    local results = {}
    for i = 1, math.min(k, #scores) do
        results[#results + 1] = scores[i]
    end

    res:json({ results = results, total = vector_count })
end)
