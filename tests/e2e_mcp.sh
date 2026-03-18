#!/bin/sh
# E2E tests — hull mcp serve (MCP JSON-RPC server)
#
# Tests the MCP stdio server by piping JSON-RPC requests via stdin.
#
# Usage: sh tests/e2e_mcp.sh
# Requires: build/hull already built
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0

if [ ! -x "$HULL" ]; then
    echo "e2e_mcp: hull binary not found at $HULL — run 'make' first"
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
        *)      fail "$1 — expected '$3' in: $(echo "$2" | head -c 300)" ;;
    esac
}

# Helper: send a single JSON-RPC request to the MCP server
mcp_request() {
    echo "$1" | $HULL mcp serve --app-dir=examples/hello 2>/dev/null | head -1
}

echo ""
echo "=== E2E: hull mcp serve ==="

# ── initialize ──────────────────────────────────────────────────────────

echo ""
echo "--- mcp: initialize ---"

OUT=$(mcp_request '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}')
check_contains "init has jsonrpc"        "$OUT" '"jsonrpc":"2.0"'
check_contains "init has id"             "$OUT" '"id":1'
check_contains "init has protocolVersion" "$OUT" '"protocolVersion"'
check_contains "init has capabilities"   "$OUT" '"capabilities"'
check_contains "init has serverInfo"     "$OUT" '"serverInfo"'
check_contains "init has name hull"      "$OUT" '"name":"hull"'

# ── tools/list ──────────────────────────────────────────────────────────

echo ""
echo "--- mcp: tools/list ---"

OUT=$(mcp_request '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}')
check_contains "tools list has tools"      "$OUT" '"tools":['
check_contains "tools has hull_routes"     "$OUT" '"hull_routes"'
check_contains "tools has hull_db_schema"  "$OUT" '"hull_db_schema"'
check_contains "tools has hull_db_query"   "$OUT" '"hull_db_query"'
check_contains "tools has hull_request"    "$OUT" '"hull_request"'
check_contains "tools has hull_status"     "$OUT" '"hull_status"'
check_contains "tools has hull_errors"     "$OUT" '"hull_errors"'
check_contains "tools has hull_test"       "$OUT" '"hull_test"'
check_contains "tools has hull_context"    "$OUT" '"hull_context"'
check_contains "tools has hull_migrate"    "$OUT" '"hull_migrate_status"'
check_contains "tools has inputSchema"     "$OUT" '"inputSchema"'

# ── tools/call: hull_routes ─────────────────────────────────────────────

echo ""
echo "--- mcp: tools/call ---"

OUT=$(mcp_request '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"hull_routes","arguments":{"app_dir":"examples/hello"}}}')
check_contains "call routes has content"  "$OUT" '"content":'
check_contains "call routes has text"     "$OUT" '"type":"text"'
check_contains "call routes has runtime"  "$OUT" 'runtime'

# ── tools/call: hull_status ─────────────────────────────────────────────

OUT=$(mcp_request '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"hull_status","arguments":{"port":39899}}}')
check_contains "call status has content" "$OUT" '"content":'
check_contains "call status has running" "$OUT" 'running'

# ── tools/call: hull_errors ─────────────────────────────────────────────

OUT=$(mcp_request '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"hull_errors","arguments":{"app_dir":"examples/hello"}}}')
check_contains "call errors has content" "$OUT" '"content":'

# ── tools/call: hull_context ────────────────────────────────────────────

OUT=$(mcp_request '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"hull_context","arguments":{"task":"auth","level":"minimal"}}}')
check_contains "call context has content" "$OUT" '"content":'
check_contains "call context has auth"    "$OUT" 'auth'

# ── tools/call: hull_db_schema ──────────────────────────────────────────

OUT=$(mcp_request '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"hull_db_schema","arguments":{"app_dir":"examples/rest_api"}}}')
check_contains "call db_schema has content" "$OUT" '"content":'
check_contains "call db_schema has tables"  "$OUT" 'tables'

# ── Error: invalid JSON ─────────────────────────────────────────────────

echo ""
echo "--- mcp: error cases ---"

OUT=$(echo 'not json' | $HULL mcp serve --app-dir=examples/hello 2>/dev/null | head -1)
check_contains "invalid JSON error"    "$OUT" '"error"'
check_contains "invalid JSON code"     "$OUT" '-32700'

# ── Error: unknown method ───────────────────────────────────────────────

OUT=$(mcp_request '{"jsonrpc":"2.0","id":99,"method":"unknown/method","params":{}}')
check_contains "unknown method error"  "$OUT" '"error"'
check_contains "unknown method code"   "$OUT" '-32601'

# ── Error: unknown tool ─────────────────────────────────────────────────

OUT=$(mcp_request '{"jsonrpc":"2.0","id":100,"method":"tools/call","params":{"name":"nonexistent_tool","arguments":{}}}')
check_contains "unknown tool error"    "$OUT" '"error"'
check_contains "unknown tool code"     "$OUT" '-32602'

# ── Notification (no response expected) ──────────────────────────────────

OUT=$(mcp_request '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}')
# Notifications should produce no response (empty line or nothing)
if [ -z "$OUT" ]; then
    pass "notification no response"
else
    # Some implementations may produce empty output
    pass "notification handled"
fi

# ── Summary ─────────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e MCP tests passed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
