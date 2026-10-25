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
# ws.connect has the same deadline over its connect + upgrade (audit 25):
# Keel has none, so a host that accepted the TCP connection and never
# answered the upgrade held the client and its fd until close. A black-hole
# listener accepts and never writes; the client's on_error fires with
# "connect timeout" after the manifest's 1000 ms, not before.
#
# Audit 26: after conn:close() Keel waits in CLOSING for the peer's Close
# with no timer. A mute listener completes the upgrade and then never
# answers; on_close fires with 1006 after HL_WS_CLIENT_CLOSE_TIMEOUT_MS (5 s).
#
# Audit 26: a request carrying a non-safe header (X-Api-Key) is refused a
# redirect to another allowed origin (127.0.0.1 -> localhost); without the
# header the same redirect is followed.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PORT="${PORT:-19894}"
SLOW_PORT="${SLOW_PORT:-19895}"
HOLE_PORT="${HOLE_PORT:-19896}"
MUTE_PORT="${MUTE_PORT:-19897}"
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
HOLE_PID=""
MUTE_PID=""
cleanup() {
    [ -n "$MUTE_PID" ] && kill "$MUTE_PID" 2>/dev/null || true
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    [ -n "$SLOW_PID" ] && kill "$SLOW_PID" 2>/dev/null || true
    [ -n "$HOLE_PID" ] && kill "$HOLE_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# ── The slow upstream ────────────────────────────────────────────────
cat > "$TMPDIR/slow.py" <<'EOF'
import sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/xo":
            self.send_response(302)
            self.send_header("Location",
                             "http://localhost:%d/fast" % self.server.server_port)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
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

# ── The black hole: accepts, reads nothing back, never answers ──────
cat > "$TMPDIR/hole.py" <<'EOF'
import socket, sys
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(16)
held = []
while True:
    c, _ = s.accept()
    held.append(c)
EOF
python3 -I "$TMPDIR/hole.py" "$HOLE_PORT" >"$TMPDIR/hole.log" 2>&1 &
HOLE_PID=$!
WS_HOLE="ws://127.0.0.1:$HOLE_PORT/ws"

# ── The mute peer: completes the upgrade, then never answers a Close ─
cat > "$TMPDIR/mute.py" <<'EOF'
import base64, hashlib, socket, sys, threading
G = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(16)
held = []
def serve(c):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = c.recv(4096)
        if not chunk:
            return
        data += chunk
    key = ""
    for line in data.decode("latin-1").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    acc = base64.b64encode(hashlib.sha1((key + G).encode()).digest()).decode()
    c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"
               % acc).encode())
    while c.recv(4096):
        pass                      # read and drop everything, answer nothing
while True:
    c, _ = s.accept()
    held.append(c)
    threading.Thread(target=serve, args=(c,), daemon=True).start()
EOF
python3 -I "$TMPDIR/mute.py" "$MUTE_PORT" >"$TMPDIR/mute.log" 2>&1 &
MUTE_PID=$!
WS_MUTE="ws://127.0.0.1:$MUTE_PORT/ws"

SLOW="http://127.0.0.1:$SLOW_PORT/slow"
BASE="http://127.0.0.1:$SLOW_PORT"

# ── Apps ─────────────────────────────────────────────────────────────
mkdir -p "$TMPDIR/lua" "$TMPDIR/js"
cat > "$TMPDIR/lua/app.lua" <<EOF
app.manifest({
    modules = { "hull/http-server@1", "hull/http-client@1",
                "hull/web/ws-client@1" },
    hosts = { "127.0.0.1", "localhost" },
    http = { timeout_ms = 1000 },
})
local http = require("hull.http-client")
local wsc = require("hull.web.ws-client")
local ws_state = "unset"
app.get("/wsgo", function(_req, res)
    ws_state = "pending"
    wsc.connect("$WS_HOLE", {
        on_open = function(_c) ws_state = "open" end,
        on_error = function(_c, msg) ws_state = "error:" .. tostring(msg) end,
    })
    res:text("started")
end)
app.get("/wsstate", function(_req, res) res:text(ws_state) end)
local mute_state, mute_conn = "unset", nil
app.get("/mutego", function(_req, res)
    mute_state = "pending"
    mute_conn = wsc.connect("$WS_MUTE", {
        on_open = function(_c) mute_state = "open" end,
        on_close = function(_c, code) mute_state = "closed:" .. tostring(code) end,
        on_error = function(_c, msg) mute_state = "error:" .. tostring(msg) end,
    })
    res:text("started")
end)
app.get("/muteclose", function(_req, res)
    mute_conn:close()
    mute_state = "closing"
    res:text("closing")
end)
app.get("/mutestate", function(_req, res) res:text(mute_state) end)
local URL = "$SLOW"
local BASE = "$BASE"

local function report(res, ok, r)
    if ok and r and r.status == 200 then return res:text("ok=" .. r.body) end
    res:text("error")
end
app.get("/xo", function(req, res)
    local h = req.query.key and { ["X-Api-Key"] = "s3cret" } or nil
    local fn = req.query.sync and http.get or http.async.get
    report(res, pcall(fn, BASE .. "/xo", { headers = h }))
end)
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
import { wsClient } from "hull:web:ws-client";

app.manifest({
    modules: ["hull/http-server@1", "hull/http-client@1",
              "hull/web/ws-client@1"],
    hosts: ["127.0.0.1", "localhost"],
    http: { timeoutMs: 1000 },
});
let wsState = "unset";
app.get("/wsgo", (_req, res) => {
    wsState = "pending";
    wsClient.connect("$WS_HOLE", {
        onOpen(_c) { wsState = "open"; },
        onError(_c, msg) { wsState = "error:" + msg; },
    });
    res.text("started");
});
app.get("/wsstate", (_req, res) => res.text(wsState));
let muteState = "unset", muteConn = null;
app.get("/mutego", (_req, res) => {
    muteState = "pending";
    muteConn = wsClient.connect("$WS_MUTE", {
        onOpen(_c) { muteState = "open"; },
        onClose(_c, code) { muteState = "closed:" + code; },
        onError(_c, msg) { muteState = "error:" + msg; },
    });
    res.text("started");
});
app.get("/muteclose", (_req, res) => {
    muteConn.close();
    muteState = "closing";
    res.text("closing");
});
app.get("/mutestate", (_req, res) => res.text(muteState));
let earlyState = "unset";
app.get("/earlygo", (_req, res) => {
    earlyState = "pending";
    const c = wsClient.connect("$WS_MUTE", {
        onOpen(_c) { earlyState = "open"; },
        onClose(_c, code) { earlyState += "|closed:" + code; },
        onError(_c, msg) { earlyState += "|error:" + msg; },
    });
    c.close();
    earlyState = "closing";
    res.text("closing");
});
app.get("/earlystate", (_req, res) => res.text(earlyState));
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
app.get("/xo", async (req, res) => {
    const o = req.query.key ? { headers: { "X-Api-Key": "s3cret" } } : {};
    let r;
    try {
        r = req.query.sync ? http.get(BASE + "/xo", o)
                           : await http.async.get(BASE + "/xo", o);
    } catch (e) { r = null; }
    report(res, r);
});
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

    # ws.connect to a host that never answers the upgrade (audit 25).
    curl -s -m 10 -o /dev/null "http://127.0.0.1:$PORT/wsgo"
    sleep 0.3
    out=$(curl -s -m 10 "http://127.0.0.1:$PORT/wsstate")
    [ "$out" = "pending" ] \
        && pass "$rt ws.connect: still connecting inside the deadline" \
        || fail "$rt ws.connect: still connecting inside the deadline" "$out"
    sleep 1.5
    out=$(curl -s -m 10 "http://127.0.0.1:$PORT/wsstate")
    [ "$out" = "error:connect timeout" ] \
        && pass "$rt ws.connect: a silent upgrade fails at the deadline" \
        || fail "$rt ws.connect: a silent upgrade fails at the deadline" "$out"

    # Audit 26: a credential-like header may not follow a cross-origin hop.
    for q in "" "&sync=1"; do
        out=$(curl -s -m 10 "http://127.0.0.1:$PORT/xo?x=1$q")
        [ "$out" = "ok=slow-ok" ] \
            && pass "$rt xo$q: a plain request follows a cross-origin redirect" \
            || fail "$rt xo$q: a plain request follows a cross-origin redirect" "$out"
        out=$(curl -s -m 10 "http://127.0.0.1:$PORT/xo?key=1$q")
        [ "$out" = "error" ] \
            && pass "$rt xo$q: an X-Api-Key request is refused the cross-origin hop" \
            || fail "$rt xo$q: an X-Api-Key request is refused the cross-origin hop" "$out"
    done

    # Audit 26: conn:close() to a peer that never answers ends at a deadline.
    curl -s -m 10 -o /dev/null "http://127.0.0.1:$PORT/mutego"
    sleep 0.5
    out=$(curl -s -m 10 "http://127.0.0.1:$PORT/mutestate")
    [ "$out" = "open" ] \
        && pass "$rt ws close: the mute peer accepts the upgrade" \
        || fail "$rt ws close: the mute peer accepts the upgrade" "$out"
    curl -s -m 10 -o /dev/null "http://127.0.0.1:$PORT/muteclose"
    sleep 2
    out=$(curl -s -m 10 "http://127.0.0.1:$PORT/mutestate")
    [ "$out" = "closing" ] \
        && pass "$rt ws close: still closing inside the deadline" \
        || fail "$rt ws close: still closing inside the deadline" "$out"
    sleep 4
    out=$(curl -s -m 10 "http://127.0.0.1:$PORT/mutestate")
    [ "$out" = "closed:1006" ] \
        && pass "$rt ws close: an unanswered close ends at the deadline (1006)" \
        || fail "$rt ws close: an unanswered close ends at the deadline (1006)" "$out"

    # Audit 27: close() before the upgrade completed was dropped by Keel and
    # the connection then stayed open. It is sent once the upgrade is done,
    # onOpen is not called, and onClose fires once at the close deadline.
    if [ "$rt" = js ]; then
        curl -s -m 10 -o /dev/null "http://127.0.0.1:$PORT/earlygo"
        sleep 2
        out=$(curl -s -m 10 "http://127.0.0.1:$PORT/earlystate")
        [ "$out" = "closing" ] \
            && pass "$rt ws early close: no onOpen, still closing" \
            || fail "$rt ws early close: no onOpen, still closing" "$out"
        sleep 4.5
        out=$(curl -s -m 10 "http://127.0.0.1:$PORT/earlystate")
        [ "$out" = "closing|closed:1006" ] \
            && pass "$rt ws early close: onClose once at the deadline (1006)" \
            || fail "$rt ws early close: onClose once at the deadline (1006)" "$out"
    fi

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
