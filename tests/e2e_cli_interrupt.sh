#!/bin/sh
# app.main observes SIGINT / SIGTERM (shared/main_signal.h, platform gap 7),
# both runtimes, on whichever runner this build has (serve.c with HTTP,
# serve_cli.c without):
#   - a handler: the callback runs, main sees the signal and returns 3 -> 3
#   - no handler: SIGTERM ends the process 143 with buffered stdout FLUSHED
#   - a handler that never lets main finish: 130 once the grace ends
#   - two SIGINTs during the grace: 130 at once
#   - audit 44: main-then-serve (main + a route, serve.c runner) whose main
#     handles SIGTERM and returns 0 exits 143 - it never enters the serve loop
#   - audit 44: main's own code reads ctx.stdin after the interrupt
#   - audit 44: an inherited SIG_IGN for SIGINT stays ignored
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

# A background job of a shell without job control inherits SIGINT ignored,
# and Hull leaves an inherited SIG_IGN alone (audit 44): the child is started
# through a wrapper that puts the default disposition back. A script, not a
# shell function: a backgrounded function runs in a subshell, so $! would
# not be the hull process. Every step execs, so it keeps the one pid.
# sigint_wrapper FILE PERL-DISPOSITION PYTHON-DISPOSITION
sigint_wrapper() {
    if command -v perl >/dev/null 2>&1; then
        printf '%s\n' '#!/bin/sh' \
            'exec perl -e '"'"'$SIG{INT} = "'"$2"'"; exec @ARGV or die "exec: $!"'"'"' "$@"' > "$1"
    else
        printf '%s\n' '#!/bin/sh' \
            'exec python3 -c '"'"'import os, signal, sys; signal.signal(signal.SIGINT, signal.'"$3"'); os.execvp(sys.argv[1], sys.argv[1:])'"'"' "$@"' > "$1"
    fi
    chmod +x "$1"
}
if ! command -v perl >/dev/null 2>&1 && ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: no perl or python3 to set the child's SIGINT disposition"
    exit 0
fi
dfl="$work/sigint_dfl"
ign="$work/sigint_ign"
sigint_wrapper "$dfl" DEFAULT SIG_DFL
sigint_wrapper "$ign" IGNORE SIG_IGN

# start FIXTURE MODE [HULL FLAGS...]: run in the background, wait for the
# fixture's ready line. Sets $pid.
start() {
    _fx=$1; _mode=$2; shift 2
    : > "$out"
    "$dfl" "$HULL_BIN" "$@" "$_fx" -- "$_mode" < "${stdin_file:-/dev/null}" > "$out" 2>&1 &
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

# Audit 44 (both runtimes).
printf 'hello-line\n' > "$work/stdin.txt"
n=8
has_server=0
"$HULL_BIN" help 2>&1 | grep -q '^  dev ' && has_server=1
for ext in lua js; do
    # cli_interrupt_audit44.main_then_serve_sigterm: main AND a route; main
    # handles SIGTERM and returns 0 within the grace. The stop request ended
    # nothing before: the runner fell through into the serve loop and served
    # until SIGKILL.
    if [ "$has_server" = 1 ]; then
        port=$(( 20000 + ($$ % 20000) ))
        [ "$ext" = js ] && port=$((port + 1))
        start "tests/fixtures/cli_interrupt_serve/app.$ext" x -p "$port"
        kill -TERM "$pid"
        i=0
        while kill -0 "$pid" 2>/dev/null; do
            i=$((i + 1))
            if [ "$i" -ge 50 ]; then
                if command -v curl >/dev/null 2>&1 &&
                   curl -s -m 2 "http://127.0.0.1:$port/ping" 2>/dev/null | grep -q pong; then
                    kill -KILL "$pid" 2>/dev/null || true
                    fail "cli_interrupt_audit44.main_then_serve_sigterm ($ext): still serving after SIGTERM"
                fi
                kill -KILL "$pid" 2>/dev/null || true
                fail "cli_interrupt_audit44.main_then_serve_sigterm ($ext): still running 5 s after SIGTERM"
            fi
            sleep 0.1
        done
        finish
        [ "$rc" = 143 ] || fail "cli_interrupt_audit44.main_then_serve_sigterm ($ext): exit $rc, expected 143"
        grep -q '^interrupt: main saw SIGTERM' "$out" ||
            fail "cli_interrupt_audit44.main_then_serve_sigterm ($ext): main did not see the signal"
        echo "PASS: cli_interrupt_audit44.main_then_serve_sigterm ($ext): exits 143, never serves"
        n=$((n + 1))
    fi

    # js_main_signal_audit44.stdin_after_signal_await (and its Lua twin): the
    # code main runs after the interrupt is main's own, so ctx.stdin works.
    stdin_file="$work/stdin.txt"
    start "tests/fixtures/cli_interrupt_stdin/app.$ext" x
    stdin_file=
    kill -INT "$pid"
    gone_within 5
    finish
    [ "$rc" = 4 ] || fail "${ext}_main_signal_audit44.stdin_after_signal_await: exit $rc, expected 4"
    grep -q '^interrupt: stdin true hello-line' "$out" ||
        fail "${ext}_main_signal_audit44.stdin_after_signal_await: main could not read stdin after the interrupt"
    echo "PASS: ${ext}_main_signal_audit44.stdin_after_signal_await"
    n=$((n + 1))

    # main_signal_audit44.ignored_sigint_stays_ignored (end to end): started
    # with SIGINT ignored, the process keeps ignoring it; SIGTERM still
    # reaches main.
    : > "$out"
    "$ign" "$HULL_BIN" "tests/fixtures/cli_interrupt/app.$ext" -- handler < /dev/null > "$out" 2>&1 &
    pid=$!
    i=0
    while ! grep -q '^interrupt: ready' "$out" 2>/dev/null; do
        i=$((i + 1)); [ "$i" -lt 100 ] || fail "$ext ignored: never became ready"
        sleep 0.1
    done
    kill -INT "$pid"
    sleep 0.5
    kill -0 "$pid" 2>/dev/null ||
        fail "main_signal_audit44.ignored_sigint_stays_ignored ($ext): an ignored SIGINT ended the process"
    grep -q '^interrupt: handler' "$out" &&
        fail "main_signal_audit44.ignored_sigint_stays_ignored ($ext): an ignored SIGINT reached app.main"
    kill -TERM "$pid"
    finish
    [ "$rc" = 3 ] || fail "main_signal_audit44.ignored_sigint_stays_ignored ($ext): SIGTERM exit $rc, expected 3"
    echo "PASS: main_signal_audit44.ignored_sigint_stays_ignored ($ext) end to end"
    n=$((n + 1))
done
echo "$n/$n app.main interrupt end-to-end tests passed"
