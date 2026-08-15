#!/bin/sh
# tests/e2e_stream_meta.sh — the compute stream-metadata SDK helpers
# (hull_stream_is_first/is_last/chunk_index via host_call(HULL_OP_STREAM)),
# restored to the canonical hull_compute.h. Drives the streamprobe plugin over a
# 3-chunk compute.stream and asserts the guest sees first / middle / last +
# chunk index, and that an ordinary (non-stream) compute.call reports 0,0,0.
# Lua + JS, interpreter + AOT (production hull build). (#331)
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$ROOT/build/hull}"
export HULL_QUIET_AOT=1
PASS=0; FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }
skip() { printf "  \033[33mSKIP\033[0m: %s\n" "$1"; }

WAMRC=""
for w in "$ROOT/build/wamrc" "$ROOT/build/wamrc-build/wamrc" "$(command -v wamrc 2>/dev/null || true)"; do
    [ -n "$w" ] && [ -x "$w" ] && { WAMRC="$w"; break; }
done

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cp -r "$ROOT/tests/fixtures/stream_meta" "$TMP/app"

# Assert the fixture's hull_compute.h is byte-identical to the CANONICAL embedded
# source (the HULL_COMPUTE_H literal in compute.lua) — independently extracted,
# not compared against the copy it was made from. This is the same canonical the
# repo-wide check enforces, verified here against the exact header the stream
# helpers are exercised through.
CANON_C="$TMP/canon_hull_compute.h"
awk '
    index($0, "local HULL_COMPUTE_H = [[") == 1 { f = 1; sub(/^local [A-Z_]+ = \[\[/, ""); }
    f { print }
    /^\]\]$/ { if (f) exit }
' "$ROOT/stdlib/cli/lua/hull/compute.lua" | sed '$d' > "$CANON_C"
if [ -s "$CANON_C" ] && cmp -s "$TMP/app/compute/streamprobe/hull_compute.h" "$CANON_C"; then
    pass "streamprobe hull_compute.h is byte-identical to the canonical embedded header"
else
    fail "streamprobe hull_compute.h drifted from the canonical embedded header"
fi

if ( cd "$TMP/app" && "$HULL" compute build streamprobe ) >"$TMP/build.log" 2>&1 && [ -f "$TMP/app/compute/streamprobe.wasm" ]; then
    pass "streamprobe compiles (stream helpers link)"
else
    fail "streamprobe compile"; sed -n '1,20p' "$TMP/build.log"; echo "stream-meta: ${PASS}p ${FAIL}f"; exit 1
fi

# 3 chunks: first (1,0,0), middle (0,0,1), last (0,1,2). Host-driven -> identical
# for Lua and JS.
STREAM="1,0,0;0,0,1;0,1,2"

check() {  # label, workdir, launch argv...
    label="$1"; workdir="$2"; shift 2
    PORT=$((19300 + $$ % 300))
    ( cd "$workdir" && "$@" -p "$PORT" --no-sandbox -l debug ) >"$TMP/srv.log" 2>&1 &
    PID=$!; sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "$label: server start"; cat "$TMP/srv.log"; return; fi
    s=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/stream")
    n=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/nonstream")
    is_aot=0
    grep -qE "cached module 'streamprobe' \(abi=[0-9]+, aot=1" "$TMP/srv.log" && is_aot=1
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    [ "$s" = "$STREAM" ] \
        && pass "$label: stream first/middle/last + chunk-index ($STREAM)" \
        || fail "$label: stream (got: $s)"
    [ "$n" = "0,0,0" ] \
        && pass "$label: non-stream call reports default metadata (0,0,0)" \
        || fail "$label: nonstream (got: $n)"
    case "$label" in
        *aot) [ "$is_aot" = "1" ] && pass "$label: ran against a real AOT module (aot=1)" || fail "$label: AOT not loaded";;
    esac
}

echo "--- interpreter (both runtimes) ---"
check "lua/interp" "$TMP/app" "$HULL" app.lua
check "js/interp"  "$TMP/app" "$HULL" app.js

if [ -n "$WAMRC" ]; then
    echo "--- AOT via hull build (production) ---"
    jsdir="$TMP/app_js"; cp -r "$TMP/app" "$jsdir"; rm -f "$jsdir/app.lua"
    aot_build() {
        rt="$1"; dir="$2"; bin="$TMP/bin_$rt"
        bo="$("$HULL" build "$dir" -o "$bin" --no-verify-platform 2>&1)"
        if printf '%s' "$bo" | grep -q "platform library not embedded"; then
            skip "$rt/aot: hull is not an embedded build (make EMBED_PLATFORM=1) — CI builds embedded"; return 2
        fi
        [ -x "$bin" ] || { fail "$rt/aot: hull build produced no binary"; printf '%s\n' "$bo" | tail -8; return 1; }
        printf '%s' "$bo" | grep -q "AOT compute/streamprobe.wasm" \
            && pass "$rt/aot: hull build AOT-compiled streamprobe" || fail "$rt/aot: no AOT compiled"
        check "$rt/aot" "$dir" "$bin"
    }
    aot_build lua "$TMP/app"
    aot_build js  "$jsdir"
else
    skip "AOT: no wamrc — the CI AOT job builds wamrc so this path is exercised there"
fi

echo ""
echo "stream-meta: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
