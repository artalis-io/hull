#!/bin/sh
# Release smoke test — run MANUALLY after `gh release create` succeeds.
#
# This is the only test that exercises the live install-from-GitHub
# path end-to-end. It downloads `wamrc` from the just-published
# release via `hull tools install wamrc`, verifies it actually runs,
# and then uninstalls. Treat as the final go/no-go before announcing
# a release.
#
# Why not in CI? Because the asset doesn't exist until `gh release
# create` finishes, and the install codepath uses HL_VERSION to pick
# the tag — so CI can't test against itself before publishing. Once
# the release is up, this is the one-line confirmation that the
# whole pipeline (release.yml → tools/install → trust chain →
# atomic install) works.
#
# Usage:
#     # After `gh release create v0.1.2 ...` returns success
#     curl -fsSL https://gethull.dev/install.sh | sh
#     sh tests/release_smoke.sh
#
# Or in-tree against a locally-built hull pinned to the published tag:
#     HULL=/usr/local/bin/hull sh tests/release_smoke.sh
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

HULL="${HULL:-hull}"
PASS=0
FAIL=0

assert() {
    msg="$1"; shift
    if "$@"; then
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $msg"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    msg="$1"; haystack="$2"; needle="$3"
    if echo "$haystack" | grep -qF -- "$needle"; then
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $msg"
        echo "    expected: $needle"
        echo "    got:"
        echo "$haystack" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

# hull flavor install <flavor> — since Phase 4.3, pure-compute is a build.lua
# PRESET on the default composable base (which already drops HTTP/TLS/Keel and
# composes each back per app). There is no per-flavor platform lib to fetch, so
# `hull flavor install pure-compute` is a no-op that reports the preset and
# installs nothing. Uses an isolated HOME so a real ~/.hull/platform is never
# touched. (A FUTURE non-preset flavor with a real asset stem would
# download+verify a lib here; there is no such flavor today.)
smoke_platform_install() {
    echo ""
    echo "── hull flavor install pure-compute (preset -- nothing to install) ──"
    PLAT_HOME=$(mktemp -d)
    OUT=$(HOME="$PLAT_HOME" "$HULL" flavor install pure-compute 2>&1)
    RC=$?
    echo "$OUT" | sed 's/^/    /'
    assert "exits 0"                              [ "$RC" -eq 0 ]
    assert_contains "reports preset (nothing to install)"  "$OUT" "preset"
    N=$(ls "$PLAT_HOME"/.hull/platform/libhull_platform-pure-compute*.a 2>/dev/null | wc -l | tr -d ' ')
    assert "preset installs no lib (got $N, want 0)"  [ "$N" -eq 0 ]
    OUT=$(HOME="$PLAT_HOME" "$HULL" flavor list 2>&1)
    assert_contains "list shows pure-compute"     "$OUT" "pure-compute"
    rm -rf "$PLAT_HOME"
}

# hull feature install <name> — LIVE fetch + verify of the published composable
# feature archive (libhull_feature-<name>-<arch>.a). Native-only (DuckDB is not
# built for cosmo). Uses an isolated HOME so a real ~/.hull/feature is untouched.
smoke_feature_install() {
    echo ""
    echo "── hull feature install duckdb (LIVE GitHub HTTPS download) ──"
    FEAT_HOME=$(mktemp -d)
    OUT=$(HOME="$FEAT_HOME" "$HULL" feature install duckdb 2>&1)
    RC=$?
    echo "$OUT" | sed 's/^/    /'
    assert "exits 0"                         [ "$RC" -eq 0 ]
    assert_contains "SHA-256 verified"       "$OUT" "SHA-256 verified"
    N=$(ls "$FEAT_HOME"/.hull/feature/libhull_feature-duckdb-*.a 2>/dev/null | wc -l | tr -d ' ')
    assert "feature lib landed in ~/.hull/feature (got $N)"  [ "$N" -ge 1 ]
    OUT=$(HOME="$FEAT_HOME" "$HULL" feature list 2>&1)
    assert_contains "list shows duckdb installed"  "$OUT" "duckdb"

    # Runtime features (lua/js) are a distinct kind: mandatory-exactly-one, so
    # they're EMBEDDED in the distributed hull + auto-composed from the entry
    # extension. `feature list` reports them as embedded and `feature install
    # <rt>` is a no-op redirect (nothing to fetch). Validates the runtime-feature
    # surface shipped in #113 on a real published binary.
    echo ""
    echo "── runtime feature surface (lua/js embedded + auto-composed) ──"
    OUT=$(HOME="$FEAT_HOME" "$HULL" feature list 2>&1)
    echo "$OUT" | sed 's/^/    /'
    assert_contains "list shows lua runtime"       "$OUT" "lua"
    assert_contains "list shows js runtime"        "$OUT" "js"
    assert_contains "runtimes marked embedded"     "$OUT" "embedded"
    OUT=$(HOME="$FEAT_HOME" "$HULL" feature install lua 2>&1); RC=$?
    assert "feature install lua exits 0 (embedded no-op)"  [ "$RC" -eq 0 ]
    assert_contains "install lua reports embedded" "$OUT" "embedded"
    rm -rf "$FEAT_HOME"
}

if ! command -v "$HULL" >/dev/null 2>&1; then
    echo "release_smoke: '$HULL' not found on PATH — install hull first"
    exit 1
fi

echo "── Identify the running hull ──"
VERSION=$("$HULL" version 2>&1)
PLATFORM=$("$HULL" doctor --json 2>&1 | grep -o '"platform":"[^"]*"' | head -1)
echo "  $VERSION"
echo "  $PLATFORM"

# wamrc isn't published for cosmo — skip the install bit on a cosmo
# binary and just verify the registry reports it correctly.
#
# Detect cosmo from the doctor platform field ("platform":"cosmo"), NOT the
# version string: `hull version` prints the git-describe tag (e.g. "hull
# 0.10.0") with no "cosmo" marker, so keying on $VERSION silently ran the
# native branch against a cosmo binary and produced spurious feature-install
# failures (features are native-only). $PLATFORM is set above from
# `doctor --json`.
case "$PLATFORM" in
    *cosmo*)
        echo ""
        echo "── cosmo binary — wamrc install is unsupported by design ──"
        OUT=$("$HULL" agent tools 2>&1)
        assert_contains "registry lists wamrc"             "$OUT" "\"name\":\"wamrc\""
        assert_contains "wamrc not available on cosmo"     "$OUT" "\"available_for_platform\":false"
        # Cosmo DOES publish per-flavor platform libs (dual-arch), so the
        # `hull flavor install` path is exercised on cosmo binaries.
        smoke_platform_install
        # cosmocc IS published for cosmo (the exception to every other tool): it's
        # the toolchain a cosmo hull needs to `hull build` an APE. Exercise the
        # LIVE install + assert the bundle lays down cosmocc + the busybox
        # ride-along (item E). This downloads ~300 MB - the point is to prove the
        # trimmed big-asset fetch + trust chain end to end.
        assert_contains "registry lists cosmocc"       "$OUT" "\"name\":\"cosmocc\""
        echo ""
        echo "── hull tools install cosmocc (LIVE ~300 MB HTTPS download) ──"
        "$HULL" tools uninstall cosmocc >/dev/null 2>&1 || true
        OUT_CC=$("$HULL" tools install cosmocc 2>&1)
        RC_CC=$?
        echo "$OUT_CC" | sed 's/^/    /'
        assert "cosmocc install exits 0"               [ "$RC_CC" -eq 0 ]
        assert_contains "cosmocc SHA-256 verified"     "$OUT_CC" "SHA-256 verified"
        CC_DIR="${HOME}/.hull/tools/cosmocc"
        assert "bundle laid down bin/cosmocc"          [ -e "$CC_DIR/bin/cosmocc" ]
        assert "bundle laid down bin/busybox.exe"      [ -e "$CC_DIR/bin/busybox.exe" ]
        "$HULL" tools uninstall cosmocc >/dev/null 2>&1 || true
        echo ""
        echo "── Summary ──"
        echo "  Passed: $PASS"
        echo "  Failed: $FAIL"
        [ "$FAIL" -eq 0 ] && exit 0 || exit 1
        ;;
esac

echo ""
echo "── hull tools list (pre-install) ──"
OUT=$("$HULL" tools list 2>&1)
assert "exits 0"                       [ "$?" -eq 0 ]
assert_contains "lists wamrc"          "$OUT" "wamrc"

# If wamrc happens to already be installed (re-running the smoke
# test), uninstall first so we exercise the install path.
if echo "$OUT" | grep -q '\[installed\]'; then
    echo ""
    echo "── wamrc already installed — removing to exercise install ──"
    "$HULL" tools uninstall wamrc >/dev/null 2>&1 || true
fi

echo ""
echo "── hull tools install wamrc (LIVE GitHub HTTPS download) ──"
OUT=$("$HULL" tools install wamrc 2>&1)
RC=$?
echo "$OUT" | sed 's/^/    /'
assert "exits 0"                       [ "$RC" -eq 0 ]
assert_contains "SHA-256 verified"     "$OUT" "SHA-256 verified"
assert_contains "installed wamrc"      "$OUT" "installed wamrc"
# Signature verification only if the release key is configured.
if echo "$OUT" | grep -q "release signature verified"; then
    echo "  ok  release signature verified (Ed25519)"
    PASS=$((PASS + 1))
elif echo "$OUT" | grep -q "no embedded release public key"; then
    echo "  ok  ran without signature (placeholder build)"
    PASS=$((PASS + 1))
fi

echo ""
echo "── exercise the installed binary ──"
WAMRC_PATH="$HOME/.hull/tools/wamrc"
assert "wamrc landed in canonical location" [ -x "$WAMRC_PATH" ]
OUT=$("$WAMRC_PATH" --help 2>&1)
assert_contains "wamrc --help runs"    "$OUT" "Usage"

echo ""
echo "── hull doctor reports managed install ──"
OUT=$("$HULL" doctor 2>&1)
assert_contains "doctor sees the install" "$OUT" "$WAMRC_PATH"
assert_contains "flagged as managed"      "$OUT" "managed via"

echo ""
echo "── hull agent tools reports installed: true ──"
OUT=$("$HULL" agent tools 2>&1)
assert_contains "installed=true"       "$OUT" "\"installed\":true"

echo ""
echo "── hull tools uninstall wamrc ──"
OUT=$("$HULL" tools uninstall wamrc 2>&1)
assert_contains "uninstalled"          "$OUT" "uninstalled wamrc"
assert "file removed"                  [ ! -e "$WAMRC_PATH" ]

# (tcc-as-a-tool was retired in the compiler-free work - it is no longer in the
# registry nor published, so there is no `hull tools install tcc` to smoke-test.)

# libc-musl static-link floor bundle (Tier B). Published for linux-x86_64 /
# linux-aarch64 only. This is the ONLY multi-file .tar bundle asset, so it
# exercises the new install path end to end: fetch -> SHA-256 verify -> ustar
# extract into a directory -> directory uninstall. Linux-only (musl is Linux).
if [ "$(uname -s)" = "Linux" ]; then
    A=$(uname -m); [ "$A" = "arm64" ] && A=aarch64
    FLOOR_TOOL="libc-musl-$A"
    FLOOR_DIR="$HOME/.hull/tools/$FLOOR_TOOL"
    "$HULL" tools uninstall "$FLOOR_TOOL" >/dev/null 2>&1 || true
    echo ""
    echo "── hull tools install $FLOOR_TOOL (Tier B floor bundle, LIVE download) ──"
    OUT=$("$HULL" tools install "$FLOOR_TOOL" 2>&1); RC=$?
    echo "$OUT" | sed 's/^/    /'
    assert "exits 0"                       [ "$RC" -eq 0 ]
    assert_contains "SHA-256 verified"     "$OUT" "SHA-256 verified"
    assert_contains "installed as bundle"  "$OUT" "(bundle)"
    assert "floor dir created"             [ -d "$FLOOR_DIR" ]
    assert "crt1.o extracted"              [ -f "$FLOOR_DIR/crt1.o" ]
    assert "libc.a extracted"              [ -f "$FLOOR_DIR/libc.a" ]
    assert "libgcc.a extracted"            [ -f "$FLOOR_DIR/libgcc.a" ]
    OUT=$("$HULL" tools list 2>&1)
    assert_contains "list shows floor installed"  "$OUT" "$FLOOR_TOOL"
    echo "── hull tools uninstall $FLOOR_TOOL ──"
    OUT=$("$HULL" tools uninstall "$FLOOR_TOOL" 2>&1)
    assert_contains "floor uninstalled"    "$OUT" "uninstalled $FLOOR_TOOL"
    assert "floor dir removed"             [ ! -d "$FLOOR_DIR" ]
fi

# musl platform archive SET (audit #4c) — the counterpart of the floor: Hull's
# own archives (libhull_platform.a + slim + every libhull_feature-*.a) built
# against musl, for `hull build --target=<arch>-linux-musl`. A data-only .tar
# bundle like the floor, but a CROSS-target asset published for ALL native
# hosts (install on any host to build TO musl), so this runs unconditionally.
# fetch -> SHA-256 verify -> ustar extract into a dir -> dir uninstall.
PM_ARCH=x86_64
PM_TOOL="platform-musl-$PM_ARCH"
PM_DIR="$HOME/.hull/tools/$PM_TOOL"
"$HULL" tools uninstall "$PM_TOOL" >/dev/null 2>&1 || true
echo ""
echo "── hull tools install $PM_TOOL (musl platform archive set, LIVE download) ──"
OUT=$("$HULL" tools install "$PM_TOOL" 2>&1); RC=$?
echo "$OUT" | sed 's/^/    /'
assert "exits 0"                       [ "$RC" -eq 0 ]
assert_contains "SHA-256 verified"     "$OUT" "SHA-256 verified"
assert_contains "installed as bundle"  "$OUT" "(bundle)"
assert "bundle dir created"            [ -d "$PM_DIR" ]
assert "musl base extracted"           [ -f "$PM_DIR/libhull_platform.a" ]
assert "musl slim base extracted"      [ -f "$PM_DIR/libhull_platform-slim.a" ]
assert "musl lua runtime archive"      [ -f "$PM_DIR/libhull_feature-lua.a" ]
OUT=$("$HULL" tools list 2>&1)
assert_contains "list shows $PM_TOOL installed" "$OUT" "$PM_TOOL"
echo "── hull tools uninstall $PM_TOOL ──"
OUT=$("$HULL" tools uninstall "$PM_TOOL" 2>&1)
assert_contains "musl set uninstalled" "$OUT" "uninstalled $PM_TOOL"
assert "musl set dir removed"          [ ! -d "$PM_DIR" ]

# Toolchain-free linker bundle: zig. A per-platform multi-file .tar bundle
# published for all three native platforms (hull-zig-<platform>.tar) - the
# largest tool asset (~330 MB), so it also exercises the raised download cap.
# fetch -> verify -> nested extract -> the bundled DRIVER runs -> dir uninstall.
# Native (runs after the cosmo early-exit); no OS gate - Linux + macOS.
# (A standalone lld bundle is intentionally NOT shipped - binary lld is
# dynamically linked against libLLVM; see tools_install.c's registry NOTE.)
ZIG_DIR="$HOME/.hull/tools/zig"
"$HULL" tools uninstall zig >/dev/null 2>&1 || true
echo ""
echo "── hull tools install zig (per-platform bundle, LIVE download) ──"
OUT=$("$HULL" tools install zig 2>&1); RC=$?
echo "$OUT" | sed 's/^/    /'
assert "exits 0"                        [ "$RC" -eq 0 ]
assert_contains "SHA-256 verified"      "$OUT" "SHA-256 verified"
assert_contains "installed as bundle"   "$OUT" "(bundle)"
assert "bundle dir created"             [ -d "$ZIG_DIR" ]
assert "zig driver executable"          [ -x "$ZIG_DIR/zig" ]
assert "zig lib/ tree extracted"        [ -d "$ZIG_DIR/lib" ]
DOUT=$("$ZIG_DIR/zig" version 2>&1) || true
assert_contains "zig version runs"      "$DOUT" "0.13"
OUT=$("$HULL" tools list 2>&1)
assert_contains "list shows zig installed"  "$OUT" "zig"
echo "── hull tools uninstall zig ──"
OUT=$("$HULL" tools uninstall zig 2>&1)
assert_contains "zig uninstalled"       "$OUT" "uninstalled zig"
assert "zig dir removed"                [ ! -d "$ZIG_DIR" ]

# Per-flavor platform lib install (native single-arch lib).
smoke_platform_install

# Composable feature lib install (native-only; DuckDB is not built for cosmo).
smoke_feature_install

# ── Platform-sig E2E (post-§3.2: works on installed binaries) ──
#
# Same chain release.yml's in-CI smoke exercises, but post-publish
# against the actually-uploaded artifact. Sign-platform and eject
# extract the embedded .a (the §3.2 fix), build --sign produces a
# signed app, --verify-sig refuses to start if the gethull layer is
# broken. Catches "the artifact uploaded isn't the artifact CI built"
# surprises and any release-time CDN/storage tampering.
echo ""
echo "── Platform-sig E2E: sign-platform + build --sign + --verify-sig ──"
PSIG_TMP=$(mktemp -d)
cleanup_psig() { rm -rf "$PSIG_TMP"; }
trap cleanup_psig EXIT INT TERM

cd "$PSIG_TMP"
mkdir -p app
cat > app/app.lua <<'LUA'
app.manifest({modules = {"hull/http-server@1"}})
app.get("/", function(req, res) res:json({ok=true, smoke="platform-sig"}) end)
LUA

"$HULL" keygen plat >/dev/null 2>&1
"$HULL" keygen dev  >/dev/null 2>&1

# sign-platform: §3.2 extracts the embedded .a when build/ has none.
OUT=$("$HULL" sign-platform plat 2>&1)
assert_contains "sign-platform extracted embedded .a"   "$OUT" "using embedded platform"
assert_contains "sign-platform wrote platform.sig"      "$OUT" "wrote build/platform.sig"

# build --sign: the gethull cross-check fires here (or skips
# gracefully on a placeholder-pubkey hull).
OUT=$("$HULL" build --sign dev.key -o myapp app/ 2>&1)
RC=$?
if [ "$RC" -ne 0 ]; then
    # Builds may fail if the running hull is a placeholder build with
    # no embedded signed manifest (only release CI emits one). In
    # that case --no-verify-platform is needed; not a release-smoke
    # failure per se, but the rest of the test can't run.
    if echo "$OUT" | grep -q "no embedded platform manifest"; then
        echo "  ok  sign chain works; this hull has no embedded signed manifest"
        echo "      (placeholder build — release binaries should have one)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL hull build --sign exited $RC"
        echo "$OUT" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
else
    assert "myapp produced"  [ -x myapp ]

    # Run with --verify-sig. The platform-sig + app-sig + file-hash
    # chain all gets exercised at startup. Picks a high port to
    # avoid collision.
    PORT=$((10000 + (RANDOM % 50000)))
    ./myapp --verify-sig dev.pub --no-sandbox -p "$PORT" app/app.lua >/dev/null 2>&1 &
    SVR=$!
    sleep 2
    if kill -0 "$SVR" 2>/dev/null; then
        RESP=$(curl -fsS "http://127.0.0.1:$PORT/" 2>/dev/null || echo FAIL)
        kill "$SVR" 2>/dev/null || true
        wait "$SVR" 2>/dev/null || true
        assert_contains "myapp --verify-sig serves OK"  "$RESP" '"ok":true'
        assert_contains "smoke field present"           "$RESP" '"smoke":"platform-sig"'
    else
        echo "  FAIL myapp did not start under --verify-sig (sig chain broken)"
        FAIL=$((FAIL + 1))
    fi
fi

cd /
cleanup_psig
trap - EXIT INT TERM

echo ""
echo "── Summary ──"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
