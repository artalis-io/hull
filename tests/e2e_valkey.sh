#!/bin/sh
# e2e_valkey.sh: end-to-end test of the Valkey/Redis KV backend against a REAL
# server. Exercises the full hull.kv / hull.cache store surface (set/get/del/
# exists/incr/cas/scan/clear + TTL + kv/cache namespace isolation) and the
# borrow-copy guard, in both the Lua and JS runtimes.
#
# Server selection (first available):
#   1. local redis-server on PATH (fast, no Docker)   -> engine "redis-local"
#   2. docker valkey/valkey:8                          -> engine "valkey-docker"
#   3. docker redis:7                                  -> engine "redis-docker"
# With none available the test SKIPs (exit 0) rather than failing, so a dev box
# without Redis/Docker is not blocked. CI runs the Docker engines explicitly.
#
# The KV backend must be compiled in: build hull with HL_ENABLE_VALKEY=1 (dev)
# or compose --with=valkey. A plain base SKIPs (the app-load errors with an
# install hint, which we detect).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$ROOT/build/hull}"
FIX="$ROOT/tests/fixtures"
PORT="${VALKEY_TEST_PORT:-16399}"
PASS=0

log()  { printf '%s\n' "$*"; }
skip() { log "SKIP: $*"; exit 0; }
fail() { log "FAIL: $*"; exit 1; }

[ -x "$HULL" ] || fail "hull binary not found at $HULL (make HL_ENABLE_VALKEY=1)"

# A base without the KV backend composed: the fixture app-load fails with an
# install hint. Detect that and SKIP rather than fail.
probe="$("$HULL" "$FIX/valkey_kv_lua/app.lua" --no-sandbox -- "redis://127.0.0.1:$PORT" 2>&1 || true)"
case "$probe" in
    *"feature install valkey"*|*"--with=valkey"*)
        skip "hull built without the valkey backend (HL_ENABLE_VALKEY=0)" ;;
esac

# ---- server lifecycle -------------------------------------------------------
SERVER_PID=""; CONTAINER=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    [ -n "$CONTAINER" ] && docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

wait_ready() {
    # $1 = engine label; poll a bare TCP connect via hull-independent means.
    i=0
    while [ "$i" -lt 50 ]; do
        if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then exec 3>&- 3<&-; return 0; fi
        i=$((i+1)); sleep 0.1
    done
    return 1
}

start_server() {
    if command -v redis-server >/dev/null 2>&1; then
        ENGINE="redis-local"
        redis-server --port "$PORT" --save '' --appendonly no --daemonize no \
            >/tmp/hull_valkey_srv.log 2>&1 &
        SERVER_PID=$!
    elif command -v docker >/dev/null 2>&1; then
        CONTAINER="hull-valkey-e2e-$$"
        if docker pull valkey/valkey:8 >/dev/null 2>&1; then
            ENGINE="valkey-docker"; IMG="valkey/valkey:8"
        else
            ENGINE="redis-docker"; IMG="redis:7"
        fi
        docker run -d --name "$CONTAINER" -p "$PORT:6379" "$IMG" >/dev/null
    else
        skip "no redis-server on PATH and no docker; cannot start a KV server"
    fi
    wait_ready || fail "$ENGINE did not become ready on port $PORT"
    log "engine: $ENGINE (port $PORT)"
}

run_fixture() {
    # $1 = runtime label, $2 = entry file
    rt="$1"; entry="$2"
    [ -f "$entry" ] || { log "  ($rt fixture absent, skipping)"; return 0; }
    out="$("$HULL" "$entry" --no-sandbox -- "redis://127.0.0.1:$PORT" 2>&1)" || {
        log "$out"; fail "$rt fixture exited non-zero";
    }
    printf '%s\n' "$out" | grep -q "ALL OK" || { log "$out"; fail "$rt fixture missing ALL OK"; }
    log "  $rt: ALL OK"
    PASS=$((PASS+1))
}

start_server
log "== Valkey/Redis KV E2E =="
run_fixture "lua" "$FIX/valkey_kv_lua/app.lua"
run_fixture "js"  "$FIX/valkey_kv_js/app.js"

log "PASS ($PASS runtime(s))"
