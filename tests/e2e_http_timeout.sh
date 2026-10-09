#!/bin/sh
# e2e_http_timeout.sh: the outbound HTTP timeout is configurable.
#
# The timeout is ONE deadline for the whole request (DNS, connect, TLS, send,
# receive, redirects). Its default is 30 s; the manifest's
# `http = { timeout_ms = N }` (JS `http: { timeoutMs: N }`) sets the app's
# default, and a per-call `timeout_ms` / `timeoutMs` option overrides it.
#
# A local server answers /slow after ~2 s. Each app (Lua and JS) declares a
# 1000 ms manifest default and asks, for both the async http.async.get and the
# sync http.get:
#   - no option          -> the manifest's 1000 ms applies: fails
#   - a per-call 1000 ms -> fails
#   - a per-call 5000 ms -> overrides the manifest's 1000 ms: succeeds
# Under the 30 s default every one of these would succeed, so a failure can
# only come from the configured deadline.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PORT="${PORT:-19894}"
SLOW_PORT="${SLOW_PORT:-19895}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: e2e_http_timeout needs python3 for the slow server"
    exit 0
fi

TMPDIR=$(mktemp -d)
SERVER_PID=""
SLOW_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    [ -n "$SLOW_PID" ] && kill "$SLOW_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# ── The slow upstream ────────────────────────────────────────────────
cat > "$TMPDIR/slow.py" <<'EOF'
import sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/slow":
            time.sleep(2)
        body = b"slow-ok"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
    def log_message(self, *a):
        pass

ThreadingHTTPServer.daemon_threads = True
ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
EOF
python3 -I "$TMPDIR/slow.py" "$SLOW_PORT" >"$TMPDIR/slow.log" 2>&1 &
SLOW_PID=$!
up=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if curl -s -o /dev/null "http://127.0.0.1:$SLOW_PORT/fast"; then up=1; break; fi
    sleep 0.25
done
[ "$up" = 1 ] || { echo "FAIL: slow server did not start"; cat "$TMPDIR/slow.log"; exit 1; }

SLOW="http://127.0.0.1:$SLOW_PORT/slow"

# ── Apps ─────────────────────────────────────────────────────────────
mkdir -p "$TMPDIR/lua" "$TMPDIR/js"
cat > "$TMPDIR/lua/app.lua" <<EOF
app.manifest({
    modules = { "hull/http-server@1", "hull/http-client@1" },
    hosts = { "127.0.0.1" },
    http = { timeout_ms = 1000 },
})
local http = require("hull.http-client")
local URL = "$SLOW"

local function report(res, ok, r)
    if ok and r and r.status == 200 then return res:text("ok=" .. r.body) end
    res:text("error")
end
local function opts(req)
    local t = tonumber(req.query and req.query.t)
    return t and { timeout_ms = t } or nil
end
app.get("/up", function(_req, res) res:text("up") end)
app.get("/fetch", function(req, res)
    report(res, pcall(http.async.get, URL, opts(req)))
end)
app.get("/sync", function(req, res)
    report(res, pcall(http.get, URL, opts(req)))
end)
EOF

cat > "$TMPDIR/js/app.js" <<EOF
import { app } from "hull:app";
import { httpClient as http } from "hull:http-client";

app.manifest({
    modules: ["hull/http-server@1", "hull/http-client@1"],
    hosts: ["127.0.0.1"],
    http: { timeoutMs: 1000 },
});
const URL = "$SLOW";

function report(res, r) {
    if (r && r.status === 200) res.text("ok=" + r.body);
    else res.text("error");
}
function opts(req) {
    const t = req.query && req.query.t;
    return t ? { timeoutMs: Number(t) } : undefined;
}
app.get("/up", (_req, res) => res.text("up"));
app.get("/fetch", async (req, res) => {
    let r;
    try { r = await http.async.get(URL, opts(req)); } catch (e) { r = null; }
    report(res, r);
});
app.get("/sync", (req, res) => {
    let r;
    try { r = http.get(URL, opts(req)); } catch (e) { r = null; }
    report(res, r);
});
EOF

run_app() {
    rt="$1"; entry="$2"
    echo "=== $rt ==="
    "$HULL" --no-sandbox -p "$PORT" "$entry" >"$TMPDIR/$rt.log" 2>&1 &
    SERVER_PID=$!
    up=0
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if curl -s -o /dev/null "http://127.0.0.1:$PORT/up"; then up=1; break; fi
        sleep 0.5
    done
    if [ "$up" != 1 ]; then
        fail "$rt: server starts" "$(tail -5 "$TMPDIR/$rt.log")"
        kill "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""
        return
    fi

    for kind in fetch sync; do
        out=$(curl -s -m 10 "http://127.0.0.1:$PORT/$kind")
        [ "$out" = "error" ] \
            && pass "$rt $kind: the manifest default (1000 ms) applies" \
            || fail "$rt $kind: the manifest default (1000 ms) applies" "$out"

        out=$(curl -s -m 10 "http://127.0.0.1:$PORT/$kind?t=1000")
        [ "$out" = "error" ] \
            && pass "$rt $kind: a per-call 1000 ms times out" \
            || fail "$rt $kind: a per-call 1000 ms times out" "$out"

        out=$(curl -s -m 10 "http://127.0.0.1:$PORT/$kind?t=5000")
        [ "$out" = "ok=slow-ok" ] \
            && pass "$rt $kind: a per-call 5000 ms overrides the manifest and succeeds" \
            || fail "$rt $kind: a per-call 5000 ms overrides the manifest and succeeds" "$out"
    done

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

run_app lua "$TMPDIR/lua/app.lua"
run_app js  "$TMPDIR/js/app.js"

echo ""
echo "e2e_http_timeout: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
