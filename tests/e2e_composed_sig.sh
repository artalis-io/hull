#!/bin/sh
# e2e_composed_sig.sh - end-to-end test for the composed-feature signature layer
# (issue #114, docs/composed_feature_signing.md).
#
# The gethull platform-sig layer is a placeholder in normal dev builds (all-zeros
# HL_PLATFORM_PUBKEY_HEX -> verification skipped). So this test builds its OWN
# throwaway signing chain with a TEST platform key, mirroring the release
# pipeline's sign-platform-manifest job (release.yml), and pins that test key
# into the platform lib + hull via EXTRA_CFLAGS. Then it exercises the full
# chain: build an app, confirm the composed attestation lands in package.sig, run
# it under the gethull verify, and confirm a tampered attestation is fatal.
#
# Stands up the test-key hull + platform lib + embedded feature archives, then:
#   Phase 1: an app builds --sign, the gethull cross-check passes, package.sig
#            carries the gethull block.
#   Phase 2: package.sig.gethull.composed records the composed archives, each with
#            a hash matching both the on-disk archive and the signed manifest.
#   Runtime: the app boots under the ACTIVE gethull verify (5b base + 5c composed),
#            exits 3.
#   Phase 3: a tampered composed hash is fatal at runtime (5c refuses to boot).
#
# macOS/Linux native only (uses the local toolchain to rebuild the platform lib
# + hull with a test key). Skips on cosmo.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

case "$(uname -s)" in
    Darwin) OS=darwin ;;
    Linux)  OS=linux ;;
    *) echo "SKIP: unsupported OS"; exit 0 ;;
esac
case "$(uname -m)" in
    arm64|aarch64) MA=$( [ "$OS" = darwin ] && echo arm64 || echo aarch64 ) ;;
    x86_64|amd64)  MA=x86_64 ;;
    *) echo "SKIP: unsupported arch"; exit 0 ;;
esac
ARCH="$OS-$MA"
echo "== composed-sig e2e (arch: $ARCH) =="

command -v xxd >/dev/null 2>&1   || { echo "SKIP: xxd not found"; exit 0; }
command -v sha256sum >/dev/null 2>&1 && SHA=sha256sum || SHA="shasum -a 256"

WORK=$(mktemp -d)
SIG_H="$ROOT/build/embedded_platform_sig.h"
# Everything below writes TEST-KEY artifacts into build/ (the signed manifest
# header, platform.sig, and objects compiled with the test pubkey). On exit,
# remove them so a subsequent normal `make` rebuilds clean (the removed objects
# force a fresh compile with the real/placeholder pubkey).
cleanup() {
    rm -rf "$WORK"
    rm -f "$ROOT/build/embedded_platform_sig.h" "$ROOT/build/platform.sig" \
          "$ROOT/build/signature.o" "$ROOT/build/platform_sig.o" \
          "$ROOT/build/mod_tool.o" "$ROOT/build/embedded_platform_sig.o" \
          "$ROOT/build/build_assets.o" "$ROOT/build/hull" \
          "$ROOT/build/libhull_platform.a"
    echo "(cleaned test-key build artifacts; run 'make' to restore a normal build)"
}
trap cleanup EXIT

fail() { echo "FAIL: $1"; exit 1; }
pass() { echo "  ok: $1"; }

# ── 1. test platform keypair ────────────────────────────────────────────
./build/hull keygen platform >/dev/null 2>&1 \
    || { echo "SKIP: hull keygen unavailable"; exit 0; }
# keygen writes into $PWD; move them out of the tree
mv platform.key platform.pub "$WORK/" 2>/dev/null || fail "keygen produced no keys"
PUB=$(tr -d '\n' < "$WORK/platform.pub")
[ "${#PUB}" = 64 ] || fail "test pubkey is not 64 hex chars: '$PUB'"
# The macro is a string literal, so the quotes must survive the make recipe's
# shell (same escaping as the Makefile's -DHL_VERSION=\"...\"): pass a literal
# backslash-quote so the compiler ultimately sees -D...="<hex>".
XCF="-DHL_PLATFORM_PUBKEY_HEX=\\\"$PUB\\\""

# ── 2. build the test-key platform lib + the embedded feature archives ──
# EXTRA_CFLAGS changing does not invalidate existing .o's, so remove the objects
# that bake in HL_PLATFORM_PUBKEY_HEX (signature/platform_sig/mod_tool) + the
# platform lib + hull so they recompile with the TEST key.
echo "-- building platform lib (test key) + feature archives --"
rm -f build/signature.o build/platform_sig.o build/mod_tool.o \
      build/embedded_platform_sig.o build/build_assets.o \
      build/hull build/libhull_platform.a
make EXTRA_CFLAGS="$XCF" >/dev/null 2>&1 || fail "make (hull + feature archives) failed"
make platform EXTRA_CFLAGS="$XCF" >/dev/null 2>&1 || fail "make platform (test key) failed"

# ── 3. build + sign the extended platform manifest (mirrors release.yml) ──
# The platform lib entry keeps its bare-arch name (existing format); each
# embedded feature archive is listed as "<asset>.<arch>.a".
echo "-- signing the extended manifest with the test key --"
MAN="$WORK/manifest.txt"
{
    $SHA build/libhull_platform.a | awk -v a="$ARCH" '{print $1"  "a}'
    for f in lua js http http-lua http-js tui-lua tui-js wasm wasm-lua wasm-js; do
        A="build/libhull_feature-$f.a"
        [ -f "$A" ] || continue
        $SHA "$A" | awk -v n="libhull_feature-$f.$ARCH.a" '{print $1"  "n}'
    done
} | LC_ALL=C sort -k2,2 > "$MAN"
echo "   manifest ($(wc -l < "$MAN") entries):"; sed 's/^/     /' "$MAN"

./build/hull sign-release "$MAN" --key "$WORK/platform.key" >/dev/null 2>&1 \
    || fail "sign-release (platform manifest) failed"
test -s "$MAN.sig" || fail "no signature produced"
./build/hull verify-release "$MAN" "$MAN.sig" --pubkey "$PUB" >/dev/null 2>&1 \
    || fail "self-check: test-signed manifest did not verify"
pass "extended manifest signed + self-verifies under the test key"

# ── 4. embed the signed manifest + rebuild hull with the test key ───────
echo "-- embedding the signed manifest, rebuilding hull --"
{
    echo '/* Auto-generated by tests/e2e_composed_sig.sh - test key, do not ship */'
    xxd -i -n hl_embedded_platform_sig_manifest "$MAN"
    xxd -i -n hl_embedded_platform_sig_signature "$MAN.sig"
} > "$SIG_H"
[ "$(wc -c < "$SIG_H")" -gt 1024 ] || fail "embedded_platform_sig.h too small (<1KB gate)"
# Only the embed-dependent objects need to pick up the new header (signature.o
# etc. already carry the test key from step 2). Relink hull.
rm -f build/embedded_platform_sig.o build/build_assets.o build/hull
make EXTRA_CFLAGS="$XCF" >/dev/null 2>&1 || fail "hull rebuild (test key + embedded sig) failed"
./build/hull version >/dev/null 2>&1 || fail "test hull does not run"
pass "test hull built with the test key + test-signed embedded manifest"

# ── 5. developer keys + sign the (test-key) platform lib ────────────────
# hull build --sign requires a developer key AND a platform.sig (the developer
# platform layer). Mirror tests/e2e_build.sh: keygen, copy the platform lib into
# a sandbox-writable dir, sign-platform, copy platform.sig into build/.
HULL="$ROOT/build/hull"
( cd "$WORK" && "$HULL" keygen >/dev/null 2>&1 ) || fail "developer keygen failed"
cp build/libhull_platform.a "$WORK/"
cp build/platform_canary_hash "$WORK/" 2>/dev/null || true
"$HULL" sign-platform --dir "$WORK/" "$WORK/developer" >/dev/null 2>&1 \
    || fail "sign-platform failed"
test -f "$WORK/platform.sig" || fail "sign-platform wrote no platform.sig"
cp "$WORK/platform.sig" build/

# ── 6. Phase 1: build + boot under the ACTIVE gethull verify (test key) ──
# hull build --sign cross-checks the platform lib against the embedded manifest
# and writes package.sig (developer app-sig + gethull platform layer). Running
# WITHOUT --no-verify-platform then verifies the gethull block against the test
# pubkey linked into the app.
APP="$WORK/app"; mkdir -p "$APP"
printf 'app.manifest({ modules = {} })\napp.main(function() return 3 end)\n' > "$APP/app.lua"

BUILD_OUT=$("$HULL" build --compiler=system --sign "$WORK/developer.key" \
            -o "$APP/bin" "$APP" 2>&1) \
    || { echo "$BUILD_OUT"; fail "hull build --sign under the test hull (gethull cross-check active)"; }
test -f "$APP/package.sig" || fail "no package.sig produced"
grep -q '"gethull"' "$APP/package.sig" || fail "package.sig has no gethull block"
pass "app builds --sign; gethull cross-check passes; package.sig carries the gethull block"

# ── Phase 2: the composed-feature attestation block ─────────────────────
# The app is app.main + modules={} (HTTP-free, lua runtime), so exactly ONE
# platform-domain archive is composed: the lua runtime lib. Assert it's recorded
# with a hash that matches BOTH the on-disk archive and the signed manifest, and
# that no http/tui asset leaked in (HTTP-free build) and release_domain is empty.
RT_ASSET="libhull_feature-lua.$ARCH.a"
command -v jq >/dev/null 2>&1 || { echo "SKIP-phase2: jq not found"; }
if command -v jq >/dev/null 2>&1; then
    jq -e '.platform.gethull.composed' "$APP/package.sig" >/dev/null \
        || fail "package.sig has no gethull.composed block"
    # runtime archive present in platform_domain with the right hash
    REC_HASH=$(jq -r --arg n "$RT_ASSET" \
        '.platform.gethull.composed.platform_domain.assets[]
         | select(.name==$n) | .sha256' "$APP/package.sig")
    [ -n "$REC_HASH" ] || fail "composed platform_domain missing runtime asset $RT_ASSET"
    DISK_HASH=$($SHA build/libhull_feature-lua.a | awk '{print $1}')
    [ "$REC_HASH" = "$DISK_HASH" ] \
        || fail "composed runtime hash $REC_HASH != on-disk $DISK_HASH"
    MAN_HASH=$(awk -v n="$RT_ASSET" '$2==n {print $1}' "$MAN")
    [ "$REC_HASH" = "$MAN_HASH" ] \
        || fail "composed runtime hash $REC_HASH != signed-manifest $MAN_HASH"
    # HTTP-free app: no http/tui platform assets, empty release_domain
    N_PLAT=$(jq '.platform.gethull.composed.platform_domain.assets | length' "$APP/package.sig")
    [ "$N_PLAT" = 1 ] || fail "expected exactly 1 platform-domain asset, got $N_PLAT"
    jq -e '.platform.gethull.composed.platform_domain.assets[]
           | select(.name|test("http|tui"))' "$APP/package.sig" >/dev/null 2>&1 \
        && fail "http/tui asset leaked into an HTTP-free app's composed block" || true
    N_REL=$(jq '.platform.gethull.composed.release_domain.assets | length' "$APP/package.sig")
    [ "$N_REL" = 0 ] || fail "expected empty release_domain, got $N_REL assets"
    pass "composed block records the runtime archive; hash matches disk + signed manifest"
fi

# ── Runtime verify with the gethull layer ACTIVE (5b base + 5c composed) ──
# --verify-sig <pubkey-file> runs hl_verify_startup, which reads ./package.sig
# from disk (CWD-relative) and, absent --no-verify-platform, verifies BOTH the
# base platform-sig (5b) and the composed-feature attestation (5c) against the
# test pubkey linked into this hull. app.main returns 3, so a clean verified boot
# exits 3; any [sig] refusal exits non-3.
DEV_PUB="$WORK/developer.pub"
test -f "$DEV_PUB" || fail "developer.pub missing (keygen layout changed)"
run_app() {  # $1 = extra args; captures stdout+stderr to $RUN_OUT, sets RC
    RUN_OUT=$(mktemp)
    set +e
    ( cd "$APP" && ./bin --verify-sig "$DEV_PUB" $1 ) >"$RUN_OUT" 2>&1
    RC=$?
    set -e
}

run_app ""
if grep -qiE "\[sig\]" "$RUN_OUT" && grep -qiE "refus|verification failed|mismatch|invalid" "$RUN_OUT"; then
    cat "$RUN_OUT"; rm -f "$RUN_OUT"
    fail "test hull refused a validly-signed app under the ACTIVE gethull verify"
fi
[ "$RC" = 3 ] || { cat "$RUN_OUT"; rm -f "$RUN_OUT"; fail "app.main should exit 3 (got $RC) under the active gethull verify"; }
rm -f "$RUN_OUT"
pass "validly-signed app verifies (5b base + 5c composed) + exits 3 (test key)"

# ── Phase 3: a tampered composed hash is FATAL at runtime (5c) ───────────
# Flip the recorded runtime-archive hash in package.sig's composed block. The
# platform-key manifest is untouched, so 5b still passes; 5c then finds the
# recorded hash absent from the signed manifest and must refuse to boot. 5c runs
# before the app-signature step, so the refusal is specifically the composed
# attestation (message contains "composed"), not a generic app-sig failure.
if command -v jq >/dev/null 2>&1; then
    cp "$APP/package.sig" "$WORK/package.sig.orig"
    BAD="ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
    jq --arg n "$RT_ASSET" --arg h "$BAD" \
        '(.platform.gethull.composed.platform_domain.assets[]
          | select(.name==$n) | .sha256) = $h' \
        "$WORK/package.sig.orig" > "$APP/package.sig"
    run_app ""
    if [ "$RC" = 3 ]; then
        cat "$RUN_OUT"; rm -f "$RUN_OUT"
        cp "$WORK/package.sig.orig" "$APP/package.sig"
        fail "tampered composed hash was NOT rejected (booted + exited 3)"
    fi
    grep -qi "composed" "$RUN_OUT" \
        || { cat "$RUN_OUT"; rm -f "$RUN_OUT"; cp "$WORK/package.sig.orig" "$APP/package.sig"; \
             fail "tamper rejected, but not via the composed-attestation path"; }
    rm -f "$RUN_OUT"
    cp "$WORK/package.sig.orig" "$APP/package.sig"
    pass "tampered composed-feature hash is fatal at runtime (5c refuses boot)"
else
    echo "SKIP-phase3: jq not found"
fi

echo "PASS: composed-sig e2e (harness + composed-block attestation + runtime tamper)"
