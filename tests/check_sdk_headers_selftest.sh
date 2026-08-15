#!/bin/sh
# tests/check_sdk_headers_selftest.sh — deterministic self-test proving
# check_sdk_headers.sh is not a no-op: it PASSES on the clean tree, FAILS with a
# non-zero exit AND the EXACT drifting path when a maintained copy is perturbed,
# and recovers after the perturbation is reverted. Uses a git-tracked header and
# restores it via `git checkout` (ephemeral in CI). (#331)
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

CHECK="tests/check_sdk_headers.sh"
VICTIM="examples/compute/compute/hash/hull_compute.h"

restore() { git checkout -- "$VICTIM" 2>/dev/null; }
# Belt-and-suspenders: restore the victim no matter how we leave — a failed
# assertion, an unexpected error, or a signal — so the working tree is always
# clean after both the passing and the deliberately-failing checker paths.
trap restore EXIT INT TERM
fail() { echo "SELFTEST FAIL: $1"; exit 1; }   # trap handles the restore

# Refuse to run with pre-existing local edits to the victim (would be clobbered).
git diff --quiet -- "$VICTIM" || { echo "SELFTEST FAIL: $VICTIM has uncommitted changes; aborting"; trap - EXIT INT TERM; exit 1; }

# 1. clean tree passes
sh "$CHECK" >/dev/null 2>&1 || fail "check reported drift on a clean tree"

# 2. a perturbed maintained copy is detected: non-zero exit + exact path
printf '\n// selftest drift\n' >> "$VICTIM"
out="$(sh "$CHECK" 2>&1)"; rc=$?
restore
[ "$rc" -ne 0 ] || fail "check did NOT fail on injected drift"
printf '%s\n' "$out" | grep -qF "DRIFT $VICTIM" || fail "check did not report the exact drifting path"

# 3. restored tree passes again
sh "$CHECK" >/dev/null 2>&1 || fail "check still reports drift after restore"

echo "SELFTEST OK: check-sdk-headers passes clean, fails on drift reporting the exact path, and recovers."
