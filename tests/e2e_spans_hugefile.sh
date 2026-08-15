#!/bin/sh
# tests/e2e_spans_hugefile.sh — mapped-spans completion (3b). Two properties the
# other span e2es do not reach, driven Lua + JS on interpreter AND AOT via the
# public hull_span.h SDK (spanmeta plugin):
#
#   1. THREE repeated windows over a SPARSE 5 GiB file, each at a DISTINCT offset
#      > 4 GiB (0x100000003 / 0x110000005 / 0x130000007), each reporting its exact
#      64-bit foffset in declaration order (4294967299 / 4563402757 / 5100273671)
#      -- proving foffset is a true 64-bit field, not truncated to 32 bits; plus
#      first / exact-end / one-past / width-straddling bounded reads on window[0]
#      (first=42, end=59, onepast=-4, straddle=-4).
#   2. hull_span_setup(out, out_cap) returns the TRUE span count for exact, excess,
#      and insufficient out_cap (3 spans: cap 3 -> ret 3 filled 3; cap 5 -> ret 3
#      filled 3; cap 2 -> ret 3 filled 2, dropping the 3rd foffset).
#
# AOT is the production hull build path (--enable-shared-heap). Requires clang +
# wamrc + an embedded hull; the CI job provides all three (non-skippable).
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
cp -r "$ROOT/tests/fixtures/spans_huge" "$TMP/app"

if ! command -v python3 >/dev/null 2>&1; then skip "python3 absent"; echo "spans-hugefile: 0 passed, 0 failed (skipped)"; exit 0; fi

# Sparse 5 GiB file (no real blocks); seed window[0] at 0x100000003 with
# byte0=42, byte255=59. The other two > 4 GiB windows stay sparse (zeros).
python3 - "$TMP/app/huge.bin" <<'PY'
import sys
p = sys.argv[1]
with open(p, "wb") as f:
    f.truncate(5 * 1024 * 1024 * 1024)          # sparse: no real blocks
    f.seek(0x100000003)
    f.write(bytes([42] + [0] * 254 + [59]))
PY

if cmp -s "$TMP/app/compute/spanmeta/hull_span.h" "$ROOT/templates/hull_span.h"; then
    pass "spanmeta hull_span.h is byte-exact to templates/hull_span.h"
else
    fail "spanmeta hull_span.h drifted from templates/hull_span.h"
fi

if ( cd "$TMP/app" && "$HULL" compute build spanmeta ) >"$TMP/build.log" 2>&1 && [ -f "$TMP/app/compute/spanmeta.wasm" ]; then
    pass "spanmeta compiles to interpreter WASM"
else
    fail "spanmeta compile"; sed -n '1,20p' "$TMP/build.log"; echo "spans-hugefile: ${PASS}p ${FAIL}f"; exit 1
fi

# cap=3 (exact) and cap=5 (excess) show all 3 distinct foffsets in declaration
# order; cap=2 (insufficient) still returns the TRUE count 3 but fills only 2.
EXACT3="ret=3;filled=3;names=big0,big1,big2;foffs=4294967299,4563402757,5100273671;first=42;end=59;onepast=-4;straddle=-4"
INSUF2="ret=3;filled=2;names=big0,big1;foffs=4294967299,4563402757;first=42;end=59;onepast=-4;straddle=-4"

check() {  # label, workdir, launch argv...
    label="$1"; workdir="$2"; shift 2
    PORT=$((19500 + $$ % 300))
    ( cd "$workdir" && "$@" -p "$PORT" --no-sandbox -l debug ) >"$TMP/srv.log" 2>&1 &
    PID=$!; sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "$label: server start"; cat "$TMP/srv.log"; return; fi
    c3=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/meta?cap=3")
    c5=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/meta?cap=5")
    c2=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/meta?cap=2")
    # insufficient-capacity lookup: find over the entries setup populated.
    f3=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/meta?cap=3&find=big2")
    f2hit=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/meta?cap=2&find=big1")
    f2miss=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/meta?cap=2&find=big2")
    # hull_span_setup() argument validation probed from the guest (cap=17 sentinel).
    argc=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/meta?cap=17")
    is_aot=0
    grep -qE "cached module 'spanmeta' \(abi=[0-9]+, aot=1" "$TMP/srv.log" && is_aot=1
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    [ "$c3" = "$EXACT3" ] \
        && pass "$label: 3 distinct >4GiB windows, each exact 64-bit foffset (order) + first/end/one-past/straddle reads" \
        || fail "$label: cap3 (got: $c3)"
    [ "$c5" = "$EXACT3" ] \
        && pass "$label: setup out_cap excess (5): true count 3, all 3 foffsets" \
        || fail "$label: cap5 (got: $c5)"
    [ "$c2" = "$INSUF2" ] \
        && pass "$label: setup out_cap insufficient (2): ret=3 (true count), 2 foffsets filled" \
        || fail "$label: cap2 (got: $c2)"
    [ "$f3" = "ret=3;filled=3;find=2" ] \
        && pass "$label: lookup over full cap finds big2 (find=2)" \
        || fail "$label: find cap3 (got: $f3)"
    [ "$f2hit" = "ret=3;filled=2;find=1" ] \
        && pass "$label: insufficient-cap lookup finds a filled name (big1 -> 1)" \
        || fail "$label: find cap2 hit (got: $f2hit)"
    [ "$f2miss" = "ret=3;filled=2;find=-1" ] \
        && pass "$label: insufficient-cap lookup does NOT find an unfilled real span (big2 -> -1)" \
        || fail "$label: find cap2 miss (got: $f2miss)"
    [ "$argc" = "argnull=-5;argneg=-5;argzero=3" ] \
        && pass "$label: hull_span_setup arg validation (NULL+cap=-5, neg cap=-5, NULL+0 count=3)" \
        || fail "$label: argcheck (got: $argc)"
    case "$label" in
        *aot) [ "$is_aot" = "1" ] && pass "$label: ran against a real AOT module (aot=1)" || fail "$label: AOT not loaded";;
    esac
}

echo "--- interpreter (both runtimes) ---"
check "lua/interp" "$TMP/app" "$HULL" app.lua
check "js/interp"  "$TMP/app" "$HULL" app.js

if [ -n "$WAMRC" ]; then
    echo "--- AOT via hull build (production --enable-shared-heap) ---"
    jsdir="$TMP/app_js"; cp -r "$TMP/app" "$jsdir"; rm -f "$jsdir/app.lua"
    aot_build() {
        rt="$1"; dir="$2"; bin="$TMP/bin_$rt"
        bo="$("$HULL" build "$dir" -o "$bin" --no-verify-platform 2>&1)"
        if printf '%s' "$bo" | grep -q "platform library not embedded"; then
            skip "$rt/aot: hull is not an embedded build (make EMBED_PLATFORM=1) — CI builds embedded"; return 2
        fi
        [ -x "$bin" ] || { fail "$rt/aot: hull build produced no binary"; printf '%s\n' "$bo" | tail -8; return 1; }
        printf '%s' "$bo" | grep -q "AOT compute/spanmeta.wasm" \
            && pass "$rt/aot: hull build AOT-compiled spanmeta (--enable-shared-heap)" || fail "$rt/aot: no AOT compiled"
        check "$rt/aot" "$dir" "$bin"
    }
    aot_build lua "$TMP/app"
    aot_build js  "$jsdir"
else
    skip "AOT: no wamrc — the CI AOT job builds wamrc so this path is exercised there"
fi

echo ""
echo "spans-hugefile: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
