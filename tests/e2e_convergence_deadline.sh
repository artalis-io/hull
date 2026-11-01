#!/bin/sh
# e2e_convergence_deadline.sh: a blocking native call ends by its run's
# deadline (audit convergence §6.2, the phase-2 confirming audit's M1 / M2).
#
# A synchronous http.get and a synchronous compute.call each block the run -
# and the event loop - inside ONE binding call, where the run watchdog's flag
# is never seen. Their own timeouts (30 s / 60 s for http, 10 s / 1 h for
# compute) ran past the handler's limits.request_ms: a /slow handler with a
# 1.5 s deadline answered after 8 s, and a /ping sent meanwhile waited 7.7 s.
# Both now clamp their timeout to what is left of the run.
#
#   - sync http: a local upstream answers after 6 s; the handler (deadline
#     1.5 s) calls http.get with a per-call 10 s timeout. It must end, and a
#     /ping sent meanwhile be answered, well before the upstream answers.
#   - sync compute: loop_forever (a guest whose hull_process is `loop br 0`,
#     never returning) runs with gas at its ceiling and a per-call 10 s
#     timeout under the same 1.5 s deadline.
#   - guest time is charged: with Hull's default instruction budget (100M)
#     and request deadline (10 s), a loop_forever call with a 3 s timeout
#     used to cost the run nothing (the guest call was excluded from the
#     binding dispatcher's measured time, and AOT code is never gas-metered)
#     and the handler answered 200; it now trips the budget (500).
#
# Both runtimes. The deadline cases run with a huge --max-instructions so the
# deadline, not the budget, is what stops them.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

HULL="${HULL:-build/hull}"
PORT_BASE="${PORT_BASE:-19980}"
SLOW_PORT=$((PORT_BASE + 9))
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "e2e_convergence_deadline: python3 not found, skipping"
    exit 0
fi

WORK=$(mktemp -d)
SERVER_PID=""
SLOW_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    [ -n "$SLOW_PID" ] && kill "$SLOW_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

now_ms() { python3 -c 'import time; print(int(time.monotonic() * 1000))'; }

# The slow upstream: answers every GET after 6 s.
cat > "$WORK/slow.py" <<'EOF'
import http.server, sys, time
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(6)
        b = b"late"
        self.send_response(200)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def log_message(self, *a):
        pass
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
EOF
python3 "$WORK/slow.py" "$SLOW_PORT" &
SLOW_PID=$!

start_server() {   # start_server <dir> <entry> <port> [hull args...]
    dir=$1; entry=$2; port=$3; shift 3
    "$HULL" --no-sandbox -p "$port" "$@" "$dir/$entry" >"$dir/log" 2>&1 &
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

write_lua_app() {   # write_lua_app <dir> <limits or empty>
    mkdir -p "$1/compute"
    cp tests/fixtures/compute/loop_forever.wasm "$1/compute/loop_forever.wasm"
    cat > "$1/app.lua" <<EOF
local app = require("hull.app")
app.manifest({ modules = { "hull/http-server@1", "hull/http-client@1",
                           "hull/compute@1" },
               hosts = { "127.0.0.1" }$2 })
local http = require("hull.http-client")
local compute = require("hull.compute")
app.get("/ping", function(req, res) res:text("pong") end)
app.get("/http", function(req, res)
    local ok, r = pcall(http.get, "http://127.0.0.1:$SLOW_PORT/",
                        { timeout_ms = 10000 })
    res:text("http " .. tostring(ok) .. " " ..
             tostring(type(r) == "table" and r.status or r))
end)
app.get("/compute", function(req, res)
    local t = tonumber(req.query and req.query.t) or 10000
    local out, err = compute.call("loop_forever", "x",
                                  { gas = 2000000000, timeout_ms = t })
    res:text("compute " .. tostring(out) .. " " .. tostring(err))
end)
EOF
}

write_js_app() {   # write_js_app <dir> <limits or empty>
    mkdir -p "$1/compute"
    cp tests/fixtures/compute/loop_forever.wasm "$1/compute/loop_forever.wasm"
    cat > "$1/app.js" <<EOF
import { app } from "hull:app";
import { httpClient as http } from "hull:http-client";
import { compute } from "hull:compute";
app.manifest({ modules: ["hull/http-server@1", "hull/http-client@1",
                         "hull/compute@1"],
               hosts: ["127.0.0.1"]$2 });
app.get("/ping", (req, res) => res.text("pong"));
app.get("/http", (req, res) => {
    let r;
    try { r = String(http.get("http://127.0.0.1:$SLOW_PORT/", { timeoutMs: 10000 }).status); }
    catch (e) { r = "error " + String(e && e.message ? e.message : e); }
    res.text("http " + r);
});
app.get("/compute", (req, res) => {
    const t = Number((req.query && req.query.t) || 10000);
    let r;
    try { r = String(compute.call("loop_forever", "x", { gas: 2000000000, timeoutMs: t })); }
    catch (e) { r = "error " + String(e && e.message ? e.message : e); }
    res.text("compute " + r);
});
EOF
}

# One blocking call under a 1.5 s request deadline, with a /ping sent while
# it runs. Before the fix /http took ~6 s and /compute ~10 s.
check_clamped() {   # check_clamped <rt> <port> <path> <what>
    rt=$1; port=$2; path=$3; what=$4
    t0=$(now_ms)
    curl -s -o "$WORK/body" -w '%{http_code}' --max-time 30 \
        "http://127.0.0.1:$port$path" >"$WORK/code" &
    cpid=$!
    sleep 0.3
    tp0=$(now_ms)
    ping=$(curl -s --max-time 30 "http://127.0.0.1:$port/ping")
    tp1=$(now_ms)
    wait "$cpid"
    t1=$(now_ms)
    el=$((t1 - t0)); lat=$((tp1 - tp0))
    if [ "$el" -lt 4000 ]; then
        pass "$rt: a sync $what ends by the run's 1.5 s deadline (${el} ms, $(cat "$WORK/code"))"
    else
        fail "$rt: a sync $what ends by the run's 1.5 s deadline" \
             "$(cat "$WORK/code") $(cat "$WORK/body") after ${el} ms"
    fi
    if [ "$ping" = "pong" ] && [ "$lat" -lt 4000 ]; then
        pass "$rt: /ping sent during the $what is served (${lat} ms)"
    else
        fail "$rt: /ping sent during the $what is served" "$ping in ${lat} ms"
    fi
}

run_runtime() {   # run_runtime <rt> <entry> <writer> <limits> <port>
    rt=$1; entry=$2; writer=$3; limits=$4; rport=$5; bport=$(($5 + 1))
    echo "=== $rt: blocking calls under limits.request_ms ==="
    "$writer" "$WORK/$rt" "$limits"
    start_server "$WORK/$rt" "$entry" "$rport" --max-instructions 100000000000
    check_clamped "$rt" "$rport" /http "http.get"
    check_clamped "$rt" "$rport" /compute "compute.call"
    stop_server

    echo "=== $rt: guest time is charged to the instruction budget ==="
    "$writer" "$WORK/$rt-budget" ""
    start_server "$WORK/$rt-budget" "$entry" $bport
    code=$(curl -s -o "$WORK/body" -w '%{http_code}' --max-time 30 \
        "http://127.0.0.1:$bport/compute?t=3000")
    if [ "$code" = "500" ] && grep -q "instruction limit" "$WORK/$rt-budget/log"; then
        pass "$rt: 3 s of guest code trips the default instruction budget"
    else
        fail "$rt: 3 s of guest code trips the default instruction budget" \
             "$code $(cat "$WORK/body")"
        tail -5 "$WORK/$rt-budget/log"
    fi
    out=$(curl -s --max-time 5 "http://127.0.0.1:$bport/ping")
    [ "$out" = "pong" ] && pass "$rt: the server keeps serving" \
                        || fail "$rt: the server keeps serving" "$out"
    stop_server
}

run_runtime lua app.lua write_lua_app ", limits = { request_ms = 1500 }" $((PORT_BASE + 1))
run_runtime js app.js write_js_app ", limits: { requestMs: 1500 }" $((PORT_BASE + 3))

echo ""
echo "e2e_convergence_deadline: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
