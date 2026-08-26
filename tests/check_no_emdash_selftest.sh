#!/bin/sh
# check_no_emdash_selftest.sh - deterministic negative test: prove
# check_no_emdash.sh BITES on an em-dash planted in living scope, and returns
# CLEAN once it is removed. H1 / S5.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

GATE="sh tests/check_no_emdash.sh"
EM=$(printf '\342\200\224')
PROBE="src/hull/__selftest_emdash_probe.h"   # in living scope (src/)
FAILED=0
pass() { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAILED=$((FAILED + 1)); }

cleanup() {
    git reset -q -- "$PROBE" 2>/dev/null || true
    rm -f "$PROBE"
}
trap cleanup EXIT INT TERM

# 1. Baseline: the real tree must already be clean.
if $GATE >/dev/null 2>&1; then
    pass "baseline living scope is clean"
else
    bad "baseline living scope is NOT clean (real em-dash present)"
fi

# 2. Plant an em-dash in a tracked (intent-to-add) in-scope file -> gate must bite.
printf '/* selftest %s bite */\n' "$EM" > "$PROBE"
git add -N "$PROBE" 2>/dev/null   # intent-to-add so `git ls-files` sees it
out=$($GATE 2>&1); rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "$PROBE"; then
    pass "planted em-dash in src/ -> gate BITES"
else
    bad "planted em-dash -> gate did NOT bite (rc=$rc)"
fi

# 3. Remove the probe -> gate returns clean again.
cleanup
if $GATE >/dev/null 2>&1; then
    pass "probe removed -> gate CLEAN again"
else
    bad "probe removed -> gate still failing"
fi

[ "$FAILED" -eq 0 ] && { echo "check-no-emdash selftest: all negative checks pass"; exit 0; }
echo "check-no-emdash selftest: $FAILED failure(s)"; exit 1
