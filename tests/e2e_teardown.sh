#!/bin/sh
# e2e_teardown.sh: shutting down with work in flight exits cleanly.
#
# Audit 14 G1 / audit 15. The serve teardown freed the runtime before Keel's
# kl_http_server_free cancelled the ops still suspended on connections and
# closed the connections still open, so:
#   - a handler parked on hull.sleep past the drain deadline was cancelled
#     into a freed VM;
#   - a WebSocket still open ran its on_close against a freed route and VM
#     (remote-forceable at every restart, always with --drain-timeout 0);
#   - a detached http.fetch (from a timer, a WebSocket callback, app.main)
#     leaked its continuation, and QuickJS aborted in JS_FreeRuntime;
#   - an unawaited worker.dispatch / db.async left by app.main did the same;
#   - and an HTTPS server freed its TLS context twice (Hull, then Keel).
# Each run below must exit 0 - not 134 (abort) or 139 (segfault) - and log
# no sanitizer / assertion report.
#
# Requires build/hull and curl. The WebSocket legs need python3, the HTTPS
# legs openssl; each is skipped without it.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

HULL="${HULL:-build/hull}"
FIX="$(cd "$(dirname "$0")" && pwd)/fixtures/teardown"
PORT_BASE="${PORT_BASE:-19940}"
PASS=0
FAIL=0
SKIP=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "  SKIP: $1"; }

. "$(dirname "$0")/lib/hull_proc.sh"
hull_proc_init "$HULL"
if ! hull_proc_graceful; then
    echo "SKIP: $(hull_proc_graceful_note)"
    exit 0
fi

WORK=$(mktemp -d)
SERVER_PID=""
HELPERS=""
cleanup() {
    for p in $HELPERS; do kill "$p" 2>/dev/null || true; done
    [ -n "$SERVER_PID" ] && kill -9 "$SERVER_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

HAVE_PY=0
command -v python3 >/dev/null 2>&1 && HAVE_PY=1

# A WebSocket client that upgrades, then holds the connection and never
# answers the server's close frame: the drain cannot close it.
cat > "$WORK/ws_hold.py" <<'PY'
import base64, os, socket, ssl, sys, time
port, tls = int(sys.argv[1]), sys.argv[2] == "tls"
s = socket.create_connection(("127.0.0.1", port))
if tls:
    c = ssl.create_default_context()
    c.check_hostname = False
    c.verify_mode = ssl.CERT_NONE
    s = c.wrap_socket(s, server_hostname="localhost")
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\nSec-WebSocket-Key: " + key + "\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n").encode())
resp = s.recv(4096)
print("UPGRADED" if b" 101 " in resp else "REFUSED", flush=True)
time.sleep(60)
PY

# Self-signed certificate for the HTTPS legs.
HAVE_TLS=0
if command -v openssl >/dev/null 2>&1; then
    if MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 -nodes \
         -keyout "$WORK/key.pem" -out "$WORK/cert.pem" -days 1 \
         -subj "/CN=localhost" </dev/null >/dev/null 2>&1 \
       && [ -s "$WORK/cert.pem" ] && [ -s "$WORK/key.pem" ]; then
        HAVE_TLS=1
    fi
fi

# start_server NAME PORT SCHEME APP [extra args] -> SERVER_PID, waits for /ping
start_server() {
    _name=$1; _port=$2; _scheme=$3; _app=$4; shift 4
    "$HULL" --no-sandbox -p "$_port" "$@" "$_app" >"$WORK/$_name.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 40); do
        if curl -sk -o /dev/null -m 2 "$_scheme://127.0.0.1:$_port/ping"; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

# stop_server -> STOP_RC: SIGTERM, then the real exit status (bounded).
stop_server() {
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    ( sleep 20; kill -9 "$SERVER_PID" 2>/dev/null ) >/dev/null 2>&1 &
    _wd=$!
    STOP_RC=0
    wait "$SERVER_PID" 2>/dev/null || STOP_RC=$?
    kill "$_wd" 2>/dev/null || true
    wait "$_wd" 2>/dev/null || true
    SERVER_PID=""
}

clean_log() {
    ! grep -qiE "AddressSanitizer|LeakSanitizer|runtime error:|Assertion|double free|Segmentation" "$1"
}

# load_server NAME PORT SCHEME: a parked /slow, a timer's detached fetch, a
# held WebSocket.
load_server() {
    _name=$1; _port=$2; _scheme=$3
    curl -sk -o /dev/null -m 30 "$_scheme://127.0.0.1:$_port/slow" >/dev/null 2>&1 &
    HELPERS="$HELPERS $!"
    if [ "$_scheme" = http ]; then
        curl -s -o /dev/null -m 2 "http://127.0.0.1:$_port/arm/$_port" || true
        for _ in $(seq 1 20); do
            [ "$(curl -s -m 2 "http://127.0.0.1:$_port/fired")" = yes ] && break
            sleep 0.1
        done
    fi
    WS_HELD=0
    if [ "$HAVE_PY" = 1 ]; then
        _mode=plain
        [ "$_scheme" = https ] && _mode=tls
        python3 "$WORK/ws_hold.py" "$_port" "$_mode" >"$WORK/$_name.ws" 2>&1 &
        HELPERS="$HELPERS $!"
        for _ in $(seq 1 20); do
            grep -q UPGRADED "$WORK/$_name.ws" 2>/dev/null && { WS_HELD=1; break; }
            sleep 0.1
        done
    fi
    sleep 0.3   # let the /slow handler park
}

port=$PORT_BASE
for rt in lua js; do
    for drain in 100 0; do
        port=$((port + 1))
        name="$rt-http-drain$drain"
        echo "=== $rt: SIGTERM, plain HTTP, --drain-timeout $drain ==="
        if ! start_server "$name" "$port" http "$FIX/server.$rt" --drain-timeout "$drain"; then
            fail "$name: server did not start"; cat "$WORK/$name.log"; continue
        fi
        load_server "$name" "$port" http
        [ "$WS_HELD" = 1 ] || skip "$name: no WebSocket held (python3 missing or upgrade refused)"
        stop_server
        if [ "$STOP_RC" = 0 ] && clean_log "$WORK/$name.log"; then
            pass "$name: exit 0 with a parked request, a detached fetch and an open WebSocket"
        else
            fail "$name: exit $STOP_RC"; tail -20 "$WORK/$name.log"
        fi
    done

    port=$((port + 1))
    name="$rt-https"
    echo "=== $rt: SIGTERM, HTTPS ==="
    if [ "$HAVE_TLS" != 1 ]; then
        skip "$name: openssl could not mint a certificate"
    elif ! start_server "$name" "$port" https "$FIX/server.$rt" \
            --drain-timeout 100 --tls-cert "$WORK/cert.pem" --tls-key "$WORK/key.pem"; then
        fail "$name: server did not start"; cat "$WORK/$name.log"
    else
        load_server "$name" "$port" https
        stop_server
        if [ "$STOP_RC" = 0 ] && clean_log "$WORK/$name.log"; then
            pass "$name: exit 0 (the server TLS context is freed once, after close_notify)"
        else
            fail "$name: exit $STOP_RC"; tail -20 "$WORK/$name.log"
        fi
    fi
done

# app.main returning with work in flight; its detached fetch targets a running
# server's /slow, which never answers in time.
port=$((port + 1))
echo "=== app.main leaves work behind ==="
if start_server target "$port" http "$FIX/server.lua"; then
    for rt in lua js; do
        rc=0
        "$HULL" --no-sandbox "$FIX/main.$rt" -- "$port" >"$WORK/main-$rt.log" 2>&1 || rc=$?
        if [ "$rc" = 0 ] && grep -q "teardown-main: left work behind" "$WORK/main-$rt.log" \
           && clean_log "$WORK/main-$rt.log"; then
            pass "$rt: app.main with unawaited dispatch / db.async / sleep / fetch exits 0"
        else
            fail "$rt: app.main exit $rc"; tail -20 "$WORK/main-$rt.log"
        fi
    done
    stop_server
    [ "$STOP_RC" = 0 ] && pass "fetch target: exit 0 with the cancelled fetches' requests parked" \
                       || { fail "fetch target: exit $STOP_RC"; tail -20 "$WORK/target.log"; }
else
    fail "fetch target did not start"; cat "$WORK/target.log"
fi

echo ""
echo "e2e_teardown: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
