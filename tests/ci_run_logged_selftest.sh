#!/usr/bin/env bash
# ci_run_logged_selftest.sh - fixtures for the nightly logging wrapper.
# Validates exit-status preservation (the load-bearing property: a failed command
# must still fail the job) and artifact-path coverage (the promised log file is
# created and contains the combined stdout+stderr).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/tests/ci_run_logged.sh"
fail=0

ck() {  # ck GOT WANT NAME
    if [ "$1" = "$2" ]; then
        :
    else
        echo "FAIL: $3 (got '$1' want '$2')"; fail=1
    fi
}

tmp="$(mktemp -d)"

# 1. success path: exit 0; log created with BOTH stdout and stderr (2>&1).
bash "$W" "$tmp/ok.log" bash -c 'echo the-stdout; echo the-stderr >&2; exit 0'
ck "$?" 0 "success: exit status 0 preserved"
test -f "$tmp/ok.log"; ck "$?" 0 "success: promised log file created"
grep -q the-stdout "$tmp/ok.log"; ck "$?" 0 "success: stdout captured"
grep -q the-stderr "$tmp/ok.log"; ck "$?" 0 "success: stderr captured (combined 2>&1)"

# 2. failure path: CMD's real non-zero status is preserved (NOT tee's 0), and the
#    log is still written (so the artifact exists on failure).
bash "$W" "$tmp/bad.log" bash -c 'echo boom-before-exit; exit 7'
ck "$?" 7 "failure: CMD exit status 7 preserved (not masked by tee)"
test -f "$tmp/bad.log"; ck "$?" 0 "failure: log file still created"
grep -q boom-before-exit "$tmp/bad.log"; ck "$?" 0 "failure: output captured before exit"

# 3. a different non-zero code is preserved (not collapsed to 1).
bash "$W" "$tmp/c2.log" bash -c 'exit 2'
ck "$?" 2 "failure: exit status 2 preserved distinctly"

# 4. nested log dir is created (deterministic path under a subdir).
bash "$W" "$tmp/sub/dir/nested.log" bash -c 'echo hi'
ck "$?" 0 "nested log path: exit ok"
test -f "$tmp/sub/dir/nested.log"; ck "$?" 0 "nested log path: file created (mkdir -p)"

# 5. usage error (too few args) exits 2 without creating a log.
bash "$W" only-one-arg
ck "$?" 2 "usage error exits 2"

rm -rf "$tmp"
if [ "$fail" = 0 ]; then
    echo "ci_run_logged selftest: OK"
else
    echo "ci_run_logged selftest: FAILED"
fi
exit "$fail"
