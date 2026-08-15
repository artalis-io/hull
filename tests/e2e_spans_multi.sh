#!/bin/sh
# tests/e2e_spans_multi.sh — MULTIPLE named mapped spans (3b final slice). Attach
# three spans (alpha, beta, gamma) over data.bin and drive the spanlist plugin
# (public hull_span.h SDK) from BOTH runtimes on the interpreter AND AOT. Asserts:
#   - declaration-order discovery: count=3, order=alpha,beta,gamma;
#   - successful lookup of each name (find=0/1/2);
#   - unknown name -> find=-1;
#   - AOT actually ran (aot=1), not an interpreter fallback.
#
# AOT is the PRODUCTION `hull build` path (--enable-shared-heap). Requires clang
# (hull compute build) + wamrc + an embedded hull; the CI job provides all three
# so every leg runs non-skippably.
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
cp -r "$ROOT/tests/fixtures/spans_multi" "$TMP/app"
# 16 KB data file so the three windows (offsets 0 / 1000 / 8195) fit.
python3 -c "import sys; sys.stdout.buffer.write(bytes(i & 0xff for i in range(16384)))" > "$TMP/app/data.bin"

# Guard: the plugin's committed SDK header is byte-exact to the canonical template.
if cmp -s "$TMP/app/compute/spanlist/hull_span.h" "$ROOT/templates/hull_span.h"; then
    pass "spanlist hull_span.h is byte-exact to templates/hull_span.h"
else
    fail "spanlist hull_span.h drifted from templates/hull_span.h"
fi

# Compile the plugin to interpreter WASM.
if ( cd "$TMP/app" && "$HULL" compute build spanlist ) >"$TMP/build.log" 2>&1 && [ -f "$TMP/app/compute/spanlist.wasm" ]; then
    pass "spanlist compiles to interpreter WASM (hull compute build)"
else
    fail "spanlist compile"; sed -n '1,20p' "$TMP/build.log"; echo "spans-multi: ${PASS}p ${FAIL}f"; exit 1
fi

# Query the running server and assert discovery order + lookups.
#   $1 label   $2 workdir   $3.. launch command (argv)
check() {
    label="$1"; workdir="$2"; shift 2
    PORT=$((19700 + $$ % 300))
    ( cd "$workdir" && "$@" -p "$PORT" --no-sandbox -l debug ) >"$TMP/srv.log" 2>&1 &
    PID=$!; sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "$label: server start"; cat "$TMP/srv.log"; return; fi
    a=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/list?name=alpha")
    b=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/list?name=beta")
    g=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/list?name=gamma")
    u=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/list?name=zzz")
    is_aot=0
    grep -qE "cached module 'spanlist' \(abi=[0-9]+, aot=1" "$TMP/srv.log" && is_aot=1
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    # Declaration-order discovery is identical for every query; find= varies.
    [ "$a" = "count=3;order=alpha,beta,gamma;find=0" ] \
        && pass "$label: 3 spans, declaration order, find(alpha)=0" \
        || fail "$label: alpha (got: $a)"
    [ "$b" = "count=3;order=alpha,beta,gamma;find=1" ] \
        && pass "$label: find(beta)=1" || fail "$label: beta (got: $b)"
    [ "$g" = "count=3;order=alpha,beta,gamma;find=2" ] \
        && pass "$label: find(gamma)=2" || fail "$label: gamma (got: $g)"
    [ "$u" = "count=3;order=alpha,beta,gamma;find=-1" ] \
        && pass "$label: unknown name -> find=-1" || fail "$label: unknown (got: $u)"
    case "$label" in
        *aot) [ "$is_aot" = "1" ] \
            && pass "$label: ran against a real AOT module (aot=1), not the interpreter" \
            || fail "$label: AOT not loaded (interpreter fallback?)";;
    esac
}

echo "--- interpreter (both runtimes) ---"
check "lua/interp" "$TMP/app" "$HULL" app.lua
check "js/interp"  "$TMP/app" "$HULL" app.js

if [ -n "$WAMRC" ]; then
    echo "--- AOT via hull build (production --enable-shared-heap) ---"
    jsdir="$TMP/app_js"; cp -r "$TMP/app" "$jsdir"; rm -f "$jsdir/app.lua"
    aot_build() {  # rt, dir
        rt="$1"; dir="$2"; bin="$TMP/bin_$rt"
        bo="$("$HULL" build "$dir" -o "$bin" --no-verify-platform 2>&1)"
        if printf '%s' "$bo" | grep -q "platform library not embedded"; then
            skip "$rt/aot: hull is not an embedded build (make EMBED_PLATFORM=1) — CI builds embedded"; return 2
        fi
        [ -x "$bin" ] || { fail "$rt/aot: hull build produced no binary"; printf '%s\n' "$bo" | tail -8; return 1; }
        printf '%s' "$bo" | grep -q "AOT compute/spanlist.wasm" \
            && pass "$rt/aot: hull build AOT-compiled spanlist (--enable-shared-heap)" \
            || fail "$rt/aot: no AOT compiled"
        check "$rt/aot" "$dir" "$bin"
    }
    aot_build lua "$TMP/app"
    aot_build js  "$jsdir"
else
    skip "AOT: no wamrc — the CI AOT job builds wamrc so this path is exercised there"
fi

echo ""
echo "spans-multi: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
