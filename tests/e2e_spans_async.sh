#!/bin/sh
# tests/e2e_spans_async.sh - end-to-end async span forwarding (mapped-spans D.4).
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
-- persistent-instance async with a span + immediate buffer close
local pinst = compute.instance("echo")
app.get("/paspan", function(req, res)
    local w = fs.mmap("data.bin", { offset = 0, length = 4096 })
    local r, e = pinst.async.call(req.query.t or "x",
                                  { spans = { { name = "src", buffer = w } } })
    w:close()  -- safe: submission pin held it across the worker run
    res:json({ out = r or "NIL", err = e or "none" })
end)
-- repeated reuse: N persistent async span calls on the same instance, each with a
-- fresh window. All must echo (busy + span chain return to baseline every call).
app.get("/pareuse", function(req, res)
    local ok = true
    for i = 1, 5 do
        local w = fs.mmap("data.bin", { offset = 0, length = 4096 })
        local r = pinst.async.call("v" .. i, { spans = { { name = "s", buffer = w } } })
        w:close()
        if r ~= ("v" .. i) then ok = false end
    end
    res:json({ ok = ok })
end)
-- a gas-limited persistent async span call (may trap); the instance must stay
-- reusable afterward (span torn down, no chain/borrow left on the instance).
app.get("/patrap", function(req, res)
    local w = fs.mmap("data.bin", { offset = 0, length = 4096 })
    pcall(pinst.async.call, "hello", { gas = 1, spans = { { name = "src", buffer = w } } })
    w:close()
    local w2 = fs.mmap("data.bin", { offset = 0, length = 4096 })
    local r2 = pinst.async.call("after", { spans = { { name = "src", buffer = w2 } } })
    w2:close()
    res:json({ after = r2 or "NIL" })
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
const pinst = compute.instance("echo");
app.get("/paspan", async (req, res) => {
    const w = fs.mmap("data.bin", { offset: 0, length: 4096 });
    const r = await pinst.async.call(req.query.t || "x",
                                     { spans: [{ name: "src", buffer: w }] });
    w.close();
    res.json({ out: r ? dec(r) : "NIL" });
});
app.get("/pareuse", async (req, res) => {
    let ok = true;
    for (let i = 1; i <= 5; i++) {
        const w = fs.mmap("data.bin", { offset: 0, length: 4096 });
        const r = await pinst.async.call("v" + i, { spans: [{ name: "s", buffer: w }] });
        w.close();
        if (dec(r) !== "v" + i) ok = false;
    }
    res.json({ ok: ok });
});
app.get("/patrap", async (req, res) => {
    const w = fs.mmap("data.bin", { offset: 0, length: 4096 });
    try {
        await pinst.async.call("hello", { gas: 1, spans: [{ name: "src", buffer: w }] });
    } catch (e) { /* may trap; the point is the instance stays reusable */ }
    w.close();
    const w2 = fs.mmap("data.bin", { offset: 0, length: 4096 });
    const r2 = await pinst.async.call("after", { spans: [{ name: "src", buffer: w2 }] });
    w2.close();
    res.json({ after: r2 ? dec(r2) : "NIL" });
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
    paspan=$(curl -sf "http://127.0.0.1:$PORT/paspan?t=persist_span" 2>/dev/null || echo FAIL)
    pareuse=$(curl -sf "http://127.0.0.1:$PORT/pareuse" 2>/dev/null || echo FAIL)
    # The gas-trap case relies on the async worker's trap error reaching the
    # handler; that is JS-only for now -- a Lua async trap currently hangs the
    # request (pre-existing #317, independent of spans). Span trap-cleanup itself
    # is proven at the C level (test_wasm_spans.d3_gas_cleanup_reusable).
    patrap=""
    [ "$runtime" = "js" ] && patrap=$(curl -sf --max-time 8 "http://127.0.0.1:$PORT/patrap" 2>/dev/null || echo FAIL)
    kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
    case "$span" in
        *'"out":"hello_async"'*) pass "${runtime} pooled async span call echoes input" ;;
        *) fail "${runtime} pooled async span call (got: $span)" ;;
    esac
    case "$plain" in
        *'"out":"plain_async"'*) pass "${runtime} empty spans = plain async call" ;;
        *) fail "${runtime} empty spans plain call (got: $plain)" ;;
    esac
    case "$paspan" in
        *'"out":"persist_span"'*) pass "${runtime} persistent async span + immediate close" ;;
        *) fail "${runtime} persistent async span (got: $paspan)" ;;
    esac
    case "$pareuse" in
        *'"ok":true'*) pass "${runtime} persistent async span repeated reuse (baseline restored)" ;;
        *) fail "${runtime} persistent async span reuse (got: $pareuse)" ;;
    esac
    if [ "$runtime" = "js" ]; then
        case "$patrap" in
            *'"after":"after"'*) pass "${runtime} persistent async span gas-limited then reusable" ;;
            *) fail "${runtime} persistent async span trap/reuse (got: $patrap)" ;;
        esac
    else
        printf "  \033[33mSKIP\033[0m: %s (async trap hang, #317)\n" "${runtime} persistent async span gas-trap"
    fi
}

run_rt "lua" "lua"
run_rt "js"  "js"

# (audit 20) An async call copies its input off the VM heap before it submits
# the op, and a MappedBuffer input is not on the heap at all: the whole mapping
# was copied before anything checked it (the worker then refused it past
# max_input), and every in-flight copy was outside the heap limit. Now an input
# over max_input is refused before the copy, and each copy holds its bytes on
# the VM's off-heap total (the one OWNED WasmBuffers count on) under the heap
# limit until the op is freed. Run under -m 8M:
#   over_max - a 2 MB mapping, default max_input (1 MB): input_too_large
#   held     - a 3 MB OWNED buffer held + a 6 MB input: past the heap limit
#              (it was copied and run)
#   seq      - five 2 MB inputs one after another: each copy's hold is given
#              back when its op ends, so all five run (kept, the fifth would
#              pass 8 MB)
OFF="$TMP/offheap"; mkdir -p "$OFF/compute"
cp examples/compute/compute/echo.wasm "$OFF/compute/echo.wasm"
head -c 2097152 /dev/zero > "$OFF/big2.bin"
head -c 6291456 /dev/zero > "$OFF/big6.bin"

cat > "$OFF/app.lua" << 'EOF'
local compute = require("hull.compute")
local fs = require("hull.fs")
app.manifest({ modules = { "hull/compute@1", "hull/fs@1", "hull/http-server@1" },
               fs = { read = { "big2.bin", "big6.bin" } } })
local MB = 1024 * 1024
local function call(m, opts)
    local ok, r = pcall(compute.async.call, "echo", m, opts)
    return ok and ("len:" .. #r) or tostring(r)
end
app.get("/over_max", function(req, res)
    local m = fs.mmap("big2.bin")
    local out = call(m, {})
    m:close()
    res:json({ out = out })
end)
app.get("/held", function(req, res)
    local b = compute.buffer(string.rep("x", 3 * MB))
    local m = fs.mmap("big6.bin")
    local out = call(m, { max_input = 16 * MB, max_output = 16 * MB })
    m:close(); b:close()
    res:json({ out = out })
end)
app.get("/seq", function(req, res)
    local outs = {}
    for i = 1, 5 do
        local m = fs.mmap("big2.bin")
        outs[i] = call(m, { max_input = 4 * MB, max_output = 4 * MB,
                            heap = 8 * MB })
        m:close()
    end
    res:json({ out = table.concat(outs, ",") })
end)
EOF

cat > "$OFF/app.js" << 'EOF'
import { app } from "hull:app";
import { compute } from "hull:compute";
import { fs } from "hull:fs";
app.manifest({ modules: ["hull/compute@1", "hull/fs@1", "hull/http-server@1"],
               fs: { read: ["big2.bin", "big6.bin"] } });
const MB = 1024 * 1024;
async function call(m, opts) {
    try {
        const r = await compute.async.call("echo", m, opts);
        return "len:" + r.byteLength;
    } catch (e) {
        return String(e && e.message);
    }
}
app.get("/over_max", async (req, res) => {
    const m = fs.mmap("big2.bin");
    const out = await call(m, {});
    m.close();
    res.json({ out });
});
app.get("/held", async (req, res) => {
    const b = compute.buffer(new ArrayBuffer(3 * MB));
    const m = fs.mmap("big6.bin");
    const out = await call(m, { maxInput: 16 * MB, maxOutput: 16 * MB });
    m.close(); b.close();
    res.json({ out });
});
app.get("/seq", async (req, res) => {
    const outs = [];
    for (let i = 0; i < 5; i++) {
        const m = fs.mmap("big2.bin");
        outs.push(await call(m, { maxInput: 4 * MB, maxOutput: 4 * MB,
                                  heap: 8 * MB }));
        m.close();
    }
    res.json({ out: outs.join(",") });
});
EOF

run_offheap() {
    runtime="$1"; ext="$2"
    echo "--- async input copies under the heap limit (${runtime}) ---"
    PORT=$((20400 + $$ % 500))
    "$HULL" -m 8M -p "$PORT" --no-sandbox "$OFF/app.${ext}" >"$OFF/srv.log" 2>&1 &
    PID=$!
    sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "${runtime} offheap server failed to start"; cat "$OFF/srv.log"; return; fi
    over=$(curl -sf --max-time 20 "http://127.0.0.1:$PORT/over_max" 2>/dev/null || echo FAIL)
    held=$(curl -sf --max-time 20 "http://127.0.0.1:$PORT/held" 2>/dev/null || echo FAIL)
    seq=$(curl -sf --max-time 30 "http://127.0.0.1:$PORT/seq" 2>/dev/null || echo FAIL)
    kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
    case "$over" in
        *input_too_large*) pass "${runtime} mapping over max_input refused before it is copied" ;;
        *) fail "${runtime} over max_input (got: $over)" ;;
    esac
    case "$held" in
        *"past its heap limit"*) pass "${runtime} input copy counted with held buffers against the heap limit" ;;
        *) fail "${runtime} held buffers + input copy (got: $held)" ;;
    esac
    case "$seq" in
        *'"out":"len:2097152,len:2097152,len:2097152,len:2097152,len:2097152"'*) pass "${runtime} each input copy's hold is given back when its op ends" ;;
        *) fail "${runtime} sequential input copies (got: $seq)" ; cat "$OFF/srv.log" ;;
    esac
}

run_offheap "lua" "lua"
run_offheap "js"  "js"

echo ""
echo "spans-async: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
