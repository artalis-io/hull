#!/bin/sh
# e2e_gpu_hull_test.sh - `hull test` and `hull agent test` load hull/gpu.
#
# The GPU context used to be opened only by the server's startup (serve.c),
# so the app context behind `hull test` / `hull agent test` (and the app.main
# runner of a built binary, covered by e2e_feature_gpu.sh) never registered
# hull.gpu: an app requiring it failed to load under test. Now every entry
# point that runs app code opens it the same way (cap/gpu_setup.c).
#
# Oracle: `hull app.lua` (the server path) says whether this host has a GPU
# device. `hull test` must agree - on a device, a test dispatches a shader
# directly, through a route (test.get) and through app.main
# (test.run_main); without one, hull.gpu is absent in both (Lua only: a JS
# `import ... from "hull:gpu"` fails to load with no device, under the
# server too). Without a device this checks little - the Linux CI job has
# none; the macOS one has Metal and sets HULL_E2E_REQUIRE_GPU=1, which fails
# when no device is found.
#
# Needs a monolithic GPU build: `make fetch-wgpu` first. Rebuilds build/
# with HL_ENABLE_GPU=1 (the config sentinel cleans it).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

echo "=== build hull with HL_ENABLE_GPU=1 ==="
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu >"$WORK/build.log" 2>&1 \
    || { tail -40 "$WORK/build.log"; echo "FAIL: GPU build"; exit 1; }
HULL=$(pwd)/build/hull

SHADER='@group(0) @binding(0) var<storage,read_write> d: array<f32>;
@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id: vec3<u32>){ d[id.x]=d[id.x]*2.0; }'

echo "=== oracle: does the server path see a GPU device? ==="
mkdir -p "$WORK/probe"
cat > "$WORK/probe/app.lua" <<'LUA'
app.manifest({ gpu = true, modules = { "hull/gpu@1" } })
app.main(function()
    local ok, gpu = pcall(require, "hull.gpu")
    print((ok and gpu and gpu.available()) and "PROBE GPU yes" or "PROBE GPU no")
    return 0
end)
LUA
PROBE=$("$HULL" "$WORK/probe/app.lua" 2>&1) || true
echo "$PROBE" | grep "PROBE GPU" || { echo "$PROBE"; echo "FAIL: probe did not run"; exit 1; }
if echo "$PROBE" | grep -q "PROBE GPU yes"; then EXPECT=true; else EXPECT=false; fi
if [ "${HULL_E2E_REQUIRE_GPU:-0}" = 1 ] && [ "$EXPECT" != true ]; then
    echo "FAIL: HULL_E2E_REQUIRE_GPU=1 but no GPU device"; exit 1
fi
echo "expect gpu = $EXPECT"

mk_lua() {   # $1 = dir
    mkdir -p "$1/shaders" "$1/tests"
    printf '%s\n' "$SHADER" > "$1/shaders/dbl.wgsl"
    cat > "$1/app.lua" <<'LUA'
app.manifest({ gpu = true, modules = { "hull/gpu@1", "hull/http-server@1" } })
local ok, gpu = pcall(require, "hull.gpu")
GPU = (ok and gpu and gpu.available()) and gpu or nil
if GPU then GPU.load("dbl") end
local function dbl(x)
    local out = GPU.dispatch("dbl", {
        buffers = {{ data = string.pack("<ff", x, x + 1), usage = "readwrite" }},
        workgroups = { x = 2 }, output = 1 })
    return (string.unpack("<f", out))
end
DBL = dbl
app.get("/dbl", function(req, res) res:json({ v = dbl(5) }) end)
app.main(function(ctx)
    if not GPU then return 3 end
    ctx.stdout:write(tostring(dbl(tonumber(ctx.args[1]))), "\n")
    return 0
end)
LUA
    cat > "$1/tests/test_gpu.lua" <<LUA
test("hull.gpu loads under hull test as on the server", function()
    test.eq(GPU ~= nil, $EXPECT)
end)
if $EXPECT then
    test("a test dispatches a shader", function() test.eq(DBL(3), 6) end)
    test("a route dispatches under test.get", function()
        local res = test.get("/dbl")
        test.eq(res.status, 200)
        test.eq(res.json.v, 10)
    end)
    test("app.main dispatches under test.run_main", function()
        local r = test.run_main({ "4" })
        test.eq(r.code, 0)
        test.eq(r.stdout, "8.0\n")
    end)
end
LUA
}

mk_js() {   # $1 = dir
    mkdir -p "$1/shaders" "$1/tests"
    printf '%s\n' "$SHADER" > "$1/shaders/dbl.wgsl"
    cat > "$1/app.js" <<'JS'
import { app } from "hull:app";
import { gpu } from "hull:gpu";
app.manifest({ gpu: true, modules: ["hull/gpu@1", "hull/http-server@1"] });
const G = gpu && gpu.available() ? gpu : null;
if (G) G.load("dbl");
function dbl(x) {
    const out = G.dispatch("dbl", {
        buffers: [{ data: new Float32Array([x, x + 1]).buffer, usage: "readwrite" }],
        workgroups: { x: 2 }, output: 0 });
    return new Float32Array(out)[0];
}
globalThis.DBL = dbl;
globalThis.HAS_GPU = G !== null;
app.get("/dbl", (req, res) => res.json({ v: dbl(5) }));
app.main((ctx) => {
    if (!G) return 3;
    ctx.stdout.write(String(dbl(Number(ctx.args[0]))) + "\n");
    return 0;
});
JS
    cat > "$1/tests/test_gpu.js" <<JS
test("hull:gpu loads under hull test as on the server", () => {
    test.eq(HAS_GPU, $EXPECT);
});
if ($EXPECT) {
    test("a test dispatches a shader", () => { test.eq(DBL(3), 6); });
    test("a route dispatches under test.get", () => {
        const res = test.get("/dbl");
        test.eq(res.status, 200);
        test.eq(res.json.v, 10);
    });
    test("app.main dispatches under test.runMain", () => {
        const r = test.runMain(["4"]);
        test.eq(r.code, 0);
        test.eq(r.stdout, "8\n");
    });
}
JS
}

run() {   # $1 = label, $2 = dir, $3 = the number of tests it must pass
    OUT=$("$HULL" test "$2" 2>&1) || { echo "$OUT"; echo "FAIL: hull test ($1)"; exit 1; }
    echo "$OUT" | grep -q "^$3/$3 tests passed" \
        || { echo "$OUT"; echo "FAIL: $1: want $3/$3 tests passed"; exit 1; }
    echo "$OUT" | grep -E "PASS|tests passed" | sed 's/^/    /'
    echo "ok  hull test ($1)"
    AOUT=$("$HULL" agent test "$2" 2>&1) || { echo "$AOUT"; echo "FAIL: hull agent test ($1)"; exit 1; }
    echo "$AOUT" | grep -q '"failed": *0' || echo "$AOUT" | grep -q '"failed":0' \
        || { echo "$AOUT"; echo "FAIL: hull agent test ($1)"; exit 1; }
    echo "ok  hull agent test ($1)"
}

if [ "$EXPECT" = true ]; then N=4; else N=1; fi
mk_lua "$WORK/lua"; run lua "$WORK/lua" "$N"
if [ "$EXPECT" = true ]; then
    mk_js "$WORK/js"; run js "$WORK/js" 4
else
    echo "skip  js (hull:gpu does not load without a device)"
fi

echo "=== an app that does not declare hull/gpu gets no gpu.* under test ==="
mkdir -p "$WORK/undeclared/tests"
cat > "$WORK/undeclared/app.lua" <<'LUA'
app.manifest({ modules = { "hull/http-server@1" } })
app.get("/", function(req, res) res:text("ok") end)
LUA
cat > "$WORK/undeclared/tests/test_x.lua" <<'LUA'
test("hull.gpu is refused when undeclared", function()
    local ok = pcall(require, "hull.gpu")
    test.eq(ok, false)
end)
LUA
run undeclared "$WORK/undeclared" 1

echo "PASS: e2e_gpu_hull_test"
