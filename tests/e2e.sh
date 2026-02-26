#!/bin/sh
# E2E tests — start hull with each runtime, verify routes via curl
#
# Usage: sh tests/e2e.sh
#        RUNTIME=js sh tests/e2e.sh    # test JS only
#        RUNTIME=lua sh tests/e2e.sh   # test Lua only
# Requires: build/hull already built, curl available
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}

if [ ! -x "$HULL" ]; then
    echo "e2e: hull binary not found at $HULL — run 'make' first"
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
    # $1 = port
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if curl -s "http://127.0.0.1:$1/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    echo "  server did not start on port $1"
    return 1
}

run_tests() {
    LABEL=$1     # display label ("lua" or "js")
    RT_NAME=$2   # runtime value in JSON ("lua" or "quickjs")
    PORT=$3
    APP=$4       # path to app file

    echo ""
    echo "=== E2E: $LABEL runtime (port $PORT) ==="

    TMPDIR=$(mktemp -d)
    DB="$TMPDIR/data.db"

    $HULL -p "$PORT" -d "$DB" "$APP" &
    SERVER_PID=$!
    trap "kill $SERVER_PID 2>/dev/null; rm -rf $TMPDIR" EXIT

    if ! wait_for_server "$PORT"; then
        fail "$LABEL — server startup"
        kill "$SERVER_PID" 2>/dev/null || true
        rm -rf "$TMPDIR"
        trap - EXIT
        return
    fi

    # GET /health
    RESP=$(curl -s "http://127.0.0.1:$PORT/health")
    check_contains "$LABEL GET /health status" "$RESP" '"status"'
    check_contains "$LABEL GET /health ok" "$RESP" '"ok"'
    check_contains "$LABEL GET /health runtime" "$RESP" "\"$RT_NAME\""

    # GET /
    RESP=$(curl -s "http://127.0.0.1:$PORT/")
    check_contains "$LABEL GET / message" "$RESP" '"message"'

    # GET /visits — should contain the visit from GET /
    RESP=$(curl -s "http://127.0.0.1:$PORT/visits")
    check_contains "$LABEL GET /visits array" "$RESP" "["

    # POST /echo — body should be echoed back
    RESP=$(curl -s -X POST -H "Content-Type: text/plain" \
           -d 'hello world' "http://127.0.0.1:$PORT/echo")
    check_contains "$LABEL POST /echo body" "$RESP" '"body"'
    check_contains "$LABEL POST /echo content" "$RESP" 'hello world'

    # GET /greet/:name — route param
    RESP=$(curl -s "http://127.0.0.1:$PORT/greet/World")
    check_contains "$LABEL GET /greet/World greeting" "$RESP" '"Hello, World!"'

    # POST /greet/:name — route param + body
    RESP=$(curl -s -X POST -H "Content-Type: text/plain" \
           -d 'payload' "http://127.0.0.1:$PORT/greet/Hull")
    check_contains "$LABEL POST /greet/Hull greeting" "$RESP" '"Hello, Hull!"'
    check_contains "$LABEL POST /greet/Hull body" "$RESP" '"payload"'

    # GET /greet/:name with query string — params + query coexist
    RESP=$(curl -s "http://127.0.0.1:$PORT/greet/Test?lang=en")
    check_contains "$LABEL GET /greet/Test?lang=en greeting" "$RESP" '"Hello, Test!"'

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
    trap - EXIT
}

# Run for selected runtimes — use different ports to avoid conflicts
if [ "$RUNTIME" != "js" ]; then
    run_tests "lua" "lua"     19850 examples/hello/app.lua
fi
if [ "$RUNTIME" != "lua" ]; then
    run_tests "js"  "quickjs" 19851 examples/hello/app.js
fi

# Summary
echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e tests passed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
