#!/bin/sh
# E2E test for `hull update`.
#
# Verifies:
#   1. --help prints usage and exits 0
#   2. --check against the real repo (no releases yet → 404, clean error)
#   3. --check against a stable public repo (cli/cli) - exercises full
#      HTTPS + GitHub API + JSON-parse + version-compare path using the
#      embedded CA bundle
#   4. Unknown flags don't crash
#
# Does NOT exercise actual download + replace (that would clobber the
# test binary). The compile-time tests already cover SHA-256 + atomic
# rename indirectly via the keel/mbedTLS unit tests.
#
# Usage: sh tests/e2e_update.sh
#        make e2e-update
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"

# On Windows an APE's exit status reaches this shell shifted left by 8, so
# `$?` is 0 for every outcome (jart/cosmopolitan#1521); hull_run recovers the
# real one. A direct call everywhere else.
. "$SRCDIR/tests/lib/hull_rc.sh"
hull_rc_init "$HULL"
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

if [ ! -x "$HULL" ]; then
    echo "FAIL: $HULL not found - run 'make' first"
    exit 1
fi

echo "── hull update --help ──"
OUT=$("$HULL" update --help 2>&1)
RC=$?
assert "exits 0" [ "$RC" -eq 0 ]
assert_contains "shows usage" "$OUT" "Usage: hull update"
assert_contains "documents --check" "$OUT" "--check"
assert_contains "documents --force" "$OUT" "--force"
assert_contains "documents --repo" "$OUT" "--repo"

# Skip network tests if no connectivity
if ! curl -fsSL --max-time 5 https://api.github.com >/dev/null 2>&1; then
    echo ""
    echo "  skip: no network access - skipping live update checks"
    echo ""
    echo "── Summary ──"
    echo "  Passed: $PASS"
    echo "  Failed: $FAIL"
    [ "$FAIL" -eq 0 ] && exit 0 || exit 1
fi

echo ""
echo "── hull update --check (no releases yet) ──"
OUT=$("$HULL" update --check 2>&1 || true)
assert_contains "checks the default repo" "$OUT" "checking artalis-io/hull"
# Either: returned 404 (no releases yet) or found a real release
case "$OUT" in
    *"returned HTTP 404"*|*"failed to fetch"*|*"could not parse"*)
        echo "  ok  no-release path handled cleanly"
        PASS=$((PASS + 1))
        ;;
    *"already up to date"*|*"update available"*|*"is not newer than this hull"*)
        echo "  ok  found a real release (artalis-io/hull has tags now)"
        PASS=$((PASS + 1))
        ;;
    *)
        echo "  FAIL unexpected output from --check against artalis-io/hull"
        echo "$OUT" | sed 's/^/    /'
        FAIL=$((FAIL + 1))
        ;;
esac

echo ""
echo "── hull update --check against a public repo with releases ──"
# cli/cli (gh) is a stable, widely-cached GitHub repo that always has releases.
# This exercises the full HTTPS-to-GitHub-API + JSON-parse path using the
# embedded CA bundle. The asset name (hull-linux-x86_64 / hull-cosmo / etc.)
# won't match cli/cli's assets, but --check stops before that.
OUT=$("$HULL" update --check --repo=cli/cli 2>&1 || true)
assert_contains "checks cli/cli"     "$OUT" "checking cli/cli"
assert_contains "shows current"      "$OUT" "current"
assert_contains "shows latest tag"   "$OUT" "latest"

# Either "update available" (most cases - different version) or
# "already up to date" (if HL_VERSION happens to be the same string).
case "$OUT" in
    *"update available"*|*"already up to date"*)
        echo "  ok  HTTPS + GitHub API + JSON parse work via embedded CA bundle"
        PASS=$((PASS + 1))
        ;;
    *)
        echo "  FAIL did not detect update / up-to-date state"
        FAIL=$((FAIL + 1))
        ;;
esac

echo ""
echo "── unknown flags ──"
# Refused up front (it used to be ignored, so a typo did nothing silently),
# before any network access.
RC_OUT="${TMPDIR:-/tmp}/hull_update_rc.$$"
RC=$(hull_run "$RC_OUT" "$HULL" update --unknown-flag)
OUT=$(cat "$RC_OUT"); rm -f "$RC_OUT"
assert_contains "unknown flag refused" "$OUT" "unknown option '--unknown-flag'"
if [ "$RC" -ne 0 ]; then
    echo "  ok  unknown flag exits non-zero"; PASS=$((PASS + 1))
else
    echo "  FAIL unknown flag exited 0"; FAIL=$((FAIL + 1))
fi
case "$OUT" in
    *checking*) echo "  FAIL unknown flag still reached the network"; FAIL=$((FAIL + 1)) ;;
    *)          echo "  ok  refused before any network access"; PASS=$((PASS + 1)) ;;
esac
OUT=$("$HULL" update --channel=beta 2>&1 || true)
assert_contains "only the stable channel" "$OUT" "only the stable channel"

echo ""
echo "── Summary ──"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
