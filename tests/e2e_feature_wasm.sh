#!/bin/sh
# e2e_feature_wasm.sh - WASM as a composable feature: the compute-free slim
# invariant + the needs_wasm two-signal gate (docs/wasm_feature.md).
#
# Proves:
#   - the base platform lib is COMPUTE-LESS (0 WAMR symbols);
#   - a compute-free app skips the WASM feature (binary has 0 WAMR symbols) and
#     still runs;
#   - a compute app (S1: declares hull/compute) composes WASM and compute.call
#     actually executes in the produced binary;
#   - a .wasm-only app (S2: ships compute/*.wasm, declares no compute module)
#     composes WASM via the second signal;
#   - both runtimes behave symmetrically.
# Locks in the base-flip + gate so they can't silently regress.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

HULL=./build/hull
ECHO_WASM=examples/compute/compute/echo.wasm

echo "=== build hull + the compute-less platform lib + the wasm archives ==="
make >/dev/null
make platform >/dev/null
# The base is compute-less: a produced compute app composes the wasm core +
# its runtime's compute bridge, so hull build needs them in build/ (found by
# build.lua). The distributed hull embeds them instead.
make feature-wasm feature-wasm-lua feature-wasm-js >/dev/null

# Count DEFINED symbols matching $2 (exclude undefined 'U'/'u' refs). Mach-O
# prefixes an underscore; ELF does not. We assert what a binary DEFINES.
count_syms() { nm "$1" 2>/dev/null | grep -cE " [A-TV-Za-tv-z] _?$2" || true; }

echo "=== the base platform lib is COMPUTE-LESS (0 WAMR symbols) ==="
base_wamr=$(count_syms build/libhull_platform.a "wasm_runtime_full_init")
echo "libhull_platform.a: WAMR=$base_wamr"
[ "$base_wamr" -eq 0 ] || { echo "FAIL: base platform lib still carries WAMR"; exit 1; }
echo "ok  base platform lib carries no WASM runtime"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ── fixtures ────────────────────────────────────────────────────────────
# Lua: compute app (S1), compute-free, and .wasm-only (S2).
mkdir -p "$WORK/lua_compute/compute" "$WORK/lua_free" "$WORK/lua_s2/compute"
cp "$ECHO_WASM" "$WORK/lua_compute/compute/"
cp "$ECHO_WASM" "$WORK/lua_s2/compute/"
cat > "$WORK/lua_compute/app.lua" <<'LUA'
local compute = require("hull.compute")
app.manifest({ compute = true, modules = { "hull/compute@1" } })
app.main(function()
    if not compute.available() then return 9 end
    local out = compute.call("echo", "ping")
    return out == "ping" and 0 or 7
end)
LUA
cat > "$WORK/lua_free/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 4 end)
LUA
# S2: ships compute/*.wasm but declares NO compute module (the WASM-backed-udf
# shape). The gate must still compose WASM.
cat > "$WORK/lua_s2/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 0 end)
LUA

# JS: compute app + compute-free.
mkdir -p "$WORK/js_compute/compute" "$WORK/js_free"
cp "$ECHO_WASM" "$WORK/js_compute/compute/"
cat > "$WORK/js_compute/app.js" <<'JS'
import { app } from "hull:app";
import { compute } from "hull:compute";
app.manifest({ compute: true, modules: ["hull/compute@1"] });
app.main(() => {
    if (!compute.available()) return 9;
    const o = compute.call("echo", "ping");
    return (o && o.byteLength === 4) ? 0 : 7;   // "ping" = 4 bytes
});
JS
cat > "$WORK/js_free/app.js" <<'JS'
import { app } from "hull:app";
app.manifest({ modules: [] });
app.main(() => 5);
JS

build() { "$HULL" build --no-verify-platform --compiler=system -o "$1/bin" "$1" >/tmp/wasm_build.log 2>&1; }

# ── 1. compute app (S1): composes WASM + compute.call runs ──────────────
echo "=== Lua compute app (S1) composes WASM + compute.call executes ==="
build "$WORK/lua_compute" || { cat /tmp/wasm_build.log; echo "FAIL: lua compute build"; exit 1; }
w=$(count_syms "$WORK/lua_compute/bin" "wasm_runtime_full_init")
[ "$w" -ge 1 ] || { echo "FAIL: lua compute app has no WAMR ($w)"; exit 1; }
set +e; "$WORK/lua_compute/bin" >/dev/null 2>&1; rc=$?; set -e
[ "$rc" -eq 0 ] || { echo "FAIL: lua compute.call did not return 'ping' (exit $rc)"; exit 1; }
echo "ok  lua compute app: WAMR composed + compute.call('echo','ping')=='ping'"

# ── 2. compute-free app: skips WASM (0 WAMR), still runs ────────────────
echo "=== Lua compute-free app skips WASM (0 WAMR) ==="
build "$WORK/lua_free" || { cat /tmp/wasm_build.log; echo "FAIL: lua free build"; exit 1; }
w=$(count_syms "$WORK/lua_free/bin" "wasm_runtime_full_init")
[ "$w" -eq 0 ] || { echo "FAIL: compute-free lua app still has WAMR ($w)"; exit 1; }
set +e; "$WORK/lua_free/bin" >/dev/null 2>&1; rc=$?; set -e
[ "$rc" -eq 4 ] || { echo "FAIL: compute-free lua app did not run (exit $rc)"; exit 1; }
echo "ok  lua compute-free app: 0 WAMR symbols + runs"

# ── 3. .wasm-only app (S2): composes WASM via the second signal ─────────
echo "=== Lua .wasm-only app (S2, no hull/compute) composes WASM ==="
build "$WORK/lua_s2" || { cat /tmp/wasm_build.log; echo "FAIL: lua S2 build"; exit 1; }
w=$(count_syms "$WORK/lua_s2/bin" "wasm_runtime_full_init")
[ "$w" -ge 1 ] || { echo "FAIL: S2 app (ships .wasm) did not compose WASM ($w)"; exit 1; }
echo "ok  S2 app composed WASM from compute/*.wasm alone"

# ── 4. JS symmetric ─────────────────────────────────────────────────────
echo "=== JS compute app composes WASM + compute.call executes ==="
build "$WORK/js_compute" || { cat /tmp/wasm_build.log; echo "FAIL: js compute build"; exit 1; }
w=$(count_syms "$WORK/js_compute/bin" "wasm_runtime_full_init")
[ "$w" -ge 1 ] || { echo "FAIL: js compute app has no WAMR ($w)"; exit 1; }
set +e; "$WORK/js_compute/bin" >/dev/null 2>&1; rc=$?; set -e
[ "$rc" -eq 0 ] || { echo "FAIL: js compute.call did not return 4 bytes (exit $rc)"; exit 1; }
echo "ok  js compute app: WAMR composed + compute.call executes"

echo "=== JS compute-free app skips WASM (0 WAMR) ==="
build "$WORK/js_free" || { cat /tmp/wasm_build.log; echo "FAIL: js free build"; exit 1; }
w=$(count_syms "$WORK/js_free/bin" "wasm_runtime_full_init")
[ "$w" -eq 0 ] || { echo "FAIL: compute-free js app still has WAMR ($w)"; exit 1; }
set +e; "$WORK/js_free/bin" >/dev/null 2>&1; rc=$?; set -e
[ "$rc" -eq 5 ] || { echo "FAIL: compute-free js app did not run (exit $rc)"; exit 1; }
echo "ok  js compute-free app: 0 WAMR symbols + runs"

echo "PASS: e2e_feature_wasm (base compute-less; needs_wasm gate; compute.call in composed apps, both runtimes)"
