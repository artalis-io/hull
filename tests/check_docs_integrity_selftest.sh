#!/bin/sh
# tests/check_docs_integrity_selftest.sh - deterministic NEGATIVE test.
#
# Proves check_docs_integrity.sh is not a no-op: for each of its five checks it
# injects a single representative violation, asserts the gate exits non-zero AND
# reports that specific check, then removes the injection. Ends by confirming the
# tree is clean again. Mirrors tests/check_sdk_headers_selftest.sh.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

GATE="sh tests/check_docs_integrity.sh"
FAILED=0
BAK="/tmp/hull_readme_bak.$$"
pass() { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAILED=$((FAILED + 1)); }

cleanup() {
    rm -f docs/__selftest_c1.md docs/archive/__selftest_a.md \
          tests/__selftest_ref_probe.sh docs/kvmem_design.md
    [ -f "$BAK" ] && cp "$BAK" docs/README.md && rm -f "$BAK"
}
trap cleanup EXIT INT TERM

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

echo "docs-integrity self-test:"

# Baseline: the real tree must pass.
if $GATE >/dev/null 2>&1; then pass "baseline: clean tree passes"
else bad "baseline: clean tree should pass but did not"; fi

# 1. CATALOG - an uncatalogued top-level doc.
: > docs/__selftest_c1.md
expect_bite "1/CATALOG" "uncatalogued top-level doc"
rm -f docs/__selftest_c1.md

# 2. LINKS - a broken relative Markdown link in an active index.
cp docs/README.md "$BAK"
printf '\n[selftest](__selftest_nope_%s.md)\n' "$$" >> docs/README.md
expect_bite "2/LINKS" "broken markdown link"
cp "$BAK" docs/README.md; rm -f "$BAK"

# 3. ARCHIVE - an archived doc missing from the archive inventory.
: > docs/archive/__selftest_a.md
expect_bite "3/ARCHIVE" "uninventoried archived doc"
rm -f docs/archive/__selftest_a.md

# 4. SOURCE-REFS - a dangling docs/ reference in first-party code.
printf '# probe references docs/__selftest_ref_missing_%s.md\n' "$$" > tests/__selftest_ref_probe.sh
expect_bite "4/SOURCE-REFS" "dangling docs ref in code"
rm -f tests/__selftest_ref_probe.sh

# 5. RESURRECTION - a moved historical doc reappears at its old path.
: > docs/kvmem_design.md
expect_bite "5/RESURRECTION" "resurrected moved doc"
rm -f docs/kvmem_design.md

# Final: the tree is clean again (cleanup worked, no leftover injections).
if $GATE >/dev/null 2>&1; then pass "tree clean again after self-test"
else bad "tree not clean after self-test (leftover injection?)"; fi

if [ "$FAILED" -eq 0 ]; then
    printf '\n\033[32mdocs-integrity self-test passed: all 5 checks bite.\033[0m\n'
    exit 0
fi
printf '\n\033[31mdocs-integrity self-test FAILED (%d).\033[0m\n' "$FAILED"
exit 1
