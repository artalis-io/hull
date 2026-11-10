#!/bin/sh
# e2e_ws_close.sh: a WebSocket the app closes itself still runs its on_close.
#
# Audit 44 M5 (a regression from #1007): conn:close() / conn.close() set the
# node's `closed` flag, and since #1007 the registry lookup skipped closed
# nodes, so when Keel delivered the close the runtime found no node: the
# app's on_close never ran, and the node, its conn object and the endpoint's
# connection count leaked (ws.connections() over-reported for good).
#
#   ws_audit44.app_close_runs_on_close  one close from on_open / on_message:
#                                        on_close ran, connections() == 0
#   ws_audit44.app_close_no_leak        N cycles on each path: N on_close
#                                        calls, nothing left registered
#
# Both runtimes. Requires build/hull, curl and python3 (skipped without it).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

HULL="${HULL:-build/hull}"
FIX="$(cd "$(dirname "$0")" && pwd)/fixtures/ws_close"
PORT_BASE="${PORT_BASE:-19980}"
CYCLES="${CYCLES:-20}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found"
    exit 0
fi

WORK=$(mktemp -d)
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill -9 "$SERVER_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

start_server() {
    _name=$1; _port=$2; _app=$3
    "$HULL" --no-sandbox -p "$_port" "$_app" >"$WORK/$_name.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 40); do
        curl -s -o /dev/null -m 2 "http://127.0.0.1:$_port/ping" && return 0
        sleep 0.25
    done
    return 1
}

stop_server() {
    kill -9 "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

# wait_state PORT WANT -> STATE (polled: the close lands on a later loop turn)
wait_state() {
    for _ in $(seq 1 40); do
        STATE=$(curl -s -m 2 "http://127.0.0.1:$1/state" || true)
        [ "$STATE" = "$2" ] && return 0
        sleep 0.1
    done
    return 1
}

port=$PORT_BASE
for rt in lua js; do
    echo "=== $rt ==="
    port=$((port + 1))
    if ! start_server "$rt" "$port" "$FIX/app.$rt"; then
        fail "$rt: server did not start ($(cat "$WORK/$rt.log"))"
        stop_server
        continue
    fi

    # app_close_runs_on_close: one close from on_open, one from on_message.
    python3 "$FIX/ws_cycle.py" "$port" /open 0 1 >"$WORK/$rt.one" 2>&1 || true
    python3 "$FIX/ws_cycle.py" "$port" /msg 1 1 >>"$WORK/$rt.one" 2>&1 || true
    if wait_state "$port" "closes=2 open=0 msg=0 sent=0"; then
        pass "ws_audit44.app_close_runs_on_close ($rt)"
    else
        fail "ws_audit44.app_close_runs_on_close ($rt): state '$STATE' ($(tr '\n' ' ' <"$WORK/$rt.one"))"
    fi

    # app_close_no_leak: N cycles on each path, nothing left registered.
    python3 "$FIX/ws_cycle.py" "$port" /open 0 "$CYCLES" >"$WORK/$rt.n" 2>&1 || true
    python3 "$FIX/ws_cycle.py" "$port" /msg 1 "$CYCLES" >>"$WORK/$rt.n" 2>&1 || true
    want=$((2 + 2 * CYCLES))
    if wait_state "$port" "closes=$want open=0 msg=0 sent=0"; then
        pass "ws_audit44.app_close_no_leak ($rt)"
    else
        fail "ws_audit44.app_close_no_leak ($rt): state '$STATE', want closes=$want ($(tr '\n' ' ' <"$WORK/$rt.n"))"
    fi

    if grep -qiE "AddressSanitizer|LeakSanitizer|runtime error:|Assertion|Segmentation" "$WORK/$rt.log"; then
        fail "$rt: sanitizer / assertion report in the log"
        sed -n 1,40p "$WORK/$rt.log"
    fi
    stop_server
done

echo ""
echo "e2e_ws_close: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
