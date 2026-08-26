#!/bin/sh
# tests/e2e_compute_memops.sh - freestanding libc on the REAL canonical path
# (#327). A compute plugin whose clang-lowered struct copies / runtime-length
# loops emit implicit memcpy/memset/memmove must build via `hull compute build`
# with NO undefined `env.memcpy`-style import, and must run correctly under BOTH
# the interpreter AND AOT. Before the fix the canonical hull_compute.h lacked
# bare memcpy/memset/memmove, so such a plugin trapped on first call with
# "failed to call unlinked import function (env, memcpy)".
#
# Drives the production toolchain end to end:
#   1. `hull compute new` installs the canonical header; we drop in a plugin
#      that exercises all three ops (both overlap directions, identical pointers,
#      zero length, misaligned src/dst) via compiler-generated AND direct calls.
#   2. `hull compute build` compiles it; wasm-objdump asserts the module has NO
#      undefined function imports at all (covers memcpy/memset/memmove + any
#      accidental memcmp/bcmp/host_call).
#   3. run under the interpreter (hull <app.lua>) - assert the byte contract.
#   4. `hull build` (AOT) + run the standalone binary - assert aot=1 and the same
#      contract, so the fix holds under AOT too.
#
# Requires clang (for `hull compute build`) + an embedded hull (for `hull build`)
# + wamrc (for AOT). wasm-objdump (wabt) gates only the import-scan sub-check;
# the interp/AOT legs run without it (a successful load already proves there is
# no unresolved import - WAMR fails instantiation otherwise). The CI job installs
# all of them so every leg runs non-skippably.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$ROOT/build/hull}"
PASS=0; FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }
skip() { printf "  \033[33mSKIP\033[0m: %s\n" "$1"; }
finish() { echo ""; echo "compute-memops: ${PASS} passed, ${FAIL} failed"; [ "$FAIL" -eq 0 ]; exit $?; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
APP="$TMP/app"; mkdir -p "$APP"

# Scaffold via the real `hull compute new` (installs the canonical header), then
# drop in the body that exercises all three ops (kept identical to the committed
# unit fixture so both vehicles test one contract). `hull compute new` refuses a
# pre-existing dir, so let it create compute/memops.
( cd "$APP" && "$HULL" compute new memops >/dev/null 2>&1 )
if [ ! -f "$APP/compute/memops/hull_compute.h" ]; then
    fail "hull compute new did not scaffold the plugin"; finish
fi
if grep -q 'no_builtin("memcpy")' "$APP/compute/memops/hull_compute.h"; then
    pass "canonical hull_compute.h provides bare memcpy/memset/memmove"
else
    fail "scaffolded hull_compute.h lacks the bare libc fix"
fi
cp "$ROOT/tests/fixtures/compute/memops.c" "$APP/compute/memops/memops.c"

# ── 2. Build via the real canonical path ──
build_out="$(cd "$APP" && "$HULL" compute build memops 2>&1)"
if [ ! -f "$APP/compute/memops.wasm" ]; then
    if printf '%s' "$build_out" | grep -qiE "clang|wasm-ld|toolchain|not found|no suitable"; then
        skip "no wasm clang toolchain - cannot build the plugin"; finish
    fi
    fail "hull compute build produced no memops.wasm"; printf '%s\n' "$build_out" | tail -8; finish
fi
pass "hull compute build produced memops.wasm"

# Import scan: assert NO undefined function imports at all. A successful load in
# the interp/AOT legs below is the functional proof; this is the explicit one.
if command -v wasm-objdump >/dev/null 2>&1; then
    funcimp="$(wasm-objdump -j Import -x "$APP/compute/memops.wasm" 2>/dev/null | grep -iE '<[a-z_]+> <- env\.' || true)"
    if [ -z "$funcimp" ]; then
        pass "no undefined function imports (memcpy/memset/memmove/memcmp/host_call all resolved in-module)"
    else
        fail "undefined function import(s) present: $funcimp"
    fi
else
    skip "wasm-objdump (wabt) not found - import scan; interp/AOT load still proves resolution"
fi

# ── App that verifies the plugin's full byte contract ──
cat > "$APP/app.lua" <<'EOF'
local compute = require("hull.compute")
app.manifest({ modules = { "hull/compute@1", "hull/http-server@1", "hull/json@1" } })
-- Input-independent expected bytes [1..39] (memmove both directions + identical
-- + zero length; misaligned memcpy; misaligned memset). See memops.c.
local EXP = {
    0,1,0,1,2,3,4,5,  2,3,4,5,6,7,6,7,  9,8,7,6,  5,6,
    0,23,34,45,56,67,0,0,0,  0x11,0xAB,0xAB,0xAB,0xAB,0xAB,0x11,0x11,
}
local function check(L)
    local t = {}
    for i = 0, L - 1 do t[i + 1] = string.char((L * 7 + i * 13 + 1) % 256) end
    local input = table.concat(t)
    local out = compute.call("memops", input)
    if not out or #out ~= 103 then return false end
    for i = 1, 39 do if string.byte(out, i) ~= EXP[i] then return false end end
    local n = L < 64 and L or 64
    for i = 0, 63 do
        local want = (i < n) and string.byte(input, i + 1) or 0
        if string.byte(out, 39 + i + 1) ~= want then return false end
    end
    return true
end
app.get("/check", function(req, res)
    local sizes = { 0, 1, 7, 8, 63, 64, 100 }
    local ok = 0
    for _, L in ipairs(sizes) do if check(L) then ok = ok + 1 end end
    res:json({ ok = ok, total = #sizes })
end)
EOF

# ── 3. Interpreter run ──
PORT=$((19600 + $$ % 300))
( cd "$APP" && "$HULL" app.lua -p "$PORT" --no-sandbox -l debug >"$TMP/interp.log" 2>&1 ) &
PID=$!; sleep 2
if ! kill -0 $PID 2>/dev/null; then fail "interpreter: server failed to start"; cat "$TMP/interp.log"; else
    resp="$(curl -s --max-time 6 "http://127.0.0.1:$PORT/check")"
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    case "$resp" in
        *'"ok":7'*'"total":7'* | *'"total":7'*'"ok":7'*)
            pass "interpreter: all cases correct (memmove both dirs/identical/zero, misaligned memcpy/memset, compiler-generated)";;
        *) fail "interpreter: contract failed (got: $resp)";;
    esac
fi

# ── 4. AOT run (embedded hull + wamrc) ──
WAMRC=""
for w in "$ROOT/build/wamrc" "$ROOT/build/wamrc-build/wamrc" "$(command -v wamrc 2>/dev/null || true)"; do
    [ -n "$w" ] && [ -x "$w" ] && { WAMRC="$w"; break; }
done
BIN="$TMP/memops_bin"
aot_build="$("$HULL" build "$APP" -o "$BIN" --no-verify-platform 2>&1)"
if printf '%s' "$aot_build" | grep -q "platform library not embedded"; then
    skip "hull is not an embedded build (make EMBED_PLATFORM=1) - AOT leg needs it"
elif [ -z "$WAMRC" ]; then
    skip "wamrc not found - AOT leg needs it"
elif [ ! -x "$BIN" ]; then
    fail "hull build produced no binary"; printf '%s\n' "$aot_build" | tail -8
else
    printf '%s' "$aot_build" | grep -q "AOT compute/memops.wasm" && pass "hull build AOT-compiled memops" || fail "no AOT for memops"
    PORT=$((19900 + $$ % 300))
    ( cd "$APP" && "$BIN" -p "$PORT" --no-sandbox -l debug >"$TMP/aot.log" 2>&1 ) &
    PID=$!; sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "AOT: binary failed to start"; cat "$TMP/aot.log"; else
        # Load is lazy on the first compute.call, so hit the route first.
        resp="$(curl -s --max-time 6 "http://127.0.0.1:$PORT/check")"
        kill $PID 2>/dev/null; wait $PID 2>/dev/null
        if grep -qE "cached module 'memops' \(abi=[0-9]+, aot=1" "$TMP/aot.log"; then
            pass "memops loaded as AOT (aot=1), not interpreter fallback"
        else
            fail "AOT not loaded (interpreter fallback?)"; grep -iE "cached module|aot" "$TMP/aot.log" | head
        fi
        case "$resp" in
            *'"ok":7'*'"total":7'* | *'"total":7'*'"ok":7'*)
                pass "AOT: all cases correct under AOT";;
            *) fail "AOT: contract failed (got: $resp)";;
        esac
    fi
fi

finish
