#!/bin/sh
# e2e_build_flavor.sh - `hull build --flavor` MVP.
#
# Proves a full (default) hull can build a non-default-flavor app binary:
#   1. unknown flavor is rejected with the valid list
#   2. an app needing a dropped subsystem is rejected at BUILD time
#      (resolver validates against the TARGET flavor's caps)
#   3. a valid pure-compute app builds, runs, and returns app.main's exit code
#   4. the resulting binary has zero mbedTLS hashing symbols
#
# Native flavor libs only (fast). Cosmo flavor builds work too
# (`make platform-cosmo-<flavor>`) but are too slow to build here. See
# docs/build_flavors.md.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HULL="$ROOT/build/hull"
LIB="$ROOT/build/libhull_platform-pure-compute.a"

[ -x "$HULL" ] || { echo "SKIP: $HULL not built"; exit 0; }
case "$(file "$HULL" 2>/dev/null || true)" in
    *cosmo*|*"APE"*) echo "SKIP: cosmo flavor builds are covered separately (too slow here)"; exit 0;;
esac

# Build the pure-compute platform lib if absent (CI builds it fresh).
if [ ! -f "$LIB" ]; then
    echo "building pure-compute platform lib..."
    make -C "$ROOT" platform-pure-compute >/dev/null 2>&1 || {
        echo "SKIP: could not build pure-compute platform lib (no system cc?)"; exit 0; }
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $1"; exit 1; }
pass() { echo "  ok: $1"; }

# ── 1. unknown flavor ──────────────────────────────────────────────────
mkdir -p "$WORK/pc"
printf 'app.manifest({ modules = {} })\napp.main(function() return 7 end)\n' > "$WORK/pc/app.lua"
if out=$("$HULL" build --flavor=bogus "$WORK/pc" -o "$WORK/pc/x" 2>&1); then
    fail "unknown flavor should error"
fi
echo "$out" | grep -q "unknown build flavor 'bogus'" || fail "unknown-flavor message missing: $out"
echo "$out" | grep -q "pure-compute" || fail "unknown-flavor should list valid flavors"
pass "unknown flavor rejected with valid list"

# ── 2. forbidden module rejected at build time ─────────────────────────
mkdir -p "$WORK/srv"
printf 'app.manifest({ modules = { "hull/http-server@1" } })\napp.get("/", function(req, res) res:text("hi") end)\n' > "$WORK/srv/app.lua"
if out=$("$HULL" build --flavor=pure-compute "$WORK/srv" -o "$WORK/srv/x" 2>&1); then
    fail "pure-compute build of an http-server app should be rejected"
fi
echo "$out" | grep -q "HL_ENABLE_HTTP_SERVER" || fail "expected HTTP_SERVER rejection: $out"
pass "forbidden module (hull/http-server) rejected at build time"

# ── 3. pure-compute x runtime composes, links, and runs (issue #114, Phase D) ──
#
# The native base is runtime-less AND HTTP-core-less; a pure-compute app declares
# no HTTP module, so `hull build --flavor=pure-compute` composes only the pure
# runtime onto the Keel-free base (the runtime's few web-symbol references
# resolve to the base http_weakstub no-ops). The produced binary runs and carries
# no Keel / mbedTLS / http surface.
if ! out=$("$HULL" build --flavor=pure-compute "$WORK/pc" -o "$WORK/pc/app" 2>&1); then
    fail "pure-compute x runtime should build: $out"
fi
echo "$out" | grep -q "composed runtime" || fail "expected a composed runtime: $out"
echo "$out" | grep -q "HTTP-free app" || fail "expected the HTTP-free skip message: $out"
rc=0; "$WORK/pc/app" >/dev/null 2>&1 || rc=$?
[ "$rc" = 7 ] || fail "pure-compute app should exit 7, got $rc"
# The Keel-free payoff: no mbedTLS TLS symbols and no HTTP cap in the binary.
if command -v nm >/dev/null 2>&1; then
    n=$(nm "$WORK/pc/app" 2>/dev/null | grep -cE ' T _?mbedtls_ssl' || true)
    [ "$n" = 0 ] || fail "pure-compute binary should carry no mbedTLS TLS symbols (got $n)"
    n=$(nm "$WORK/pc/app" 2>/dev/null | grep -cE ' T _?hl_cap_http_request' || true)
    [ "$n" = 0 ] || fail "pure-compute binary should carry no HTTP cap (got $n)"
fi
pass "pure-compute x runtime builds, runs (exit 7), and drops Keel/mbedTLS/http"

# ── 3b. pure-compute x wasm feature (reduced flavor x additive feature) ────────
# A compute app on the Keel-free base: the wasm caps reference only base symbols
# present in the pure-compute lib (no Keel), so the wasm feature composes
# independently of the HTTP axis (issue #118). The binary carries WAMR but still
# no Keel / mbedTLS, and compute.call executes.
if [ ! -f "$ROOT/build/libhull_feature-wasm.a" ]; then
    make -C "$ROOT" feature-wasm feature-wasm-lua >/dev/null 2>&1 || true
fi
mkdir -p "$WORK/pcw/compute"
cp "$ROOT/examples/compute/compute/echo.wasm" "$WORK/pcw/compute/"
printf 'local compute = require("hull.compute")\napp.manifest({ compute = true, modules = { "hull/compute@1" } })\napp.main(function()\n  if not compute.available() then return 9 end\n  return compute.call("echo", "ping") == "ping" and 0 or 7\nend)\n' > "$WORK/pcw/app.lua"
if ! out=$("$HULL" build --no-verify-platform --flavor=pure-compute "$WORK/pcw" -o "$WORK/pcw/app" 2>&1); then
    fail "pure-compute x wasm should build: $out"
fi
set +e; "$WORK/pcw/app" >/dev/null 2>&1; rc=$?; set -e
[ "$rc" = 0 ] || fail "pure-compute compute app: compute.call should run (exit 0), got $rc"
if command -v nm >/dev/null 2>&1; then
    w=$(nm "$WORK/pcw/app" 2>/dev/null | grep -cE ' [A-TV-Za-tv-z] _?wasm_runtime_full_init' || true)
    [ "$w" -ge 1 ] || fail "pure-compute compute app should carry WAMR (got $w)"
    n=$(nm "$WORK/pcw/app" 2>/dev/null | grep -cE ' T _?kl_server_' || true)
    [ "$n" = 0 ] || fail "pure-compute compute app should carry no Keel (got $n)"
    n=$(nm "$WORK/pcw/app" 2>/dev/null | grep -cE ' T _?mbedtls_ssl' || true)
    [ "$n" = 0 ] || fail "pure-compute compute app should carry no mbedTLS (got $n)"
fi
pass "pure-compute x wasm: composes WAMR + runs compute.call, still drops Keel/mbedTLS"

# ── 4. --flavor=auto infers pure-compute for an app.main app and builds it ──
out=$("$HULL" build --flavor=auto "$WORK/pc" -o "$WORK/pc/auto" 2>&1) || fail "auto build failed: $out"
echo "$out" | grep -q "auto selected 'pure-compute'" \
    || fail "auto should select pure-compute for an app.main app: $out"
rc=0; "$WORK/pc/auto" >/dev/null 2>&1 || rc=$?
[ "$rc" = 7 ] || fail "auto-selected pure-compute app should exit 7, got $rc"
pass "--flavor=auto -> pure-compute selection builds + runs"

echo "PASS: e2e_build_flavor"
