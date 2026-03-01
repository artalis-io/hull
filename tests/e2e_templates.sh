#!/bin/sh
# E2E tests for the template engine — Lua and JS runtimes
#
# Usage: sh tests/e2e_templates.sh
#        RUNTIME=js sh tests/e2e_templates.sh    # test JS only
#        RUNTIME=lua sh tests/e2e_templates.sh   # test Lua only
# Requires: build/hull already built, curl available
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}

if [ ! -x "$HULL" ]; then
    echo "e2e_templates: hull binary not found at $HULL — run 'make' first"
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

wait_for_server() {
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

# Check a single test route — expects JSON with "ok":true
check_test() {
    # $1 = label, $2 = port, $3 = path
    RESP=$(curl -s "http://127.0.0.1:$2$3")
    case "$RESP" in
        *'"ok":false'*)
            fail "$1 — $RESP"
            ;;
        *'"ok":true'*)
            pass "$1"
            ;;
        *)
            fail "$1 — unexpected response: $RESP"
            ;;
    esac
}

# Check a multi-result test route (returns array) — each entry must have "ok":true
check_test_multi() {
    # $1 = label, $2 = port, $3 = path
    RESP=$(curl -s "http://127.0.0.1:$2$3")
    case "$RESP" in
        *'"ok":false'*)
            fail "$1 — $RESP"
            ;;
        *'"ok":true'*)
            pass "$1"
            ;;
        *)
            fail "$1 — unexpected response: $RESP"
            ;;
    esac
}

run_template_tests() {
    LABEL=$1
    PORT=$2
    APP=$3

    echo ""
    echo "--- template ($LABEL) port $PORT ---"

    TMPDIR_TPL=$(mktemp -d)
    start_server "$PORT" "$APP" "$TMPDIR_TPL/data.db"
    if ! wait_for_server "$PORT"; then
        fail "$LABEL template — server startup"
        stop_server; rm -rf "$TMPDIR_TPL"; return
    fi

    check_test      "$LABEL text"           "$PORT" "/test/text"
    check_test      "$LABEL var-escape"     "$PORT" "/test/var-escape"
    check_test      "$LABEL raw"            "$PORT" "/test/raw"
    check_test      "$LABEL dot-path"       "$PORT" "/test/dot-path"
    check_test      "$LABEL nil-path"       "$PORT" "/test/nil-path"
    check_test      "$LABEL if-true"        "$PORT" "/test/if-true"
    check_test      "$LABEL if-false"       "$PORT" "/test/if-false"
    check_test      "$LABEL elif"           "$PORT" "/test/elif"
    check_test      "$LABEL if-not"         "$PORT" "/test/if-not"
    check_test      "$LABEL for"            "$PORT" "/test/for"
    check_test      "$LABEL for-dot"        "$PORT" "/test/for-dot"
    check_test      "$LABEL for-nil"        "$PORT" "/test/for-nil"
    check_test      "$LABEL nested"         "$PORT" "/test/nested"
    check_test_multi "$LABEL filters"       "$PORT" "/test/filters"
    check_test      "$LABEL comment"        "$PORT" "/test/comment"
    check_test      "$LABEL extends"        "$PORT" "/test/extends"
    check_test      "$LABEL include"        "$PORT" "/test/include"
    check_test      "$LABEL for-kv"         "$PORT" "/test/for-kv"
    check_test_multi "$LABEL if-for-else"   "$PORT" "/test/if-for-else"
    check_test      "$LABEL xss"            "$PORT" "/test/xss"

    stop_server; rm -rf "$TMPDIR_TPL"
}

# ── Run tests ────────────────────────────────────────────────────────

PORT_BASE=19890

echo ""
echo "=== E2E Template Engine Tests ==="

if [ "$RUNTIME" != "js" ]; then
    run_template_tests "lua" $((PORT_BASE))     tests/fixtures/template_test/app.lua
fi

if [ "$RUNTIME" != "lua" ]; then
    run_template_tests "js"  $((PORT_BASE + 1)) tests/fixtures/template_test/app.js
fi

# ── Summary ──────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e template tests passed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
