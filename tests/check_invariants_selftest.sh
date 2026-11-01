#!/bin/sh
# check_invariants_selftest.sh - prove every invariant rule bites.
#
# `make check-invariants-selftest`. For each rule INV-N, the fixture trees
# under tests/invariants/fixtures/INV-N/ are small fake source roots:
#   good/   must PASS the rule (exit 0)
#   bad*/   must FAIL it (exit 1) and name the rule in the output
# Each run uses --no-allowlist, so a rule cannot pass a bad tree through an
# exception. The INV-10 diff check reads a fixture's own vendor.diff; every
# other run gets an empty diff so the fixtures never depend on git state.
# Then the allowlist mechanics: an exact entry and a glob cover a finding,
# an entry that matches nothing is STALE (fails), an entry without a reason
# is malformed (fails), and every rule has fixtures.
# Last, tests/check_regression_gate_selftest.sh proves the audit-fix regression
# gate (scripts/check_regression_test.sh) wants a runtime failure (audit 43).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

PY=${PYTHON:-python3}
export PYTHONDONTWRITEBYTECODE=1   # no __pycache__ in scripts/
LINT=scripts/check_invariants.py
FIX=tests/invariants/fixtures
fail=0
runs=0

tmp=$(mktemp -d "${TMPDIR:-/tmp}/hull-inv-selftest.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
: > "$tmp/empty.diff"

run_case() {   # rule dir expected_status
  rule=$1; dir=$2; want=$3
  if [ -f "$dir/vendor.diff" ]; then
    set -- --root "$dir" --rule "$rule" --no-allowlist
  else
    set -- --root "$dir" --rule "$rule" --no-allowlist --diff "$tmp/empty.diff"
  fi
  got=0
  "$PY" "$LINT" "$@" > "$tmp/out" 2>&1 || got=$?
  runs=$((runs + 1))
  if [ "$got" != "$want" ]; then
    echo "check-invariants-selftest: FAIL - $rule on $dir exited $got, want $want:"
    sed 's/^/    /' "$tmp/out"
    fail=1
  elif [ "$want" = 1 ] && ! grep -q "^  $rule " "$tmp/out"; then
    echo "check-invariants-selftest: FAIL - $rule on $dir failed without a $rule finding:"
    sed 's/^/    /' "$tmp/out"
    fail=1
  fi
}

# Every rule the lint defines has fixtures.
rules=$("$PY" -c 'import sys; sys.path.insert(0, "scripts"); import check_invariants as c; print(" ".join(sorted(c.RULES, key=lambda r: int(r.split("-")[1]))))')
for rule in $rules; do
  if [ ! -d "$FIX/$rule/good" ] || ! ls -d "$FIX/$rule"/bad* >/dev/null 2>&1; then
    echo "check-invariants-selftest: FAIL - $rule has no good/ and bad*/ fixtures under $FIX/$rule"
    fail=1
    continue
  fi
  run_case "$rule" "$FIX/$rule/good" 0
  for b in "$FIX/$rule"/bad*; do
    run_case "$rule" "$b" 1
  done
done

# Allowlist mechanics, on INV-6's bad fixture (one finding: demo_report).
bad6=$FIX/INV-6/bad
check_allow() {   # description expected_status allowlist-line
  printf '%s\n' "$3" > "$tmp/allow"
  got=0
  "$PY" "$LINT" --root "$bad6" --rule INV-6 --allowlist "$tmp/allow" \
      > "$tmp/out" 2>&1 || got=$?
  runs=$((runs + 1))
  if [ "$got" != "$2" ]; then
    echo "check-invariants-selftest: FAIL - allowlist: $1 exited $got, want $2:"
    sed 's/^/    /' "$tmp/out"
    fail=1
  fi
}
check_allow "exact entry covers the finding" 0 \
  'INV-6  src/hull/cap/demo.c:demo_report  # test: covered'
check_allow "glob entry covers the finding" 0 \
  'INV-6  src/hull/*:demo_*  # test: covered by a glob'
check_allow "an entry for another rule does not" 1 \
  'INV-7  src/hull/cap/demo.c:demo_report  # test: wrong rule'
check_allow "an entry without a reason is malformed" 1 \
  'INV-6  src/hull/cap/demo.c:demo_report'
printf '%s\n%s\n' 'INV-6  src/hull/cap/demo.c:demo_report  # test: covered' \
  'INV-6  src/hull/cap/gone.c:old_fn  # test: matches nothing' > "$tmp/allow"
got=0
"$PY" "$LINT" --root "$bad6" --rule INV-6 --allowlist "$tmp/allow" \
    > "$tmp/out" 2>&1 || got=$?
runs=$((runs + 1))
if [ "$got" != 1 ] || ! grep -q 'stale allowlist entry' "$tmp/out"; then
  echo "check-invariants-selftest: FAIL - a stale allowlist entry did not fail the run:"
  sed 's/^/    /' "$tmp/out"
  fail=1
fi

# The audit-fix regression gate (scripts/check_regression_test.sh, audit 43):
# a build failure on the pre-fix code is not proof by itself.
runs=$((runs + 1))
if ! sh tests/check_regression_gate_selftest.sh; then
  fail=1
fi

if [ "$fail" != 0 ]; then
  exit 1
fi
echo "check-invariants-selftest: OK ($runs runs: every rule passes its good fixture and fails each bad one; allowlist mechanics hold)"
