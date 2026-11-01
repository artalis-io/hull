// Stage 3: reduce (sum) per item
// Sums weighted features for each item into a score.
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
