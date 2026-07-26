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

# ── 3. reduced-flavor x runtime is deferred (fails closed, issue #114) ──
#
# The runtime-featurify epic makes the native base runtime-less; the full-config
# runtime archive's web bindings reference subsystems a reduced flavor drops, so
# composing a runtime onto a reduced-flavor base cannot link yet. `hull build`
# fails CLOSED with a clear pointer to the HTTP-as-a-feature epic rather than
# emitting a raw linker error. Re-enable the build+run assertions (exit 7, zero
# mbedTLS hashing symbols) when https://github.com/artalis-io/hull/issues/114
# lands. The flavor VALIDATION (parts 1-2) and auto-SELECTION (below) still work.
if out=$("$HULL" build --flavor=pure-compute "$WORK/pc" -o "$WORK/pc/app" 2>&1); then
    fail "reduced-flavor x runtime should fail closed until #114"
fi
echo "$out" | grep -q "isn't supported yet with the composed-runtime model" \
    || fail "expected the deferred-flavor message: $out"
echo "$out" | grep -q "issues/114" || fail "expected the #114 tracking pointer: $out"
pass "reduced-flavor x runtime fails closed with the #114 pointer"

# ── 4. --flavor=auto still infers the minimal flavor from the manifest ──
# Selection logic is independent of the compose gap: auto still picks
# pure-compute for an app.main app; the build then fails closed as in part 3.
out=$("$HULL" build --flavor=auto "$WORK/pc" -o "$WORK/pc/auto" 2>&1) || true
echo "$out" | grep -q "auto selected 'pure-compute'" \
    || fail "auto should select pure-compute for an app.main app: $out"
echo "$out" | grep -q "isn't supported yet with the composed-runtime model" \
    || fail "auto pure-compute build should fail closed until #114: $out"
pass "--flavor=auto -> pure-compute selection (build deferred per #114)"

echo "PASS: e2e_build_flavor"
