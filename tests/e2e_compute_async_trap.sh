#!/bin/sh
# tests/e2e_compute_async_trap.sh — an async compute call whose worker call traps
# (gas exhaustion / WASM trap / internal error) must SURFACE the error to the
# awaiting/yielded handler, not hang it or swallow it. Covers BOTH runtimes:
#
#   Lua (#317): push_result raised via luaL_error, which runs BEFORE lua_resume,
#     so the longjmp escaped the resume and stranded the suspended connection
#     (HTTP 000). The fix defers the raise to a lua_yieldk continuation that runs
#     INSIDE lua_resume.
#   JS (#319): the shared JS resume only ever called `resolve`, so a push_result
#     that threw (compute/db/gpu return JS_ThrowInternalError) fulfilled the
#     promise with undefined and silently swallowed the error. The fix detects a
#     thrown push_result result and routes it to `reject`.
#
# Symmetric expectation, both runtimes:
#   - uncaught -> a clean 500 (not a 000 hang, not a swallowed 200)
#   - pcall / try-catch -> the handler recovers and responds 200
#
# Also asserts the SHARED js/async.c fix generalises beyond compute: a db.async
# error (a throwing consumer, like compute/gpu) now rejects too (cross-consumer).
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
local dbmod   = require("hull.db")
app.manifest({ modules = { "hull/compute@1", "hull/db@1", "hull/http-server@1", "hull/json@1" } })
local inst = compute.instance("echo")
app.get("/ok", function(req, res)
    local r = compute.async.call("echo", req.query.t or "x", {})
    res:json({ r = r or "NIL" })
end)
app.get("/trap", function(req, res)
    local r = compute.async.call("echo", "hi", { gas = 1 })
    res:json({ r = r or "NIL" })  -- unreached
end)
app.get("/caught", function(req, res)
    local ok, err = pcall(function()
        return compute.async.call("echo", "hi", { gas = 1 })
    end)
    res:json({ ok = ok, err = ok and "none" or tostring(err) })
end)
app.get("/pinst", function(req, res)
    local r = inst.async.call("hi", { gas = 1 })
    res:json({ r = r or "NIL" })  -- unreached
end)
app.get("/dberr", function(req, res)
    local ok = pcall(function()
        return dbmod.default().async.query("SELECT * FROM no_such_table")
    end)
    res:json({ ok = ok })
end)
EOF

cat > "$TMP/app.js" << 'EOF'
import { app } from "hull:app";
import { compute } from "hull:compute";
import { db as dbmod } from "hull:db";
app.manifest({ modules: ["hull/compute@1", "hull/db@1", "hull/http-server@1", "hull/json@1"] });
const dec = (b) => { const u = new Uint8Array(b); let s = ""; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };
const inst = compute.instance("echo");
app.get("/ok", async (req, res) => {
    const r = await compute.async.call("echo", req.query.t || "x", {});
    res.json({ r: r ? dec(r) : "NIL" });
});
app.get("/trap", async (req, res) => {
    const r = await compute.async.call("echo", "hi", { gas: 1 });
    res.json({ r: "reached" });  // unreached — must reject, not resolve
});
app.get("/caught", async (req, res) => {
    let ok = true, err = "none";
    try { await compute.async.call("echo", "hi", { gas: 1 }); }
    catch (e) { ok = false; err = String(e); }
    res.json({ ok: ok, err: err });
});
app.get("/pinst", async (req, res) => {
    const r = await inst.async.call("hi", { gas: 1 });
    res.json({ r: "reached" });  // unreached
});
app.get("/dberr", async (req, res) => {
    let ok = true;
    try { await dbmod.default().async.query("SELECT * FROM no_such_table"); }
    catch (e) { ok = false; }
    res.json({ ok: ok });
});
EOF

code() { curl -s -o /dev/null --max-time 8 -w "%{http_code}" "$1" 2>/dev/null || echo 000; }
body() { curl -s --max-time 8 "$1" 2>/dev/null || echo FAIL; }

run_rt() {
    runtime="$1"; ext="$2"
    echo "--- async trap surfacing (${runtime}) ---"
    PORT=$((19820 + $$ % 400))
    "$HULL" -p "$PORT" --no-sandbox -d "$TMP/t_${runtime}.db" "$TMP/app.${ext}" >"$TMP/srv_${runtime}.log" 2>&1 &
    PID=$!
    sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "${runtime} server failed to start"; cat "$TMP/srv_${runtime}.log"; return; fi

    ok=$(body "http://127.0.0.1:$PORT/ok?t=alive")
    trap_code=$(code "http://127.0.0.1:$PORT/trap")
    caught=$(body "http://127.0.0.1:$PORT/caught")
    pinst_code=$(code "http://127.0.0.1:$PORT/pinst")
    dberr=$(body "http://127.0.0.1:$PORT/dberr")
    kill $PID 2>/dev/null; wait $PID 2>/dev/null

    case "$ok" in
        *'"r":"alive"'*) pass "${runtime} pooled async success still works" ;;
        *) fail "${runtime} pooled async success (got: $ok)" ;;
    esac
    # The core regression: the trap must NOT hang (000) and NOT resolve (200). 500.
    if [ "$trap_code" = "500" ]; then
        pass "${runtime} pooled async uncaught trap -> 500 (no hang, not swallowed)"
    else
        fail "${runtime} pooled async uncaught trap (want 500, got: $trap_code)"
    fi
    case "$caught" in
        *'"ok":false'*) pass "${runtime} pooled async trap catchable (pcall/try -> 200, recovered)" ;;
        *) fail "${runtime} pooled async trap catchable (got: $caught)" ;;
    esac
    if [ "$pinst_code" != "000" ]; then
        pass "${runtime} persistent async trap does not hang (code $pinst_code)"
    else
        fail "${runtime} persistent async trap hung (000)"
    fi
    # Cross-consumer proof for the SHARED js/async.c reject fix: db.async (another
    # throwing consumer) error is catchable too. JS-only: the JS fix is shared
    # across all consumers, so one fix covers compute+db+gpu. The Lua side is
    # per-module (the #317 continuation only landed for compute); Lua db.async /
    # gpu.async still hang on error -- tracked as #321, not #319.
    if [ "$runtime" = "js" ]; then
        case "$dberr" in
            *'"ok":false'*) pass "${runtime} db.async error is catchable (cross-consumer)" ;;
            *) fail "${runtime} db.async error catchable (got: $dberr)" ;;
        esac
    fi
}

run_rt "lua" "lua"
run_rt "js"  "js"

echo ""
echo "compute-async-trap: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
