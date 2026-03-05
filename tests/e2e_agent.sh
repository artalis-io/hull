#!/bin/sh
# E2E tests — hull agent subcommands
#
# Tests all 7 agent subcommands: routes, db schema, db query, request,
# status, errors, test — plus help/usage.
#
# Usage: sh tests/e2e_agent.sh
#        RUNTIME=js sh tests/e2e_agent.sh
#        RUNTIME=lua sh tests/e2e_agent.sh
# Requires: build/hull already built, curl available
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}

if [ ! -x "$HULL" ]; then
    echo "e2e_agent: hull binary not found at $HULL — run 'make' first"
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

check_not_contains() {
    case "$2" in
        *"$3"*) fail "$1 — unexpected '$3'" ;;
        *)      pass "$1" ;;
    esac
}

check_exit() {
    # $1 = description, $2 = actual exit code, $3 = expected exit code
    if [ "$2" = "$3" ]; then
        pass "$1"
    else
        fail "$1 — expected exit $3, got $2"
    fi
}

wait_for_server() {
    # $1 = port
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

# Auto-detect the runtime the binary actually uses (Lua preferred if both compiled in).
# This is more robust than relying on RUNTIME env var, which is a build-time setting.
EXPECTED_RT=$($HULL agent routes examples/hello 2>/dev/null | grep -o '"runtime":"[^"]*"' | head -1 | sed 's/"runtime":"//;s/"//')
if [ -z "$EXPECTED_RT" ]; then
    echo "e2e_agent: could not detect runtime — agent routes failed"
    exit 1
fi

if [ "$EXPECTED_RT" = "js" ]; then
    APP_EXT="js"
else
    APP_EXT="lua"
fi

HELLO_APP=examples/hello/app.$APP_EXT

echo ""
echo "=== E2E: hull agent (detected runtime: $EXPECTED_RT) ==="

# ── routes ────────────────────────────────────────────────────────────

echo ""
echo "--- agent routes ---"

OUT=$($HULL agent routes examples/hello 2>&1)
check_contains "routes has runtime"        "$OUT" '"runtime"'
check_contains "routes runtime value"      "$OUT" "\"runtime\":\"$EXPECTED_RT\""
check_contains "routes has routes array"   "$OUT" '"routes":['
check_contains "routes has GET method"     "$OUT" '"method":"GET"'
check_contains "routes has / pattern"      "$OUT" '"pattern":"/"'
check_contains "routes has middleware key" "$OUT" '"middleware":'

# Webhooks app has post-body middleware (idempotency)
OUT=$($HULL agent routes examples/webhooks 2>&1)
check_contains "webhooks has routes"       "$OUT" '"routes":['
check_contains "webhooks has POST method"  "$OUT" '"method":"POST"'
check_contains "webhooks has phase"        "$OUT" '"phase":'

# Bad app dir
EXIT_CODE=0
OUT=$($HULL agent routes /nonexistent 2>&1) || EXIT_CODE=$?
check_exit "routes bad dir exit code" "$EXIT_CODE" "1"

# ── db schema ─────────────────────────────────────────────────────────

echo ""
echo "--- agent db schema ---"

OUT=$($HULL agent db schema examples/rest_api 2>&1)
check_contains "db schema has tables"      "$OUT" '"tables":['
check_contains "db schema has tasks table" "$OUT" '"name":"tasks"'
check_contains "db schema has title col"   "$OUT" '"name":"title"'
check_contains "db schema has type"        "$OUT" '"type":'

# Hello app schema (visits table)
OUT=$($HULL agent db schema examples/hello 2>&1)
check_contains "db schema hello visits"    "$OUT" '"name":"visits"'

# ── db query ──────────────────────────────────────────────────────────

echo ""
echo "--- agent db query ---"

# Query on rest_api schema (empty tasks table from migration)
OUT=$($HULL agent db query "SELECT * FROM tasks" examples/rest_api 2>&1)
check_contains "db query has columns"      "$OUT" '"columns":['
check_contains "db query has title col"    "$OUT" '"title"'
check_contains "db query has rows"         "$OUT" '"rows":['
check_contains "db query has count"        "$OUT" '"count":0'

# Schema introspection query
OUT=$($HULL agent db query "SELECT name FROM sqlite_master WHERE type='table' AND name='tasks'" examples/rest_api 2>&1)
check_contains "db query finds tasks"      "$OUT" '"tasks"'
check_contains "db query count 1"          "$OUT" '"count":1'

# Bad SQL
EXIT_CODE=0
OUT=$($HULL agent db query "INVALID SQL STATEMENT" examples/rest_api 2>&1) || EXIT_CODE=$?
check_contains "db query bad SQL error"    "$OUT" '"error"'
check_exit     "db query bad SQL exit"     "$EXIT_CODE" "1"

# Missing SQL argument
EXIT_CODE=0
OUT=$($HULL agent db query 2>&1) || EXIT_CODE=$?
check_exit     "db query missing SQL exit" "$EXIT_CODE" "1"

# ── request ───────────────────────────────────────────────────────────

echo ""
echo "--- agent request ---"

PORT_REQ=39890
TMPDIR_REQ=$(mktemp -d)

$HULL -p "$PORT_REQ" -d "$TMPDIR_REQ/data.db" "$HELLO_APP" >/dev/null 2>&1 &
SERVER_PID=$!

if ! wait_for_server "$PORT_REQ"; then
    fail "request — server startup"
    stop_server
    rm -rf "$TMPDIR_REQ"
else
    # GET /health — verify all JSON fields
    OUT=$($HULL agent request GET /health -p "$PORT_REQ")
    check_contains "request GET has status"      "$OUT" '"status":'
    check_contains "request GET status 200"      "$OUT" '"status":200'
    check_contains "request GET has body"        "$OUT" '"body":'
    check_contains "request GET has headers"     "$OUT" '"headers":{'
    check_contains "request GET has elapsed_ms"  "$OUT" '"elapsed_ms":'

    # POST /echo with body and header
    OUT=$($HULL agent request POST /echo -p "$PORT_REQ" \
          -d 'hello agent' -H "Content-Type: text/plain")
    check_contains "request POST body echoed"    "$OUT" 'hello agent'
    check_contains "request POST status 200"     "$OUT" '"status":200'

    # 404 for nonexistent path
    OUT=$($HULL agent request GET /nonexistent -p "$PORT_REQ")
    check_contains "request 404 status"          "$OUT" '"status":404'

    stop_server
fi
rm -rf "$TMPDIR_REQ"

# Connection refused (no server running on unlikely port)
EXIT_CODE=0
OUT=$($HULL agent request GET /health -p 39899 2>&1) || EXIT_CODE=$?
check_contains "request no server error"   "$OUT" '"error"'
check_exit     "request no server exit"    "$EXIT_CODE" "1"

# ── status ────────────────────────────────────────────────────────────

echo ""
echo "--- agent status ---"

# No server running on unlikely port
OUT=$($HULL agent status -p 39899)
check_contains "status not running"        "$OUT" '"running":false'
check_contains "status port shown"         "$OUT" '"port":39899'

# Server running
PORT_STATUS=39891
TMPDIR_STATUS=$(mktemp -d)

$HULL -p "$PORT_STATUS" -d "$TMPDIR_STATUS/data.db" "$HELLO_APP" >/dev/null 2>&1 &
SERVER_PID=$!

if ! wait_for_server "$PORT_STATUS"; then
    fail "status — server startup"
else
    OUT=$($HULL agent status -p "$PORT_STATUS")
    check_contains "status running"            "$OUT" '"running":true'
    check_contains "status running port"       "$OUT" "\"port\":$PORT_STATUS"
fi

stop_server
rm -rf "$TMPDIR_STATUS"

# ── errors ────────────────────────────────────────────────────────────

echo ""
echo "--- agent errors ---"

TMPDIR_ERR=$(mktemp -d)

# No error file → empty errors array
OUT=$($HULL agent errors "$TMPDIR_ERR")
check_contains "errors empty"              "$OUT" '"errors":[]'

# Create .hull/last_error.json → passthrough
mkdir -p "$TMPDIR_ERR/.hull"
printf '{"error":"test error","timestamp":1234567890}\n' > "$TMPDIR_ERR/.hull/last_error.json"

OUT=$($HULL agent errors "$TMPDIR_ERR")
check_contains "errors has error"          "$OUT" '"error":"test error"'
check_contains "errors has timestamp"      "$OUT" '"timestamp":1234567890'

rm -rf "$TMPDIR_ERR"

# ── test ──────────────────────────────────────────────────────────────

echo ""
echo "--- agent test ---"

# Run tests on hello example (all tests should pass)
OUT=$($HULL agent test examples/hello 2>&1)
check_contains "test has runtime"          "$OUT" "\"runtime\":\"$EXPECTED_RT\""
check_contains "test has files"            "$OUT" '"files":['
check_contains "test has total"            "$OUT" '"total":'
check_contains "test has passed"           "$OUT" '"passed":'
check_contains "test has failed key"       "$OUT" '"failed":'
check_contains "test has test name"        "$OUT" '"name":'
check_contains "test has pass status"      "$OUT" '"status":"pass"'

# Run tests on rest_api example (CRUD tests)
OUT=$($HULL agent test examples/rest_api 2>&1)
check_contains "test rest_api has files"   "$OUT" '"files":['
check_contains "test rest_api has total"   "$OUT" '"total":'

# No test files → error
TMPDIR_NOTEST=$(mktemp -d)
cp "examples/hello/app.$APP_EXT" "$TMPDIR_NOTEST/app.$APP_EXT"
cp -r examples/hello/migrations "$TMPDIR_NOTEST/" 2>/dev/null || true

EXIT_CODE=0
OUT=$($HULL agent test "$TMPDIR_NOTEST" 2>&1) || EXIT_CODE=$?
check_contains "test no files error"       "$OUT" 'no test files'
check_exit     "test no files exit"        "$EXIT_CODE" "1"

rm -rf "$TMPDIR_NOTEST"

# No entry point → error
EXIT_CODE=0
OUT=$($HULL agent test /nonexistent 2>&1) || EXIT_CODE=$?
check_exit     "test bad dir exit"         "$EXIT_CODE" "1"

# ── help / usage ──────────────────────────────────────────────────────

echo ""
echo "--- agent help ---"

OUT=$($HULL agent --help 2>&1)
check_contains "help has Usage"            "$OUT" 'Usage'
check_contains "help lists routes"         "$OUT" 'routes'
check_contains "help lists request"        "$OUT" 'request'
check_contains "help lists test"           "$OUT" 'test'

# No subcommand → usage + exit 1
EXIT_CODE=0
OUT=$($HULL agent 2>&1) || EXIT_CODE=$?
check_contains "no subcommand usage"       "$OUT" 'Usage'
check_exit     "no subcommand exit"        "$EXIT_CODE" "1"

# Unknown subcommand → error + exit 1
EXIT_CODE=0
OUT=$($HULL agent bogus 2>&1) || EXIT_CODE=$?
check_contains "unknown subcommand msg"    "$OUT" "unknown subcommand"
check_exit     "unknown subcommand exit"   "$EXIT_CODE" "1"

# ── Summary ───────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e agent tests passed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
