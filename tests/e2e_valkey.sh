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

# Readiness probe: a PING that works under POSIX sh / dash (NO /dev/tcp - that is
# a bashism and CI runs `sh` = dash). Host redis-cli reaches both a local server
# and a docker container (the -p PORT:6379 mapping); falls back to redis-cli
# inside the container.
ping_ok() {
    if command -v redis-cli >/dev/null 2>&1; then
        redis-cli -p "$PORT" ping 2>/dev/null | grep -q PONG && return 0
    fi
    [ -n "$CONTAINER" ] && docker exec "$CONTAINER" redis-cli ping 2>/dev/null | grep -q PONG && return 0
    return 1
}
wait_ready() {
    i=0
    while [ "$i" -lt 100 ]; do
        ping_ok && return 0
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

# ── Transport-on-Keel evidence (docs/valkey_keel_transport_slice5.md) ─────────
# The connection now rides the shared cap/db_transport.c. Prove two properties
# against the real server that the unit tests cannot: rediss:// fails CLOSED (no
# plaintext fallback), and the connect timeout is live end-to-end.
TDIR=$(mktemp -d)
# A tiny app that just attempts one kv.open and reports OK/FAIL for a DSN arg.
conn_probe_app() {   # $1 = scheme, $2 = allowed host  ->  writes $TDIR/probe.lua
    cat > "$TDIR/probe.lua" <<LUA
app.manifest({ modules = { "hull/kv@1" },
    kv = { dynamic = { schemes = { "$1" }, hosts = { "$2" } } } })
local kv = require("hull.kv")
app.main(function(ctx)
    local ok, res = pcall(function()
        return kv.open({ backend = "valkey", dsn = ctx.args[1], namespace = "t" })
    end)
    if not ok or res == nil then print("CONN FAIL: " .. tostring(res)); return 0 end
    print("CONN OK"); return 1
end)
LUA
}

# Bounded process control: the probes below MUST NOT hang - a revert of the
# shared/tls_client.c non-blocking-handshake fix would block a rediss:// handshake
# forever, and this watchdog turns that into a hard, fast test failure instead of
# a stall until the outer CI-job timeout. Prefer timeout(1) / gtimeout; if neither
# is present the assertions still run (the fix itself prevents the hang).
WATCH=""
command -v timeout  >/dev/null 2>&1 && WATCH="timeout 20"
command -v gtimeout >/dev/null 2>&1 && WATCH="gtimeout 20"

log "-- rediss:// against the plaintext port must fail closed, BOUNDED (a reverted"
log "   tls_client non-blocking-handshake fix would hang and trip the watchdog) --"
conn_probe_app "rediss" "127.0.0.1"
# Capture status via if/else: under `set -e` a bare out="$(... exit 124 ...)" would
# abort the script before the watchdog diagnostic below (an if-condition suspends -e).
if out="$($WATCH "$HULL" "$TDIR/probe.lua" --no-sandbox -- "rediss://127.0.0.1:$PORT?connect_timeout=2000" 2>&1)"; then prc=0; else prc=$?; fi
[ "$prc" != 124 ] \
    || fail "rediss:// probe HUNG (watchdog fired) - the TLS handshake is not bounding (tls_client fix reverted?)"
printf '%s\n' "$out" | grep -q "CONN FAIL" \
    || { log "$out"; fail "rediss:// to a plaintext server should FAIL closed (no plaintext fallback)"; }
printf '%s\n' "$out" | grep -qiE "tls|handshake" \
    || { log "$out"; fail "rediss:// failure should cite the TLS handshake"; }
log "  ok: rediss fail-closed, bounded ($(printf '%s' "$out" | grep -o 'CONN FAIL:.*'))"

# Bounded-failure SMOKE (honest scope): an unreachable host makes the connect FAIL
# within the watchdog. This does NOT by itself prove the connect DEADLINE fired -
# an immediate routing rejection (no route to host) would also pass - so it is only
# a "connect fails, bounded" smoke. The deterministic proof that the timeout is
# installed + expires is the unit test (test_pg_transport: set_io_timeout installs
# both options / a stalled read expires within the bound).
log "-- connect bounded-failure smoke: unreachable host fails, watchdogged --"
conn_probe_app "redis" "10.255.255.1"
if out="$($WATCH "$HULL" "$TDIR/probe.lua" --no-sandbox -- "redis://10.255.255.1:6379?connect_timeout=800" 2>&1)"; then prc=0; else prc=$?; fi
[ "$prc" != 124 ] \
    || fail "connect probe HUNG (watchdog fired) - connect is not bounding"
printf '%s\n' "$out" | grep -q "CONN FAIL" \
    || { log "$out"; fail "unreachable-host connect should FAIL"; }
log "  ok: connect fails, bounded"
rm -rf "$TDIR"

log "PASS ($PASS runtime(s))"
