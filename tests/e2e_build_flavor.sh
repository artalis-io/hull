#!/bin/sh
# e2e_build_flavor.sh - `hull build --flavor` (pure-compute as a PRESET).
#
# (docs/keel_feature.md): `pure-compute` is no longer a pre-built
# platform lib -- it is a build.lua PRESET on the DEFAULT composable base, which
# drops HTTP/TLS/Keel and composes each back per app. So the flavor is a
# validation contract (reject any HTTP/TLS app) and the size payoff comes from
# the composable base itself. This proves:
#   1. unknown flavor is rejected with the valid list
#   2. an HTTP app is rejected at BUILD time under --flavor=pure-compute
#      (resolver validates against the flavor's cleared caps)
#   3. a valid pure-compute app builds, runs, and returns app.main's exit code
#   4. --flavor=auto infers pure-compute for an app.main app
#   5. on a Keel+TLS-less base (the composable release base), a compute app
#      links ZERO Keel + ZERO mbedTLS -- the real payoff.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HULL="$ROOT/build/hull"

[ -x "$HULL" ] || { echo "SKIP: $HULL not built"; exit 0; }
case "$(file "$HULL" 2>/dev/null || true)" in
    *cosmo*|*"APE"*) echo "SKIP: cosmo keeps everything in-base (fat APE)"; exit 0;;
esac

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $1"; exit 1; }
pass() { echo "  ok: $1"; }

# ── 1. unknown flavor ──────────────────────────────────────────────────
mkdir -p "$WORK/pc"
printf 'app.manifest({ modules = {} })\napp.main(function() return 7 end)\n' > "$WORK/pc/app.lua"
if out=$("$HULL" build --no-verify-platform --flavor=bogus "$WORK/pc" -o "$WORK/pc/x" 2>&1); then
    fail "unknown flavor should error"
fi
echo "$out" | grep -q "unknown build flavor 'bogus'" || fail "unknown-flavor message missing: $out"
echo "$out" | grep -q "pure-compute" || fail "unknown-flavor should list valid flavors"
pass "unknown flavor rejected with valid list"

# ── 2. forbidden module rejected at build time (validation preset) ─────
mkdir -p "$WORK/srv"
printf 'app.manifest({ modules = { "hull/http-server@1" } })\napp.get("/", function(req, res) res:text("hi") end)\n' > "$WORK/srv/app.lua"
if out=$("$HULL" build --no-verify-platform --flavor=pure-compute "$WORK/srv" -o "$WORK/srv/x" 2>&1); then
    fail "pure-compute build of an http-server app should be rejected"
fi
echo "$out" | grep -q "HL_ENABLE_HTTP_SERVER" || fail "expected HTTP_SERVER rejection: $out"
pass "forbidden module (hull/http-server) rejected at build time"

# ── 3. pure-compute app builds + runs on the default base ──────────────
if ! out=$("$HULL" build --no-verify-platform --flavor=pure-compute "$WORK/pc" -o "$WORK/pc/app" 2>&1); then
    fail "pure-compute x runtime should build: $out"
fi
rc=0; "$WORK/pc/app" >/dev/null 2>&1 || rc=$?
[ "$rc" = 7 ] || fail "pure-compute app should exit 7, got $rc"
pass "pure-compute (preset) builds on the default base + runs (exit 7)"

# ── 4. --flavor=auto infers pure-compute for an app.main app ───────────
out=$("$HULL" build --no-verify-platform --flavor=auto "$WORK/pc" -o "$WORK/pc/auto" 2>&1) \
    || fail "auto build failed: $out"
echo "$out" | grep -q "auto selected 'pure-compute'" \
    || fail "auto should select pure-compute for an app.main app: $out"
rc=0; "$WORK/pc/auto" >/dev/null 2>&1 || rc=$?
[ "$rc" = 7 ] || fail "auto-selected pure-compute app should exit 7, got $rc"
pass "--flavor=auto -> pure-compute selection builds + runs"

# ── 5. THE PAYOFF: a compute app on a Keel+TLS-less base links zero Keel/mbedTLS ─
# pure-compute's size win now comes from the composable base, not a flavor lib.
# Build a Keel-less + TLS-less base (the release's SLIM base minus SQLite) and a
# compute app on it (a non-embedded hull falls back to build/libhull_platform.a);
# assert the drop. The base sub-build fingerprint-purges build/hull + the feature
# archives, so stash + restore them. nm required.
command -v nm >/dev/null 2>&1 || { echo "PASS: e2e_build_flavor (nm absent; skipped drop check)"; exit 0; }
S=$(mktemp -d)
cp "$HULL" "$S/hull"; cp "$ROOT"/build/libhull_feature-*.a "$S/" 2>/dev/null || true
restore() { cp "$S/hull" "$ROOT/build/hull" 2>/dev/null || true
            cp "$S"/libhull_feature-*.a "$ROOT/build/" 2>/dev/null || true; rm -rf "$S"; }
make -C "$ROOT" platform HL_KEEL_FEATURE=1 HL_TLS_FEATURE=1 >/dev/null 2>&1 \
    || { restore; echo "SKIP: could not build the Keel+TLS-less base (no system cc?)"; exit 0; }
restore
out=$("$HULL" build --no-verify-platform --flavor=pure-compute "$WORK/pc" -o "$WORK/pc/slim" 2>&1) \
    || { echo "$out"; fail "pure-compute app on the Keel+TLS-less base should build"; }
k=$(nm "$WORK/pc/slim" 2>/dev/null | grep -cE ' [Tt] _?kl_' || true)
m=$(nm "$WORK/pc/slim" 2>/dev/null | grep -cE ' [Tt] _?mbedtls_ssl_handshake' || true)
[ "$k" = 0 ] || fail "compute app on the Keel-less base should carry no Keel (got $k)"
[ "$m" = 0 ] || fail "compute app on the TLS-less base should carry no mbedTLS (got $m)"
rc=0; "$WORK/pc/slim" >/dev/null 2>&1 || rc=$?
[ "$rc" = 7 ] || fail "the Keel/TLS-less pure-compute app should still exit 7, got $rc"
# Leave build/ as a full base again for any following target. This flag flip back to
# the default config makes the sentinel purge build/hull; `make platform` alone rebuilds
# only the platform LIB, not the binary, leaving following targets (e2e-ca-bundle,
# e2e-project-discovery, ...) to relink a stale/incomplete build/hull from the mutable
# tree. Build the DEFAULT target so build/hull is fully rebuilt + correct.
make -C "$ROOT" >/dev/null 2>&1 || true
pass "compute app on a Keel+TLS-less base drops Keel + mbedTLS entirely (0/0) + runs"

echo "PASS: e2e_build_flavor"
