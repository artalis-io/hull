#!/bin/sh
# tests/e2e_compute_memory64.sh — the PRODUCTION-pipeline proof for #336: `hull
# build` of a Memory64 compute plugin. Drives the REAL path (`hull build`, whose
# compute AOT no longer passes the bogus --enable-memory64 flag and keeps the
# transparent --enable-shared-heap), builds a standalone binary with both mem64
# AOTs embedded, runs it, and asserts:
#
#   Case A — plain Memory64 plugin, HEAP-LESS call (echo64): the production-pipeline
#            confirmation of #336 F1/F2/F3 -- a mem64 plugin doing ordinary linear-
#            memory work, called with no spans/segments, runs correctly (does NOT
#            segfault) on this arch. If this fails, the transparent policy is unsafe
#            on a 64-bit target and #336's decision must be revisited.
#   Case B — Memory64 span consumer (spanread64): the attached-shared-heap mem64
#            path through the built binary (the E2E analogue of #334's unit gate).
#
# Both must load as AOT (aot=1) AND Memory64 (mem64=1). Reuses the committed mem64
# fixtures (no new authoring). Needs an embedded hull + wamrc; the CI job provides
# both, so the AOT path is exercised there, non-skippable.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$ROOT/build/hull}"
PASS=0; FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }
skip() { printf "  \033[33mSKIP\033[0m: %s\n" "$1"; }

WAMRC=""
for w in "$ROOT/build/wamrc" "$ROOT/build/wamrc-build/wamrc" "$(command -v wamrc 2>/dev/null || true)"; do
    [ -n "$w" ] && [ -x "$w" ] && { WAMRC="$w"; break; }
done

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/app/compute"
cp "$ROOT/tests/fixtures/compute/echo64.wasm"     "$TMP/app/compute/echo64.wasm"
cp "$ROOT/tests/fixtures/compute/spanread64.wasm" "$TMP/app/compute/spanread64.wasm"
python3 -c "import sys; sys.stdout.buffer.write(bytes(i & 0xff for i in range(16384)))" > "$TMP/app/data.bin"

cat > "$TMP/app/app.lua" <<'EOF'
local compute = require("hull.compute")
local fs = require("hull.fs")
app.manifest({ modules = { "hull/compute@1", "hull/fs@1", "hull/http-server@1", "hull/json@1" },
               fs = { read = { "data.bin" } } })
-- Case A: plain Memory64 plugin, no spans/segments -> heap-less call.
app.get("/echo", function(req, res)
    local out = compute.call("echo64", "hello64")
    res:json({ out = out })
end)
-- Case B: Memory64 span consumer -> attached shared heap. spanread64 needs a
-- >= 96-byte input (it reuses it as the SPAN_INFO scratch) and returns 12 bytes:
-- [1]=count, [2]=window[0] via the 64-bit base.
app.get("/span", function(req, res)
    local w = fs.mmap("data.bin", { offset = 8195, length = 4096 })
    local out = compute.call("spanread64", string.rep("\0", 96),
                             { spans = { { name = "s", buffer = w } } })
    w:close()
    local function u8(o, i) return (o and #o >= i) and string.byte(o, i) or -1 end
    res:json({ count = u8(out, 1), byte = u8(out, 2) })
end)
EOF

if [ -z "$WAMRC" ]; then
    skip "wamrc not found — the CI AOT job builds it so this path is exercised there"
    echo ""; echo "compute-memory64: 0 passed, 0 failed (skipped, no wamrc)"; exit 0
fi

BIN="$TMP/app_bin"
build_out="$("$HULL" build "$TMP/app" -o "$BIN" --no-verify-platform 2>&1)"
if printf '%s' "$build_out" | grep -q "platform library not embedded"; then
    skip "hull is not an embedded build (make EMBED_PLATFORM=1) — CI builds embedded"
    echo ""; echo "compute-memory64: 0 passed, 0 failed (skipped, not embedded)"; exit 0
fi
if [ ! -x "$BIN" ]; then
    fail "hull build produced no binary"; printf '%s\n' "$build_out" | tail -20
    echo ""; echo "compute-memory64: ${PASS} passed, $((FAIL)) failed"; exit 1
fi
# The build must have AOT-compiled both mem64 modules (D1: no bogus flag -> the
# compile succeeds; the "(memory64)" tag proves detection in build.lua).
printf '%s' "$build_out" | grep -qE "AOT compute/echo64.wasm.*memory64" \
    && pass "hull build AOT-compiled echo64 (memory64)" || fail "no mem64 AOT for echo64 (bogus flag?)"
printf '%s' "$build_out" | grep -qE "AOT compute/spanread64.wasm.*memory64" \
    && pass "hull build AOT-compiled spanread64 (memory64)" || fail "no mem64 AOT for spanread64"

PORT=$((19910 + $$ % 300))
( cd "$TMP/app" && "$BIN" -p "$PORT" --no-sandbox -l debug >"$TMP/srv.log" 2>&1 ) &
PID=$!; sleep 2
if ! kill -0 $PID 2>/dev/null; then
    fail "app binary failed to start"; cat "$TMP/srv.log"
else
    echo_r=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/echo")
    span=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/span")
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    # Both modules loaded as AOT + Memory64 (not an interpreter fallback).
    grep -qE "cached module 'echo64' \(abi=[0-9]+, aot=1, mem64=1" "$TMP/srv.log" \
        && pass "echo64 loaded as AOT + Memory64 (aot=1, mem64=1)" \
        || { fail "echo64 not AOT+mem64"; grep -iE "cached module 'echo64'" "$TMP/srv.log" | head; }
    grep -qE "cached module 'spanread64' \(abi=[0-9]+, aot=1, mem64=1" "$TMP/srv.log" \
        && pass "spanread64 loaded as AOT + Memory64 (aot=1, mem64=1)" \
        || { fail "spanread64 not AOT+mem64"; grep -iE "cached module 'spanread64'" "$TMP/srv.log" | head; }
    # Case A: heap-less mem64 plugin runs correctly (no segfault) via hull build.
    case "$echo_r" in *'"out":"hello64"'*) pass "Case A: heap-less Memory64 echo64 call correct";;
      *) fail "Case A: heap-less Memory64 call wrong (got: $echo_r)";; esac
    # Case B: span consumer reads window[0]==data.bin[8195]==3 through the 64-bit base.
    case "$span" in *'"count":1'*) case "$span" in *'"byte":3'*)
        pass "Case B: Memory64 span read correct (count 1, byte 3)";; *)
        fail "Case B: Memory64 span read wrong (got: $span)";; esac ;;
      *) fail "Case B: Memory64 span not attached (got: $span)";; esac
fi

echo ""
echo "compute-memory64: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
