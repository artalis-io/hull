#!/bin/sh
# E2E request-concurrency caps (audit convergence §6.4).
#
#   1. Per client (manifest per_ip_concurrency = 8): eight slow requests from
#      one client address are in flight;
#      the ninth from that address gets 429 + Retry-After, while another
#      address is served. Client addresses come from X-Forwarded-For, which
#      the fixture trusts from 127.0.0.1 (limits.trusted_proxies).
#   2. The cap releases: once the slow requests finish, the client is served.
#   3. Per route: /narrow allows two in flight; a third (another client) gets
#      503 + Retry-After.
#   4. Untrusted X-Forwarded-For: without trusted_proxies the header is the
#      client's own text and does not pick the key - nine requests that each
#      claim another address are still one client. That fixture sets no
#      limit: the default is half --max-connections (16 here, so 8).
#   5. --max-requests-per-ip 0 turns the per-client cap off.
#   6. Open connections per client (limits.per_ip_connections = 4): client A
#      holds four idle keep-alive connections (one request each, then
#      nothing); its fifth connection is refused while client B (::1 against
#      a dual-stack listener, or 127.0.0.2 on Linux) is served. A trusted
#      proxy's connections are not counted. Idle and WebSocket connections
#      counted for nothing before (confirming audit M1).
#   7. Header and idle deadlines: a client sending its request head a byte at
#      a time (each byte well inside the read timeout) is closed at
#      --header-timeout; a keep-alive connection that sends nothing after its
#      response is closed at --keepalive-timeout.
#
# Usage: sh tests/e2e_concurrency.sh  (RUNTIME=lua|js|all)
# Requires: build/hull, curl, python3 (sections 6-7; skipped without it).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL_BIN:-$SRCDIR/build/hull}"
FIX="$SRCDIR/tests/fixtures/concurrency"
RUNTIME=${RUNTIME:-all}
PASS=0
FAIL=0
PORT=19911
HPID=""
BG_PIDS=""
TMP=$(mktemp -d "${TMPDIR:-/tmp}/hull-conc.XXXXXX")

if [ ! -x "$HULL" ]; then
    echo "e2e_concurrency: hull binary not found at $HULL - run 'make' first"
    exit 1
fi

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

stop_server() {
    if [ -n "$HPID" ]; then
        kill "$HPID" 2>/dev/null
        wait "$HPID" 2>/dev/null
        HPID=""
    fi
}
cleanup() { stop_server; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

start_server() {
    # $1 = entry file, rest = extra hull flags
    _entry=$1; shift
    "$HULL" "$_entry" -p "$PORT" --no-sandbox "$@" >"$TMP/server.log" 2>&1 &
    HPID=$!
    for _i in $(seq 1 50); do
        if curl -s -o /dev/null "http://127.0.0.1:$PORT/ping" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "  server did not start; log:"
    cat "$TMP/server.log"
    return 1
}

# status of one request: $1 = path, $2 = X-Forwarded-For ("" = none)
status() {
    if [ -n "$2" ]; then
        curl -s -o /dev/null -w '%{http_code}' -H "X-Forwarded-For: $2" \
            "http://127.0.0.1:$PORT$1"
    else
        curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT$1"
    fi
}

# launch $1 background requests to $2 from client $3 ("" = none, "seq" =
# 10.9.0.N), each writing its status to $TMP/bg.N
launch() {
    _n=$1; _path=$2; _xff=$3
    for _i in $(seq 1 "$_n"); do
        _x=$_xff
        [ "$_xff" = "seq" ] && _x="10.9.0.$_i"
        ( status "$_path" "$_x" >"$TMP/bg.$_i" ) &
        BG_PIDS="$BG_PIDS $!"
    done
    sleep 0.5   # all parked in hull.sleep
}

# Wait for the background requests. Their pids are recorded as they start:
# `$(jobs -p)` runs in a subshell, which in dash (Linux /bin/sh) has no job
# table, so it listed nothing and the requests were never waited for.
wait_bg() {
    for j in $BG_PIDS; do wait "$j" 2>/dev/null; done
    BG_PIDS=""
}

count_bg() {
    # $1 = expected status; prints how many bg.* files hold it
    _c=0
    for _f in "$TMP"/bg.*; do
        [ -f "$_f" ] || continue
        [ "$(cat "$_f")" = "$1" ] && _c=$((_c + 1))
    done
    echo "$_c"
}

run_trusted() {
    _entry=$1
    echo "-- $(basename "$_entry"): per-client and per-route caps"
    start_server "$_entry" || { fail "server start ($_entry)"; return; }
    rm -f "$TMP"/bg.*

    launch 8 /slow 203.0.113.7
    s9=$(status /slow 203.0.113.7)
    [ "$s9" = "429" ] && pass "9th request from one client refused (429)" \
                      || fail "9th request from one client: got $s9, want 429"
    ra=$(curl -s -D - -o /dev/null -H "X-Forwarded-For: 203.0.113.7" \
            "http://127.0.0.1:$PORT/ping" | tr -d '\r' | grep -i '^retry-after:')
    [ -n "$ra" ] && pass "429 carries Retry-After ($ra)" \
                 || fail "429 has no Retry-After"
    sb=$(status /ping 198.51.100.9)
    [ "$sb" = "200" ] && pass "another client is served while the first is capped" \
                      || fail "another client: got $sb, want 200"
    s6=$(status /ping "2001:db8:1:2::5")
    [ "$s6" = "200" ] && pass "an IPv6 client is served" \
                      || fail "IPv6 client: got $s6, want 200"
    wait_bg
    ok=$(count_bg 200)
    [ "$ok" = "8" ] && pass "the 8 admitted slow requests all completed" \
                    || fail "admitted slow requests completed: $ok of 8"
    sa=$(status /ping 203.0.113.7)
    [ "$sa" = "200" ] && pass "the client is served again once its requests finish" \
                      || fail "client after release: got $sa, want 200"

    # Clients that hang up mid-request: their handlers still run (and count)
    # until they finish, then leave no entry behind - the closed connections
    # are released or swept.
    for _i in $(seq 1 8); do
        curl -s -o /dev/null --max-time 0.3 -H "X-Forwarded-For: 203.0.113.50" \
            "http://127.0.0.1:$PORT/slow" &
        BG_PIDS="$BG_PIDS $!"
    done
    wait_bg
    sleep 1.6   # the abandoned handlers still count until they finish
    sh_=$(status /ping 203.0.113.50)
    [ "$sh_" = "200" ] && pass "hung-up requests do not hold the client's slots" \
                       || fail "after hang-ups: got $sh_, want 200"

    rm -f "$TMP"/bg.*
    launch 2 /narrow seq
    sn=$(status /narrow 192.0.2.77)
    [ "$sn" = "503" ] && pass "route limit refuses the 3rd /narrow (503)" \
                      || fail "3rd /narrow: got $sn, want 503"
    sp=$(status /ping 192.0.2.77)
    [ "$sp" = "200" ] && pass "other routes are unaffected by the route limit" \
                      || fail "/ping during /narrow cap: got $sp, want 200"
    wait_bg
    stop_server
}

run_untrusted() {
    echo "-- untrusted X-Forwarded-For"
    # No manifest limit: the default cap is half of --max-connections. The
    # connection cap (also half by default) is off here: it would refuse the
    # ninth connection before the request cap could answer 429.
    start_server "$FIX/untrusted/app.lua" --max-connections 16 \
        --max-connections-per-ip 0 || { fail "server start (untrusted)"; return; }
    rm -f "$TMP"/bg.*
    launch 8 /slow seq
    s9=$(status /slow 10.9.0.99)
    [ "$s9" = "429" ] && pass "spoofed X-Forwarded-For does not escape the cap" \
                      || fail "spoofed XFF 9th request: got $s9, want 429"
    wait_bg
    stop_server

    echo "-- default cap without --max-connections"
    start_server "$FIX/untrusted/app.lua" || { fail "server start (default)"; return; }
    rm -f "$TMP"/bg.*
    launch 12 /slow ""
    s13=$(status /ping "")
    [ "$s13" = "200" ] && pass "default cap (half of 256) admits 13 concurrent requests" \
                       || fail "default cap: 13th concurrent request got $s13, want 200"
    wait_bg
    stop_server

    echo "-- --max-requests-per-ip 0"
    start_server "$FIX/untrusted/app.lua" --max-requests-per-ip 0 || { fail "server start (off)"; return; }
    rm -f "$TMP"/bg.*
    launch 10 /slow ""
    s11=$(status /ping "")
    [ "$s11" = "200" ] && pass "cap off: 11th concurrent request served" \
                       || fail "cap off: got $s11, want 200"
    wait_bg
    ok=$(count_bg 200)
    [ "$ok" = "10" ] && pass "cap off: all 10 slow requests completed" \
                     || fail "cap off: $ok of 10 completed"
    stop_server

    echo "-- --max-requests-per-ip 2"
    start_server "$FIX/untrusted/app.lua" --max-requests-per-ip 2 || { fail "server start (2)"; return; }
    rm -f "$TMP"/bg.*
    launch 2 /slow ""
    s3=$(status /ping "")
    [ "$s3" = "429" ] && pass "flag overrides the default (3rd refused at 2)" \
                      || fail "--max-requests-per-ip 2: got $s3, want 429"
    wait_bg
    stop_server
}

HELPER="$SRCDIR/tests/e2e_conn_helper.py"
conn() { python3 "$HELPER" "$@"; }

run_conncap() {
    _entry=$1
    echo "-- $(basename "$_entry"): open connections per client"
    # A dual-stack listener, so ::1 is a second client on every host.
    _b=127.0.0.1; _hostb=""
    if start_server "$_entry" -b :: 2>/dev/null && [ "$(conn probe ::1 "$PORT")" = "200" ]; then
        _hostb=::1
    else
        stop_server
        start_server "$_entry" -b 0.0.0.0 || { fail "server start (conncap)"; return; }
        if [ "$(conn probe 127.0.0.2 "$PORT")" = "200" ]; then _hostb=127.0.0.2; fi
    fi
    conn hold "$_b" "$PORT" 4 3 >"$TMP/hold.out" &
    _hp=$!
    sleep 1
    [ "$(cat "$TMP/hold.out")" = "ok 4" ] && pass "client A holds 4 idle keep-alive connections" \
        || fail "client A: $(cat "$TMP/hold.out"), want ok 4"
    s5=$(conn probe "$_b" "$PORT")
    [ "$s5" != "200" ] && pass "client A's 5th connection is refused ($s5)" \
                       || fail "client A's 5th connection was served (cap not applied)"
    if [ -n "$_hostb" ]; then
        sb=$(conn probe "$_hostb" "$PORT")
        [ "$sb" = "200" ] && pass "client B ($_hostb) is served while A holds its cap" \
                          || fail "client B ($_hostb): got $sb, want 200"
    else
        echo "  SKIP: no second loopback address for client B"
    fi
    wait "$_hp" 2>/dev/null
    sleep 0.3
    sa=$(conn probe "$_b" "$PORT")
    [ "$sa" = "200" ] && pass "closed connections give their slots back" \
                      || fail "client A after closing: got $sa, want 200"
    stop_server
}

run_conncap_trusted() {
    echo "-- trusted proxy connections are not counted"
    start_server "$FIX/app.lua" --max-connections-per-ip 2 || { fail "server start (trusted conn)"; return; }
    conn hold 127.0.0.1 "$PORT" 5 1 >"$TMP/hold.out"
    [ "$(cat "$TMP/hold.out")" = "ok 5" ] && pass "a trusted proxy holds 5 connections over a cap of 2" \
        || fail "trusted proxy: $(cat "$TMP/hold.out"), want ok 5"
    stop_server

    echo "-- --max-connections-per-ip 0"
    start_server "$FIX/conncap/app.lua" --max-connections-per-ip 0 || { fail "server start (conn off)"; return; }
    conn hold 127.0.0.1 "$PORT" 6 1 >"$TMP/hold.out"
    [ "$(cat "$TMP/hold.out")" = "ok 6" ] && pass "cap off: 6 connections from one client" \
        || fail "cap off: $(cat "$TMP/hold.out"), want ok 6"
    stop_server
}

run_deadlines() {
    echo "-- header and keep-alive deadlines"
    start_server "$FIX/conncap/app.lua" --header-timeout 1500 --keepalive-timeout 1000 \
        || { fail "server start (deadlines)"; return; }
    d=$(conn drip 127.0.0.1 "$PORT" 0.3 8)
    case "$d" in
        "closed 1."*|"closed 2."*) pass "a slow header sender is closed at the header deadline ($d)" ;;
        *) fail "slow header sender: $d, want closed after ~1.5 s" ;;
    esac
    i=$(conn idle 127.0.0.1 "$PORT" 8)
    case "$i" in
        "closed 1."*|"closed 0."*|"closed 2."*) pass "an idle keep-alive connection is closed ($i)" ;;
        *) fail "idle keep-alive connection: $i, want closed after ~1 s" ;;
    esac
    s=$(conn probe 127.0.0.1 "$PORT")
    [ "$s" = "200" ] && pass "a prompt request is unaffected by the deadlines" \
                     || fail "prompt request: got $s, want 200"
    stop_server
}

case "$RUNTIME" in
    lua) run_trusted "$FIX/app.lua" ;;
    js)  run_trusted "$FIX/app.js" ;;
    *)   run_trusted "$FIX/app.lua"; run_trusted "$FIX/app.js" ;;
esac
run_untrusted
if command -v python3 >/dev/null 2>&1; then
    case "$RUNTIME" in
        lua) run_conncap "$FIX/conncap/app.lua" ;;
        js)  run_conncap "$FIX/conncap/app.js" ;;
        *)   run_conncap "$FIX/conncap/app.lua"; run_conncap "$FIX/conncap/app.js" ;;
    esac
    run_conncap_trusted
    run_deadlines
else
    echo "  SKIP: python3 not found (connection cap and deadline sections)"
fi

echo ""
echo "e2e_concurrency: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
