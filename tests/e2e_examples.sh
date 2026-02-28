#!/bin/sh
# E2E tests for all example apps — start hull, verify routes via curl
#
# Usage: sh tests/e2e_examples.sh
#        RUNTIME=js sh tests/e2e_examples.sh    # test JS only
#        RUNTIME=lua sh tests/e2e_examples.sh   # test Lua only
# Requires: build/hull already built, curl available
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}

if [ ! -x "$HULL" ]; then
    echo "e2e_examples: hull binary not found at $HULL — run 'make' first"
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

check_status() {
    # $1 = description, $2 = actual status code, $3 = expected status code
    if [ "$2" = "$3" ]; then
        pass "$1"
    else
        fail "$1 — expected status $3, got $2"
    fi
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

start_server() {
    # $1 = port, $2 = app file, $3 = db path
    $HULL -p "$1" -d "$3" "$2" >/dev/null 2>&1 &
    SERVER_PID=$!
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

# ── Hello ────────────────────────────────────────────────────────────

test_hello() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "--- hello ($LABEL) port $PORT ---"

    TMPDIR_HELLO=$(mktemp -d)
    start_server "$PORT" "$APP" "$TMPDIR_HELLO/data.db"
    if ! wait_for_server "$PORT"; then
        fail "$LABEL hello — server startup"
        stop_server; rm -rf "$TMPDIR_HELLO"; return
    fi

    RESP=$(curl -s "http://127.0.0.1:$PORT/health")
    check_contains "$LABEL hello GET /health" "$RESP" '"status"'

    RESP=$(curl -s "http://127.0.0.1:$PORT/")
    check_contains "$LABEL hello GET /" "$RESP" '"message"'

    RESP=$(curl -s "http://127.0.0.1:$PORT/visits")
    check_contains "$LABEL hello GET /visits" "$RESP" "["

    RESP=$(curl -s -X POST -H "Content-Type: text/plain" -d 'hello' "http://127.0.0.1:$PORT/echo")
    check_contains "$LABEL hello POST /echo" "$RESP" 'hello'

    RESP=$(curl -s "http://127.0.0.1:$PORT/greet/World")
    check_contains "$LABEL hello GET /greet/:name" "$RESP" '"Hello, World!"'

    stop_server; rm -rf "$TMPDIR_HELLO"
}

# ── REST API ─────────────────────────────────────────────────────────

test_rest_api() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "--- rest_api ($LABEL) port $PORT ---"

    TMPDIR_REST=$(mktemp -d)
    start_server "$PORT" "$APP" "$TMPDIR_REST/data.db"
    if ! wait_for_server "$PORT"; then
        fail "$LABEL rest_api — server startup"
        stop_server; rm -rf "$TMPDIR_REST"; return
    fi

    # Create a task
    RESP=$(curl -s -w "\n%{http_code}" -X POST -H "Content-Type: application/json" \
           -d '{"title":"Buy milk"}' "http://127.0.0.1:$PORT/tasks")
    BODY=$(echo "$RESP" | sed '$d')
    STATUS=$(echo "$RESP" | tail -1)
    check_status "$LABEL rest_api POST /tasks status" "$STATUS" "201"
    check_contains "$LABEL rest_api POST /tasks body" "$BODY" '"title"'
    check_contains "$LABEL rest_api POST /tasks title" "$BODY" 'Buy milk'

    # List tasks
    RESP=$(curl -s "http://127.0.0.1:$PORT/tasks")
    check_contains "$LABEL rest_api GET /tasks" "$RESP" 'Buy milk'

    # Get single task
    RESP=$(curl -s "http://127.0.0.1:$PORT/tasks/1")
    check_contains "$LABEL rest_api GET /tasks/1" "$RESP" 'Buy milk'

    # Update task
    RESP=$(curl -s -X PUT -H "Content-Type: application/json" \
           -d '{"title":"Buy oat milk","done":true}' "http://127.0.0.1:$PORT/tasks/1")
    check_contains "$LABEL rest_api PUT /tasks/1" "$RESP" '"ok"'

    # Delete task
    RESP=$(curl -s -X DELETE "http://127.0.0.1:$PORT/tasks/1")
    check_contains "$LABEL rest_api DELETE /tasks/1" "$RESP" '"ok"'

    # 404 on deleted task
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/tasks/1")
    check_status "$LABEL rest_api GET /tasks/1 after delete" "$STATUS" "404"

    # Validation: missing title
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" \
             -d '{}' "http://127.0.0.1:$PORT/tasks")
    check_status "$LABEL rest_api POST /tasks no title" "$STATUS" "400"

    stop_server; rm -rf "$TMPDIR_REST"
}

# ── Bench DB ─────────────────────────────────────────────────────────

test_bench_db() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "--- bench_db ($LABEL) port $PORT ---"

    TMPDIR_BENCH=$(mktemp -d)
    start_server "$PORT" "$APP" "$TMPDIR_BENCH/data.db"
    if ! wait_for_server "$PORT"; then
        fail "$LABEL bench_db — server startup"
        stop_server; rm -rf "$TMPDIR_BENCH"; return
    fi

    RESP=$(curl -s "http://127.0.0.1:$PORT/health")
    check_contains "$LABEL bench_db GET /health" "$RESP" '"ok"'

    RESP=$(curl -s "http://127.0.0.1:$PORT/read")
    check_contains "$LABEL bench_db GET /read" "$RESP" '"kind"'

    RESP=$(curl -s -X POST "http://127.0.0.1:$PORT/write")
    check_contains "$LABEL bench_db POST /write" "$RESP" '"inserted"'

    RESP=$(curl -s -X POST "http://127.0.0.1:$PORT/write-batch")
    check_contains "$LABEL bench_db POST /write-batch" "$RESP" '"inserted"'

    RESP=$(curl -s "http://127.0.0.1:$PORT/mixed")
    check_contains "$LABEL bench_db GET /mixed" "$RESP" '"kind"'

    stop_server; rm -rf "$TMPDIR_BENCH"
}

# ── Auth ─────────────────────────────────────────────────────────────

test_auth() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "--- auth ($LABEL) port $PORT ---"

    TMPDIR_AUTH=$(mktemp -d)
    COOKIE_JAR="$TMPDIR_AUTH/cookies.txt"
    start_server "$PORT" "$APP" "$TMPDIR_AUTH/data.db"
    if ! wait_for_server "$PORT"; then
        fail "$LABEL auth — server startup"
        stop_server; rm -rf "$TMPDIR_AUTH"; return
    fi

    # Health check
    RESP=$(curl -s "http://127.0.0.1:$PORT/health")
    check_contains "$LABEL auth GET /health" "$RESP" '"ok"'

    # Register
    RESP=$(curl -s -w "\n%{http_code}" -X POST -H "Content-Type: application/json" \
           -d '{"email":"alice@test.com","password":"secret1234","name":"Alice"}' \
           "http://127.0.0.1:$PORT/register")
    BODY=$(echo "$RESP" | sed '$d')
    STATUS=$(echo "$RESP" | tail -1)
    check_status "$LABEL auth POST /register status" "$STATUS" "201"
    check_contains "$LABEL auth POST /register email" "$BODY" 'alice@test.com'
    check_contains "$LABEL auth POST /register name" "$BODY" 'Alice'

    # Duplicate register
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" \
             -d '{"email":"alice@test.com","password":"secret1234","name":"Alice"}' \
             "http://127.0.0.1:$PORT/register")
    check_status "$LABEL auth POST /register duplicate" "$STATUS" "409"

    # Validation: short password
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" \
             -d '{"email":"bob@test.com","password":"short","name":"Bob"}' \
             "http://127.0.0.1:$PORT/register")
    check_status "$LABEL auth POST /register short pw" "$STATUS" "400"

    # GET /me without session → 401
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/me")
    check_status "$LABEL auth GET /me no session" "$STATUS" "401"

    # Login (save cookie)
    RESP=$(curl -s -c "$COOKIE_JAR" -X POST -H "Content-Type: application/json" \
           -d '{"email":"alice@test.com","password":"secret1234"}' \
           "http://127.0.0.1:$PORT/login")
    check_contains "$LABEL auth POST /login email" "$RESP" 'alice@test.com'
    check_contains "$LABEL auth POST /login name" "$RESP" 'Alice'

    # Wrong password
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" \
             -d '{"email":"alice@test.com","password":"wrong"}' \
             "http://127.0.0.1:$PORT/login")
    check_status "$LABEL auth POST /login bad pw" "$STATUS" "401"

    # GET /me with session
    RESP=$(curl -s -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/me")
    check_contains "$LABEL auth GET /me email" "$RESP" 'alice@test.com'
    check_contains "$LABEL auth GET /me name" "$RESP" 'Alice'

    # Logout
    RESP=$(curl -s -b "$COOKIE_JAR" -X POST "http://127.0.0.1:$PORT/logout")
    check_contains "$LABEL auth POST /logout" "$RESP" '"ok"'

    # GET /me after logout → 401
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/me")
    check_status "$LABEL auth GET /me after logout" "$STATUS" "401"

    stop_server; rm -rf "$TMPDIR_AUTH"
}

# ── JWT API ──────────────────────────────────────────────────────────

test_jwt_api() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "--- jwt_api ($LABEL) port $PORT ---"

    TMPDIR_JWT=$(mktemp -d)
    start_server "$PORT" "$APP" "$TMPDIR_JWT/data.db"
    if ! wait_for_server "$PORT"; then
        fail "$LABEL jwt_api — server startup"
        stop_server; rm -rf "$TMPDIR_JWT"; return
    fi

    # Health check
    RESP=$(curl -s "http://127.0.0.1:$PORT/health")
    check_contains "$LABEL jwt_api GET /health" "$RESP" '"ok"'

    # Register
    RESP=$(curl -s -w "\n%{http_code}" -X POST -H "Content-Type: application/json" \
           -d '{"email":"alice@test.com","password":"secret1234","name":"Alice"}' \
           "http://127.0.0.1:$PORT/register")
    BODY=$(echo "$RESP" | sed '$d')
    STATUS=$(echo "$RESP" | tail -1)
    check_status "$LABEL jwt_api POST /register status" "$STATUS" "201"
    check_contains "$LABEL jwt_api POST /register" "$BODY" 'alice@test.com'

    # GET /me without token → 401
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/me")
    check_status "$LABEL jwt_api GET /me no token" "$STATUS" "401"

    # Login — get token
    RESP=$(curl -s -X POST -H "Content-Type: application/json" \
           -d '{"email":"alice@test.com","password":"secret1234"}' \
           "http://127.0.0.1:$PORT/login")
    check_contains "$LABEL jwt_api POST /login token" "$RESP" '"token"'
    check_contains "$LABEL jwt_api POST /login email" "$RESP" 'alice@test.com'

    # Extract token (grab value between "token":" and next ")
    TOKEN=$(echo "$RESP" | sed 's/.*"token":"//' | sed 's/".*//')

    # GET /me with token
    RESP=$(curl -s -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/me")
    check_contains "$LABEL jwt_api GET /me email" "$RESP" 'alice@test.com'
    check_contains "$LABEL jwt_api GET /me name" "$RESP" 'Alice'

    # Refresh token
    RESP=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/refresh")
    check_contains "$LABEL jwt_api POST /refresh" "$RESP" '"token"'

    # Wrong password
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" \
             -d '{"email":"alice@test.com","password":"wrong"}' \
             "http://127.0.0.1:$PORT/login")
    check_status "$LABEL jwt_api POST /login bad pw" "$STATUS" "401"

    stop_server; rm -rf "$TMPDIR_JWT"
}

# ── CRUD with Auth ───────────────────────────────────────────────────

test_crud_with_auth() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "--- crud_with_auth ($LABEL) port $PORT ---"

    TMPDIR_CRUD=$(mktemp -d)
    COOKIE_JAR="$TMPDIR_CRUD/cookies.txt"
    COOKIE_JAR2="$TMPDIR_CRUD/cookies2.txt"
    start_server "$PORT" "$APP" "$TMPDIR_CRUD/data.db"
    if ! wait_for_server "$PORT"; then
        fail "$LABEL crud_with_auth — server startup"
        stop_server; rm -rf "$TMPDIR_CRUD"; return
    fi

    # Register two users
    curl -s -X POST -H "Content-Type: application/json" \
         -d '{"email":"alice@test.com","password":"secret1234","name":"Alice"}' \
         "http://127.0.0.1:$PORT/register" >/dev/null

    curl -s -X POST -H "Content-Type: application/json" \
         -d '{"email":"bob@test.com","password":"secret1234","name":"Bob"}' \
         "http://127.0.0.1:$PORT/register" >/dev/null

    # Login as Alice
    curl -s -c "$COOKIE_JAR" -X POST -H "Content-Type: application/json" \
         -d '{"email":"alice@test.com","password":"secret1234"}' \
         "http://127.0.0.1:$PORT/login" >/dev/null

    # Login as Bob
    curl -s -c "$COOKIE_JAR2" -X POST -H "Content-Type: application/json" \
         -d '{"email":"bob@test.com","password":"secret1234"}' \
         "http://127.0.0.1:$PORT/login" >/dev/null

    # Tasks require auth
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/tasks")
    check_status "$LABEL crud GET /tasks no auth" "$STATUS" "401"

    # Alice creates a task
    RESP=$(curl -s -w "\n%{http_code}" -b "$COOKIE_JAR" -X POST -H "Content-Type: application/json" \
           -d '{"title":"Alice task"}' "http://127.0.0.1:$PORT/tasks")
    BODY=$(echo "$RESP" | sed '$d')
    STATUS=$(echo "$RESP" | tail -1)
    check_status "$LABEL crud POST /tasks status" "$STATUS" "201"
    check_contains "$LABEL crud POST /tasks title" "$BODY" 'Alice task'

    # Alice sees her task
    RESP=$(curl -s -b "$COOKIE_JAR" "http://127.0.0.1:$PORT/tasks")
    check_contains "$LABEL crud GET /tasks alice" "$RESP" 'Alice task'

    # Bob does NOT see Alice's task (data isolation)
    RESP=$(curl -s -b "$COOKIE_JAR2" "http://127.0.0.1:$PORT/tasks")
    check_contains "$LABEL crud GET /tasks bob empty" "$RESP" '[]'

    # Bob cannot access Alice's task by ID
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -b "$COOKIE_JAR2" "http://127.0.0.1:$PORT/tasks/1")
    check_status "$LABEL crud GET /tasks/1 bob denied" "$STATUS" "404"

    # Alice deletes her task
    RESP=$(curl -s -b "$COOKIE_JAR" -X DELETE "http://127.0.0.1:$PORT/tasks/1")
    check_contains "$LABEL crud DELETE /tasks/1" "$RESP" '"ok"'

    stop_server; rm -rf "$TMPDIR_CRUD"
}

# ── Middleware ───────────────────────────────────────────────────────

test_middleware() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "--- middleware ($LABEL) port $PORT ---"

    TMPDIR_MW=$(mktemp -d)
    start_server "$PORT" "$APP" "$TMPDIR_MW/data.db"
    if ! wait_for_server "$PORT"; then
        fail "$LABEL middleware — server startup"
        stop_server; rm -rf "$TMPDIR_MW"; return
    fi

    # Health check
    RESP=$(curl -s "http://127.0.0.1:$PORT/health")
    check_contains "$LABEL middleware GET /health" "$RESP" '"ok"'

    # Root route has request_id
    RESP=$(curl -s -D - "http://127.0.0.1:$PORT/")
    check_contains "$LABEL middleware GET / request_id" "$RESP" 'request_id'
    check_contains "$LABEL middleware X-Request-ID header" "$RESP" 'X-Request-ID'

    # API route has rate limit headers
    RESP=$(curl -s -D - "http://127.0.0.1:$PORT/api/items")
    check_contains "$LABEL middleware GET /api/items" "$RESP" 'apple'
    check_contains "$LABEL middleware X-RateLimit-Limit" "$RESP" 'X-RateLimit-Limit'
    check_contains "$LABEL middleware X-RateLimit-Remaining" "$RESP" 'X-RateLimit-Remaining'

    # CORS preflight
    RESP=$(curl -s -D - -X OPTIONS -H "Origin: http://localhost:5173" "http://127.0.0.1:$PORT/api/items")
    check_contains "$LABEL middleware OPTIONS CORS" "$RESP" 'Access-Control-Allow-Origin'

    # Debug endpoint
    RESP=$(curl -s "http://127.0.0.1:$PORT/api/debug")
    check_contains "$LABEL middleware GET /api/debug" "$RESP" '"request_id"'
    check_contains "$LABEL middleware total_requests" "$RESP" '"total_requests"'

    stop_server; rm -rf "$TMPDIR_MW"
}

# ── Webhooks ─────────────────────────────────────────────────────────

test_webhooks() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "--- webhooks ($LABEL) port $PORT ---"

    TMPDIR_WH=$(mktemp -d)
    start_server "$PORT" "$APP" "$TMPDIR_WH/data.db"
    if ! wait_for_server "$PORT"; then
        fail "$LABEL webhooks — server startup"
        stop_server; rm -rf "$TMPDIR_WH"; return
    fi

    # Health check
    RESP=$(curl -s "http://127.0.0.1:$PORT/health")
    check_contains "$LABEL webhooks GET /health" "$RESP" '"ok"'

    # Register a webhook pointing to the receiver on the same server
    RESP=$(curl -s -w "\n%{http_code}" -X POST -H "Content-Type: application/json" \
           -d "{\"url\":\"http://127.0.0.1:$PORT/webhooks/receive\",\"events\":\"user.created,order.placed\"}" \
           "http://127.0.0.1:$PORT/webhooks")
    BODY=$(echo "$RESP" | sed '$d')
    STATUS=$(echo "$RESP" | tail -1)
    check_status "$LABEL webhooks POST /webhooks status" "$STATUS" "201"
    check_contains "$LABEL webhooks POST /webhooks url" "$BODY" 'webhooks/receive'

    # List webhooks
    RESP=$(curl -s "http://127.0.0.1:$PORT/webhooks")
    check_contains "$LABEL webhooks GET /webhooks" "$RESP" 'user.created'

    # Fire an event — should deliver to the registered webhook
    RESP=$(curl -s -X POST -H "Content-Type: application/json" \
           -d '{"event":"user.created","data":{"user_id":1}}' \
           "http://127.0.0.1:$PORT/events")
    check_contains "$LABEL webhooks POST /events event_id" "$RESP" '"event_id"'
    check_contains "$LABEL webhooks POST /events deliveries" "$RESP" '"deliveries"'

    # List events
    RESP=$(curl -s "http://127.0.0.1:$PORT/events")
    check_contains "$LABEL webhooks GET /events" "$RESP" 'user.created'

    # Verify signature — send with correct signature
    # (The webhook receiver endpoint validates HMAC-SHA256 signatures)
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
             -H "Content-Type: application/json" \
             -H "X-Webhook-Signature: sha256=invalid" \
             -d '{"event":"test"}' \
             "http://127.0.0.1:$PORT/webhooks/receive")
    check_status "$LABEL webhooks bad signature" "$STATUS" "401"

    # Missing signature → 401
    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
             -H "Content-Type: application/json" \
             -d '{"event":"test"}' \
             "http://127.0.0.1:$PORT/webhooks/receive")
    check_status "$LABEL webhooks no signature" "$STATUS" "401"

    stop_server; rm -rf "$TMPDIR_WH"
}

# ── Run all example tests ────────────────────────────────────────────

PORT_BASE=19870

echo ""
echo "=== E2E Example Tests ==="

if [ "$RUNTIME" != "js" ]; then
    test_hello          "lua" $((PORT_BASE))     examples/hello/app.lua
    test_rest_api       "lua" $((PORT_BASE + 1)) examples/rest_api/app.lua
    test_bench_db       "lua" $((PORT_BASE + 2)) examples/bench_db/app.lua
    test_auth           "lua" $((PORT_BASE + 3)) examples/auth/app.lua
    test_jwt_api        "lua" $((PORT_BASE + 4)) examples/jwt_api/app.lua
    test_crud_with_auth "lua" $((PORT_BASE + 5)) examples/crud_with_auth/app.lua
    test_middleware      "lua" $((PORT_BASE + 6)) examples/middleware/app.lua
    test_webhooks       "lua" $((PORT_BASE + 7)) examples/webhooks/app.lua
fi

if [ "$RUNTIME" != "lua" ]; then
    test_hello          "js" $((PORT_BASE + 10)) examples/hello/app.js
    test_rest_api       "js" $((PORT_BASE + 11)) examples/rest_api/app.js
    test_bench_db       "js" $((PORT_BASE + 12)) examples/bench_db/app.js
    test_auth           "js" $((PORT_BASE + 13)) examples/auth/app.js
    test_jwt_api        "js" $((PORT_BASE + 14)) examples/jwt_api/app.js
    test_crud_with_auth "js" $((PORT_BASE + 15)) examples/crud_with_auth/app.js
    test_middleware      "js" $((PORT_BASE + 16)) examples/middleware/app.js
    test_webhooks       "js" $((PORT_BASE + 17)) examples/webhooks/app.js
fi

# ── Summary ──────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e example tests passed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
