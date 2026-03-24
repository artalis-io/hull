// GPU vector similarity search example
//
// Demonstrates GPU compute shaders for real-time similarity search.
// Index a set of embedding vectors, then query for nearest neighbors
// using cosine similarity computed on the GPU.
//
// Run:  hull examples/gpu_search/app.js
//       (requires: make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu)
//
// Test: curl localhost:3000/health
//       curl -X POST localhost:3000/index -d '{"dimensions":4,"vectors":[[1,0,0,0],[0,1,0,0],[0.7,0.7,0,0]]}'
//       curl -X POST localhost:3000/search -d '{"query":[0.8,0.6,0,0],"k":2}'

import { gpu } from "hull:gpu";

app.manifest({ gpu: true });

// WGSL compute shader: cosine similarity between query and N candidates
const SHADER = `
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
`;

// Compile shader at startup
if (gpu.available()) {
    gpu.compile("cosine_similarity", SHADER);
} else {
    log.warn("GPU not available — search endpoints will return errors");
}

// Index state
let dimensions = 0;
let vectorCount = 0;

// Health check
app.get("/health", (_req, res) => {
    res.json({
        ok: true,
        gpu: gpu.available(),
        devices: gpu.devices(),
        indexed: vectorCount,
    });
});

// List GPU devices
app.get("/devices", (_req, res) => {
    res.json(gpu.devices());
});

// Index vectors: POST /index
// Body: { "dimensions": 128, "vectors": [[0.1, 0.2, ...], ...] }
app.post("/index", (req, res) => {
    const body = JSON.parse(req.body);
    if (!body || !body.vectors || !body.dimensions) {
        res.status(400).json({ error: "need dimensions and vectors" });
        return;
    }

    dimensions = body.dimensions;
    vectorCount = body.vectors.length;

    // Pack all vectors as contiguous f32 binary
    const buf = new ArrayBuffer(vectorCount * dimensions * 4);
    const view = new Float32Array(buf);
    let offset = 0;
    for (const vec of body.vectors) {
        for (const v of vec) {
            view[offset++] = v;
        }
    }

    // Upload to persistent GPU buffer
    gpu.buffer("candidates", buf);
    res.json({ indexed: vectorCount, dimensions });
});

// Search: POST /search
// Body: { "query": [0.1, 0.2, ...], "k": 5 }
app.post("/search", (req, res) => {
    if (vectorCount === 0) {
        res.status(400).json({ error: "no vectors indexed" });
        return;
    }

    const body = JSON.parse(req.body);
    if (!body || !body.query) {
        res.status(400).json({ error: "need query" });
        return;
    }

    const k = body.k || 5;

    // Pack query as f32 binary
    const queryBuf = new ArrayBuffer(body.query.length * 4);
    const queryView = new Float32Array(queryBuf);
    body.query.forEach((v, i) => { queryView[i] = v; });

    // Pack uniforms: { dimensions: u32, count: u32 } padded to 16 bytes
    const uniformBuf = new ArrayBuffer(16);
    const uniformView = new DataView(uniformBuf);
    uniformView.setUint32(0, dimensions, true);
    uniformView.setUint32(4, vectorCount, true);

    // Dispatch: ceil(count / 64) workgroups
    const wgX = Math.ceil(vectorCount / 64);

    const output = gpu.dispatch("cosine_similarity", {
        uniforms: uniformBuf,
        buffers: [
            { data: queryBuf, usage: "read" },                    // binding 1: query
            { name: "candidates", usage: "read" },                // binding 2: persistent
            { size: vectorCount * 4, usage: "readwrite" },        // binding 3: results
        ],
        workgroups: { x: wgX, y: 1, z: 1 },
        output: 2,  // 0-indexed: read back the 3rd buffer (results)
    });

    // Unpack f32 scores and find top-K
    const resultView = new Float32Array(output);
    const scores = [];
    for (let i = 0; i < vectorCount; i++) {
        scores.push({ index: i, score: resultView[i] });
    }
    scores.sort((a, b) => b.score - a.score);

    res.json({ results: scores.slice(0, k), total: vectorCount });
});
