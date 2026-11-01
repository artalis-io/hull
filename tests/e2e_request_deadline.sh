#!/bin/sh
# e2e_request_deadline.sh: per-request wall budget (audit convergence §6.2).
#
# An HTTP handler run is bounded by HL_RUN_HTTP_DEFAULT_MS (10 s) unless the
# app sets manifest `limits.request_ms` (JS `limits.requestMs`); timers and
# WebSocket / SSE callbacks by HL_RUN_CALLBACK_DEFAULT_MS (5 s) unless
# `limits.callback_ms`. --max-run-ms overrides both. A run stopped by its
# deadline is answered 503 naming the limit, the server keeps serving, and a
# handler that PARKS (hull.sleep) is not charged its wait: each resume is a
# run of its own.
#
# The server runs with a huge --max-instructions so the busy loops below are
# stopped by the deadline, not the instruction budget.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

HULL="${HULL:-build/hull}"
PORT_BASE="${PORT_BASE:-19960}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

WORK=$(mktemp -d)
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

now_ms() {
    # POSIX sh has no sub-second clock; python3 or perl give one.
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import time; print(int(time.monotonic() * 1000))'
    else
        perl -MTime::HiRes=time -e 'printf("%d\n", time() * 1000)'
    fi
}

start_server() {   # start_server <dir> <entry> <port> [hull args...]
    dir=$1; entry=$2; port=$3; shift 3
    "$HULL" --no-sandbox --max-instructions 100000000000 -p "$port" "$@" \
        "$dir/$entry" >"$dir/log" 2>&1 &
    SERVER_PID=$!
    i=0
    while [ $i -lt 40 ]; do
        if curl -s -o /dev/null "http://127.0.0.1:$port/ping"; then return 0; fi
        sleep 0.25
        i=$((i + 1))
    done
    echo "FAIL: server did not start"; cat "$dir/log"; exit 1
}

stop_server() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

write_lua_app() {   # write_lua_app <dir> <limits table or empty>
    mkdir -p "$1"
    cat > "$1/app.lua" <<EOF
local app = require("hull.app")
app.manifest({ modules = { "hull/http-server@1", "hull/timers@1" }$2 })
local fired = 0
app.every(300, function()
    fired = fired + 1
    if fired == 1 then while true do end end
end)
app.get("/ping", function(req, res) res:text("pong") end)
app.get("/spin", function(req, res) while true do end end)
app.get("/sleepy", function(req, res)
    hull.sleep(1200) hull.sleep(1200)
    res:text("slept")
end)
app.get("/fired", function(req, res) res:text(tostring(fired)) end)
EOF
}

write_js_app() {   # write_js_app <dir> <limits object or empty>
    mkdir -p "$1"
    cat > "$1/app.js" <<EOF
import { app } from "hull:app";
app.manifest({ modules: ["hull/http-server@1", "hull/timers@1"]$2 });
let fired = 0;
app.every(300, () => {
    fired++;
    if (fired === 1) { while (true) {} }
});
app.get("/ping", (req, res) => res.text("pong"));
app.get("/spin", (req, res) => { while (true) {} });
app.get("/sleepy", async (req, res) => {
    await hull.sleep(1200); await hull.sleep(1200);
    res.text("slept");
});
app.get("/fired", (req, res) => res.text(String(fired)));
EOF
}

# A slow handler under a 1.5 s app deadline, a /ping sent while it spins, a
# handler that parks longer than its deadline, and a timer that spins.
check_configured() {   # check_configured <runtime> <dir> <entry> <port>
    rt=$1; dir=$2; entry=$3; port=$4
    echo "=== $rt: limits.request_ms / callback_ms ==="
    start_server "$dir" "$entry" "$port"

    # The timer's first fire spins; its 1 s callback deadline stops it and
    # the timer keeps firing.
    sleep 2
    if grep -q "time limit" "$dir/log"; then
        pass "$rt: a spinning timer is stopped by the callback deadline"
    else
        fail "$rt: a spinning timer is stopped by the callback deadline" "$(tail -3 "$dir/log")"
    fi
    n=$(curl -s --max-time 5 "http://127.0.0.1:$port/fired")
    [ -n "$n" ] && [ "$n" -ge 2 ] 2>/dev/null \
        && pass "$rt: the timer fires again after it was stopped ($n)" \
        || fail "$rt: the timer fires again after it was stopped" "$n"

    curl -s -o "$dir/spin.body" -w '%{http_code}' --max-time 30 \
        "http://127.0.0.1:$port/spin" >"$dir/spin.code" &
    spin_pid=$!
    sleep 0.3
    t0=$(now_ms)
    ping=$(curl -s --max-time 20 "http://127.0.0.1:$port/ping")
    t1=$(now_ms)
    wait "$spin_pid"
    code=$(cat "$dir/spin.code")
    body=$(cat "$dir/spin.body")
    [ "$code" = "503" ] \
        && pass "$rt: a handler past its deadline is answered 503" \
        || fail "$rt: a handler past its deadline is answered 503" "$code"
    case "$body" in
        *"time limit"*) pass "$rt: the 503 names the limit" ;;
        *) fail "$rt: the 503 names the limit" "$body" ;;
    esac
    lat=$((t1 - t0))
    if [ "$ping" = "pong" ] && [ "$lat" -lt 4000 ]; then
        pass "$rt: /ping sent during the spin is served within the deadline (${lat} ms)"
    else
        fail "$rt: /ping sent during the spin is served within the deadline" "$ping in ${lat} ms"
    fi

    out=$(curl -s --max-time 20 "http://127.0.0.1:$port/sleepy")
    [ "$out" = "slept" ] \
        && pass "$rt: a handler parked past its deadline is not charged the wait" \
        || fail "$rt: a handler parked past its deadline is not charged the wait" "$out"
    out=$(curl -s --max-time 5 "http://127.0.0.1:$port/ping")
    [ "$out" = "pong" ] \
        && pass "$rt: the server keeps serving" \
        || fail "$rt: the server keeps serving" "$out"
    stop_server
}

write_lua_app "$WORK/lua" ", limits = { request_ms = 1500, callback_ms = 1000 }"
check_configured lua "$WORK/lua" app.lua $((PORT_BASE + 1))
write_js_app "$WORK/js" ", limits: { requestMs: 1500, callbackMs: 1000 }"
check_configured js "$WORK/js" app.js $((PORT_BASE + 2))

# Hull's default for an HTTP handler (10 s), with no limits in the manifest.
for rt in lua js; do
    echo "=== $rt: the default HTTP deadline ==="
    if [ "$rt" = lua ]; then
        write_lua_app "$WORK/def-$rt" ""; entry=app.lua
    else
        write_js_app "$WORK/def-$rt" ""; entry=app.js
    fi
    port=$((PORT_BASE + 3)); [ "$rt" = js ] && port=$((PORT_BASE + 4))
    start_server "$WORK/def-$rt" "$entry" "$port"
    sleep 6   # past the timer's 5 s callback default
    t0=$(now_ms)
    code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 40 "http://127.0.0.1:$port/spin")
    t1=$(now_ms)
    el=$((t1 - t0))
    if [ "$code" = "503" ] && [ "$el" -ge 9000 ] && [ "$el" -lt 16000 ]; then
        pass "$rt: a spinning handler is stopped at the 10 s default (${el} ms)"
    else
        fail "$rt: a spinning handler is stopped at the 10 s default" "$code after ${el} ms"
    fi
    stop_server
done

# --max-run-ms overrides the manifest.
echo "=== lua: --max-run-ms overrides limits.request_ms ==="
write_lua_app "$WORK/ovr" ", limits = { request_ms = 20000 }"
start_server "$WORK/ovr" app.lua $((PORT_BASE + 5)) --max-run-ms 1000
sleep 1.5
t0=$(now_ms)
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 30 "http://127.0.0.1:$((PORT_BASE + 5))/spin")
t1=$(now_ms)
el=$((t1 - t0))
if [ "$code" = "503" ] && [ "$el" -lt 5000 ]; then
    pass "lua: --max-run-ms 1000 wins over request_ms 20000 (${el} ms)"
else
    fail "lua: --max-run-ms 1000 wins over request_ms 20000" "$code after ${el} ms"
fi
stop_server

echo ""
echo "e2e_request_deadline: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
