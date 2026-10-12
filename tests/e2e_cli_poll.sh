#!/bin/sh
# Real CLI entry points on an HTTP-free build, with both runtimes enabled.
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
HULL_BIN="${HULL_BIN:-build/hull}"
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL_BIN"
work=$(mktemp -d)
trap 'rm -f "$work"/*.db* "$work"/output*; rmdir "$work"' EXIT
out="$work/output"
fail() { echo "FAIL: $1"; cat "$out" 2>/dev/null || true; exit 1; }

# Optional compiler/tool readiness can make doctor exit nonzero. Inspect the
# build-capability data; this suite runs apps and does not need those tools.
doctor=$("$HULL_BIN" doctor --json || true)
echo "$doctor" | grep -q '"http":false' || fail 'this suite requires an HTTP-free build'
echo "$doctor" | grep -q '"runtime":"lua+js"' || fail 'this suite requires both runtimes'

for ext in lua js; do
    dbpath="$work/$ext.db"
    if [ "$HULL_RC_SHIFT" = 1 ]; then dbpath=$(cygpath -m "$dbpath"); fi
    for expected in 0 7; do
        if [ "$expected" = 7 ]; then arg=exit7; else arg=ok; fi
        rc=$(hull_run "$out" "$HULL_BIN" "tests/fixtures/cli_poll/app.$ext" -d "$dbpath" -- "$arg")
        [ "$rc" = "$expected" ] || fail "$ext exit $rc, expected $expected"
        grep -q '^poll-cli: timers=32 workers=32 db=32 total=560' "$out" || fail "$ext did not complete every operation"
        echo "PASS: $ext timers, worker imports/completions, async DB, exit $expected"
    done
done

for mode in resolve reject; do
    expected=0
    [ "$mode" != reject ] || expected=1
    rc=$(hull_run "$out" "$HULL_BIN" tests/fixtures/cli_poll/settlement.js -- "$mode")
    [ "$rc" = "$expected" ] || fail "JS $mode exit $rc, expected $expected"
    if [ "$mode" = reject ]; then
        grep -q 'expected CLI rejection' "$out" || fail 'JS rejection diagnostic missing'
    else
        grep -q 'settled with a pending timer' "$out" || fail 'JS main did not settle'
    fi
    echo "PASS: JS $mode with a pending timer tears down cleanly"
done
echo '6/6 poll CLI end-to-end tests passed'
