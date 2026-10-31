// Stage 2: apply weights
// Multiplies each value by its feature weight.
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
