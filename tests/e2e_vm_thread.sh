#!/bin/sh
# e2e_vm_thread.sh: --vm-thread (audit convergence phase 5, design §6.3
# phase 1) - one native call stuck past its run's deadline degrades one
# request, not the server.
#
# The fixture's hull._test_native_sleep / hull._testNativeSleep sleeps in C
# without looking at the run's budget or stop flag (HULL_TEST_NATIVE_SLEEP=1),
# the shape of an uncharged engine builtin. For each runtime:
#
#   off       without --vm-thread the stuck run holds the whole server: a /ping
#             sent meanwhile waits for it (today's behaviour, unchanged);
#   handler   with it, the stuck request is answered 503 at about deadline +
#             grace, a /ping on a new connection meanwhile is answered 503 at
#             once, and once the native call returns the server serves again;
#   middleware the same for a run stuck in a middleware;
#   timer     a timer stuck the same way: /ping is 503 while it is, 200 after,
#             and the timer keeps firing;
#   park      a handler that parks (hull.sleep) still works on the VM thread;
#   manifest  limits.vm_thread turns it on without the flag;
#   env       so does HULL_VM_THREAD=1;
#   ceiling   still stuck after the hard ceiling: the process exits, status 75;
#   sigterm   SIGTERM during standby: the process exits within the drain time.
#
# Requires build/hull and curl.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

HULL="${HULL:-build/hull}"
case "$HULL" in /*) ;; *) HULL="$(pwd)/$HULL" ;; esac
FIX="$(cd "$(dirname "$0")" && pwd)/fixtures/vm_thread"
PORT_BASE="${PORT_BASE:-19720}"
PASS=0
FAIL=0
SKIP=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "  SKIP: $1"; }

. "$(dirname "$0")/lib/hull_proc.sh"
hull_proc_init "$HULL"

command -v curl >/dev/null 2>&1 || { echo "SKIP: curl not found"; exit 0; }

WORK=$(mktemp -d)
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill -9 "$SERVER_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# The suite runs every leg under its own settings; an inherited
# HULL_VM_THREAD (make e2e-vm-thread-all) would turn the "off" leg on.
unset HULL_VM_THREAD

PORT=$PORT_BASE
next_port() { PORT=$((PORT + 1)); }

# start ENTRY LOG ARGS...: background hull on $PORT, wait until it answers.
start() {
    entry=$1; log=$2; shift 2
    HULL_TEST_NATIVE_SLEEP=1 "$HULL" "$FIX/$entry" -p "$PORT" --no-db \
        "$@" > "$log" 2>&1 &
    SERVER_PID=$!
    i=0
    while [ $i -lt 100 ]; do
        if curl -s -o /dev/null "http://127.0.0.1:$PORT/ping" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    echo "    server did not start:"; sed 's/^/    | /' "$log"
    return 1
}

stop() {
    [ -n "$SERVER_PID" ] && hull_proc_stop "$SERVER_PID" 10
    SERVER_PID=""
}

# No sanitizer / assertion report in any server log (ASan, UBSan, TSan
# builds run this suite too).
SAN_LOGS=""
check_sanitizers() {
    bad=""
    for l in "$WORK"/*.log; do
        [ -f "$l" ] || continue
        if grep -qE "ERROR: AddressSanitizer|runtime error:|WARNING: ThreadSanitizer|Assertion failed|FATAL: .* off the event-loop thread" "$l"; then
            bad="$bad $(basename "$l")"
            sed 's/^/    | /' "$l" | head -60
        fi
    done
    if [ -z "$bad" ]; then pass "$1: no sanitizer or assertion report"
    else fail "$1: sanitizer / assertion report in$bad"; fi
    rm -f "$WORK"/*.log
}

# req PATH [TIMEOUT]: prints "<status> <seconds>".
req() {
    curl -s -o /dev/null -m "${2:-15}" -w '%{http_code} %{time_total}' \
        "http://127.0.0.1:$PORT$1" 2>/dev/null || true
}

# in_range VALUE LO HI (floating point seconds)
in_range() {
    awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v >= lo && v <= hi) }'
}

STD="--max-run-ms 1000 --vm-thread-grace-ms 500"

run_runtime() {
    rt=$1; app=$2; man=$3
    echo "--- $rt ---"

    # ── off: today's behaviour ─────────────────────────────────────────
    next_port
    if start "$app" "$WORK/off.log" --max-run-ms 1000; then
        req "/stuck?ms=2500" > "$WORK/off_stuck" &
        sp=$!
        sleep 0.5
        out=$(req /ping)
        wait $sp || true
        st=$(cut -d' ' -f1 "$WORK/off_stuck")
        code=${out%% *}; t=${out#* }
        if [ "$code" = "200" ] && in_range "$t" 1.5 10; then
            pass "$rt off: /ping waits for the stuck run ($t s)"
        else
            fail "$rt off: /ping during a stuck run: $out (expected 200 after ~2 s)"
        fi
        if [ "$st" = "503" ]; then
            pass "$rt off: the stuck run trips once the call returns (503, the time-limit answer)"
        else
            fail "$rt off: stuck request answered $st (expected 503)"
        fi
        if grep -q "vm-thread" "$WORK/off.log"; then
            fail "$rt off: the VM thread was started without the flag"
        else
            pass "$rt off: no VM thread"
        fi
        stop
    fi

    # ── handler stuck ──────────────────────────────────────────────────
    next_port
    if start "$app" "$WORK/h.log" --vm-thread $STD; then
        out=$(req /park)
        [ "${out%% *}" = "200" ] && pass "$rt park: a parked handler resumes on the VM thread" \
            || fail "$rt park: /park answered $out"
        req "/stuck?ms=4000" > "$WORK/h_stuck" &
        sp=$!
        sleep 2
        during=$(req /ping 5)
        wait $sp || true
        stuck=$(cat "$WORK/h_stuck")
        code=${stuck%% *}; t=${stuck#* }
        if [ "$code" = "503" ] && in_range "$t" 1.2 3.2; then
            pass "$rt handler: the stuck request is answered 503 at deadline + grace ($t s)"
        else
            fail "$rt handler: stuck request: $stuck (expected 503 after ~1.5 s)"
        fi
        code=${during%% *}; t=${during#* }
        if [ "$code" = "503" ] && in_range "$t" 0 1.0; then
            pass "$rt handler: /ping while stuck is refused 503 at once ($t s)"
        else
            fail "$rt handler: /ping while stuck: $during (expected an immediate 503)"
        fi
        sleep 2.5
        after=$(req /ping)
        [ "${after%% *}" = "200" ] && pass "$rt handler: serving again once the call returned" \
            || fail "$rt handler: /ping after: $after"
        out=$(req /park)
        [ "${out%% *}" = "200" ] && pass "$rt handler: parked handlers still work after recovery" \
            || fail "$rt handler: /park after recovery: $out"
        if grep -q "run abandoned" "$WORK/h.log" && grep -q "serving again" "$WORK/h.log"; then
            pass "$rt handler: abandonment and recovery are logged"
        else
            fail "$rt handler: log lacks the abandonment / recovery lines"
            sed 's/^/    | /' "$WORK/h.log"
        fi

        # ── middleware stuck (same server) ─────────────────────────────
        mw=$(req "/mw/stuck?ms=3500")
        code=${mw%% *}; t=${mw#* }
        if [ "$code" = "503" ] && in_range "$t" 1.2 3.2; then
            pass "$rt middleware: a stuck middleware is answered 503 ($t s)"
        else
            fail "$rt middleware: $mw (expected 503 after ~1.5 s)"
        fi
        sleep 2.5
        out=$(req "/mw/stuck")
        [ "${out%% *}" = "200" ] && pass "$rt middleware: middleware passes again after recovery" \
            || fail "$rt middleware: after recovery: $out"

        # ── timer stuck (same server) ──────────────────────────────────
        before=$(curl -s "http://127.0.0.1:$PORT/timer/runs" | tr -dc '0-9')
        req /timer/stick > /dev/null
        sleep 2
        during=$(req /ping 5)
        sleep 2.5
        after=$(req /ping)
        runs=$(curl -s "http://127.0.0.1:$PORT/timer/runs" | tr -dc '0-9')
        sleep 0.5
        runs2=$(curl -s "http://127.0.0.1:$PORT/timer/runs" | tr -dc '0-9')
        if [ "${during%% *}" = "503" ] && [ "${after%% *}" = "200" ]; then
            pass "$rt timer: /ping 503 while a timer is stuck, 200 after"
        else
            fail "$rt timer: during=$during after=$after"
        fi
        if [ -n "$runs2" ] && [ -n "$runs" ] && [ "$runs2" -gt "$runs" ] && \
           [ "$runs" -gt "${before:-0}" ]; then
            pass "$rt timer: the timer keeps firing after recovery"
        else
            fail "$rt timer: runs before=$before after=$runs later=$runs2"
        fi
        stop
    fi

    # ── manifest turns it on ───────────────────────────────────────────
    next_port
    if start "$man" "$WORK/m.log" --max-run-ms 800; then
        out=$(req "/stuck?ms=3000")
        code=${out%% *}; t=${out#* }
        if [ "$code" = "503" ] && in_range "$t" 0.9 2.5; then
            pass "$rt manifest: limits.vm_thread abandons the stuck run ($t s)"
        else
            fail "$rt manifest: $out (expected 503 after ~1.2 s)"
        fi
        stop
    fi

    # ── HULL_VM_THREAD=1 turns it on ───────────────────────────────────
    next_port
    export HULL_VM_THREAD=1
    if start "$app" "$WORK/e.log" --max-run-ms 800 --vm-thread-grace-ms 400; then
        out=$(req "/stuck?ms=3000")
        code=${out%% *}; t=${out#* }
        if [ "$code" = "503" ] && in_range "$t" 0.9 2.5 && \
           grep -q "VM thread on" "$WORK/e.log"; then
            pass "$rt env: HULL_VM_THREAD=1 abandons the stuck run ($t s)"
        else
            fail "$rt env: $out (expected 503 after ~1.2 s)"
        fi
        stop
    fi
    unset HULL_VM_THREAD

    # ── hard ceiling ───────────────────────────────────────────────────
    next_port
    if start "$app" "$WORK/c.log" --vm-thread $STD --vm-thread-ceiling-ms 1500; then
        req "/stuck?ms=20000" > /dev/null &
        t0=$(date +%s)
        ( sleep 15; kill -9 "$SERVER_PID" 2>/dev/null ) 2>/dev/null &
        wd=$!
        st=0
        wait "$SERVER_PID" 2>/dev/null || st=$?
        kill "$wd" 2>/dev/null || true
        el=$(( $(date +%s) - t0 ))
        SERVER_PID=""
        if grep -q "hard ceiling reached" "$WORK/c.log" && [ "$el" -le 8 ]; then
            pass "$rt ceiling: the process exits after the ceiling (${el} s)"
        else
            fail "$rt ceiling: no ceiling exit (status $st, ${el} s)"
            sed 's/^/    | /' "$WORK/c.log"
        fi
        if [ "$st" = "75" ]; then
            pass "$rt ceiling: exit status 75 (EX_TEMPFAIL)"
        else
            fail "$rt ceiling: exit status $st (expected 75)"
        fi
        wait 2>/dev/null || true
    fi

    # ── SIGTERM during standby ─────────────────────────────────────────
    if hull_proc_graceful; then
        next_port
        if start "$app" "$WORK/t.log" --vm-thread $STD --drain-timeout 500; then
            req "/stuck?ms=20000" > /dev/null &
            sleep 2
            kill -TERM "$SERVER_PID" 2>/dev/null || true
            t0=$(date +%s)
            if hull_proc_wait "$SERVER_PID" 10; then
                el=$(( $(date +%s) - t0 ))
                pass "$rt sigterm: exits during standby (${el} s)"
            else
                fail "$rt sigterm: did not exit within 10 s of SIGTERM"
            fi
            SERVER_PID=""
            wait 2>/dev/null || true
        fi
    else
        skip "$rt sigterm: $(hull_proc_graceful_note)"
    fi
}

run_runtime lua app.lua manifest.lua
check_sanitizers lua
run_runtime js app.js manifest.js
check_sanitizers js

echo ""
echo "vm-thread e2e: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
