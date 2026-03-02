#!/bin/sh
# Template rendering performance benchmark — measures throughput at varying complexity
#
# Usage: sh bench/bench_template.sh
#        RUNTIME=lua sh bench/bench_template.sh   # benchmark Lua only
#        RUNTIME=js  sh bench/bench_template.sh   # benchmark JS only
#
# Requires: build/hull already built, wrk and curl available
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

cd "$(dirname "$0")/.."

HULL=./build/hull
THREADS=${THREADS:-4}
CONNECTIONS=${CONNECTIONS:-100}
DURATION=${DURATION:-10s}
RUNTIME=${RUNTIME:-all}

if [ ! -x "$HULL" ]; then
    echo "bench_template: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

if ! command -v wrk >/dev/null 2>&1; then
    echo "bench_template: wrk not found. Install with: brew install wrk (macOS) or apt install wrk (Linux)"
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

    echo ""
    echo "=== Hull Template Benchmark: $LABEL ==="
    echo "  threads:      $THREADS"
    echo "  connections:  $CONNECTIONS"
    echo "  duration:     $DURATION"
    echo ""

    $HULL -p "$PORT" "$APP" &
    SERVER_PID=$!
    trap "kill $SERVER_PID 2>/dev/null" EXIT

    if ! wait_for_server "$PORT"; then
        echo "  FAIL: $LABEL server did not start"
        kill "$SERVER_PID" 2>/dev/null || true
        trap - EXIT
        return
    fi

    # Warmup
    wrk -t2 -c10 -d2s "$URL/health" >/dev/null 2>&1

    # 1. Baseline — JSON, no template
    echo "--- GET /health (JSON baseline) ---"
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$URL/health"
    echo ""

    # 2. Simple — variable substitution only
    echo "--- GET /simple (variable substitution) ---"
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$URL/simple"
    echo ""

    # 3. Loop — 50-item loop + conditionals
    echo "--- GET /loop (50-item loop + conditionals) ---"
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$URL/loop"
    echo ""

    # 4. Full — inheritance + include + loop + filters + conditionals
    echo "--- GET /full (inheritance + include + loop + filters) ---"
    wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$URL/full"
    echo ""

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    trap - EXIT
}

# Run for selected runtimes — use different ports to avoid conflicts
if [ "$RUNTIME" != "js" ]; then
    run_bench "Lua" 19880 examples/bench_template/app.lua
fi
if [ "$RUNTIME" != "lua" ]; then
    run_bench "QuickJS" 19881 examples/bench_template/app.js
fi
