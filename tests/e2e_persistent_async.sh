#!/bin/sh
# tests/e2e_persistent_async.sh — persistent-instance async baseline (issue #316).
# Before the fix, inst.async:call self-rejected: the binding set pi->busy=1 to
# reserve the instance, then the worker called instance_call_buf which rejected on
# busy. The fix routes the worker through the *_async entries that bypass the busy
# REJECT (the submission owns the reservation; done/cancel clear it). This asserts
# the path RUNS, that busy returns to baseline (repeated reuse + a sync call after
# an async both succeed), across Lua and JS. No spans here -- span coverage is in
# tests/e2e_spans_async.sh.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u
HULL="${HULL_BIN:-build/hull}"
PASS=0; FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/compute"
cp examples/compute/compute/echo.wasm "$TMP/compute/echo.wasm"

cat > "$TMP/app.lua" << 'EOF'
local compute = require("hull.compute")
app.manifest({ modules = { "hull/compute@1", "hull/http-server@1" } })
local inst = compute.instance("echo")
-- single async call
app.get("/pa", function(req, res)
    local r, e = inst.async.call(req.query.t or "x")
    res:json({ out = r or "NIL", err = e or "none" })
end)
-- repeated reuse: N sequential async calls on the same instance (busy must clear
-- between them or the 2nd would reject "instance busy")
app.get("/reuse", function(req, res)
    local ok = true
    for i = 1, 5 do
        local r = inst.async.call("n" .. i)
        if r ~= ("n" .. i) then ok = false end
    end
    res:json({ ok = ok })
end)
-- a sync call right after an async proves busy was cleared
app.get("/mixed", function(req, res)
    local a = inst.async.call("A")
    local s = inst:call("B")
    res:json({ a = a or "NIL", s = s or "NIL" })
end)
EOF

cat > "$TMP/app.js" << 'EOF'
import { app } from "hull:app";
import { compute } from "hull:compute";
app.manifest({ modules: ["hull/compute@1", "hull/http-server@1"] });
const inst = compute.instance("echo");
const dec = (b) => { const u = new Uint8Array(b); let s = ""; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };
app.get("/pa", async (req, res) => {
    const r = await inst.async.call(req.query.t || "x");
    res.json({ out: r ? dec(r) : "NIL" });
});
app.get("/reuse", async (req, res) => {
    let ok = true;
    for (let i = 1; i <= 5; i++) {
        const r = await inst.async.call("n" + i);
        if (dec(r) !== "n" + i) ok = false;
    }
    res.json({ ok: ok });
});
app.get("/mixed", async (req, res) => {
    const a = await inst.async.call("A");
    const s = inst.call("B");
    res.json({ a: a ? dec(a) : "NIL", s: s ? dec(s) : "NIL" });
});
EOF

run_rt() {
    runtime="$1"; ext="$2"
    echo "--- persistent async baseline (${runtime}) ---"
    PORT=$((19600 + $$ % 400))
    "$HULL" -p "$PORT" --no-sandbox "$TMP/app.${ext}" >"$TMP/srv.log" 2>&1 &
    PID=$!; sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "${runtime} server failed to start"; cat "$TMP/srv.log"; return; fi
    pa=$(curl -sf "http://127.0.0.1:$PORT/pa?t=hello_pa" 2>/dev/null || echo FAIL)
    reuse=$(curl -sf "http://127.0.0.1:$PORT/reuse" 2>/dev/null || echo FAIL)
    mixed=$(curl -sf "http://127.0.0.1:$PORT/mixed" 2>/dev/null || echo FAIL)
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    case "$pa"    in *'"out":"hello_pa"'*) pass "${runtime} persistent async runs" ;; *) fail "${runtime} persistent async (got: $pa)" ;; esac
    case "$reuse" in *'"ok":true'*)        pass "${runtime} repeated reuse (busy clears)" ;; *) fail "${runtime} repeated reuse (got: $reuse)" ;; esac
    case "$mixed" in *'"a":"A"'*'"s":"B"'*|*'"s":"B"'*'"a":"A"'*) pass "${runtime} sync after async (busy cleared)" ;; *) fail "${runtime} sync after async (got: $mixed)" ;; esac
}

run_rt "lua" "lua"
run_rt "js"  "js"

echo ""
echo "persistent-async: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
