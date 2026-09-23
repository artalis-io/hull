#!/bin/sh
# check_module_deps_selftest.sh - deterministic negative test: prove
# check_module_deps.sh BITES on an undeclared dependency, and returns CLEAN
# once it is removed.
#
# The gate's whole value is that it fails on the shape that shipped in
# hull/ssh. A gate nobody has seen fail is a gate nobody knows works - and
# this one has already been wrong once (its first version read only the FIRST
# line of a wrapped .deps array, so it reported modules that do declare their
# deps). That near-miss is the argument for this file: without it, the fix
# would have been to edit the registry until the gate went quiet.
#
# The probe rides INSIDE an existing module's subtree rather than standing
# alone: a file the registry does not own is correctly skipped, so a probe at
# the top level would prove nothing.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

GATE="sh tests/check_module_deps.sh"
PROBE="stdlib/lua/hull/ssh/__selftest_probe.lua"
FAILED=0
pass() { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAILED=$((FAILED + 1)); }

cleanup() { rm -f "$ROOT/$PROBE"; }
trap cleanup EXIT INT TERM

echo "check-module-deps self-test:"

# 1. CLEAN to begin with. If the tree already fails, the negative leg below
#    would "pass" for the wrong reason.
if $GATE >/dev/null 2>&1; then
    pass "1/CLEAN: the tree passes before anything is planted"
else
    bad "1/CLEAN: the tree already FAILS - fix that first, this test cannot run"
    echo ""
    $GATE 2>&1 | sed 's/^/    /'
    exit 1
fi

# 2. BITES. hull/ssh does not declare hull/jwt, and has no reason to.
cat > "$ROOT/$PROBE" <<'LUA'
-- planted by check_module_deps_selftest.sh; removed on exit
local jwt = require("hull.jwt")
return jwt
LUA

out=$($GATE 2>&1)
if [ "$?" -ne 0 ] && printf '%s' "$out" | grep -q 'hull/ssh requires hull/jwt'; then
    pass "2/BITES: an undeclared require is reported, by name"
else
    bad "2/BITES: the gate did not report the planted dependency"
    printf '%s\n' "$out" | sed 's/^/    /'
fi

# 3. CLEAN again once removed, so the gate is not simply always-red.
rm -f "$ROOT/$PROBE"
if $GATE >/dev/null 2>&1; then
    pass "3/CLEAN: removing it returns the gate to green"
else
    bad "3/CLEAN: still failing after the probe was removed"
fi

echo ""
if [ "$FAILED" -ne 0 ]; then
    echo "check-module-deps self-test: FAILED"
    exit 1
fi
echo "check-module-deps self-test: all checks passed."
