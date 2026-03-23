-- GPU pipeline example — multi-stage compute in a single GPU submission
--
-- Demonstrates gpu.pipeline() for chaining shaders without round-trips.
-- A 3-stage scoring pipeline:
--   1. normalize: scale each f32 to [0, 1] range
--   2. weight:    multiply by per-feature weights
--   3. reduce:    sum weighted features into per-item scores
--
-- Run:  hull examples/gpu_pipeline/app.lua
--       (requires: make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu)
--
-- Test:
--   curl localhost:3000/health
--   curl -X POST localhost:3000/score -d '{"items":[[80,150,3],[60,200,1],[95,50,5]],"weights":[0.5,0.3,0.2]}'

app.manifest({ gpu = true })

-- ── Stage 1: normalize each value by column max ──────────────────
-- Finds max per column, divides each value by it.
-- Input: flat f32 array (items × features), output: normalized in-place.
local NORMALIZE = [[
struct Params { items: u32, features: u32 }
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read_write> data: array<f32>;

@compute @workgroup_size(1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let col = gid.x;
    if (col >= params.features) { return; }

    // Find max in this column
    var mx: f32 = 0.0;
    for (var r: u32 = 0u; r < params.items; r = r + 1u) {
        let v = data[r * params.features + col];
        if (v > mx) { mx = v; }
    }

    // Normalize
    if (mx > 0.0) {
        for (var r: u32 = 0u; r < params.items; r = r + 1u) {
            let idx = r * params.features + col;
            data[idx] = data[idx] / mx;
        }
    }
}
]]

-- ── Stage 2: apply weights ───────────────────────────────────────
-- Multiplies each value by its feature weight.
local WEIGHT = [[
struct Params { items: u32, features: u32 }
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read_write> data: array<f32>;
@group(0) @binding(2) var<storage, read> weights: array<f32>;

@compute @workgroup_size(1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let idx = gid.x;
    let total = params.items * params.features;
    if (idx >= total) { return; }
    let col = idx % params.features;
    data[idx] = data[idx] * weights[col];
}
]]

-- ── Stage 3: reduce (sum) per item ───────────────────────────────
-- Sums weighted features for each item into a score.
local REDUCE = [[
struct Params { items: u32, features: u32 }
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> data: array<f32>;
@group(0) @binding(2) var<storage, read_write> scores: array<f32>;

@compute @workgroup_size(1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let row = gid.x;
    if (row >= params.items) { return; }
    var sum: f32 = 0.0;
    for (var c: u32 = 0u; c < params.features; c = c + 1u) {
        sum = sum + data[row * params.features + c];
    }
    scores[row] = sum;
}
]]

-- Compile shaders at startup
if gpu.available() then
    gpu.compile("normalize", NORMALIZE)
    gpu.compile("weight", WEIGHT)
    gpu.compile("reduce", REDUCE)
else
    log.warn("GPU not available")
end

app.get("/health", function(req, res)
    res:json({ ok = true, gpu = gpu.available(), devices = gpu.devices() })
end)

-- POST /score — score items through a 3-stage pipeline
-- Body: { "items": [[80,150,3], [60,200,1], [95,50,5]], "weights": [0.5, 0.3, 0.2] }
app.post("/score", function(req, res)
    local body = json.decode(req.body)
    if not body or not body.items or not body.weights then
        res:status(400):json({ error = "need items and weights" })
        return
    end

    local items = #body.items
    local features = #body.weights

    -- Pack data as flat f32: items × features
    local data_parts = {}
    for _, item in ipairs(body.items) do
        for _, v in ipairs(item) do
            data_parts[#data_parts + 1] = string.pack("f", v)
        end
    end
    local data = table.concat(data_parts)

    -- Pack weights as f32
    local weight_parts = {}
    for _, w in ipairs(body.weights) do
        weight_parts[#weight_parts + 1] = string.pack("f", w)
    end
    local weights = table.concat(weight_parts)

    -- Uniforms: { items: u32, features: u32 } padded to 16 bytes
    local uniforms = string.pack("I4I4I4I4", items, features, 0, 0)

    -- 3-stage pipeline: normalize → weight → reduce
    -- Single GPU submission, single readback
    local out, err = gpu.pipeline({
        -- Stage 1: normalize columns (dispatch one thread per feature)
        {
            shader = "normalize",
            uniforms = uniforms,
            buffers = {
                { name = "data", data = data },
            },
            workgroups = { x = features },
        },
        -- Stage 2: apply weights (dispatch one thread per element)
        {
            shader = "weight",
            uniforms = uniforms,
            buffers = {
                { name = "data" },                       -- reuse from stage 1
                { name = "weights", data = weights },    -- new buffer
            },
            workgroups = { x = items * features },
        },
        -- Stage 3: reduce to scores (dispatch one thread per item)
        {
            shader = "reduce",
            uniforms = uniforms,
            buffers = {
                { name = "data" },                                    -- reuse
                { name = "scores", size = items * 4 },   -- output
            },
            workgroups = { x = items },
        },
    }, {
        outputs = { { stage = 3, buffer = 2 } },  -- read back scores
    })

    if err then
        res:status(500):json({ error = err })
        return
    end

    -- Unpack scores
    local scores = {}
    for i = 1, items do
        local score = string.unpack("f", out, (i - 1) * 4 + 1)
        scores[#scores + 1] = {
            index = i - 1,
            score = math.floor(score * 1000 + 0.5) / 1000,  -- round to 3dp
        }
    end
    table.sort(scores, function(a, b) return a.score > b.score end)

    res:json({ scores = scores })
end)

-- GET /compare — compare pipeline (1 submit) vs 3× dispatch (3 submits)
app.get("/compare", function(req, res)
    local items = 1000
    local features = 16

    -- Generate deterministic data
    local data_parts = {}
    local x = 42
    for _ = 1, items * features do
        x = ((x * 1103515245 + 12345) % 2147483648)
        data_parts[#data_parts + 1] = string.pack("f", (x % 1000) / 10.0)
    end
    local data = table.concat(data_parts)

    local weight_parts = {}
    for f = 1, features do
        weight_parts[f] = string.pack("f", 1.0 / features)
    end
    local weights = table.concat(weight_parts)
    local uniforms = string.pack("I4I4I4I4", items, features, 0, 0)

    -- Time: 3 separate dispatches
    local t0 = time.clock()
    for _ = 1, 10 do
        gpu.dispatch("normalize", {
            uniforms = uniforms,
            buffers = {{ data = data, size = items * features * 4 }},
            workgroups = { x = features },
            output = 1,
        })
        gpu.dispatch("weight", {
            uniforms = uniforms,
            buffers = {
                { data = data, size = items * features * 4 },
                { data = weights },
            },
            workgroups = { x = items * features },
            output = 1,
        })
        gpu.dispatch("reduce", {
            uniforms = uniforms,
            buffers = {
                { data = data },
                { size = items * 4 },
            },
            workgroups = { x = items },
            output = 2,
        })
    end
    local dispatch_ms = (time.clock() - t0) * 1000 / 10

    -- Time: single pipeline
    t0 = time.clock()
    for _ = 1, 10 do
        gpu.pipeline({
            { shader = "normalize", uniforms = uniforms,
              buffers = {{ name = "d", data = data }},
              workgroups = { x = features } },
            { shader = "weight", uniforms = uniforms,
              buffers = {{ name = "d" }, { name = "w", data = weights }},
              workgroups = { x = items * features } },
            { shader = "reduce", uniforms = uniforms,
              buffers = {{ name = "d" }, { name = "s", size = items * 4 }},
              workgroups = { x = items } },
        }, { outputs = {{ stage = 3, buffer = 2 }} })
    end
    local pipeline_ms = (time.clock() - t0) * 1000 / 10

    res:json({
        items = items,
        features = features,
        dispatch_3x_ms = math.floor(dispatch_ms * 100 + 0.5) / 100,
        pipeline_ms = math.floor(pipeline_ms * 100 + 0.5) / 100,
        speedup = math.floor(dispatch_ms / pipeline_ms * 10 + 0.5) / 10,
    })
end)
