#!/bin/sh
# Hull benchmark — runs wrk against both Lua and JS runtimes
#
# Usage: sh bench.sh
#        RUNTIME=lua sh bench.sh   # benchmark Lua only
#        RUNTIME=js  sh bench.sh   # benchmark JS only
#
# Requires: build/hull already built, wrk and curl available
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
THREADS=${THREADS:-4}
CONNECTIONS=${CONNECTIONS:-100}
DURATION=${DURATION:-10s}
RUNTIME=${RUNTIME:-all}

if [ ! -x "$HULL" ]; then
    echo "bench: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

if ! command -v wrk >/dev/null 2>&1; then
    echo "bench: wrk not found. Install with: brew install wrk (macOS) or apt install wrk (Linux)"
    exit 1
fi

wait_for_server() {
    PORT=$1
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    echo "  server did not start on port $PORT"
    return 1
}

run_bench() {
    LABEL=$1
    PORT=$2
    APP=$3
    URL="http://127.0.0.1:$PORT"

    TMPDIR=$(mktemp -d)
    DB="$TMPDIR/bench.db"

    echo ""
    echo "=== Hull Benchmark: $LABEL ==="
    echo "  threads:      $THREADS"
    echo "  connections:  $CONNECTIONS"
    echo "  duration:     $DURATION"
    echo ""

    $HULL -p "$PORT" -d "$DB" "$APP" &
    SERVER_PID=$!
    trap "kill $SERVER_PID 2>/dev/null; rm -rf $TMPDIR" EXIT

    if ! wait_for_server "$PORT"; then
        echo "  FAIL: $LABEL server did not start"
        kill "$SERVER_PID" 2>/dev/null || true
        rm -rf "$TMPDIR"
        trap - EXIT
        return
    fi

    # Warmup
    wrk -t2 -c10 -d2s "$URL/health" >/dev/null 2>&1

    # GET /health — lightweight JSON (no DB)
    echo "--- GET /health (no DB) ---"
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$URL/health"
    echo ""

    # GET / — JSON response with DB write
    echo "--- GET / (DB write + JSON) ---"
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$URL/"
    echo ""

    # GET /greet/World — route param extraction
    echo "--- GET /greet/World (route param) ---"
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$URL/greet/World"
    echo ""

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
    trap - EXIT
}

# Run for selected runtimes — use different ports to avoid conflicts
if [ "$RUNTIME" != "js" ]; then
    run_bench "Lua" 19860 examples/hello/app.lua
fi
if [ "$RUNTIME" != "lua" ]; then
    run_bench "QuickJS" 19861 examples/hello/app.js
fi
