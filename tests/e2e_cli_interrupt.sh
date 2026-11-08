#!/bin/sh
# app.main observes SIGINT / SIGTERM (shared/main_signal.h, platform gap 7),
# both runtimes, on whichever runner this build has (serve.c with HTTP,
# serve_cli.c without):
#   - a handler: the callback runs, main sees the signal and returns 3 -> 3
#   - no handler: SIGTERM ends the process 143 with buffered stdout FLUSHED
#   - a handler that never lets main finish: 130 once the grace ends
#   - two SIGINTs during the grace: 130 at once
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
HULL_BIN="${HULL_BIN:-build/hull}"
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL_BIN"
if [ "$HULL_RC_SHIFT" = 1 ]; then
    echo "SKIP: signals are not deliverable to an APE from this shell"
    exit 0
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out="$work/output"
fail() { echo "FAIL: $1"; cat "$out" 2>/dev/null || true; exit 1; }

# start FIXTURE MODE [HULL FLAGS...]: run in the background, wait for the
# fixture's ready line. Sets $pid.
start() {
    _fx=$1; _mode=$2; shift 2
    : > "$out"
    "$HULL_BIN" "$@" "$_fx" -- "$_mode" > "$out" 2>&1 &
    pid=$!
    i=0
    while ! grep -q '^interrupt: ready' "$out" 2>/dev/null; do
        i=$((i + 1))
        [ "$i" -lt 100 ] || fail "$_fx $_mode never became ready"
        kill -0 "$pid" 2>/dev/null || fail "$_fx $_mode exited before it was ready"
        sleep 0.1
    done
}
# finish: wait for $pid; its exit status lands in $rc (never aborts under
# set -e; run in THIS shell - a $(...) subshell does not own the child).
finish() { rc=0; wait "$pid" || rc=$?; }
# gone_within SECONDS: fail unless $pid has exited within the time.
gone_within() {
    i=0
    while kill -0 "$pid" 2>/dev/null; do
        i=$((i + 1))
        [ "$i" -lt $(( $1 * 10 )) ] || fail "pid $pid still running after $1 s"
        sleep 0.1
    done
}

for ext in lua js; do
    fx="tests/fixtures/cli_interrupt/app.$ext"

    start "$fx" handler
    kill -INT "$pid"
    finish
    [ "$rc" = 3 ] || fail "$ext handler: exit $rc, expected 3 (main's return)"
    grep -q '^interrupt: handler SIGINT' "$out" || fail "$ext handler: callback did not run"
    grep -q '^interrupt: main saw SIGINT' "$out" || fail "$ext handler: main did not see the signal"
    echo "PASS: $ext handler runs, main sees SIGINT and returns 3"

    start "$fx" none
    kill -TERM "$pid"
    finish
    [ "$rc" = 143 ] || fail "$ext none: exit $rc, expected 143 (128 + SIGTERM)"
    grep -q '^interrupt: buffered' "$out" || fail "$ext none: buffered stdout was not flushed"
    echo "PASS: $ext no handler: SIGTERM exits 143 with stdout flushed"

    start "$fx" stuck --interrupt-grace-ms 300
    kill -INT "$pid"
    gone_within 3
    finish
    [ "$rc" = 130 ] || fail "$ext stuck: exit $rc, expected 130 (128 + SIGINT) after the grace"
    grep -q '^interrupt: handler SIGINT' "$out" || fail "$ext stuck: callback did not run"
    echo "PASS: $ext handler that never finishes: 130 after the grace"

    start "$fx" stuck
    kill -INT "$pid"
    sleep 0.3
    kill -INT "$pid"
    gone_within 3
    finish
    [ "$rc" = 130 ] || fail "$ext twice: exit $rc, expected 130 at once"
    grep -q '^interrupt: handler SIGINT' "$out" || fail "$ext twice: callback did not run"
    echo "PASS: $ext second SIGINT during the grace exits 130 at once"
done
echo '8/8 app.main interrupt end-to-end tests passed'
