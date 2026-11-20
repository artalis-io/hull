#!/bin/sh
# e2e_host_denied.sh: a host manifest.hosts does not admit is refused with a
# code, not the message a network failure gets (platform gap 3).
#
# http.fetch, a synchronous http.get and ws.connect to a refused host used to
# fail with "http.fetch: failed to start request" / "http.get failed: ..." /
# "host not in allowlist", the first two the same text a connection refused
# or a DNS failure gives, so an app could not tell "the manifest said no" from
# "the network said no". Each now raises (Lua) "[hull:denied] <call>: host
# '<host>' is not in manifest.hosts" / throws (JS) an Error with an own
# `code` "denied" and an own `host`. When an allowlist entry is a "$VAR" whose
# variable is unset, the message names the entry. A network failure to an
# admitted host is never coded denied. Both runtimes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PORT="${PORT:-19916}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: e2e_host_denied needs python3 to read JSON"
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
cat > "$TMPDIR/lua/app.lua" <<'EOF'
local app = require("hull.app")
app.manifest({
    modules = { "hull/http-server@1", "hull/http-client@1", "hull/web/ws-client@1" },
    hosts = { "127.0.0.1", "$GAP3_HOST" },
    env = { "GAP3_HOST" },
})
local http = require("hull.http-client")
local ws = require("hull.web.ws-client")
local function report(res, ok, e)
    if ok then return res:json({ code = "ok" }) end
    local msg = tostring(e)
    res:json({ code = msg:match("^%[hull:([%w_]+)%] ") or "none", msg = msg })
end
app.get("/fetch", function(req, res)
    report(res, pcall(http.async.get, "http://denied.example/x"))
end)
app.get("/get", function(req, res)
    report(res, pcall(http.get, "http://denied.example/x"))
end)
app.get("/ws", function(req, res)
    report(res, pcall(ws.connect, "ws://denied.example/", {}))
end)
app.get("/net", function(req, res)
    report(res, pcall(http.async.get, "http://127.0.0.1:1/x"))
end)
EOF

cat > "$TMPDIR/js/app.js" <<'EOF'
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
import { wsClient } from "hull:web:ws-client";
app.manifest({
    modules: ["hull/http-server@1", "hull/http-client@1", "hull/web/ws-client@1"],
    hosts: ["127.0.0.1", "$GAP3_HOST"],
    env: ["GAP3_HOST"],
});
const report = (res, e) => res.json({ code: (e && e.code) || "none",
                                      host: (e && e.host) || null,
                                      msg: String(e && e.message) });
app.get("/fetch", async (req, res) => {
    try { await httpClient.async.get("http://denied.example/x"); res.json({ code: "ok" }); }
    catch (e) { report(res, e); }
});
app.get("/get", (req, res) => {
    try { httpClient.get("http://denied.example/x"); res.json({ code: "ok" }); }
    catch (e) { report(res, e); }
});
app.get("/ws", (req, res) => {
    try { wsClient.connect("ws://denied.example/", {}); res.json({ code: "ok" }); }
    catch (e) { report(res, e); }
});
app.get("/net", async (req, res) => {
    try { await httpClient.async.get("http://127.0.0.1:1/x"); res.json({ code: "ok" }); }
    catch (e) { report(res, e); }
});
EOF

jget() {
    printf '%s' "$1" | python3 -c "import sys, json
try: print(json.load(sys.stdin).get(sys.argv[1]))
except Exception: print('?')" "$2"
}

wait_up() {
    i=0
    while [ $i -lt 50 ]; do
        curl -s -o /dev/null "http://127.0.0.1:$PORT/net" && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

run_runtime() {
    rt=$1; entry=$2
    echo "-- $rt --"
    ( cd "$TMPDIR/$rt" && unset GAP3_HOST && exec "$HULL" "$entry" -p "$PORT" \
        > "$TMPDIR/$rt.log" 2>&1 ) &
    SERVER_PID=$!
    if ! wait_up; then
        fail "$rt: server did not start" "$(tail -5 "$TMPDIR/$rt.log")"
        return
    fi
    for route in fetch get ws; do
        out=$(curl -sS "http://127.0.0.1:$PORT/$route")
        msg=$(jget "$out" msg)
        if [ "$(jget "$out" code)" = "denied" ] && \
           printf '%s' "$msg" | grep -q "host 'denied.example' is not in manifest.hosts" && \
           printf '%s' "$msg" | grep -q 'entry \$GAP3_HOST names an unset variable'; then
            pass "$rt: $route to a refused host raises coded denied, naming the host and the unset entry"
        else
            fail "$rt: $route to a refused host" "$out"
        fi
        if [ "$rt" = js ]; then
            [ "$(jget "$out" host)" = "denied.example" ] \
                && pass "$rt: $route's error carries an own host" \
                || fail "$rt: $route's error host" "$out"
        fi
    done
    out=$(curl -sS "http://127.0.0.1:$PORT/net")
    c=$(jget "$out" code)
    [ "$c" != "denied" ] && [ "$c" != "?" ] \
        && pass "$rt: a fetch to an admitted host that refuses the connection is not coded denied" \
        || fail "$rt: network failure" "$out"
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

run_runtime lua app.lua
run_runtime js app.js

echo ""
echo "e2e_host_denied: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
