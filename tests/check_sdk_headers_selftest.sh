#!/bin/sh
# tests/check_sdk_headers_selftest.sh — deterministic self-test proving
# check_sdk_headers.sh is not a no-op: it PASSES on the clean tree, FAILS with a
# non-zero exit AND the EXACT drifting path when a maintained copy is perturbed,
# and recovers after the perturbation is reverted.
#
# Restoration is FILESYSTEM-owned (cp from a temp backup), NOT `git checkout`, so
# it works in an environment with a writable worktree but read-only Git metadata
# (where writing .git/index.lock would fail). Restoration results and byte
# identity are checked, never suppressed; the temp backup is removed on every
# exit. (#331)
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

CHECK="tests/check_sdk_headers.sh"
VICTIM="examples/compute/compute/hash/hull_compute.h"
BACKUP="$(mktemp)"

fail() { echo "SELFTEST FAIL: $1"; exit 1; }

# Until the victim is confirmed clean, the trap only removes the temp backup — it
# must NOT write the victim (that could clobber a user's uncommitted edit).
trap 'rm -f "$BACKUP"' EXIT INT TERM

# Snapshot the victim's HEAD bytes into the backup (git show reads objects only —
# no index write, safe under read-only Git metadata) and reject a dirty victim so
# we never overwrite pre-existing staged/unstaged changes.
git show "HEAD:$VICTIM" > "$BACKUP" 2>/dev/null || fail "cannot read $VICTIM at HEAD"
cmp -s "$VICTIM" "$BACKUP" || fail "$VICTIM has uncommitted changes vs HEAD; aborting (no changes made)"

# Victim is clean: arm the filesystem restore-on-exit (cp, index-independent),
# and report — never suppress — a restoration failure.
trap 'cp "$BACKUP" "$VICTIM" 2>/dev/null || echo "SELFTEST ERROR: final restore of $VICTIM failed"; rm -f "$BACKUP"' EXIT INT TERM

# 1. clean tree passes
sh "$CHECK" >/dev/null 2>&1 || fail "check reported drift on a clean tree"

# 2. a perturbed maintained copy is detected: non-zero exit + exact path
printf '\n// selftest drift\n' >> "$VICTIM"
out="$(sh "$CHECK" 2>&1)"; rc=$?
cp "$BACKUP" "$VICTIM" || fail "could not restore $VICTIM from backup"
cmp -s "$BACKUP" "$VICTIM" || fail "restored $VICTIM is not byte-identical to the backup"
[ "$rc" -ne 0 ] || fail "check did NOT fail on injected drift"
printf '%s\n' "$out" | grep -qF "DRIFT $VICTIM" || fail "check did not report the exact drifting path"

# 3. restored tree passes again
sh "$CHECK" >/dev/null 2>&1 || fail "check still reports drift after restore"

echo "SELFTEST OK: check-sdk-headers passes clean, fails on drift reporting the exact path, and recovers (filesystem restore)."
