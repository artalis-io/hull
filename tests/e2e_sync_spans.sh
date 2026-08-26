#!/bin/sh
# tests/e2e_sync_spans.sh - the SYNCHRONOUS compute.call and instance:call
# bindings (Lua + JS) forward attached mapped spans (#325). Before the fix the
# sync bindings parsed opts.spans then discarded it, so a plugin saw zero spans.
#
# Drives the memcpy-free spancount fixture (raw host_call(SPAN_INFO), no
# hull_span.h - independent of #327): output byte 0 = attached span count,
# byte 1 = window[0]. Over a NON-page-aligned window of a deterministic file
# (byte[i]=i&0xff; offset 8195 => window[0] == 3), it proves:
#   - attachment: a spans call reports count 1 and reads the correct byte (3);
#   - teardown:   a following plain call reports count 0, and re-running the
#                 spans call again reports 1 (attach+teardown per call);
#   - both compute.call (pooled) and instance:call (persistent).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$ROOT/build/hull}"
export HULL_QUIET_AOT=1
PASS=0; FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/compute"
cp "$ROOT/tests/fixtures/compute/spancount.wasm" "$TMP/compute/spancount.wasm"
python3 -c "import sys; sys.stdout.buffer.write(bytes(i & 0xff for i in range(16384)))" > "$TMP/data.bin"

cat > "$TMP/app.lua" <<'EOF'
local compute = require("hull.compute")
local fs = require("hull.fs")
app.manifest({ modules = { "hull/compute@1", "hull/fs@1", "hull/http-server@1", "hull/json@1" },
               fs = { read = { "data.bin" } } })
local inst = compute.instance("spancount")
local function win() return fs.mmap("data.bin", { offset = 8195, length = 4096 }) end
local function report(res, out) res:json({ count = out and string.byte(out, 1) or -1,
                                           byte = (out and #out > 1) and string.byte(out, 2) or -1 }) end
-- pooled sync compute.call WITH spans
app.get("/span", function(req, res)
    local w = win()
    local out = compute.call("spancount", "", { spans = { { name = "s", buffer = w } } })
    w:close(); report(res, out)
end)
-- pooled sync compute.call, no spans (teardown/plain)
app.get("/plain", function(req, res)
    report(res, compute.call("spancount", "", {}))
end)
-- persistent sync instance:call WITH spans
app.get("/inst", function(req, res)
    local w = win()
    local out = inst:call("", { spans = { { name = "s", buffer = w } } })
    w:close(); report(res, out)
end)
-- persistent sync instance:call, no spans
app.get("/instplain", function(req, res)
    report(res, inst:call(""))
end)
EOF

cat > "$TMP/app.js" <<'EOF'
import { app } from "hull:app";
import { compute } from "hull:compute";
import { fs } from "hull:fs";
app.manifest({ modules: ["hull/compute@1", "hull/fs@1", "hull/http-server@1", "hull/json@1"],
               fs: { read: ["data.bin"] } });
const inst = compute.instance("spancount");
const win = () => fs.mmap("data.bin", { offset: 8195, length: 4096 });
const report = (res, out) => { const u = out ? new Uint8Array(out) : null;
    res.json({ count: u && u.length > 0 ? u[0] : -1, byte: u && u.length > 1 ? u[1] : -1 }); };
app.get("/span", (req, res) => { const w = win();
    let out; try { out = compute.call("spancount", "", { spans: [{ name: "s", buffer: w }] }); } finally { w.close(); }
    report(res, out); });
app.get("/plain", (req, res) => report(res, compute.call("spancount", "", {})));
app.get("/inst", (req, res) => { const w = win();
    let out; try { out = inst.call("", { spans: [{ name: "s", buffer: w }] }); } finally { w.close(); }
    report(res, out); });
app.get("/instplain", (req, res) => report(res, inst.call("")));
EOF

get() { curl -s --max-time 6 "$1" 2>/dev/null; }
run_rt() {
    rt="$1"; entry="$2"
    echo "--- sync span forwarding (${rt}) ---"
    PORT=$((19870 + $$ % 300))
    "$HULL" -p "$PORT" --no-sandbox "$TMP/$entry" >"$TMP/srv_$rt.log" 2>&1 &
    PID=$!; sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "$rt server start"; cat "$TMP/srv_$rt.log"; return; fi
    span=$(get "http://127.0.0.1:$PORT/span")
    plain=$(get "http://127.0.0.1:$PORT/plain")
    reuse=$(get "http://127.0.0.1:$PORT/span")
    inst=$(get "http://127.0.0.1:$PORT/inst")
    instp=$(get "http://127.0.0.1:$PORT/instplain")
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    case "$span" in *'"count":1'*) case "$span" in *'"byte":3'*)
        pass "$rt compute.call: sync spans attach (count 1, window[0]=3)";; *)
        fail "$rt compute.call attach byte (got: $span)";; esac ;;
      *) fail "$rt compute.call: sync spans not attached (got: $span)";; esac
    case "$plain" in *'"count":0'*) pass "$rt compute.call: plain call sees 0 (teardown/no leak)";;
      *) fail "$rt compute.call plain (got: $plain)";; esac
    case "$reuse" in *'"count":1'*) pass "$rt compute.call: re-attach after teardown (count 1)";;
      *) fail "$rt compute.call reuse (got: $reuse)";; esac
    case "$inst" in *'"count":1'*) case "$inst" in *'"byte":3'*)
        pass "$rt instance:call: sync spans attach (count 1, window[0]=3)";; *)
        fail "$rt instance:call attach byte (got: $inst)";; esac ;;
      *) fail "$rt instance:call: sync spans not attached (got: $inst)";; esac
    case "$instp" in *'"count":0'*) pass "$rt instance:call: plain call sees 0 (teardown)";;
      *) fail "$rt instance:call plain (got: $instp)";; esac
}
run_rt lua app.lua
run_rt js  app.js
echo ""
echo "sync-spans: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
