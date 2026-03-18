#!/bin/sh
# E2E tests — hull agent context subcommand
#
# Tests the context documentation retrieval for all 12 tasks and 3 levels.
#
# Usage: sh tests/e2e_context.sh
# Requires: build/hull already built
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0

if [ ! -x "$HULL" ]; then
    echo "e2e_context: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

check_contains() {
    case "$2" in
        *"$3"*) pass "$1" ;;
        *)      fail "$1 — expected '$3' in: $(echo "$2" | head -c 200)" ;;
    esac
}

check_exit() {
    if [ "$2" = "$3" ]; then
        pass "$1"
    else
        fail "$1 — expected exit $3, got $2"
    fi
}

echo ""
echo "=== E2E: hull agent context ==="

# ── All 12 tasks at compact level ──────────────────────────────────────

echo ""
echo "--- context: all tasks (compact) ---"

for task in auth db middleware templates routing testing build deploy search i18n webhooks validation; do
    OUT=$($HULL agent context --task="$task" --level=compact 2>&1)
    check_contains "context $task has task"    "$OUT" "\"task\":\"$task\""
    check_contains "context $task has level"   "$OUT" '"level":"compact"'
    check_contains "context $task has content" "$OUT" '"content":'
done

# ── Level selection ─────────────────────────────────────────────────────

echo ""
echo "--- context: level selection ---"

# Minimal should be shorter than compact
MINIMAL=$($HULL agent context --task=auth --level=minimal 2>&1)
COMPACT=$($HULL agent context --task=auth --level=compact 2>&1)
FULL=$($HULL agent context --task=auth --level=full 2>&1)

check_contains "minimal has level"  "$MINIMAL" '"level":"minimal"'
check_contains "compact has level"  "$COMPACT" '"level":"compact"'
check_contains "full has level"     "$FULL"    '"level":"full"'

# All should have content
check_contains "minimal has content" "$MINIMAL" '"content":'
check_contains "compact has content" "$COMPACT" '"content":'
check_contains "full has content"    "$FULL"    '"content":'

# ── Positional task argument ────────────────────────────────────────────

echo ""
echo "--- context: argument forms ---"

OUT=$($HULL agent context auth --level=minimal 2>&1)
check_contains "positional task arg" "$OUT" '"task":"auth"'

OUT=$($HULL agent context --task auth --level compact 2>&1)
check_contains "space-separated args" "$OUT" '"task":"auth"'

# ── Error cases ─────────────────────────────────────────────────────────

echo ""
echo "--- context: errors ---"

# Invalid task
EXIT_CODE=0
OUT=$($HULL agent context --task=nonexistent 2>&1) || EXIT_CODE=$?
check_contains "invalid task error"   "$OUT" '"error"'
check_contains "invalid task message" "$OUT" 'unknown task'

# Invalid level
EXIT_CODE=0
OUT=$($HULL agent context --task=auth --level=bogus 2>&1) || EXIT_CODE=$?
check_contains "invalid level error"   "$OUT" '"error"'

# Missing task
EXIT_CODE=0
OUT=$($HULL agent context 2>&1) || EXIT_CODE=$?
check_exit "missing task exit" "$EXIT_CODE" "1"

# ── JSON validity (basic check) ─────────────────────────────────────────

echo ""
echo "--- context: JSON validity ---"

for task in auth db routing; do
    for level in minimal compact full; do
        OUT=$($HULL agent context --task="$task" --level="$level" 2>&1)
        # Check it starts with { and ends with }
        case "$OUT" in
            \{*\}) pass "JSON valid: $task/$level" ;;
            *)     fail "JSON invalid: $task/$level — $OUT" ;;
        esac
    done
done

# ── Summary ─────────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e context tests passed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
