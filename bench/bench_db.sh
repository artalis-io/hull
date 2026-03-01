#!/bin/sh
# SQLite performance benchmark — measures read, write, and mixed workloads
#
# Usage: sh bench_db.sh
#
# Requires: build/hull already built, wrk available
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
THREADS=${THREADS:-4}
CONNECTIONS=${CONNECTIONS:-100}
DURATION=${DURATION:-10s}
PORT=19870

if [ ! -x "$HULL" ]; then
    echo "bench_db: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

if ! command -v wrk >/dev/null 2>&1; then
    echo "bench_db: wrk not found"
    exit 1
fi

wait_for_server() {
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    echo "  server did not start on port $PORT"
    return 1
}

TMPDIR=$(mktemp -d)
DB="$TMPDIR/bench.db"

echo ""
echo "=== Hull SQLite Benchmark ==="
echo "  threads:      $THREADS"
echo "  connections:  $CONNECTIONS"
echo "  duration:     $DURATION"
echo "  db:           $DB"
echo ""

$HULL -p "$PORT" -d "$DB" examples/bench_db/app.lua &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null; rm -rf $TMPDIR" EXIT

if ! wait_for_server; then
    echo "  FAIL: server did not start"
    exit 1
fi

# Warmup
wrk -t2 -c10 -d2s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1

# 1. Baseline — no DB
echo "--- GET /health (no DB baseline) ---"
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "http://127.0.0.1:$PORT/health"
echo ""

# 2. Read-heavy — SELECT 20 rows
echo "--- GET /read (SELECT 20 rows, indexed) ---"
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "http://127.0.0.1:$PORT/read"
echo ""

# 3. Write-heavy — single INSERT per request
echo "--- POST /write (single INSERT) ---"
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" -s bench_post.lua "http://127.0.0.1:$PORT/write"
echo ""

# 4. Write-batch — 10 INSERTs in a single transaction
echo "--- POST /write-batch (10 INSERTs in txn) ---"
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" -s bench_post.lua "http://127.0.0.1:$PORT/write-batch"
echo ""

# 5. Mixed — 1 INSERT + 1 SELECT per request
echo "--- GET /mixed (INSERT + SELECT 20) ---"
wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "http://127.0.0.1:$PORT/mixed"
echo ""

# Report DB size
if [ -f "$DB" ]; then
    DB_SIZE=$(ls -lh "$DB" | awk '{print $5}')
    WAL_SIZE="n/a"
    if [ -f "$DB-wal" ]; then
        WAL_SIZE=$(ls -lh "$DB-wal" | awk '{print $5}')
    fi
    echo "--- DB file sizes ---"
    echo "  database: $DB_SIZE"
    echo "  WAL:      $WAL_SIZE"
    echo ""
fi

kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
rm -rf "$TMPDIR"
trap - EXIT
