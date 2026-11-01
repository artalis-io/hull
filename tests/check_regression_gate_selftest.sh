#!/bin/sh
# check_regression_gate_selftest.sh - prove scripts/check_regression_test.sh
# demands a RUNTIME failure on the pre-fix code (audit 43).
#
# Run by tests/check_invariants_selftest.sh (`make check-invariants-selftest`).
# Each case builds a throwaway git repository: a base commit with a tiny C
# "library" (include/x.h, src/x.c) and a Makefile whose test rule links
# $(REGRESSION_STUBS) the way mk/tests.mk does, then a "fix" commit, and runs
# the gate against the base:
#   runtime     the fix changes x_old's behaviour; the test fails at runtime
#               on the pre-fix code                              -> exit 0
#   build-only  the fix is a new function x_new; the test only fails to build
#               on the pre-fix code                              -> exit 1
#   trailer     build-only + a 'REGRESSION-GATE: build-only <reason>' trailer
#                                                                -> exit 0
#   stub        build-only + tests/regression_stubs/x_new.c with the OLD
#               behaviour; the headers-kept pass fails at runtime -> exit 0
#   stub-passes build-only + a stub that already behaves like the fix
#                                                                -> exit 1
#   unchanged   the fix's test passes on the pre-fix code too    -> exit 1
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."
GATE=${REGRESSION_GATE_SCRIPT:-$(pwd)/scripts/check_regression_test.sh}
CC=${CC_GATE_SELFTEST:-cc}

tmp=$(mktemp -d "${TMPDIR:-/tmp}/hull-gate-selftest.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
fail=0
runs=0

mkrepo() {   # dir
  r=$1
  mkdir -p "$r/include" "$r/src" "$r/tests" "$r/scripts"
  cp "$GATE" "$r/scripts/check_regression_test.sh"
  printf 'int x_old(void);\n' > "$r/include/x.h"
  printf '#include "x.h"\nint x_old(void) { return 0; }\n' > "$r/src/x.c"
  printf 'REGRESSION_STUBS ?=\nbuild/test_x: tests/test_x.c src/x.c include/x.h\n\tmkdir -p build\n\t%s -Werror=implicit-function-declaration -Iinclude -o $@ tests/test_x.c src/x.c $(REGRESSION_STUBS)\n' "$CC" > "$r/Makefile"
  printf '/* placeholder */\n' > "$r/tests/README.c"
  git -C "$r" init -q
  git -C "$r" config user.email selftest@example.invalid
  git -C "$r" config user.name selftest
  git -C "$r" config commit.gpgsign false
  git -C "$r" config core.hooksPath /dev/null
  git -C "$r" add -A
  git -C "$r" commit -q -m base
}

test_src() {   # call expected
  printf '#include "x.h"\n#define UTEST(a, b) int main(void)\nUTEST(demo_audit43, case1) { return %s() == 1 ? 0 : 1; }\n' "$1"
}

expect() {   # name dir want
  got=0
  (cd "$2" && sh scripts/check_regression_test.sh "$(git -C "$2" rev-list --max-parents=0 HEAD)") \
    > "$tmp/out" 2>&1 || got=$?
  runs=$((runs + 1))
  if [ "$got" != "$3" ]; then
    echo "check-regression-gate-selftest: FAIL - case $1 exited $got, want $3:"
    sed 's/^/    /' "$tmp/out"
    fail=1
  fi
}

new_api() {   # dir: add x_new (the fix) and a test of it
  printf 'int x_old(void);\nint x_new(void);\n' > "$1/include/x.h"
  printf '#include "x.h"\nint x_old(void) { return 0; }\nint x_new(void) { return 1; }\n' > "$1/src/x.c"
  test_src x_new > "$1/tests/test_x.c"
}

# runtime
r=$tmp/runtime; mkrepo "$r"
printf '#include "x.h"\nint x_old(void) { return 1; }\n' > "$r/src/x.c"
test_src x_old > "$r/tests/test_x.c"
git -C "$r" add -A; git -C "$r" commit -q -m fix
expect runtime "$r" 0

# build-only
r=$tmp/buildonly; mkrepo "$r"; new_api "$r"
git -C "$r" add -A; git -C "$r" commit -q -m fix
expect build-only "$r" 1

# trailer
r=$tmp/trailer; mkrepo "$r"; new_api "$r"
git -C "$r" add -A
git -C "$r" commit -q -m fix -m 'REGRESSION-GATE: build-only the fix is a new accessor with no old behaviour'
expect trailer "$r" 0

# stub
r=$tmp/stub; mkrepo "$r"; new_api "$r"
mkdir -p "$r/tests/regression_stubs"
printf 'int x_new(void) { return 0; }\n' > "$r/tests/regression_stubs/x_new.c"
git -C "$r" add -A; git -C "$r" commit -q -m fix
expect stub "$r" 0

# stub-passes
r=$tmp/stubpasses; mkrepo "$r"; new_api "$r"
mkdir -p "$r/tests/regression_stubs"
printf 'int x_new(void) { return 1; }\n' > "$r/tests/regression_stubs/x_new.c"
git -C "$r" add -A; git -C "$r" commit -q -m fix
expect stub-passes "$r" 1

# unchanged
r=$tmp/unchanged; mkrepo "$r"
printf '#include "x.h"\nint x_old(void) { return 1 - 1; }\n' > "$r/src/x.c"
printf '#include "x.h"\n#define UTEST(a, b) int main(void)\nUTEST(demo_audit43, case1) { return x_old() == 0 ? 0 : 1; }\n' > "$r/tests/test_x.c"
git -C "$r" add -A; git -C "$r" commit -q -m fix
expect unchanged "$r" 1

if [ "$fail" != 0 ]; then
  exit 1
fi
echo "check-regression-gate-selftest: OK ($runs runs: a runtime failure passes; a build failure alone fails unless stubbed into a runtime failure or opted out by trailer)"
