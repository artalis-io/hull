#!/bin/sh
# check_regression_test.sh - does an audit fix's new test fail on the pre-fix code?
#
# docs/design/audit_convergence.md section 5.1: every audit fix ships a test
# named <area>_audit<N>.<case> that FAILS before the fix. This script checks
# that mechanically for a PR:
#
#   1. find the UTEST cases the change adds under tests/ (UTEST(suite, case)
#      lines added in `git diff <merge-base> HEAD -- tests/`);
#   2. put every NON-test file back to the merge-base (modified and deleted
#      files restored, added files removed; tests/, docs, *.md and CHANGELOG
#      keep the PR's version);
#   3. build the test binaries those cases live in (build/<file basename>);
#   4. run only the added cases (utest --filter=suite.case) and expect AT
#      LEAST ONE to fail AT RUNTIME;
#   5. a binary that does not build against the pre-fix code proves nothing
#      by itself (any test that calls the fix's new API "fails" that way). It
#      gets a second, headers-kept pass: the PR's headers (*.h outside tests/)
#      are put back, the other sources stay reverted, and every file the PR
#      adds under tests/regression_stubs/*.c (pre-fix stand-ins for the fix's
#      new functions, keeping the OLD behaviour) is linked into the binary
#      (make REGRESSION_STUBS=...). A case that then fails at runtime counts.
#
# Opt-out (audit 43): when no added case can fail at runtime because the fix
# is nothing but new API, a commit message of the PR (or the PR body, which CI
# passes in REGRESSION_GATE_NOTE) may carry the trailer
#     REGRESSION-GATE: build-only <reason>
# Then a build failure on the pre-fix code is accepted, and logged with the
# reason. Without it, a build failure alone is a FAIL.
#
# Exit 0: at least one added test fails at runtime on the pre-fix code (or
#         the build-only opt-out applies and a binary did not build).
# Exit 1: no added test fails at runtime on the pre-fix code.
# Exit 2: nothing to check (no UTEST added, or nothing outside tests/ changed).
#
# It MODIFIES the working tree and restores HEAD on exit, so run it in a clean
# checkout (CI does: .github/workflows/audit-regression.yml, non-blocking for
# now - see docs/audit_process.md). Locally:
#     sh scripts/check_regression_test.sh [BASE_REF]      # default origin/main
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

base_ref=${1:-origin/main}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "check-regression-test: the working tree has uncommitted changes; run it in a clean checkout" >&2
  exit 2
fi

mb=$(git merge-base "$base_ref" HEAD)
head=$(git rev-parse HEAD)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hull-regress.XXXXXX")

restore() {
  # Files the change deleted were restored from the merge-base: remove them
  # again, then put every tracked file back to HEAD.
  if [ -f "$tmp/reverted" ]; then
    grep '^D ' "$tmp/reverted" | cut -c3- | while read -r p; do rm -f -- "$p"; done
  fi
  git checkout -q "$head" -- . 2>/dev/null || true
  rm -rf "$tmp"
}
trap restore EXIT INT TERM

# 1. Added UTEST cases: "file<TAB>suite.case".
git diff -U0 "$mb" HEAD -- 'tests/*.c' | awk '
  /^\+\+\+ b\// { file = substr($0, 7); next }
  /^\+[^+]/ {
    if (match($0, /UTEST(_F)?\([ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*,[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\)/)) {
      s = substr($0, RSTART, RLENGTH)
      sub(/UTEST(_F)?\([ \t]*/, "", s); sub(/[ \t]*\)$/, "", s)
      gsub(/[ \t]*,[ \t]*/, ".", s)
      print file "\t" s
    }
  }' > "$tmp/added"

if [ ! -s "$tmp/added" ]; then
  echo "check-regression-test: no UTEST case added under tests/ - nothing to check"
  echo "  (section 5.1: an audit fix ships a test named <area>_audit<N>.<case>)"
  exit 2
fi
echo "check-regression-test: added test cases:"
sed 's/^/  /' "$tmp/added"

# 2. Put the non-test changes back to the merge-base.
git diff --name-status --no-renames "$mb" HEAD | while IFS="$(printf '\t')" read -r st path; do
  case "$path" in
    tests/*|docs/*|*.md|CHANGELOG*|.github/*) continue ;;
  esac
  case "$st" in
    A) rm -f -- "$path" ;;
    *) git checkout -q "$mb" -- "$path" ;;
  esac
  echo "$st $path" >> "$tmp/reverted"
done

if [ ! -s "$tmp/reverted" ]; then
  echo "check-regression-test: the change touches nothing outside tests/ and docs - nothing to revert"
  exit 2
fi
echo "check-regression-test: reverted to $mb:"
sed 's/^/  /' "$tmp/reverted"

# 3 + 4. Build each binary and run its added cases. "$1" names the pass.
run_cases() {
  pass=$1; shift
  for f in "$@"; do
    bin=build/$(basename "$f" .c)
    rm -f -- "$bin"
    if ! make -j"$jobs" "$bin" REGRESSION_STUBS="$stubs" > "$tmp/build.log" 2>&1; then
      echo "check-regression-test: [$pass] $bin does not build on the pre-fix code:"
      { grep -E 'error:|Error [0-9]' "$tmp/build.log" || tail -5 "$tmp/build.log"; } |
        head -20 | cut -c1-240 | sed 's/^/    /'
      echo "$f" >> "$tmp/nobuild.$pass"
      continue
    fi
    grep "^$f	" "$tmp/added" | cut -f2 | while read -r tc; do
      if "$bin" --filter="$tc" > "$tmp/run.log" 2>&1; then
        echo "  [$pass] PASSES on pre-fix code: $tc ($bin)"
      else
        echo "  [$pass] FAILS  on pre-fix code: $tc ($bin)"
        echo x >> "$tmp/fails"
      fi
    done
  done
}

stubs=
run_cases reverted $(cut -f1 "$tmp/added" | sort -u)

# 5. Headers-kept pass for the binaries that did not build: the PR's headers
#    back, the sources still reverted, the PR's tests/regression_stubs/*.c
#    linked in.
if [ ! -f "$tmp/fails" ] && [ -s "$tmp/nobuild.reverted" ]; then
  grep -E '^[AM] ' "$tmp/reverted" | cut -c3- | grep -E '\.h$' | while read -r p; do
    git checkout -q "$head" -- "$p"
    echo "  kept the PR's $p"
  done
  for s in $(git diff --name-only --diff-filter=A "$mb" HEAD -- 'tests/regression_stubs/*.c'); do
    stubs="$stubs $(pwd)/$s"
    echo "  linking stub $s"
  done
  echo "check-regression-test: headers-kept pass (sources reverted, PR headers + stubs):"
  run_cases headers $(cat "$tmp/nobuild.reverted")
fi

if [ -f "$tmp/fails" ]; then
  echo "check-regression-test: OK - $(wc -l < "$tmp/fails" | tr -d ' ') added test(s) fail at runtime without the fix"
  exit 0
fi

if [ -s "$tmp/nobuild.reverted" ]; then
  # The explicit opt-out: a fix that is nothing but new API.
  note=$( { git log --format=%B "$mb..$head"; printf '%s\n' "${REGRESSION_GATE_NOTE:-}"; } |
    sed -n 's/^REGRESSION-GATE:[ 	]*build-only[ 	]*//p' | head -1)
  if [ -n "$note" ]; then
    echo "check-regression-test: OK (build-only opt-out) - no added test fails at runtime; accepted because"
    echo "  REGRESSION-GATE: build-only $note"
    exit 0
  fi
  echo "check-regression-test: FAIL - the added tests only fail to BUILD on the pre-fix code, which any test"
  echo "  of new API does. Add tests/regression_stubs/*.c with pre-fix stand-ins so a case fails at runtime,"
  echo "  or, if the fix is nothing but new API, a 'REGRESSION-GATE: build-only <reason>' trailer."
  exit 1
fi
echo "check-regression-test: FAIL - every added test passes on the pre-fix code, so none of them proves the fix"
exit 1
