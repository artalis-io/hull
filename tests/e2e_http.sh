#!/bin/sh
# E2E HTTP client tests — exercises the http module from Lua and JS
#
# Architecture:
#   1. Compiles and starts a Keel echo server (returns request details as JSON)
#   2. Starts Hull with test apps that make outbound HTTP requests to the echo server
#   3. Curls the Hull app routes, which trigger outbound calls, and verifies responses
#
# Usage: sh tests/e2e_http.sh
#        RUNTIME=js sh tests/e2e_http.sh    # test JS only
#        RUNTIME=lua sh tests/e2e_http.sh   # test Lua only
# Requires: build/hull already built, curl available
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}
ECHO_PORT=19860
ECHO_PID=""

if [ ! -x "$HULL" ]; then
    echo "e2e_http: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

check_contains() {
    # $1 = description, $2 = response body, $3 = expected substring
    case "$2" in
        *"$3"*) pass "$1" ;;
        *)      fail "$1 — expected '$3' in: $2" ;;
    esac
}

wait_for_server() {
    # $1 = port, $2 = path (default /health)
    _path="${2:-/health}"
    for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if curl -s "http://127.0.0.1:$1${_path}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.3
    done
    echo "  server did not start on port $1"
    return 1
}

stop_pid() {
    if [ -n "$1" ]; then
        kill "$1" 2>/dev/null || true
        wait "$1" 2>/dev/null || true
    fi
}

cleanup() {
    stop_pid "$ECHO_PID"
    stop_pid "$SERVER_PID"
    if [ -n "$TMPDIR_WORK" ] && [ -d "$TMPDIR_WORK" ]; then
        rm -rf "$TMPDIR_WORK"
    fi
}
trap cleanup EXIT

# ── Step 1: Compile the echo server ──────────────────────────────────

echo ""
echo "=== Step 1: Compile echo server ==="

TMPDIR_WORK=$(mktemp -d)
ECHO_BIN="$TMPDIR_WORK/http_echo_server"

KEEL_DIR="$SRCDIR/vendor/keel"
KEEL_LIB="$KEEL_DIR/libkeel.a"

if [ ! -f "$KEEL_LIB" ]; then
    echo "  building libkeel.a..."
    make -C "$KEEL_DIR" >/dev/null 2>&1
fi

cc -std=c11 -Wall -Wextra -O2 \
    -I"$KEEL_DIR/include" \
    -o "$ECHO_BIN" \
    "$SRCDIR/tests/fixtures/http_echo_server.c" \
    "$KEEL_LIB" -lpthread 2>&1

if [ ! -x "$ECHO_BIN" ]; then
    echo "  FATAL: failed to compile echo server"
    exit 1
fi
pass "echo server compiled"

# ── Step 2: Start the echo server ────────────────────────────────────

echo ""
echo "=== Step 2: Start echo server on port $ECHO_PORT ==="

"$ECHO_BIN" "$ECHO_PORT" >/dev/null 2>&1 &
ECHO_PID=$!

if ! wait_for_server "$ECHO_PORT" "/health"; then
    fail "echo server did not start"
    echo "FATAL: cannot continue without echo server"
    exit 1
fi
pass "echo server started (pid $ECHO_PID)"

# Verify echo server works
RESP=$(curl -s "http://127.0.0.1:$ECHO_PORT/echo")
check_contains "echo server GET /echo" "$RESP" '"method":"GET"'

RESP=$(curl -s -X POST -H "Content-Type: text/plain" -d 'test' "http://127.0.0.1:$ECHO_PORT/echo")
check_contains "echo server POST /echo body" "$RESP" '"body":"test"'

# ── Step 3: Run tests for each runtime ───────────────────────────────

run_http_tests() {
    LABEL=$1     # display label ("lua" or "js")
    PORT=$2
    APP=$3       # path to app file

    echo ""
    echo "=== E2E HTTP: $LABEL runtime (port $PORT) ==="

    DB="$TMPDIR_WORK/${LABEL}_data.db"
    SERVER_PID=""

    "$HULL" -p "$PORT" -d "$DB" -l debug "$APP" >/dev/null 2>&1 &
    SERVER_PID=$!

    if ! wait_for_server "$PORT"; then
        fail "$LABEL — Hull server startup"
        stop_pid "$SERVER_PID"
        SERVER_PID=""
        return
    fi
    pass "$LABEL — Hull server started"

    # Note: echo body is JSON-in-JSON, so inner quotes are escaped as \"
    # We use escaped patterns for method checks since they're in the nested string.

    # ── http.get() ──
    RESP=$(curl -s "http://127.0.0.1:$PORT/test/get")
    check_contains "$LABEL http.get() status" "$RESP" '"status":200'
    check_contains "$LABEL http.get() method" "$RESP" 'method'
    check_contains "$LABEL http.get() is GET" "$RESP" 'GET'

    # ── http.post() ──
    RESP=$(curl -s "http://127.0.0.1:$PORT/test/post")
    check_contains "$LABEL http.post() status" "$RESP" '"status":200'
    check_contains "$LABEL http.post() body" "$RESP" 'hello from'
    check_contains "$LABEL http.post() header" "$RESP" 'X-Test'

    # ── http.put() ──
    RESP=$(curl -s "http://127.0.0.1:$PORT/test/put")
    check_contains "$LABEL http.put() status" "$RESP" '"status":200'
    check_contains "$LABEL http.put() body" "$RESP" 'put-body'

    # ── http.patch() ──
    RESP=$(curl -s "http://127.0.0.1:$PORT/test/patch")
    check_contains "$LABEL http.patch() status" "$RESP" '"status":200'
    check_contains "$LABEL http.patch() body" "$RESP" 'patch-body'

    # ── http.delete() ──
    RESP=$(curl -s "http://127.0.0.1:$PORT/test/delete")
    check_contains "$LABEL http.delete() status" "$RESP" '"status":200'
    check_contains "$LABEL http.delete() method" "$RESP" 'DELETE'

    # ── http.request() with custom method ──
    RESP=$(curl -s "http://127.0.0.1:$PORT/test/request")
    check_contains "$LABEL http.request(OPTIONS) status" "$RESP" '"status":200'
    check_contains "$LABEL http.request(OPTIONS) method" "$RESP" 'OPTIONS'

    # ── Custom headers ──
    RESP=$(curl -s "http://127.0.0.1:$PORT/test/headers")
    check_contains "$LABEL custom header sent" "$RESP" 'X-Custom-Header'
    check_contains "$LABEL custom header value" "$RESP" "test-value-${LABEL}"

    # ── Host denied ──
    RESP=$(curl -s "http://127.0.0.1:$PORT/test/denied")
    check_contains "$LABEL denied host rejected" "$RESP" '"denied":true'

    stop_pid "$SERVER_PID"
    SERVER_PID=""
}

if [ "$RUNTIME" != "js" ]; then
    run_http_tests "lua" 19861 "$SRCDIR/tests/fixtures/http_client_app.lua"
fi
if [ "$RUNTIME" != "lua" ]; then
    run_http_tests "js"  19862 "$SRCDIR/tests/fixtures/http_client_app.js"
fi

# ── Summary ──────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e HTTP client tests passed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
