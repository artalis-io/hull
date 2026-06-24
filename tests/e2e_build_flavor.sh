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
# Native only (cosmo flavor libs are out of MVP scope). See docs/build_flavors.md.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HULL="$ROOT/build/hull"
LIB="$ROOT/build/libhull_platform-pure-compute.a"

[ -x "$HULL" ] || { echo "SKIP: $HULL not built"; exit 0; }
case "$(file "$HULL" 2>/dev/null || true)" in
    *cosmo*|*"APE"*) echo "SKIP: --flavor not supported for cosmo builds yet"; exit 0;;
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

# ── 3. valid pure-compute app builds, runs, returns app.main exit ──────
"$HULL" build --flavor=pure-compute "$WORK/pc" -o "$WORK/pc/app" >/dev/null 2>&1 \
    || fail "valid pure-compute build failed"
[ -x "$WORK/pc/app" ] || fail "pure-compute binary not produced"
set +e
"$WORK/pc/app" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 7 ] || fail "expected app.main exit 7, got $rc"
pass "pure-compute app builds, runs, returns exit 7"

# ── 4. no mbedTLS hashing symbols in the pure-compute binary ───────────
if command -v nm >/dev/null 2>&1; then
    n=$(nm "$WORK/pc/app" 2>/dev/null | grep -cE 'mbedtls_sha|mbedtls_md_hmac' || true)
    [ "$n" -eq 0 ] || fail "pure-compute binary has $n mbedTLS hashing symbols"
    pass "zero mbedTLS hashing symbols"
fi

# ── 5. --flavor=auto infers the minimal flavor from the manifest ───────
out=$("$HULL" build --flavor=auto "$WORK/pc" -o "$WORK/pc/auto" 2>&1) \
    || fail "auto build failed: $out"
echo "$out" | grep -q "auto selected 'pure-compute'" \
    || fail "auto should select pure-compute for an app.main app: $out"
set +e
"$WORK/pc/auto" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 7 ] || fail "auto pure-compute binary should exit 7, got $rc"
pass "--flavor=auto -> pure-compute for an app.main app"

echo "PASS: e2e_build_flavor"
