#!/bin/sh
# e2e_feature_tls.sh — TLS as a composable feature (docs/tls_feature.md, a2).
#
# Proves the "Keel move" compose end to end: a tls-less base
# (HL_TLS_FEATURE=1: drops mbedTLS + the crypto/tls transport backends + Keel's
# tls_mbedtls.o, keeping the weak hl_crypto_*_active_backend / hl_tls_* seams
# plus the full non-TLS crypto core) plus libhull_feature-tls.a, joined by a
# plain `hull build`, links a real HTTPS-capable app through the composed
# mbedTLS. The base carries no mbedTLS; the produced app gets it from the
# archive only when it needs TLS.
#
# The build dances across two config-sentinel states (TLS=1 for the archive,
# TLS=0 for the base), so it stashes + restores build/hull + the platform lib
# and leaves build/ as it found it. Heavy (two clean rebuilds); its own CI job.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
HULL="$ROOT/build/hull"
[ -x "$HULL" ] || { echo "SKIP: build/hull not built"; exit 0; }
case "$(file "$HULL" 2>/dev/null || true)" in
    *cosmo*|*"APE"*) echo "SKIP: cosmo keeps mbedTLS in-base (fat APE can't force-load)"; exit 0;;
esac
command -v nm >/dev/null 2>&1 || { echo "SKIP: nm unavailable"; exit 0; }

W=""
STASH=$(mktemp -d)
cp "$HULL" "$STASH/hull"                                   # the default toolchain
[ -f build/libhull_platform.a ] && cp build/libhull_platform.a "$STASH/plat.a" || true
cleanup() {
    # Restore build/ to the default toolchain + platform lib we started with.
    cp "$STASH/hull" build/hull 2>/dev/null || true
    [ -f "$STASH/plat.a" ] && cp "$STASH/plat.a" build/libhull_platform.a 2>/dev/null || true
    rm -rf "$STASH"
    [ -n "$W" ] && rm -rf "$W" || true
}
trap cleanup EXIT
fail() { echo "FAIL: $1"; exit 1; }

hs=' [Tt] _\{0,1\}mbedtls_ssl_handshake$'   # a strong (defined) handshake symbol

# 1. The feature archive (TLS=1 config).
make feature-tls >/dev/null 2>&1 || fail "make feature-tls"
cp build/libhull_feature-tls.a "$STASH/feat.a"

# 2. The tls-less base (TLS=0). The sentinel clean wipes build/hull.
make platform HL_TLS_FEATURE=1 >/dev/null 2>&1 || fail "make platform HL_TLS_FEATURE=1"
n=$(nm build/libhull_platform.a 2>/dev/null | grep -c "$hs" || true)
[ "$n" = 0 ] || fail "base is not tls-less ($n mbedtls_ssl_handshake symbols)"
# The non-TLS crypto core must survive (SHA-256 is the cap-layer transform, not mbedTLS).
nm build/libhull_platform.a 2>/dev/null | grep -q hl_cap_crypto_sha256 \
    || fail "base lost the crypto core"
# The weak transport seam stays in the base so tls-free apps link.
nm build/libhull_platform.a 2>/dev/null | grep -q hl_tls_client_ctx_create \
    || fail "base lost the tls transport seam"
echo "ok  tls-less base builds (0 mbedtls_ssl_handshake, crypto core + seam intact)"

# 3. Assemble: default toolchain hull + the tls-less base + the archive.
cp "$STASH/hull" build/hull
cp "$STASH/feat.a" build/libhull_feature-tls.a

# 4. Compose a TLS-needing app: an outbound-HTTPS (http-client) app implies the
# TLS stack. `hull build` sees needs_tls + a tls-less base and composes
# libhull_feature-tls.a on its own (auto-inference, no --with=tls).
W=$(mktemp -d)
mkdir -p "$W/web"
cat > "$W/web/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/http-server@1", "hull/http-client@1" },
    hosts = { "example.com" },
})
app.get("/", function(req, res) res:json({ ok = true }) end)
LUA
out=$("$HULL" build --no-verify-platform "$W/web" -o "$W/web/bin" 2>&1) \
    || fail "hull build (auto-infer tls onto the tls-less base): $out"
echo "$out" | grep -qi "composed TLS feature" \
    || fail "expected auto-inference to compose tls (no --with), got: $out"
w=$(nm "$W/web/bin" 2>/dev/null | grep -c "$hs" || true)
[ "$w" -ge 1 ] || fail "auto-composed HTTPS app has no mbedTLS (should come from the archive)"
echo "ok  auto-inference: plain hull build on a tls-less base composes tls (HTTPS app links mbedTLS)"

# 5. THE PAYOFF — a tls-free CLI app on the tls-less base composes no TLS, so the
# produced binary links with ZERO mbedTLS (the weak hl_tls_* seam fails closed;
# release_io's signature verify still works, it just can't open an HTTPS socket).
mkdir -p "$W/cli"
cat > "$W/cli/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function(ctx) ctx.stdout:write("CLI_OK\n") return 0 end)
LUA
out=$("$HULL" build --no-verify-platform "$W/cli" -o "$W/cli/bin" 2>&1) \
    || fail "hull build (tls-free app on the tls-less base should link clean): $out"
echo "$out" | grep -qi "composed TLS feature" \
    && fail "tls-free app should compose no TLS, got: $out" || true
n=$(nm "$W/cli/bin" 2>/dev/null | grep -c "$hs" || true)
[ "$n" = 0 ] || fail "tls-free app still carries mbedTLS ($n mbedtls_ssl_handshake) — the drop regressed"
rc=0; "$W/cli/bin" >/dev/null 2>&1 || rc=$?
[ "$rc" = 0 ] || fail "tls-free app should run (exit 0), got $rc"
echo "ok  PAYOFF: tls-free app on a tls-less base drops mbedTLS entirely (0 mbedtls_ssl_handshake) + runs"

echo "PASS: e2e_feature_tls (tls-less base; HTTPS app composes mbedTLS; tls-free drops it)"
