// Cosine similarity: compare a query vector against N candidate vectors.
//
// Binding layout (auto-layout, matches Hull gpu.dispatch):
//   @binding(0) - uniform: Params { dimensions, count }
//   @binding(1) - storage read: query vector (f32 x dimensions)
//   @binding(2) - storage read: candidate vectors (f32 x dimensions x count)
//   @binding(3) - storage read_write: results (f32 x count)

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
    if (idx >= params.count) {
        return;
    }

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
