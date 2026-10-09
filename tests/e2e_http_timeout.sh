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
# The deadline covers the whole redirect chain (audit 11). /hop/N/D answers
# after D ms with a redirect to /hop/N-1/D, and /hop/0/D with 200. Keel starts
# a fresh timer on every hop, so a chain of hops each well inside the timeout
# used to succeed however long it ran in total:
#   - /hop/4/700 (5 requests x 700 ms, each under the 1000 ms timeout) fails,
#     sync, async in a handler (attached), and async from app.main (detached:
#     no request to suspend, so Hull arms its own deadline timer)
#   - the same chain with a per-call 10000 ms succeeds (4 hops are allowed)
#   - at most 5 hops are followed: /hop/5/0 succeeds, /hop/6/0 fails
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
        if self.path.startswith("/hop/"):
            _, _, n, d = self.path.split("/")
            n, d = int(n), int(d)
            time.sleep(d / 1000.0)
            if n > 0:
                self.send_response(302)
                self.send_header("Location", "/hop/%d/%d" % (n - 1, d))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            body = b"chain-ok"
        else:
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
BASE="http://127.0.0.1:$SLOW_PORT"

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
local BASE = "$BASE"

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
local function chain_url(req) return BASE .. "/hop/" .. req.query.h end
app.get("/chain", function(req, res)
    report(res, pcall(http.async.get, chain_url(req), opts(req)))
end)
app.get("/chain_sync", function(req, res)
    report(res, pcall(http.get, chain_url(req), opts(req)))
end)

-- Detached: app.main runs before the serve loop, with no request.
local detached = {}
local function outcome(ok, r)
    if ok and r and r.status == 200 then return "ok=" .. r.body end
    return "error"
end
app.main(function(_ctx)
    detached.long = outcome(pcall(http.async.get, BASE .. "/hop/4/700"))
    detached.short = outcome(pcall(http.async.get, BASE .. "/hop/2/0",
                                   { timeout_ms = 10000 }))
    return 0
end)
app.get("/detached", function(req, res)
    res:text(detached[req.query.k] or "unset")
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
const BASE = "$BASE";

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
const chainUrl = (req) => BASE + "/hop/" + req.query.h;
app.get("/chain", async (req, res) => {
    let r;
    try { r = await http.async.get(chainUrl(req), opts(req)); } catch (e) { r = null; }
    report(res, r);
});
app.get("/chain_sync", (req, res) => {
    let r;
    try { r = http.get(chainUrl(req), opts(req)); } catch (e) { r = null; }
    report(res, r);
});

// Detached: app.main runs before the serve loop, with no request.
const detached = {};
async function outcome(p) {
    try {
        const r = await p;
        return r && r.status === 200 ? "ok=" + r.body : "error";
    } catch (e) { return "error"; }
}
app.main(async (_ctx) => {
    detached.long = await outcome(http.async.get(BASE + "/hop/4/700"));
    detached.short = await outcome(http.async.get(BASE + "/hop/2/0", { timeoutMs: 10000 }));
    return 0;
});
app.get("/detached", (req, res) => res.text(detached[req.query.k] || "unset"));
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

    # The deadline bounds the whole redirect chain, not each hop.
    for kind in chain chain_sync; do
        out=$(curl -s -m 15 "http://127.0.0.1:$PORT/$kind?h=4/700")
        [ "$out" = "error" ] \
            && pass "$rt $kind: 5 x 700 ms hops exceed the 1000 ms deadline" \
            || fail "$rt $kind: 5 x 700 ms hops exceed the 1000 ms deadline" "$out"

        out=$(curl -s -m 15 "http://127.0.0.1:$PORT/$kind?h=4/700&t=10000")
        [ "$out" = "ok=chain-ok" ] \
            && pass "$rt $kind: the same chain fits a 10000 ms deadline" \
            || fail "$rt $kind: the same chain fits a 10000 ms deadline" "$out"

        out=$(curl -s -m 15 "http://127.0.0.1:$PORT/$kind?h=5/0&t=10000")
        [ "$out" = "ok=chain-ok" ] \
            && pass "$rt $kind: 5 redirect hops are followed" \
            || fail "$rt $kind: 5 redirect hops are followed" "$out"

        out=$(curl -s -m 15 "http://127.0.0.1:$PORT/$kind?h=6/0&t=10000")
        [ "$out" = "error" ] \
            && pass "$rt $kind: a 6th redirect hop is refused" \
            || fail "$rt $kind: a 6th redirect hop is refused" "$out"
    done

    out=$(curl -s -m 10 "http://127.0.0.1:$PORT/detached?k=long")
    [ "$out" = "error" ] \
        && pass "$rt detached (app.main): the chain is bounded by the deadline" \
        || fail "$rt detached (app.main): the chain is bounded by the deadline" "$out"
    out=$(curl -s -m 10 "http://127.0.0.1:$PORT/detached?k=short")
    [ "$out" = "ok=chain-ok" ] \
        && pass "$rt detached (app.main): a chain inside the deadline succeeds" \
        || fail "$rt detached (app.main): a chain inside the deadline succeeds" "$out"

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

run_app lua "$TMPDIR/lua/app.lua"
run_app js  "$TMPDIR/js/app.js"

echo ""
echo "e2e_http_timeout: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
