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
#      LEAST ONE to fail. A binary that no longer builds against the pre-fix
#      code counts as failing (the test cannot pass there) and is reported.
#
# Exit 0: at least one added test fails on pre-fix code (or does not build).
# Exit 1: every added test passes on pre-fix code - it does not prove the fix.
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

# 3 + 4. Build each binary and run its added cases.
failed=0
for f in $(cut -f1 "$tmp/added" | sort -u); do
  bin=build/$(basename "$f" .c)
  if ! make -j"$jobs" "$bin" > "$tmp/build.log" 2>&1; then
    echo "check-regression-test: $bin does not build on the pre-fix code - counts as failing:"
    { grep -E 'error:|Error [0-9]' "$tmp/build.log" || tail -5 "$tmp/build.log"; } |
      head -20 | cut -c1-240 | sed 's/^/    /'
    failed=$((failed + 1))
    continue
  fi
  grep "^$f	" "$tmp/added" | cut -f2 | while read -r tc; do
    if "$bin" --filter="$tc" > "$tmp/run.log" 2>&1; then
      echo "  PASSES on pre-fix code: $tc ($bin)"
    else
      echo "  FAILS  on pre-fix code: $tc ($bin)"
      echo x >> "$tmp/fails"
    fi
  done
done
[ -f "$tmp/fails" ] && failed=$((failed + $(wc -l < "$tmp/fails")))

if [ "$failed" -gt 0 ]; then
  echo "check-regression-test: OK - $failed added test(s) fail without the fix"
  exit 0
fi
echo "check-regression-test: FAIL - every added test passes on the pre-fix code, so none of them proves the fix"
exit 1
