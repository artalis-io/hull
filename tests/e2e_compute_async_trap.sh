#!/bin/sh
# tests/e2e_compute_async_trap.sh — a Lua compute.async.call whose worker call
# traps (gas exhaustion / WASM trap / internal error) must SURFACE the error to
# the yielded handler, not hang the request (issue #317). Before the fix, the
# error path raised via luaL_error from push_result — which runs BEFORE
# lua_resume, so the longjmp escaped the resume and stranded the suspended
# connection (observed HTTP 000). The fix defers the raise to a lua_yieldk
# continuation that runs INSIDE lua_resume:
#   - uncaught  -> a clean 500 (not a 000 hang)
#   - pcall'd   -> the handler recovers and responds 200
#
# LUA-ONLY on purpose. The JS sibling (`compute.async.call` and other JS async
# ops) has a SEPARATE bug: the shared JS resume never calls reject, so a worker
# error is silently swallowed (the promise fulfills with undefined). That is
# tracked in #319 and will get its own JS coverage when fixed; asserting today's
# swallow here would bake in wrong behavior.
#
# Persistent-instance async additionally depends on the busy-owner fix (#316,
# landing via #313). On a base without #316 the first persistent async call is
# rejected with `instance_busy`; either way the point here is only that it does
# NOT hang, so the persistent leg asserts a non-000 status, not a specific error.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u
HULL="${HULL:-build/hull}"
export HULL_QUIET_AOT=1
PASS=0; FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/compute"
cp tests/fixtures/compute/echo.wasm "$TMP/compute/echo.wasm"

cat > "$TMP/app.lua" << 'EOF'
local compute = require("hull.compute")
app.manifest({ modules = { "hull/compute@1", "hull/http-server@1", "hull/json@1" } })
local inst = compute.instance("echo")
-- pooled async, success (baseline)
app.get("/ok", function(req, res)
    local r = compute.async.call("echo", req.query.t or "x", {})
    res:json({ r = r or "NIL" })
end)
-- pooled async, uncaught trap -> must be a clean 500, not a 000 hang
app.get("/trap", function(req, res)
    local r = compute.async.call("echo", "hi", { gas = 1 })
    res:json({ r = r or "NIL" })  -- unreached
end)
-- pooled async, trap wrapped in pcall -> handler recovers, 200
app.get("/caught", function(req, res)
    local ok, err = pcall(function()
        return compute.async.call("echo", "hi", { gas = 1 })
    end)
    res:json({ ok = ok, err = ok and "none" or tostring(err) })
end)
-- persistent async trap -> must not hang (status asserted non-000 only; #316)
app.get("/pinst", function(req, res)
    local r = inst.async.call("hi", { gas = 1 })
    res:json({ r = r or "NIL" })  -- unreached
end)
EOF

code() { curl -s -o /dev/null --max-time 8 -w "%{http_code}" "$1" 2>/dev/null || echo 000; }
body() { curl -s --max-time 8 "$1" 2>/dev/null || echo FAIL; }

echo "--- compute.async trap surfacing (lua) ---"
PORT=$((19820 + $$ % 400))
"$HULL" -p "$PORT" --no-sandbox "$TMP/app.lua" >"$TMP/srv.log" 2>&1 &
PID=$!
sleep 2
if ! kill -0 $PID 2>/dev/null; then
    fail "lua server failed to start"; cat "$TMP/srv.log"
else
    ok=$(body "http://127.0.0.1:$PORT/ok?t=alive")
    trap_code=$(code "http://127.0.0.1:$PORT/trap")
    caught=$(body "http://127.0.0.1:$PORT/caught")
    pinst_code=$(code "http://127.0.0.1:$PORT/pinst")
    kill $PID 2>/dev/null; wait $PID 2>/dev/null

    case "$ok" in
        *'"r":"alive"'*) pass "pooled async success still works" ;;
        *) fail "pooled async success (got: $ok)" ;;
    esac
    # The core regression: the trap must NOT hang (000). It surfaces as 500.
    if [ "$trap_code" = "500" ]; then
        pass "pooled async uncaught trap -> 500 (no hang)"
    else
        fail "pooled async uncaught trap (want 500, got: $trap_code)"
    fi
    case "$caught" in
        *'"ok":false'*) pass "pooled async trap catchable (pcall -> 200, recovered)" ;;
        *) fail "pooled async trap catchable (got: $caught)" ;;
    esac
    # Persistent path: only assert it does not hang (000). Error text depends on #316.
    if [ "$pinst_code" != "000" ]; then
        pass "persistent async trap does not hang (code $pinst_code)"
    else
        fail "persistent async trap hung (000)"
    fi
fi

echo ""
echo "compute-async-trap: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
