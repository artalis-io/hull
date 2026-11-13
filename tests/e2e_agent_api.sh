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

# Ports the servers below listen on. Below 32768 on purpose: Linux hands out
# client source ports from 32768-60999 (ip_local_port_range), so a fixed
# listening port in that range can be held by any client socket on the host
# and the server then fails to bind (e2e_agent.sh flaked that way on CI).
# 11000-12999 is used by no other e2e script; the pid offset keeps two
# concurrent runs apart.
PORT_BASE=$((11000 + ($$ % 200) * 10))

# How long a server may take to come up; the wait ends as soon as the port
# answers or the process exits.
START_TIMEOUT=${HULL_E2E_START_TIMEOUT:-120}

LOG_DIR=$(mktemp -d)
trap 'rm -rf "$LOG_DIR"' EXIT

# dump_log LOG: print a server's output after a failed start.
dump_log() {
    echo "  --- $1 ---"
    if [ -s "$1" ]; then tail -n 60 "$1" | sed 's/^/  | /'; else echo "  | (empty)"; fi
}

# wait_until_up PID PORT LOG: wait until http://127.0.0.1:PORT/health
# answers. Fails at once when PID exits and after START_TIMEOUT seconds
# otherwise, printing LOG either way.
wait_until_up() {
    _wpid=$1; _wport=$2; _wlog=$3
    _wend=$(( $(date +%s) + START_TIMEOUT ))
    while :; do
        if curl -s -m 5 -o /dev/null "http://127.0.0.1:$_wport/health" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$_wpid" 2>/dev/null; then
            echo "  process $_wpid exited before port $_wport answered"
            break
        fi
        if [ "$(date +%s)" -ge "$_wend" ]; then
            echo "  port $_wport did not answer within ${START_TIMEOUT}s"
            break
        fi
        sleep 0.25
    done
    dump_log "$_wlog"
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

PORT_API=$((PORT_BASE + 0))
TMPDIR_API=$(mktemp -d)

$HULL -p "$PORT_API" -d "$TMPDIR_API/data.db" --agent-api "examples/hello/app.$APP_EXT" >"$LOG_DIR/api.log" 2>&1 &
SERVER_PID=$!

if ! wait_until_up "$SERVER_PID" "$PORT_API" "$LOG_DIR/api.log"; then
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

    # DNS rebinding (audit 9): a page on an attacker's name resolved to
    # 127.0.0.1 sends that name as Host, and a browser adds Origin.
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -H "Host: rebind.example:$PORT_API" \
        "http://127.0.0.1:$PORT_API/_hull/agent/routes")
    if [ "$STATUS" = "403" ]; then pass "api refuses a foreign Host"
    else fail "api foreign Host returned $STATUS (expected 403)"; fi
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -H "Origin: http://rebind.example" \
        "http://127.0.0.1:$PORT_API/_hull/agent/routes")
    if [ "$STATUS" = "403" ]; then pass "api refuses a request with Origin"
    else fail "api Origin request returned $STATUS (expected 403)"; fi
    OUT=$(curl -s -H "Host: localhost:$PORT_API" "http://127.0.0.1:$PORT_API/_hull/agent/routes")
    check_contains "api accepts Host localhost" "$OUT" '"routes"'

    stop_server
fi
rm -rf "$TMPDIR_API"

# ── /routes answers from the live app (audit 18) ───────────────────────
#
# The app registers a route named after the time its top level ran and
# reports that name at /stamp. /routes must list exactly that route: a
# fresh app context (the old behaviour) re-ran the top level later and
# listed a different one.

echo ""
echo "--- agent-api: /routes reuses the live app context ---"

PORT_LIVE=$((PORT_BASE + 1))
TMPDIR_LIVE=$(mktemp -d)
cat > "$TMPDIR_LIVE/app.lua" <<'LUAEOF'
local time = require("hull.time")
app.manifest({ modules = { "hull/http-server@1", "hull/time@1" } })
local stamp = "/r" .. tostring(time.now_ms())
app.get("/health", function(_req, res) res:text("ok") end)
app.get("/stamp", function(_req, res) res:text(stamp) end)
app.get(stamp, function(_req, res) res:text("ok") end)
LUAEOF
cat > "$TMPDIR_LIVE/app.js" <<'JSEOF'
import { app } from "hull:app";
import { time } from "hull:time";
app.manifest({ modules: ["hull/http-server@1", "hull/time@1"] });
const stamp = "/r" + String(time.nowMs());
app.get("/health", (_req, res) => res.text("ok"));
app.get("/stamp", (_req, res) => res.text(stamp));
app.get(stamp, (_req, res) => res.text("ok"));
JSEOF

$HULL -p "$PORT_LIVE" --no-db --agent-api "$TMPDIR_LIVE/app.$APP_EXT" >"$LOG_DIR/live.log" 2>&1 &
SERVER_PID=$!

if ! wait_until_up "$SERVER_PID" "$PORT_LIVE" "$LOG_DIR/live.log"; then
    fail "agent-api live - server startup"
    stop_server
else
    sleep 0.2
    STAMP=$(curl -s "http://127.0.0.1:$PORT_LIVE/stamp")
    OUT=$(curl -s "http://127.0.0.1:$PORT_LIVE/_hull/agent/routes")
    case "$STAMP" in
        /r[0-9]*) check_contains "api routes lists the live app's routes" "$OUT" "\"pattern\":\"$STAMP\"" ;;
        *)        fail "agent-api live - /stamp returned '$STAMP'" ;;
    esac
    stop_server
fi
rm -rf "$TMPDIR_LIVE"

# ── Without --agent-api flag ────────────────────────────────────────────

echo ""
echo "--- agent-api: endpoints disabled ---"

PORT_NO_API=$((PORT_BASE + 2))
TMPDIR_NO_API=$(mktemp -d)

$HULL -p "$PORT_NO_API" -d "$TMPDIR_NO_API/data.db" "examples/hello/app.$APP_EXT" >"$LOG_DIR/no_api.log" 2>&1 &
SERVER_PID=$!

if ! wait_until_up "$SERVER_PID" "$PORT_NO_API" "$LOG_DIR/no_api.log"; then
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
