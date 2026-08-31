#!/bin/sh
# tests/check_site_consistency_selftest.sh - deterministic NEGATIVE test.
#
# Proves check_site_consistency.sh is not a no-op: it injects one representative
# violation per check (plus the specific tab-content regressions the gate must
# catch) into a COPY-restored site file, asserts the gate exits non-zero AND
# reports that specific check, then restores. Mirrors
# tests/check_docs_integrity_selftest.sh.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

GATE="sh tests/check_site_consistency.sh"
SITE=site/index.html
BAK="/tmp/hull_site_bak.$$"
FAILED=0
pass() { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAILED=$((FAILED + 1)); }

cleanup() { [ -f "$BAK" ] && cp "$BAK" "$SITE" && rm -f "$BAK"; }
trap cleanup EXIT INT TERM
cp "$SITE" "$BAK"

# expect_bite <check-id-substring> <label>: run the gate, require non-zero exit
# AND that the named check appears in the output.
expect_bite() {
    id=$1; label=$2
    out=$($GATE 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "$id"; then
        pass "$label -> gate bites ($id)"
    else
        bad "$label -> gate did NOT bite (expected $id; rc=$rc)"
    fi
}

echo "site-consistency self-test:"

# Baseline: the real site must pass.
if $GATE >/dev/null 2>&1; then pass "baseline: clean site passes"
else bad "baseline: clean site should pass but did not"; fi

# 1. VERSION - a data-hull-version marker that reads a stale version.
cp "$BAK" "$SITE"
printf '<span data-hull-version>v0.0.1</span>\n' >> "$SITE"
expect_bite "1/VERSION" "stale data-hull-version marker"

# 2a. TABS - a MISSING install panel (drop the Windows panel's id).
cp "$BAK" "$SITE"
sed 's/id="install-panel-windows"/id="install-panel-GONE"/' "$BAK" > "$SITE"
expect_bite "2/TABS" "missing install tab panel"

# 2b. TABS - a WRONG-platform installer command (Windows panel shows install.sh).
cp "$BAK" "$SITE"
sed 's#gethull.dev/install.ps1#gethull.dev/install.sh#g' "$BAK" > "$SITE"
expect_bite "2/TABS" "wrong-platform installer in a panel"

# 3. WINDOWS - the Windows verify panel handed POSIX shell syntax (iwr -> curl).
cp "$BAK" "$SITE"
sed 's/iwr/curl -fsSL/g' "$BAK" > "$SITE"
expect_bite "3/WINDOWS" "Windows verify panel using POSIX syntax"

# Restore and confirm the site is clean again.
cp "$BAK" "$SITE"
if $GATE >/dev/null 2>&1; then pass "site clean again after self-test"
else bad "site not clean after self-test (restore failed?)"; fi

if [ "$FAILED" -eq 0 ]; then
    printf '\n\033[32msite-consistency self-test passed: all checks bite.\033[0m\n'
    exit 0
fi
printf '\n\033[31msite-consistency self-test FAILED (%d).\033[0m\n' "$FAILED"
exit 1
