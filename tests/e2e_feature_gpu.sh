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
# hull build takes a local feature archive only from the running hull's own
# directory (never ./build), so stage it beside the copied binary.
cp build/libhull_feature-gpu.a "$(dirname "$HULL")/"
APP=$(mktemp -d)
PLAIN=$(mktemp -d)
trap 'rm -rf "$APP" "$PLAIN" /tmp/hull_base_gpu_e2e' EXIT

# The shader lives in shaders/: a built binary compiles only shaders embedded
# from shaders/ unless the manifest sets gpu.dynamic_shaders (audit
# convergence 7.3). SAME is byte-identical to the file, so gpu.compile of it is
# the embedded shader; OTHER is not, so it is refused.
mkdir -p "$APP/shaders"
printf '%s\n' \
    '@group(0) @binding(0) var<storage,read_write> d: array<f32>;' \
    '@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id: vec3<u32>){ d[id.x]=d[id.x]*2.0; }' \
    > "$APP/shaders/dbl.wgsl"
cat > "$APP/app.lua" <<'LUA'
app.manifest({ gpu = true, modules = { "hull/gpu@1" } })
local SAME = "@group(0) @binding(0) var<storage,read_write> d: array<f32>;\n" ..
    "@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id: vec3<u32>){ d[id.x]=d[id.x]*2.0; }\n"
local OTHER = "@group(0) @binding(0) var<storage,read_write> d: array<f32>;\n" ..
    "@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id: vec3<u32>){ d[id.x]=d[id.x]*3.0; }\n"
app.main(function()
    -- pcall: on a GPU-capable binary running on a GPU-LESS host, hull.gpu is
    -- not registered (no device), so require errors. That's fine here -- the
    -- build composed the feature and the binary booted, which is the point.
    local ok, gpu = pcall(require, "hull.gpu")
    if ok and gpu and gpu.available() then
        assert(gpu.load("dbl"), "gpu.load of an embedded shader failed")
        local okc, err = gpu.compile("tri", OTHER)
        assert(okc == nil and err == "dynamic_shader_refused",
               "dynamic source was not refused: " .. tostring(err))
        print("POLICY: dynamic source refused")
        assert(gpu.compile("dbl2", SAME), "byte-identical source refused")
        print("POLICY: embedded source accepted")
        local out = gpu.dispatch("dbl2", {
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
if echo "$RUN_OUT" | grep -q "dispatch verified on device"; then
    echo "$RUN_OUT" | grep -q "POLICY: dynamic source refused" \
        || { echo "FAIL: dynamic source not refused"; exit 1; }
    echo "ok  built binary: embedded shaders only, dynamic source refused"
fi
echo "$BUILD_OUT" | grep -q "shader(s) pre-checked" \
    || { echo "FAIL: hull build did not check the embedded shaders"; exit 1; }
echo "ok  hull build pre-checked the embedded shaders"

echo "=== gpu.dynamic_shaders = true: the opt-in compiles runtime source ==="
DYN=$(mktemp -d)
cat > "$DYN/app.lua" <<'LUA'
app.manifest({ gpu = { dynamic_shaders = true }, modules = { "hull/gpu@1" } })
app.main(function()
    local ok, gpu = pcall(require, "hull.gpu")
    if ok and gpu and gpu.available() then
        assert(gpu.compile("tri",
            "@group(0) @binding(0) var<storage,read_write> d: array<f32>;\n" ..
            "@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id: vec3<u32>){ d[id.x]=d[id.x]*3.0; }"),
            "the opt-in did not compile dynamic source")
        print("DYNAMIC OK (compiled on device)")
    else
        print("DYNAMIC OK (no GPU device)")
    end
    return 0
end)
LUA
"$HULL" build --compiler=system --with=gpu --no-verify-platform -o "$DYN/bin" "$DYN" >/dev/null 2>&1 || true
DYN_OUT=$("$DYN/bin" 2>&1) || true
rm -rf "$DYN"
echo "$DYN_OUT" | grep -q "DYNAMIC OK" || { echo "$DYN_OUT"; echo "FAIL: opt-in app"; exit 1; }
if echo "$RUN_OUT" | grep -q "dispatch verified on device"; then
    echo "$DYN_OUT" | grep -q "compiled on device" \
        || { echo "$DYN_OUT"; echo "FAIL: opt-in did not compile on the device"; exit 1; }
fi
echo "ok  gpu.dynamic_shaders opt-in"

echo "=== the compile helper: a served opt-in app compiles runtime source in a helper process ==="
# The built binary spawns itself as the helper before its sandbox
# (cap/gpu_helper.c). HULL_TEST_GPU_HELPER makes the helper's worker hang,
# crash or send an oversized reply; the server must answer the compile with
# an error and keep serving. Needs a GPU device (the helper is spawned only
# when the binary has one); skipped otherwise.
if echo "$RUN_OUT" | grep -q "dispatch verified on device"; then
    HAPP=$(mktemp -d)
    HCACHE=$(mktemp -d)
    cat > "$HAPP/app.lua" <<'LUA'
app.manifest({ gpu = { dynamic_shaders = true },
               modules = { "hull/gpu@1", "hull/http-server@1" } })
local SRC = "@group(0) @binding(0) var<storage,read_write> d: array<f32>;\n" ..
    "@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id: vec3<u32>)" ..
    "{ d[id.x] = d[id.x] * 7.0; }\n"
local n = 0
app.get("/ping", function(req, res) res:text("pong") end)
app.get("/compile", function(req, res)
    local gpu = require("hull.gpu")
    n = n + 1
    local ok, err = gpu.compile("t" .. n, SRC)
    res:text(ok and "compiled" or ("error:" .. tostring(err)))
end)
LUA
    "$HULL" build --compiler=system --with=gpu --no-verify-platform -o "$HAPP/bin" "$HAPP" >/dev/null 2>&1 || true
    test -x "$HAPP/bin" || { echo "FAIL: helper app did not build"; exit 1; }

    # One server per hook; prints "<compile reply>|<ping reply>".
    helper_case() {
        _port=$1; _hook=$2
        (cd "$HAPP" && HULL_CACHE_DIR="$HCACHE" HULL_TEST_GPU_HELPER="$_hook" \
            exec ./bin -p "$_port") >"$HAPP/srv-$_port.log" 2>&1 &
        _pid=$!
        _up=0
        for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
            if curl -s "http://127.0.0.1:$_port/ping" >/dev/null 2>&1; then _up=1; break; fi
            sleep 0.25
        done
        if [ "$_up" -eq 1 ]; then
            _c=$(curl -s --max-time 30 "http://127.0.0.1:$_port/compile")
            _p=$(curl -s --max-time 5 "http://127.0.0.1:$_port/ping")
        else
            _c="(no server)"; _p=""
        fi
        kill "$_pid" 2>/dev/null || true
        wait "$_pid" 2>/dev/null || true
        echo "$_c|$_p"
    }
    # The failures first: an OK verdict is cached on disk, so the hooks
    # would not be reached for the same source afterwards.
    OUT=$(helper_case 19931 slow)
    [ "$OUT" = "error:compile_timeout|pong" ] \
        || { cat "$HAPP/srv-19931.log"; echo "got: $OUT"; echo "FAIL: helper past its deadline"; exit 1; }
    echo "ok  helper past its deadline: killed, compile_timeout, server keeps serving"
    OUT=$(helper_case 19932 crash)
    [ "$OUT" = "error:shader_helper_failed|pong" ] \
        || { cat "$HAPP/srv-19932.log"; echo "got: $OUT"; echo "FAIL: helper crash"; exit 1; }
    echo "ok  helper crash: shader_helper_failed, server keeps serving"
    OUT=$(helper_case 19933 oversize)
    [ "$OUT" = "error:shader_helper_failed|pong" ] \
        || { cat "$HAPP/srv-19933.log"; echo "got: $OUT"; echo "FAIL: oversized helper reply"; exit 1; }
    echo "ok  oversized helper reply refused"
    OUT=$(helper_case 19934 "")
    [ "$OUT" = "compiled|pong" ] \
        || { cat "$HAPP/srv-19934.log"; echo "got: $OUT"; echo "FAIL: helper compile"; exit 1; }
    echo "ok  runtime source compiled through the helper"
    ls "$HCACHE"/gpu-shaders/blobs/*/* >/dev/null 2>&1 \
        || { ls -R "$HCACHE"; echo "FAIL: no sealed verdict cached"; exit 1; }
    # A restart finds the verdict in the sealed cache: the helper is not
    # asked (its crash hook would refuse the compile otherwise).
    OUT=$(helper_case 19935 crash)
    [ "$OUT" = "compiled|pong" ] \
        || { cat "$HAPP/srv-19935.log"; echo "got: $OUT"; echo "FAIL: cached verdict not used"; exit 1; }
    echo "ok  a cached verdict skips the helper after a restart"
    rm -rf "$HAPP" "$HCACHE"
else
    echo "skip  compile helper cases (no GPU device)"
fi

echo "=== a shader the pre-check refuses fails the build ==="
BAD=$(mktemp -d)
mkdir -p "$BAD/shaders"
printf 'app.manifest({ gpu = true, modules = { "hull/gpu@1" } })\napp.main(function() return 0 end)\n' \
    > "$BAD/app.lua"
printf '@compute @workgroup_size(1) fn main() { let $x = 1; }\n' > "$BAD/shaders/bad.wgsl"
BAD_OUT=$("$HULL" build --compiler=system --with=gpu --no-verify-platform -o "$BAD/bin" "$BAD" 2>&1) && BAD_RC=0 || BAD_RC=$?
BAD_BIN=0; test -e "$BAD/bin" && BAD_BIN=1
rm -rf "$BAD"
[ "$BAD_RC" -ne 0 ] && [ "$BAD_BIN" -eq 0 ] \
    && echo "$BAD_OUT" | grep -q "shader check failed" \
    || { echo "$BAD_OUT"; echo "FAIL: a refused shader did not fail the build"; exit 1; }
echo "ok  a refused shader fails the build"

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
