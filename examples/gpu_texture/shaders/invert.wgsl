@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var input_sampler: sampler;
@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;

// A sampled texture binds a view AND a sampler (bindings 0 and 1), so the
// shader must use the sampler: WGSL drops a declaration the entry point never
// uses from the pipeline's layout, and the bind group then fails validation.
@compute @workgroup_size(1, 1)
fn main(@builtin(global_invocation_id) gid: vec3u) {
    let dims = textureDimensions(input_tex);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }
    let uv = (vec2f(gid.xy) + vec2f(0.5, 0.5)) / vec2f(dims);
    let color = textureSampleLevel(input_tex, input_sampler, uv, 0.0);
    textureStore(output_tex, vec2i(gid.xy), vec4f(1.0 - color.r, 1.0 - color.g, 1.0 - color.b, color.a));
}
