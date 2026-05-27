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
case "$VERSION" in
    *cosmo*)
        echo ""
        echo "── cosmo binary — wamrc install is unsupported by design ──"
        OUT=$("$HULL" agent tools 2>&1)
        assert_contains "registry lists wamrc"             "$OUT" "\"name\":\"wamrc\""
        assert_contains "wamrc not available on cosmo"     "$OUT" "\"available_for_platform\":false"
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

echo ""
echo "── Summary ──"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
