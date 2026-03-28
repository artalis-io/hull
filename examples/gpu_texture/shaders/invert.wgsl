@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var input_sampler: sampler;
@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(1, 1)
fn main(@builtin(global_invocation_id) gid: vec3u) {
    let dims = textureDimensions(input_tex);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }
    let color = textureLoad(input_tex, vec2i(gid.xy), 0);
    textureStore(output_tex, vec2i(gid.xy), vec4f(1.0 - color.r, 1.0 - color.g, 1.0 - color.b, color.a));
}
