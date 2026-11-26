#!/bin/sh
# e2e_host_port.sh: a manifest.hosts entry "host:port" admits that port only
# (#1227). The app reaches its own server through "127.0.0.1:<its port>";
# the same host on another port is refused with the code a host outside
# manifest.hosts gets (`denied`), by http.fetch, a synchronous http.get and
# ws.connect, before anything is dialled. Both runtimes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PORT="${PORT:-19917}"
OTHER=$((PORT + 1))
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: e2e_host_port needs python3 to read JSON"
    exit 0
fi

HULL=$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")
TMPDIR=$(mktemp -d)
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

mkdir -p "$TMPDIR/lua" "$TMPDIR/js"
cat > "$TMPDIR/lua/app.lua" <<EOF2
local app = require("hull.app")
app.manifest({
    modules = { "hull/http-server@1", "hull/http-client@1", "hull/web/ws-client@1" },
    hosts = { "127.0.0.1:$PORT" },
})
local http = require("hull.http-client")
local ws = require("hull.web.ws-client")
local function report(res, ok, e)
    if ok then return res:json({ code = "ok", status = e and e.status }) end
    local msg = tostring(e)
    res:json({ code = msg:match("^%[hull:([%w_]+)%] ") or "none", msg = msg })
end
app.get("/pong", function(req, res) res:text("pong") end)
app.get("/same", function(req, res)
    report(res, pcall(http.async.get, "http://127.0.0.1:$PORT/pong"))
end)
app.get("/fetch", function(req, res)
    report(res, pcall(http.async.get, "http://127.0.0.1:$OTHER/pong"))
end)
app.get("/get", function(req, res)
    report(res, pcall(http.get, "http://127.0.0.1:$OTHER/pong"))
end)
app.get("/ws", function(req, res)
    report(res, pcall(ws.connect, "ws://127.0.0.1:$OTHER/", {}))
end)
EOF2

cat > "$TMPDIR/js/app.js" <<EOF2
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
import { wsClient } from "hull:web:ws-client";
app.manifest({
    modules: ["hull/http-server@1", "hull/http-client@1", "hull/web/ws-client@1"],
    hosts: ["127.0.0.1:$PORT"],
});
const report = (res, e) => res.json({ code: (e && e.code) || "none",
                                      msg: String(e && e.message) });
app.get("/pong", (req, res) => res.text("pong"));
app.get("/same", async (req, res) => {
    try { const r = await httpClient.async.get("http://127.0.0.1:$PORT/pong");
          res.json({ code: "ok", status: r.status }); }
    catch (e) { report(res, e); }
});
app.get("/fetch", async (req, res) => {
    try { await httpClient.async.get("http://127.0.0.1:$OTHER/pong"); res.json({ code: "ok" }); }
    catch (e) { report(res, e); }
});
app.get("/get", (req, res) => {
    try { httpClient.get("http://127.0.0.1:$OTHER/pong"); res.json({ code: "ok" }); }
    catch (e) { report(res, e); }
});
app.get("/ws", (req, res) => {
    try { wsClient.connect("ws://127.0.0.1:$OTHER/", {}); res.json({ code: "ok" }); }
    catch (e) { report(res, e); }
});
EOF2

# A manifest whose entry names a port that is not one is refused at load.
mkdir -p "$TMPDIR/bad"
cat > "$TMPDIR/bad/app.lua" <<'EOF2'
local app = require("hull.app")
app.manifest({ modules = { "hull/http-server@1", "hull/http-client@1" },
               hosts = { "127.0.0.1:0" } })
app.get("/", function(req, res) res:text("x") end)
EOF2

jget() {
    printf '%s' "$1" | python3 -c "import sys, json
try: print(json.load(sys.stdin).get(sys.argv[1]))
except Exception: print('?')" "$2"
}

wait_up() {
    i=0
    while [ $i -lt 50 ]; do
        curl -s -o /dev/null "http://127.0.0.1:$PORT/pong" && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

run_runtime() {
    rt=$1; entry=$2
    echo "-- $rt --"
    ( cd "$TMPDIR/$rt" && exec "$HULL" "$entry" -p "$PORT" > "$TMPDIR/$rt.log" 2>&1 ) &
    SERVER_PID=$!
    if ! wait_up; then
        fail "$rt: server did not start" "$(tail -5 "$TMPDIR/$rt.log")"
        return
    fi
    out=$(curl -sS -m 10 "http://127.0.0.1:$PORT/same")
    [ "$(jget "$out" code)" = "ok" ] && [ "$(jget "$out" status)" = "200" ] \
        && pass "$rt: http.fetch reaches the admitted host:port" \
        || fail "$rt: admitted host:port" "$out"
    for route in fetch get ws; do
        out=$(curl -sS -m 10 "http://127.0.0.1:$PORT/$route")
        if [ "$(jget "$out" code)" = "denied" ] && \
           printf '%s' "$(jget "$out" msg)" | grep -q "host '127.0.0.1:$OTHER' is not in manifest.hosts"; then
            pass "$rt: $route to the same host on another port is refused, coded denied"
        else
            fail "$rt: $route to another port" "$out"
        fi
    done
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

run_runtime lua app.lua
run_runtime js app.js

echo "-- load check --"
if out=$(cd "$TMPDIR/bad" && "$HULL" app.lua -p "$PORT" 2>&1); then
    fail "a malformed port refuses the app at load" "it started"
else
    printf '%s' "$out" | grep -q '"127.0.0.1:0" in hosts is not a host pattern' \
        && pass "a malformed port refuses the app at load, naming the entry" \
        || fail "a malformed port refuses the app at load" "$out"
fi

echo ""
echo "e2e_host_port: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
