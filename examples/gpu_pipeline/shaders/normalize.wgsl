// Stage 1: normalize each value by column max
// Finds max per column, divides each value by it.
// Input: flat f32 array (items × features), output: normalized in-place.
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
