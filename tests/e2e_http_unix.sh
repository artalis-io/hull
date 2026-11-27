#!/bin/sh
# e2e_http_unix.sh: http.fetch and http.get over a declared Unix-domain
# socket (#1228). An app.main app, sandboxed, declares manifest
# http.unix_sockets and names each socket in an http+unix:// URL by the
# entry's own spelling:
#   - an app-relative socket and an "@state/" one answer, through http.fetch
#     and the synchronous http.get, with the request path intact;
#   - a socket the manifest does not declare is refused, coded `denied`;
#   - a declared entry whose path is a symbolic link is refused, coded
#     `denied`;
#   - a redirect off the socket (to a TCP URL) is not followed;
#   - an entry with ".." refuses the app at load.
# Both runtimes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: e2e_http_unix needs python3 for the socket server"
    exit 0
fi

HULL=$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")
# Short: a socket path is at most 103 bytes (macOS), and the state directory
# below is HULL_STATE_DIR itself.
T=$(mktemp -d /tmp/hux.XXXXXX)
T=$(cd "$T" && pwd -P)
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    [ -n "$KEEP" ] && echo "kept $T" || rm -rf "$T"
}
trap cleanup EXIT

# One server thread per socket path. GET /ping answers "uds:<path>"; GET
# /redir redirects to a TCP URL, which the client must not follow.
cat > "$T/uds.py" <<'EOF2'
import http.server, os, socketserver, sys, threading
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/redir":
            self.send_response(302)
            self.send_header("Location", "http://127.0.0.1:9/away")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        b = ("uds:" + self.path).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def log_message(self, *a):
        pass
class S(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    def get_request(self):
        r, _ = super().get_request()
        return r, ("local", 0)
for p in sys.argv[1:]:
    try:
        os.unlink(p)
    except FileNotFoundError:
        pass
    threading.Thread(target=S(p, H).serve_forever, daemon=True).start()
open(sys.argv[1] + ".up", "w").close()
threading.Event().wait()
EOF2

for rt in lua js; do mkdir -p "$T/$rt/run"; done
STATE="$T/state"
mkdir -m 700 "$STATE"

python3 "$T/uds.py" "$T/lua/run/svc.sock" "$T/js/run/svc.sock" \
    "$STATE/st.sock" "$T/lua/run/other.sock" "$T/js/run/other.sock" &
SERVER_PID=$!
i=0
while [ ! -e "$T/lua/run/svc.sock.up" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
for rt in lua js; do ln -s svc.sock "$T/$rt/run/link.sock"; done

cat > "$T/lua/app.lua" <<'EOF2'
local app = require("hull.app")
app.manifest({
    modules = { "hull/http-client@1", "hull/log@1" },
    state = { name = "hux" },
    http = { unix_sockets = { "run/svc.sock", "@state/st.sock", "run/link.sock" } },
})
local http = require("hull.http-client")
local log = require("hull.log")
local function show(tag, ok, r, e2)
    if ok and r == nil then   -- a failed request returns nil, err
        log.info("R " .. tag .. " err " .. (tostring(e2):match("^%[hull:([%w_]+)%] ") or "none"))
    elseif ok then
        log.info("R " .. tag .. " ok " .. tostring(r.status) .. " " .. tostring(r.body))
    else
        local m = tostring(r)
        log.info("R " .. tag .. " err " .. (m:match("^%[hull:([%w_]+)%] ") or "none"))
    end
end
app.main(function(ctx)
    show("fetch", pcall(http.async.get, "http+unix://run%2Fsvc.sock/v1/ping"))
    show("get", pcall(http.get, "http+unix://run%2Fsvc.sock/v1/ping"))
    show("state", pcall(http.async.get, "http+unix://%40state%2Fst.sock/s"))
    show("undeclared", pcall(http.async.get, "http+unix://run%2Fother.sock/x"))
    show("link", pcall(http.get, "http+unix://run%2Flink.sock/x"))
    show("redir", pcall(http.async.get, "http+unix://run%2Fsvc.sock/redir"))
    return 0
end)
EOF2

cat > "$T/js/app.js" <<'EOF2'
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
import { log } from "hull:log";
app.manifest({
    modules: ["hull/http-client@1", "hull/log@1"],
    state: { name: "hux" },
    http: { unixSockets: ["run/svc.sock", "@state/st.sock", "run/link.sock"] },
});
const show = (tag, r, e) => e
    ? log.info(`R ${tag} err ${(e && e.code) || "none"}`)
    : log.info(`R ${tag} ok ${r.status} ${r.body}`);
const tryAsync = async (tag, url) => {
    try { show(tag, await httpClient.async.get(url)); } catch (e) { show(tag, null, e); }
};
const trySync = (tag, url) => {
    try { show(tag, httpClient.get(url)); } catch (e) { show(tag, null, e); }
};
app.main(async () => {
    await tryAsync("fetch", "http+unix://run%2Fsvc.sock/v1/ping");
    trySync("get", "http+unix://run%2Fsvc.sock/v1/ping");
    await tryAsync("state", "http+unix://%40state%2Fst.sock/s");
    await tryAsync("undeclared", "http+unix://run%2Fother.sock/x");
    trySync("link", "http+unix://run%2Flink.sock/x");
    await tryAsync("redir", "http+unix://run%2Fsvc.sock/redir");
    return 0;
});
EOF2

line() { grep "R $2 " "$1" | head -1 | sed 's/.*R [a-z]* //'; }

for rt in lua js; do
    echo "-- $rt --"
    log="$T/$rt.log"
    ( cd "$T/$rt" && HULL_STATE_DIR="$STATE" "$HULL" "app.$rt" > "$log" 2>&1 ) || true
    got=$(line "$log" fetch)
    [ "$got" = "ok 200 uds:/v1/ping" ] && pass "$rt: http.fetch reaches the app-relative socket" \
        || fail "$rt: http.fetch reaches the app-relative socket" "$got"
    got=$(line "$log" get)
    [ "$got" = "ok 200 uds:/v1/ping" ] && pass "$rt: http.get reaches it too" \
        || fail "$rt: http.get reaches it too" "$got"
    got=$(line "$log" state)
    [ "$got" = "ok 200 uds:/s" ] && pass "$rt: an @state/ socket answers" \
        || fail "$rt: an @state/ socket answers" "$got"
    got=$(line "$log" undeclared)
    [ "$got" = "err denied" ] && pass "$rt: an undeclared socket is refused, coded denied" \
        || fail "$rt: an undeclared socket is refused, coded denied" "$got"
    got=$(line "$log" link)
    [ "$got" = "err denied" ] && pass "$rt: a symlinked socket path is refused, coded denied" \
        || fail "$rt: a symlinked socket path is refused, coded denied" "$got"
    got=$(line "$log" redir)
    case "$got" in
        "ok 302 "*|err*) pass "$rt: a redirect off the socket is not followed ($got)" ;;
        *) fail "$rt: a redirect off the socket is not followed" "$got" ;;
    esac
done

echo "-- load-time refusal --"
mkdir -p "$T/bad"
cat > "$T/bad/app.lua" <<'EOF2'
local app = require("hull.app")
app.manifest({ modules = { "hull/http-client@1" },
               http = { unix_sockets = { "../x.sock" } } })
app.main(function() return 0 end)
EOF2
rc=0
( cd "$T/bad" && "$HULL" app.lua > "$T/bad.log" 2>&1 ) || rc=$?
if [ "$rc" -ne 0 ] && grep -q 'in http.unix_sockets is not a socket path' "$T/bad.log"; then
    pass "an entry with '..' refuses the app at load"
else
    fail "an entry with '..' refuses the app at load" "rc=$rc $(tail -2 "$T/bad.log" | tr '\n' ' ')"
fi

echo "-- the list is taken whole or refused --"
# A JS app may use the Lua spelling (exec's host takes both); a non-string
# entry, more than 16 entries or both spellings refuse the app at load
# instead of being dropped (audit).
mkdir -p "$T/snake/run"
cat > "$T/snake/app.js" <<EOF2
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
import { log } from "hull:log";
app.manifest({ modules: ["hull/http-client@1", "hull/log@1"],
               http: { unix_sockets: ["$T/js/run/svc.sock"] } });
app.main(async () => {
    const r = await httpClient.async.get("http+unix://" + encodeURIComponent("$T/js/run/svc.sock") + "/p");
    log.info("R snake ok " + r.status + " " + r.body);
    return 0;
});
EOF2
( cd "$T/snake" && "$HULL" app.js > "$T/snake.log" 2>&1 ) || true
got=$(line "$T/snake.log" snake)
[ "$got" = "ok 200 uds:/p" ] && pass "js: http.unix_sockets (the Lua spelling) is taken" \
    || fail "js: http.unix_sockets (the Lua spelling) is taken" "$got $(tail -2 "$T/snake.log" | tr '\n' ' ')"
refused() {  # refused <label> <dir> <entry file> <body>
    mkdir -p "$T/$2"
    printf '%s\n' "$4" > "$T/$2/$3"
    rc=0
    ( cd "$T/$2" && "$HULL" "$3" > "$T/$2.log" 2>&1 ) || rc=$?
    if [ "$rc" -ne 0 ] && grep -q 'must be a list of at most 16 strings' "$T/$2.log"; then pass "$1"
    else fail "$1" "rc=$rc $(tail -2 "$T/$2.log" | tr '\n' ' ')"; fi
}
SEVENTEEN='"a.sock","b.sock","c.sock","d.sock","e.sock","f.sock","g.sock","h.sock","i.sock","j.sock","k.sock","l.sock","m.sock","n.sock","o.sock","p.sock","q.sock"'
refused "lua: a non-string entry refuses the app" lnum app.lua \
    'local app = require("hull.app") app.manifest({ modules = { "hull/http-client@1" }, http = { unix_sockets = { "a.sock", 123 } } }) app.main(function() return 0 end)'
refused "lua: a 17th entry refuses the app" l17 app.lua \
    "local app = require(\"hull.app\") app.manifest({ modules = { \"hull/http-client@1\" }, http = { unix_sockets = { $SEVENTEEN } } }) app.main(function() return 0 end)"
refused "js: a non-string entry refuses the app" jnum app.js \
    'import { app } from "hull:app"; app.manifest({ modules: ["hull/http-client@1"], http: { unixSockets: ["a.sock", 123] } }); app.main(() => 0);'
refused "js: a 17th entry refuses the app" j17 app.js \
    "import { app } from \"hull:app\"; app.manifest({ modules: [\"hull/http-client@1\"], http: { unixSockets: [$SEVENTEEN] } }); app.main(() => 0);"
refused "js: both spellings refuse the app" jboth app.js \
    'import { app } from "hull:app"; app.manifest({ modules: ["hull/http-client@1"], http: { unixSockets: ["a.sock"], unix_sockets: ["a.sock"] } }); app.main(() => 0);'

echo "-- hull inspect --"
# The sockets are outbound authority: hull inspect lists them, and marks an
# absolute one (outside the app and state directories). Both spellings.
for sp in unix_sockets unixSockets; do
    mkdir -p "$T/insp_$sp"
    printf '{"manifest":{"modules":["hull/http-client@1"],"http":{"%s":["run/svc.sock","/var/run/docker.sock"]}}}\n' \
        "$sp" > "$T/insp_$sp/package.sig"
    out=$("$HULL" inspect "$T/insp_$sp" 2>&1) || true
    if printf '%s\n' "$out" | grep -qF "http.unix_socket: run/svc.sock" &&
       printf '%s\n' "$out" | grep -qF "[!] http.unix_socket: /var/run/docker.sock"; then
        pass "hull inspect lists http.$sp and marks an absolute socket"
    else
        fail "hull inspect lists http.$sp and marks an absolute socket" "$out"
    fi
done

echo ""
echo "e2e_http_unix: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
