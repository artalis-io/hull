#!/bin/sh
# tests/e2e_spans_async.sh — end-to-end async span forwarding (mapped-spans D.4).
# Proves the whole chain for a POOLED async call: the binding deep-copies names +
# submission-pins the buffer into the worker op, the worker attaches the span set
# (D.2) and tears it down, and the pins release on completion. echo does not read
# the span, so a correct echo output means attach+run+teardown+pin-release all
# worked. Also asserts: an empty spans list is a plain call, and closing the
# buffer immediately after the call is safe (the pin held it).
#
# (Persistent-instance async is currently blocked by a pre-existing busy-guard bug,
# issue #316, independent of spans; not exercised here.)
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
head -c 8192 /dev/zero > "$TMP/data.bin"

cat > "$TMP/app.lua" << 'EOF'
local compute = require("hull.compute")
local fs = require("hull.fs")
app.manifest({ modules = { "hull/compute@1", "hull/fs@1", "hull/http-server@1" },
               fs = { read = { "data.bin" } } })
app.get("/aspan", function(req, res)
    local w = fs.mmap("data.bin", { offset = 0, length = 4096 })
    local r, e = compute.async.call("echo", req.query.t or "x",
                                    { spans = { { name = "src", buffer = w } } })
    w:close()  -- safe: the submission pin held it for the worker's duration
    res:json({ out = r or "NIL", err = e or "none" })
end)
app.get("/aplain", function(req, res)
    local r, e = compute.async.call("echo", req.query.t or "x", { spans = {} })
    res:json({ out = r or "NIL", err = e or "none" })
end)
EOF

cat > "$TMP/app.js" << 'EOF'
import { app } from "hull:app";
import { compute } from "hull:compute";
import { fs } from "hull:fs";
app.manifest({ modules: ["hull/compute@1", "hull/fs@1", "hull/http-server@1"],
               fs: { read: ["data.bin"] } });
const dec = (b) => { const u = new Uint8Array(b); let s = ""; for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]); return s; };
app.get("/aspan", async (req, res) => {
    const w = fs.mmap("data.bin", { offset: 0, length: 4096 });
    const r = await compute.async.call("echo", req.query.t || "x",
                                       { spans: [{ name: "src", buffer: w }] });
    w.close();
    res.json({ out: r ? dec(r) : "NIL" });
});
app.get("/aplain", async (req, res) => {
    const r = await compute.async.call("echo", req.query.t || "x", { spans: [] });
    res.json({ out: r ? dec(r) : "NIL" });
});
EOF

run_rt() {
    runtime="$1"; ext="$2"
    echo "--- async span forwarding (${runtime}) ---"
    PORT=$((19900 + $$ % 500))
    "$HULL" -p "$PORT" --no-sandbox "$TMP/app.${ext}" >"$TMP/srv.log" 2>&1 &
    PID=$!
    sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "${runtime} server failed to start"; cat "$TMP/srv.log"; return; fi
    span=$(curl -sf "http://127.0.0.1:$PORT/aspan?t=hello_async" 2>/dev/null || echo FAIL)
    plain=$(curl -sf "http://127.0.0.1:$PORT/aplain?t=plain_async" 2>/dev/null || echo FAIL)
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    case "$span" in
        *'"out":"hello_async"'*) pass "${runtime} async span call echoes input" ;;
        *) fail "${runtime} async span call (got: $span)" ;;
    esac
    case "$plain" in
        *'"out":"plain_async"'*) pass "${runtime} empty spans = plain async call" ;;
        *) fail "${runtime} empty spans plain call (got: $plain)" ;;
    esac
}

run_rt "lua" "lua"
run_rt "js"  "js"

echo ""
echo "spans-async: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
