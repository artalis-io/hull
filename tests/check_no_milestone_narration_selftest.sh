#!/bin/sh
# check_no_milestone_narration_selftest.sh - deterministic negative test:
# prove check_no_milestone_narration.sh BITES on planted narration shapes, HONORS
# the audit + exact-survivor exceptions, and returns CLEAN once removed. H1 / S5.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

GATE="sh tests/check_no_milestone_narration.sh"
PROBE="src/hull/__selftest_narration_probe.h"   # in gate-2 scope (src/)
FAILED=0
pass() { printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAILED=$((FAILED + 1)); }

cleanup() { git reset -q -- "$PROBE" 2>/dev/null || true; rm -f "$PROBE"; }
trap cleanup EXIT INT TERM

plant() { printf '/* %s */\n' "$1" > "$PROBE"; git add -N "$PROBE" 2>/dev/null; }

# 1. Baseline: real tree passes (only the exact sbom.c survivor, allowlisted).
$GATE >/dev/null 2>&1 && pass "baseline passes (survivors allowlisted)" \
                       || bad "baseline does NOT pass (unexpected narration)"

# 2. Each narration SHAPE must bite.
for probe in "recompose in Phase C now" "the Phase 4.2 keel split" \
             "wired in Phase 3d-3 later" "added in Slice 6 of the frontend" \
             "checkpoint 3 enforces the pattern"; do
    plant "$probe"
    out=$($GATE 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "$PROBE"; then
        pass "shape bites: '$probe'"
    else
        bad "shape did NOT bite: '$probe' (rc=$rc)"
    fi
done

# 3. Semantic exception: an audit-provenance line must NOT bite.
plant "Phase 6 audit M-9: allowlist tightened"
$GATE >/dev/null 2>&1 && pass "audit-provenance line is allowed (no bite)" \
                       || bad "audit-provenance line wrongly bit"

# 4. A BARE architectural 'Phase <N>:' label must NOT bite (serve.c pattern).
plant "Phase 7: create KlServer"
$GATE >/dev/null 2>&1 && pass "bare 'Phase N:' pipeline label is allowed (no bite)" \
                       || bad "bare 'Phase N:' label wrongly bit"

# 5. Remove the probe -> clean again.
cleanup
$GATE >/dev/null 2>&1 && pass "probe removed -> gate CLEAN again" \
                       || bad "probe removed -> gate still failing"

[ "$FAILED" -eq 0 ] && { echo "check-no-milestone-narration selftest: all checks pass"; exit 0; }
echo "check-no-milestone-narration selftest: $FAILED failure(s)"; exit 1
