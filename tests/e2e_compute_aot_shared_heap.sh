#!/bin/sh
# tests/e2e_compute_aot_shared_heap.sh - compute plugins that read a WAMR shared
# heap (a mapped SPAN or a compute.SEGMENT) must read correct nonzero bytes under
# AOT (#326). Before the fix, `hull build`'s compute AOT omitted
# --enable-shared-heap, so the metadata was right but the backing bytes came back
# as zeros. This drives the REAL production path (`hull build`, which now passes
# the flag) - not a test-only wamrc invocation - builds a standalone binary with
# both AOTs embedded, runs it, and asserts the AOT was actually loaded (aot=1) and
# the span + segment reads are correct and nonzero.
#
# Requires an embedded hull (platform lib) + wamrc; the CI job provides both, so
# there the AOT path is exercised, non-skippable.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$ROOT/build/hull}"
PASS=0; FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }
skip() { printf "  \033[33mSKIP\033[0m: %s\n" "$1"; }

# wamrc presence (AOT is the whole point here).
WAMRC=""
for w in "$ROOT/build/wamrc" "$ROOT/build/wamrc-build/wamrc" "$(command -v wamrc 2>/dev/null || true)"; do
    [ -n "$w" ] && [ -x "$w" ] && { WAMRC="$w"; break; }
done

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/app/compute"
cp "$ROOT/tests/fixtures/compute/spancount.wasm" "$TMP/app/compute/spancount.wasm"
cp "$ROOT/tests/fixtures/compute/segread.wasm"   "$TMP/app/compute/segread.wasm"
python3 -c "import sys; sys.stdout.buffer.write(bytes(i & 0xff for i in range(16384)))" > "$TMP/app/data.bin"

cat > "$TMP/app/app.lua" <<'EOF'
local compute = require("hull.compute")
local fs = require("hull.fs")
app.manifest({ modules = { "hull/compute@1", "hull/fs@1", "hull/http-server@1", "hull/json@1" },
               fs = { read = { "data.bin" } } })
-- a read-only shared-heap SEGMENT whose first byte is a nonzero sentinel (0x42).
compute.segment("segread", "data", ("\66"):rep(4096))
local function u8(out, i) return (out and #out >= i) and string.byte(out, i) or -1 end
-- SPAN read (shared heap): window[0] over data.bin at non-page-aligned offset 8195 == 3
app.get("/span", function(req, res)
    local w = fs.mmap("data.bin", { offset = 8195, length = 4096 })
    local out = compute.call("spancount", "", { spans = { { name = "s", buffer = w } } })
    w:close()
    res:json({ count = u8(out, 1), byte = u8(out, 2) })
end)
-- SEGMENT read (shared heap): segment 0's first byte == 0x42 (66)
app.get("/seg", function(req, res)
    local out = compute.call("segread", "")
    res:json({ count = u8(out, 1), byte = u8(out, 2) })
end)
EOF

if [ -z "$WAMRC" ]; then
    skip "wamrc not found - the CI AOT job builds it so this path is exercised there"
    echo ""; echo "compute-aot-shared-heap: 0 passed, 0 failed (skipped, no wamrc)"; exit 0
fi

# Build via the REAL production path. Needs an embedded hull (platform lib).
BIN="$TMP/app_bin"
build_out="$("$HULL" build "$TMP/app" -o "$BIN" --no-verify-platform 2>&1)"
if printf '%s' "$build_out" | grep -q "platform library not embedded"; then
    skip "hull is not an embedded build (make EMBED_PLATFORM=1) - CI builds embedded"
    echo ""; echo "compute-aot-shared-heap: 0 passed, 0 failed (skipped, not embedded)"; exit 0
fi
if [ ! -x "$BIN" ]; then
    fail "hull build produced no binary"; printf '%s\n' "$build_out" | tail -15
    echo ""; echo "compute-aot-shared-heap: ${PASS} passed, $((FAIL)) failed"; exit 1
fi
# The build must have AOT-compiled both modules with shared-heap support.
printf '%s' "$build_out" | grep -q "AOT compute/spancount.wasm" && pass "hull build AOT-compiled spancount" || fail "no AOT for spancount"
printf '%s' "$build_out" | grep -q "AOT compute/segread.wasm"   && pass "hull build AOT-compiled segread"   || fail "no AOT for segread"

PORT=$((19880 + $$ % 300))
# Run from the app dir so fs.mmap finds the runtime data file (data.bin); the
# segment bytes are embedded in app.lua, but the span's window is a runtime file.
( cd "$TMP/app" && "$BIN" -p "$PORT" --no-sandbox -l debug >"$TMP/srv.log" 2>&1 ) &
PID=$!; sleep 2
if ! kill -0 $PID 2>/dev/null; then fail "app binary failed to start"; cat "$TMP/srv.log"; else
    span=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/span")
    seg=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/seg")
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    # Proof the AOT was actually loaded (not an interpreter fallback).
    if grep -qE "cached module 'spancount' \(abi=[0-9]+, aot=1" "$TMP/srv.log" \
       && grep -qE "cached module 'segread' \(abi=[0-9]+, aot=1" "$TMP/srv.log"; then
        pass "both modules loaded as AOT (aot=1)"
    else
        fail "AOT not loaded (interpreter fallback?)"; grep -iE "cached module|aot" "$TMP/srv.log" | head
    fi
    # SPAN backing bytes correct + nonzero under AOT.
    case "$span" in *'"count":1'*) case "$span" in *'"byte":3'*)
        pass "AOT span read: correct nonzero byte (3)";; *)
        fail "AOT span read wrong (got: $span)";; esac ;;
      *) fail "AOT span not attached (got: $span)";; esac
    # SEGMENT backing bytes correct + nonzero under AOT.
    case "$seg" in *'"count":1'*) case "$seg" in *'"byte":66'*)
        pass "AOT segment read: correct nonzero byte (0x42)";; *)
        fail "AOT segment read wrong (got: $seg)";; esac ;;
      *) fail "AOT segment not loaded (got: $seg)";; esac
fi

echo ""
echo "compute-aot-shared-heap: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
