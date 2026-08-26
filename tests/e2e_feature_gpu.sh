#!/bin/sh
# e2e_feature_gpu.sh - GPU (wgpu-native) as a composable feature, end to end.
#
# Builds a BASE hull (EMBED_PLATFORM=1, no GPU compiled in) + the GPU feature
# archive, then `hull build --with=gpu` an app and boots it. Proves that
# `hull build` composes libhull_feature-gpu.a + a generated registry (the strong
# hl_gpu_feature_backends hook) into the app binary, so a base-built hull gains
# GPU purely from the feature -- while a plain app stays GPU-free and a base
# build rejects a gpu app. See docs/features_and_flavors.md.
#
# This is a BUILD-only e2e: gpu.dispatch needs a physical GPU device, which CI
# runners lack. So the app pcall-requires hull.gpu and branches on
# gpu.available(): with a device it runs a real dispatch and checks the result;
# without one it just confirms the composed binary links + boots. Both print
# "GPU FEATURE APP OK". On Linux the composed binary links -lvulkan, so the
# runtime needs the Vulkan loader present (libvulkan1) even with no GPU device;
# the CI job installs it. macOS links the Metal frameworks (always in the SDK).
#
# Must run on a fresh build tree (no prior HL_ENABLE_GPU=1 objects), so it lives
# in its own CI job. Requires `make fetch-wgpu` to have run.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

echo "=== build base hull (EMBED_PLATFORM=1; base is GPU-free) ==="
make EMBED_PLATFORM=1 >/dev/null
# Stash the base hull: `make feature-gpu` re-invokes make with HL_ENABLE_GPU=1,
# and the build-config sentinel cleans build/ on the flag flip. Save the
# build-tool binary first so it survives the feature-archive build.
cp build/hull /tmp/hull_base_gpu_e2e

echo "=== build the GPU feature archive ==="
make feature-gpu >/dev/null
ls -la build/libhull_feature-gpu.a
# Symbol present? (macOS nm prefixes a leading '_', Linux doesn't - match both.)
nm build/libhull_feature-gpu.a 2>/dev/null | grep -qE '[ _]hl_gpu_backend_wgpu$' \
    || { echo "FAIL: feature archive lacks hl_gpu_backend_wgpu"; exit 1; }

HULL=/tmp/hull_base_gpu_e2e
APP=$(mktemp -d)
PLAIN=$(mktemp -d)
trap 'rm -rf "$APP" "$PLAIN" /tmp/hull_base_gpu_e2e' EXIT

cat > "$APP/app.lua" <<'LUA'
app.manifest({ gpu = true, modules = { "hull/gpu@1" } })
app.main(function()
    -- pcall: on a GPU-capable binary running on a GPU-LESS host, hull.gpu is
    -- not registered (no device), so require errors. That's fine here -- the
    -- build composed the feature and the binary booted, which is the point.
    local ok, gpu = pcall(require, "hull.gpu")
    if ok and gpu and gpu.available() then
        gpu.compile("dbl",
            "@group(0) @binding(0) var<storage,read_write> d: array<f32>;\n" ..
            "@compute @workgroup_size(1) fn main(" ..
            "@builtin(global_invocation_id) id: vec3<u32>){ d[id.x]=d[id.x]*2.0; }")
        local out = gpu.dispatch("dbl", {
            buffers    = {{ data = string.pack("<ffff", 1, 2, 3, 4), usage = "readwrite" }},
            workgroups = { x = 4 },
            output     = 1,
        })
        assert(out, "dispatch returned nil")
        local a = string.unpack("<f", out)
        assert(a == 2.0, "dispatch wrong result: " .. tostring(a))
        print("GPU FEATURE APP OK (dispatch verified on device)")
    else
        print("GPU FEATURE APP OK (composed; no GPU device)")
    end
    return 0
end)
LUA

echo "=== hull build --with=gpu (system compiler links wgpu + frameworks) ==="
BUILD_OUT=$("$HULL" build --compiler=system --with=gpu --no-verify-platform -o "$APP/bin" "$APP" 2>&1) || true
echo "$BUILD_OUT"
echo "$BUILD_OUT" | grep -q "composed feature 'gpu'" || { echo "FAIL: feature not composed"; exit 1; }
test -x "$APP/bin" || { echo "FAIL: no composed binary produced"; exit 1; }

echo "=== boot the composed binary ==="
RUN_OUT=$("$APP/bin" 2>&1) || true
echo "$RUN_OUT"
echo "$RUN_OUT" | grep -q "GPU FEATURE APP OK" || {
    echo "--- boot failed; diagnostics ---"
    file "$APP/bin" 2>/dev/null || true
    command -v ldd >/dev/null 2>&1 && ldd "$APP/bin" 2>&1 | grep -i "vulkan\|not found" || true
    echo "FAIL: composed gpu binary did not boot"; exit 1
}
echo "ok  --with=gpu composed + booted"

echo "=== negative: a plain app must NOT compose gpu ==="
printf 'app.manifest({modules={}})\napp.main(function() print("PLAIN OK") return 0 end)\n' \
    > "$PLAIN/app.lua"
PLAIN_OUT=$("$HULL" build --no-verify-platform -o "$PLAIN/bin" "$PLAIN" 2>&1) || true
if echo "$PLAIN_OUT" | grep -q "composed feature"; then
    echo "$PLAIN_OUT"; echo "FAIL: composed a feature for a plain app"; exit 1
fi
"$PLAIN/bin" 2>&1 | grep -q "PLAIN OK" || { echo "FAIL: plain app did not run"; exit 1; }
echo "ok  plain app stayed GPU-free"

echo "=== negative: without --with=gpu the composed binary must NOT run the gpu app ==="
# `hull build` only WARNS on a resolver failure (it doesn't hard-fail), so a base
# build still produces a binary - but with the base (GPU-free) platform, so the
# runtime resolver rejects hull/gpu at load and the app never prints OK.
"$HULL" build --no-verify-platform -o "$APP/bin_base" "$APP" >/dev/null 2>&1 || true
BASE_RUN=$("$APP/bin_base" 2>&1) || true
if echo "$BASE_RUN" | grep -q "GPU FEATURE APP OK"; then
    echo "$BASE_RUN"; echo "FAIL: base-built binary ran the gpu app without the feature"; exit 1
fi
echo "$BASE_RUN" | grep -q "requires HL_ENABLE_GPU" \
    || { echo "$BASE_RUN"; echo "FAIL: expected a GPU-cap rejection at load"; exit 1; }
echo "ok  base binary rejects the gpu app at load"

echo "PASS: GPU feature composed into an app binary; base stays GPU-free"
