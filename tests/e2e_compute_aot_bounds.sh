#!/bin/sh
# tests/e2e_compute_aot_bounds.sh - AOT compute code must bounds-check every
# linear-memory access (audit 31).
#
# Hull's WAMR runtime is built without hardware bound checks: linear memory is
# mapped at its exact size, with no guard region past it. wamrc on a 64-bit
# target drops the software bounds checks unless told otherwise (it relies on
# guard pages), so before the fix `hull build`'s AOT did unchecked native loads
# and stores at mem_base + addr - a guest could read and write host memory.
#
# This drives the REAL production path (`hull build`, with the wamrc flags it
# uses - not a test-only wamrc invocation): it AOT-compiles a module that loads
# and stores at 1 GiB (far past its one-page memory), runs the built binary,
# asserts the module was loaded as AOT (aot=1, so the stamp's bounds-check bits
# passed the runtime check), that both accesses trap, and that the server is
# still serving afterwards.
#
# Requires an embedded hull (platform lib) + wamrc; skips cleanly without them.
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
if [ -z "$WAMRC" ]; then
    skip "wamrc not found (build with: make wamrc)"
    echo ""; echo "compute-aot-bounds: 0 passed, 0 failed (skipped, no wamrc)"; exit 0
fi

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/app/compute"
cp "$ROOT/tests/fixtures/compute/oob.wasm" "$TMP/app/compute/oob.wasm"

cat > "$TMP/app/app.lua" <<'EOF'
local compute = require("hull.compute")
app.manifest({ modules = { "hull/compute@1", "hull/http-server@1", "hull/json@1" } })
local function run(op)
    local out, err = compute.call("oob", op)
    return { out = out, err = err and tostring(err) or nil }
end
app.get("/ok",    function(req, res) res:json(run("k")) end)
app.get("/read",  function(req, res) res:json(run("r")) end)
app.get("/write", function(req, res) res:json(run("w")) end)
EOF

BIN="$TMP/app_bin"
# --no-cache: compile with this build's wamrc flags, never a cached entry.
build_out="$("$HULL" build "$TMP/app" -o "$BIN" --no-verify-platform --no-cache 2>&1)"
if printf '%s' "$build_out" | grep -q "platform library not embedded"; then
    skip "hull is not an embedded build (make EMBED_PLATFORM=1)"
    echo ""; echo "compute-aot-bounds: 0 passed, 0 failed (skipped, not embedded)"; exit 0
fi
if [ ! -x "$BIN" ]; then
    fail "hull build produced no binary"; printf '%s\n' "$build_out" | tail -15
    echo ""; echo "compute-aot-bounds: ${PASS} passed, ${FAIL} failed"; exit 1
fi
if printf '%s' "$build_out" | grep -q "AOT compute/oob.wasm" \
   && ! printf '%s' "$build_out" | grep -q "not embedding AOT\|skipping AOT\|AOT failed"; then
    pass "hull build AOT-compiled and embedded oob"
else
    fail "no embedded AOT for oob"; printf '%s\n' "$build_out" | grep -i aot
fi

PORT=$((20180 + $$ % 300))
( cd "$TMP/app" && "$BIN" -p "$PORT" --hull-no-sandbox -l debug >"$TMP/srv.log" 2>&1 ) &
PID=$!; sleep 2
if ! kill -0 $PID 2>/dev/null; then
    fail "app binary failed to start"; cat "$TMP/srv.log"
else
    ok=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/ok")
    rd=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/read")
    wr=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/write")
    ok2=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/ok")
    alive=0; kill -0 $PID 2>/dev/null && alive=1
    kill $PID 2>/dev/null; wait $PID 2>/dev/null || true

    if grep -qE "cached module 'oob' \(abi=[0-9]+, aot=1" "$TMP/srv.log"; then
        pass "oob loaded as AOT (aot=1: the stamp records bounds checks)"
    else
        fail "oob not loaded as AOT"; grep -iE "cached module|aot|refusing" "$TMP/srv.log" | head
    fi
    case "$ok" in *'"out":"k"'*) pass "in-bounds call works ($ok)";;
        *) fail "in-bounds call failed (got: $ok)";; esac
    case "$rd" in *'"err":'*) case "$rd" in *'"out":'*) fail "OOB load returned data (got: $rd)";;
            *) pass "OOB load trapped ($rd)";; esac ;;
        *) fail "OOB load did not trap (got: $rd)";; esac
    case "$wr" in *'"err":'*) pass "OOB store trapped ($wr)";;
        *) fail "OOB store did not trap (got: $wr)";; esac
    if [ "$alive" -eq 1 ]; then
        case "$ok2" in *'"out":"k"'*) pass "server still serving after the traps";;
            *) fail "call after the traps failed (got: $ok2)";; esac
    else
        fail "server died (an unchecked access faulted the process)"; tail -20 "$TMP/srv.log"
    fi
fi

echo ""
echo "compute-aot-bounds: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
