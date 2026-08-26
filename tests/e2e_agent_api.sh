#!/bin/sh
# E2E tests - hull --agent-api (diagnostic HTTP endpoints)
#
# Tests /_hull/agent/* endpoints when --agent-api is enabled,
# and verifies they are absent without the flag.
#
# Usage: sh tests/e2e_agent_api.sh
#        RUNTIME=js sh tests/e2e_agent_api.sh
# Requires: build/hull already built, curl available
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}

if [ ! -x "$HULL" ]; then
    echo "e2e_agent_api: hull binary not found at $HULL - run 'make' first"
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
    case "$2" in
        *"$3"*) pass "$1" ;;
        *)      fail "$1 - expected '$3' in: $(echo "$2" | head -c 200)" ;;
    esac
}

wait_for_server() {
    for _i in 1 2 3 4 5 6 7 8 9 10; do
        if curl -s "http://127.0.0.1:$1/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    echo "  server did not start on port $1"
    return 1
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

# Auto-detect runtime
EXPECTED_RT=$($HULL agent routes examples/hello 2>/dev/null | grep -o '"runtime":"[^"]*"' | head -1 | sed 's/"runtime":"//;s/"//')
if [ "$EXPECTED_RT" = "js" ]; then APP_EXT="js"; else APP_EXT="lua"; fi

echo ""
echo "=== E2E: hull --agent-api (detected runtime: $EXPECTED_RT) ==="

# ── With --agent-api flag ───────────────────────────────────────────────

echo ""
echo "--- agent-api: endpoints enabled ---"

PORT_API=39895
TMPDIR_API=$(mktemp -d)

$HULL -p "$PORT_API" -d "$TMPDIR_API/data.db" --agent-api "examples/hello/app.$APP_EXT" >/dev/null 2>&1 &
SERVER_PID=$!

if ! wait_for_server "$PORT_API"; then
    fail "agent-api - server startup"
    stop_server
    rm -rf "$TMPDIR_API"
else
    # GET /_hull/agent/routes
    OUT=$(curl -s "http://127.0.0.1:$PORT_API/_hull/agent/routes")
    check_contains "api routes has runtime" "$OUT" '"runtime"'
    check_contains "api routes has routes"  "$OUT" '"routes"'

    # GET /_hull/agent/schema
    OUT=$(curl -s "http://127.0.0.1:$PORT_API/_hull/agent/schema")
    check_contains "api schema has tables" "$OUT" '"tables"'

    # GET /_hull/agent/status
    OUT=$(curl -s "http://127.0.0.1:$PORT_API/_hull/agent/status")
    check_contains "api status has running" "$OUT" '"running"'

    # GET /_hull/agent/errors
    OUT=$(curl -s "http://127.0.0.1:$PORT_API/_hull/agent/errors")
    check_contains "api errors has errors" "$OUT" '"errors"'

    # Content-Type should be JSON
    HEADERS=$(curl -sI "http://127.0.0.1:$PORT_API/_hull/agent/routes")
    check_contains "api content-type json" "$HEADERS" 'application/json'

    stop_server
fi
rm -rf "$TMPDIR_API"

# ── Without --agent-api flag ────────────────────────────────────────────

echo ""
echo "--- agent-api: endpoints disabled ---"

PORT_NO_API=39896
TMPDIR_NO_API=$(mktemp -d)

$HULL -p "$PORT_NO_API" -d "$TMPDIR_NO_API/data.db" "examples/hello/app.$APP_EXT" >/dev/null 2>&1 &
SERVER_PID=$!

if ! wait_for_server "$PORT_NO_API"; then
    fail "no-agent-api - server startup"
    stop_server
    rm -rf "$TMPDIR_NO_API"
else
    # /_hull/agent/routes should 404 without --agent-api
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT_NO_API/_hull/agent/routes")
    if [ "$STATUS" = "404" ]; then
        pass "api disabled returns 404"
    else
        fail "api disabled returns $STATUS (expected 404)"
    fi

    stop_server
fi
rm -rf "$TMPDIR_NO_API"

# ── Summary ─────────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e agent-api tests passed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
